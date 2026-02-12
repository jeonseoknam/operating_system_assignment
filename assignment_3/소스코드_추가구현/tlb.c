// tlb.c — 소프트웨어 TLB(집합-연관 캐시) 구현부: 조회/삽입/무효화/통계 제공
#include "types.h"      // <stdint.h> 대신 types.h 사용
#include "defs.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "tlb.h"

// 컴파일 타임 검사: 세트 수는 반드시 2의 거듭제곱(마스킹 인덱싱을 위해)
#if (SOFT_TLB_SETS & (SOFT_TLB_SETS - 1)) != 0
#error "SOFT_TLB_SETS must be power of two"
#endif

// 세트 인덱스 해시 함수(간단 혼합 + Knuth 상수 사용). 결과는 [0, SOFT_TLB_SETS)로 마스킹.
static inline uint32_t tlb_index(uint32_t pid, uint32_t va_page) {
  // Knuth multiplicative hash
  uint32_t h = pid ^ (va_page >> 12) ^ (va_page * U32_C(2654435761));
  return h & (uint32_t)(SOFT_TLB_SETS - 1U);
}

// 전역 TLB 인스턴스(락/엔트리/카운터 포함)
struct soft_tlb g_tlb;

// 초기화: 엔트리 invalid 클리어 + 통계 카운터 0 설정
void tlb_init(void){
  initlock(&g_tlb.lock, "softtlb");
  acquire(&g_tlb.lock);
  for (int s = 0; s < SOFT_TLB_SETS; s++)
    for (int w = 0; w < SOFT_TLB_WAYS; w++)
      g_tlb.sets[s][w].valid = (uint8_t)0;
  g_tlb.hits = g_tlb.misses = g_tlb.evicts = g_tlb.invalidations = g_tlb.lookups = U32_C(0);
  release(&g_tlb.lock);
}

// 조회: (pid, va_page)로 탐색. 히트 시 pa/flags 반환, age 증가. 미스 시 0 반환.
int tlb_lookup(uint32_t pid, uint32_t va_page, uint32_t *pa_out, uint16_t *fl_out){
  acquire(&g_tlb.lock);
  g_tlb.lookups++;
  uint32_t s = tlb_index(pid, va_page);
  for (int w = 0; w < SOFT_TLB_WAYS; w++) {
    struct tlb_entry *e = &g_tlb.sets[s][w];
    if (e->valid && e->pid == pid && e->va_page == va_page) {
      g_tlb.hits++;
      e->age++;                       // 간단 LRU 근사: 접근 시 age 가산
      if (pa_out) *pa_out = e->pa_page;
      if (fl_out) *fl_out = e->flags;
      release(&g_tlb.lock);
      return 1;
    }
  }
  g_tlb.misses++;
  release(&g_tlb.lock);
  return 0;
}

// 교체 대상 선택(락 없이 내부에서만 호출). 빈 슬롯 우선, 없으면 최소 age 선택(LRU 근사).
static int victim_way_nolock(uint32_t s){
#if SOFT_TLB_WAYS == 1
  return 0;
#else
  int v = 0;
  uint32_t minage = g_tlb.sets[s][0].age;
  for (int w = 0; w < SOFT_TLB_WAYS; w++) {
    if (!g_tlb.sets[s][w].valid) return w;
    if (g_tlb.sets[s][w].age < minage) {
      minage = g_tlb.sets[s][w].age;
      v = w;
    }
  }
  return v;
#endif
}

// 삽입/갱신: 동일 키 존재 시 갱신, 없으면 victim 선정 후 교체. 교체 시 evicts++
void tlb_insert(uint32_t pid, uint32_t va_page, uint32_t pa_page, uint16_t flags){
  acquire(&g_tlb.lock);
  uint32_t s = tlb_index(pid, va_page);

  // 동일 키 갱신 경로
  for (int w = 0; w < SOFT_TLB_WAYS; w++) {
    struct tlb_entry *e = &g_tlb.sets[s][w];
    if (e->valid && e->pid == pid && e->va_page == va_page) {
      e->pa_page = pa_page;
      e->flags   = flags;
      e->age++;
      release(&g_tlb.lock);
      return;
    }
  }

  // 교체 경로
  int wv = victim_way_nolock(s);
  struct tlb_entry *v = &g_tlb.sets[s][wv];
  if (v->valid) g_tlb.evicts++;
  v->pid     = pid;
  v->va_page = va_page;
  v->pa_page = pa_page;
  v->flags   = flags;
  v->valid   = (uint8_t)1;
  v->age     = U32_C(1);
  release(&g_tlb.lock);
}

