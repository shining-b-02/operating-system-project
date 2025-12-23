//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "buf.h"

static struct inode* create(char *path, short type, short major, short minor);
static int isdirempty(struct inode *dp);

// 스냅샷 경로 판별을 더 안전하게: 절대/상대 모두 차단
static int path_is_under_snapshot_any(const char *p) {
  if(!p) return 0;

  // 절대경로: "/snapshot" 또는 "/snapshot/..."
  if (p[0] == '/' &&
      p[1] == 's' && p[2] == 'n' && p[3] == 'a' && p[4] == 'p' &&
      p[5] == 's' && p[6] == 'h' && p[7] == 'o' && p[8] == 't' &&
      (p[9] == 0 || p[9] == '/'))
    return 1;

  // 상대경로: "snapshot" 또는 "snapshot/..."
  if (p[0] == 's' && p[1] == 'n' && p[2] == 'a' && p[3] == 'p' &&
      p[4] == 's' && p[5] == 'h' && p[6] == 'o' && p[7] == 't' &&
      (p[8] == 0 || p[8] == '/'))
    return 1;

  // "./snapshot/..." 또는 "../x/snapshot/..." 같은 수준의 단순 케이스 방지
  // 경로 어딘가에 "/snapshot/"가 들어가면 차단
  for (int i = 0; p[i]; i++) {
    if (p[i] == '/' &&
        p[i+1] == 's' && p[i+2] == 'n' && p[i+3] == 'a' && p[i+4] == 'p' &&
        p[i+5] == 's' && p[i+6] == 'h' && p[i+7] == 'o' && p[i+8] == 't' &&
        p[i+9] == '/')
      return 1;
  }
  return 0;
}

// 스냅샷에 대한 "쓰기 성격" 요청인지 판단 (open omode 전용)
static int is_write_like_open(int omode) {
  return (omode & O_WRONLY) || (omode & O_RDWR) || (omode & O_CREATE);
}

// 스냅샷 트리 판별: 디렉터리 inode에서 ".."를 따라 root까지 올라가며
// 중간에 /snapshot inode를 만나면 1, 아니면 0.
static int is_under_snapshot_dir(struct inode *d) {
  int ret = 0;
  struct inode *snap = namei("/snapshot");
  if(snap == 0) return 0;

  struct inode *cur = d;
  ilock(cur);

  struct inode *to_free = 0;
  for(;;){
    if(cur->dev == snap->dev && cur->inum == snap->inum){ ret = 1; break; }

    uint off;
    struct inode *par = dirlookup(cur, "..", &off); // unlocked inode
    if(par == 0) break;

    // root에 도달
    if(par->dev == cur->dev && par->inum == cur->inum){ iput(par); break; }

    iunlock(cur);
    if(to_free && to_free != d) iput(to_free);
    to_free = par;
    cur = par;
    ilock(cur);
  }
  iunlock(cur);
  if(to_free && to_free != d) iput(to_free);
  iput(snap);
  return ret;
}

// 경로 기반 빠른 1차 필터 + 부모 디렉터리 inode로 최종 판정
static int path_write_into_snapshot(const char *path){
  if(path == 0) return 0;

  // 1) 절대경로 /snapshot/... 빠른 차단
  if(path[0] == '/' && strncmp(path, "/snapshot", 9) == 0){
    char c = path[9];
    if(c == 0 || c == '/') return 1;
  }

  // 2) 상대경로가 root에서 시작하는 경우(현재 CWD가 /)의 "snapshot/..."
  if(myproc() && myproc()->cwd && myproc()->cwd->inum == ROOTINO){
    if(strncmp(path, "snapshot", 8) == 0){
      char c = path[8];
      if(c == 0 || c == '/') return 1;
    }
  }

  // 3) nameiparent로 실제 부모 inode를 얻어 최종 확인
  char last[DIRSIZ];
  struct inode *dp = nameiparent((char*)path, last); // unlocked
  if(dp == 0) return 0;
  int under = is_under_snapshot_dir(dp);
  iput(dp);
  return under;
}

//////////////////////////////////////////////////////////////////////////////////////////////
// 경로 이어붙이기 유틸
static void catpath(char *base, const char *seg, int max)
{
  int l = strlen(base);
  if(l < max && l > 0 && base[l-1] != '/'){
    if(l+1 < max){ base[l] = '/'; base[l+1] = 0; l++; }
  }
  // base 끝에 seg를 안전하게 복사
  safestrcpy(base + l, (char*)seg, max - l);
}


