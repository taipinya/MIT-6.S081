# 6.S081 Lab Locks 复习总结

> 主题：Memory allocator 与 Buffer cache 的锁竞争优化  
> 目标：理解实现思路、并发正确性、踩坑原因与最终排查过程。

---

## 1. Lab Locks 要解决什么问题

`locks` lab 的核心不是“写出一个功能完全不同的内核模块”，而是在保持 xv6 原有语义正确的前提下，降低热点锁的竞争。

主要涉及两部分：

1. **Memory allocator**  
   原始 xv6 使用一把全局 `kmem.lock` 管理所有空闲物理页。多个 CPU 同时分配或释放页时，会频繁竞争这一把锁。

2. **Buffer cache**  
   原始 xv6 使用一把全局 `bcache.lock` 管理所有磁盘块缓存。文件系统读写频繁时，多个进程会反复争抢同一把锁。

优化方向可以概括为：

```text
全局一把大锁
        ↓
按 CPU / 按 bucket 拆分成多把小锁
        ↓
降低锁竞争，同时保持数据结构一致性
```

---

## 2. Memory allocator 的实现思路

### 2.1 原始问题

原始 xv6 中，所有空闲页都挂在一个全局 freelist 上：

```text
kmem.lock
   ↓
freelist: page -> page -> page -> ...
```

不管哪个 CPU 调用 `kalloc()` 或 `kfree()`，都要获取同一把 `kmem.lock`。在多核环境下，这会造成明显锁竞争。

### 2.2 优化思路：每个 CPU 一个 freelist

改造后，为每个 CPU 维护一个独立的空闲页链表：

```text
CPU0: kmems[0].lock -> freelist0
CPU1: kmems[1].lock -> freelist1
CPU2: kmems[2].lock -> freelist2
...
```

这样大多数情况下：

```text
CPU i 分配页：只访问 kmems[i]
CPU i 释放页：只放回 kmems[i]
```

不同 CPU 访问不同锁，锁竞争会明显下降。

### 2.3 当前实现的主要逻辑

你的 `kalloc.c` 中定义了：

```c
struct kmem{
  struct spinlock lock;
  struct run *freelist;
};

struct kmem kmems[NCPU];
```

初始化时，为每个 CPU 的 freelist 初始化一把锁：

```c
for(int i = 0; i < NCPU; i++){
  initlock(&kmems[i].lock, "kmem");
}
```

释放页时，通过 `cpuid()` 找到当前 CPU，并把页放回当前 CPU 的 freelist：

```c
push_off();
int hart = cpuid();
pop_off();

acquire(&kmems[hart].lock);
r->next = kmems[hart].freelist;
kmems[hart].freelist = r;
release(&kmems[hart].lock);
```

分配页时，优先从当前 CPU 的 freelist 分配：

```c
acquire(&kmems[hart].lock);
r = kmems[hart].freelist;
if(r)
  kmems[hart].freelist = r->next;
```

如果当前 CPU 没有空闲页，再从其他 CPU 的 freelist 中偷一个：

```c
for(int i = (hart + 1) % NCPU; i != hart; i = (i + 1) % NCPU){
  acquire(&kmems[i].lock);
  if(kmems[i].freelist){
    r = kmems[i].freelist;
    kmems[i].freelist = r->next;
    release(&kmems[i].lock);
    break;
  }
  release(&kmems[i].lock);
}
```

### 2.4 需要注意的并发点

#### 1. `cpuid()` 要在关中断状态下调用

因为当前进程可能被调度到其他 CPU 上，所以 xv6 要求调用 `cpuid()` 时关闭中断。

更稳妥的写法是：

```c
push_off();
int hart = cpuid();
acquire(&kmems[hart].lock);
pop_off();
```

或者至少保证：

```text
cpuid() 的调用本身处于 push_off()/pop_off() 保护中。
```

#### 2. 偷页时要小心锁顺序

