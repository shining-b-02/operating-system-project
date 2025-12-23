#include "types.h"
#include "stat.h"
#include "user.h"

static uint parse_hex(const char *p){
  uint x = 0;
  if(p[0]=='0' && (p[1]=='x' || p[1]=='X')) p += 2;
  while(*p){
    char c = *p++;
    int v;
    if(c>='0' && c<='9') v = c - '0';
    else if(c>='a' && c<='f') v = c - 'a' + 10;
    else if(c>='A' && c<='F') v = c - 'A' + 10;
    else break;
    x = (x<<4) | (uint)v;
  }
  return x;
}

static void usage(void){
  printf(1, "usage: pfind <hex_pa_page>\n");
  exit();
}

int main(int argc, char **argv){
  if(argc != 2) usage();
  uint pa_page = parse_hex(argv[1]) & ~0xFFF; // 페이지 정렬

  struct vref buf[64];
  int n = phys2virt(pa_page, buf, 64);
  if(n < 0){
    printf(1, "pfind: syscall failed\n"); exit();
  }
  printf(1, "PA_PAGE=0x%x -> %d refs\n", pa_page, n);
  for(int i=0;i<n;i++){
    printf(1, "  (pid=%d, va=0x%x, flags=0x%x)\n", buf[i].pid, buf[i].va, buf[i].flags);
  }
  exit();
}