// 스냅샷 경로/ID/예외 유틸
extern void cow_incref(uint);
extern void cow_decref(uint);
extern int  cow_get_ref(uint);

int
path_is_under_snapshot(const char *path)
{
  if(path == 0) return 0;
  // "/snapshot" 또는 "/snapshot/..." 만 매치
  // strncmp 원형은 defs.h 에 있음.
  if(path[0] == '/' && strncmp(path, "/snapshot", 9) == 0){
    char c = path[9];
    return (c == 0 || c == '/');
  }
  return 0;
}

static void format_snap_id(char *buf, int id) {
  buf[0] = '0' + (id/10)%10;
  buf[1] = '0' + (id%10);
  buf[2] = 0;
}

static int ensure_snapshot_root(void)
{
  struct inode *dp = namei("/snapshot");
  if(dp){ iput(dp); return 0; }

  begin_op();
  // create(path, type, major, minor) 는 sysfile.c 내부 static이므로 호출 가능
  dp = create("/snapshot", T_DIR, 0, 0);
  if(!dp){ end_op(); return -1; }
  iunlockput(dp);
  end_op();
  return 0;
}

static int is_dev_or_snapshot_dir(struct inode *ip, const char *path)
{
  if(ip->type == T_DEV) return 1;
  if(path && path_is_under_snapshot(path)) return 1;
  return 0;
}

// 파일 inode를 “블록 공유”로 복제 (COW 참조수 올림)
static int clone_file_inode(struct inode *src, struct inode *dst)
{
  dst->type  = src->type;
  dst->major = src->major;
  dst->minor = src->minor;
  dst->nlink = 1;
  dst->size  = src->size;

  // direct
  for(int i=0;i<NDIRECT;i++){
    if(src->addrs[i]){
      dst->addrs[i] = src->addrs[i];
      cow_incref(src->addrs[i]);
    } else dst->addrs[i] = 0;
  }

  // indirect
  if(src->addrs[NDIRECT]){
    dst->addrs[NDIRECT] = src->addrs[NDIRECT];
    cow_incref(src->addrs[NDIRECT]); // 간접블록 자체

    struct buf *ib = bread(src->dev, src->addrs[NDIRECT]);
    uint *ia = (uint*)ib->data;
    for(int j=0;j<NINDIRECT;j++){
      if(ia[j]) cow_incref(ia[j]);   // 간접 항목(실제 데이터 블록)
    }
    brelse(ib);
  } else dst->addrs[NDIRECT] = 0;

  iupdate(dst);
  return 0;
}

//unlink 내부 헬퍼(경로 하나 지우기)
static int snapshot_unlink(char *path)
{
  char name[DIRSIZ];
  uint off;
  struct inode *ip, *dp;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);
  // ".", ".." 보호
  if(strncmp(name, ".",  DIRSIZ) == 0 || strncmp(name, "..", DIRSIZ) == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  struct dirent de;
  memset(&de, 0, sizeof(de));
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");

  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();
  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}
/////////////////////////////////////////////////////////////////////////////////////////////

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *curproc = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd] == 0){
      curproc->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

int
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

int
sys_read(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return fileread(f, p, n);
}

int
sys_write(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return filewrite(f, p, n);
}

int
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

int
sys_fstat(void)
{
  struct file *f;
  struct stat *st;

  if(argfd(0, 0, &f) < 0 || argptr(1, (void*)&st, sizeof(*st)) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
// link: 소스(old)나 타겟(new) 어느 쪽이든 /snapshot/* 관여 시 차단
int
sys_link(void)
{
  char name[DIRSIZ], *new, *old;
  struct inode *dp, *ip;

  if(argstr(0, &old) < 0 || argstr(1, &new) < 0)
    return -1;

  // 타겟 경로(/snapshot/*)로의 링크 생성 금지
  if(path_write_into_snapshot(new))
    return -1; // EPERM

  // 소스 경로가 스냅샷 안쪽이면(소스 inode의 nlink 변경이므로) 금지
  if(path_is_under_snapshot(old))
    return -1; // EPERM

  begin_op();

  // 소스 inode 획득
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  // 디렉토리 하드링크 금지 (xv6 기본 정책)
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  // 소스 링크 카운트 증가
  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  // 타겟의 부모 디렉토리에 엔트리 추가
  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);

  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }

  iunlockput(dp);
  iput(ip);
  end_op();
  return 0;

bad:
  // 실패 시 소스의 링크 카운트 롤백
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

// unlink: /snapshot/* 에 대한 쓰기 시도 차단(견고 버전)
int
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], *path;
  uint off;

  if(argstr(0, &path) < 0)
    return -1;

  // 스냅샷 아래는 메타데이터 변경도 금지
  if (path_is_under_snapshot_any(path))
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }
  ilock(dp);

  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");

  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();
  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;
  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && ip->type == T_FILE)
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
// 루트에서 'snapshot' 엔트리는 곧바로 건너뛰도록 보조 함수
static int is_root_snapshot_entry(const char *parent, const char name[DIRSIZ]){
  if(parent && parent[0]=='/' && parent[1]==0)
    if(strncmp((char*)name, "snapshot", DIRSIZ) == 0) return 1;
  return 0;
}

