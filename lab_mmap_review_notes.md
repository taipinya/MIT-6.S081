# MIT 6.S081 Lab mmap 复习笔记

> 关键词：`mmap`、`munmap`、VMA、lazy allocation、page fault、MAP_SHARED 写回、fork 继承、文件偏移 offset  
> 实验难度：Hard  
> 复习目标：一眼回忆实现框架、关键函数、易错点和 debug 过程。

---

## 1. 实验目标概述

本实验要求在 xv6 中实现两个系统调用：

```c
void *mmap(void *addr, uint64 length, int prot, int flags, int fd, uint64 offset);
int munmap(void *addr, uint64 length);
```

其核心功能是：

- 将一个文件映射到用户进程的虚拟地址空间；
- 用户程序可以像访问普通内存一样访问文件内容；
- 映射采用懒加载机制，`mmap()` 本身不分配物理页；
- 用户首次访问映射地址时触发 page fault，再由内核分配物理页、读取文件内容、建立页表映射；
- `munmap()` 负责解除映射，必要时将修改写回文件；
- `fork()` 后子进程应继承父进程的 mmap 映射语义；
- `exit()` 时应自动清理所有 mmap 区域。

一句话概括：

> `mmap` 不是立即把文件全部读入内存，而是先在进程中登记一段文件映射区域；真正访问时通过 page fault 懒加载文件页；解除映射或退出时再负责写回、释放和清理资源。

---

## 2. 实验整体架构

本实验可以拆成五条主线：

```text
系统调用框架
    ↓
VMA 元数据管理
    ↓
page fault 懒加载
    ↓
munmap 写回与释放
    ↓
fork / exit 生命周期处理
```

对应修改位置大致如下：

| 模块 | 主要修改 |
|---|---|
| `user/user.h` | 添加 `mmap`、`munmap` 用户态声明 |
| `user/usys.pl` | 添加 syscall stub |
| `kernel/syscall.h` | 添加 syscall 编号 |
| `kernel/syscall.c` | 注册 `sys_mmap`、`sys_munmap` |
| `Makefile` | 添加 `_mmaptest` |
| `kernel/proc.h` | 定义 VMA 结构，在 `struct proc` 中添加 `vmas[]` |
| `kernel/proc.c` | 初始化 VMA、fork 复制 VMA、exit 清理 VMA |
| `kernel/sysfile.c` | 实现 `sys_mmap()`、`sys_munmap()`、`do_munmap()` |
| `kernel/trap.c` | 在 `usertrap()` 中处理 mmap page fault |
| `kernel/vm.c` | 实现 mmap 专用页表工具函数，如懒加载、跳过未映射页的 unmap、复制 mmap 页 |

---

## 3. mmap 参数语义

实验中的 `mmap` 接口为：

```c
void *mmap(void *addr, uint64 length, int prot, int flags, int fd, uint64 offset);
```

本实验只实现子集：

| 参数 | 含义 | 本实验简化 |
|---|---|---|
| `addr` | 用户希望映射到的地址 | 总是 0，由内核选择地址 |
| `length` | 映射长度 | 需要按页向上取整 |
| `prot` | 权限 | `PROT_READ` / `PROT_WRITE` / 二者组合 |
| `flags` | 映射方式 | `MAP_SHARED` 或 `MAP_PRIVATE` |
| `fd` | 文件描述符 | 对应被映射文件 |
| `offset` | 文件起始偏移 | 题目保证初始为 0，但实现中仍需在 VMA 中维护 offset |

返回值：

- 成功：返回映射区域的起始虚拟地址；
- 失败：返回 `-1`，即 `0xffffffffffffffff`。

---

## 4. munmap 参数语义

`munmap(addr, length)` 用于解除 mmap 映射。

本实验保证解除范围只会是：

1. 解除 VMA 开头一段；
2. 解除 VMA 结尾一段；
3. 解除整个 VMA；

不会从中间挖洞。

示意：

```text
合法：
[ unmap ][ remain ]
[ remain ][ unmap ]
[        unmap      ]

不需要处理：
[ remain ][ unmap ][ remain ]
```

---

## 5. 用户进程虚拟地址空间回顾

xv6 用户进程虚拟地址空间大致如下：

