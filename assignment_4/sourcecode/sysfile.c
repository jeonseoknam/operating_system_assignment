//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// print_addr를 위해서 추가
#include "buf.h"

// /snapshot/blockmeta 덤프 시 한 트랜잭션서 처리할 블록 개수
#define BLOCKMETA_CHUNK 16

// 스냅샷 ID 관리
static struct spinlock snap_lock;
static int snap_inited = 0;
static int next_snap_id = 1;

// 스냅샷/복구용 헬퍼 함수 프로토타입
static struct inode *create_subdir(struct inode *parent, char *name);
static struct inode *create_snapshot_file(struct inode *parent, char *name);
static struct inode *create_file(struct inode *parent, char *name);
static void          dir_unlink_at(struct inode *dp, uint off, struct inode *ip);
static int           snapshot_clone_dir(struct inode *src_dir, struct inode *dst_dir, int is_root);
static int           rollback_clone_dir(struct inode *src_snap_dir, struct inode *dst_dir);
static int           snapshot_remove_tree(struct inode *dir);
static int           clear_root_except_snapshot(void);


// fs.c 에서 제공
extern void rebuild_block_meta(int dev);
extern void cleanup_orphan_blocks(int dev);
extern void itrunc(struct inode *ip);        // 새로 추가
extern struct superblock sb;                 // sb.size 사용용
extern int block_refcnt[FSSIZE];
extern int block_snapcnt[FSSIZE];
extern struct spinlock block_meta_lock;

static void inc_snap_for_inode_blocks(struct inode *ip);


// refcnt[b]++, snapcnt[b]++ (is_snap != 0 인 경우, 즉 스냅샷이 생성되는 경우)
extern void blockmeta_add_ref(uint b, int is_snap);

// refcnt[b]++ (일반 파일용, snapcnt는 건드리지 않음)
extern void blockmeta_add_ref_nosnap(uint b);

// snapcnt[b]-- (스냅샷 파일이 사라질 때)
extern void blockmeta_dec_snap(uint b);


// 커널용 문자열 함수 프로토타입(string.c에 구현되어 있음)
int strcmp(const char*, const char*);

