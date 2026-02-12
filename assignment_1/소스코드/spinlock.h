#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "types.h"

struct cpu; // 전방선언

// Mutual exclusion lock.
struct spinlock {
  uint locked;       // 현재 이 락이 잡혀있으면 1, 아니면 0

  char *name;        // 디버깅용 문자열 이름. 예를 들어 "ptable" 같은 이름을 붙여두면, 프로세스 테이블 락인지 바로 알 수 있음
  struct cpu *cpu;   // 현재 이 락을 들고 있는 CPU를 가리킨다.
  uint pcs[10];      // 락을 잡았을 때의 호출 스택(program counter)들을 저장한다
                    
};

#endif
