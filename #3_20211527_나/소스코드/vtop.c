#include "types.h"
#include "stat.h"
#include "user.h"

static uint parse_hex(const char *p){
  uint x=0; if(p[0]=='0'&&(p[1]=='x'||p[1]=='X')) p+=2;
  for(;*p;p++){ char c=*p; int v;
    if(c>='0'&&c<='9') v=c-'0';
    else if(c>='a'&&c<='f') v=c-'a'+10;
    else if(c>='A'&&c<='F') v=c-'A'+10;
    else break;
    x=(x<<4)|(uint)v;
  } return x;
}

static void usage(void){
  printf(1,"usage: vtop <hex_va> | -s | -a N [-r M]\n");
  exit();
}

int main(int argc, char **argv){
  uint va=0; int pages=0, repeat=1; int i=1;
  if(argc<2) usage();

  // 모드 선택
  if(!strcmp(argv[i],"-s")){
    volatile int dummy=123;      // 최적화 방지
    va=(uint)&dummy; i++;
  } else if(!strcmp(argv[i],"-a")){
    if(i+1>=argc) usage();
    pages=atoi(argv[i+1]); if(pages<=0) usage(); i+=2;
    char *base=sbrk(pages*4096);
    if(base==(char*)-1){ printf(1,"vtop: sbrk failed\n"); exit(); }
    for(int k=0;k<pages;k++) base[k*4096]=(char)k; // 실제 할당
    printf(1,"[vtop] base=0x%x pages=%d\n",(uint)base,pages);
    va=(uint)base;
  } else {
    va=parse_hex(argv[i]); i++;
  }

  // 반복 횟수
  if(i+1<=argc && !strcmp(argv[i],"-r")){
    if(i+1>=argc) usage();
    repeat=atoi(argv[i+1]); if(repeat<=0) repeat=1;
  }

  for(int r=1;r<=repeat;r++){
    uint pa=0, flags=0, hits=0, misses=0;
    if(vtop((void*)va,&pa,&flags)<0){
      printf(1,"vtop: not present (VA=0x%x)\n",va);
      exit();
    }
    tlbstat(&hits,&misses);
    printf(1,"[%d] VA=0x%x -> PA=0x%x flags=0x%x  TLB[hits=%d misses=%d]\n",
           r, va, pa, flags, hits, misses);
  }
  exit();
}