如果一边持有本 CPU 的锁，一边去拿其他 CPU 的锁，理论上可能形成死锁：

```text
CPU0 持有 lock0，等待 lock1
CPU1 持有 lock1，等待 lock0
```

更清晰、安全的实现方式是：

```text
1. 先尝试从本 CPU freelist 分配；
2. 如果本 CPU freelist 为空，释放本 CPU 的锁；
3. 再逐个获取其他 CPU 的锁进行 steal；
4. 偷到一个 page 后立即释放对应锁。
```

这个思路比“持有自己锁再去偷别人”更容易分析，也更美观。

---

## 3. Buffer cache 的实现思路

### 3.1 原始问题

原始 xv6 中，所有 buffer 都挂在一个全局双向链表中，并由一把全局锁保护：

```text
bcache.lock
   ↓
head <-> buf <-> buf <-> buf <-> ...
```

每次 `bread()` 都会进入 `bget()`，查找或分配 buffer。文件系统并发读写时，所有请求都会争抢 `bcache.lock`。

### 3.2 优化思路：哈希分桶

改造后，根据磁盘块号把 buffer 分配到不同 bucket 中：

```c
static int
hash(uint blockno)
{
  return blockno % NBUCKET;
}
```

整体结构变成：

```text
bucket 0: lock0 -> buf -> buf -> ...
bucket 1: lock1 -> buf -> buf -> ...
bucket 2: lock2 -> buf -> buf -> ...
...
```

查找某个 block 时，只需要进入：

```text
bucket = blockno % NBUCKET
```

这样不同 block 很可能落在不同 bucket，锁竞争下降。

### 3.3 关键正确性要求

Buffer cache 的优化必须满足以下约束：

```text
1. 同一个 (dev, blockno) 最多只能有一个 buf；
2. 每个 buf 必须始终位于且只位于一个 bucket 链表中；
3. 修改 refcnt、dev、blockno、valid 和链表指针时，必须持有对应 bucket lock；
4. 跨 bucket 迁移 buf 时，必须同时保护源 bucket 和目标 bucket；
5. miss 后准备创建新缓存前，必须再次检查目标 bucket，防止别人抢先创建。
```

其中最重要的是第一点：

```text
同一个磁盘块不能被缓存成两份。
```

一旦出现重复缓存，文件系统的 bitmap、inode、log 等元数据就可能被不同 buffer 分别修改和写回，最终表现为很诡异的错误，例如：

```text
panic: balloc: out of blocks
```

---

## 4. 辅助函数设计

相比把所有逻辑都堆进 `bget()`，更推荐拆成几个小函数。

### 4.1 hash 函数

```c
static int
hash(uint blockno)
{
  return blockno % NBUCKET;
}
```

作用：统一 bucket 定位逻辑，避免到处写 `blockno % NBUCKET`。

### 4.2 从 bucket 删除 buffer

```c
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
```

作用：封装双向链表删除逻辑，减少 `bget()` 中的指针操作噪声。

### 4.3 插入 bucket 头部

```c
static void
insert_into_bucket(int buc, struct buf *b)
{
  b->prev = 0;
  b->next = bcache_bucket[buc].head;

  if(bcache_bucket[buc].head)
    bcache_bucket[buc].head->prev = b;

  bcache_bucket[buc].head = b;
}
```

作用：把跨 bucket 偷来的 buffer 挂到目标 bucket。

### 4.4 查找指定 block

```c
static struct buf*
find_in_bucket(int buc, uint dev, uint blockno)
{
  struct buf *b;

  for(b = bcache_bucket[buc].head; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno)
      return b;
  }

  return 0;
}
```

作用：判断目标 block 是否已经缓存。

### 4.5 查找空闲 victim

```c
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
```

作用：在某个 bucket 内找一个最久未使用的空闲 buffer。这里是**局部 LRU**，不是全局 LRU。

---

## 5. 最终 `bget()` 的清晰流程

