// ptlb_memtest.c 
#include "types.h"
#include "stat.h"
#include "user.h"
#include "mmu.h"
#include "vlist.h"

// ---- 선택: TLB 통계 찍고 싶을 때 사용 ----
static void tlb_print(const char *tag) {
  struct tlb_stats s;
  if (sw_tlb_read(&s) == 0) {
    int hitpct = s.lookups ? (int)(s.hits * 100 / s.lookups) : 0;
    printf(1, "[%s] hits=%d misses=%d hit%%=%d lookups=%d evicts=%d invalidations=%d lines=%d ways=%d sets=%d\n",
           tag, s.hits, s.misses, hitpct, s.lookups, s.evicts, s.invalidations, s.lines, s.ways, s.sets);
  }
}

// ---- 유틸 ----
static void print_vtop(const char *tag, uint va) {
  uint pa, fl;
  int rc = vtop((void*)va, &pa, &fl);
  if (rc == 0) {
    printf(1, "%s va=0x%x -> pa=0x%x flags=0x%x (P=%d W=%d U=%d)\n",
           tag, va, pa, fl, !!(fl&PTE_P), !!(fl&PTE_W), !!(fl&PTE_U));
  } else {
    printf(1, "%s va=0x%x -> rc=%d\n", tag, va, rc);
  }
}

static void pfind(uint pa_page) {
  struct vlist buf[32];
  int n = phys2virt(pa_page, buf, 32);
  printf(1, "[pfind] pa_page=0x%x -> %d entries\n", pa_page, n);
  for (int i = 0; i < n; i++)
    printf(1, "  [%d] pid=%d va=0x%x flags=0x%x\n",
           i, buf[i].pid, buf[i].va_page, buf[i].flags);
}

// 이미 매핑돼 있으면 "언맵만" 해서 깔끔한 상태로 만든다.
// free_if_last=0 으로 호출하여 실제 물리프레임은 절대 해제하지 않음.
static void ensure_unmapped(uint va){
  uint pa, fl;
  int rc = vtop((void*)va, &pa, &fl);
  if (rc == 0) {
    if (vm_unmap_one((void*)va, 0) < 0) {   // ★ 변경: unmap1() → vm_unmap_one(...,0)
      printf(1, "ensure_unmapped: vm_unmap_one(0x%x) fail\n", va);
      exit();
    }
  }
}

// ---- 본 테스트 ----
int
main(void)
{
  printf(1, "=== ptlb_memtest: start ===\n");

  sw_tlb_reset(1); // 1: 엔트리 flush+카운터0, 0: 카운터만 0

  // 1) 힙에서 정확히 1 페이지 확보 (top 페이지)
  char *p = sbrk(4096);
  if ((int)p < 0) { printf(1,"sbrk fail\n"); exit(); }
  uint va0 = ((uint)p) & ~0xFFF;

  // sw_vtop 기본 동작 & TLB hit(동일 VA 2회) + 오프셋 보존
  print_vtop("[vtop#1]", va0);
  print_vtop("[vtop#2]", va0);  // 두 번째 호출은 SW-TLB hit 기대
  tlb_print("baseline (after two vtop on va0)"); // 원하면 주석 해제

  uint pa0, fl0;
  if (vtop((void*)va0, &pa0, &fl0) != 0) { printf(1,"vtop base fail\n"); exit(); }
  uint pa_page = pa0 & ~0xFFF;

  // 오프셋 보존: va0+0x123 → (pa & 0xFFF)==0x123 기대
  uint pa_chk, fl_chk;
  if (vtop((void*)(va0+0x123), &pa_chk, &fl_chk) == 0) {
    printf(1, "[offset] va=0x%x -> pa=0x%x (pa&0xFFF=0x%x, expect 0x123)\n",
           va0+0x123, pa_chk, pa_chk & 0xFFF);
  }

  // 2) 같은 프로세스 내 alias 생성 → IPT 중복 체인 + TLB 검증
  uint va_alias = va0 + 0x2000;      // 비어있도록 떨어진 주소 사용
  ensure_unmapped(va_alias);
  if (vm_map_user_page(va_alias, pa_page, PTE_U|PTE_W) < 0) {
    printf(1,"alias map fail\n"); exit();
  }
  printf(1, "-- after alias(self) --\n");
  pfind(pa_page);                     // (pid,va0)와 (pid,va_alias) 두 엔트리 기대
  print_vtop("[alias vtop#1]", va_alias);
  print_vtop("[alias vtop#2]", va_alias); // 두 번째 호출은 TLB hit 기대
  // tlb_print("after alias two vtop on alias");

  // 권한 변경: W 제거(RO). IPT 갱신 + SW-TLB invalidation 결과 확인
  if (protect_nowrite((void*)va_alias) < 0) {
    printf(1,"protect_nowrite fail\n"); exit();
  }
  if (vtop((void*)va_alias, &pa_chk, &fl_chk) == 0) {
    printf(1,"[after protect] va_alias flags=0x%x (expect W cleared)\n", fl_chk);
  }
  // tlb_print("after protect_nowrite + one vtop(alias)");

  // 3) 자식들 여러 명이 같은 PFN을 각자 매핑 → 체인 개수 증가 확인
  int kids = 3;
  for (int i=0; i<kids; i++) {
    int pid = fork();
    if (pid < 0) { printf(1,"fork fail\n"); exit(); }
    if (pid == 0) {
      // child: 자신의 힙 위쪽 빈 페이지에 같은 PFN 매핑
      uint brk = (uint)sbrk(0);
      uint va_c = ( (brk + 0x4000 + (i<<12)) & ~0xFFF );
      ensure_unmapped(va_c);
      if (vm_map_user_page(va_c, pa_page, PTE_U|PTE_W) < 0) {
        printf(1,"child map fail\n"); exit();
      }
      // 부모가 관측할 시간 제공
      sleep(50);
      // 해제 경로를 다양화: 일부는 언맵 후 종료(물리해제는 하지 않음)
      if ((i & 1) == 0) vm_unmap_one((void*)va_c, 0);   
      exit();
    }
  }

  // 부모: 아이들이 매핑을 붙인 뒤 관측
  sleep(20);
  printf(1, "-- with children --\n");
  pfind(pa_page);

  // 4) alias 해제 → 마지막 참조 아니면 프레임 유지(=double free 방지)
  {
    int rc = vm_unmap_one((void*)va_alias, 1);          
    if (rc < 0) { printf(1,"vm_unmap_one(alias) fail rc=%d\n", rc); exit(); }
  }
  print_vtop("[after unmap alias]", va_alias);  // 음수(rc) 기대
  printf(1, "-- after unmap(alias) --\n");
  pfind(pa_page);   // parent는 보통 va0만 남고, child 매핑들이 남아 있을 수 있음
  // tlb_print("after unmap alias + one vtop(alias)");

  // 5) 자식 종료 대기 → exit() 후 IPT 정리 확인
  for (int i=0; i<kids; i++) wait();
  printf(1, "-- after children exit --\n");
  pfind(pa_page);

  // 6) 부모의 마지막 참조 해제: top 페이지 반환 → 남은 참조가 0이면 실제 프레임 해제
  if (sbrk(-4096) == (char*)-1) { printf(1,"sbrk(-4096) fail\n"); exit(); }
  print_vtop("[after free va0]", va0);      // 음수(rc) 기대
  printf(1, "-- final pfind --\n");
  pfind(pa_page); // 0 entries 기대(모든 참조 해제)

  printf(1, "=== ptlb_memtest: done ===\n");
  exit();
}

