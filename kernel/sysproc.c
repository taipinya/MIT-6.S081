#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "date.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

extern pte_t * walk(pagetable_t, uint64, int);

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;


  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}


#ifdef LAB_PGTBL
int
sys_pgaccess(void)
{
  // lab pgtbl: your code here.
  uint64  startva;
  int pgnum;
  int bitmask = 0;
  uint64 ubmaddr;

  if(argaddr(0, &startva) < 0 || argint(1, &pgnum) < 0 || argaddr(2, &ubmaddr) < 0)//每个函数返回-1表示失败
    return -1;

  if(pgnum > 32) //设置查找页数上限
    return -1;
  
  
  pagetable_t pagetable = myproc()->pagetable;
  uint64 va;
  pte_t *pte;

  for(int i = 0; i < pgnum; i++){
    va = startva + i*PGSIZE;
    pte = walk(pagetable, va, 0);
    if(pte == 0)continue; //若找不到walk()会返回0

    if(*pte & PTE_A){
      //已访问
      bitmask |= (1 << i); //第i为置1
      *pte &= ~PTE_A;  //清除访存位；
    }
  }

  copyout(pagetable, ubmaddr, (char *)&bitmask, sizeof(int));
  return 0;
}
#endif

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
