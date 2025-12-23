#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define MAX_FRINFO 60000

static void
usage(void)
{
  printf(1, "usage: memdump [-a] [-p PID]\n");
  exit();
}

int
main(int argc, char *argv[])
{
  int show_all = 0;      // -a가 있을 때만 free 프레임까지 표시
  int filter_pid = -1;   // -p로 지정된 PID만 표시(기본: 필터 없음)

  if (argc == 1) usage();

  // 옵션 파싱
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-a")) {
      show_all = 1;
    } else if (!strcmp(argv[i], "-p")) {
      if (i + 1 >= argc) usage();
      filter_pid = atoi(argv[++i]);
    } else {
      usage();
    }
  }

  static struct physframe_info buf[MAX_FRINFO];
  int n = dump_physmem_info((void *)buf, MAX_FRINFO);
  if (n < 0) {
    printf(1, "memdump: dump_physmem_info failed\n");
    exit();
  }

  printf(1, "[memdump] pid=%d\n", getpid());
  printf(1, "[frame#]\t[alloc]\t[pid]\t[start_tick]\n");

  for (int i = 0; i < n; i++) {
    int alloc = buf[i].allocated;
    int pid   = buf[i].pid;

    // 기본은 "할당된 것만" 출력, -a면 free까지 포함
    if (!show_all && alloc == 0) continue;

    // -p가 있으면 해당 PID만 출력(보통 alloc==1인 것만 의미 있음)
    if (filter_pid != -1 && pid != filter_pid) continue;

    printf(1, "%d\t\t%d\t%d\t%d\n",
           buf[i].frame_index, alloc, pid, buf[i].start_tick);
  }

  exit();
}

