// projtest.c — vtop/pfind 일괄 검증
#include "types.h"
#include "stat.h"
#include "user.h"
#include "mmu.h"

#ifndef PGSIZE
#define PGSIZE 4096
#endif

static void to_hex(uint x, char *b){
  static const char *D="0123456789abcdef";
  b[0]='0'; b[1]='x';
  for(int i=0;i<8;i++){ int s=(7-i)*4; b[2+i]=D[(x>>s)&0xF]; }
  b[10]=0;
}

static void run(const char *prog, char *const argv[]){
  int pid=fork();
  if(pid<0){ printf(1,"fork fail\n"); return; }
  if(pid==0){ exec(prog, argv); printf(1,"exec %s failed\n", prog); exit(); }
  wait();
}

int
main(void)
{
  printf(1,"\n==== projtest: vtop/pfind 통합 검증 ====\n");

  // (A) STLB hit/miss
  printf(1,"\n[A] STLB hit/miss 확인\n");
  char *a1[]={(char*)"vtop",(char*)"-s",(char*)"-r",(char*)"3",0};
  run("vtop", a1);

  // (B) remap/unmap → IPT/STLB
  printf(1,"\n[B] remap/unmap → IPT/STLB 일관성\n");
  char *base = sbrk(PGSIZE*2);
  base[0]=1; base[PGSIZE]=2;

  uint pa=0, fl=0;
  if(vtop(base,&pa,&fl)<0){ printf(1,"vtop syscall fail\n"); exit(); }
  char va_hex[16], pfn_hex[16];
  to_hex((uint)base, va_hex);
  to_hex(pa & ~(PGSIZE-1), pfn_hex);
  printf(1,"[B] VA=%s  PFN=%s  flags=0x%x\n", va_hex, pfn_hex, fl);

  char *v1[]={(char*)"vtop",va_hex,0};
  char *p1[]={(char*)"pfind",pfn_hex,0};
  run("vtop",  v1);
  run("pfind", p1);

  sbrk(-PGSIZE*2);
  printf(1,"[B] 해제 후 재검증\n");
  run("pfind", (char*[]){(char*)"pfind",pfn_hex,0});
  run("vtop",  (char*[]){(char*)"vtop",va_hex,0});

  // (C) COW 공유 → 분리 → exit 정리
  printf(1,"\n[C] COW 공유→쓰기 분리→exit 정리\n");
  char *p = sbrk(PGSIZE);
  p[0]=7;
  uint opa=0, ofl=0; vtop(p,&opa,&ofl);
  char ofn[16]; to_hex(opa & ~(PGSIZE-1), ofn);
  printf(1,"[C] 원래 PFN=%s\n", ofn);

  int cpid=fork();
  if(cpid==0){
    // 자식: 잠깐 대기 후 쓰기 → COW
    sleep(20);
    p[0]=99;
    uint npa=0, nfl=0; vtop(p,&npa,&nfl);
    char nfn[16]; to_hex(npa & ~(PGSIZE-1), nfn);
    printf(1,"[child] 쓰기 후 PFN=%s (원래와 달라야 정상)\n", nfn);
    sleep(30);
    exit();
  }

  // 부모: COW 전 2 refs
  run("pfind", (char*[]){(char*)"pfind",ofn,0});

  // 자식이 쓰기한 뒤: 1 ref
  sleep(30);
  printf(1,"[parent] 자식 쓰기 이후\n");
  run("pfind", (char*[]){(char*)"pfind",ofn,0});

  // 자식 종료 대기
  wait();
  printf(1,"[parent] 자식 exit 이후\n");
  run("pfind", (char*[]){(char*)"pfind",ofn,0});

  // 안전 처리: 부모 페이지 즉시 해제 후,
  //    최종 pfind는 별도 프로세스가 exec로 수행
  sbrk(-PGSIZE);
  printf(1,"[parent] 부모 해제 이후(최종)\n");
  run("pfind", (char*[]){(char*)"pfind",ofn,0});

  printf(1,"\n==== projtest done ====\n");
  exit();
}

