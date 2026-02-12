// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

// [B: 스냅샷/정리] LOG 트랜잭션 하나에서 free 할 수 있는 최대 블록 수 제한
// cleanup_orphan_blocks()에서 여러 번 begin_op/end_op로 나누기 위해 사용
// LOGSIZE는 param.c에 정의되어 있음
#define MAX_FREE_PER_TX (LOGSIZE / 2)

#define BLOCKMETA_CHUNK  16   // 한 트랜잭션에서 쓸 최대 레코드 수


// [편의 매크로] 기존 xv6에는 없던 간단한 min 매크로
#define min(a, b) ((a) < (b) ? (a) : (b))

void itrunc(struct inode*);
// there should be one superblock per disk device, but we run with
// only one device

struct inode* iget(uint dev, uint inum);

void rebuild_block_meta(int dev); // 부팅 시 block_refcnt/block_snapcnt 재구축
static int need_cow(uint b);      // 특정 블록이 스냅샷과 공유 중인지 확인
static void set_block(struct inode *ip, uint bn, uint newb); // inode의 bn번째 논리 블록이 어떤 디스크 블록을 가리킬지 갱신
static void read_dinode_nolock(int dev, uint inum, struct dinode *out);  // 락 없이 디스크에서 dinode 한 개 읽기(rebuild_block_meta용)

// 참조는 없는데 비트맵 상 할당된 블록을 제거하는 함수
void cleanup_orphan_blocks(int dev);


// /snapshot/blockmeta 파일의 inode 캐시
// (처음 필요할 때 namei("/snapshot/blockmeta")로 한 번만 찾아서 보관)
static struct inode *blockmeta_ip = 0;

// superblock
struct superblock sb; 

// 블록 메타데이터(refcount + snapshot count), FFSIZE는 이미 param.h에 정의돼 있음
int block_refcnt[FSSIZE];   // 각 블록을 몇 개의 inode가 사용하는지
int block_snapcnt[FSSIZE];  // 각 블록을 "스냅샷 파일"이 몇 개나 참조하는지
struct spinlock block_meta_lock;


// 메타데이터 파일 관련 헬퍼 함수들
static void blockmeta_init(int dev);
//satatic void blockmeta_full_sync(void);
static void blockmeta_write_record(uint b, int ref, int snap);


// Read the super bloc16.
void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
// 기존 balloc에 block_refcnt/block_snapcnt 초기화 추가
// "새로 할당된 블록은 refcnt=1, snapcnt=0"으로 시작
// 이후 COW에서 old 블록 bdecref, 새 블록 balloc(1) 패턴으로 사용
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  // Mark block in use.
        log_write(bp);
        brelse(bp);

		uint bno = b + bi;
        bzero(dev, bno);  // 새 블록 0으로 채우기

		int ref, snap;

		acquire(&block_meta_lock);
		// free 블록이면 refcnt/snapcnt가 0이어야 함을 검증
		if(block_refcnt[bno] != 0 || block_snapcnt[bno] != 0){
          release(&block_meta_lock);
          panic("balloc: stale meta for free block");
        }
		// 새 블록은 현재 파일이 단독 소유한다고 보고 refcnt=1로 시작
		block_refcnt[bno] = 1;
		block_snapcnt[bno] = 0;
		ref = block_refcnt[bno];
		snap = block_snapcnt[bno];
		release(&block_meta_lock);

		// on-disk metadata in-place update
        blockmeta_write_record(bno, ref, snap);

		return bno;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
