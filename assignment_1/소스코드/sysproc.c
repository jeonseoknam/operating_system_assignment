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

int
sys_hello_number(void){
		int n;
		if(argint(0, &n) < 0)
				return -1;
		cprintf("Hello, xv6! Your number is %d\n", n);
		return n * 2;
}

struct k_procinfo{
		int pid;
		int ppid;
		int state; 
		uint sz; 
		char name[16];
};

// pid
int sys_get_procinfo(void){
		int pid;
		char *uaddr; 		// 유저 버퍼 시작 주소
		struct proc *p, *t;
		struct k_procinfo kinfo;
		
		// 시스템콜의 첫 번째 인자(0)를 int로 꺼내서 pid 변수에 저장
		// 사용자가 넘긴 pid값을 커널 변수 pid로 안전하게 가져온다
		if(argint(0, &pid) < 0)  
				return -1;
		// 시스템콜의 두 번째 인자(1)를 포인터로 받아온다
		// 즉, 유저가 넘긴 포인터가 가리키는 주소(struct k_procinfo *uinfo의 주소)가 가리키는 주소를
		// uaddr에 저장한다
		if(argptr(1, &uaddr, sizeof(struct k_procinfo)) < 0) 
				return -1;
		
		// 프로세스 테이블(ptable)에 접근할 때 동시성 문제를 막기 위해 락을 잡음
		// 락 관련 문제 발생
		acquire(&ptable.lock);

		// pid <= 0 : 호출한 자기 자신 정보 조회
		if(pid <= 0) 
				t = myproc();   // 현재 실행 중인 프로세스를 가져오는 함수
		// pid > 0 : 해당 PID 프로세스의 정보를 조회
		else{
				t = 0;
				for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
						if(p->pid == pid) {
								t = p;
								break;
						}
		}
		
		// 해당 프로세스(t)가 없거나 UNUSED 상태면 에러 반환
		if(t == 0 || (t->state == UNUSED)) {
				release(&ptable.lock);
				return -1;
		}
		

		// t에서 필요한 값 추출
		// 입력받은 PID의 프로세스 정보를 kinfo 구조체에 저장한다
		kinfo.pid = t->pid;      					  // 프로세스 PID
		kinfo.ppid = t->parent ? t->parent->pid : 0;  // 부모 프로세스 PID(없으면 0)
		kinfo.state = t->state;  					  // 프로세스 상태
		kinfo.sz = t->sz;       					  // 메모리 크기(bytes)
		safestrcpy(kinfo.name, t->name, sizeof(kinfo.name));  // 프로세스 이름
	
		// copyout/return을 하려면 락을 풀어야 한다
		release(&ptable.lock);
	
		// 유저 공간으로 복사
		// copyout()을 이용해 커널 공간의 kinfo를 사용자 버퍼(uaddr)로 복사한다.
		if(copyout(myproc()->pgdir, (uint)uaddr, (void*)&kinfo, sizeof(kinfo)) < 0)
				return -1;
		return 0;
}
