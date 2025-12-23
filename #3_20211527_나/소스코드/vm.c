#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"
#include "spinlock.h"

__attribute__((weak)) void pf_reset(uint pfn) { /* no-op; 실제 구현 있으면 그것이 링크됨 */ }

int pf_ready = 0;          // pf 서브시스템 준비 여부 플래그 (전역 정의)
void pf_mark_ready(void) { // 부팅 시 1로 세팅
  pf_ready = 1;
}

extern volatile uint *lapic;   // lapic.c에 정의됨

static inline int safe_curpid(void) {
  // mpinit() 이전엔 lapic이 0 → 절대 myproc()을 부르지 말고 -1(커널)로
  if(lapic == 0) return -1;
  struct proc *p = myproc();
  return p ? p->pid : -1;
}

// ---------- (A) Software Page Walker ----------
int
sw_vtop(pde_t *pgdir, const void *va, uint *pa_out, uint *flags_out)
{
  uint a = (uint)va;
  pde_t pde = pgdir[PDX(a)];
  if(!(pde & PTE_P)) return -1;
  pte_t *pgtab = (pte_t*)P2V(PTE_ADDR(pde));
  pte_t pte = pgtab[PTX(a)];
  if(!(pte & PTE_P)) return -1;

  uint pa_page = PTE_ADDR(pte);
  uint pa      = pa_page | (a & 0xFFF);

  if(pa_out)     *pa_out     = pa;
  if(flags_out)  *flags_out  = PTE_FLAGS(pte);
  return 0;
}

// ---------- (B) Soft TLB (direct-mapped) ----------
#define STLB_BITS   9
#define STLB_SIZE   (1<<STLB_BITS)
struct stlb_entry { uint pid, va_page, pa_page, flags; };
static struct stlb_entry stlb[STLB_SIZE];
static struct spinlock stlblk;
static uint tlb_hits, tlb_misses;

static inline uint stlb_idx(uint pid, uint va_page){
  return (pid*1315423911u ^ va_page) & (STLB_SIZE-1);
}

void stlb_init(void){
  initlock(&stlblk, "stlb");
  memset(stlb, 0, sizeof(stlb));
  tlb_hits = tlb_misses = 0;
}

int stlb_lookup(int pid, uint va_page, uint *pa_page, uint *flags){
  acquire(&stlblk);
  uint i = stlb_idx(pid, va_page);
  if(stlb[i].pid==pid && stlb[i].va_page==va_page){
    if(pa_page) *pa_page = stlb[i].pa_page;
    if(flags)   *flags   = stlb[i].flags;
    tlb_hits++;
    release(&stlblk);
    return 1;
  }
  tlb_misses++;
  release(&stlblk);
  return 0;
}

void stlb_fill(int pid, uint va_page, uint pa_page, uint flags){
  acquire(&stlblk);
  uint i = stlb_idx(pid, va_page);
  stlb[i].pid = pid; stlb[i].va_page = va_page;
  stlb[i].pa_page = pa_page; stlb[i].flags = flags;
  release(&stlblk);
}

void stlb_stats(uint *hits, uint *misses){
  acquire(&stlblk);
  if(hits)   *hits   = tlb_hits;
  if(misses) *misses = tlb_misses;
  release(&stlblk);
}

void stlb_invalidate_va(int pid, uint va_page){
  acquire(&stlblk);
  uint i = stlb_idx(pid, va_page);
  if(stlb[i].pid==pid && stlb[i].va_page==va_page){
    stlb[i].pid=0; stlb[i].va_page=0; stlb[i].pa_page=0; stlb[i].flags=0;
  }
  release(&stlblk);
}

void stlb_update_flags(int pid, uint va_page, uint newflags){
  acquire(&stlblk);
  uint i = stlb_idx(pid, va_page);
  if(stlb[i].pid==pid && stlb[i].va_page==va_page)
    stlb[i].flags = newflags;
  release(&stlblk);
}

void stlb_purge_pid(int pid){
  acquire(&stlblk);
  for(int i=0;i<STLB_SIZE;i++)
    if(stlb[i].pid==pid)
      stlb[i].pid=0, stlb[i].va_page=0, stlb[i].pa_page=0, stlb[i].flags=0;
  release(&stlblk);
}