// bfree를 "참조계수 기반 해제"로 바꿈
// "비트맵만" 조작하는 원래 스타일의 bfree를 bfree_raw로 분리
// 실제 해제는 refcnt/snapcnt가 0일 때 bdecref에서 호출
static void
bfree_raw(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// block_refcnt[b]-- 후 ref==0 && snap==0인 경우에만 실제로 bfree_raw()
// 스냅샷이 공유 중인 블록은 snapcnt>0이므로 실제 free되지 않음.
static void
bdecref(int dev, uint b)
{
  int ref, snap;

  acquire(&block_meta_lock);
  if(block_refcnt[b] <= 0)
    panic("bdecref: ref underflow");
  block_refcnt[b]--;
  ref  = block_refcnt[b];
  snap = block_snapcnt[b];
  release(&block_meta_lock);

  // on-disk metadata in-place update
  // 메타데이터 업데이트
  blockmeta_write_record(b, ref, snap);

  if(ref == 0 && snap == 0){
    bfree_raw(dev, b);
  }
}


// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

void
iinit(int dev)
{
  int i = 0;
  
  initlock(&icache.lock, "icache");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d ninodes %d nlog %d logstart %d\
 inodestart %d bmap start %d\n", sb.size, sb.nblocks,
          sb.ninodes, sb.nlog, sb.logstart, sb.inodestart,
          sb.bmapstart);

  // 블록 메타데이터 락 초기화 + refcnt/snapcnt 재구축
  // 부팅 시 디스크상의 inode들을 모두 스캔하여
  // block_refcnt / block_snapcnt를 다시 계산한다
  initlock(&block_meta_lock, "block_meta");
  rebuild_block_meta(dev);

  blockmeta_init(dev);          // /snapshot/blockmeta inode 잡고 full sync
}


// 모든 inode를 스캔해서(direct/indirect 블록)
//  - block_refcnt[b] : 해당 블록을 참조하는 inode 개수
//  - block_snapcnt[b]: 해당 블록을 참조하는 T_SNAP inode 개수
// 를 재구성하는 함수
void
rebuild_block_meta(int dev)
{
  int i, j;
  uint inum;
  struct dinode di;
  struct buf *bp;
  uint *a;

  acquire(&block_meta_lock);
  for(i = 0; i < FSSIZE; i++){
    block_refcnt[i]  = 0;
    block_snapcnt[i] = 0;
  }
  release(&block_meta_lock);

  for(inum = 1; inum < sb.ninodes; inum++){
    read_dinode_nolock(dev, inum, &di);
    if(di.type == 0)
      continue;

    // 1) refcnt: 모든 (파일/디렉토리/스냅샷) inode에 대해
    // 2) snapcnt: T_SNAP일 때만 추가로 ++
    int is_snap = (di.type == T_SNAP);

    // direct
    for(i = 0; i < NDIRECT; i++){
      uint b = di.addrs[i];
      if(b){
        acquire(&block_meta_lock);
        block_refcnt[b]++;           // 항상 증가
        if(is_snap) block_snapcnt[b]++; // 스냅샷 파일이면 snapcnt도 증가
        release(&block_meta_lock);
      }
    }

    // indirect
    if(di.addrs[NDIRECT]){
      uint ib = di.addrs[NDIRECT];

      acquire(&block_meta_lock);
      block_refcnt[ib]++;

	  if(is_snap)
			  block_snapcnt[ib]++;
      release(&block_meta_lock);

      bp = bread(dev, ib);
      a = (uint*)bp->data;
      for(j = 0; j < NINDIRECT; j++){
        uint b = a[j];
        if(b){
          acquire(&block_meta_lock);
          block_refcnt[b]++;
          if(is_snap) block_snapcnt[b]++;
          release(&block_meta_lock);
        }
      }
      brelse(bp);
    }
  }
}


//PAGEBREAK!
// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, sb));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk, since i-node cache is write-through.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  acquiresleep(&ip->lock);
  if(ip->valid && ip->nlink == 0){
    acquire(&icache.lock);
    int r = ip->ref;
    release(&icache.lock);
    if(r == 1){
      // inode has no links and no other references: truncate and free.
      itrunc(ip);
      ip->type = 0;
      iupdate(ip);
      ip->valid = 0;
    }
  }
  releasesleep(&ip->lock);

  acquire(&icache.lock);
  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

