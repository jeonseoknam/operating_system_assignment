// ipt.c
#include "types.h"
#include "defs.h"
#include "mmu.h"
#include "spinlock.h"
#include "proc.h"
#include "ipt.h"
#include "param.h"
#include "memlayout.h"

// 전역 심볼 (명세 스타일)
struct ipt_entry *ipt_hash[IPT_BUCKETS];
struct spinlock ipt_lock;

static int count_pfn_locked(uint32_t pfn);

// 간단한 해시 (버킷수가 2^n일 때 마스킹)
static inline uint32_t hash_u32(uint32_t x){
  x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
  return x & (IPT_BUCKETS - 1);
}

// 키 선택: (pid, va_page)를 키로 사용
static inline uint32_t ipt_bucket_idx(uint32_t pid, uint32_t va_page){
  return hash_u32(pid ^ va_page);
}

void ipt_init(void){
  initlock(&ipt_lock, "ipt");
  for(int i=0;i<IPT_BUCKETS;i++) ipt_hash[i] = 0;
}

// 락 잡힌 상태에서 체인에서 찾기
static struct ipt_entry * find_locked(uint32_t idx, uint32_t pid, uint32_t va_page){
  for(struct ipt_entry *e = ipt_hash[idx]; e; e = e->next){
    if(e->pid == pid && e->va == va_page) return e;
  }
  return 0;
}

void ipt_add(pde_t *pgdir, uint32_t pid, uint32_t va_page, uint32_t pa, uint32_t pte_flags){
  if(pgdir == 0) return;
  // 커널 매핑은 제외(유저 매핑만 추적) — 과제 의도/관리 난이도 측면
  if((pte_flags & PTE_U) == 0) return;
  if(pa == 0) return;

  uint32_t idx = ipt_bucket_idx(pid, va_page);

  acquire(&ipt_lock);
  struct ipt_entry *e = find_locked(idx, pid, va_page);
  if(e){
    // 동일한 (pid,va_page) 매핑이 또 들어오면 refcnt만 증가
    e->refcnt++;
    // 스냅샷/동기화 차원에서 최신 pfn/flags 갱신
    e->pfn   = PFN(pa);
    e->flags = (uint16_t)(pte_flags & 0x0FFF);
    release(&ipt_lock);
    return;
  }
  // 새 엔트리 할당
  e = (struct ipt_entry*)kalloc();
  if(!e){ release(&ipt_lock); return; }
  // 엔트리는 페이지 크기지만 단일 구조체만 사용 — 간단히 0으로
  memset(e, 0, PGSIZE);
  e->pfn   = PFN(pa);
  e->pid   = pid;
  e->va    = va_page;
  e->flags = (uint16_t)(pte_flags & 0x0FFF);
  e->refcnt= 1;
  e->next  = ipt_hash[idx];
  ipt_hash[idx] = e;
  release(&ipt_lock);
}

// 제거 후 "해당 PFN의 남은 총 참조수"를 반환
// - 엔트리를 못 찾은 경우에도 현재 PFN 총 참조수를 반환(=상태 보고용)
// - 호출 전후 모두 락 정합성을 유지
int
ipt_remove(pde_t *pgdir, uint32_t pid, uint32_t va_page, uint32_t pa_page)
{
  // 1) 비정상 인자: 해제 금지 방향(양수)로 반환
  if (pgdir == 0)
    return 1;

  // 2) 방어적 정렬 보정
  va_page &= ~(PGSIZE - 1);

  // 3) 커널 VA 가드(방어): 우리 테이블은 유저 매핑만 관리
  if (va_page >= KERNBASE) {
    acquire(&ipt_lock);
    int remain = count_pfn_locked(PFN(pa_page));
    release(&ipt_lock);
    return remain > 0 ? remain : 1;
  }

  uint32_t pfn = PFN(pa_page);
  uint32_t idx = ipt_bucket_idx(pid, va_page);

  acquire(&ipt_lock);

  struct ipt_entry **pp = &ipt_hash[idx];
  struct ipt_entry *e;

  while ((e = *pp) != 0) {
    if (e->pid == pid && e->va == va_page) {

      if (e->refcnt > 1) {
        e->refcnt--;
        int remain = count_pfn_locked(pfn); // 감소 반영 후
        release(&ipt_lock);
        return remain;
      }

      // refcnt == 1 → 제거
      *pp = e->next;
      kfree((char*)e);

      int remain = count_pfn_locked(pfn);   // 제거 반영 후
      release(&ipt_lock);
      return remain;
    }
    pp = &e->next;
  }

  
  //  못 찾았으면 음수 반환 (예: -2)
  release(&ipt_lock);
  return -2;
}


struct ipt_entry *ipt_lookup(uint32_t pid, uint32_t va_page){
  uint32_t idx = ipt_bucket_idx(pid, va_page);
  acquire(&ipt_lock);
  struct ipt_entry *e = find_locked(idx, pid, va_page);
  // 주의: 락 없이 포인터 반환 → 디버깅용으로만 사용 (출력 직전 락 재획득 권장)
  release(&ipt_lock);
  return e;
}