// ---------- (C) IPT: 해시 + 풀알로케이터 ----------
#define IPT_BUCKETS  8192
struct ipt_entry{
  uint pfn;       // 물리 프레임 번호
  uint pid;       // 소유 PID (-1: 커널)
  uint va;        // 페이지 기준 VA
  uint flags;     // PTE 권한 스냅샷
  uint refcnt;    // (옵션)
  struct ipt_entry *next;
};

static struct ipt_entry *ipt_hash[IPT_BUCKETS];
static struct spinlock iptlk;

#define IPT_POOL_SIZE  65536
static struct ipt_entry ipt_pool[IPT_POOL_SIZE];
static struct ipt_entry *ipt_free;

static inline uint ipt_h(uint pfn){ return pfn & (IPT_BUCKETS-1); }

static int ipt_ready = 0;   // mpinit() 이후 ipt_init()이 켜줌

void ipt_init(void){
  initlock(&iptlk, "ipt");
  memset(ipt_hash, 0, sizeof(ipt_hash));
  for(int i=0;i<IPT_POOL_SIZE-1;i++) ipt_pool[i].next = &ipt_pool[i+1];
  ipt_pool[IPT_POOL_SIZE-1].next = 0;
  ipt_free = &ipt_pool[0];
  ipt_ready = 1;                   // 여기서 활성화
}

static struct ipt_entry* ipt_alloc_ent(void){
  if(!ipt_free) return 0;
  struct ipt_entry *e = ipt_free;
  ipt_free = e->next;
  return e;
}

static void ipt_free_ent(struct ipt_entry *e){
  e->next = ipt_free; ipt_free = e;
}

void ipt_insert(uint pfn, int pid, uint va_page, uint flags){
  if(!ipt_ready) return;           // 부팅 초기에는 그냥 스킵
  acquire(&iptlk);
  struct ipt_entry *e = ipt_alloc_ent();
  if(e){
    e->pfn=pfn; e->pid=pid; e->va=va_page; e->flags=flags; e->refcnt=1;
    uint b = ipt_h(pfn);
    e->next = ipt_hash[b];
    ipt_hash[b] = e;
  }
  release(&iptlk);
}

// --- vm.c: IPT에서 (pid, va_page, pfn) 하나 제거 ---
//   PTE/물리프레임은 절대 건드리지 않는다(프레임 해제는 deallocuvm가 담당).
//   va_page는 반드시 PGROUNDDOWN 한 값으로 비교.
int
ipt_remove(int pid, uint va_page, uint pfn)
{
  if (!ipt_ready) return 0;

  va_page = PGROUNDDOWN(va_page);

  int removed = 0;
  acquire(&iptlk);

  uint b = ipt_h(pfn);
  struct ipt_entry **pp = &ipt_hash[b], *cur;

  while ((cur = *pp)) {
    if (cur->pid == pid && cur->va == va_page && cur->pfn == pfn) {
      *pp = cur->next;          // unlink
      //  엔트리 메타만 free (물리 프레임 kfree 절대 금지)
      ipt_free_ent(cur);
      removed = 1;
      break;
    }
    pp = &cur->next;
  }

  release(&iptlk);
  return removed;
}

int ipt_query(uint pfn, struct vref *kbuf, int max){
  if(!ipt_ready) return 0;         // 준비 전이면 결과 없음
  int n=0;
  acquire(&iptlk);
  struct ipt_entry *e;
  for (e = ipt_hash[ipt_h(pfn)]; e && n < max; e = e->next) {
    if (e->pfn != pfn) continue;
    kbuf[n].pid = e->pid;
    kbuf[n].va  = e->va;
    kbuf[n].flags = e->flags;
    n++;
  }
  release(&iptlk);
  return n;
}

int ipt_update_flags(int pid, uint va_page, uint pfn, uint newflags){
  if(!ipt_ready) return 0;
  int updated = 0;
  acquire(&iptlk);
  struct ipt_entry *e;
    
  for (e = ipt_hash[ipt_h(pfn)]; e; e = e->next) {
    if (e->pid == pid && e->va == va_page && e->pfn == pfn) {
        e->flags = newflags;
        updated = 1;
        break;
    }
  }
  release(&iptlk);
  return updated;
}

