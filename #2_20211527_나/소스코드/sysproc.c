#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"

extern struct {
	struct spinlock lock;
	struct proc proc[NPROC];
} ptable;

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
sys_settickets(void)
{
  int t, e;
  if (argint(0, &t) < 0) return -1;
  if (argint(1, &e) < 0) return -1;

  // 인자 검증
  if (t < 1 || t >= STRIDE_MAX)   // 1 <= tickets < STRIDE_MAX
    return -1;                    // 잘못된 입력이면 -1

  struct proc *p = myproc();

  // 스케줄러와의 경합 최소화를 위해 잠깐 ptable 락을 잡고 갱신
  acquire(&ptable.lock);
  p->tickets = t;
  p->stride  = STRIDE_MAX / t;    // 정수 나눗셈, 버림
  if (e >= 1)                     // end_ticks는 1 이상일 때만 반영
    p->end_ticks = e;             // (그 외는 무시)
  release(&ptable.lock);

  return 0;
}