```text
低地址
0x0000000000000000
│
│  text
│  data
│  bss
│  heap
│
│  p->sz
│
│  未使用空洞
│
│  mmap 区域，从高地址向低地址分配
│
│  TRAPFRAME
│  TRAMPOLINE
│
高地址
MAXVA
```

### 为什么 mmap 从 `TRAPFRAME` 下方往低地址分配？

因为：

- `TRAMPOLINE` 是用户态/内核态切换的跳板页；
- `TRAPFRAME` 保存用户寄存器现场；
- 两者位于用户虚拟地址空间最高处，不能被 mmap 覆盖；
- 普通 heap 从低地址向高地址增长；
- mmap 从高地址向低地址增长，可以减少与 heap 冲突的风险。

典型设计：

```c
#define MMAPTOP TRAPFRAME
```

第一次 mmap：

```c
va = TRAPFRAME - PGROUNDUP(length);
```

之后新的 mmap 区域继续放在已有 mmap 区域下方。

---

## 6. VMA 结构设计

VMA 是 Virtual Memory Area，表示进程中的一段连续虚拟地址区域。

本实验中每个 mmap 映射都对应一个 VMA。

建议结构：

```c
#define NVMA 16

struct vma {
  int used;              // 是否有效
  uint64 addr;           // 当前 VMA 起始虚拟地址
  uint64 length;         // 页对齐后的映射长度
  uint64 offset;         // 当前 VMA 起点对应文件偏移
  int prot;              // PROT_READ / PROT_WRITE
  int flags;             // MAP_SHARED / MAP_PRIVATE
  struct file *file;     // 被映射文件
};
```

在 `struct proc` 中加入：

```c
struct vma vmas[NVMA];
```

### 为什么必须记录 `offset`？

这是本实验最关键的隐藏坑。

文件偏移应为：

```c
file_offset = v->offset + (va - v->addr);
```

不能只写：

```c
file_offset = va - v->addr;
```

原因是 `munmap()` 可能解除 VMA 开头一部分。解除后：

```c
v->addr += len;
v->length -= len;
```

如果不同时维护：

```c
v->offset += len;
```

那么剩余 VMA 的起点虽然变了，但它在文件中对应的偏移没有被记录，后续 page fault 或写回会读写错误的文件位置。

示例：

```text
原始映射：
虚拟地址 [0x10000, 0x11000) -> 文件偏移 [0, 4096)
虚拟地址 [0x11000, 0x12000) -> 文件偏移 [4096, 8192)

munmap 掉开头一页后：
剩余虚拟地址 [0x11000, 0x12000)

此时剩余 VMA 的 addr 变成 0x11000，
但它对应的文件偏移应该是 4096，而不是 0。
```

因此必须维护：

```c
v->offset += len;
```

---

## 7. 系统调用框架实现

### user.h

```c
void *mmap(void *addr, uint64 length, int prot, int flags,
           int fd, uint64 offset);
int munmap(void *addr, uint64 length);
```

`mmap` 返回 `void *`，因为它返回的是用户虚拟地址。

`munmap` 返回 `int`，因为它只表示成功或失败：

```text
成功返回 0
失败返回 -1
```

### usys.pl

```perl
entry("mmap");
entry("munmap");
```

### syscall.h

添加系统调用编号：

```c
#define SYS_mmap    ...
#define SYS_munmap  ...
```

### syscall.c

```c
extern uint64 sys_mmap(void);
extern uint64 sys_munmap(void);

[SYS_mmap]    sys_mmap,
[SYS_munmap]  sys_munmap,
```

### Makefile

加入：

```makefile
$U/_mmaptest\
```

---

## 8. sys_mmap 实现思路

`sys_mmap()` 只负责登记 VMA，不分配物理页，不读取文件。

核心流程：

```text
1. 取系统调用参数
2. 检查 length、addr、offset
3. 通过 argfd 获取 struct file *
4. 检查文件权限与 prot / flags 是否匹配
5. 找空闲 VMA
6. 选择 mmap 虚拟地址
7. 填写 VMA 元数据
8. filedup 增加文件引用计数
9. 返回映射起始地址
```

关键点：

```c
uint64 maplen = PGROUNDUP(length);
```

`PGROUNDUP` 表示将映射长度向上取整到页大小整数倍。

