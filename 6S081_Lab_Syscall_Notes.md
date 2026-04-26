# 6.S081 Syscall Lab 复习笔记

> 涵盖 **System Call Tracing** 和 **Sysinfo** 两个实验

---

## 一、核心概念：系统调用的完整流程

理解这张图是做好这两个 lab 的基础：

```
用户程序 调用 trace(mask)
    │
    │  user/user.h        → 声明函数原型，让用户程序知道函数存在
    │  user/usys.pl       → 生成汇编桩，把系统调用编号塞进 a7 寄存器
    │  kernel/syscall.h   → 定义编号常量 SYS_trace = 22（两端共享的"暗号"）
    │
    ▼
  ecall 指令 → 陷入内核，CPU 跳转到 trap handler
    │
    ▼
kernel/syscall.c  syscall()
    │  读取 a7 寄存器中的编号
    │  查 syscalls[] 函数指针数组，找到对应处理函数
    │
    ▼
kernel/sysproc.c  sys_trace()
    │  用 argint() / argaddr() 读取用户传来的参数
    │  执行具体逻辑
    │  返回值写入 trapframe->a0
    │
    ▼
返回用户态
```

---

## 二、为什么同一个系统调用有四种命名？

以 `sysinfo` 为例：

| 名称 | 文件 | 作用 |
|------|------|------|
| `sysinfo` | `user/user.h` | 用户程序调用的函数名 |
| `SYS_sysinfo` | `kernel/syscall.h` | 编号常量，两端共享 |
| `[SYS_sysinfo] sys_sysinfo` | `kernel/syscall.c` | 编号到内核函数的映射表 |
| `sys_sysinfo()` | `kernel/sysproc.c` | 内核实际执行的函数，`sys_` 前缀做命名空间隔离 |

**本质原因**：系统调用横跨用户态和内核态两个世界，命名统一会导致符号冲突，每一层的职责不同，命名反映职责。

---

## 三、Lab 1：System Call Tracing

### 任务目标

新增 `trace(mask)` 系统调用，当进程调用任何系统调用时，若该调用的编号对应 mask 中某个 bit，则打印日志：

```
pid: syscall 调用名 -> 返回值
```

### 实现步骤

#### Step 1：用户态注册（让代码能编译）

```
Makefile        → 加 $U/_trace
user/user.h     → 加 int trace(int);
user/usys.pl    → 加 entry("trace");
kernel/syscall.h → 加 #define SYS_trace 22
```

#### Step 2：给进程添加 tracemask 字段

```c
// kernel/proc.h，放在第三块末尾（私有字段，无需持锁）
int tracemask;   // Trace mask for syscall tracing
```

> **为什么不能整块复制 proc 结构体？**
> 因为 `pid`、`state`、`lock`、`kstack` 等字段子进程必须重新初始化，不能继承父进程的值。

#### Step 3：实现 sys_trace()

```c
// kernel/sysproc.c
uint64
sys_trace(void)
{
  int mask;
  if(argint(0, &mask) < 0)
    return -1;
  myproc()->tracemask = mask;
  return 0;   // 系统调用成功惯例返回 0
}
```

#### Step 4：fork() 继承 tracemask

```c
// kernel/proc.c fork() 函数中
np->tracemask = p->tracemask;
```

#### Step 5：在 syscall() 中打印日志

```c
// kernel/syscall.c，新增系统调用名称数组
static char *syscall_names[] = {
  "",        // 0 占位
  "fork",    // 1
  "exit",    // 2
  // ... 按顺序填到
  "trace",   // 22
};

// 修改 syscall() 函数
void syscall(void) {
  int num;
  struct proc *p = myproc();
  num = p->trapframe->a7;
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    p->trapframe->a0 = syscalls[num]();
    // 位运算检查：第 num 个 bit 是否在 mask 中
    if(p->tracemask & (1 << num)) {
      printf("%d: syscall %s -> %d\n",
             p->pid, syscall_names[num], p->trapframe->a0);
    }
  }
}
```

### 关键知识点

**位运算检查 mask：**
```c
// 不是判断 mask 在 1-22 之间！
// 而是检查第 num 个 bit 是否被设置
if(p->tracemask & (1 << num))

// 例：trace(1 << SYS_read) 设置了第5位
// 只有 num=5 时，(1<<5) & mask 非零，才打印
```

