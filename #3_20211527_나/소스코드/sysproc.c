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
  int use_lock;
  struct run *freelist;
} kmem;

int sys_dump_physmem_info(void);

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

extern struct physframe_info pf_info[]; // 전역 테이블
extern const int PFN_TABLE_SIZE;

int
sys_dump_physmem_info(void)
{
  char *uaddr;
  int maxe;
  if(argptr(0, &uaddr, sizeof(struct physframe_info*)) < 0) return -1;
  if(argint(1, &maxe) < 0) return -1;

  if(maxe <= 0) return 0;
  if (maxe > PFN_TABLE_SIZE) maxe = PFN_TABLE_SIZE;
  // 스냅샷 일관성 보장: kmem.lock으로 보호
  acquire(&kmem.lock);
  int n = maxe;
  // 연속 복사
  int ok = copyout(myproc()->pgdir, (uint)uaddr,
                   (void*)pf_info, sizeof(struct physframe_info)*n);
  release(&kmem.lock);
  if(ok < 0) return -1;
  return n;
}

// vtop: 현재 프로세스 pgdir 기준
int sys_vtop(void){
  char *u_va;
  char *u_pa_out, *u_flags_out;
  if(argptr(0, &u_va, 1) < 0) return -1;
  if(argptr(1, &u_pa_out, sizeof(uint)) < 0) return -1;
  if(argptr(2, &u_flags_out, sizeof(uint)) < 0) return -1;

  uint va = (uint)u_va;
  uint pa, flags;

  int pid = myproc() ? myproc()->pid : -1;
  uint va_page = PGROUNDDOWN(va);

  // Soft TLB 조회 → 미스면 sw_vtop 수행 후 채움
  uint pa_page;
  if(!stlb_lookup(pid, va_page, &pa_page, &flags)){
    if(sw_vtop(myproc()->pgdir, (void*)va, &pa, &flags) < 0) return -1;
    pa_page = pa & ~0xFFF;
    stlb_fill(pid, va_page, pa_page, flags);
  }else{
    pa = pa_page | (va & 0xFFF);
  }

  if(copyout(myproc()->pgdir, (uint)u_pa_out, (char*)&pa, sizeof(pa)) < 0) return -1;
  if(copyout(myproc()->pgdir, (uint)u_flags_out, (char*)&flags, sizeof(flags)) < 0) return -1;
  return 0;
}

// phys2virt: pfn 역질의
int sys_phys2virt(void){
  int pa_page, max;
  char *u_out;

  if(argint(0, &pa_page) < 0) return -1;
  if(argint(2, &max) < 0) return -1;     // ← 먼저 max를 읽는다
  if(max <= 0) return 0;
  if(max > 64) max = 64;

  // 이제 max를 반영해 사용자 버퍼 검증
  if(argptr(1, &u_out, sizeof(struct vref) * max) < 0) return -1;

  struct vref kbuf[64];
  int n = ipt_query((uint)pa_page >> 12, kbuf, max);
  if(n <= 0) return 0;

  // --- 유저 페이지(PTE_U)만 남기기: in-place compact ---
  int m = 0;
  for(int i = 0; i < n; i++){
    if(kbuf[i].flags & PTE_U)      // 유저 비트 있는 것만
      kbuf[m++] = kbuf[i];         //   앞으로 땡겨서 보관
  }

  // m <= max 이므로 사용자 버퍼 검증은 기존 max 기준이면 충분
  int bytes = sizeof(struct vref) * m;
  if(copyout(myproc()->pgdir, (uint)u_out, (char*)kbuf, bytes) < 0) return -1;
  return m;                         // 필터링 후 개수 반환
}

// tlbstat: 히트/미스 통계
int sys_tlbstat(void){
  char *u_hits, *u_misses;
  if(argptr(0, &u_hits, sizeof(uint)) < 0) return -1;
  if(argptr(1, &u_misses, sizeof(uint)) < 0) return -1;
  uint h, m; stlb_stats(&h, &m);
  if(copyout(myproc()->pgdir, (uint)u_hits, (char*)&h, sizeof(h)) < 0) return -1;
  if(copyout(myproc()->pgdir, (uint)u_misses, (char*)&m, sizeof(m)) < 0) return -1;
  return 0;
}
