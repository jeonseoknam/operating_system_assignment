#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

// Stride 스케줄러 헬퍼 함수 원형
static struct proc* pick_min_pass_proc(void);
static void rebase_passes(void);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;
	
  // 전역 프로세스 테이블(배열)을 훑어 UNUSED 상태를 찾을 때, 동시성 문제 있으므로 ptable.lock을 잡고 진행한다
  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  // UNUSED 슬롯을 못 찾으면 종료(return 0)
  release(&ptable.lock);
  return 0;


found:
  p->state = EMBRYO; // UNUSED를 찾는 순간 EMBRYO로 상태를 바꾼다 -> 슬롯 사용 표시만 한 것. 아직 유저 프로그램을 돌릴 수 있는 상태는 아님
  p->pid = nextpid++;  // pid 할당

  release(&ptable.lock); // 테이블 갱신이 끝났으므로 락 해제

  // Allocate kernel stack. 
  if((p->kstack = kalloc()) == 0){  
	// 실패하면 슬롯 상태를 UNUSED로 롤백하고 종료
    p->state = UNUSED;
    return 0;
  }
  // 스택은 상위 주소에서 아래로 자란다. 따라서 스택 포인터 sp를 스택 최상단(높은 주소)로 잡는다.
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  // 트랩 프레임(trapframe) 자리 잡기
  // trapframe: 시스템 콜/인터럽트 등으로 인한 유저 -> 커널 진입 시 유저 레지스터 상태를 저장하는 구조체이다. 
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  // trapret: 트랩프레임을 복원해 유저 모드로 되돌리는 표준 루틴이다
  sp -= 4;
  *(uint*)sp = (uint)trapret;
	
  // 스케줄러가 swtch()로 이 프로세스에 처음으로 컨텍스트를 넘겨줄 때, 어디서부터 실행을 시작할지를 정한다
  // 그 시작점이 바로 forkret이다
  // forkret 안에서는 보통 락 해제 등 마무리를 하고, 이어서 위에 푸시해둔 trapret으로 점프해 유저/커널 복귀 흐름을 완성한다
  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;
	
  // stride scheduler 관련 초기화 추가
  p->tickets   = 1;  // 기본 티켓 수 1
  p->stride    = 0;  // settickets() 호출 시 계산
  p->pass      = 0;  // 시작 시 pass = 0
  p->ticks     = 1;  // 실행된 틱 누적
  p->end_ticks = -1; // 무제한 실행(end_ticks 설정 전까지)

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
  
  if(np->pid > 2 && np->parent->pid > 2){
		  cprintf("Process %d start\n", np->pid);
  }

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);
  
  // 종료되는 프로세스의 pass를 0으로 초기화
  curproc->pass = 0;

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }
  
  if(curproc->pid > 2 && curproc->parent->pid > 2){
		  cprintf("Process %d exit\n", curproc->pid);
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);

	// 1) RUNNABLE 중 최소 pass를 선택(pass 값이 같다면, 최소 pid를 선택)
	struct proc *p = pick_min_pass_proc();

	if(p == 0){
			// 실행할 것이 없는 경우
			release(&ptable.lock);
			continue;
	}

	// 2) 선택된 프로세스에게 CPU 1틱 부여
	c->proc = p;
	switchuvm(p);            // 프로세스 p의 주소로 가상 주소를 보게 한다
	p->state = RUNNING;		 // 프로세스 p가 실제로 CPU를 쓸 수 있도록 RUNNING 상태로 바꾼다

	// 컨텍스트 스위치: p가 타이머에 선점되거나 yield/블록 되면 돌아온다
	swtch(&c->scheduler, p->context);  // 지금(스케줄러)의 register set을 c->scheduler 컨텍스트에 저장하고, 프로세스 쪽에 저장돼 있던 p->context를 복원해서 그 자리부터 이어서 실행한다
	// 즉 스케줄러쪽 코드는 멈추고, 프로세스 코드가 시작된다
	// 반대로, 나중에 p가 yield()/sleep() 하거나 타이머에 선점되면 sched() 내부에서 역방향으로 swtch(&p->context, &c->scheduler)가 호출되어 스케줄러로 돌아온다
	switchkvm(); // 프로세스 실행을 마치고 스케줄러로 돌아온 직후, MMU를 커널 전용 페이지 테이블로 되돌린다.
	c->proc = 0;  // per-CPU 변수에 "현재 실행 중인 프로세스 없음"을 기록한다


    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");

    // 각 프로세스가 1틱만큼 CPU 점유를 마치고 제어권을 넘길 때 본인의 stride 값만큼 pass 값을 올림(명세)
     p->ticks++;
     p->pass += p->stride;
 
     // 프로세스의 pass값이 PASS_MAX 초과 시 rebase_passes() 호출
     if(p->pass > PASS_MAX){
             rebase_passes();
}

  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

