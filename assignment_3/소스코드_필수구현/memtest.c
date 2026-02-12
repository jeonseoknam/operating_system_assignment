/*==============================================================================
 * memtest.c  |  메모리 스트레스/관찰 통합 시나리오 오케스트레이터
 *
 * 목적  : memstress(페이지 확보/보유)와 memdump(프레임 테이블 조회)를
 *         시간차로 실행하여, 프레임 할당·보유·해제 흐름을 관찰.
 * 흐름  : memstress 두 개를 순차 실행 → 중간에 memdump -p <PID>로 상태 확인 →
 *         자식 종료 대기 → 마지막으로 재확인(memdump -p 5).
 * 주의  : 아래 코드는 PID 4, 5를 하드코딩하여 memdump를 실행.
 *==============================================================================*/

#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
  int pid;

  /* 첫 번째 memstress 실행: 31페이지를 500틱 동안 보유하도록 자식 프로세스 생성 */
  pid = fork();
  if(pid < 0){
    printf(1, "fork failed\n");
    exit();
  }

  if(pid == 0){
    char *args[] = { "memstress", "-n", "31", "-t", "500", 0 };
    exec("memstress", args);          // 자식: memstress로 프로그램 교체
    printf(1, "exec memstress failed\n");
    exit();
  }

  sleep(100);  /* 완충 대기: 두 번째 memstress가 첫 번째와 겹치는 보유 구간을 만들기 위함 */

  /* 두 번째 memstress 실행: 동일한 조건으로 또 하나의 자식 생성 */
  int pid2 = fork();
  if(pid2 == 0){
    char *args2[] = { "memstress", "-n", "31", "-t", "500", 0 };
    exec("memstress", args2);
    printf(1, "exec memstress failed\n");
    exit();
  }

  sleep(100);  /* 완충 대기: 이후 memdump가 두 memstress의 보유 구간 안에서 실행되도록 조정 */

  /* 첫 번째 관찰: PID=4의 프레임만 출력 (하드코딩) */
  int pid3 = fork();
  if(pid3 < 0){
    printf(1, "fork failed\n");
    exit();
  }

  if(pid3 == 0){
    char *args3[] = { "memdump", "-p", "4", 0 };
    exec("memdump", args3);           // 자식: memdump -p 4 실행
    printf(1, "exec memdump failed\n");
    exit();
  }

  sleep(100);  /* 완충 대기: 다음 관찰과의 시간 간격 확보 */

  /* 두 번째 관찰: PID=5의 프레임만 출력 (하드코딩) */
  int pid4 = fork();
  if(pid4 < 0){
    printf(1, "fork failed\n");
    exit();
  }

  if(pid4 == 0){
    char *args4[] = { "memdump", "-p", "5", 0 };
    exec("memdump", args4);
    printf(1, "exec memdump failed\n");
    exit();
  }

  /* 앞서 생성한 네 개의 자식(memstress×2, memdump×2) 종료 대기 */
  wait();
  wait();
  wait();
  wait();

  sleep(100);  /* 정리 후 재검증 전 짧은 대기: 해제/무효화가 반영될 시간을 줌 */

  /* 최종 확인: PID=5의 프레임이 해제되었는지 재확인 (보통 0행 기대) */
  int pid5 = fork();
  if(pid5 < 0){
    printf(1, "fork failed\n");
    exit();
  }

  if(pid5 == 0){
    char *args5[] = { "memdump", "-p", "5", 0 };
    exec("memdump", args5);
    printf(1, "exec memdump failed\n");
    exit();
  }

  wait();  /* 마지막 memdump 자식 종료 대기 */

  exit();
}