static int
clone_tree_rec(char *src_path, char *dst_path)
{
  struct inode *src = namei(src_path);
  if(!src) return -1;

  ilock(src);

  // /snapshot 트리나 T_DEV 파일은 스킵
  if(is_dev_or_snapshot_dir(src, src_path)){
    iunlockput(src);
    return 0;
  }

  if(src->type == T_DIR){
    // 1) 잠금 상태에서 엔트리 목록만 로컬로 복사
    struct { char name[DIRSIZ]; } list[64];
    int n = 0;

    struct dirent de;
    for(int off = 0; off + sizeof(de) <= src->size; off += sizeof(de)){
      if(readi(src, (char*)&de, off, sizeof(de)) != sizeof(de)) break;
      if(de.inum == 0) continue;
      if(de.name[0]=='.' && (de.name[1]==0 || (de.name[1]=='.' && de.name[2]==0))) continue;
      if(is_root_snapshot_entry(src_path, de.name)) continue;
      if(n < (int)(sizeof(list)/sizeof(list[0]))){
        memmove(list[n].name, de.name, DIRSIZ);
        n++;
      } else {
        // (xv6 기본 트리면 충분하지만) 과하면 안전하게 중단
        break;
      }
    }

    // 2) create 전에 unlock (교착 방지)
    iunlock(src);

    begin_op();
    struct inode *dst = create(dst_path, T_DIR, 0, 0);
    if(!dst){ end_op(); iput(src); return -1; }
    iunlockput(dst);
    end_op();

    // 3) 로컬 목록으로 재귀(이때 src 락 불필요)
    for(int i=0;i<n;i++){
      char child_src[MAXPATH]; safestrcpy(child_src, src_path, sizeof(child_src));
      char child_dst[MAXPATH]; safestrcpy(child_dst, dst_path, sizeof(child_dst));
      catpath(child_src, list[i].name, sizeof(child_src));
      catpath(child_dst, list[i].name, sizeof(child_dst));
      clone_tree_rec(child_src, child_dst);
    }

    iput(src);
    return 0;

  } else {
    // ---- 파일 분기 ----
    // create 전에 반드시 unlock (교착 방지)
    iunlock(src);

    begin_op();
    struct inode *dst = create(dst_path, T_FILE, 0, 0);
    if(!dst){ end_op(); iput(src); return -1; }
    // create()는 dst를 "이미 ilock 잡은 상태"로 반환함
    // ilock(dst) 다시 하면 교착

    // 원본을 잠가서 블록 공유(ref++) 설정
    ilock(src);
    clone_file_inode(src, dst);   // dst는 이미 잠겨 있음

    // 언락 순서: src → dst
    iunlock(src);
    iunlockput(dst);

    end_op();
    iput(src);
    return 0;
  }
}

int snapshot_clone_tree(int snap_id)
{
  if(ensure_snapshot_root() < 0) return -1;

  char idbuf[8]; format_snap_id(idbuf, snap_id);
  char dst_root[MAXPATH]; safestrcpy(dst_root, "/snapshot", sizeof(dst_root));
  catpath(dst_root, idbuf, sizeof(dst_root));

  return clone_tree_rec("/", dst_root);
}

