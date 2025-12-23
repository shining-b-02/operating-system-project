// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
static void itrunc(struct inode*);
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb; 
static uint balloc(uint dev);

// 자유 inode 개수 세기 (on-disk dinode 테이블 스캔)
int
fs_count_free_inodes(void){
  int free = 0;
  for(uint inum = 1; inum < sb.ninodes; inum++){
    struct buf *bp = bread(ROOTDEV, IBLOCK(inum, sb));
    struct dinode *dip = (struct dinode*)bp->data + inum % IPB;
    if(dip->type == 0) free++;
    brelse(bp);
  }
  return free;
}

// ---- 내부 유틸: 디렉터리 재귀 카운트 (루트에서만 /snapshot 제외) ----
static int
count_dir_children(struct inode *dp, int is_root){
  if(dp->type != T_DIR) return 0;

  int need = 0;
  struct dirent de;

  for(int off = 0; off + sizeof(de) <= dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de)) break;
    if(de.inum == 0) continue;

    // skip "." and ".."
    if(de.name[0]=='.' && (de.name[1]==0 || (de.name[1]=='.' && de.name[2]==0)))
      continue;

    // 루트에서만 "snapshot" 디렉터리 스킵
    if(is_root && namecmp(de.name, "snapshot") == 0)
      continue;

    // 이 항목 하나가 스냅샷에서 "새 inode 1개"로 생성됨
    need += 1;

    // 자식이 디렉터리면 재귀 카운트
    struct inode *ip = iget(dp->dev, de.inum);  // ← 이 줄이 반드시 있어야 함
    ilock(ip);
    if(ip->type == T_DIR)
      need += count_dir_children(ip, 0);
    iunlockput(ip);  // 락 해제 + 참조 감소
  }
  return need;
}
// 스냅샷 1회에 필요한 inode 수 추정
int
count_needed_inodes(const char *path){
  (void)path; // 현재 "/"만 대상으로 사용

  int need = 0;

  // 루트부터 트리 카운트
  struct inode *root = iget(ROOTDEV, ROOTINO);
  ilock(root);
  need += count_dir_children(root, 1);
  iunlockput(root);

  // "/snapshot/<ID>" 디렉터리 1개는 반드시 새로 필요
  need += 1;

  // "/snapshot" 없으면 그것도 1개 더 필요
  struct inode *ss = namei("/snapshot");
  if(ss){
    iput(ss);
  }else{
    need += 1;
  }
  return need;
}

///////////////////////////////////////////////////////////////////////////////////
// oldbn의 콘텐츠를 그대로 복제한 새 블록을 만들어 반환
static uint
cow_clone_block(uint dev, uint oldbn)
{
  uint newbn = balloc(dev);

  struct buf *ob = bread(dev, oldbn);
  struct buf *nb = bread(dev, newbn);
  memmove(nb->data, ob->data, BSIZE);
  bwrite(nb);

  brelse(nb);
  brelse(ob);

  cow_decref(oldbn);   // 공유 참조 해제
  cow_incref(newbn);   // 새 블록 참조 등록
  return newbn;
}

// 간접 포인터 블록(ip->addrs[NDIRECT])을 수정하기 전에 고유화
static void
ensure_unique_indirect(struct inode *ip)
{
  // 아직 없으면 새로 할당
  if(ip->addrs[NDIRECT] == 0){
    uint bn = balloc(ip->dev);
    cow_incref(bn);                // 참조 등록
    ip->addrs[NDIRECT] = bn;
    iupdate(ip);
    return;
  }

  // 스냅샷과 공유 중이면 복제
  uint ib = ip->addrs[NDIRECT];
  if(cow_get_ref(ib) > 1){
    uint nib = cow_clone_block(ip->dev, ib);
    ip->addrs[NDIRECT] = nib;
    iupdate(ip);
  }
}

// ---- COW refcount table ----
ushort cow_refcnt[COW_MAX_BLKS];
struct spinlock cow_lock;

void
cow_init(void)
{
  initlock(&cow_lock, "cowref");
  // 초기화: 부트 시 0 (스냅샷 생성 시 전수 스캔하며 올림)
}

void
cow_incref(uint bn)
{
  if(bn >= COW_MAX_BLKS) return;
  acquire(&cow_lock);
  cow_refcnt[bn]++;
  release(&cow_lock);
}

