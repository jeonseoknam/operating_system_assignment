// snap_create.c
// 시스템콜 snap_create()를 호출하여 스냅샷을 만드는 프로그램
// id는 increment하게 증가하게 했으며, 삭제되어 중간에 비는 id가 있더라도 무시하고 계속 증가하도록 구현함(명세 언급 X) 
#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
  int id = snapshot_create();
  if(id < 0){
    printf(1, "snap_create: snapshot_create failed\n");
  } else {
    printf(1, "snapshot id = %d\n", id);
  }
  exit();
}

