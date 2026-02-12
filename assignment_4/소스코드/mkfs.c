#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#define stat xv6_stat  // avoid clash with host struct stat
#include "types.h"
#include "fs.h"
#include "stat.h"
#include "param.h"

#ifndef static_assert
#define static_assert(a, b) do { switch (0) case 0: case (a): ; } while (0)
#endif

#define NINODES 200


// Disk layout:
// [ boot block | sb block | log | inode blocks | free bit map | data blocks ]

int nbitmap = FSSIZE/(BSIZE*8) + 1;
int ninodeblocks = NINODES / IPB + 1;
int nlog = LOGSIZE;
int nmeta;    // Number of meta blocks (boot, sb, nlog, inode, bitmap)
int nblocks;  // Number of data blocks

int fsfd;
struct superblock sb;
char zeroes[BSIZE];
uint freeinode = 1;
uint freeblock;


void balloc(int);
void wsect(uint, void*);
void winode(uint, struct dinode*);
void rinode(uint inum, struct dinode *ip);
void rsect(uint sec, void *buf);
uint ialloc(ushort type);
void iappend(uint inum, void *p, int n);

// convert to intel byte order
ushort
xshort(ushort x)
{
  ushort y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

uint
xint(uint x)
{
  uint y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  a[2] = x >> 16;
  a[3] = x >> 24;
  return y;
}

int
main(int argc, char *argv[])
{
  int i, cc, fd;
  uint rootino, inum, off;
  struct dirent de;
  char buf[BSIZE];
  struct dinode din;

  static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

  if(argc < 2){
    fprintf(stderr, "Usage: mkfs fs.img files...\n");
    exit(1);
  }

  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct dirent)) == 0);

  fsfd = open(argv[1], O_RDWR|O_CREAT|O_TRUNC, 0666);
  if(fsfd < 0){
    perror(argv[1]);
    exit(1);
  }

  // 1 fs block = 1 disk sector
  nmeta = 2 + nlog + ninodeblocks + nbitmap;
  nblocks = FSSIZE - nmeta;

  sb.size      = xint(FSSIZE);
  sb.nblocks   = xint(nblocks);
  sb.ninodes   = xint(NINODES);
  sb.nlog      = xint(nlog);
  sb.logstart  = xint(2);
  sb.inodestart= xint(2+nlog);
  sb.bmapstart = xint(2+nlog+ninodeblocks);

  printf("nmeta %d (boot, super, log blocks %u inode blocks %u, bitmap blocks %u) blocks %d total %d\n",
         nmeta, nlog, ninodeblocks, nbitmap, nblocks, FSSIZE);

  // 메타 블록(boot, super, log, inode, bitmap)까지는 이미 예약된 상태
  freeblock = nmeta;     // data block 중 첫 free 블록 번호

  // 디스크 전체 0으로 초기화
  for(i = 0; i < FSSIZE; i++)
    wsect(i, zeroes);

  // superblock 기록
  memset(buf, 0, sizeof(buf));
  memmove(buf, &sb, sizeof(sb));
  wsect(1, buf);

  // 루트 디렉토리 inode 할당
  rootino = ialloc(T_DIR);
  assert(rootino == ROOTINO);

  // 루트 디렉토리 엔트리 "." 추가
  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, ".");
  iappend(rootino, &de, sizeof(de));

  // 루트 디렉토리 엔트리 ".." 추가
  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(rootino, &de, sizeof(de));

  // -----------------------------
  // argv[2..] 유저 프로그램 파일들 복사 (기존 xv6 mkfs 코드 그대로)
  // -----------------------------
  for(i = 2; i < argc; i++){
    assert(index(argv[i], '/') == 0);

    if((fd = open(argv[i], 0)) < 0){
      perror(argv[i]);
      exit(1);
    }

    // 파일 이름이 _rm, _cat 처럼 _로 시작하면, 파일시스템에는 rm, cat으로 저장
    if(argv[i][0] == '_')
      ++argv[i];

    inum = ialloc(T_FILE);

    bzero(&de, sizeof(de));
    de.inum = xshort(inum);
    strncpy(de.name, argv[i], DIRSIZ);
    iappend(rootino, &de, sizeof(de));

    while((cc = read(fd, buf, sizeof(buf))) > 0)
      iappend(inum, buf, cc);

    close(fd);
  }

  // -------------------------------------------------
  // 여기부터 새로 추가: /snapshot 디렉토리 & /snapshot/blockmeta 파일 생성
  // -------------------------------------------------

  // 1) /snapshot 디렉토리 inode 할당
  uint snapdir_ino = ialloc(T_DIR);

  // 루트 디렉토리에 "snapshot" 엔트리 추가
  bzero(&de, sizeof(de));
  de.inum = xshort(snapdir_ino);
  strcpy(de.name, "snapshot");
  iappend(rootino, &de, sizeof(de));

  // /snapshot/. 엔트리
  bzero(&de, sizeof(de));
  de.inum = xshort(snapdir_ino);
  strcpy(de.name, ".");
  iappend(snapdir_ino, &de, sizeof(de));

  // /snapshot/.. 엔트리 (부모는 루트)
  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(snapdir_ino, &de, sizeof(de));

  // 2) /snapshot/blockmeta 파일 inode 할당
  uint meta_ino = ialloc(T_FILE);

  // /snapshot 디렉토리에 "blockmeta" 엔트리 추가
  bzero(&de, sizeof(de));
  de.inum = xshort(meta_ino);
  strcpy(de.name, "blockmeta");
  iappend(snapdir_ino, &de, sizeof(de));

  // 3) /snapshot/blockmeta 파일 내용 채우기
  //
  //    - block 0 ~ FSSIZE-1 까지 각 블록에 대해 struct block_meta 한 개씩
  //    - 즉, 총 FSSIZE 개 레코드
  //    - 레코드 배열을 BSIZE 단위로 잘라서 iappend로 쓰기
  //
  int n_records     = FSSIZE;
  int n_meta_blocks = (n_records + BLOCKMETA_RPB - 1) / BLOCKMETA_RPB;

  printf("mkfs: creating /snapshot/blockmeta with %d records, %d blocks\n",
         n_records, n_meta_blocks);

  for(int bi = 0; bi < n_meta_blocks; bi++){
    struct block_meta mbuf[BLOCKMETA_RPB];
    memset(mbuf, 0, sizeof(mbuf));

    // 항상 BSIZE 만큼 append (mbuf 전체)
    iappend(meta_ino, (char*)mbuf, BSIZE);
  }

  // -------------------------------------------------
  // 새로 추가 끝
  // -------------------------------------------------

  // 루트 디렉토리 크기를 BSIZE 배수로 맞춤
  // 위에서 "snapshot" 엔트리까지 이미 다 추가된 상태의 size 기준
  rinode(rootino, &din);
  off = xint(din.size);
  off = ((off/BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(rootino, &din);

  // 지금까지 사용한 데이터 블록 수(freeblock)를 기준으로
  // 비트맵 블록(bmapstart)에 할당 비트 설정
  balloc(freeblock);

  exit(0);
}


void
wsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE){
    perror("lseek");
    exit(1);
  }
  if(write(fsfd, buf, BSIZE) != BSIZE){
    perror("write");
    exit(1);
  }
}