最终有效方案可以概括为：

```text
1. 锁目标 bucket；
2. 如果目标 block 已经缓存，refcnt++，返回；
3. 如果目标 bucket 有空闲 buffer，直接复用；
4. 如果目标 bucket 没有空闲 buffer，从其他 bucket 偷；
5. 偷 buffer 时同时持有源 bucket 和目标 bucket 的锁；
6. 真正初始化 victim 前，再检查目标 bucket，防止重复缓存；
7. 设置 dev、blockno、valid、refcnt；
8. 释放 bucket lock；
9. 获取 sleeplock；
10. 返回 buffer。
```

流程图可以理解成：

```text
bget(dev, blockno)
        │
        ▼
计算目标 bucket
        │
        ▼
锁目标 bucket
        │
        ├── 命中 block？── 是 ── refcnt++ ── 解锁 ── acquiresleep ── 返回
        │
        ├── 有空闲 buf？── 是 ── 初始化 victim ── 解锁 ── acquiresleep ── 返回
        │
        ▼
解锁目标 bucket
        │
        ▼
遍历其他 bucket 偷空闲 buf
        │
        ▼
同时锁源 bucket 和目标 bucket
        │
        ├── 目标 block 已被别人创建？── 是 ── 使用已有 buf 返回
        │
        ├── 源 bucket 有空闲 buf？── 是 ── 迁移到目标 bucket ── 初始化 ── 返回
        │
        ▼
没有任何空闲 buf
        │
        ▼
panic("bget: no buffers")
```

---

## 6. 原始版本的问题分析

你自己写的版本思路是：

```text
1. 先查目标 bucket；
2. miss 后遍历所有 bucket，找全局 timestamp 最小的空闲 victim；
3. 把 victim 从原 bucket 移到目标 bucket；
4. 再检查目标 bucket 是否已经有人缓存了目标 block；
5. 初始化 victim。
```

这个方向看起来接近“全局 LRU”，但实现难度明显更高。

### 6.1 问题一：逻辑集中在一个函数里，不容易分析

原始代码中同时混合了：

```text
查找命中
全局扫描 victim
候选锁维护
跨 bucket 迁移
重复缓存检查
初始化 victim
```

这些逻辑全部写在一个 `bget()` 里，使得每个分支的锁状态都很难看清楚。

尤其是这类代码：

```c
if(victim && vic_buc != i)
  release(&bcache_bucket[vic_buc].lock);
```

它把“候选 victim 的选择”和“锁的生命周期管理”绑在一起，可读性和可靠性都比较差。

### 6.2 问题二：全局最优 victim 不值得

你的原方案试图找所有 bucket 中 timestamp 最小的空闲 buffer，也就是严格一点的全局 LRU。

但是 locks lab 的目标不是严格实现全局 LRU，而是降低锁竞争。

全局扫描会带来几个问题：

```text
1. 要访问所有 bucket；
2. 锁状态复杂；
3. victim 在扫描结束后可能被别人改变；
4. 实现难度明显高于收益。
```

最终采用的是：

```text
目标 bucket 内局部 LRU
        +
必要时从其他 bucket stealing
```

这是一种近似 LRU。它牺牲了一点替换精度，换来了更简单、更稳定的并发实现。

### 6.3 问题三：victim 的生命周期不好保证

如果在找到 victim 后释放了相关 bucket lock，之后再使用这个 victim，就会出现风险：

```text
找到 victim
  ↓
释放 victim 所在 bucket 的锁
  ↓
别的 CPU 修改 / 迁移 / 使用了 victim
  ↓
当前 CPU 又继续拿旧 victim 做链表操作
  ↓
链表可能损坏，甚至出现重复缓存
```

最终版本避免了这个问题：

```text
只在持有对应 bucket lock 的时候选择和使用 victim。
```

### 6.4 问题四：情况划分不够清晰

