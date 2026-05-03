// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"
extern uint ticks;


struct bucket {
  struct spinlock lock;
  struct buf* head;
}bcache_bucket[NBUCKET];

struct buf buf[NBUF];


void
binit(void)
{
/*initlock(&bcache.lock, "bcache");*/
  for(int i = 0; i < NBUCKET; i++){
    initlock(&bcache_bucket[i].lock,"bcache");
  }

  //将buf随机分配到不同的桶中，建立双向链表，初始化
  for(int i = 0; i < NBUF; i++){
    int buc = i % NBUCKET;
    buf[i].timestamp = ticks;
    buf[i].prev = 0;
    buf[i].refcnt = 0;
    buf[i].next = bcache_bucket[buc].head;
    if(bcache_bucket[buc].head){
      bcache_bucket[buc].head->prev = &buf[i];
    }    
    bcache_bucket[buc].head = &buf[i];
    initsleeplock(&buf[i].lock, "buffer");

  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
/*
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int buc = (int)blockno % NBUCKET;
  acquire(&bcache_bucket[buc].lock);

  // Is the block already cached?
  for(b = bcache_bucket[buc].head; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      b->timestamp = ticks;
      release(&bcache_bucket[buc].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  //遍历所有桶的buf，找引用数为0且时间戳最小的那个
  release(&bcache_bucket[buc].lock);
  uint minticks = ~0; //全1，uint的最大值
  struct buf* victim = 0;
  int vic_buc = -1;

  for(int i = 0; i < NBUCKET; i++){
    acquire(&bcache_bucket[i].lock);
    b = bcache_bucket[i].head;
    while(b){
      if(b->refcnt == 0 && b->timestamp < minticks){
        if(victim && vic_buc != i)
          release(&bcache_bucket[vic_buc].lock); //当前桶第一次找到，释放上一个候选的桶锁
        victim = b;
        vic_buc = i;
        minticks = b->timestamp;
      }
      b = b->next;
    }
    if(vic_buc != i)
      release(&bcache_bucket[i].lock);//当前桶没找到
  }

  //将找到的buf从所处桶移植到正确的桶
  if(victim && (vic_buc != buc)) {
    //从原桶中删除
    if(victim->prev){
      victim->prev->next = victim->next;
    }
    else{
      bcache_bucket[vic_buc].head = victim->next;
    } 
    if(victim->next){
      victim->next->prev = victim->prev;
    }   
    release(&bcache_bucket[vic_buc].lock);
    //插入到新桶
    acquire(&bcache_bucket[buc].lock);
    victim->prev = 0;
    victim->next = bcache_bucket[buc].head;
    if(bcache_bucket[buc].head){
      bcache_bucket[buc].head->prev = victim;
    }    
    bcache_bucket[buc].head = victim;
  }
  else if(!victim){
    panic("bget: no buffers");
  }

  // 在正式使用 victim 前，再扫描一遍目标桶
for(b = bcache_bucket[buc].head; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno && b != victim){
        // 已经有人抢先缓存了，放弃 victim，用这个 b
        // 需要把 victim 还回去（或留在当前桶等下次驱逐）
        b->refcnt++;
        b->timestamp = ticks;
        // 释放 victim（refcnt 保持 0，归还原桶或留在 buc 桶均可）
        release(&bcache_bucket[buc].lock);
        acquiresleep(&b->lock);
        return b;
    }
}
  
  //找到后的处理
  victim->valid = 0;
  victim->timestamp = ticks;
  victim->dev = dev;
  victim->blockno = blockno;
  victim->refcnt++;
  release(&bcache_bucket[buc].lock);
  acquiresleep(&victim->lock);
  return victim;

  }
//这是我自己写的代码，对比下我认为缺乏整洁美观，需要多使用辅助函数；对于具体步骤思路不清晰，各种情况分隔不明显；并没有分析衡量好需求和实现难度，
  */


static int
hash(uint blockno)
{
  return blockno % NBUCKET;
}

static void
remove_from_bucket(int buc, struct buf *b)
{
  if(b->prev)
    b->prev->next = b->next;
  else
    bcache_bucket[buc].head = b->next;

  if(b->next)
    b->next->prev = b->prev;

  b->prev = 0;
  b->next = 0;
}

static void
insert_into_bucket(int buc, struct buf *b)
{
  b->prev = 0;
  b->next = bcache_bucket[buc].head;

  if(bcache_bucket[buc].head)
    bcache_bucket[buc].head->prev = b;

  bcache_bucket[buc].head = b;
}

static struct buf*
find_in_bucket(int buc, uint dev, uint blockno)
{
  struct buf *b;

  for(b = bcache_bucket[buc].head; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      return b;
    }
  }

  return 0;
}

static struct buf*
find_free_in_bucket(int buc)
{
  struct buf *b;
  struct buf *victim = 0;
  uint minticks = ~0;

  for(b = bcache_bucket[buc].head; b; b = b->next){
    if(b->refcnt == 0 && b->timestamp < minticks){
      victim = b;
      minticks = b->timestamp;
    }
  }

  return victim;
}

static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  struct buf *victim;
  int buc = hash(blockno);

  acquire(&bcache_bucket[buc].lock);

  // 1. 目标 bucket 中命中
  b = find_in_bucket(buc, dev, blockno);
  if(b){
    b->refcnt++;
    release(&bcache_bucket[buc].lock);
    acquiresleep(&b->lock);
    return b;
  }

  // 2. 目标 bucket 中有空闲 buf，直接复用，不需要摘链表
  victim = find_free_in_bucket(buc);
  if(victim){
    victim->dev = dev;
    victim->blockno = blockno;
    victim->valid = 0;
    victim->refcnt = 1;
    victim->timestamp = ticks;

    release(&bcache_bucket[buc].lock);
    acquiresleep(&victim->lock);
    return victim;
  }

  release(&bcache_bucket[buc].lock);

  // 3. 目标 bucket 没有空闲 buf，从其他 bucket 偷
  for(int i = 0; i < NBUCKET; i++){
    if(i == buc)
      continue;

    // 按 bucket 编号顺序加锁，避免死锁
    if(i < buc){
      acquire(&bcache_bucket[i].lock);
      acquire(&bcache_bucket[buc].lock);
    } else {
      acquire(&bcache_bucket[buc].lock);
      acquire(&bcache_bucket[i].lock);
    }

    // 4. 加锁后必须再次检查目标 bucket，防止重复缓存
    b = find_in_bucket(buc, dev, blockno);
    if(b){
      b->refcnt++;

      if(i < buc){
        release(&bcache_bucket[buc].lock);
        release(&bcache_bucket[i].lock);
      } else {
        release(&bcache_bucket[i].lock);
        release(&bcache_bucket[buc].lock);
      }

      acquiresleep(&b->lock);
      return b;
    }

    // 新增：再次检查目标 bucket 是否已有空闲 buffer
    victim = find_free_in_bucket(buc);
    if(victim){
      victim->dev = dev;
      victim->blockno = blockno;
      victim->valid = 0;
      victim->refcnt = 1;
      victim->timestamp = ticks;

      if(i < buc){
        release(&bcache_bucket[buc].lock);
        release(&bcache_bucket[i].lock);
      } else {
        release(&bcache_bucket[i].lock);
        release(&bcache_bucket[buc].lock);
      }

      acquiresleep(&victim->lock);
      return victim;
    }

    // 5. 在源 bucket 中找空闲 victim
    victim = find_free_in_bucket(i);
    if(victim){
      remove_from_bucket(i, victim);
      insert_into_bucket(buc, victim);

      victim->dev = dev;
      victim->blockno = blockno;
      victim->valid = 0;
      victim->refcnt = 1;
      victim->timestamp = ticks;

      if(i < buc){
        release(&bcache_bucket[buc].lock);
        release(&bcache_bucket[i].lock);
      } else {
        release(&bcache_bucket[i].lock);
        release(&bcache_bucket[buc].lock);
      }

      acquiresleep(&victim->lock);
      return victim;
    }

    if(i < buc){
      release(&bcache_bucket[buc].lock);
      release(&bcache_bucket[i].lock);
    } else {
      release(&bcache_bucket[i].lock);
      release(&bcache_bucket[buc].lock);
    }
  }

  panic("bget: no buffers");
}
  

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int buc = hash(b->blockno);
  acquire(&bcache_bucket[buc].lock);
  b->refcnt--; 
  if(b->refcnt < 0)
    panic("brelse: refcnt");
  if(b->refcnt == 0){
    b->timestamp = ticks;
  }
  release(&bcache_bucket[buc].lock);
}

void
bpin(struct buf *b) {
  int buc = hash(b->blockno);
  acquire(&bcache_bucket[buc].lock);
  b->refcnt++;
  release(&bcache_bucket[buc].lock);
}

void
bunpin(struct buf *b) {
  int buc = hash(b->blockno);
  acquire(&bcache_bucket[buc].lock);
  if(b->refcnt <= 0)
    panic("bunpin");  //边界处理
  b->refcnt--;
  if(b->refcnt == 0)
    b->timestamp = ticks; 
  release(&bcache_bucket[buc].lock);
}


