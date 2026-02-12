#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "physmem.h"   // struct physframe_info 정의가 보이게 추가

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// 전역 물리 페이지 관리 테이블 관련 시스템콜
int
sys_dump_physmem_info(void)
{
  char *uaddr;
  int maxe;

  // 1) 두 번째 인자부터 먼저 읽어서 크기 결정
  if (argint(1, &maxe) < 0) return -1;
  if (maxe <= 0) return -1;

  // 2) 첫 번째 인자(버퍼 포인터) 유효성 검사: 필요한 총 바이트 수로 체크
  if (argptr(0, &uaddr, maxe * sizeof(struct physframe_info)) < 0) return -1;

  // 3) 실제 작업 호출
  return dump_physmem_info_kernel(uaddr, maxe);
}

