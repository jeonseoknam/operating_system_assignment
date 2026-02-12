/*==============================================================================
 * memdump.c  |  전역 물리 프레임 테이블 덤프/필터링 유틸리티
 *
 * 목적  : 커널 시스템콜 dump_physmem_info()를 통해 물리 프레임 테이블을 사용자 공간으로
 *         수집하고, 옵션에 따라 전체/할당만/특정 PID 소유 프레임만 표 형식으로 출력.
 * 사용법: memdump [-a] [-p PID]
 *        -a      : free 프레임까지 포함하여 전체 테이블 출력
 *        -p PID  : 지정 PID가 점유 중인 프레임만 출력
 * 전제  : 커널에 struct physframe_info { frame_index, allocated, pid, start_tick, ... } 와
 *         dump_physmem_info(buf, max) 시스템콜이 구현되어 있어야 함
 * 규모  : MAX_FRINFO(60000) 엔트리까지 수신(환경에 맞게 조정 가능)
 *==============================================================================*/

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define MAX_FRINFO 60000   // 수신 버퍼 상한(가용 PFN 수보다 크거나 같게 설정 권장)

/* 잘못된 인자 사용 시 간단한 도움말을 출력하고 종료. */
static void
usage(void)
{
  printf(1, "usage: memdump [-a] [-p PID]\n");
  printf(1, "   -p PID : 해당 PID가 점유중인 물리 프레임만 출력\n");
  printf(1, "   -a     : free 프레임까지 포함해 전체 테이블 출력\n");
  exit();
}

int
main(int argc, char *argv[])
{
  int show_all = 0;     // -a 지정 여부
  int filter_pid = 0;   // -p 지정 여부
  int pid_value = -1;   // -p 인자로 받은 대상 PID

  // 옵션 파싱: 허용된 조합은 (-a) 또는 (-p PID) 중 하나이며, 미지정이면 usage 출력
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] != '-') usage();
    if (argv[i][1] == 'a' && argv[i][2] == '\0') {
      show_all = 1;
    } else if (argv[i][1] == 'p' && argv[i][2] == '\0') {
      if (i + 1 >= argc) usage();
      pid_value = atoi(argv[++i]);
      filter_pid = 1;
      if (pid_value <= 0) usage();   // 유효하지 않은 PID 방지
    } else {
      usage();
    }
  }
  if (argc == 1) usage();  // 옵션 미제공 시 사용법 안내

  // 커널로부터 물리 프레임 정보 테이블을 한 번에 받아온다
  static struct physframe_info buf[MAX_FRINFO];
  int n = dump_physmem_info((void*)buf, MAX_FRINFO);
  if (n < 0) {
    printf(1, "memdump: dump_physmem_info failed\n");
    exit();
  }

  // 실행 모드 헤더와 컬럼명을 출력.
  if (filter_pid) {
    printf(1, "[memdump] target pid=%d (self=%d)\n", pid_value, getpid());
  } else if (show_all) {
    printf(1, "[memdump] show_all (self=%d)\n", getpid());
  } else {
    printf(1, "[memdump] allocated-only (self=%d)\n", getpid());
  }
  printf(1, "[frame#]\t[alloc]\t[pid]\t[start_tick]\n");

  // 본문 출력: 모드에 따라 필터링하여 행을 출력하고, 출력 건수를 집계.
  int printed = 0;
  for (int i = 0; i < n; i++) {
    int alloc = buf[i].allocated;  // 1=할당됨, 0=free
    int owner = buf[i].pid;        // 프레임 소유 PID (free의 경우 구현에 따라 0/-1 등)

    if (filter_pid) {
      if (alloc == 1 && owner == pid_value) {
        printf(1, "%d\t\t%d\t%d\t%d\n",
               buf[i].frame_index, alloc, owner, buf[i].start_tick);
        printed++;
      }
    } else if (show_all) {
      printf(1, "%d\t\t%d\t%d\t%d\n",
             buf[i].frame_index, alloc, owner, buf[i].start_tick);
      printed++;
    } else { // allocated-only: 할당된 프레임만 출력
      if (alloc == 1) {
        printf(1, "%d\t\t%d\t%d\t%d\n",
               buf[i].frame_index, alloc, owner, buf[i].start_tick);
        printed++;
      }
    }
  }

  // 출력된 행이 없을 때 사용자에게 상황을 안내.
  if (printed == 0) {
    if (filter_pid)
      printf(1, "(no frames owned by pid=%d — process may have exited)\n", pid_value);
    else if (!show_all)
      printf(1, "(no allocated frames)\n");
  }
  exit();
}