示例：

```text
length = 100  -> maplen = 4096
length = 4096 -> maplen = 4096
length = 4097 -> maplen = 8192
```

原因：页表只能按页建立映射，不能只映射几十个字节。

### sys_mmap 伪代码

```c
uint64
sys_mmap(void)
{
  uint64 addr;
  uint64 length;
  int prot;
  int flags;
  int fd;
  uint64 offset;
  struct file *f;
  struct proc *p = myproc();

  argaddr(0, &addr);
  argaddr(1, &length);
  argint(2, &prot);
  argint(3, &flags);
  if(argfd(4, &fd, &f) < 0)
    return -1;
  argaddr(5, &offset);

  if(length == 0)
    return -1;

  if(addr != 0)
    return -1;

  if(offset != 0)
    return -1;

  if((prot & PROT_READ) && !f->readable)
    return -1;

  if((prot & PROT_WRITE) && (flags & MAP_SHARED) && !f->writable)
    return -1;

  int idx = -1;
  for(int i = 0; i < NVMA; i++){
    if(!p->vmas[i].used){
      idx = i;
      break;
    }
  }

  if(idx < 0)
    return -1;

  uint64 top = TRAPFRAME;
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used && p->vmas[i].addr < top)
      top = p->vmas[i].addr;
  }

  uint64 maplen = PGROUNDUP(length);
  uint64 va = top - maplen;

  if(va < p->sz)
    return -1;

  p->vmas[idx].used = 1;
  p->vmas[idx].addr = va;
  p->vmas[idx].length = maplen;
  p->vmas[idx].offset = offset;
  p->vmas[idx].prot = prot;
  p->vmas[idx].flags = flags;
  p->vmas[idx].file = filedup(f);

  return va;
}
```

---

## 9. Lazy Allocation 与 Page Fault

### 为什么 mmap 不能立即分配所有页？

如果用户 mmap 一个很大的文件：

```c
mmap(0, 1GB, PROT_READ, MAP_PRIVATE, fd, 0);
```

如果立即读取整个文件并分配物理页，会导致：

- mmap 调用非常慢；
- 物理内存可能不足；
- 用户可能只访问文件中的很小一部分，提前加载浪费巨大。

因此采用懒加载：

```text
mmap 时：
只记录 VMA

第一次访问映射地址：
触发 page fault

page fault 处理：
分配物理页
从文件读取对应页
建立页表映射
返回用户态继续执行
```

---

## 10. 在 usertrap 中处理 mmap page fault

RISC-V 中：

```text
scause = 13 -> load page fault
scause = 15 -> store / AMO page fault
```

在 `usertrap()` 中加入：

```c
} else if(r_scause() == 13 || r_scause() == 15) {
  if(mmap_pagefault(r_stval(), r_scause()) < 0)
    setkilled(p);
}
```

其中：

- `r_stval()` 得到出错虚拟地址；
- `r_scause()` 表示缺页类型；
- `mmap_pagefault()` 负责判断是否是合法 mmap 缺页。

---

## 11. mmap_pagefault 实现思路

核心流程：

```text
1. fault 地址页对齐
2. 查找包含该地址的 VMA
3. 检查读写权限
4. kalloc 分配物理页
5. memset 清零
6. readi 从文件读取一页
7. 根据 prot 设置 PTE 权限
8. mappages 建立映射
```

关键公式：

```c
uint64 va0 = PGROUNDDOWN(va);
uint64 file_off = v->offset + (va0 - v->addr);
```

### 注意：VMA 区间判断必须左闭右开

正确：

```c
va0 >= v->addr && va0 < v->addr + v->length
```

错误：

```c
va0 > v->addr
```

这个 bug 会导致 VMA 第一页无法被识别。因为访问 `p[0]` 时，页对齐后的地址正好等于 `v->addr`。

### mmap_pagefault 伪代码