//PAGEBREAK!
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Only called when the inode has no links
// to it (no directory entries referring to it)
// and has no in-memory reference to it (is
// not an open file or current directory).
void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
	  // 원래 bfree() 호출 -> bdecref()로 교체
	  // 스냅샷에서 공유 중인 블록은 snapcnt>0이라 실제 free되지 않음
      bdecref(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
		// 간접 블록 엔트리도 참조계수 기반 free
        bdecref(ip->dev, a[j]);
    }
    brelse(bp);
	// 간접 블록 자체도 bdecref로 ref/snap 기반 free
    bdecref(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

//PAGEBREAK!
// Read data from inode.
// Caller must hold ip->lock.
int
readi(struct inode *ip, char *dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(ip, dst, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(dst, bp->data + off%BSIZE, m);
    brelse(bp);
  }
  return n;
}

// PAGEBREAK!
// Write data to inode.
// Caller must hold ip->lock.
// 파일에 데이터를 쓰는 함수(스냅샷 로직 추가)
// 요약: 버퍼(src)의 n 바이트를, 파일 ip의 현재 오프셋(off)로부터 차례대로 쓰되, 
// 각 파일 블록에 대해 COW가 필요하면 새 블록을 하나 할당해서 기존 내용을 복사한 뒤 그 새 블록에 써 넣는 로직이다
// 각 파일 블록을 쓰기 전에 need_cow()로 스냅샷 공유 여부를 검사.
// 공유 중이라면 새 블록을 할당해서 old 내용을 복사한 뒤 inode 매핑을 새 블록으로 교체
int
writei(struct inode *ip, char *src, uint off, uint n)
{
  uint tot, m;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
      return -1;
    return devsw[ip->major].write(ip, src, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  // n: 전체로 쓰고 싶은 바이트 수, tot: 지금까지 쓴 전체 바이트 수(누적)
  // off: 파일 내 현재 쓰기 위치(파일 오프셋), src: 사용자 버퍼의 현재 위치
  // 블록 단위로 나누어 쓰는 루프
  for(tot=0; tot<n; tot+=m, off+=m, src+=m){

  // 1. 이 오프셋(off)이 속한 파일 블록 찾기
  uint bn  = off / BSIZE;   // 현재 오프셋 off가 파일의 몇 번째 논리 블록에 속하는지 계산(bn = 데이터를 쓸 위치가 속한 파일 블록 번호)
  uint bno = bmap(ip, bn);  // (해당 논리 블록이 없으면 새로 할당됨: refcnt=1, bno=실제 디스크 블록 번호)

  // COW 필요 여부 체크
  // need_cow()를 이용해서 디스크 블록 bno가 스냅샷과 공유 중인지 확인한다.
  // need_cow()가 true라면 이 블록을 스냅샷도 쓰고 있으므로 COW를 진행한다.
  if(need_cow(bno)){
    uint newb = balloc(ip->dev);            // 새 블록 할당(refcnt(newb)=1)
    struct buf *obp = bread(ip->dev, bno);  // old block 읽기
    struct buf *nbp = bread(ip->dev, newb); // new block 버퍼
    memmove(nbp->data, obp->data, BSIZE);   // old->new 전체 복사(기존 블록 bno의 내용을 통째로 newb 블록으로 복사)
    log_write(nbp);							// 로그 시스템에 변경된 newb 버퍼를 기록(저널링)
    brelse(obp);							// 버퍼 사용 끝났으니 해제
    brelse(nbp);
	
//	cprintf("[DEBUG] [COW] inum = %d bn = %d old = %x -> new = %x\n", ip->inum, bn, bno, newb);

    // inode 매핑 교체
	// inode ip에서 논리 블록 bn이 가리키는 디스크 블록을 newb로 바꾸는 작업을 진행한다
    set_block(ip, bn, newb);

    // old는 참조 감소 (스냅샷 pin이 0일 때만 실제 해제됨)
	// 즉, 기존 블록은 참조 감소한다. 
    bdecref(ip->dev, bno);

	// 이후 로직에서는 새 블록(newb) 사용
    bno = newb;
  }

  // 실제 데이터 쓰기
  struct buf *bp = bread(ip->dev, bno);   	// 지금 쓰려는 디스크 블록 bno를 버퍼로 읽어온다
  m = min(n - tot, BSIZE - off%BSIZE);		// 이번에 블록에 쓸 바이트 수 계산. min(아직 안 쓴 남은 전체 바이트 수, 현재 블록에서 남아 있는 공간)
  memmove(bp->data + off%BSIZE, src, m); 	// 메모리->버퍼로 데이터 복사
  log_write(bp);							// 이 버퍼의 변경 내용을 로그에 기록(저널링)
  brelse(bp);								// 버퍼 사용 끝
  }

  // 파일 크기 갱신
  // 이때 이번 write 때문에 파일이 더 커졌으면 inode의 파일 크기도 같이 키운다.
  if(n > 0 && off > ip->size){ 				
    ip->size = off;
    iupdate(ip);
  }
  return n;
}

//PAGEBREAK!
// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");

  return 0;
}

//PAGEBREAK!
// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}

// 디스크 블록 b에 대해 COW가 필요한 상황인지 체크하는 함수
// 블록 b를 덮어쓰기 전에, 이 블록이 스냅샷과 공유 중이면 COW를 해야 하고, 아니면 그냥 덮어쓴다
// snap>0 -> 1: COW가 필요하다
// snap<=0 -> 0: COW가 필요 없다(그냥 덮어써도 됨)
static int
need_cow(uint b)
{
  int snap;
  acquire(&block_meta_lock);

  // b번 블록을 현재 몇 개의 스냅샷이 참조 중인지 읽어온다
  // 예: snap == 2 -> 이 블록을 참조하는 스냅샷이 2개 있음
  snap = block_snapcnt[b];
  release(&block_meta_lock);
  return snap > 0;
}

// 파일의 bn번째 데이터 블록이 실제 디스크의 어느 블록(newb)에 저장되는지 설정해주는 함수
// 즉, inode 안에 있는 블록 번호 테이블(직접/간접 블록)을 수정하는 역할을 한다.
// ip: 어느 파일(inode)에 대해 작업할지, bn: 그 파일 안에서의 블록 번호(0,1,2,... 같은 논리적 블록 인덱스)
// newb: 이 논리 블록이 실제 디스크에서 가리킬 디스크 블록 번호
// 디스크에서 inum에 해당하는 dinode를 직접 읽어온다 (락 없음)
static void
set_block(struct inode *ip, uint bn, uint newb)
{
  // 1) 직접 블록 영역
  if(bn < NDIRECT){
    ip->addrs[bn] = newb;
    iupdate(ip);
    return;
  }

  // 2) 간접 블록 영역으로 인덱스 조정
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    uint ib = ip->addrs[NDIRECT];

    if(ib == 0)
      panic("set_block: no indirect");

    // * 간접 블록 자체에 대해 COW 필요 여부 검사
    if(need_cow(ib)){
			
      // (1) 새 간접 블록 할당
      uint newib = balloc(ip->dev);

      // 디버그용 출력(제출 전에 지우기)
     // cprintf("[DEBUG] [COW-INDIRECT] inum=%d old_ib=%x -> new_ib=%x\n", ip->inum, ib, newib);


      // (2) 기존 간접 블록 내용을 새 블록으로 복사
      struct buf *obp = bread(ip->dev, ib);
      struct buf *nbp = bread(ip->dev, newib);
      memmove(nbp->data, obp->data, BSIZE);
      log_write(nbp);
      brelse(obp);
      brelse(nbp);

      // (3) inode가 가리키는 간접 블록을 새 블록으로 교체
      ip->addrs[NDIRECT] = newib;
      iupdate(ip);          // 디스크의 inode도 갱신

      // (4) 이전 간접 블록에 대한 참조 감소
      bdecref(ip->dev, ib);

      // 이후 로직에서는 새 간접 블록을 사용
      ib = newib;
    }

    // 여기서부터는 "이 inode만 사용하는 간접 블록 ib"를 수정하는 부분
    struct buf *bp = bread(ip->dev, ib);
    uint *a = (uint*)bp->data;

    a[bn] = newb;           // bn번째 엔트리를 newb로 교체

    log_write(bp);
    brelse(bp);
    return;
  }

  panic("set_block: out of range");
}


