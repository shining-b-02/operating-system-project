typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;
typedef uint pde_t;

struct physframe_info {
  uint  frame_index;   // PFN
  int   allocated;     // 1: in use, 0: free
  int   pid;           // owner pid, kernel/none: -1
  uint  start_tick;    // first-use tick
};

struct vref {
  uint pid;    // 소유 PID
  uint va;     // 페이지 단위 가상주소 (PGROUNDDOWN)
  uint flags;  // PTE 권한 스냅샷
};
