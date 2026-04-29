# 🧵 6.S081 Lab Thread 复习笔记

> **核心主题**：用户级线程切换 · 多线程并发安全 · 线程同步屏障

---

## 📋 目录

- [任务一：Uthread 用户级线程切换](#任务一)
- [任务二：并发哈希表](#任务二)
- [任务三：Barrier 屏障](#任务三)
- [踩坑总结](#踩坑总结)
- [核心知识点速查](#核心知识点速查)

---

<a name="任务一"></a>
## 任务一：Uthread — 用户级线程切换

### 🎯 目标
在 `user/uthread.c` 和 `user/uthread_switch.S` 中实现用户级线程的**创建**和**上下文切换**。

---

### 💡 核心概念

#### Callee-save 寄存器（RISC-V）
被调用的函数**必须保证**这些寄存器在返回后值不变：

| 寄存器 | 别名 | 说明 |
|--------|------|------|
| x1 | ra | 返回地址 ⭐ |
| x2 | sp | 栈指针 ⭐ |
| x8 | s0/fp | 帧指针 |
| x9 | s1 | 保存寄存器 |
| x18-x27 | s2-s11 | 保存寄存器 |

> 💬 **为什么只保存 callee-save？**
> `thread_switch` 本身是函数调用，编译器调用它之前已经自动处理好了 caller-save 寄存器。

---

### 🏗️ 数据结构设计

在 `struct thread` 中新增 `context` 字段，**直接存寄存器的值**（不是地址）：

```c
struct context {
    uint64 ra;    // 关键：决定切换后从哪里执行
    uint64 sp;    // 关键：决定用哪个栈
    uint64 s0;
    uint64 s1;
    // ... s2-s11
};

struct thread {
    char           stack[STACK_SIZE];  // 线程运行时的调用栈
    int            state;
    struct context context;            // 寄存器快照（全局内存中）
};
```

> ⚠️ **常见误解**：`context` 不在 `stack` 里，是 `struct thread` 的独立字段，存在全局内存中。
> `context.sp` 的值**指向** stack，但 context 本身和 stack 是平级的两块区域。

```
struct thread (全局内存):
┌──────────────────────┐
│ stack[STACK_SIZE]    │  ← 线程运行时用的栈（局部变量、调用帧）
├──────────────────────┤
│ state                │
├──────────────────────┤
│ context.ra = 0x1234  │  ← 直接存值（快照）
│ context.sp = 0x5000  │ ──────────────────────────→ 指向上面的 stack
│ context.s0 = 0x0000  │
│ ...                  │
└──────────────────────┘
```

---

### ✍️ thread_create 实现

创建线程 = **伪造一个"曾经被切走"的现场**：

```c
void thread_create(void (*func)()) {
    struct thread *t = ...; // 找空闲槽
    t->state = RUNNABLE;

    // ⭐ 设置 ra：第一次切换过来时，ret 跳到 func
    t->context.ra = (uint64)func;

    // ⭐ 设置 sp：指向栈顶（注意：栈向下增长！）
    t->context.sp = (uint64)(t->stack + STACK_SIZE);
}
```

```
stack 内存布局（栈向下增长）：
stack[0]                    stack[STACK_SIZE]
   |___________________________|
   低地址                    高地址
                               ↑
                          sp 从这里开始，向左增长
```

---

### ✍️ thread_switch 实现（汇编）

```asm
# a0 = 当前线程 context 指针
# a1 = 下一个线程 context 指针

thread_switch:
    # 保存当前线程寄存器 → a0 指向的 context
    sd ra,  0(a0)
    sd sp,  8(a0)
    sd s0,  16(a0)
    # ... s1-s11

    # 恢复下一个线程寄存器 ← a1 指向的 context
    ld ra,  0(a1)
    ld sp,  8(a1)
    ld s0,  16(a1)
    # ... s1-s11

    ret   # 跳到 ra（第一次=func开头，后续=上次断点）
```

---

### 🔄 完整调度流程

```
某线程运行中，调用 thread_yield()
        │
        ▼
current->state = RUNNABLE   // 让出但还能继续
thread_schedule()
        │
        ▼
找到下一个 RUNNABLE 线程 next
next->state = RUNNING
        │
        ▼
thread_switch(&old->context, &next->context)
        │
   ┌────┴────┐
   保存 old   恢复 next
   寄存器     寄存器
        │
        ▼
      ret → 跳到 next->context.ra

第一次调度：ra = func → 从函数开头执行
后续切回来：ra = thread_switch 调用处的下一条指令 → 继续执行
```

---

### 🔑 ra 在哪里被设置？

| 时机 | ra 的值 | 谁来设置 |
|------|---------|---------|
| **第一次调度** | `func` 的地址 | 你在 `thread_create` 里手动写 |
| **切换回来** | `thread_switch` 调用处的下一条指令 | CPU 自动存入 ra，switch 保存进 context |

---

<a name="任务二"></a>
## 任务二：并发哈希表

### 🎯 目标
修复 `notxv6/ph.c` 中多线程哈希表的竞争问题，在真实 Linux 机器上运行（非 xv6）。

### 🔍 为什么会丢 key？

```
两个线程同时插入同一个桶：

线程0：e0->next = 旧头
线程1：e1->next = 旧头   ← 也拿到了旧头，不知道线程0存在
线程0：*p = e0           ← 头更新为 e0
线程1：*p = e1           ← 头更新为 e1，e0 从链表消失！
                                          ↑ key 丢失！
```

### ✍️ 解决方案：每个桶一把锁

```c
// ❌ 错误：一把全局锁，ph_fast 无法通过（完全串行）
pthread_mutex_t global_lock;

// ✅ 正确：每个桶一把锁，不同桶可以真正并行
pthread_mutex_t locks[NBUCKET];

// 初始化
for (int i = 0; i < NBUCKET; i++)
    pthread_mutex_init(&locks[i], NULL);

// put() 里
int bucket = key % NBUCKET;
pthread_mutex_lock(&locks[bucket]);
// ... insert ...
pthread_mutex_unlock(&locks[bucket]);
```

### 🧪 测试

```bash
make ph
./ph 1    # 单线程，应该 0 keys missing
./ph 2    # 双线程，加锁后应该 0 keys missing
make grade  # ph_safe + ph_fast 都要过
```

**ph_fast 要求**：两线程 puts/s ≥ 单线程的 1.25 倍（必须用分桶锁才能达到）

### 📝 需要写 answers-thread.txt
描述两线程丢 key 的具体事件序列（见上面的竞争分析）。

---

<a name="任务三"></a>
## 任务三：Barrier — 线程屏障

### 🎯 目标
实现 `barrier()`，让所有线程在此等待，直到所有线程都到达，才能继续下一轮。

### 💡 Barrier 是什么？

```
3个线程赛跑，每圈结束都要等齐再开始下一圈：

第1圈：
线程A ──────────────→ 到终点，等待...
线程B ──────→ 到终点，等待...
线程C ────────────────────→ 到终点

✅ 所有人到齐！开始第2圈
```

### 🏗️ 数据结构

```c
struct barrier {
    pthread_mutex_t barrier_mutex;  // 保护共享变量的锁
    pthread_cond_t  barrier_cond;   // 等待队列（"睡眠室"）
    int nthread;  // 当前轮次已到达的线程数
    int round;    // 当前轮次编号
} bstate;        // 全局变量（类型定义时直接声明）
```

> 💬 **`pthread_cond_t` 是什么？**
> 一个等待队列，存放正在睡眠的线程。`broadcast()` 把队列里所有线程唤醒。

### ✍️ 正确实现

```c
static void barrier() {
    pthread_mutex_lock(&bstate.barrier_mutex);

    bstate.nthread++;

    if (bstate.nthread == nthread) {
        // 我是最后一个到的
        bstate.nthread = 0;  // ⭐ 重置计数（为下一轮准备）
        bstate.round++;      // ⭐ 先递增轮次
        pthread_cond_broadcast(&bstate.barrier_cond);  // 再唤醒
    } else {
        // 不是最后一个，等待
        int my_round = bstate.round;             // 记住当前轮次
        while (bstate.round == my_round) {       // ⭐ 循环等待
            pthread_cond_wait(&bstate.barrier_cond, &bstate.barrier_mutex);
        }
    }

    pthread_mutex_unlock(&bstate.barrier_mutex);
}
```

### ⚠️ 关键细节

#### 1. 为什么要循环 while，不用 if？
**虚假唤醒（spurious wakeup）**：POSIX 标准允许 `cond_wait` 在没有 `broadcast` 的情况下自己醒来。

```c
// ❌ 危险：虚假唤醒后条件没满足，继续跑 → 错误
if (bstate.round == my_round)
    pthread_cond_wait(...);

// ✅ 安全：醒来重新检查，没变就继续睡
while (bstate.round == my_round)
    pthread_cond_wait(...);
```

#### 2. round++ 必须在 broadcast 之前
```c
// ❌ 错误：线程醒来发现 round 还没变，assert(i == t) 失败
pthread_cond_broadcast(...);
bstate.round++;

// ✅ 正确
bstate.round++;
pthread_cond_broadcast(...);
```

#### 3. cond_wait 会自动释放锁！

```
线程B 调用 cond_wait → 原子地：释放锁 + 进入睡眠
线程C 调用 cond_wait → 原子地：释放锁 + 进入睡眠

此时锁空闲，其他线程可以进入 barrier()

线程A（最后到达）→ 拿到锁 → nthread++ → 清零 → round++ → broadcast
线程B、C 被唤醒 → 重新拿锁 → 检查 round 变了 → 退出循环
```

### 🧪 测试

```bash
make barrier
./barrier 2
./barrier 4
make grade
```

---

<a name="踩坑总结"></a>
## 🪤 踩坑总结

### 任务一

| 问题 | 错误理解 | 正确理解 |
|------|---------|---------|
| context 存什么 | 存寄存器的地址 | 直接存寄存器的值（快照） |
| context 在哪里 | 在 stack 里 | 和 stack 平级，在全局内存 |
| sp 为什么是 stack+SIZE | 不理解 | 栈向下增长，sp 从高地址开始 |

### 任务三

| 问题 | 错误写法 | 原因 |
|------|---------|------|
| 共享变量未加锁 | `pcount++` 直接写 | 多线程同时 ++ 会丢失更新 |
| 用了错误的全局变量 | `round++`（局部static）| 应该用 `bstate.round` |
| 没有清零 nthread | 忘了重置 | 下一轮计数会从错误值开始 |
| 单次 if 而非 while | `if (... == my_round) wait` | 虚假唤醒会导致提前退出 |
| 语法错误 | `&bstate.barrier_cond.` | 句号应为逗号 |

---

<a name="核心知识点速查"></a>
## 📚 核心知识点速查

### 多线程思维框架

> 对每一个**共享变量**，问自己：
> 1. 会被多个线程读写吗？→ 是 → 有锁保护吗？→ 没有 → 加锁

### pthread API 速查

```c
// 互斥锁
pthread_mutex_t lock;
pthread_mutex_init(&lock, NULL);
pthread_mutex_lock(&lock);
pthread_mutex_unlock(&lock);

// 条件变量
pthread_cond_t cond;
pthread_cond_wait(&cond, &mutex);      // 睡眠 + 释放锁（原子）
pthread_cond_broadcast(&cond);          // 唤醒所有等待线程

// 线程
pthread_t tid;
pthread_create(&tid, NULL, func, arg); // 创建线程
pthread_join(&tid, &retval);           // 等待线程结束
```

### 用户级线程 vs pthread

| | 任务一 uthread | 任务二 pthread |
|--|--|--|
| 调度者 | 你写的 thread_schedule | 操作系统内核 |
| 并发 | 假并发，同一时刻只有一个跑 | 真并发，多核同时执行 |
| 切换时机 | 显式调用 yield | 内核随时可以抢占 |
| 竞争条件 | 不会有 | 真实存在，必须加锁 |

---

*生成于 6.S081 Lab Thread 学习过程 · 祝复习顺利 🎉*