// 선형 탐색으로 지금까지 본 pid의 pass들 중에서 가장 값이 작은 프로세스를 선택하는 함수
static struct proc* pick_min_pass_proc(void){
		// pass가 가장 작은 것이 제일 좋은 것이므로 best로 naming
		struct proc *p, *best = 0;
		for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
				// RUNNABLE인 프로세스만 고려한다
				if(p->state != RUNNABLE) 
						continue;
				
				// pass 최솟값 갖는 프로세스를 선정, 동률이면 pid 작은 것을 선택
				if(best == 0 || p->pass < best->pass || (p->pass == best->pass && p->pid < best->pid)){
						best = p;
				}
		}
		return best;
}

// 프로세스들의 pass 값 안정성(숫자 안정성)을 위해 정규화하는 함수
// distance cutting, 정규화의 기능을 한다
static void rebase_passes(void){
		// 0xFFFFFFFF은 <limits.h>의 UINT_MAX와 같다
		// unsigned int의 최대값인데, 이는 아직 최소값을 못 찾은 상태를 표시하는 플래그 역할을 한다
		uint min_pass = 0xFFFFFFFF;
		struct proc *p;

		// 1) RUNNABLE 중 최소 pass 찾기
		for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
				// RUNNABLE인 프로세스만 고려한다
				if(p->state != RUNNABLE)
						continue;
				if(p->pass < min_pass)
						min_pass = p->pass;
		}

		if(min_pass == 0xFFFFFFFF)
				return;   //RUNNABLE 없음
		
		cprintf("\nRebase Process Start\n\n");

		// 2) 모든 RUNNABLE 표준화 + distance cutting
		for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
				if(p->state != RUNNABLE)
						continue;        // RUNNABLE 아니면 고려하지 않는다

				uint old = p->pass;
				uint newp = old - min_pass;   // 가장 작은 프로세스의 pass만큼 빼기
				
				// 정규환한 프로세스의 pass값이 DISTANCE_MAX의 값을 넘으면, 해당 프로세스의 pass값을 DISTANCE_MAX값으로 바꿔버린다.(clamp)
				// 이는 모든 프로세스 간의 pass값의 차이가 DISTANCE_MAX 값을 넘기지 않음을 의미한다.
				// 왜냐하면 정규화 후에는 가장 작은 pass값이 0이고, 따라서 어떤 프로세스의 pass값이 DISTANCE_MAX 이상만 아니라면, 모든 프로세스 간의 pass값 차이가 DISTANCE_MAX 이하로 형성되기 때문이다.
				if(newp > DISTANCE_MAX){
						cprintf("Process %d's pass is standardize from %d, with distance cutting, to %d\n", p->pid, (int)old, (int)DISTANCE_MAX);
						p->pass = DISTANCE_MAX;
				} else {
				// Distance Cutting 상황이 아니라면, 원래대로 정규화만 진행한다.
						if(old != newp) {
								cprintf("Process %d's pass is standardize from %d, to %d\n", p->pid, (int)old, (int)newp);
						}
						p->pass = newp;
				}
		}
		cprintf("\nRebase Process End\n\n");
}


