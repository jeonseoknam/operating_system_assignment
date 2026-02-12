// ipt.h
#pragma once
#include "types.h"
#include "mmu.h"
#include "defs.h"
//#include "spinlock.h"

#ifndef IPT_BUCKETS
#define IPT_BUCKETS 1024            // 2의 거듭제곱 (마스킹 쉬움)
#endif

// 물리주소 → PFN
#define PFN(pa)   ((uint32_t)(pa) >> 12)
// VA 페이지 경계
#define VAPAGE(va) ((uint32_t)(va) & ~0xFFF)

struct ipt_entry {
  uint32_t pfn;        // 물리 프레임 번호 (pa >> 12)
  uint32_t pid;        // 소유 프로세스 PID
  uint32_t va;         // 매핑된 가상주소(페이지 기준)
  uint16_t flags;      // PTE 권한 스냅샷 (P/W/U 등)
  uint16_t refcnt;     // 역참조 카운트(옵션) — 중복 삽입 시 증가
  struct ipt_entry *next; // 해시 체인
};

// 명세가 요구하는 전역 버킷 배열
extern struct ipt_entry *ipt_hash[IPT_BUCKETS];

// 전역 락(간단히 1개)
extern struct spinlock ipt_lock;

// API
void ipt_init(void);
// 새 pgdir을 전범위 스캔하여 IPT를 재구축
void ipt_rebuild_for_pgdir(uint32_t pid, pde_t *pgdir);
// (선택) old pgdir의 모든 유저 매핑을 IPT에서 제거
void ipt_scrub_for_pgdir(uint32_t pid, pde_t *pgdir);
// pgdir->pid 역매핑 헬퍼로 항상 정확한 pid 확보
int ipt_pid_from_pgdir(pde_t *pgdir);

void ipt_add(pde_t *pgdir, uint32_t pid, uint32_t va_page, uint32_t pa, uint32_t pte_flags);
int ipt_remove(pde_t *pgdir, uint32_t pid, uint32_t va_page, uint32_t pa_page);

struct ipt_entry *ipt_lookup(uint32_t pid, uint32_t va_page); // 편의
uint32_t ipt_count_pfn(uint32_t pfn);                         // 편의

int ipt_remove_fallback(uint32_t pid, uint32_t va_page, uint32_t pfn);