// safestrcpy와 비슷한 스타일의 safe strcat 구현
static void
safestrcat(char *dst, const char *src, int dstsz)
{
  int n = 0;

  // dst에서 이미 채워진 길이 찾기
  while(n < dstsz && dst[n] != 0)
    n++;

  // src를 dst 끝에 붙이되, 항상 마지막에 '\0' 보장
  for(int i = 0; src[i] && n + 1 < dstsz; i++, n++)
    dst[n] = src[i];

  dst[n] = 0;
}

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *curproc = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd] == 0){
      curproc->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

int
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

int
sys_read(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return fileread(f, p, n);
}

int
sys_write(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return filewrite(f, p, n);
}

int
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

int
sys_fstat(void)
{
  struct file *f;
  struct stat *st;

  if(argfd(0, 0, &f) < 0 || argptr(1, (void*)&st, sizeof(*st)) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
int
sys_link(void)
{
  char name[DIRSIZ], *new, *old;
  struct inode *dp, *ip;

  if(argstr(0, &old) < 0 || argstr(1, &new) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

//PAGEBREAK!
int
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], *path;
  uint off;

  if(argstr(0, &path) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;
  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && ip->type == T_FILE)
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

int
sys_open(void)
{
  char *path;
  int fd, omode;
  struct file *f;
  struct inode *ip;

  if(argstr(0, &path) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
	// 1) 디렉토리면 쓰기 금지
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }

    // 2) 스냅샷(T_SNAP) 파일은 읽기 전용으로만 open 허용
    if(ip->type == T_SNAP){
      // 쓰기 관련 모드가 하나라도 있으면 에러
      if(omode & (O_WRONLY | O_RDWR)){
        iunlockput(ip);
        end_op();
        return -1;
      }
      // 여기서 omode를 강제로 O_RDONLY로 normalize
      omode = O_RDONLY;
    }
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  end_op();

  f->type = FD_INODE;
  f->ip = ip;
  f->off = 0;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  return fd;
}

int
sys_mkdir(void)
{
  char *path;
  struct inode *ip;

  begin_op();
  if(argstr(0, &path) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_mknod(void)
{
  struct inode *ip;
  char *path;
  int major, minor;

  begin_op();
  if((argstr(0, &path)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEV, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_chdir(void)
{
  char *path;
  struct inode *ip;
  struct proc *curproc = myproc();
  
  begin_op();
  if(argstr(0, &path) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(curproc->cwd);
  end_op();
  curproc->cwd = ip;
  return 0;
}

int
sys_exec(void)
{
  char *path, *argv[MAXARG];
  int i;
  uint uargv, uarg;

  if(argstr(0, &path) < 0 || argint(1, (int*)&uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv))
      return -1;
    if(fetchint(uargv+4*i, (int*)&uarg) < 0)
      return -1;
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    if(fetchstr(uarg, &argv[i]) < 0)
      return -1;
  }
  return exec(path, argv);
}

int
sys_pipe(void)
{
  int *fd;
  struct file *rf, *wf;
  int fd0, fd1;

  if(argptr(0, (void*)&fd, 2*sizeof(fd[0])) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      myproc()->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  fd[0] = fd0;
  fd[1] = fd1;
  return 0;
}

// 커널 내부에서 쓸 mkdir helper 
static int
kern_mkdir(char *path)
{
  struct inode *ip;
  begin_op();
  ip = create(path, T_DIR, 0, 0);
  if(ip == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

static int
ensure_snapshot_root(void)
{
  struct inode *ip;

  begin_op();
  ip = namei("/snapshot");
  if(ip == 0) {
    end_op();
    // 없다면 생성 시도
    if (kern_mkdir("/snapshot") < 0)
      return -1;
    return 0;
  } else {
    iput(ip);  // namei() -> iput()
    end_op();
    return 0;
  }
}

static void
init_snap_once(void)
{
  if(!snap_inited){
    initlock(&snap_lock, "snap_lock");
    snap_inited = 1;
  }
}

// 10진수로 id를 문자열로 (유틸)
static void itoa_dec(int x, char *buf)
{
  char tmp[16];
  int n = 0;
  if(x == 0){ buf[0]='0'; buf[1]=0; return; }
  while(x > 0){ tmp[n++] = '0' + (x % 10); x /= 10; }
  for(int i=0;i<n;i++) buf[i] = tmp[n-1-i];
  buf[n] = 0;
}

// x를 16진수 문자열로 변환 (0x prefix 없이, %x와 같은 형식)
/*
static void
itoa_hex(uint x, char *buf)
{
  char tmp[16];
  int n = 0;

  if (x == 0) {
    buf[0] = '0';
    buf[1] = 0;
    return;
  }

  while (x > 0) {
    int d = x & 0xF;   // 16진수 한 자리
    if (d < 10)
      tmp[n++] = '0' + d;
    else
      tmp[n++] = 'a' + (d - 10);  // 소문자 hex: a~f
    x >>= 4;
  }

  // 역순으로 뒤집어서 buf에 저장
  for (int i = 0; i < n; i++)
    buf[i] = tmp[n - 1 - i];
  buf[n] = 0;
}
*/

//  snapshot_create는 static이 아니어야 함
//  (sys_snapshot_create()에서 호출해야 하니까)
int
snapshot_create(void)
{
  init_snap_once();

  // 1) /snapshot 디렉토리 보장
  if (ensure_snapshot_root() < 0)
    return -1;

  // 2) 새 ID 배정
  int id;
  acquire(&snap_lock);
  id = next_snap_id++;
  release(&snap_lock);

  // 3) /snapshot/<id> 디렉토리 생성
  char name[16], path[32];
  itoa_dec(id, name);
  safestrcpy(path, "/snapshot/", sizeof(path));
  safestrcat(path, name, sizeof(path));

  if (kern_mkdir(path) < 0)
    return -1;

  // 4) 원본 루트와 새 루트를 inode로 얻음
  begin_op();

  struct inode *root = iget(ROOTDEV, ROOTINO);
  struct inode *snap_root = namei(path);  // 방금 만든 /snapshot/<id>

  if(root == 0 || snap_root == 0){
    end_op();
    return -1;
  }

  ilock(root);
  ilock(snap_root);

  // 5) "/" 아래를 재귀 복사하되, /snapshot과 T_DEV는 제외
  snapshot_clone_dir(root, snap_root, 1 /* is_root */);

  iunlockput(snap_root);
  iunlockput(root);

  end_op();

  return id;
}



// 현재 파일시스템을 /snapshot/<id> 상태로 롤벡하는 함수
int
snapshot_rollback(int id)
{
  if(id < 0){
		  return -1;
  }
  char name[16], path[32];
  itoa_dec(id, name);
  safestrcpy(path, "/snapshot/", sizeof(path));
  safestrcat(path, name, sizeof(path));

  begin_op();

  // /snapshot/<id> 찾기
  struct inode *snap_root = namei(path);
  if(snap_root == 0){
    end_op();
    return -1; // invalid id
  }

  // 1) 현재 루트 정리 (snapshot 디렉토리 제외)
  clear_root_except_snapshot();

  // 2) 스냅샷 내용을 루트로 복사
  struct inode *root = iget(ROOTDEV, ROOTINO);
  ilock(root);
  ilock(snap_root);

  rollback_clone_dir(snap_root, root);

  iunlockput(snap_root);
  iunlockput(root);

  end_op();

  return 0;
}



// /snapshot/<id> 트리 전체 삭제(디렉토리/파일 unlink)
// 그 결과를 바탕으로 rebuild_block_meta()를 다시 호출
int
snapshot_delete(int id)
{
  if(id < 0){
		  return -1;
  }

  char name[16], path[32];
  itoa_dec(id, name);
  safestrcpy(path, "/snapshot/", sizeof(path));
  safestrcat(path, name, sizeof(path));


  // ----------------------------
  // 1단계: /snapshot/<id> 디렉토리 트리 삭제 + 엔트리 unlink
  // ----------------------------
  begin_op();


  struct inode *snap_dir = namei(path);
  if (snap_dir == 0) {
    end_op();
    return -1;
  }

  ilock(snap_dir);
  snapshot_remove_tree(snap_dir);

  iunlock(snap_dir);
  iput(snap_dir);


  struct inode *parent;
  char base[DIRSIZ];

  parent = nameiparent(path, base);   // parent = "/snapshot"
  if (parent == 0) {
    end_op();
    return -1;
  }

  ilock(parent);

  struct dirent de;
  uint off;
  int found = 0;

  for (off = 0; off < parent->size; off += sizeof(de)) {
    if (readi(parent, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("snapshot_delete: readi parent");

    if (de.inum == 0)
      continue;
    if (strcmp(de.name, base) != 0)
      continue;

    struct inode *ip = iget(parent->dev, de.inum);
    ilock(ip);
    dir_unlink_at(parent, off, ip);
    iunlock(ip);
    iput(ip);

    found = 1;
    break;
  }

  if (!found) {
    iunlockput(parent);
    end_op();
    return -1;
  }

  iunlockput(parent);

  end_op();   // step1: 트리 삭제 끝

  cleanup_orphan_blocks(ROOTDEV);     // 비트맵과 비교해서 실제로 bfree

  return 0;
}


// 디렉토리 복사용 헬퍼, 디렉토리 하나를 만드는 헬퍼
// src_dir: 원본 디렉토리 (root 또는 하위)
// dst_dir: /snapshot/<id> 안의 대응 디렉토리
// is_root: src_dir가 "/" 루트인지 여부 ("/snapshot" 제외 처리를 위해)
// src_dir: 현재 파일시스템의 디렉토리 ("/" 또는 그 하위)
// dst_dir: /snapshot/<id> 쪽의 대응 디렉토리
// is_root: src_dir가 실제 루트("/")인지 여부
//          - 루트일 때만 "snapshot" 엔트리를 스킵한다.
static int
snapshot_clone_dir(struct inode *src_dir, struct inode *dst_dir, int is_root)
{
  struct dirent de;
  uint off;

  if(src_dir->type != T_DIR || dst_dir->type != T_DIR)
    panic("snapshot_clone_dir: not dir");

  for(off = 0; off < src_dir->size; off += sizeof(de)){
    if(readi(src_dir, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("snapshot_clone_dir: readi");

    if(de.inum == 0)
      continue;
    if(strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
      continue;

    // 루트("/")에서만 /snapshot 디렉토리는 캡처하지 않음
    if(is_root && strcmp(de.name, "snapshot") == 0)
      continue;

    // 원본 child inode
    struct inode *child = iget(src_dir->dev, de.inum);
    ilock(child);

    if(child->type == T_DEV){
      // T_DEV는 스냅샷 대상에서 제외
      iunlockput(child);
      continue;
    }

    if(child->type == T_DIR){
      // 스냅샷 트리 쪽에 대응 디렉토리 하나 생성
      struct inode *newdir = create_subdir(dst_dir, de.name);
      // child, newdir 모두 ilock 상태로 재귀 호출
      snapshot_clone_dir(child, newdir, 0 /* is_root=0 */);
      iunlockput(newdir);
    } else if(child->type == T_FILE){
      // 스냅샷용 파일(T_SNAP) inode 생성
      struct inode *snap_file = create_snapshot_file(dst_dir, de.name);

      // 원본 파일과 동일한 블록을 가리키도록 size/addrs 복사
      snap_file->size = child->size;
      memmove(snap_file->addrs, child->addrs, sizeof(child->addrs));
      iupdate(snap_file);

      //  이 스냅샷 파일이 참조하는 모든 블록에 대해
      //    refcnt++ + snapcnt++ 반영
      inc_snap_for_inode_blocks(snap_file);

      iunlockput(snap_file);
    } else {
      // 설계상 T_DIR / T_FILE / T_DEV만 나와야 한다.
      panic("snapshot_clone_dir: unexpected itype");
    }

    iunlockput(child);
  }

  return 0;
}




// parent 디렉토리 아래에 이름이 name인 새 디렉토리를 만든다.
// - parent 는 이미 ilock(parent) 가 잡혀 있어야 함.
// - begin_op()/end_op() 는 바깥에서 처리한다고 가정.
// - 성공 시: 새로 만든 디렉토리 inode* 를 "락 잡힌 상태"로 리턴.
// - 실패 상황(이미 같은 이름이 있거나 ialloc 실패 등)에서는 panic 또는 0 리턴 등으로 처리.
static struct inode *
create_subdir(struct inode *parent, char *name)
{
  struct inode *ip;

  // 혹시 같은 이름이 이미 있으면 버그이므로 panic 처리
  if((ip = dirlookup(parent, name, 0)) != 0){
    iput(ip);
    panic("create_subdir: entry already exists");
  }

  // 새 디렉토리 inode 할당
  if((ip = ialloc(parent->dev, T_DIR)) == 0)
    panic("create_subdir: ialloc");

  ilock(ip);

  ip->major = 0;
  ip->minor = 0;
  ip->nlink = 1;       // 자기 자신을 가리키는 링크('.')
  ip->size  = 0;
  memset(ip->addrs, 0, sizeof(ip->addrs));
  iupdate(ip);

  // parent 의 nlink 는 ".." 링크 때문에 1 증가
  parent->nlink++;
  iupdate(parent);

  // 새 디렉토리 안에 "." 과 ".." 엔트리 생성
  if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", parent->inum) < 0)
    panic("create_subdir: create dots");

  // parent 디렉토리에 이 디렉토리를 연결
  if(dirlink(parent, name, ip->inum) < 0)
    panic("create_subdir: dirlink");

  // ip 는 여전히 ilock(ip) 잡힌 상태로 리턴 (caller가 사용 후 iunlockput 등)
  return ip;
}

// parent 디렉토리 아래에 이름이 name인 일반 파일(T_FILE)을 만든다.
// - parent 는 이미 ilock(parent) 상태여야 함.
// - begin_op()/end_op() 는 바깥에서 처리한다고 가정.
// - 성공 시: 새로 만든 파일 inode* 를 "락 잡힌 상태"로 리턴.
static struct inode *
create_file(struct inode *parent, char *name)
{
  struct inode *ip;

  // 같은 이름이 이미 있으면 버그로 간주
  if((ip = dirlookup(parent, name, 0)) != 0){
    iput(ip);
    panic("create_file: entry already exists");
  }

  // 새 inode 할당(T_FILE)
  if((ip = ialloc(parent->dev, T_FILE)) == 0)
    panic("create_file: ialloc");

  ilock(ip);

  ip->major = 0;
  ip->minor = 0;
  ip->nlink = 1;      // parent에서의 한 개 링크
  ip->size  = 0;
  memset(ip->addrs, 0, sizeof(ip->addrs));
  iupdate(ip);

  // parent 디렉토리에 연결
  if(dirlink(parent, name, ip->inum) < 0)
    panic("create_file: dirlink");

  // ip 는 여전히 ilock(ip) 상태로 리턴
  return ip;
}


// parent 디렉토리 아래에 이름이 name인 스냅샷 파일(T_SNAP)을 만든다.
// - parent 는 이미 ilock(parent) 상태여야 함.
// - 데이터 블록 매핑(addrs[])는 나중에 호출자가 채울 것임.
// - 성공 시: 새 스냅샷 inode* 를 "락 잡힌 상태"로 리턴.
static struct inode *
create_snapshot_file(struct inode *parent, char *name)
{
  struct inode *ip;

  // 이미 같은 이름의 엔트리가 있으면 버그이므로 panic
  if((ip = dirlookup(parent, name, 0)) != 0){
    iput(ip);
    panic("create_snapshot_file: entry already exists");
  }

  // 새 inode 할당 (스냅샷 파일 타입)
  if((ip = ialloc(parent->dev, T_SNAP)) == 0)
    panic("create_snapshot_file: ialloc");

  ilock(ip);

  ip->major = 0;
  ip->minor = 0;
  ip->nlink = 1;     // parent 디렉토리에서의 한 개 링크
  ip->size  = 0;     // 실제 size / addrs[]는 이후에 원본 파일을 보고 복사
  memset(ip->addrs, 0, sizeof(ip->addrs));
  iupdate(ip);

  // parent 디렉토리에 연결
  if(dirlink(parent, name, ip->inum) < 0)
    panic("create_snapshot_file: dirlink");

  // ip 는 ilock(ip) 상태로 리턴
  return ip;
}

// ---------------------------------------------------------
//  스냅샷/롤백용 메타데이터 갱신 헬퍼
//   - inc_snap_for_inode_blocks : T_SNAP이 참조하는 블록들에 대해 (refcnt++, snapcnt++)
//   - dec_snap_for_inode_blocks : T_SNAP이 참조하던 블록들에 대해 (snapcnt--)
//   - inc_ref_for_inode_blocks  : T_FILE이 참조하는 블록들에 대해 (refcnt++)
// ---------------------------------------------------------

// T_SNAP inode가 참조하는 모든 블록에 대해
//   refcnt++ + snapcnt++ 수행
static void
inc_snap_for_inode_blocks(struct inode *ip)
{
  int i;
  uint b;
  struct buf *bp;
  uint *a;

  if(ip->type != T_SNAP)
    panic("inc_snap_for_inode_blocks: not T_SNAP");

  // 1) direct blocks
  for(i = 0; i < NDIRECT; i++){
    b = ip->addrs[i];
    if(b)
      blockmeta_add_ref(b, 1);   // refcnt++, snapcnt++
  }

  // 2) indirect block + 그 안의 엔트리들
  if(ip->addrs[NDIRECT]){
    uint ib = ip->addrs[NDIRECT];

    // 간접 블록 자체도 하나의 블록이므로 ref/snap++
    blockmeta_add_ref(ib, 1);

    bp = bread(ip->dev, ib);
    a  = (uint*)bp->data;
    for(i = 0; i < NINDIRECT; i++){
      b = a[i];
      if(b)
        blockmeta_add_ref(b, 1);   // refcnt++, snapcnt++
    }
    brelse(bp);
  }
}

// T_SNAP inode가 참조하던 모든 블록에 대해
//   snapcnt-- 만 수행 (refcnt는 itrunc/bdecref 에 맡김)
static void
dec_snap_for_inode_blocks(struct inode *ip)
{
  int i;
  uint b;
  struct buf *bp;
  uint *a;

  if(ip->type != T_SNAP)
    panic("dec_snap_for_inode_blocks: not T_SNAP");

  // 1) direct blocks
  for(i = 0; i < NDIRECT; i++){
    b = ip->addrs[i];
    if(b)
      blockmeta_dec_snap(b);   // snapcnt--
  }

  // 2) indirect block + 그 안의 엔트리들
  if(ip->addrs[NDIRECT]){
    uint ib = ip->addrs[NDIRECT];

    blockmeta_dec_snap(ib);    // 간접 블록 자체

    bp = bread(ip->dev, ib);
    a  = (uint*)bp->data;
    for(i = 0; i < NINDIRECT; i++){
      b = a[i];
      if(b)
        blockmeta_dec_snap(b); // snapcnt--
    }
    brelse(bp);
  }
}

// T_FILE inode가 참조하는 모든 블록에 대해
//   refcnt++ 만 수행 (snapcnt는 건드리지 않음)
//
// rollback 시에 T_SNAP → T_FILE 로 복구할 때
//   새로 생긴 일반 파일이 기존 블록을 같이 쓰므로 refcnt 를 늘려주는 용도
static void
inc_ref_for_inode_blocks(struct inode *ip)
{
  int i;
  uint b;
  struct buf *bp;
  uint *a;

  if(ip->type != T_FILE)
    panic("inc_ref_for_inode_blocks: not T_FILE");

  // 1) direct blocks
  for(i = 0; i < NDIRECT; i++){
    b = ip->addrs[i];
    if(b)
      blockmeta_add_ref_nosnap(b);   // refcnt++
  }

  // 2) indirect block + 그 안의 엔트리들
  if(ip->addrs[NDIRECT]){
    uint ib = ip->addrs[NDIRECT];

    blockmeta_add_ref_nosnap(ib);    // 간접 블록 자체도 refcnt++

    bp = bread(ip->dev, ib);
    a  = (uint*)bp->data;
    for(i = 0; i < NINDIRECT; i++){
      b = a[i];
      if(b)
        blockmeta_add_ref_nosnap(b); // refcnt++
    }
    brelse(bp);
  }
}


// 트리 삭제 헬퍼(snapshot_delect(id)의 헬퍼)
// dir: 이미 ilock(dir) 상태여야 함
// 역할: dir 아래의 모든 엔트리를 재귀적으로 삭제 (스냅샷 트리 전체 제거)
//       단, '.' / '..' 은 건드리지 않음.
//       dir 자신은 여기서 unlink하지 않고, snapshot_delete() 쪽에서
//       부모 디렉토리에서 지워준다.
static int
snapshot_remove_tree(struct inode *dir)
{
  struct dirent de;
  uint off;

  if(dir->type != T_DIR)
    panic("snapshot_remove_tree: not dir");

  // dir의 dirent들을 처음부터 끝까지 스캔
  for(off = 0; off < dir->size; off += sizeof(de)){
    if(readi(dir, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("snapshot_remove_tree: readi");

    // 빈 엔트리면 넘어감
    if(de.inum == 0)
      continue;

    // "." / ".." 는 건드리지 않음
    if(strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
      continue;

    // 자식 inode 얻기
    struct inode *child = iget(dir->dev, de.inum);
    ilock(child);

    if(child->type == T_DIR){
      // 하위 디렉토리라면, 그 안을 먼저 재귀적으로 싹 비운다.
      snapshot_remove_tree(child);
      // 그리고 현재 dir에서 이 디렉토리 엔트리를 제거
      dir_unlink_at(dir, off, child);
      iunlock(child);
      iput(child);
    } else {
      //  스냅샷 파일(T_SNAP)이면 먼저 snapcnt-- 반영
      if(child->type == T_SNAP){
        dec_snap_for_inode_blocks(child);
      }

      // 그 다음 디렉토리 엔트리 unlink + nlink 감소
      dir_unlink_at(dir, off, child);
      iunlock(child);
      iput(child);
    }
  }

  // dir 자체는 여기서 제거하지 않는다.
  // /snapshot/<id> 디렉토리 자체를 없애는 건 snapshot_delete() 가
  // 부모 디렉토리(/snapshot)에서 이름 <id> 를 unlink하면서 처리.
  return 0;
}




// dp: 부모 디렉토리 (락 잡힌 상태여야 함)
// off: dp 내에서 이 엔트리가 위치한 바이트 오프셋
// ip: 이 엔트리가 가리키는 자식 inode (락 잡힌 상태여야 함)
//
// 역할:
//   - dp의 off 위치에 있는 dirent를 비우고 (de.inum = 0)
//   - 디렉토리라면 dp->nlink--
//   - ip->nlink-- 하고 iupdate(ip)
//   - 락 해제/ iput 은 caller가 한다 (여기서는 하지 않음)
static void
dir_unlink_at(struct inode *dp, uint off, struct inode *ip)
{
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dir_unlink_at: dp not dir");

  // dirent 비우기
  memset(&de, 0, sizeof(de));
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("dir_unlink_at: writei");

  // 디렉토리였으면 부모 링크 감소 ( ".." 제거 효과 )
  if(ip->type == T_DIR){
    if(dp->nlink < 1)
      panic("dir_unlink_at: dp nlink < 1");
    dp->nlink--;
    iupdate(dp);
  }

  // 자식 inode의 링크 감소
  if(ip->nlink < 1)
    panic("dir_unlink_at: ip nlink < 1");
  ip->nlink--;
  iupdate(ip);
}

// 루프 디렉토리 아래의 모든 파일/디렉토리(snapshot 제외)를 정리하는 헬퍼 함수
// 루트 디렉토리("/")에서
//  - "." , ".." 는 건드리지 않고
//  - "snapshot" 디렉토리는 남겨두고
//  - 나머지 모든 엔트리를 삭제(파일/디렉토리 전부 재귀 삭제)
//
// 호출 전후에 begin_op()/end_op() 는 바깥에서 처리한다고 가정.
static int
clear_root_except_snapshot(void)
{
  struct inode *root = iget(ROOTDEV, ROOTINO);
  ilock(root);

  struct dirent de;
  uint off;

  for(off = 0; off < root->size; off += sizeof(de)){
    if(readi(root, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("clear_root: readi");

    if(de.inum == 0)
      continue;

    // ".", ".."는 삭제 금지
    if(strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
      continue;

    // "/snapshot" 디렉토리는 롤백 시에도 남겨둔다
    if(strcmp(de.name, "snapshot") == 0)
      continue;

    // 이제 이 엔트리는 제거 대상
    struct inode *child = iget(root->dev, de.inum);
    ilock(child);

    if(child->type == T_DIR){
      // 하위 디렉토리면, 그 안을 먼저 싹 비운다 (스냅샷이 아니어도 일반 디렉토리)
      snapshot_remove_tree(child);
      // 그리고 root 에서 이 디렉토리 엔트리를 제거
      dir_unlink_at(root, off, child);
      iunlock(child);
      iput(child);
    } else {
      // 일반 파일/장치 파일(T_FILE, T_DEV 등)인 경우, 바로 unlink
      dir_unlink_at(root, off, child);
      iunlock(child);
      iput(child);
    }
  }

  iunlockput(root);
  return 0;
}

// src_snap_dir: /snapshot/<id> 트리 안의 디렉토리
// dst_dir     : 현재 파일시스템 쪽 디렉토리 ("/" 또는 그 하위)
// 역할:
//   - src_snap_dir의 내용을 그대로 dst_dir 아래에 재귀적으로 생성한다.
//   - 스냅샷 디렉토리(T_DIR)는 일반 T_DIR로,
//   - 스냅샷 파일(T_SNAP)은 일반 T_FILE로 복구하되,
//     data block(addrs[])는 그대로 공유해서 COW 조건만 만족시키도록 한다.
static int
rollback_clone_dir(struct inode *src_snap_dir, struct inode *dst_dir)
{
  struct dirent de;
  uint off;

  if(src_snap_dir->type != T_DIR || dst_dir->type != T_DIR)
    panic("rollback_clone_dir: not dir");

  for(off = 0; off < src_snap_dir->size; off += sizeof(de)){
    if(readi(src_snap_dir, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("rollback_clone_dir: readi");

    if(de.inum == 0)
      continue;
    if(strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
      continue;

    struct inode *src = iget(src_snap_dir->dev, de.inum);
    ilock(src);

    if(src->type == T_DIR){
      // 새 디렉토리(T_DIR) 생성
      struct inode *newdir = create_subdir(dst_dir, de.name);
      // 양쪽 디렉토리는 ilock 상태에서 재귀
      rollback_clone_dir(src, newdir);
      iunlockput(newdir);
    } else if(src->type == T_SNAP){
      // 현재 파일시스템용 일반 파일(T_FILE) 생성
      struct inode *file = create_file(dst_dir, de.name);

      file->size = src->size;
      memmove(file->addrs, src->addrs, sizeof(file->addrs));
      iupdate(file);

	  //  새로 생긴 일반 파일이 같은 블록을 같이 쓰므로
      //  해당 블록들의 refcnt++ (snapcnt는 그대로)
      inc_ref_for_inode_blocks(file);

      iunlockput(file);
    } else {
      // 설계상 스냅샷 트리에는 T_DIR / T_SNAP만 있어야 함
      panic("rollback_clone_dir: unexpected itype");
    }

    iunlockput(src);
  }

  return 0;
}

// 유저 프로그램 print_addr를 위한 시스템콜
int
sys_print_addr(void)
{
  char *path;
  if(argstr(0, &path) < 0)
    return -1;

  begin_op();
  struct inode *ip = namei(path);
  if(ip == 0){
    end_op();
    cprintf("print_addr: no such file %s\n", path);
    return -1;
  }

  ilock(ip);

  if(ip->type != T_FILE && ip->type != T_SNAP){
    cprintf("print_addr: not a regular file (type=%d)\n", ip->type);
    iunlockput(ip);
    end_op();
    return -1;
  }

  // 1) direct block 주소들 출력
  int i;
  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i] == 0)
      break;
    cprintf("addr[%d] : %x\n", i, ip->addrs[i]);
  }

  // 2) indirect pointer + 첫 번째 indirect 엔트리 예시 출력
  if(ip->addrs[NDIRECT]){
    uint indb = ip->addrs[NDIRECT];
    cprintf("addr[%d] : %x (INDIRECT POINTER)\n", NDIRECT, indb);

    struct buf *bp = bread(ip->dev, indb);
    uint *a = (uint*)bp->data;

    if(a[0] != 0){
      cprintf("addr[%d] -> [0] (bn : %d) : %x\n",
              NDIRECT, NDIRECT, a[0]);
    }
    brelse(bp);
  }

  iunlockput(ip);
  end_op();
  return 0;
}