```c
int
mmap_pagefault(uint64 va, uint64 scause)
{
  struct proc *p = myproc();
  struct vma *v = 0;
  uint64 va0 = PGROUNDDOWN(va);

  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used &&
       va0 >= p->vmas[i].addr &&
       va0 < p->vmas[i].addr + p->vmas[i].length){
      v = &p->vmas[i];
      break;
    }
  }

  if(v == 0)
    return -1;

  if(scause == 13 && !(v->prot & PROT_READ))
    return -1;

  if(scause == 15 && !(v->prot & PROT_WRITE))
    return -1;

  char *mem = kalloc();
  if(mem == 0)
    return -1;

  memset(mem, 0, PGSIZE);

  ilock(v->file->ip);
  int n = readi(v->file->ip, 0, (uint64)mem,
                v->offset + (va0 - v->addr),
                PGSIZE);
  iunlock(v->file->ip);

  if(n < 0){
    kfree(mem);
    return -1;
  }

  int perm = PTE_U;
  if(v->prot & PROT_READ)
    perm |= PTE_R;
  if(v->prot & PROT_WRITE)
    perm |= PTE_W;

  if(mappages(p->pagetable, va0, PGSIZE, (uint64)mem, perm) < 0){
    kfree(mem);
    return -1;
  }

  return 0;
}
```

---

## 12. readi 参数复习

`readi` 原型大致为：

```c
int readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n);
```

参数含义：

| 参数 | 含义 |
|---|---|
| `ip` | 要读取的文件 inode |
| `user_dst` | 目标地址是否是用户地址 |
| `dst` | 读取结果放到哪里 |
| `off` | 文件内读取偏移 |
| `n` | 最多读取字节数 |

在 mmap page fault 中：

```c
readi(v->file->ip, 0, (uint64)mem, file_off, PGSIZE);
```

这里 `user_dst = 0`，因为 `mem` 是内核通过 `kalloc()` 得到的内核地址，不是用户地址。

必须先：

```c
memset(mem, 0, PGSIZE);
```

这样文件最后不足一页时，剩余部分保持为 0，不会留下脏数据。

---

## 13. sys_munmap 与 do_munmap

推荐结构：

```text
sys_munmap()
    负责取参数
    ↓
do_munmap()
    真正完成解除映射
```

`sys_munmap()`：

```c
uint64
sys_munmap(void)
{
  uint64 addr;
  uint64 length;

  argaddr(0, &addr);
  argaddr(1, &length);

  if(length == 0)
    return -1;

  return do_munmap(addr, length);
}
```

---

## 14. do_munmap 实现思路

核心流程：

```text
1. 找到包含 addr 的 VMA
2. 检查 munmap 范围是否合法
3. 如果 MAP_SHARED，先写回已映射页
4. 调用 mmap 专用 uvmunmap，解除映射并释放物理页
5. 根据解除范围更新 VMA
6. 如果整个 VMA 被解除，fileclose 并清空 VMA
```

### 为什么需要 mmap 专用 uvmunmap？

因为 mmap 是 lazy allocation。VMA 范围内不是每一页都一定被访问过。

没有访问过的页：

```text
没有 PTE
没有物理页
不能按普通 uvmunmap 强行解除
```

xv6 原始 `uvmunmap()` 遇到未映射页可能 panic：

```c
panic("uvmunmap: not mapped");
```

所以应写一个 mmap 专用版本，遇到未映射页直接跳过。

---

## 15. MAP_SHARED 写回逻辑

对于 `MAP_SHARED`，解除映射前要写回文件：

```c
if(v->flags & MAP_SHARED){
  begin_op();

  for(uint64 a = addr; a < addr + len; a += PGSIZE){
    uint64 pa = walkaddr(p->pagetable, a);
    if(pa == 0)
      continue;

    ilock(v->file->ip);
    int n = writei(v->file->ip, 0, pa,
                   v->offset + (a - v->addr),
                   PGSIZE);
    iunlock(v->file->ip);

    if(n < 0){
      end_op();
      return -1;
    }
  }

  end_op();
}
```

注意：

```c
v->offset + (a - v->addr)
```

才是文件中的真实写回偏移。

不能写成：

```c
a - PGSIZE
```

因为 `writei` 的偏移是文件偏移，不是虚拟地址。

---

## 16. do_munmap 伪代码

