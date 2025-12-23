#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

// xv6 FS 상수(커널 fs.h 값과 동일)
#define NDIRECT   12
#define NINDIRECT 128
#define MAXA      (NDIRECT + 1 + NINDIRECT)

// 소문자 16진수로 출력
static void print_hex_lower(uint x){
  if(x == 0){ printf(1, "0"); return; }
  char tmp[16];
  int i = 0;
  const char *D = "0123456789abcdef";
  while(x && i < (int)sizeof(tmp)){
    tmp[i++] = D[x & 0xF];
    x >>= 4;
  }
  while(--i >= 0) printf(1, "%c", tmp[i]);
}

int
main(int argc, char *argv[]){
  if(argc != 2){
    printf(1,"Usage: print_addr <file>\n");
    exit();
  }

  uint addrs[MAXA];
  int n = get_file_block_addrs(argv[1], addrs, MAXA);
  if(n < 0){
    printf(1,"error\n");
    exit();
  }

  // direct
  for(int i = 0; i < NDIRECT && i < n; i++){
    if(addrs[i]){
      printf(1, "addr[%d] : ", i);
      print_hex_lower(addrs[i]);
      printf(1, "\n");
    }
  }

  // indirect pointer block itself
  if(n > NDIRECT){
    uint ib = addrs[NDIRECT];
    if(ib){
      printf(1, "addr[%d] : ", NDIRECT);
      print_hex_lower(ib);
      printf(1, " (INDIRECT POINTER)\n");
    }

    // indirect entries
    int idx = 0;
    for(int j = NDIRECT + 1; j < n; j++, idx++){
      if(addrs[j]){
        int bn = NDIRECT + idx; // 논리 블록 번호
        printf(1, "addr[%d] -> [%d] (bn : %d) : ", NDIRECT, idx, bn);
        print_hex_lower(addrs[j]);
        printf(1, "\n");
      }
    }
  }

  exit();
}