// 후위순회 삭제: 파일은 ref--/free, 디렉토리는 자식부터 제거 후 자기 자신 제거
static int
delete_tree_rec(char *path)
{
  struct inode *ip = namei(path);
  if(!ip) return -1;

  ilock(ip);
  int is_dir = (ip->type == T_DIR);

  if(is_dir){
    struct dirent de;
    for(int off = 0; off + sizeof(de) <= ip->size; off += sizeof(de)){
      if(readi(ip, (char*)&de, off, sizeof(de)) != sizeof(de)) break;
      if(de.inum == 0) continue;
      if(de.name[0]=='.' && (de.name[1]==0 || (de.name[1]=='.' && de.name[2]==0))) continue;

      char child[MAXPATH]; safestrcpy(child, path, sizeof(child));
      catpath(child, de.name, sizeof(child));

      iunlock(ip);               // 재귀 전에 unlock
      delete_tree_rec(child);
      ilock(ip);                 // 복귀 후 다시 lock
    }

    iunlockput(ip);
    return snapshot_unlink(path);  // 내부에서 begin_op/end_op
  } else {
    iunlockput(ip);
    return snapshot_unlink(path);
  }
}

int snapshot_delete_tree(int snap_id)
{
  char idbuf[8]; format_snap_id(idbuf, snap_id);
  char root[MAXPATH]; safestrcpy(root, "/snapshot", sizeof(root));
  catpath(root, idbuf, sizeof(root));
  return delete_tree_rec(root);
}

// 복구: /snapshot 유지, 나머지 / 아래 깨끗이 지우고 스냅샷에서 다시 복제
static int
wipe_root_except_snapshot(void)
{
  struct inode *root = namei("/");
  if(!root) return -1;

  // 1) 루트 잠금 상태에서 이름 목록만 수집
  ilock(root);
  struct dirent de;
  enum { MAXN = 64 };
  char names[MAXN][DIRSIZ];
  int n = 0;

  for(int off = 0; off + sizeof(de) <= root->size; off += sizeof(de)){
    if(readi(root, (char*)&de, off, sizeof(de)) != sizeof(de)) break;
    if(de.inum == 0) continue;
    // "." ".." 제외
    if(de.name[0]=='.' && (de.name[1]==0 || (de.name[1]=='.' && de.name[2]==0)))
      continue;
    // "/snapshot" 보존
    if(strncmp(de.name,"snapshot",DIRSIZ)==0) continue;

    if(n < MAXN){
      memmove(names[n], de.name, DIRSIZ);
      n++;
    } else {
      // (xv6 기본 트리면 충분) 과하면 일단 중단
      break;
    }
  }
  iunlock(root);
  iput(root);

  // 2) 루트 락 없이 자식들을 하나씩 삭제
  for(int i=0;i<n;i++){
    char child[MAXPATH];
    safestrcpy(child, "/", sizeof(child));
    catpath(child, names[i], sizeof(child));  // "/<name>"
    delete_tree_rec(child);
  }

  return 0;
}

static int
restore_rec(char *src_path, char *dst_path)
{
  struct inode *src = namei(src_path);
  if(!src) return -1;

  ilock(src);
  int is_dir = (src->type == T_DIR);
  iunlock(src);  // create 전에 반드시 unlock

  if(is_dir){
    // 목적지 디렉토리 생성: 이미 있으면 스킵, "/"도 스킵
    int need_create = !((dst_path[0]=='/' && dst_path[1]==0));  // dst != "/"
    if(need_create){
      struct inode *exist = namei(dst_path);
      if(exist){
        iunlockput(exist); // 이미 있으면 OK
      }else{
        begin_op();
        struct inode *dst = create(dst_path, T_DIR, 0, 0);
        if(!dst){ end_op(); iput(src); return -1; }
        iunlockput(dst);
        end_op();
      }
    }

    // src 엔트리 목록을 잠금 상태에서 복사 → unlock → 재귀
    ilock(src);
    struct dirent de;
    enum { MAXN = 64 };
    char names[MAXN][DIRSIZ];
    int n = 0;

    for(int off=0; off + sizeof(de) <= src->size; off += sizeof(de)){
      if(readi(src,(char*)&de,off,sizeof(de)) != sizeof(de)) break;
      if(de.inum==0) continue;
      if(de.name[0]=='.' && (de.name[1]==0 || (de.name[1]=='.' && de.name[2]==0)))
        continue;
      if(n < MAXN){
        memmove(names[n], de.name, DIRSIZ);
        n++;
      } else break;
    }
    iunlock(src);

    for(int i=0;i<n;i++){
      char ss[MAXPATH]; safestrcpy(ss, src_path, sizeof(ss));
      char dd[MAXPATH]; safestrcpy(dd, dst_path, sizeof(dd));
      catpath(ss, names[i], sizeof(ss));
      catpath(dd, names[i], sizeof(dd));
      restore_rec(ss, dd);
    }
    iput(src);
    return 0;

  } else {
    // 파일: create는 dst가 없을 때만 수행
    struct inode *exist = namei(dst_path);
    if(!exist){
      begin_op();
      struct inode *dst = create(dst_path, T_FILE, 0, 0);
      if(!dst){ end_op(); iput(src); return -1; }
      // create() 는 dst를 잠금 잡은 상태로 반환
      ilock(src);
      clone_file_inode(src, dst);
      iunlock(src);
      iunlockput(dst);
      end_op();
    } else {
      // 이미 있다면 내용만 덮도록 블록 공유 복제
      // (필요 없으면 단순히 스킵해도 무방)
      ilock(src);
      ilock(exist);
      clone_file_inode(src, exist);
      iunlock(exist);
      iunlock(src);
      iput(exist);
    }
    iput(src);
    return 0;
  }
}