void
ipt_purge_pid(int pid)
{
  if (!ipt_ready) return;

  acquire(&iptlk);
  for (int b = 0; b < IPT_BUCKETS; b++) {
    struct ipt_entry **pp = &ipt_hash[b], *e;
    while ((e = *pp)) {
      if (e->pid == pid) {
        *pp = e->next;       // unlink
        ipt_free_ent(e);     //  메타만 free
        continue;            //  다음 노드 검사(건너뛰기 방지)
      }
      pp = &e->next;
    }
  }
  release(&iptlk);
}
// pfn(= 물리 프레임 번호) 의 현재 IPT 참조 개수 반환
int ipt_refcount(uint pfn) {
  struct vref tmp[64];
  int total = 0;
  // ipt_query(pfn, buf, max) 는 최대 'max'개를 채워주고 실제 개수를 리턴한다고 가정
  int n = ipt_query(pfn, tmp, 64);
  if(n > 0) total += n;
  return total;
}
//----------------------------------------------------------------------

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.

// ---------- (D) vm.c의 기존 경로에 IPT/SoftTLB 연동 ----------
// mappages(): 매핑 생성 시 IPT에 삽입
int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;
  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;

	// ---- IPT 삽입 (유저 매핑만) ----
	uint flags   = PTE_FLAGS(*pte);
	if (flags & PTE_U) {                 // 커널 매핑(PTE_U==0)은 건너뜀
  		int  pid     = safe_curpid();      // 부팅 초기엔 -1이 나오도록 이미 안전
  		uint va_page = (uint)a;            // a는 이미 PGROUNDDOWN 된 값
  		uint pfn     = (pa >> 12);
  		ipt_insert(pfn, pid, va_page, flags);
	}

    if(a == last) break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}
// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();

  extern void stlb_init(void);
  extern void ipt_init(void);

  static int inited = 0;
  if (!inited) {
    stlb_init();
    ipt_init();
    pf_mark_ready();     //  이 시점 이후에만 kfree가 pf_reset 수행
    inited = 1;
  }
}
// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
// vm.c
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;

  if (newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for (; a < oldsz; a += PGSIZE) {
    pte = walkpgdir(pgdir, (char*)a, 0);
    if (!pte) { a = PGADDR(PDX(a)+1,0,0) - PGSIZE; continue; }
    if ((*pte & PTE_P) == 0) continue;

    pa = PTE_ADDR(*pte);
    if (pa == 0) panic("deallocuvm kfree");

    uint va_page = PGROUNDDOWN(a);
    uint pfn     = pa >> 12;

    // 현재 실행중인 주소공간인지 확인 (부모가 자식 pgdir을 free하는 경우가 많음)
    int is_cur_pgdir = (myproc() && myproc()->pgdir == pgdir);
    int pid_cur = safe_curpid(); // 현재 프로세스 pid (is_cur_pgdir일 때만 사용)

    // 1) 먼저 PTE를 죽인다
    *pte = 0;

    // 2) 현재 주소공간일 때만 HW TLB flush 및 STLB/IPT 조작
    if (is_cur_pgdir) {
      lcr3(V2P(pgdir));                   // HW TLB flush
      stlb_invalidate_va(pid_cur, va_page);
      ipt_remove(pid_cur, va_page, pfn);  //  부모가 자식 pgdir을 free할 땐 실행하지 않음
    }

    // 3) 남은 레퍼런스가 없을 때만 프레임 해제
    if (ipt_refcount(pfn) == 0) {
      pf_reset(pfn);                      // no-op이면 무시
      kfree(P2V(pa));
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;

  // IPT flags 동기화
  int pid = safe_curpid();
  uint pa_page = PTE_ADDR(*pte);
  ipt_update_flags(pid, PGROUNDDOWN((uint)uva), pa_page>>12, PTE_FLAGS(*pte));
  stlb_update_flags(pid, PGROUNDDOWN((uint)uva), PTE_FLAGS(*pte));              // 소프트 TLB 동기화
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)P2V(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0) {
      kfree(mem);
      goto bad;
    }
  }
  return d;

bad:
  freevm(d);
  return 0;
}

