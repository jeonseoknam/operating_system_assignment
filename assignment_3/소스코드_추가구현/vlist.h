// vlist.h  (커널/유저 공용)
#ifndef XV6_VLIST_H
#define XV6_VLIST_H
#include "types.h"

// 물리 페이지(Physical Page)에 매핑된 가상 주소(Virtual Page)들의 정보를 담는 구조체
// - pid      : 해당 VA를 사용하는 프로세스의 PID
// - va_page  : 페이지 단위로 정렬된 가상 주소 (하위 12비트는 0)
// - flags    : 해당 VA의 PTE 플래그 (P/W/U 등 권한 비트)

struct vlist {
  int  pid;
  uint va_page;
  uint flags;
};
#endif