**返回值在哪里：** 系统调用执行完后返回值存在 `p->trapframe->a0`。

---

## 四、Lab 2：Sysinfo

### 任务目标

新增 `sysinfo(struct sysinfo *)` 系统调用，填充：
- `freemem`：空闲内存字节数
- `nproc`：状态不为 UNUSED 的进程数

### 实现步骤

#### Step 1：用户态注册（同 trace，多一个结构体前向声明）

```c
// user/user.h
struct sysinfo;               // 前向声明
int sysinfo(struct sysinfo *);
```

#### Step 2：获取空闲内存

```c
// kernel/kalloc.c
// kmem.freelist 是单链表，每个节点是一个空闲物理页（4096字节）
uint64
kget_freemem(void)
{
  struct run *r;
  uint64 count = 0;

  acquire(&kmem.lock);
  r = kmem.freelist;
  while(r) {
    r = r->next;
    count++;
  }
  release(&kmem.lock);

  return count * PGSIZE;  // PGSIZE 已在 riscv.h 定义，无需手动定义
}
```

#### Step 3：获取进程数量

```c
// kernel/proc.c
// proc[NPROC] 是全局进程数组
uint64
get_nproc(void)
{
  uint64 count = 0;
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);          // 访问 state 需要持锁
    if(p->state != UNUSED)
      count++;
    release(&p->lock);
  }
  return count;
}
```

#### Step 4：实现 sys_sysinfo()，用 copyout 拷回用户空间

```c
// kernel/sysproc.c
// 需要在文件开头添加：
#include "sysinfo.h"
extern uint64 kget_freemem(void);
extern uint64 get_nproc(void);

uint64
sys_sysinfo(void)
{
  uint64 addr;
  struct sysinfo info;
  struct proc *p = myproc();

  if(argaddr(0, &addr) < 0)    // 读取用户传来的指针地址
    return -1;

  info.freemem = kget_freemem();
  info.nproc = get_nproc();

  // copyout(页表, 用户地址, 内核数据地址, 大小)
  if(copyout(p->pagetable, addr, (char *)&info, sizeof(info)) < 0)
    return -1;

  return 0;
}
```

### 关键知识点

**为什么需要 copyout？**
内核和用户程序处于不同的地址空间，不能直接赋值。`copyout` 通过页表把内核数据安全地写入用户空间的地址。

**为什么用 extern 而不是 #include .c 文件？**
`#include` 只能包含头文件。`extern` 声明告诉编译器"这个函数在别的 .c 文件里实现，链接时会找到它"。

**argaddr vs argint：**
| 函数 | 用途 |
|------|------|
| `argint(n, &val)` | 读第 n 个参数，整数 |
| `argaddr(n, &val)` | 读第 n 个参数，地址/指针 |
| `argstr(n, buf, max)` | 读第 n 个参数，字符串 |

---

## 五、遇到的坑

| 问题 | 原因 | 解决 |
|------|------|------|
| `SYS_trace undeclared` | `syscall.h` 没有保存或未包含 | 检查文件保存，确认 `#include` |
| `kmen.lock` 拼写错误 | 手误 | 改为 `kmem.lock` |
| `sys_sysinfo` 用了 `argfd` | `argfd` 是读文件描述符的，不是指针 | 改用 `argaddr` |
| `storage size of sysinfo isn't known` | 没有 `#include "sysinfo.h"` | 补上头文件 |
| `make` 提示 kernel is up to date | 文件未保存导致时间戳未更新 | `make clean && make` |

---

## 六、proc 结构体字段分区理解

```c
struct proc {
  struct spinlock lock;

  // 需要持 p->lock 才能访问
  enum procstate state;
  int pid;
  // ...

  // 需要持 wait_lock 才能访问
  struct proc *parent;

  // 进程私有，无需持锁
  uint64 kstack;
  struct trapframe *trapframe;
  // ...
  int tracemask;   // ← 新增字段放这里，因为是进程私有的
};
```

**context 两个关键字段：**
- `context.ra`：进程第一次被调度时跳转的地址（新进程跳到 `forkret`）
- `context.sp`：内核栈指针，初始化为 `kstack + PGSIZE`（栈顶，因为栈向下增长）

---

*完成于 6.S081 Syscall Lab*
