#include "types.h"
#include "user.h"
int main(int argc, char *argv[]){
  if(argc != 2){ printf(1,"Usage: snap_delete <ID>\n"); exit(); }
  int id = atoi(argv[1]);
  int r = snapshot_delete(id);
  if(r < 0) printf(1,"delete failed\n");
  exit();
}

