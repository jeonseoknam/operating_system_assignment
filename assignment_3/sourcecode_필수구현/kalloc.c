// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

//---- 전역 프레임 테이블 & 유틸 ----
#include "proc.h"     // myproc()
#include "date.h"
#include "physmem.h"  // struct physframe_info, PFNNUM

// 전역 프레임 테이블(고정 크기 .bss)
struct physframe_info pf_info[PFNNUM];

extern uint ticks;
extern struct spinlock tickslock;

static void pf_init(void); // 전역 프레임 테이블 초기화

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;            // kinit1 단계(0) / kinit2 이후(1)
  struct run *freelist;
} kmem;

//------------------------------------------------------------------------------
// 부팅 단계별 초기화
//------------------------------------------------------------------------------

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
//    the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages after
//    installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;

  // 전역 프레임 테이블 초기화 (부트 1단계)
  pf_init();

  // entrypgdir 로 맵된 영역을 자유 리스트에 연결
  freerange(vstart, vend);
}

void
kinit2(void *vstart, void *vend)
{
  // 나머지 물리 페이지를 자유 리스트에 연결
  freerange(vstart, vend);

  // 이후부터는 락 사용 (SMP/스케줄러 활성화 단계)
  kmem.use_lock = 1;
}

//------------------------------------------------------------------------------
// 전역 프레임 테이블 초기화
//------------------------------------------------------------------------------
static void
pf_init(void)
{
  for (int i = 0; i < PFNNUM; i++) {
    pf_info[i].frame_index = i;
    pf_info[i].allocated   = 0;
    pf_info[i].pid         = -1;   // unknown/free
    pf_info[i].start_tick  = 0;
  }
}

//------------------------------------------------------------------------------
// free list 구성
//------------------------------------------------------------------------------
void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for (; p + PGSIZE <= (char*)vend; p += PGSIZE)
    kfree(p);
}

//------------------------------------------------------------------------------
// kfree: 페이지를 해제하여 free list에 되돌림
// (부팅 1단계에서는 락 없이, 이후에는 kmem.lock 하에서 동작)
//------------------------------------------------------------------------------
void
kfree(char *v)
{
  struct run *r;

  if ((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(v, 1, PGSIZE);

  if (kmem.use_lock)
    acquire(&kmem.lock);

  // ---- update frame table (mark free) ----
  uint pa  = V2P((uint)v);
  uint idx = pa >> 12; // PFN
  if (idx < PFNNUM) {
    pf_info[idx].allocated   = 0;
    pf_info[idx].pid         = -1;
    pf_info[idx].start_tick  = 0;
    pf_info[idx].frame_index = idx;
  }
  // ----------------------------------------

  // 자유 리스트 연결
  r = (struct run*)v;
  r->next = kmem.freelist;
  kmem.freelist = r;

  if (kmem.use_lock)
    release(&kmem.lock);
}

//------------------------------------------------------------------------------
// kalloc: 4096-byte 페이지 하나를 할당하여 반환
// (부팅 1단계에서는 최소 메타데이터만 기록, 2단계 이후 pid/ticks 기록)
//------------------------------------------------------------------------------
char*
kalloc(void)
{
  struct run *r;

  if (kmem.use_lock) acquire(&kmem.lock);

  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;

  if (r) {
    uint pa  = V2P((uint)r);
    uint idx = pa >> 12;
    if (idx < PFNNUM) {
      if (kmem.use_lock) {
        // 부팅 2단계 이후: pid / tick까지 기록
        uint t = ticks; // 근사치 스냅샷이면 락 불필요
        int owner = -1;
        struct proc *p = myproc();
        if (p) owner = p->pid;

        pf_info[idx].allocated   = 1;
        pf_info[idx].pid         = owner;
        pf_info[idx].start_tick  = t;
        pf_info[idx].frame_index = idx;
      } else {
        // 부팅 1단계: 최소 정보만
        pf_info[idx].allocated   = 1;
        pf_info[idx].pid         = -1;
        pf_info[idx].start_tick  = 0;
        pf_info[idx].frame_index = idx;
      }
    }
  }

  if (kmem.use_lock) release(&kmem.lock);
  return (char*)r;
}

//------------------------------------------------------------------------------
// dump_physmem_info_kernel:
//  전역 프레임 테이블의 스냅샷을 유저 버퍼(uaddr)로 copyout.
//  * 거대한 정적 버퍼를 사용하지 않고, 작은 청크로 안전하게 복사.
//------------------------------------------------------------------------------
int
dump_physmem_info_kernel(char *uaddr, int max_entries)
{
  struct proc *cur = myproc();
  if (!cur || !uaddr || max_entries <= 0)
    return -1;

  int limit = PFNNUM;
  if (max_entries < limit) limit = max_entries;

  // 청크 단위로 스냅샷 → copyout
  enum { CHUNK = 128 }; // 필요 시 64~256 사이로 조정
  struct physframe_info tmp[CHUNK];

  int copied = 0;
  for (int base = 0; base < limit; base += CHUNK) {
    int n = (limit - base < CHUNK) ? (limit - base) : CHUNK;

    if (kmem.use_lock) acquire(&kmem.lock);
    for (int i = 0; i < n; i++)
      tmp[i] = pf_info[base + i];
    if (kmem.use_lock) release(&kmem.lock);

    if (copyout(cur->pgdir,
                (uint)uaddr + base * sizeof(struct physframe_info),
                (void*)tmp,
                n * sizeof(struct physframe_info)) < 0)
      return (copied ? copied : -1);

    copied += n;
  }

  return copied;
}

