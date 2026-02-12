// pfind.c — 지정한 물리 페이지(pa_page)에 매핑된 (pid, va, flags) 목록을 사용자 공간에서 조회/출력

#include "types.h"
#include "user.h"
#include "fcntl.h"
#include "vlist.h"  // struct vlist { int pid; uint va_page; uint flags; }
#include "stat.h"

// 16진수 문자열을 u32로 변환(선택적 "0x"/"0X" 프리픽스 지원)
static uint parse_hex_u32(const char *s) {
  uint v = 0;
  if (!s) return 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  for (; *s; s++) {
    int d;
    if (*s >= '0' && *s <= '9')       d = *s - '0';
    else if (*s >= 'a' && *s <= 'f')  d = *s - 'a' + 10;
    else if (*s >= 'A' && *s <= 'F')  d = *s - 'A' + 10;
    else break;
    v = (v << 4) | (uint)d;
  }
  return v;
}


int main(int argc, char **argv) {
  if (argc < 2) { printf(1,"usage: pfind PA_PAGE_HEX\n"); exit(); }
  uint pa_page = parse_hex_u32(argv[1]);       // 물리 페이지 주소(페이지 정렬된 값) 파싱

  int max = 1;                                  // 커널에 요청할 최대 엔트리 수(작게 시작)
  struct vlist buf[4];                          // 스택 고정 버퍼(여유분 4개 확보); overflow 방지는 커널이 max를 준수한다는 전제
                                                // 필요 시 max를 늘려 재호출하는 전략으로 확장 가능

  int n = phys2virt(pa_page, buf, max);         // 커널 시스템콜/래퍼: pa_page에 매핑된 (pid, va, flags) 목록 채움
  if (n < 0) { printf(1, "phys2virt failed\n"); exit(); }

  for (int i = 0; i < n; i++)
    printf(1,"pid=%d va=0x%x flags=0x%x\n", buf[i].pid, buf[i].va_page, buf[i].flags);

  exit();
}
