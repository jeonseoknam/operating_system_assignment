// physmem.h  
#ifndef PHYSMEM_H
#define PHYSMEM_H

#include "types.h"   // xv6의 uint 정의

#define PFNNUM 60000

struct physframe_info {
  uint frame_index;  // 물리 프레임 번호 (pa >> 12)
  int  allocated;    // 1=할당됨, 0=free
  int  pid;          // 소유 PID, 없으면 -1
  uint start_tick;   // 할당 시작 tick
};

#endif