int
snapshot_restore_from(int snap_id)
{
  char idbuf[8]; format_snap_id(idbuf, snap_id);

  char src_root[MAXPATH];
  safestrcpy(src_root, "/snapshot", sizeof(src_root));
  catpath(src_root, idbuf, sizeof(src_root));

  // 1) 스냅샷 존재 확인
  struct inode *src = namei(src_root);
  if(src == 0){
    // 스냅샷이 없으면 아무 것도 하지 말고 실패 반환
    return -1;
  }
  iput(src);

  // 2) 루트 정리 후 복구
  if(wipe_root_except_snapshot() < 0)
    return -1;

  return restore_rec(src_root, "/");  // 성공 시 0
}
///////////////////////////////////////////////////////////////////////////////////////////////////////
int
sys_open(void)
{
  char *path;
  int fd, omode;
  struct file *f;
  struct inode *ip;

  if(argstr(0, &path) < 0 || argint(1, &omode) < 0)
    return -1;

  // 스냅샷 아래 쓰기/생성 시도는 즉시 거부 (begin_op() 이전에)
  if (path_is_under_snapshot_any(path) && is_write_like_open(omode)) {
    return -1; // EPERM
  }

  begin_op();

  if(omode & O_CREATE){
    // create() 내부에서 트랜잭션을 소비하므로 실패시 end_op 필요
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f) fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  iunlock(ip);
  end_op();

  f->type = FD_INODE;
  f->ip = ip;
  f->off = 0;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  return fd;
}

int sys_mkdir(void)
{
  char *path;
  struct inode *ip;

  if(argstr(0, &path) < 0) return -1;
  if(path_write_into_snapshot(path)) return -1;

  begin_op();
  if((ip = create(path, T_DIR, 0, 0)) == 0){ end_op(); return -1; }
  iunlockput(ip); end_op();
  return 0;
}

int sys_mknod(void)
{
  struct inode *ip; char *path; int major, minor;

  if(argstr(0, &path) < 0) return -1;
  if(path_write_into_snapshot(path)) return -1;

  begin_op();
  if(argint(1,&major) < 0 || argint(2,&minor) < 0 ||
     (ip = create(path, T_DEV, major, minor)) == 0){
    end_op(); return -1;
  }
  iunlockput(ip); end_op(); return 0;
}

int
sys_chdir(void)
{
  char *path;
  struct inode *ip;
  struct proc *curproc = myproc();
  
  begin_op();
  if(argstr(0, &path) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(curproc->cwd);
  end_op();
  curproc->cwd = ip;
  return 0;
}

int
sys_exec(void)
{
  char *path, *argv[MAXARG];
  int i;
  uint uargv, uarg;

  if(argstr(0, &path) < 0 || argint(1, (int*)&uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv))
      return -1;
    if(fetchint(uargv+4*i, (int*)&uarg) < 0)
      return -1;
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    if(fetchstr(uarg, &argv[i]) < 0)
      return -1;
  }
  return exec(path, argv);
}

int
sys_pipe(void)
{
  int *fd;
  struct file *rf, *wf;
  int fd0, fd1;

  if(argptr(0, (void*)&fd, 2*sizeof(fd[0])) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      myproc()->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  fd[0] = fd0;
  fd[1] = fd1;
  return 0;
}