// 락 없이 디스크의 dinode 한 개 읽기
// rebuild_block_meta에서 inode 전체를 순회할 때 사용
static void
read_dinode_nolock(int dev, uint inum, struct dinode *out)
{
  struct buf *bp = bread(dev, IBLOCK(inum, sb));
  struct dinode *dip = (struct dinode*)bp->data + inum % IPB;
  memmove(out, dip, sizeof(*out));
  brelse(bp);
}

 // Free a disk block.
 // orphan 정리 전용. 기존 xv6의 bfree를 이름만 달리 복사
 // cleanup_orphan_blocks()에서 refcnt/snapcnt==0인 블록을 직접 free할때만 사용한다
static void
bfree(int dev, uint b)
{
   struct buf *bp;
   int bi, m;

   bp = bread(dev, BBLOCK(b, sb));
   bi = b % BPB;
   m = 1 << (bi % 8);
   if((bp->data[bi/8] & m) == 0)
     panic("freeing free block");
   bp->data[bi/8] &= ~m;
   log_write(bp);
   brelse(bp);
}

// 디스크 비트맵과 block_refcnt / block_snapcnt를 비교해서
// "아무도 참조하지 않는데(refcnt == 0 && snapcnt == 0)
//  비트맵에는 할당된 것으로 표시된 데이터 블록"을 찾아 실제로 bfree()로 해제하는 함수.
//  => 내부에서 begin_op ~ end_op 를 직접 관리한다.
//     (호출하는 쪽에서는 트랜잭션을 열지 말 것!)
void
cleanup_orphan_blocks(int dev)
{
  int b;
  int freed_in_tx = 0;

  //  메타데이터 블록 개수 = 전체 - 데이터블록
  //    데이터 블록은 [first_data, sb.size) 구간에만 존재
  int first_data = sb.size - sb.nblocks;

  begin_op();   // 첫 트랜잭션 시작

  //  메타데이터(0 .. first_data-1)는 건너뛰고
  //    오직 "데이터 블록" 구간만 검사한다.
  for (b = first_data; b < sb.size; b++) {
    int bi = b % BPB;
    int m  = 1 << (bi % 8);

    // 비트맵에서 이 블록이 할당 상태인지 확인
    struct buf *bp = bread(dev, BBLOCK(b, sb));
    int allocated = bp->data[bi/8] & m;
    brelse(bp);

    if (!allocated)
      continue;

    int ref, snap;
    acquire(&block_meta_lock);
    ref  = block_refcnt[b];
    snap = block_snapcnt[b];
    release(&block_meta_lock);

    // 어떤 파일/스냅샷에서도 참조하지 않는 "데이터 블록"만 free
    if (ref == 0 && snap == 0) {
      bfree(dev, b);   //  항상 begin_op~end_op 안

      freed_in_tx++;
      // 한 트랜잭션에서 너무 많은 free를 하지 않도록 쪼개기
      if (freed_in_tx >= MAX_FREE_PER_TX) {
        end_op();          // 지금까지 free 한 것 commit
        begin_op();        // 새 트랜잭션 시작
        freed_in_tx = 0;
      }
    }
  }

  end_op();    // 마지막 트랜잭션 종료
}

