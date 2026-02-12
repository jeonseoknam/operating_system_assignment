#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "physmem.h"   // struct physframe_info 정의가 보이게 추가
#include "ipt.h"
#include "tlb.h"
#include "vlist.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

/*int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
} */

int sys_sbrk(void){
  int n; struct proc *p = myproc();
  if(argint(0,&n) < 0) return -1;
  int addr = p->sz;
  if(n > 0){
    if(growproc(n) < 0) return -1;  // 내부에서 allocuvm → mappages → ipt_add
  }else if(n < 0){
    // deallocuvm → vm_unmap_one → ipt_remove
    if(deallocuvm(p->pgdir, addr, addr+n) == 0) return -1;
    p->sz += n;
  }
  return addr;
}


int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// 전역 물리 페이지 관리 테이블 관련 시스템콜
int
sys_dump_physmem_info(void)
{
  char *uaddr;
  int maxe;

  // 1) 두 번째 인자부터 먼저 읽어서 크기 결정
  if (argint(1, &maxe) < 0) return -1;
  if (maxe <= 0) return -1;

  // 2) 첫 번째 인자(버퍼 포인터) 유효성 검사: 필요한 총 바이트 수로 체크
  if (argptr(0, &uaddr, maxe * sizeof(struct physframe_info)) < 0) return -1;

  // 3) 실제 작업 호출
  return dump_physmem_info_kernel(uaddr, maxe);
}

int sys_vtop(void)
{
  uint32_t va_arg;           // raw user VA (정수로 받기)
  uint32_t u_pa_out_arg;     // user buffer address to write PA
  uint32_t u_flags_out_arg;  // user buffer address to write flags

  // 0: VA 값
  if (argint(0, (int*)&va_arg) < 0) return -1;
  // 1: pa_out 주소
  if (argint(1, (int*)&u_pa_out_arg) < 0) return -1;
  // 2: flags_out 주소
  if (argint(2, (int*)&u_flags_out_arg) < 0) return -1;

  uint32_t pa = 0, flags = 0;
  struct proc *p = myproc();
  if (!p) return -1;

  int rc = sw_vtop(p->pgdir, (void*)va_arg, &pa, &flags);

  // 성공일 때만 결과를 user buffer로 복사
  if (rc == 0) {
    if (copyout(p->pgdir, (uint)u_pa_out_arg,    (char*)&pa,    sizeof(pa))    < 0) return -1;
    if (copyout(p->pgdir, (uint)u_flags_out_arg, (char*)&flags, sizeof(flags)) < 0) return -1;
  }

  // --- 선택: 하드웨어 참조와 비교 ---
  // vm.c에 hw_walk_probe 구현 & defs.h에 프로토타입 선언해둠
  // 안 쓸 거면 이 블록을 주석(#if 0) 처리
#ifdef VTOP_DEBUG
  extern int hw_walk_probe(pde_t *pgdir, const void *va, uint *pa_out, uint *fl_out);
  uint hw_pa=0, hw_flags=0;
  int hw_rc = hw_walk_probe(p->pgdir, (void*)va_arg, &hw_pa, &hw_flags);
  if (hw_rc==0 && (hw_pa != pa || hw_flags != flags)) {
    cprintf("[mismatch] va=0x%x sw:pa=0x%x fl=0x%x  hw:pa=0x%x fl=0x%x\n",
            va_arg, pa, flags, hw_pa, hw_flags);
  }
#endif

  return rc;
}


// (추가) SW_TLB를 위한 시스템콜 핸들러 구현
int sys_tlb_stats_sys(void){
  struct tlb_stats st;
  if (tlb_get_stats(&st) < 0) return -1;
  char *u;
  if (argptr(0, &u, sizeof(st)) < 0) return -1;
  return copyout(myproc()->pgdir, (uint)u, (char*)&st, sizeof(st)) < 0 ? -1 : 0;
}

int
sys_sw_tlb_reset(void)
{
  int mode = 0;           // 0: 카운터만 0, 1: 엔트리 flush + 카운터 0
  argint(0, &mode);
  if (mode == 1) tlb_reset_all();
  else           tlb_reset_counters();
  return 0;
}

int
sys_sw_tlb_read(void)
{
  char *uaddr;
  if (argptr(0, &uaddr, sizeof(struct tlb_stats)) < 0)
    return -1;

  struct tlb_stats s;     // ★ tlb.h의 것 (모든 멤버 uint32_t)
  tlb_get_stats(&s);      // 내부에서 채움

  if (copyout(myproc()->pgdir, (uint)uaddr, (char*)&s, sizeof(s)) < 0)
    return -1;

  return 0;
}