原始版本中，`vic_buc == buc`、`vic_buc != buc`、`victim == 0`、目标 block 被别人抢先缓存等情况混在一起。

最终版本把情况拆开：

```text
情况 1：目标 bucket 命中
情况 2：目标 bucket 有空闲 buffer
情况 3：目标 bucket 无空闲，需要跨 bucket steal
情况 4：steal 过程中发现别人已经缓存目标 block
情况 5：完全没有空闲 buffer
```

这样每个分支都比较短，也更容易判断锁是否正确释放。

---

## 7. `brelse()`、`bpin()`、`bunpin()` 的配套修改

### 7.1 `brelse()`

释放 buffer 时，应该减少引用计数。如果引用计数变为 0，更新时间戳：

```c
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
  if(b->refcnt == 0)
    b->timestamp = ticks;
  release(&bcache_bucket[buc].lock);
}
```

这里的 `timestamp` 表示：

```text
该 buffer 最近一次变为空闲的时间。
```

因此 victim 选择时可以找“空闲最久”的 buffer。

### 7.2 `bpin()` 和 `bunpin()`

`bpin()` 增加引用计数，防止日志系统正在使用的 buffer 被回收。

```c
void
bpin(struct buf *b)
{
  int buc = hash(b->blockno);

  acquire(&bcache_bucket[buc].lock);
  b->refcnt++;
  release(&bcache_bucket[buc].lock);
}
```

`bunpin()` 减少引用计数：

```c
void
bunpin(struct buf *b)
{
  int buc = hash(b->blockno);

  acquire(&bcache_bucket[buc].lock);
  b->refcnt--;
  if(b->refcnt < 0)
    panic("bunpin: refcnt");
  if(b->refcnt == 0)
    b->timestamp = ticks;
  release(&bcache_bucket[buc].lock);
}
```

---

## 8. 调试过程总结

### 8.1 最初的错误现象

`make grade` 中出现：

```text
test writebig: panic: balloc: out of blocks
```

这个错误表面上是文件系统没有空闲块，但一开始不能直接认定是磁盘真的不够，因为 buffer cache 错误也可能污染 bitmap，导致 `balloc()` 误判。

### 8.2 先怀疑 buffer cache 一致性

因为修改了 `bio.c`，最初重点怀疑：

```text
1. 是否出现了同一个 block 被缓存两份；
2. 是否有 buffer 从 bucket 链表中丢失；
3. 跨 bucket 迁移时是否破坏了链表；
4. refcnt 是否被错误维护。
```

因此先重构 `bget()`，把逻辑改成更清晰的分桶 stealing 方案。

### 8.3 单独测试与 grade 测试结果不一致

后来发现：

```text
kalloctest 单独运行：通过
bcachetest 单独运行：通过
usertests 单独运行：通过
make grade：失败
```

这说明问题不一定在单个测试本身，而可能和 grade 的测试顺序、文件系统镜像状态有关。

### 8.4 关键验证：单独 grade usertests 通过

单独运行：

```bash
./grade-lab-lock usertests
```

结果通过，说明完整的 `usertests` 在干净环境下并没有问题。

### 8.5 最终原因：FSSIZE 太小

查看 grade 脚本后发现，完整 grade 的测试顺序大致是：

```text
1. kalloctest
2. usertests sbrkmuch
3. bcachetest
4. usertests
```

也就是说，最后一次完整 `usertests` 并不是在完全空白的文件系统压力背景下运行。前面的测试已经消耗了一部分文件系统空间，最后 `writebig` 又需要大量磁盘块，因此默认 `FSSIZE` 太小时会触发：

```text
panic: balloc: out of blocks
```

将 `kernel/param.h` 中的文件系统大小改为：

```c
#define FSSIZE 20000
```

之后：

```bash
make clean
rm -f fs.img xv6.img
make CPUS=1 grade
make grade
```

均通过。

---

## 9. 本次实现中的经验教训

