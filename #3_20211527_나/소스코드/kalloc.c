// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"
#include "proc.h"
#include "date.h"

extern int pf_ready; 

// ---- 전역 프레임 추적 테이블 ----
#define PFNNUM 60000                // 테이블 크기
const int PFN_TABLE_SIZE = PFNNUM;  // 다른 파일에서 extern으로 참조할 변수 이름
struct physframe_info pf_info[PFNNUM];

static inline uint pa_to_pfn(uint pa){ return pa >> 12; }

static inline void pf_reset(uint pfn){
  if(pfn >= PFNNUM) return;
  pf_info[pfn].frame_index = pfn;
  pf_info[pfn].allocated = 0;
  pf_info[pfn].pid = -1;
  pf_info[pfn].start_tick = 0;
}

// 초기화 루틴(한 번만 호출)
static void pfinfo_init_once(void){
  for(int i=0;i<PFNNUM;i++){
    pf_info[i].frame_index = i;
    pf_info[i].allocated = 0;
    pf_info[i].pid = -1;
    pf_info[i].start_tick = 0;
  }
}

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  pfinfo_init_once();              // pf_info[] 전체 초기화는 여기서 한 번만
  freerange(vstart, vend);
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE)
    kfree(p);
}
//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(char *v)
{
  struct run *r;

  if((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  // poison & freelist 삽입
  memset(v, 1, PGSIZE);
  r = (struct run*)v;

  if(kmem.use_lock) acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  if(kmem.use_lock) release(&kmem.lock);
}
// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
extern uint ticks;
extern struct spinlock tickslock;

char*
kalloc(void)
{
  struct run *r;

  if(kmem.use_lock) acquire(&kmem.lock);
  r = kmem.freelist;
  if(r) kmem.freelist = r->next;
  if(kmem.use_lock) release(&kmem.lock);

  if(r){
    uint pa  = V2P((char*)r);
    uint pfn = pa_to_pfn(pa);

    if(!kmem.use_lock){
      // ----- 부팅 초기: CPU/틱 미초기화, 안전하게 최소 기록만 -----
      pf_info[pfn].allocated  = 1;
      pf_info[pfn].pid        = -1;   // 아직 소유 프로세스 개념 없음
      pf_info[pfn].start_tick = 0;
      pf_info[pfn].frame_index= pfn;
    } else {
      // ----- 정상 운행 이후 -----
      acquire(&kmem.lock);
      pf_info[pfn].allocated = 1;
      struct proc *p = myproc();          // 이제 안전
      pf_info[pfn].pid = p ? p->pid : -1;

      acquire(&tickslock);
      pf_info[pfn].start_tick = ticks;
      release(&tickslock);

      pf_info[pfn].frame_index = pfn;
      release(&kmem.lock);
    }
  }
  return (char*)r;
}

