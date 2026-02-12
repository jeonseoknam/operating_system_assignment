// snap_rollback.c
// 시스템콜 snalshot_rollback()을 호출하여 인자로 받은 ID의 스냅샷으로 현재 파일 시스템을 복구하는 프로그램
#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
  if(argc != 2){
    printf(1, "Usage: snap_rollback id\n");
    exit();
  }

  // 인자로 받은 id는 char *이므로, 정수로 처리하기 위해 atoi()를 사용했다
  int id = atoi(argv[1]);
  int r = snapshot_rollback(id);
  if(r < 0){
    printf(1, "snap_rollback: failed (id=%d)\n", id);
  } else {
    printf(1, "snap_rollback: success (id=%d)\n", id);
  }

  exit();
}