void
winode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *dip = *ip;
  wsect(bn, buf);
}

void
rinode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *ip = *dip;
}

void
rsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE){
    perror("lseek");
    exit(1);
  }
  if(read(fsfd, buf, BSIZE) != BSIZE){
    perror("read");
    exit(1);
  }
}

uint
ialloc(ushort type)
{
  uint inum = freeinode++;
  struct dinode din;

  bzero(&din, sizeof(din));
  din.type = xshort(type);
  din.nlink = xshort(1);
  din.size = xint(0);
  winode(inum, &din);
  return inum;
}

void
balloc(int used)
{
  uchar buf[BSIZE];
  int i;

  printf("balloc: first %d blocks have been allocated\n", used);
  assert(used < BSIZE*8);
  bzero(buf, BSIZE);
  for(i = 0; i < used; i++){
    buf[i/8] = buf[i/8] | (0x1 << (i%8));
  }
  printf("balloc: write bitmap block at sector %d\n", sb.bmapstart);
  wsect(sb.bmapstart, buf);
}

#define min(a, b) ((a) < (b) ? (a) : (b))

void
iappend(uint inum, void *xp, int n)
{
  char *p = (char*)xp;
  uint fbn, off, n1;
  struct dinode din;
  char buf[BSIZE];
  uint indirect[NINDIRECT];
  uint x;

  rinode(inum, &din);
  off = xint(din.size);
  // printf("append inum %d at off %d sz %d\n", inum, off, n);
  while(n > 0){
    fbn = off / BSIZE;
    assert(fbn < MAXFILE);
    if(fbn < NDIRECT){
      if(xint(din.addrs[fbn]) == 0){
        din.addrs[fbn] = xint(freeblock++);
      }
      x = xint(din.addrs[fbn]);
    } else {
      if(xint(din.addrs[NDIRECT]) == 0){
        din.addrs[NDIRECT] = xint(freeblock++);
      }
      rsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      if(indirect[fbn - NDIRECT] == 0){
        indirect[fbn - NDIRECT] = xint(freeblock++);
        wsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      }
      x = xint(indirect[fbn-NDIRECT]);
    }
    n1 = min(n, (fbn + 1) * BSIZE - off);
    rsect(x, buf);
    bcopy(p, buf + off - (fbn * BSIZE), n1);
    wsect(x, buf);
    n -= n1;
    off += n1;
    p += n1;
  }
  din.size = xint(off);
  winode(inum, &din);
}