```c
int
do_munmap(uint64 addr, uint64 length)
{
  struct proc *p = myproc();
  struct vma *v = 0;
  uint64 len = PGROUNDUP(length);

  if(len == 0)
    return -1;

  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used &&
       addr >= p->vmas[i].addr &&
       addr < p->vmas[i].addr + p->vmas[i].length){
      v = &p->vmas[i];
      break;
    }
  }

  if(v == 0)
    return -1;

  if(addr < v->addr || addr + len > v->addr + v->length)
    return -1;

  if(addr != v->addr && addr + len != v->addr + v->length)
    return -1;

  if(v->flags & MAP_SHARED){
    begin_op();

    for(uint64 a = addr; a < addr + len; a += PGSIZE){
      uint64 pa = walkaddr(p->pagetable, a);
      if(pa == 0)
        continue;

      ilock(v->file->ip);
      int n = writei(v->file->ip, 0, pa,
                     v->offset + (a - v->addr),
                     PGSIZE);
      iunlock(v->file->ip);

      if(n < 0){
        end_op();
        return -1;
      }
    }

    end_op();
  }

  uvmunmap_mmap(p->pagetable, addr, len / PGSIZE, 1);

  if(addr == v->addr && len == v->length){
    fileclose(v->file);
    v->used = 0;
    v->addr = 0;
    v->length = 0;
    v->offset = 0;
    v->prot = 0;
    v->flags = 0;
    v->file = 0;
  } else if(addr == v->addr){
    v->addr += len;
    v->offset += len;
    v->length -= len;
  } else if(addr + len == v->addr + v->length){
    v->length -= len;
  }

  return 0;
}
```

---

## 17. writei 参数复习

`writei` 原型大致为：

```c
int writei(struct inode *ip, int user_src, uint64 src, uint off, uint n);
```

参数含义：

| 参数 | 含义 |
|---|---|
| `ip` | 要写入的文件 inode |
| `user_src` | 源地址是否是用户地址 |
| `src` | 数据来源地址 |
| `off` | 文件内写入偏移 |
| `n` | 写入字节数 |

在 mmap 写回中：

```c
writei(v->file->ip, 0, pa, file_off, PGSIZE);
```

这里 `user_src = 0`，因为 `pa` 是内核可直接访问的物理内存地址。

---

## 18. uvmunmap_mmap 实现思路

mmap 专用 unmap 函数应跳过未映射页：

```c
void
uvmunmap_mmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap_mmap: not aligned");

  for(a = va; a < va + npages * PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      continue;

    if((*pte & PTE_V) == 0)
      continue;

    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap_mmap: not a leaf");

    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }

    *pte = 0;
  }
}
```

---

## 19. fork 处理

fork 后子进程需要继承父进程的 VMA。

至少要做：

```text
1. 复制 VMA 元数据
2. 对 VMA 中的 struct file 调用 filedup()
3. 处理父进程已经映射出来的 mmap 页
```

题目允许子进程不与父进程共享物理页，但为了保证 fork 后子进程能看到父进程 fork 前已经写入 mmap 内存的内容，可以复制父进程已经映射的 mmap 页。

### fork 中复制 VMA

```c
for(int i = 0; i < NVMA; i++){
  np->vmas[i] = p->vmas[i];

  if(np->vmas[i].used && np->vmas[i].file){
    filedup(np->vmas[i].file);

    if(uvmcopy_mmap(p->pagetable, np->pagetable, &np->vmas[i]) < 0)
      goto bad;
  }
}
```

### 为什么需要 filedup？

因为父子进程都持有同一个 `struct file *`。

如果 fork 时不增加引用计数，那么父进程 munmap 或 exit 调用 `fileclose()` 后，子进程 VMA 中的 file 指针可能变成悬空指针。

---

## 20. 复制 mmap 页

如果父进程已经访问过某些 mmap 页，这些页可能包含尚未写回文件的修改。

fork 后子进程应该看到 fork 时刻的内存视图。

因此可以复制父进程中已经存在的 mmap 页：

```c
int
uvmcopy_mmap(pagetable_t old, pagetable_t new, struct vma *v)
{
  for(uint64 a = v->addr; a < v->addr + v->length; a += PGSIZE){
    uint64 pa = walkaddr(old, a);
    if(pa == 0)
      continue;

    char *mem = kalloc();
    if(mem == 0)
      return -1;

    memmove(mem, (char*)pa, PGSIZE);

    int perm = PTE_U;
    if(v->prot & PROT_READ)
      perm |= PTE_R;
    if(v->prot & PROT_WRITE)
      perm |= PTE_W;

    if(mappages(new, a, PGSIZE, (uint64)mem, perm) < 0){
      kfree(mem);
      return -1;
    }
  }

  return 0;
}
```