uint32_t ipt_count_pfn(uint32_t pfn){
  uint32_t cnt = 0;
  acquire(&ipt_lock);
  for(int i=0;i<IPT_BUCKETS;i++){
    for(struct ipt_entry *e = ipt_hash[i]; e; e = e->next){
      if(e->pfn == pfn) cnt += e->refcnt;
    }
  }
  release(&ipt_lock);
  return cnt;
}

// 유저 영역 PDE 범위: [0 .. KERNBASE)
static inline uint
user_pdx_end(void)
{
  return (KERNBASE >> PDXSHIFT);
}

// 새 pgdir을 스캔하여 (pid, va_page) → (pa_page, flags) 를 IPT에 채운다
// - pid == 0 이면 아무 것도 하지 않습니다(식별 불가).
// - 유저 매핑(PTE_U)만 넣는다
// - 이미 들어있는 (pid,va_page)는 ipt_add()의 중복 처리(refcnt++)/갱신 로직에 맡긴다.
void
ipt_rebuild_for_pgdir(uint32_t pid, pde_t *pgdir)
{
  if (pid == 0 || pgdir == 0)
    return;

  const uint end_pdx = user_pdx_end();

  for (uint pdx = 0; pdx < end_pdx; pdx++) {
    pde_t pde = pgdir[pdx];
    if ((pde & PTE_P) == 0)
      continue; // 이 PDE에는 PT 자체가 없음

    // PDE가 가리키는 페이지 테이블(물리) → 커널가상으로 변환
    pte_t *pt = (pte_t*)P2V(PTE_ADDR(pde));

    for (uint ptx = 0; ptx < NPTENTRIES; ptx++) {
      pte_t pte = pt[ptx];
      if ((pte & PTE_P) == 0)
        continue; // 엔트리 없음
      if ((pte & PTE_U) == 0)
        continue; // 커널 전용 매핑은 추적 안 함

      uint32_t va_page = PGADDR(pdx, ptx, 0);  // 페이지 경계
      uint32_t pa_page = PTE_ADDR(pte);        // 페이지 프레임 물리주소
      uint32_t flags   = (pte & 0x0FFF);       // 하위 12비트(perm/A/D 등)

      // 내부에서 ipt_lock을 잡고, 필요시 kalloc을 호출
      // 중복 키(pid,va_page)면 refcnt++ 또는 최신 flags/pfn으로 갱신
      ipt_add(pgdir, pid, va_page, pa_page, flags);
    }
  }
}

// 기존 pgdir의 모든 유저 매핑을 IPT에서 제거
// exec에서 oldpgdir 정리나 테스트용으로 씀
// - pid == 0이면 제거 대상 pid를 특정할 수 없으므로 아무 것도 안 함.
void
ipt_scrub_for_pgdir(uint32_t pid, pde_t *pgdir)
{
  if (pid == 0 || pgdir == 0)
    return;

  const uint end_pdx = user_pdx_end();

  for (uint pdx = 0; pdx < end_pdx; pdx++) {
    pde_t pde = pgdir[pdx];
    if ((pde & PTE_P) == 0)
      continue;

    pte_t *pt = (pte_t*)P2V(PTE_ADDR(pde));
    for (uint ptx = 0; ptx < NPTENTRIES; ptx++) {
      pte_t pte = pt[ptx];
      if ((pte & PTE_P) == 0)
        continue;
      if ((pte & PTE_U) == 0)
        continue;

      uint32_t va_page = PGADDR(pdx, ptx, 0);
      uint32_t pa_page = PTE_ADDR(pte);

      // 내부에서 ipt_lock을 잡는다.
      ipt_remove(pgdir, pid, va_page, pa_page);
    }
  }
}

static int
count_pfn_locked(uint32_t pfn)
{
  int cnt = 0;
  // ipt_lock이 이미 잡혀 있다고 가정
  for (int i = 0; i < IPT_BUCKETS; i++) {
    for (struct ipt_entry *x = ipt_hash[i]; x; x = x->next) {
      if (x->pfn == pfn)
        cnt += x->refcnt;
    }
  }
  return cnt;
}

int
ipt_remove_fallback(uint32_t pid, uint32_t va_page, uint32_t pfn)
{
  acquire(&ipt_lock);

  for (int i = 0; i < IPT_BUCKETS; i++) {
    struct ipt_entry **pp = &ipt_hash[i];
    while (*pp) {
      struct ipt_entry *e = *pp;

      // ★ pid, va, pfn 모두 일치할 때만 제거/감소
      if (e->pid == pid && e->va == va_page && e->pfn == pfn) {
        if (e->refcnt > 1) {
          e->refcnt--;
          int remain = count_pfn_locked(pfn);
          release(&ipt_lock);
          return remain;
        } else {
          *pp = e->next;
          kfree((char*)e);
          int remain = count_pfn_locked(pfn);
          release(&ipt_lock);
          return remain;
        }
      }
      pp = &e->next;
    }
  }

  // 못 찾음: 물리 해제 방지를 위해 음수 반환
  release(&ipt_lock);
  return -2;
}


