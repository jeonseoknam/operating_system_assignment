#include "types.h"
#include "user.h"
#include "vlist.h"
#include "stat.h"

#define PGSIZE 4096
#define PTE_U  0x004
#define PTE_W  0x002

// 이미 있다면 생략: sw_vtop 래퍼
//extern int vtop_sys(void *va, uint *pa_out, uint *flags_out);

int
main(void)
{
  // 1) 원본 페이지 하나 확보
  char *p = sbrk(PGSIZE);
  if ((int)p == -1) { printf(1, "sbrk fail\n"); exit(); }
  p[0] = 0x5A;  // A/D 비트 세우고 내용도 기록

  // 2) sw_vtop로 물리 페이지 얻기
  uint pa, fl;
  if (vtop(p, &pa, &fl) < 0) { printf(1,"vtop_sys fail\n"); exit(); }
  uint pa_page = pa & ~0xFFF;
  printf(1, "base VA=0x%x -> pa_page=0x%x flags=0x%x\n", (uint)p, pa_page, fl);

  // 3) 비어 있을 법한 다른 VA 선택(원본으로부터 4페이지 뒤)
  uint va2 = ((uint)p) + 4*PGSIZE;
  va2 &= ~0xFFF;

  // 4) 같은 물리페이지를 다른 VA에 추가 매핑 (U|W)
  if (vm_map_user_page(va2, pa_page, PTE_U | PTE_W) < 0) {
    printf(1, "vm_map_user_page fail\n"); exit();
  }
  printf(1, "aliased map: va2=0x%x -> same pa_page=0x%x\n", va2, pa_page);

  // 5) alias 확인(동일 데이터가 보이는지)
  char *q = (char*)va2;
  printf(1, "before: p[0]=0x%x q[0]=0x%x\n", p[0] & 0xFF, q[0] & 0xFF);
  q[0] ^= 0xFF; // 뒤집기
  printf(1, "after : p[0]=0x%x q[0]=0x%x (should be same)\n", p[0] & 0xFF, q[0] & 0xFF);

  // 6) phys2virt로 IPT 체인 확인
  struct vlist buf[8];
  int n = phys2virt(pa_page, buf, 8);
  if (n < 0) { printf(1, "phys2virt fail\n"); exit(); }

  printf(1, "phys2virt(pa_page=0x%x) -> %d entries\n", pa_page, n);
  for (int i=0;i<n;i++)
    printf(1, "  [%d] pid=%d va=0x%x flags=0x%x\n", i, buf[i].pid, buf[i].va_page, buf[i].flags);

  // 기대: 같은 pid로 서로 다른 va 두 줄 이상(≥2)
  exit();
}

