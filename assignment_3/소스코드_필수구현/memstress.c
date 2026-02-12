/*==============================================================================
 * memstress.c  |  메모리 스트레스/관찰용 사용자 프로그램
 *
 * 목적  : 지정한 개수의 페이지를 sbrk()로 확보하고, 일정 tick 동안 보유한 뒤 종료한다.
 * 기능  : (선택) 각 페이지의 첫 바이트를 실제로 쓰기(-w)하여 페이지 접근과 TLB 채움을 유도한다.
 * 사용법: memstress [-n pages] [-t ticks] [-w]
 *        -n <pages> : 확보할 페이지 수 (기본 31)
 *        -t <ticks> : 보유 시간 ticks (기본 500)
 *        -w         : 각 페이지의 첫 바이트에 실제로 write 수행
 * 비고  : 페이지 크기 4096을 상수로 사용합니다. 환경에 따라 PGSIZE 상수로 대체하는 것이 바람직하다.
 *==============================================================================*/

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

// 잘못된 인자 사용 시 사용법을 안내하고 즉시 종료한다.
static void
usage(void) {
  printf(1, "usage: memstress [-n pages] [-t ticks] [-w]\n");
  printf(1, "   -n <pages> : 확보할 페이지 수 (기본 31)\n");
  printf(1, "   -t <ticks> : 보유 시간 ticks (기본 500)\n"); 
  printf(1, "   -w       : 각 페이지 첫 바이트에 실제로 write\n");
  exit();
}

int
main(int argc, char *argv[])
{
  int pages = 31;        // 기본 확보 페이지 수
  int hold_ticks = 500;  // 기본 보유 시간(틱) 
  int do_write = 0;      // -w 옵션 처리(1이면 각 페이지 첫 바이트에 쓰기 수행)

  // 옵션 파싱: 형식 오류나 인자 누락 시 usage() 호출 후 종료한다.
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] != '-') usage();

    if (argv[i][1] == 'n' && argv[i][2] == '\0') {
      // -n <pages> : 확보할 페이지 수 설정
      if (i + 1 >= argc) usage();
      pages = atoi(argv[++i]);
      if (pages <= 0) usage();      // 0 이하 방지
    } else if (argv[i][1] == 't' && argv[i][2] == '\0') {
      // -t <ticks> : 보유 시간(틱) 설정
      if (i + 1 >= argc) usage();
      hold_ticks = atoi(argv[++i]);
      if (hold_ticks < 0) usage();  // 음수 방지
    } else if (argv[i][1] == 'w' && argv[i][2] == '\0') {
      // -w : 각 페이지 첫 바이트에 실제 쓰기 수행
      do_write = 1;
    } else {
      usage();
    }
  }

  // 실행 구성 로깅: 이후 memdump/tlbstat 등과 함께 관찰 시 유용하다.
  int pid = getpid();
  printf(1, "[memstress] pid=%d pages=%d hold=%d ticks write=%d\n",
         pid, pages, hold_ticks, do_write);

  // 메모리 확보: sbrk로 pages * 4096 바이트만큼 힙을 증가시킵니다. (4096 = 4KiB 페이지 가정)
  int inc = pages * 4096;             
  char *base = sbrk(inc);
  if (base == (char*)-1) {
    // 메모리 부족 등으로 sbrk 실패 시 즉시 종료.
    printf(1, "[memstress] sbrk failed\n");
    exit();
  }

  // -w 옵션: 각 페이지의 첫 바이트를 한 번씩 기록하여 실제 접근을 유도.
  // 이는 페이지 워크/프레임 할당 및 TLB 채움을 확실히 발생.
  if (do_write) {
    for (int p = 0; p < pages; p++) {
      base[p * 4096] = (char)(p & 0xff);  // 페이지 p의 첫 바이트에 식별 가능한 패턴 기록
    }
  }

  // 보유 단계: 설정한 tick 동안 점유 상태를 유지.
  // 이 구간에 memdump -p <pid> 등으로 프레임 점유 여부를 관찰 가능.
  sleep(hold_ticks);

  // 종료 로그 후 정상 종료합니다.
  printf(1, "[memstress] pid=%d done\n", pid);
  exit();
}