---

## 21. exit 处理

进程退出时，要像对所有有效 VMA 调用 `munmap` 一样清理：

```c
for(int i = 0; i < NVMA; i++){
  if(p->vmas[i].used){
    do_munmap(p->vmas[i].addr, p->vmas[i].length);
  }
}
```

建议顺序：

```text
先清理 mmap
再关闭普通 fd
```

原因：

- mmap 对文件有额外引用；
- MAP_SHARED 写回需要文件 inode；
- 提前清理 mmap 更符合资源生命周期。

因为 `mmap()` 中已经 `filedup()`，即使普通 fd 先关闭，理论上也不会立即释放文件对象，但从逻辑上先 munmap 更清晰。

---

## 22. allocproc / freeproc 处理

VMA 是 `struct proc` 的一部分，生命周期伴随进程。

### allocproc

新进程分配时，应清零 VMA 表：

```c
memset(p->vmas, 0, sizeof(p->vmas));
```

### freeproc

进程结构复用前，也可以清零：

```c
memset(p->vmas, 0, sizeof(p->vmas));
```

注意：

- `exit()` 负责真正释放资源、写回文件、fileclose；
- `freeproc()` 主要是兜底清理结构体字段，不能依赖它完成 mmap 语义。

---

## 23. 本次遇到的主要困难与解决过程

### 困难 1：`user.h` 中返回类型不清楚

最初疑问：

```c
void *munmap(void *addr, size_t length);
```

问题：

- `mmap` 返回地址，所以是 `void *`；
- `munmap` 只表示成功失败，应返回 `int`。

最终：

```c
void *mmap(void *addr, uint64 length, int prot, int flags,
           int fd, uint64 offset);
int munmap(void *addr, uint64 length);
```

---

### 困难 2：VMA 生命周期不清楚

问题：

- VMA 是不是跟着 proc 创建和删除？
- allocproc / freeproc / fork / exit 分别做什么？

最终理解：

| 位置 | 职责 |
|---|---|
| `allocproc()` | 初始化 VMA 表 |
| `freeproc()` | 清零 VMA 字段，避免 proc 复用污染 |
| `fork()` | 复制 VMA，增加文件引用计数，复制已映射 mmap 页 |
| `exit()` | 执行 munmap 语义，写回并释放 mmap 资源 |

---

### 困难 3：头文件 incomplete type 报错

遇到错误：

```text
field ‘lock’ has incomplete type
dereferencing pointer to incomplete type ‘struct file’
```

原因：

- `proc.h` 中有 `struct spinlock lock`，必须在包含 `proc.h` 前包含 `spinlock.h`；
- VMA 中保存 `struct file *`，在 `proc.h` 中可用前向声明；
- 但在 `.c` 文件中访问 `v->file->ip` 时，必须包含 `file.h`。

解决原则：

```text
头文件中只保存指针：用 struct file; 前向声明
源文件中访问字段：include "file.h"
include 顺序：spinlock.h 在 proc.h 前面
```

---

### 困难 4：第一页 page fault 不命中 VMA

错误写法：

```c
va0 > v->addr
```

导致访问 mmap 返回地址第一页时，`va0 == v->addr`，无法命中 VMA。

正确写法：

```c
va0 >= v->addr && va0 < v->addr + v->length
```

VMA 区间应按左闭右开理解：

```text
[start, end)
```

---

### 困难 5：munmap 遇到未映射页 panic

原因：

- mmap 是懒加载；
- VMA 中有些页可能从未访问；
- 原版 `uvmunmap()` 对未映射页会 panic。

解决：

实现 mmap 专用 `uvmunmap_mmap()`，遇到没有 PTE 或无效 PTE 时直接跳过。

---

### 困难 6：MAP_SHARED 写回时机

问题：

- 如果先 unmap 再写回，物理页已经释放，内容丢失；
- 必须在 `uvmunmap_mmap()` 前写回文件。

正确顺序：

```text
找到 VMA
检查范围
MAP_SHARED 写回
uvmunmap_mmap 解除映射
更新 / 清空 VMA
```