static void
blockmeta_init(int dev)
{
  struct inode *snapdir;
  struct inode *ip;
 // char name[DIRSIZ];

  // /snapshot/blockmeta 를 찾아온다.
  // 이미 mkfs에서 만들어졌다고 가정.
  begin_op();
  snapdir = namei("/snapshot");
  if(snapdir == 0){
    end_op();
    panic("blockmeta_init: no /snapshot");
  }

  // /snapshot 디렉토리 안에서 blockmeta 찾기
  ilock(snapdir);
  ip = dirlookup(snapdir, "blockmeta", 0);
  iunlockput(snapdir);
  end_op();

  if(ip == 0)
    panic("blockmeta_init: no /snapshot/blockmeta");

  // 전역 포인터에 저장
  blockmeta_ip = ip;

  // 첫 부팅 또는 crash 후 재부팅 시점에
  // 메모리에서 다시 계산된 ref/snap을 blockmeta 파일로 전체 덮어쓰기
 // blockmeta_full_sync();
}

/*
static void
blockmeta_full_sync(void)
{
  if(blockmeta_ip == 0)
    panic("blockmeta_full_sync: no blockmeta_ip");

  struct block_meta m;
  uint off = 0;
  uint b;
  int cnt_in_tx = 0;

  begin_op();
  ilock(blockmeta_ip);

  for (b = 0; b < sb.size && b < FSSIZE; b++) {

    acquire(&block_meta_lock);
    m.ref  = (uchar)block_refcnt[b];
    m.snap = (uchar)block_snapcnt[b];
    release(&block_meta_lock);

    if (writei(blockmeta_ip, (char*)&m,
               off, sizeof(m)) != sizeof(m))
      panic("blockmeta_full_sync: writei");
    off += sizeof(m);
    cnt_in_tx++;

    // 로그에 너무 많은 블록이 쌓이지 않도록 중간중간 커밋
    if (cnt_in_tx >= BLOCKMETA_CHUNK) {
      iunlock(blockmeta_ip);
      end_op();          // 지금까지 기록한 것 커밋

      begin_op();        // 새 트랜잭션 시작
      ilock(blockmeta_ip);
      cnt_in_tx = 0;
    }
  }

  iunlock(blockmeta_ip);
  end_op();
}
*/

