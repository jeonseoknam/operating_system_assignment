#include "types.h"
#include "stat.h"
#include "user.h"
#include "mmu.h"   // PTE_* bits

static void show(const char *tag, uint va){
  uint pa, fl;
  int rc = vtop((void*)va, &pa, &fl);
  if(rc==0)
    printf(1,"%s va=0x%x -> pa=0x%x flags=0x%x (P=%d W=%d U=%d)\n",
           tag, va, pa, fl, !!(fl&PTE_P), !!(fl&PTE_W), !!(fl&PTE_U));
  else
    printf(1,"%s va=0x%x -> rc=%d\n", tag, va, rc);
}

static void check_offset(uint va_base) {
  uint pa, fl;
  int rc = vtop((void*)(va_base + 0x123), &pa, &fl);
  printf(1, "[offset] va=0x%x rc=%d pa=0x%x (pa&0xFFF=0x%x expect 0x123)\n",
         va_base+0x123, rc, pa, pa & 0xFFF);
}

static void check_unmapped(uint va) {
  uint pa, fl;
  int rc = vtop((void*)va, &pa, &fl);
  printf(1, "[unmapped] va=0x%x rc=%d (expect -1 or -2)\n", va, rc);
}

int
main(void)
{
  // 1) 사용자 힙에서 정확히 1페이지만 확보
  char *p  = sbrk(4096);
  if((int)p < 0){ printf(1,"sbrk fail\n"); exit(); }
  uint va0 = ((uint)p) & ~0xFFF;

  // 2) 동일 VA 두 번 조회 → 2번째는 SW-TLB hit 기대(커널 로그로 확인)
  show("[vtop]", va0);
  show("[vtop]", va0);

  // 3) 오프셋 보존 확인
  check_offset(va0);

  // 4) 원본의 PA/flags
  uint pa0, fl0;
  if(vtop((void*)va0, &pa0, &fl0) != 0){ printf(1,"vtop base fail\n"); exit(); }
  uint pa_page = pa0 & ~0xFFF;

  // 5) alias 만들기: va1은 아직 미할당 영역으로 설정
  uint va1 = va0 + 0x1000;  // sbrk(4096)만 했으니 va1은 unmapped 상태
  if(vm_map_user_page(va1, pa_page, PTE_U|PTE_W) < 0){
    printf(1,"vm_map_user_page RWU fail\n"); exit();
  }
  show("[alias RWU]", va1);

  // 6) 권한 변경: W 제거 → 플래그 0x7 → 0x5 기대 + TLB invalidation 반영
  if (protect_nowrite((void*)va1) < 0) {
    printf(1,"protect_nowrite fail\n"); exit();
  }
  show("[alias RO ]", va1);

  // 7) unmap → vtop이 미매핑 에러(-2) 반환 기대
  if (vm_unmap_one((void*)va1, 0) < 0) {
    printf(1,"unmap fail\n"); exit();
  }
  show("[after unmap]", va1);   // rc 음수 기대
  check_unmapped(va1);

  // 8) 다시 remap (RO) → flags 0x5 확인
  if (vm_map_user_page(va1, pa_page, PTE_U /*RO*/) < 0) {
    printf(1,"remap RO fail\n"); exit();
  }
  show("[remap RO]", va1);

  printf(1,"[vtop_proof] done\n");
  exit();
}