// 부모 pgdir에서 va의 PTE를 찾아 동일 pa를 자식 pgdir에 공유 매핑.
// 두 쪽 모두 PTE_W를 내리고 PTE_COW를 세팅한다.
static int
cow_share_page(pde_t *pgdir_parent, pde_t *pgdir_child, uint va, int child_pid)
{
  pte_t *ppte = walkpgdir(pgdir_parent, (char*)PGROUNDDOWN(va), 0);
  if(ppte == 0 || (*ppte & PTE_P) == 0 || (*ppte & PTE_U) == 0)
    return -1;

  uint pa     = PTE_ADDR(*ppte);
  uint flags  = PTE_FLAGS(*ppte);
  uint va_page = PGROUNDDOWN(va);
  uint pfn     = pa >> 12;

  // 1) 부모 PTE를 RO + COW로
  uint cow_flags = (flags & ~PTE_W) | PTE_COW;
  *ppte = pa | cow_flags;

  // 1-1) (실행중이 부모 컨텍스트라면) 하드웨어 TLB도 무효화
  stlb_invalidate_va(safe_curpid(), va_page);   // 소프트TLB 사용

  // 2) IPT: 부모 엔트리 갱신 (없으면 새로 생성)
  int ppid = safe_curpid();   // 부모 pid
  int updated = ipt_update_flags(ppid, va_page, pfn, cow_flags);
  if(updated == 0){
    // 부모 엔트리가 없었음 → 부모 것도 만들어 둔다
    ipt_insert(pfn, ppid, va_page, cow_flags);
  }

  // 3) 자식 PTE를 직접 만든다(== mappages() 사용 금지!)
  pte_t *cpte = walkpgdir(pgdir_child, (char*)va_page, 1);  // 필요 시 PT 생성
  if(cpte == 0) return -1;
  *cpte = pa | cow_flags;                      // 동일 PFN, RO+COW

  // 4) IPT: 자식쪽 삽입
  ipt_insert(pfn, child_pid, va_page, cow_flags);

  return 0;
}

// 기존 copyuvm(실제 복사) 대신: 모든 유저 페이지를 COW 공유로 붙인다.
pde_t*
cowuvm(pde_t *pgdir_parent, uint sz, int child_pid)
{
  pde_t *d;
  if((d = setupkvm()) == 0)
    return 0;

  for(uint va = 0; va < sz; va += PGSIZE){
    pte_t *pte = walkpgdir(pgdir_parent, (char*)va, 0);
    if(pte == 0) { va = PGADDR(PDX(va)+1, 0, 0) - PGSIZE; continue; }
    if((*pte & PTE_P) == 0) continue;           // unmapped
    if((*pte & PTE_U) == 0) continue;           // 커널 전용은 공유 대상 아님
    if(cow_share_page(pgdir_parent, d, va, child_pid) < 0){ freevm(d); return 0; }
  }
  // 부모의 TLB를 한번에 싹 비움(부모 PTE가 바뀌었기 때문)
  lcr3(V2P(pgdir_parent));
  return d;
}

// faulting va에 대해 COW 처리: 새 페이지를 만들어 내용 복사 후 쓰기 가능하게 갱신
int cow_fault(pde_t *pgdir, uint va)
{
  uint va_page = PGROUNDDOWN(va);

  pte_t *pte = walkpgdir(pgdir, (char*)va_page, 0);
  if(!pte || (*pte & PTE_P) == 0) return -1;
  if((*pte & PTE_COW) == 0) return -1;

  uint old_pa   = PTE_ADDR(*pte);
  uint old_flag = PTE_FLAGS(*pte);

  char *mem = kalloc();
  if(mem == 0) return -1;
  memmove(mem, (char*)P2V(old_pa), PGSIZE);

  int pid = safe_curpid();

  // STLB/ipt에서 기존 매핑 제거
  stlb_invalidate_va(pid, va_page);
  ipt_remove(pid, va_page, old_pa >> 12);

  // 새 페이지로 재매핑: 쓰기 가능, COW 해제
  *pte = V2P(mem) | ((old_flag | PTE_W) & ~PTE_COW);
  uint nflags = PTE_FLAGS(*pte);

  // IPT에 새 프레임 삽입
  ipt_insert((V2P(mem) >> 12), pid, va_page, nflags);

  // 하드웨어 TLB flush
  lcr3(V2P(pgdir));
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

