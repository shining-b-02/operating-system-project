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
sys_hello_number(void){
	int n;
	if(argint(0, &n) < 0)
		return -1;
	cprintf("hello xv6! Your number is %d\n", n);
	return 2*n;
}

struct k_procinfo { int pid, ppid, state; uint sz; char name[16]; };

int sys_get_procinfo(void) {
  int pid;
  char *uaddr;                 // user buffer addr
  struct proc *p, *t;
  struct k_procinfo kinfo;

  if (argint(0, &pid) < 0) return -1;
  if (argptr(1, &uaddr, sizeof(kinfo)) < 0) return -1;

  acquire(&ptable.lock);
  if (pid <= 0) {
    t = myproc();
  } else {
    t = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
      if (p->pid == pid) { t = p; break; }
    }
  }
  if (t == 0 || t->state == UNUSED) {
    release(&ptable.lock);
    return -1;
  }

  // 채우기
  kinfo.pid   = t->pid;
  kinfo.ppid  = (t->parent) ? t->parent->pid : 0;
  kinfo.state = t->state;
  kinfo.sz    = t->sz;
  safestrcpy(kinfo.name, t->name, sizeof(kinfo.name));
  release(&ptable.lock);

  // 유저 공간으로 복사
  if (copyout(myproc()->pgdir, (uint)uaddr, (void*)&kinfo, sizeof(kinfo)) < 0)
    return -1;
  return 0;
}


