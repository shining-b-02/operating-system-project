#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[]) {
  int res = hello_number(5);
  printf(1, "hello_number(5) returned %d\n", res);
  
 // int res2 = hello_number(-7);
 // printf(1, "hello_number(-7) returned %d\n", res2);
  exit();
}

