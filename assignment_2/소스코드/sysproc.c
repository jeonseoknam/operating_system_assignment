#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

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

// 커널 핸들러(시스템 콜)
// 티켓값이 클수록 stride가 작아지면서 더 자주 선택된다. 따라서 티켓값이 클수록(stride가 작을수록) 더 빨리 프로세스가 종료된다
int
sys_settickets(void)
{
		int tickets, end_ticks;
		struct proc *p = myproc();
	
		// 사용자 영역에서 tickets, end_ticks에 해당하는 정수 인자 2개를 읽어온다
		if(argint(0, &tickets) < 0) 
				return -1;
		if(argint(1, &end_ticks) < 0)
				return -1;

		// 유효성 검사(명세 참고)
		// tickets: 1 이상 && STRIDE_MAX 미만
		if(tickets < 1 || tickets >= STRIDE_MAX)
				return -1;

		// Stride 스케쥴러 관련 필드 갱신
		p->tickets = tickets;
		p->stride = STRIDE_MAX / tickets;   // 정수 나눗셈(나머지 버림)
		
		// 테스트 가이드라인 결과와 같도록, 초기 pass값을 0으로 설정
		p->pass = 0;

		// end_ticks: 1미만이면 무시(기존 값 유지), 1 이상이면 지정
		if(end_ticks >= 1) 
				p->end_ticks = end_ticks;

		return 0;
}