void
cow_decref(uint bn)
{
  if(bn >= COW_MAX_BLKS) return;
  acquire(&cow_lock);
  if(cow_refcnt[bn] > 0) cow_refcnt[bn]--;
  release(&cow_lock);
}

int
cow_get_ref(uint bn)
{
  if(bn >= COW_MAX_BLKS) return 0;
  return cow_refcnt[bn];
}

// ---- bcow: 쓰기 직전 기존 블록이 공유중(refcnt>1)이면 새 블록에 복사 후 매핑 교체 ----
// is_dir==1이면 디렉토리: 디렉토리 데이터 블록은 COW 비대상 → 바로 반환
int
bcow_maybe_clone(struct inode *ip, uint lbn, uint oldbn, int is_dir)
{
  if(is_dir) return oldbn; // 디렉토리는 COW 미적용
  if(oldbn == 0) return 0;
  if(cow_get_ref(oldbn) <= 1) return oldbn; // 공유 아님 → 그대로

  // 공유 중 → 새 블록 할당 후 내용 복제
  uint newbn = balloc(ip->dev);
  struct buf *ob = bread(ip->dev, oldbn);
  struct buf *nb = bread(ip->dev, newbn);
  memmove(nb->data, ob->data, BSIZE);
  log_write(nb);
  brelse(ob);
  brelse(nb);

  // 인덱스 lbn 위치의 실제 매핑을 newbn으로 교체
  // direct
  if(lbn < NDIRECT){
    if(ip->addrs[lbn] != oldbn) panic("bcow direct race");
    ip->addrs[lbn] = newbn;
  } else {
    // indirect
    lbn -= NDIRECT;
    // 간단화를 위해 간접블록 버퍼 직접 갱신
    uint indirect_bn = ip->addrs[NDIRECT];
    if(indirect_bn == 0) panic("bcow no indirect");
    struct buf *ib = bread(ip->dev, indirect_bn);
    uint *ia = (uint*)ib->data;
    if(ia[lbn] != oldbn) panic("bcow indir race");
    ia[lbn] = newbn;
    log_write(ib);
    brelse(ib);
  }

  // 참조카운트 갱신
  cow_decref(oldbn);
  cow_incref(newbn);
  iupdate(ip);
  return newbn;
}
//////////////////////////////////////////////////////////////////////////////////////

// Read the super block.
void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  // Mark block in use.
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

//////////////////////////////////////////////////////////////////////////////////
// ---- bfree 확장: refcnt==0 일 때만 실제 해제 ----
static void
bfree_ext(int dev, uint b)
{
  if(b >= COW_MAX_BLKS) return;
  if(cow_get_ref(b) > 0){
    // 참조가 남아 있으면 free 금지
    return;
  }
  bfree(dev, b); // 기존 xv6 bfree 호출
}
///////////////////////////////////////////////////////////////////////////////////

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

void
iinit(int dev)
{
  int i = 0;
  
  initlock(&icache.lock, "icache");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d ninodes %d nlog %d logstart %d\
 inodestart %d bmap start %d\n", sb.size, sb.nblocks,
          sb.ninodes, sb.nlog, sb.logstart, sb.inodestart,
          sb.bmapstart);
}

//static
struct inode* iget(uint dev, uint inum);

//PAGEBREAK!
// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, sb));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk, since i-node cache is write-through.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
//static 
struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  acquiresleep(&ip->lock);
  if(ip->valid && ip->nlink == 0){
    acquire(&icache.lock);
    int r = ip->ref;
    release(&icache.lock);
    if(r == 1){
      // inode has no links and no other references: truncate and free.
      itrunc(ip);
      ip->type = 0;
      iupdate(ip);
      ip->valid = 0;
    }
  }
  releasesleep(&ip->lock);

  acquire(&icache.lock);
  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

