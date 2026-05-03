// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct kmem{
  struct spinlock lock;
  struct run *freelist;
};

//为每个cpu维护一个锁
struct kmem kmems[NCPU];

void
kinit()
{
  for(int i = 0; i < NCPU; i++){
    initlock(&kmems[i].lock, "kmem");
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  //获取cpuid，注意开关中断
  push_off();
  int hart = cpuid();
  pop_off();
  //获取锁，分配空闲空间
  acquire(&kmems[hart].lock);
  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  r->next = kmems[hart].freelist;
  kmems[hart].freelist = r;
  release(&kmems[hart].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  //获取cpuid，注意开关中断
  push_off();
  int hart = cpuid();
  pop_off();
  //获取锁，分配空闲空间
  //若空闲列表为空，则steal
  acquire(&kmems[hart].lock);
  r = kmems[hart].freelist;
  if(r)
    kmems[hart].freelist = r->next;
  else{
    //steal
    for(int i = (hart +1)%NCPU; i != hart; i = (i+1)%NCPU){
      acquire(&kmems[i].lock);
      if(kmems[i].freelist){
        //找到非空的空闲链表，steal
        r = kmems[i].freelist;
        kmems[i].freelist = r->next;
        release(&kmems[i].lock);
        break;
      }
      release(&kmems[i].lock);
    }
  }
  release(&kmems[hart].lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
