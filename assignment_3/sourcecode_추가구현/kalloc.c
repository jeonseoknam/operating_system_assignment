// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

// ---- 전역 프레임 테이블(과제용) ----
#include "proc.h"     // myproc()
#include "date.h"
#include "physmem.h"  // PFNNUM, struct physframe_info

//#define KMEM_SANITY_DEBUG 1   // 필요 시 활성화: 프리리스트 무결성 검사 panic
//#define KMEM_LOG 1            // 필요 시 활성화: 디버그 로그

struct physframe_info pf_info[PFNNUM];

extern uint ticks;
extern struct spinlock tickslock;

static void pf_init(void); // proto

void freerange(void *vstart, void *vend);
extern char end[]; // from kernel.ld

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

// ----------------- 내부 헬퍼: 프리리스트 무결성 검사 -----------------
#ifdef KMEM_SANITY_DEBUG
static int freelist_sanity(void) {
  struct run *r = kmem.freelist;
  int steps = 0, max_steps = 200000;
  while (r) {
    if (((uint)r) % PGSIZE) return -1;                 // 정렬 깨짐
	if ((char*)r < end || (char*)r >= (char*)P2V(PHYSTOP)) return -2;
    r = r->next;
    if (++steps > max_steps) return -3;                // 순환 의심
  }
  return 0;
}
#endif

// ----------------- 초기화 -----------------
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;

  pf_init();   // 전역 프레임 테이블 초기화

  freerange(vstart, vend);
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}

static void
pf_init(void)
{
  for (int i = 0; i < PFNNUM; i++) {
    pf_info[i].frame_index = i;
    pf_info[i].allocated   = 0;
    pf_info[i].pid         = -1;
    pf_info[i].start_tick  = 0;
  }
}

// ----------------- 초기 프리리스트 구성 -----------------
void
freerange(void *vstart, void *vend)
{
  char *p = (char*)PGROUNDUP((uint)vstart);
  for (; p + PGSIZE <= (char*)vend; p += PGSIZE)
    kfree(p);
}

// ----------------- 해제(kfree) -----------------
// v: 커널 가상주소(KVA)이며 페이지 정렬되어야 함.
void
kfree(char *v)
{

// 수정:
if ((uint)v % PGSIZE || v < end || v >= (char*)P2V(PHYSTOP)) {
  cprintf("kfree: bad addr v=0x%x pa=0x%x\n", (uint)v, V2P((uint)v));
  panic("kfree: bad addr");
}

  uint pa  = V2P((uint)v);
  uint idx = pa >> 12;

  if (kmem.use_lock) acquire(&kmem.lock);

  // kmem.use_lock==1 (kinit2 이후)부터는 double free 가드 가능
  if (kmem.use_lock && idx < PFNNUM) {
    if (pf_info[idx].allocated == 0) {
      // 이미 free 상태인데 또 free 시도
      if (kmem.use_lock) release(&kmem.lock);
	  cprintf("kfree: double free pa=0x%x\n", pa);
	  panic("kfree: double free");
    }
  }

  // 1) 먼저 페이지를 채워 dangling ref 잡기 (헤더를 나중에 쓴다!)
  memset(v, 1, PGSIZE);

  // 2) 프레임 테이블 업데이트(Free 표시)
  if (idx < PFNNUM) {
    pf_info[idx].allocated   = 0;
    pf_info[idx].pid         = -1;
    pf_info[idx].start_tick  = 0;
    pf_info[idx].frame_index = idx;
  }

  // 3) 프리리스트에 연결
  struct run *r = (struct run*)v;
  r->next = kmem.freelist;
  kmem.freelist = r;

#ifdef KMEM_SANITY_DEBUG
  int s = freelist_sanity();
  if (s != 0) {
    if (kmem.use_lock) release(&kmem.lock);
    panic("kfree: freelist corrupt (%d)", s);
  }
#endif

  if (kmem.use_lock) release(&kmem.lock);
}

// ----------------- 할당(kalloc) -----------------
char*
kalloc(void)
{
  struct run *r;

  if (kmem.use_lock) acquire(&kmem.lock);

#ifdef KMEM_SANITY_DEBUG
  {
    int s = freelist_sanity();
    if (s != 0) {
      if (kmem.use_lock) release(&kmem.lock);
      panic("kalloc: freelist corrupt (%d)", s);
    }
  }
#endif

  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;

  // 과제용 메타데이터 업데이트는 락 내부에서 일관되게
  if (r) {
    uint pa  = V2P((uint)r);
    uint idx = pa >> 12;

    if (idx < PFNNUM) {
      if (kmem.use_lock) {
        uint t = ticks;        // 스냅샷 정도면 락 불필요
        int owner = -1;
        struct proc *p = myproc();  // 락 안에서 호출해도 xv6에서는 안전
        if (p) owner = p->pid;

        pf_info[idx].allocated   = 1;
        pf_info[idx].pid         = owner;
        pf_info[idx].start_tick  = t;
        pf_info[idx].frame_index = idx;
      } else {
        // 부팅 초기(kinit1 구간)에는 최소 표기만
        pf_info[idx].allocated   = 1;
        pf_info[idx].pid         = -1;
        pf_info[idx].start_tick  = 0;
        pf_info[idx].frame_index = idx;
      }
    }
  }

  if (kmem.use_lock) release(&kmem.lock);

#ifdef KMEM_LOG
  if (!r) cprintf("[kalloc] out of memory\n");
#endif
  return (char*)r;
}

// ----------------- 과제용 dump -----------------
int
dump_physmem_info_kernel(char *uaddr, int max_entries)
{
  struct proc *cur = myproc();
  if (!cur || !uaddr || max_entries <= 0) return -1;

  int limit = PFNNUM;
  if (max_entries < limit) limit = max_entries;

  int copied = 0;
  for (int i = 0; i < limit; i++) {
    if (copyout(cur->pgdir,
                (uint)uaddr + i * sizeof(struct physframe_info),
                (void*)&pf_info[i],
                sizeof(struct physframe_info)) < 0)
      break;
    copied++;
  }
  return copied;
}

