#include "types.h"
#include "stat.h"
#include "user.h"

static char* s2str(int s){
  switch(s){
    case 0: return "UNUSED"; case 1: return "EMBRYO";
    case 2: return "SLEEPING"; case 3: return "RUNNABLE";
    case 4: return "RUNNING"; case 5: return "ZOMBIE";
  } return "UNKNOWN";
}

int main(int argc, char *argv[]){
  struct procinfo info;
  int pid = (argc >= 2) ? atoi(argv[1]) : 0; // 0이면 자기 자신
  if (get_procinfo(pid, &info) < 0) {
    printf(2, "psinfo: failed (pid=%d)\n", pid);
    exit();
  }
  printf(1, "PID=%d PPID=%d STATE=%s SZ=%d NAME=%s\n",
         info.pid, info.ppid, s2str(info.state), info.sz, info.name);
  exit();
}

