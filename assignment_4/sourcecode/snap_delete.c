// snap_delete.c
// 시스템콜 snapshot_delete()를 호출하여 인자로 받은 ID의 스냅샷 파일을 삭제하는 프로그램
#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
  if(argc != 2){
    printf(1, "Usage: snap_delete id\n");
    exit();
  }

  int id = atoi(argv[1]);
  int r = snapshot_delete(id);
  if(r < 0){
    printf(1, "snap_delete: failed (id=%d)\n", id);
  } else {
    printf(1, "snap_delete: success (id=%d)\n", id);
  }

  exit();
}

