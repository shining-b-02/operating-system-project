#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

static void
usage(void) {
  printf(1, "usage: memstress [-n pages] [-t ticks] [-w]\n");
  exit();
}

int
main(int argc, char *argv[])
{
  int pages = 10;          // 기본: 10 페이지
  int hold_ticks = 200;    // 기본: 200 ticks 유지
  int do_write = 0;        // 기본: 쓰기 미수행

  // 옵션 파싱
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-n")) {
      if (i + 1 >= argc) usage();
      pages = atoi(argv[++i]);
      if (pages <= 0) usage();
    } else if (!strcmp(argv[i], "-t")) {
      if (i + 1 >= argc) usage();
      hold_ticks = atoi(argv[++i]);
      if (hold_ticks < 0) usage();
    } else if (!strcmp(argv[i], "-w")) {
      do_write = 1;
    } else {
      usage();
    }
  }

  int pid = getpid();
  printf(1, "[memstress] pid=%d pages=%d hold=%d ticks write=%d\n",
         pid, pages, hold_ticks, do_write);

  // 메모리 확보
  int inc = pages * 4096;
  char *base = sbrk(inc);
  if (base == (char*)-1) {
    printf(1, "[memstress] sbrk failed\n");
    exit();
  }

  // 실제 접근(페이지 당 1바이트)으로 물리 할당 유도
  if (do_write) {
    for (int p = 0; p < pages; p++) {
      base[p * 4096] = (char)(p & 0xff);
    }
  }

  // 유지 시간
  sleep(hold_ticks);

  printf(1, "[memstress] pid=%d done\n", pid);
  exit();
}

