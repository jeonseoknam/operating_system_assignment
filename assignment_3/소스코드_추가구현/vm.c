#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

// (2) 역페이지 테이블용 헤더 추가
#include "ipt.h"

// (3) SW TLB
#include "tlb.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a    = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);

  for (;;) {
    if ((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if (*pte & PTE_P)
      panic("remap");

    // 실제 PTE 기록
    uint32_t pa_page = PTE_ADDR(pa);                 //  page-aligned PA
    *pte = pa_page | perm | PTE_P;

    // ---- IPT 삽입: 유저 영역만 추적 ----
	if ((uint)a < KERNBASE) {
	  int pid = (myproc() && myproc()->pgdir == pgdir)
              ? myproc()->pid
              : ipt_pid_from_pgdir(pgdir);      //  없으면 -1
    if (pid > 0) {                                //  pid > 0 일 때만 삽입
    	uint32_t va_page   = (uint32_t)PGROUNDDOWN((uint)a);
    	uint32_t pte_flags = (*pte) & (PTE_P | PTE_W | PTE_U);
    	ipt_add(pgdir, (uint32_t)pid, va_page, pa_page, pte_flags);
  	}
  }

    // 새 매핑은 remap이 아니므로 SW-TLB invalidation 불필요

    if (a == last)
      break;
    a  += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.

int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  uint a;

  if (newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for (; a < oldsz; a += PGSIZE) {
    int rem;
    // free_if_last=1: 해당 PFN의 마지막 참조일 때만 실제 kfree
    // remaining_out(=rem)은 여기서는 굳이 쓰지 않아도 됩니다.
    (void)vm_unmap_one(pgdir, a, /*free_if_last=*/1, &rem);
  }
  return newsz;
}


// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte = walkpgdir(pgdir, uva, 0);
  if (pte == 0)
    panic("clearpteu");

  // clearpteu()
  uint32_t va_page = (uint32_t)PGROUNDDOWN((uint)uva);
  uint32_t pa_page = PTE_ADDR(*pte);
  int pid = (myproc() && myproc()->pgdir == pgdir) ? myproc()->pid : ipt_pid_from_pgdir(pgdir);
  if (pid > 0 && (pgdir != kpgdir) && (*pte & PTE_U)) {
    ipt_remove(pgdir, (uint32_t)pid, va_page, pa_page);
  }

  // 실제 권한 변경
  *pte &= ~PTE_U;

  // --- SW-TLB 정합성: 현재 활성 주소공간이면 해당 엔트리 무효화 ---
  if (myproc() && myproc()->pgdir == pgdir) {
    tlb_invalidate_one(myproc()->pid, va_page);
  }
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)P2V(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0) {
      kfree(mem);
      goto bad;
    }
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if (pte == 0)                 //  추가: PDE or PTE page 자체가 없음
    return 0;
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}


// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

// 성공 0
// 실패 음수: -1(PDE 없음), -2(PTE 없음), -3(인자 오류)
int
sw_vtop(pde_t *pgdir, const void *va, uint32_t *pa_out, uint32_t *pte_flags_out)
{
  // va==0 은 xv6에서 "정상"일 수 있으므로 에러로 보면 안 됨!
  if (!pgdir || !pa_out || !pte_flags_out)
    return -3;

  uint32_t a = (uint32_t)va;
  uint32_t va_page = PGROUNDDOWN(a);

  // 현재 PID (TLB 태깅용), 커널 초기부팅 구간 안전하게 0 처리
  struct proc *mp = myproc();
  uint32_t pid = (mp ? (uint32_t)mp->pid : 0);

  // --- (A) 소프트 TLB 히트 시 바로 반환 ---
  uint32_t pa_page;
  uint16_t tlb_flags;
  if (tlb_lookup(pid, va_page, &pa_page, &tlb_flags)) {
    *pa_out        = pa_page | (a & 0xFFF);
    *pte_flags_out = (uint32_t)tlb_flags;   // tlb에는 하위 12비트 전체를 넣어둘 것을 권장
    return 0;
  }

  // --- (B) PDE 검사 ---
  uint32_t pdx = PDX(a);
  pde_t pde = pgdir[pdx];
  if ((pde & PTE_P) == 0)
    return -1; // PDE 없음

  // --- (C) PTE 접근: PDE의 물리주소 → 커널 가상주소(P2V)로 변환 필수! ---
  pte_t *pgtab = (pte_t*)P2V(PTE_ADDR(pde));

  uint32_t ptx = PTX(a);
  pte_t pte = pgtab[ptx];
  if ((pte & PTE_P) == 0)
    return -2; // PTE 없음

  // --- (D) 최종 물리주소 조합 ---
  uint32_t pa = PTE_ADDR(pte) | (a & 0xFFF);

  // --- (E) 플래그: 하위 12비트 전체를 반환(A/D 포함) → hw와 일치 ---
  uint32_t flags = (pte & 0xFFF);

  // --- (F) TLB 적재 ---
  tlb_insert(pid, va_page, PTE_ADDR(pte), (uint16_t)flags);

  *pa_out = pa;
  *pte_flags_out = flags;
  return 0;
}



// 디버그용 walkpgdir 래퍼
int hw_walk_probe(pde_t *pgdir, const void *va, uint *pa_out, uint *fl_out) {
  uint v = (uint)va;
  pde_t pde = pgdir[PDX(v)];
  if ((pde & PTE_P) == 0) return 1; // PDE 없음

  pte_t *pt = (pte_t*)P2V(PTE_ADDR(pde));
  pte_t pte = pt[PTX(v)];
  if ((pte & PTE_P) == 0) return 2; // PTE 없음

  *pa_out = (PTE_ADDR(pte) | (v & 0xFFF));
  *fl_out = (pte & 0xFFF);
  return 0;
}

int vm_map_user_page(pde_t *pgdir, uint va_page, uint pa_page, int perm) {
  // 1) 페이지 정렬 검증
  if ((va_page & (PGSIZE-1)) || (pa_page & (PGSIZE-1)))
    return -1;

  // 2) 사용자 영역만 허용 (필요 시 정책에 맞게 조정)
  if (va_page >= KERNBASE) return -1;
  if ((perm & PTE_U) == 0) return -1;

  // 3) 한 페이지만 매핑 (중복 매핑이면 mappages가 panic("remap"))
  return mappages(pgdir, (void*)va_page, PGSIZE, pa_page, perm);
}

// vm.c
/*int
protect_nowrite(pde_t *pgdir, void *uva)
{
  pte_t *pte = walkpgdir(pgdir, uva, 0);
  if (!pte || (*pte & PTE_P) == 0)
    return -1;

  uint32_t va_page = PGROUNDDOWN((uint)uva);

  // 현재/비현재 pgdir 모두에서 pid를 계산해 두되,
  // 실제 TLB invalidate는 '현재 주소공간'일 때만 수행.
  int pid = (myproc() && myproc()->pgdir == pgdir)
              ? myproc()->pid
              : ipt_pid_from_pgdir(pgdir);

  // 쓰기 권한 제거
  *pte &= ~PTE_W;

  // 현재 활성 주소공간인 경우에만 SW-TLB invalidate
  if (myproc() && myproc()->pgdir == pgdir) {
    tlb_invalidate_one(pid, va_page);   // ← pid를 실제로 사용
  }
  return 0;
} */

int
protect_nowrite(pde_t *pgdir, void *uva)
{
  if (!pgdir || !uva) return -1;

  pte_t *pte = walkpgdir(pgdir, uva, 0);
  if (!pte || (*pte & PTE_P) == 0)
    return -1;

  uint va_page   = PGROUNDDOWN((uint)uva);
  uint pa_page   = PTE_ADDR(*pte);
  uint old_flags = (*pte) & 0xFFF;
  uint new_flags = old_flags & ~PTE_W;    // W 비트만 제거

  // 이미 W가 꺼져있으면 그대로 종료
  if (new_flags == old_flags)
    return 0;

  // PTE를 새 플래그로 갱신 (주소 비트 보존)
  *pte = pa_page | new_flags;

  // 현재/비현재 모두에서 pid 산출 (IPT 갱신용)
  int pid = (myproc() && myproc()->pgdir == pgdir)
              ? myproc()->pid
              : ipt_pid_from_pgdir(pgdir);

  // ---- IPT 플래그 동기화 (유저 매핑만) ----
  if ((old_flags & PTE_U) && pid > 0 && pgdir != kpgdir) {
    // 1) 기존 (pid, va) 엔트리 제거
    (void)ipt_remove(pgdir, (uint32_t)pid, va_page, pa_page);
    // 2) 새 플래그로 재삽입
    ipt_add(pgdir, (uint32_t)pid, va_page, pa_page, new_flags);
  }

  // ---- SW-TLB invalidate: 현재 활성 주소공간이면 ----
  if (myproc() && myproc()->pgdir == pgdir) {
    tlb_invalidate_one(pid, va_page);
  }

  return 0;
}

// vm.c
// vm.c
int
vm_unmap_one(pde_t *pgdir, uint va_any, int free_if_last, int *remaining_out)
{
  uint a = PGROUNDDOWN(va_any);

  pte_t *pte = walkpgdir(pgdir, (void*)a, 0);
  if (!pte)            return -1;   // PDE 없음
  if (!(*pte & PTE_P)) return -2;   // PTE 없음

  uint pa_page = PTE_ADDR(*pte);
  uint32_t pfn     = PFN(pa_page);      //  추가
  int  remaining = -1;              // 모르면 kfree 안 함

  // 유저 매핑이면 IPT에서 먼저 제거
  if (a < KERNBASE && (*pte & PTE_U)) {
    int pid = (myproc() && myproc()->pgdir == pgdir)
                ? myproc()->pid
                : ipt_pid_from_pgdir(pgdir);

    if (pid > 0 && pgdir != kpgdir) {
      int r = ipt_remove(pgdir, (uint32_t)pid, a, pa_page);
      if (r >= 0) {
        remaining = r;
      } else {
        remaining = ipt_remove_fallback((uint32_t)pid, a, pfn);
      }
    }
  }

  // 실제 언맵
  *pte = 0;

  // 현재 주소공간이면 TLB invalidate
  if (myproc() && myproc()->pgdir == pgdir)
    tlb_invalidate_one(myproc()->pid, a);

  // 마지막 참조면 물리 프레임 해제
  if (free_if_last && remaining == 0)
    kfree((char*)P2V(pa_page));

  if (remaining_out) *remaining_out = remaining;
  return 0;
}

