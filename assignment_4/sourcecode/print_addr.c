//print_addr.c 
// 커널 쪽 print_addr 시스템콜을 호출하는 유저 프로그램(wrapper)
// 출력 결과 해석
// addr[0...11]: 파일의 직접 블록들이 가리키는 디스크 블록 번호를 출력
// addr[12]: 간접 블록 자체의 주소 출력
// addr[12] -> [0]: 간접 블록 내의 첫 번째 엔트리 -> 논리 블록 bn = 12가 가리키는 실제 데이터 블록 번호를 출력
// 사용 목적: 특정 파일의 물리 디스크 블록 매핑이 어떻게 변하는지를 직접 눈으로 확인하는 도구이다.

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

int
main(int argc, char *argv[])
{
  // 인자 개수 체크
  // 사용 방법: print_addr <파일 이름>
  if(argc != 2){
    printf(1, "Usage: print_addr filename\n");
    exit();
  }

  // 존재 여부 정도만 확인 
  int fd = open(argv[1], O_RDONLY);
  if(fd < 0){
    printf(1, "print_addr: cannot open %s\n", argv[1]);
    exit();
  }
  close(fd);

  // 커널의 print_addr syscall 호출 (커널에서 cprintf로 출력)
  if(print_addr(argv[1]) < 0){
    printf(1, "print_addr: syscall failed\n");
  }

  exit();
}

