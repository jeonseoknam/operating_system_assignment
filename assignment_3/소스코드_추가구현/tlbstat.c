// tlbstat.c (fixed) — 소프트웨어 TLB 통계 1줄 요약 출력 유틸리티
#include "types.h"
#include "stat.h"
#include "user.h"

/*------------------------------------------------------------------------------
 * print_stats: 커널에서 전달된 struct tlb_stats를 사람이 읽기 쉬운 한 줄로 출력
 *  - hits/misses/lookups/evicts/invalidations: 동작 카운터
 *  - lines/ways/sets: TLB 구성 파라미터(총 라인수, 연관도, 세트수)
 *----------------------------------------------------------------------------*/
static void print_stats(const struct tlb_stats *s) {
  printf(1,
    "TLB: hits=%d misses=%d lookups=%d evicts=%d invalidations=%d lines=%d ways=%d sets=%d\n",
    s->hits, s->misses, s->lookups, s->evicts, s->invalidations,
    s->lines, s->ways, s->sets);
}

int
main(int argc, char *argv[])
{
  struct tlb_stats st;       // 커널이 채워줄 통계 구조체 버퍼

  // tlb_stats_sys: 시스템콜 래퍼(커널의 소프트 TLB 통계를 사용자 공간으로 복사)
  if (tlb_stats_sys(&st) < 0) {
    printf(1, "tlbstat: tlb_stats_sys failed\n");
    exit();
  }

  print_stats(&st);          // 수집한 통계를 한 줄로 출력
  exit();
}
