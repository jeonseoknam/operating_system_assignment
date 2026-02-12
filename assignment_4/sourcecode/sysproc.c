#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "fcntl.h"

extern int snapshot_create(void);
extern int snapshot_rollback(int id);
extern int snapshot_delete(int id);

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

// snapshot_create()를 호출하기 위한 시스템콜 래퍼
int
sys_snapshot_create(void)
{
  return snapshot_create();
}

// snapshot_rollback()을 호출하기 위한 시스템콜 래퍼
int
sys_snapshot_rollback(void)
{
  int id;
  if (argint(0, &id) < 0)
    return -1;
  return snapshot_rollback(id);
}

// snapshot_delete()를 호출하기 위한 시스템콜 래퍼
int
sys_snapshot_delete(void)
{
  int id;
  if (argint(0, &id) < 0)
    return -1;
  return snapshot_delete(id);
}

