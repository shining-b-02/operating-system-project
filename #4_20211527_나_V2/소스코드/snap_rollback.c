#include "types.h"
#include "user.h"

int
main(int argc, char *argv[])
{
  if(argc != 2){
    printf(1, "usage: snap_rollback <id>\n");
    exit();
  }
  int id = atoi(argv[1]);
  int r = snapshot_rollback(id);
  if(r < 0){
    printf(1, "rollback failed (no such snapshot)\n");
  }
  exit();
}

