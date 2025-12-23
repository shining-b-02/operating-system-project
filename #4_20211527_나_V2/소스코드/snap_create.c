#include "types.h"
#include "user.h"

static void print2(int n) {
  // 00~99 두 자리로 출력
  char s[3];
  s[0] = '0' + (n/10)%10;
  s[1] = '0' + (n%10);
  s[2] = 0;
  printf(1, "%s", s);
}

int main(void){
  int id = snapshot_create();
  if(id < 0) {
    printf(1, "snap_create: failed\n");
  } else {
    printf(1, "snapshot id = ");
    print2(id);
    printf(1, "\n");
  }
  exit();
}