// 단일 엔트리 무효화: (pid, va_page) 일치 항목을 valid=0로 표기, invalidations++
void tlb_invalidate_one(uint32_t pid, uint32_t va_page){
  acquire(&g_tlb.lock);
  uint32_t s = tlb_index(pid, va_page);
  for (int w = 0; w < SOFT_TLB_WAYS; w++) {
    struct tlb_entry *e = &g_tlb.sets[s][w];
    if (e->valid && e->pid == pid && e->va_page == va_page) {
      e->valid = (uint8_t)0;
      g_tlb.invalidations++;
    }
  }
  release(&g_tlb.lock);
}

// 특정 PID 소유 엔트리 전부 무효화(invalidate all for PID)
void tlb_invalidate_pid(uint32_t pid){
  acquire(&g_tlb.lock);
  for (int s = 0; s < SOFT_TLB_SETS; s++)
    for (int w = 0; w < SOFT_TLB_WAYS; w++) {
      struct tlb_entry *e = &g_tlb.sets[s][w];
      if (e->valid && e->pid == pid) {
        e->valid = (uint8_t)0;
        g_tlb.invalidations++;
      }
    }
  release(&g_tlb.lock);
}

// 전체 무효화(global flush). 한 번의 연산으로 invalidations를 1 증가시켜 이벤트로 집계.
void tlb_invalidate_all(void){
  acquire(&g_tlb.lock);
  for (int s = 0; s < SOFT_TLB_SETS; s++)
    for (int w = 0; w < SOFT_TLB_WAYS; w++)
      g_tlb.sets[s][w].valid = (uint8_t)0;
  g_tlb.invalidations++;
  release(&g_tlb.lock);
}

// 통계 스냅샷 획득: 락 하에 로컬 변수로 복사 후, 락 해제하고 사용자 버퍼로 memmove
int tlb_get_stats(struct tlb_stats *out){
  struct tlb_stats s;
  acquire(&g_tlb.lock);
  s.hits          = g_tlb.hits;
  s.misses        = g_tlb.misses;
  s.evicts        = g_tlb.evicts;
  s.invalidations = g_tlb.invalidations;
  s.lookups       = g_tlb.lookups;
  s.lines         = SOFT_TLB_LINES;
  s.ways          = SOFT_TLB_WAYS;
  s.sets          = SOFT_TLB_SETS;
  release(&g_tlb.lock);
  memmove(out, &s, sizeof(s));
  return 0;
}

// 엔트리 덤프: (옵션) 유효 엔트리만 수집. 최대 max개까지 채움.
int tlb_dump_entries(struct tlb_entry *buf, int max, int only_valid){
  int n = 0;
  acquire(&g_tlb.lock);
  for (int s = 0; s < SOFT_TLB_SETS && n < max; s++)
    for (int w = 0; w < SOFT_TLB_WAYS && n < max; w++) {
      struct tlb_entry e = g_tlb.sets[s][w];
      if (!only_valid || e.valid) buf[n++] = e;
    }
  release(&g_tlb.lock);
  return n;
}

// (일관성 보조) 단일 VA 무효화: 모든 세트를 순회하며 (pid,va_page) 일치 엔트리를 invalid
void tlb_invalidate_va(uint32_t pid, uint32_t va_page) {
  acquire(&g_tlb.lock);
  g_tlb.invalidations++;
  for (int s = 0; s < SOFT_TLB_SETS; s++) {
    for (int w = 0; w < SOFT_TLB_WAYS; w++) {
      struct tlb_entry *e = &g_tlb.sets[s][w];
      if (e->valid && e->pid == pid && e->va_page == va_page) {
        e->valid = (uint8_t)0;
      }
    }
  }
  release(&g_tlb.lock);
}

// (일관성 보조) VA 범위 무효화: [a, b) 페이지 경계로 순회하며 tlb_invalidate_va 호출
void tlb_invalidate_range(uint32_t pid, uint32_t a, uint32_t b) {
  for (uint32_t va = (uint32_t)PGROUNDDOWN(a); va < b; va += (uint32_t)PGSIZE)
    tlb_invalidate_va(pid, va);
}

// 카운터만 0으로 리셋(엔트리는 보존). 측정 구간 초기화용.
void tlb_reset_counters(void) {
  acquire(&g_tlb.lock);
  g_tlb.hits = g_tlb.misses = g_tlb.evicts = g_tlb.invalidations = g_tlb.lookups = 0;
  release(&g_tlb.lock);
}

// 전체 초기화: 모든 엔트리 invalid + 모든 카운터 0 (하드 리셋)
void tlb_reset_all(void) {
  acquire(&g_tlb.lock);
  for (int s=0; s<SOFT_TLB_SETS; s++)
    for (int w=0; w<SOFT_TLB_WAYS; w++)
      g_tlb.sets[s][w].valid = 0;
  g_tlb.hits = g_tlb.misses = g_tlb.evicts = g_tlb.invalidations = g_tlb.lookups = 0;
  release(&g_tlb.lock);
}
