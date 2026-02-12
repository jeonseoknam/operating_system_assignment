// meta_dump.c
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

struct block_meta {
  uchar ref;
  uchar snap;
};

int
main(int argc, char *argv[])
{
  int fd;
  struct block_meta m;
  int idx = 0;

  fd = open("/snapshot/blockmeta", O_RDONLY);
  if(fd < 0){
    printf(1, "bmdump: cannot open /snapshot/blockmeta\n");
    exit();
  }

  // 한 레코드(struct block_meta)씩 읽으면서,
  // ref==0 && snap==0 인 블록은 그냥 건너뛰고,
  // 값이 있는 블록만 출력
  while(read(fd, &m, sizeof(m)) == sizeof(m)){
    if(m.ref != 0 || m.snap != 0){
      printf(1, "b %x : ref=%d snap=%d\n", idx, m.ref, m.snap);
    }
    idx++;
  }

  close(fd);
  exit();
}