### 9.1 不要为了“理论最优”牺牲实现可靠性

全局 LRU 看起来更精确，但在分桶锁环境下，实现成本很高。相比之下，局部 LRU + stealing 更适合这个 lab：

```text
足够正确
足够简单
锁竞争低
容易通过测试
```

### 9.2 并发代码要先分清“数据归属”

对于 buffer cache，每个 buffer 必须明确属于某个 bucket。

跨 bucket 迁移时，必须问清楚：

```text
当前 buffer 属于哪个 bucket？
我是否持有这个 bucket 的锁？
我要把它移动到哪个 bucket？
我是否也持有目标 bucket 的锁？
```

只要这些问题不清楚，就很容易写出看似能跑、压力测试下出错的代码。

### 9.3 辅助函数能显著降低理解难度

把双向链表操作拆成：

```text
remove_from_bucket()
insert_into_bucket()
find_in_bucket()
find_free_in_bucket()
```

之后，`bget()` 的主逻辑就从“指针细节堆叠”变成“状态流程描述”，代码更容易检查。

### 9.4 单独测试通过不等于 grade 通过

这次最典型的现象是：

```text
单独 usertests 通过
完整 make grade 失败
```

原因不是 usertests 本身错，而是 grade 的测试顺序改变了系统状态。

以后遇到这种情况，应该区分：

```text
1. 单独测试是否通过；
2. grade 中该测试是否独立运行；
3. 前序测试是否会改变 fs.img 或系统状态；
4. 是否需要增大 FSSIZE 或清理测试残留。
```

### 9.5 错误信息可能只是最终表现，不是根因

`balloc: out of blocks` 可能来自两类完全不同的问题：

```text
1. 文件系统真的没有空闲块；
2. buffer cache 错误污染了 bitmap。
```

调试时不能只看 panic 字面意思，要结合：

```text
是否单独通过？
是否 CPUS=1 通过？
是否修改 FSSIZE 后通过？
是否只有 grade 中失败？
```

这次最终证明主要是第一类问题。

---

## 10. 推荐复习版实现原则

### Memory allocator

```text
1. 每个 CPU 一个 freelist；
2. 每个 freelist 一把锁；
3. kalloc 优先从本 CPU freelist 分配；
4. 本 CPU 没有空闲页时，从其他 CPU steal；
5. cpuid() 要在关中断状态下调用；
6. steal 时尽量避免同时持有多把锁，降低死锁风险。
```

### Buffer cache

```text
1. 用 blockno % NBUCKET 定位 bucket；
2. 每个 bucket 一把锁；
3. 命中只锁目标 bucket；
4. 目标 bucket 有空闲 buffer 时直接复用；
5. 目标 bucket 无空闲 buffer 时，从其他 bucket steal；
6. steal 时同时保护源 bucket 和目标 bucket；
7. 初始化 victim 前必须再次检查目标 bucket；
8. 不追求严格全局 LRU，采用分桶近似 LRU；
9. brelse 中 refcnt 归零时更新时间戳；
10. 保证同一 (dev, blockno) 只存在一个缓存副本。
```

---

## 11. 最终通过测试的关键点

最终通过的关键并不是单一修改，而是几个点共同成立：

```text
1. kalloc 使用 per-CPU freelist，降低 kmem 锁竞争；
2. bcache 使用 hash bucket，降低 bcache 锁竞争；
3. bget 重构为清晰的命中、复用、steal 三阶段；
4. 避免全局 victim 扫描带来的复杂锁状态；
5. brelse/bpin/bunpin 正确维护 refcnt；
6. 将 FSSIZE 调整为 20000，避免完整 grade 过程中磁盘块不足；
7. CPUS=1 grade 和默认多 CPU grade 均通过。
```

这次 lab 最值得记住的一句话是：

```text
锁优化不是简单地把一把锁拆成多把锁，而是要重新设计数据归属、迁移规则和失败路径。
```