static void
blockmeta_write_record(uint b, int ref, int snap)
{
  if(blockmeta_ip == 0)
    return;   // 아직 초기화 안 되었으면 조용히 무시 (부팅 초기 단계 대비)

  if(b >= FSSIZE)
    return;

  struct block_meta m;
  m.ref  = (uchar)ref;
  m.snap = (uchar)snap;

  // 이미 sys_*에서 begin_op()가 열려 있고,
  // 그 안에서 balloc/bdecref/writei 등이 돌고 있다는 전제하에,
  // 여기서는 별도 begin_op() 없이 writei만 호출한다.
  ilock(blockmeta_ip);
  if(writei(blockmeta_ip, (char*)&m,
            b * sizeof(struct block_meta),
            sizeof(struct block_meta)) != sizeof(struct block_meta))
    panic("blockmeta_write_record: writei");
  iunlock(blockmeta_ip);
}

// ------------------------------------------------------
//  /snapshot/blockmeta in-place 갱신 헬퍼들
//  - blockmeta_add_ref()      : refcnt++, (is_snap!=0이면 snapcnt++)
//  - blockmeta_add_ref_nosnap : refcnt++
//  - blockmeta_dec_snap()     : snapcnt--
// ------------------------------------------------------

// refcnt[b]++, 그리고 is_snap != 0 이면 snapcnt[b]++ 까지 수행한 뒤,
// /snapshot/blockmeta 의 b 번째 레코드를 in-place 갱신한다.
//
void
blockmeta_add_ref(uint b, int is_snap)
{
  int ref, snap;

  acquire(&block_meta_lock);
  if(b >= FSSIZE)
    panic("blockmeta_add_ref: b out of range");

  block_refcnt[b]++;
  ref = block_refcnt[b];

  if(is_snap){
    block_snapcnt[b]++;
  }
  snap = block_snapcnt[b];
  release(&block_meta_lock);

  // 기존 3인자 버전 helper 호출
  blockmeta_write_record(b, ref, snap);
}

// 일반 파일용: refcnt[b]++ 만 수행하고 디스크 갱신
void
blockmeta_add_ref_nosnap(uint b)
{
  blockmeta_add_ref(b, 0);
}

// 스냅샷이 없어질 때(스냅샷 파일 삭제 시),
// T_SNAP inode가 참조하던 블록에 대해 snapcnt[b]-- 만 수행.
// refcnt 감소는 itrunc -> bdecref 경로에서 처리.
//
void
blockmeta_dec_snap(uint b)
{
  int ref, snap;

  acquire(&block_meta_lock);
  if(b >= FSSIZE)
    panic("blockmeta_dec_snap: b out of range");
  if(block_snapcnt[b] <= 0)
    panic("blockmeta_dec_snap: underflow");

  block_snapcnt[b]--;
  ref  = block_refcnt[b];
  snap = block_snapcnt[b];
  release(&block_meta_lock);

  blockmeta_write_record(b, ref, snap);
}