// phys2virt(pa_page, out[], max)
int sys_phys2virt(void) {
  uint pa_page; int max; uint out_uva;
  if (argint(0, (int*)&pa_page) < 0) return -1;
  if (argint(1, (int*)&out_uva) < 0) return -1;  // ← 순서를 0,1,2로 통일(가독성)
  if (argint(2, &max) < 0) return -1;
  if (max < 0) return -1;
  if (max > 1024) max = 1024;

  cprintf("[phys2virt] pa_page=0x%x out_uva=0x%x max=%d sizeof(vlist)=%d (kernel)\n",
          pa_page, out_uva, max, (int)sizeof(struct vlist));
  
  // 필요시 상한 설정: if (max > 1024) max = 1024;

  uint target_pfn = PFN(pa_page);
  int n = 0;
  struct vlist tmp;
  
  // 출력 버퍼 범위 검사 (오버플로우 방지 포함)
  uint need = (uint)max * (uint)sizeof(struct vlist);
  if (max > 0) {
    if (out_uva >= myproc()->sz) return -1;
    if (need > myproc()->sz)     return -1;
    if (out_uva + need < out_uva) return -1;          // wrap-around
    if (out_uva + need > myproc()->sz) return -1;     // 범위 초과
  }

  acquire(&ipt_lock);
  for (int i = 0; i < IPT_BUCKETS && n < max; i++) {
    for (struct ipt_entry *e = ipt_hash[i]; e && n < max; e = e->next) {
      if (e->pfn == target_pfn) {
        tmp.pid     = e->pid;
        tmp.va_page = e->va;
        tmp.flags   = e->flags;
	

        uint dst = out_uva + n * sizeof(struct vlist);
        cprintf("[phys2virt] copyout idx=%d dst=0x%x pid=%d va=0x%x flags=0x%x\n",
                n, dst, tmp.pid, tmp.va_page, tmp.flags);

        // out_uva + n*sizeof(struct vlist) 에 1개씩 안전하게 복사
        if (copyout(myproc()->pgdir,
                    out_uva + n * sizeof(struct vlist),
                    (char*)&tmp, sizeof(tmp)) < 0) {
          release(&ipt_lock);
          return -1;   // 가드 페이지 등으로 쓰기 실패 → 깔끔히 실패 반환
        }
        n++;
      }
    }
  }
  release(&ipt_lock);
  return n;
}



// vm_alias_by_pa(pa_page, dst_va, flags)
int sys_vm_alias_by_pa(void) {
  uint pa_page, dst_va, flags;
  if (argint(0, (int*)&pa_page) < 0) return -1;
  if (argint(1, (int*)&dst_va)  < 0) return -1;   // << 변경: argptr → argint
  if (argint(2, (int*)&flags)   < 0) return -1;

  uint va_page = PGROUNDDOWN(dst_va);
  uint pa_aln  = PTE_ADDR(pa_page);

  pde_t *pgdir = myproc()->pgdir;
  pde_t pde    = pgdir[PDX(va_page)];
  if (pde & PTE_P) {
    pte_t *pt  = (pte_t*)P2V(PTE_ADDR(pde));
    pte_t *pte = &pt[PTX(va_page)];
    if (*pte & PTE_P) return -1;   // 이미 매핑되어 있으면 금지
  }
  return vm_map_user_page(pgdir, va_page, pa_aln, flags);
}


int sys_vm_unmap_one(void) {
  uint uva; 
  int free_if_last;
  if (argint(0, (int*)&uva) < 0) return -1;          // 포인터 값을 '정수'로 받기
  if (argint(1, &free_if_last) < 0) return -1;

  return vm_unmap_one(myproc()->pgdir, PGROUNDDOWN(uva), free_if_last, 0);
  // 마지막 인자 0: '해당 VA가 없어도 -1로 에러 반환' 하지 않도록(필요 시 정책대로)
}


// sys_vm_map_user_page(va_page, pa_page, perm)
int sys_vm_map_user_page(void) {
  uint va_page, pa_page;
  int perm;
  if (argint(0, (int*)&va_page) < 0) return -1;
  if (argint(1, (int*)&pa_page) < 0) return -1;
  if (argint(2, &perm) < 0) return -1;

  // 페이지 정렬 요구
  if ((va_page & (PGSIZE-1)) || (pa_page & (PGSIZE-1))) return -1;

  struct proc *p = myproc();
  return vm_map_user_page(p->pgdir, va_page, pa_page, perm);
}

int sys_protect_nowrite(void){
  int uva;
  if (argint(0, &uva) < 0) return -1;
  return protect_nowrite(myproc()->pgdir, (void*)uva);
}