//PAGEBREAK!
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
/////////////////////////////////////////////////////////////////////////////////////////////
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0){
      addr = balloc(ip->dev);
      cow_incref(addr);   // 새 블록은 ref=1
      ip->addrs[bn] = addr;
    }
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      addr = balloc(ip->dev);
	  cow_incref(addr); // 새 블록은 ref=1
	  a[bn] = addr;
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}
/////////////////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////
// Truncate inode (discard contents).
// Only called when the inode has no links
// to it (no directory entries referring to it)
// and has no in-memory reference to it (is
// not an open file or current directory).
// ---- itrunc 확장: 참조 0될 때만 bfree ----
void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  // direct
  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      uint b = ip->addrs[i];
      ip->addrs[i] = 0;
      cow_decref(b);
      if(cow_get_ref(b) == 0) bfree_ext(ip->dev, b);
    }
  }

  // indirect
  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j]){
        cow_decref(a[j]);
        if(cow_get_ref(a[j]) == 0) bfree_ext(ip->dev, a[j]);
      }
    }
    brelse(bp);
    bfree_ext(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}
//////////////////////////////////////////////////////////////////////
// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

//PAGEBREAK!
// Read data from inode.
// Caller must hold ip->lock.
int
readi(struct inode *ip, char *dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(ip, dst, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(dst, bp->data + off%BSIZE, m);
    brelse(bp);
  }
  return n;
}

/////////////////////////////////////////////////////////////////////////////////
// PAGEBREAK!
// Write data to inode.
// Caller must hold ip->lock.
int
writei(struct inode *ip, char *src, uint off, uint n)
{
  uint tot, m;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
      return -1;
    return devsw[ip->major].write(ip, src, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;
 
  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
	

    int lbn = off / BSIZE;

    // 간접 범위를 건드릴 거면, 간접 포인터 블록을 먼저 고유화
    if(lbn >= NDIRECT)
      ensure_unique_indirect(ip);

    // 원래 코드: 논리블록 lbn -> 물리블록 bn (필요 시 할당)
    uint bn = bmap(ip, lbn);

    // === 데이터 블록 COW: 공유 블록이면 사본으로 교체 ===
    if(cow_get_ref(bn) > 1){
      if(lbn < NDIRECT){
        // direct entry 교체
        uint nb = cow_clone_block(ip->dev, bn);
        ip->addrs[lbn] = nb;
        iupdate(ip);
        bn = nb;
      }else{
        // indirect entry 교체
        int idx = lbn - NDIRECT;
        struct buf *ib = bread(ip->dev, ip->addrs[NDIRECT]); // ensure_unique_indirect 덕에 고유
        uint *ia = (uint*)ib->data;

        // 방어: bmap이 갱신했을 수 있으니 재확인
        if(cow_get_ref(ia[idx]) > 1){
          uint nb = cow_clone_block(ip->dev, ia[idx]);
          ia[idx] = nb;
          bwrite(ib);   // 간접 블록 테이블 업데이트 반영
          bn = nb;
        }
        brelse(ib);
      }
    }

    struct buf *bp = bread(ip->dev, bn);

    uint off_in = off % BSIZE;
    m = min(n - tot, BSIZE - off_in);
    memmove(bp->data + off_in, src, m);
    log_write(bp);
    brelse(bp);
  }

  if(n > 0 && off > ip->size){
    ip->size = off;
    iupdate(ip);
  }
  return n;
}
//////////////////////////////////////////////////////////////////////////////////////
//PAGEBREAK!
// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");

  return 0;
}

//PAGEBREAK!
// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}

/////////////////////////////////////////////////////////////////////////////////////////
int
fs_get_file_block_addrs(const char *cpath, uint *dst, int max)
{
  if(max <= 0) return -1;

  // 과도 방어(원하면 유지, 아니면 지워도 됨)
  int cap = NDIRECT + 1 + NINDIRECT;  // direct + (indirect pointer) + indirect entries
  if(max > cap) max = cap;

  // namei는 char*를 받으므로 캐스팅
  struct inode *ip = namei((char*)cpath);
  if(!ip) return -1;

  ilock(ip);
  int count = 0;

  // direct
  for(int i = 0; i < NDIRECT && count < max; i++){
    dst[count++] = ip->addrs[i];
  }

  // indirect 포인터 블록 자체
  if(count < max){
    dst[count++] = ip->addrs[NDIRECT];
  }

  // indirect 엔트리들
  if(ip->addrs[NDIRECT] && count < max){
    struct buf *ib = bread(ip->dev, ip->addrs[NDIRECT]);
    uint *ia = (uint*)ib->data;
    for(int j = 0; j < NINDIRECT && count < max; j++){
      dst[count++] = ia[j];
    }
    brelse(ib);
  }

  iunlockput(ip);
  return count;
}
/////////////////////////////////////////////////////////////////////////////////////////////