---

### 困难 7：fork_test 中 `wanted 'A', got 0x0`

现象：

```text
fork_test starting
mismatch at 2048, wanted 'A', got 0x0
```

排查过程：

1. 怀疑 fork 未复制 VMA；
2. 补充 `np->vmas[i] = p->vmas[i]`；
3. 补充 `filedup()`；
4. 补充复制父进程已映射 mmap 页；
5. 补充 MAP_SHARED 写回；
6. 最终定位到 VMA 缺少 `offset` 字段。

根因：

- `munmap()` 从开头裁剪 VMA 后，只更新了 `v->addr`；
- 没有维护剩余 VMA 对应的文件偏移；
- 后续 page fault / writeback 使用 `va - v->addr`，导致读写文件偏移错误。

最终修复：

```c
v->offset + (va - v->addr)
```

以及从开头裁剪时：

```c
v->addr += len;
v->offset += len;
v->length -= len;
```

---

## 24. 本实验最核心的公式

### 1. 映射长度页对齐

```c
maplen = PGROUNDUP(length);
```

### 2. fault 地址页对齐

```c
va0 = PGROUNDDOWN(va);
```

### 3. 判断地址属于 VMA

```c
va0 >= v->addr && va0 < v->addr + v->length
```

### 4. 文件真实偏移

```c
file_offset = v->offset + (va0 - v->addr);
```

### 5. MAP_SHARED 写回偏移

```c
write_offset = v->offset + (a - v->addr);
```

### 6. 从开头裁剪 VMA

```c
v->addr += len;
v->offset += len;
v->length -= len;
```

### 7. 从结尾裁剪 VMA

```c
v->length -= len;
```

---

## 25. 本实验学到的知识点

### 1. 系统调用完整链路

从用户函数到内核实现：

```text
user.h 声明
usys.pl 生成用户态 stub
syscall.h 编号
syscall.c 分发表
sys_mmap / sys_munmap 内核实现
```

---

### 2. mmap 的本质

`mmap()` 不等于读文件。

它的本质是：

```text
在进程地址空间中登记一段文件映射区域
```

真正的数据读取发生在 page fault 时。

---

### 3. VMA 是 mmap 的核心元数据

一个 VMA 至少要表达：

```text
这段虚拟地址在哪里
长度是多少
权限是什么
映射方式是什么
对应哪个文件
当前起点对应文件哪个偏移
```

如果元数据不完整，后续 page fault、munmap、fork 都会出问题。

---

### 4. lazy allocation 与 page fault 的配合

mmap 充分体现了虚拟内存思想：

```text
地址空间可以先承诺
物理内存可以后分配
文件内容可以按需加载
```

---

### 5. 文件系统与虚拟内存的交叉

本实验把两个模块连接起来：

```text
page fault
    ↓
kalloc
    ↓
readi
    ↓
mappages
```

解除映射时：

```text
walkaddr
    ↓
writei
    ↓
uvmunmap
    ↓
kfree
```

---

### 6. 文件引用计数的重要性

mmap 后，即使用户 close(fd)，映射仍应有效。

因此 `mmap()` 必须：

```c
filedup(f);
```

整个 VMA 被解除时：

```c
fileclose(v->file);
```

fork 时也必须：

```c
filedup(np->vmas[i].file);
```

---

### 7. MAP_SHARED 与 MAP_PRIVATE 的区别

| 类型 | 修改是否写回文件 |
|---|---|
| `MAP_SHARED` | 是 |
| `MAP_PRIVATE` | 否 |

本实验中 `MAP_PRIVATE` 不需要实现真正 COW，只要 munmap 时不写回即可。

---

### 8. fork 的 mmap 语义

fork 后子进程需要拥有父进程的 mmap 视图。

实现策略：

```text
复制 VMA 元数据
filedup 文件引用
复制父进程已映射出来的 mmap 页
未映射页仍可在子进程中懒加载
```

---

### 9. exit 的资源回收

进程退出时，如果不清理 VMA，会导致：

- 物理页泄漏；
- 文件引用计数泄漏；
- MAP_SHARED 修改未写回；
- VMA 元数据污染。

因此 exit 中要对所有有效 VMA 执行 munmap 语义。

---

