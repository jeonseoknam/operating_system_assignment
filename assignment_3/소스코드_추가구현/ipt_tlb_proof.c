// ipt_tlb_proof.c  (TLB 정량 검증 포함 버전)
#include "types.h"
#include "stat.h"
#include "user.h"
#include "mmu.h"
#include "vlist.h"


// 필요시: user.h에 없더라도 이 파일에서 프로토타입 선언
extern int sw_tlb_reset(int mode);                 // 0: counters만 0, 1: flush+0
extern int sw_tlb_read(struct tlb_stats *out);     // 커널에서 통계 복사

static void print_tlb(const char *tag) {
  struct tlb_stats s;
  if (sw_tlb_read(&s) < 0) {
    printf(1, "[tlb %s] read failed\n", tag);
    return;
  }
  uint hitp = s.lookups ? (s.hits * 100) / s.lookups : 0;
  printf(1,
    "[tlb %s] hits=%d misses=%d hit%%=%d lookups=%d evicts=%d invalidations=%d lines=%d ways=%d sets=%d\n",
    tag, s.hits, s.misses, hitp, s.lookups, s.evicts, s.invalidations, s.lines, s.ways, s.sets);
}

static void pfind(uint pa_page){
  struct vlist buf[16];
  int n = phys2virt(pa_page, buf, 16);
  printf(1, "[pfind] pa=0x%x -> %d entries\n", pa_page, n);
  for(int i=0;i<n;i++)
    printf(1,"  [%d] pid=%d va=0x%x flags=0x%x\n", i, buf[i].pid, buf[i].va_page, buf[i].flags);
}

int
main(void)
{
  // 1) 한 페이지만 확보 (va0)
  char *base = sbrk(4096);
  if((int)base<0){ printf(1,"sbrk fail\n"); exit(); }
  uint va0 = ((uint)base) & ~0xFFF;

  // 2) va0의 pa_page
  uint pa0, fl0;
  if(vtop((void*)va0, &pa0, &fl0)!=0){ printf(1,"vtop fail\n"); exit(); }
  uint pa_page = pa0 & ~0xFFF;
  printf(1, "[base] va0=0x%x -> pa_page=0x%x flags=0x%x\n", va0, pa_page, fl0);

  // 3) 같은 PFN을 다른 VA로 alias 매핑
  uint va1 = va0 + 0x3000; // 확실히 비어 있을 거리
  if(vm_map_user_page(va1, pa_page, PTE_U|PTE_W) < 0){
    printf(1,"vm_map_user_page(alias) fail\n"); exit();
  }
  printf(1, "-- after alias(self) --\n");
  pfind(pa_page);

  // --------- A. 기본 TLB 동작(히트/미스) 수치 검증 ----------
  // flush + counters 0 → 첫 vtop은 miss, 두번째는 hit 기대
  sw_tlb_reset(1);
  uint pa, fl;
  vtop((void*)va1, &pa, &fl);  // miss
  vtop((void*)va1, &pa, &fl);  // hit
  print_tlb("baseline 2x vtop on same VA (expect hits=1, misses=1, lookups=2)");

  // --------- B. 권한 변경 시 invalidate 발생 검증 ----------
  // 엔트리는 유지(카운터만 0)한 뒤, protect_nowrite가 해당 엔트리를 무효화해야 함.
  sw_tlb_reset(0);                    // 카운터만 0 (엔트리는 그대로)
  if (protect_nowrite((void*)va1) < 0){
    printf(1,"protect_nowrite fail\n"); exit();
  }
  // invalidate로 인해 다음 vtop은 miss여야 함
  vtop((void*)va1, &pa, &fl);         // miss (재채움)
  print_tlb("after protect_nowrite + 1x vtop (expect misses=1, hits=0, lookups=1)");
  // 한 번 더 호출해서 재히트 확인
  vtop((void*)va1, &pa, &fl);         // hit
  print_tlb("after protect_nowrite + 2x vtop (expect hits=1, misses=1, lookups=2)");

  // --------- C. 프로세스 경계(자식)에서 동일 PFN 매핑 → IPT 체인 증가 ----------
  int pid = fork();
  if(pid < 0){ printf(1,"fork fail\n"); exit(); }
  if(pid == 0){
    uint sz = (uint)sbrk(0);
    uint va_child = ((sz + 0x3000) & ~0xFFF);
    if(vm_map_user_page(va_child, pa_page, PTE_U|PTE_W) < 0){
      printf(1,"child map fail\n"); exit();
    }
    sleep(50);
    exit();
  }
  sleep(10);
  printf(1, "-- with child --\n");
  pfind(pa_page);

  // --------- D. 자식 종료 후 IPT 정리 ----------
  wait();
  printf(1, "-- after child exit --\n");
  pfind(pa_page);

  printf(1,"[ipt_tlb_proof] done\n");
  exit();
}

