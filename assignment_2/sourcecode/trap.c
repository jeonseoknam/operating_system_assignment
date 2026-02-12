#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  // myproc(): 현재 실행 중인 내 프로세스를 가리키는 주소를 가져오는 함수
  // 현재 실행 중인 프로세스가 RUNNING 상태이면서 타이머 인터럽트가 발생하면 아래 분기 실행
  if(myproc() && myproc()->state == RUNNING && tf->trapno == T_IRQ0+IRQ_TIMER){
		  struct proc *p = myproc();

		  // 유저 모드에서 타이머에 의해 선점된 경우만 카운트/로그
          if((tf->cs&3) == DPL_USER){

			// 현재 틱이 종료 틱 수보다 커지면, 로그 한 번 출력하고 exit() 체크
		  if(p->end_ticks > 0 && p->ticks >= p->end_ticks){
				  if(p->pid > 2 && p->parent && p->parent->pid > 2){
				 // 커널/초기 프로세스는 로그에서 제외
				  cprintf("Process %d selected, stride : %d, ticket : %d, pass : %d -> %d (%d/%d)\n",
								  p->pid, p->stride, p->tickets, p->pass, p->pass + p->stride, p->ticks, p->end_ticks);
		  }
		  exit();
		  }

		  // 디버그 로그 (부모/자식 필터)
		  if(p->pid > 2 && p->parent && p->parent->pid > 2){
				  cprintf("Process %d selected, stride : %d, ticket : %d, pass : %d -> %d (%d/%d)\n",
								  p->pid, p->stride, p->tickets, p->pass, p->pass + p->stride, p->ticks, p->end_ticks);
		  }
		}
	
    // CPU를 양보 -> 스케줄러가 다른 프로세스를 고르게 한다.
    yield();
  }

  // Check if the process has been killed since we yielded
  // yield 이후에 해당 프로세스가 killed 상태라면 exit()
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