### 10. 区分虚拟地址、物理地址、文件偏移

本实验特别容易混淆三类值：

| 类型 | 示例 | 作用 |
|---|---|---|
| 用户虚拟地址 | `va`, `addr`, `v->addr` | 用户进程看到的地址 |
| 物理地址 / 内核可访问地址 | `pa`, `mem` | 实际存放数据的内存 |
| 文件偏移 | `v->offset + (va - v->addr)` | 文件中的位置 |

不能把虚拟地址当成文件偏移，也不能把文件偏移当成物理地址。

---

## 26. 最终测试结果

最终应通过：

```text
$ mmaptest
mmap_test starting
test mmap f
test mmap f: OK
test mmap private
test mmap private: OK
test mmap read-only
test mmap read-only: OK
test mmap read/write
test mmap read/write: OK
test mmap dirty
test mmap dirty: OK
test not-mapped unmap
test not-mapped unmap: OK
test mmap two files
test mmap two files: OK
mmap_test: ALL OK
fork_test starting
fork_test OK
mmaptest: all tests succeeded
```

并且：

```text
$ usertests
ALL TESTS PASSED
```

---

## 27. 复习时的快速检查清单

如果 mmap lab 出错，可以按下面顺序排查：

### mmap 失败

- syscall 编号是否注册？
- `user/usys.pl` 是否添加？
- `Makefile` 是否加入 `_mmaptest`？
- `argfd()` 是否取到文件？
- 文件权限判断是否过严？
- VMA 是否初始化？
- mmap 地址是否撞到 `p->sz`、`TRAPFRAME`？

### page fault 后被 kill

- `usertrap()` 是否处理 scause 13 / 15？
- `r_stval()` 是否传给 `mmap_pagefault()`？
- VMA 判断是否使用 `>=`？
- 是否正确 `PGROUNDDOWN()`？
- `readi()` 是否加 inode lock？
- `readi()` 的 `user_dst` 是否为 0？
- `mappages()` 权限是否包含 `PTE_U`？

### munmap panic

- 是否调用了原版 `uvmunmap()`？
- 是否对未映射页做了跳过处理？
- `addr` 是否页对齐？
- `len` 是否 `PGROUNDUP()`？
- `npages` 是否为 `len / PGSIZE`？

### MAP_SHARED 不生效

- 写回是否发生在 unmap 前？
- 是否只对已映射页写回？
- `writei()` 的 `user_src` 是否为 0？
- 文件偏移是否为 `v->offset + (a - v->addr)`？
- 是否用了 `begin_op()` / `end_op()`？

### fork_test 失败

- fork 是否复制 VMA？
- fork 是否对 VMA 的 file 调用 `filedup()`？
- 是否复制父进程已经映射的 mmap 页？
- VMA 是否维护 `offset`？
- 从开头 munmap 时是否 `v->offset += len`？

---

## 28. 本次实验总结

本实验的难点不在某一个函数，而在多个内核子系统之间的协作：

```text
系统调用
文件描述符
文件引用计数
inode 读写
页表映射
page fault
进程 fork
进程 exit
资源回收
```

最重要的设计思想是：

> mmap 把“文件的一段内容”和“进程虚拟地址的一段区域”建立了逻辑关系。这个关系不是一次性完成的，而是通过 VMA 记录下来，在 page fault、munmap、fork、exit 等不同阶段逐步兑现。

最终真正理解的是：

```text
mmap 阶段：建立关系
page fault 阶段：兑现一页内容
munmap 阶段：结束关系并写回
fork 阶段：复制关系和当前内存视图
exit 阶段：清理所有关系
```

本实验中最有价值的经验是：

> 看到测试失败时，不要只看失败发生的位置。比如 fork_test 报错，根因可能并不在 fork，而是在之前 VMA 裁剪后文件 offset 丢失。内核 bug 很多时候是元数据不完整导致的延迟爆炸。

---

## 29. 一句话记忆版

```text
mmap 只登记 VMA；
page fault 才 kalloc + readi + mappages；
munmap 前 MAP_SHARED 要 writei；
lazy 页 unmap 要跳过未映射页；
fork 要复制 VMA、filedup，并处理已映射页；
VMA 必须维护 offset，否则部分 munmap 后文件偏移会错。
```
