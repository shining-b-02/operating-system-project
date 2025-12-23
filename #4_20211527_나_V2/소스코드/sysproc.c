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

///////////////////////////////////////////////////////////////////////////////////////////////////////

int
sys_snapshot_create(void)
{
  int id;
  // 비어있는 ID 찾기
  for(id = 1; id <= 99; id++){
    char buf[32];
    safestrcpy(buf, "/snapshot/", sizeof(buf));
    int len = strlen(buf);
    if(len + 2 >= sizeof(buf)) return -1;
    buf[len + 0] = '0' + (id/10)%10;
    buf[len + 1] = '0' + (id%10);
    buf[len + 2] = 0;
    if(namei(buf) == 0) break;
  }
  if(id > 99) return -1;

  int need = count_needed_inodes("/");
  int free = fs_count_free_inodes();
  if (need > free) return -1;          // 우아한 실패

  // 실제 생성
  if(snapshot_clone_tree(id) < 0) return -1;
  return id;  //  성공 시 ID를 돌려준다 (유저 프로그램에서 "snapshot id = 01" 출력됨)
}

int
sys_snapshot_rollback(void)
{
  int id;
  if(argint(0,&id) < 0) return -1;
  return snapshot_restore_from(id);
}

int
sys_snapshot_delete(void)
{
  int id;
  if(argint(0,&id) < 0) return -1;
  return snapshot_delete_tree(id);
}

// 파일 블록 주소 덤프 시스템콜 (print_addr용)
int
sys_get_file_block_addrs(void)
{
  char *path;   // 포인터!
  uint *dst;
  int max;

  // path
  if(argstr(0, &path) < 0) return -1;

  // max
  if(argint(2, &max) < 0) return -1;
  if(max <= 0) return -1;

  // dst (주의: 네 환경의 argptr 시그니처가 char** 이므로 (char**)&dst 사용)
  if(argptr(1, (char**)&dst, max * sizeof(uint)) < 0) return -1;

  // 실제 작업은 fs.c 내부 헬퍼에서 처리
  return fs_get_file_block_addrs(path, dst, max);
}
/////////////////////////////////////////////////////////////////////
