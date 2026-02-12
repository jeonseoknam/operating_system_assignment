// tlb.h — 소프트웨어 TLB(집합-연관) 헤더: 엔트리 정의/전역 상태/인터페이스 선언
#pragma once
#include "types.h"
#include "spinlock.h"

// 구성 파라미터: 총 라인수 = lines, 집합 연관도 = ways, 세트수 = lines/ways
// tlb.c에서 SOFT_TLB_SETS가 2의 거듭제곱인지 컴파일 타임 검사함.
#define SOFT_TLB_WAYS   2        // 세트당 way 수(연관도)
#define SOFT_TLB_LINES  64       // 총 캐시 라인 수(= 엔트리 수)
#define SOFT_TLB_SETS   (SOFT_TLB_LINES / SOFT_TLB_WAYS)  // 세트 개수

// 단일 TLB 엔트리: (pid, va_page) → (pa_page, flags)
// - va_page/pa_page: 페이지 정렬된 VA/PA(페이지 단위 키)
// - flags: PTE 권한 비트(P/W/U 등) 스냅샷
// - valid: 유효 플래그(0/1)
// - age  : LRU 근사용 카운터(접근/갱신 시 증가)
struct tlb_entry {
  uint32_t pid;
  uint32_t va_page;
  uint32_t pa_page;
  uint16_t flags;
  uint8_t  valid;
  uint32_t age;
};

// 전역 소프트 TLB: 락, 세트-웨이 엔트리 배열, 통계 카운터
struct soft_tlb {
  struct spinlock lock;                                // 동시성 보호용 락
  struct tlb_entry sets[SOFT_TLB_SETS][SOFT_TLB_WAYS]; // [세트][웨이] 엔트리
  uint32_t hits, misses, evicts, invalidations, lookups; // 통계
};

// 전역 인스턴스(정의는 tlb.c에 있음)
extern struct soft_tlb g_tlb;

// 초기화/조회/삽입
void tlb_init(void);  // 엔트리 invalid, 카운터 0으로 초기화
int  tlb_lookup(uint32_t pid, uint32_t va_page, uint32_t *pa_page_out, uint16_t *flags_out); // 히트 시 1, 미스 0
void tlb_insert(uint32_t pid, uint32_t va_page, uint32_t pa_page, uint16_t flags);          // 동일 키면 갱신, 없으면 교체

// 무효화(Invalidate)
void tlb_invalidate_one(uint32_t pid, uint32_t va_page); // 특정 (pid,va) 한 건
void tlb_invalidate_pid(uint32_t pid);                   // 특정 PID 전범위
void tlb_invalidate_all(void);                           // 전 엔트리

// 카운터/전체 리셋
void tlb_reset_counters(void); // 통계 카운터만 0으로(엔트리는 유지)
void tlb_reset_all(void);      // 엔트리까지 전부 무효화 + 카운터 0

// 일관성 보조(Consistency): vm 변경에 연동해 호출
void tlb_invalidate_va(uint32_t pid, uint32_t va_page);                 // 단일 VA 페이지 무효화
void tlb_invalidate_range(uint32_t pid, uint32_t va_start, uint32_t va_end); // [va_start, va_end) 범위(페이지 단위)

// 통계/디버깅
struct tlb_stats {
  uint32_t hits, misses, evicts, invalidations, lookups; // 동작 통계
  uint32_t lines, ways, sets;                             // 구성 파라미터 스냅샷
};
int  tlb_get_stats(struct tlb_stats *out);                                      // 통계 복사
int  tlb_dump_entries(struct tlb_entry *buf, int max, int only_valid);          // 엔트리 덤프(유효만 선택 가능)
