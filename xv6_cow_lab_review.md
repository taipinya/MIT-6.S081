# xv6 Copy-On-Write Fork Lab 复习笔记

---

## 一、核心概念

### 什么是 COW Fork？

传统 `fork()` 会把父进程的所有物理页**完整复制**给子进程，代价高且浪费（因为 `fork` 之后往往紧跟 `exec`，复制的内存根本没用到）。

COW（Copy-On-Write）的思路是：

```
fork() 时 ——> 不复制物理页，让父子进程共享同一物理页
                        |
              把共享页的 PTE 都标记为"不可写"
                        |
         某个进程尝试写这个页 ——> 触发 Page Fault
                        |
         内核分配新页，复制内容，更新 PTE 为可写
```

**好处**：推迟复制，只在真正需要时才复制，大幅减少不必要的内存开销。

---

## 二、遇到的困难与解决思路

### 困难 1：为什么父进程也要设为不可写？

**初始误解**：只需要把子进程的 PTE 设为不可写，父进程本来就有写权限，不需要改。

**正确理解**：

```
父子进程共享物理页 P
若只有子进程 PTE 不可写：

父进程直接写 P → P 内容被修改
子进程读 P    → 读到父进程修改后的内容 ← 破坏了进程隔离！
```

**结论**：必须把父子进程的 PTE **都**设为不可写，任何一方写时都触发 Page Fault，让内核介入处理。父进程写 COW 页的处理逻辑与子进程完全对称。

---

### 困难 2：引用计数的设计

**问题**：一个物理页可能被多个进程的页表引用，需要知道何时才能真正释放。

**解决方案**：为每个物理页维护一个引用计数数组。

```c
// 在 kalloc.c 中定义
int cow_refcount[PHYSTOP / PGSIZE];

// 用物理地址除以 PGSIZE 作为下标
cow_refcount[pa / PGSIZE]
```

**各操作对应的引用计数变化**：

| 操作 | 引用计数变化 |
|------|-------------|
| `kalloc()` 分配新页 | 设为 1 |
| `uvmcopy()` 共享父进程页 | +1 |
| `kfree()` 放弃一个引用 | -1，为 0 才真正释放 |
| COW fault 处理完成 | 旧页 `kfree()`（-1），新页 `kalloc()`（设为 1）|

---

### 困难 3：kfree 的语义变化

**初始误解**：只有引用计数为 0 时才调用 `kfree`。

**正确理解**：`kfree` 的新语义是"**放弃对这个页的一个引用**"，内部自动处理是否真正释放：

```c
void kfree(void *pa) {
    acquire(&cow_lock);
    if(cow_refcount[(uint64)pa/PGSIZE] > 0)
        cow_refcount[(uint64)pa/PGSIZE]--;
    int should_free = (cow_refcount[(uint64)pa/PGSIZE] == 0);
    release(&cow_lock);

    if(should_free) {
        memset(pa, 1, PGSIZE);
        // 放回 freelist ...
    }
}
```

**所有释放物理页的地方都统一通过 `kfree` 走**，引用计数管理完全收敛在内部。

---

### 困难 4：为什么减一和判断必须在同一个锁里？

**竞态场景**：

```
CPU0                            CPU1
refcount = 1
refcount-- → 0
                                refcount++ → 1  （uvmcopy 新引用）
判断 == 0 → 释放！             ← 但 CPU1 还在引用这个页！
```

**结论**："减一"和"判断是否为 0"必须原子完成，中间不能有其他进程修改引用计数。

---

### 困难 5：kinit 启动卡死问题

**现象**：xv6 启动卡在 `booting`，无后文。

**原因**：`kinit()` 调用 `freerange()` → `kfree()` 把所有页放入 freelist。但此时 `cow_refcount` 全为 0，修改后的 `kfree` 执行 `refcount--` 变成负数，`should_free` 永远为 false，freelist 为空，内核无法启动。

**修复**：在 `kfree` 里加判断，refcount 已经是 0 时不再减：

```c
if(cow_refcount[(uint64)pa/PGSIZE] > 0)
    cow_refcount[(uint64)pa/PGSIZE]--;
```

---

### 困难 6：`panic: walk` 错误

**出现两次，原因相同**：`walk` 函数内部有检查 `if(va >= MAXVA) panic("walk")`，在调用 `walk` 之前没有验证虚拟地址合法性。

**第一次**：`usertrap` 里 `MAXVAplus` 测试传入超出范围的 va。

**第二次**：`copyout` 里 `walkaddr` 之前先调用了 `walk`，`dstva` 可能超出 `MAXVA`。

**修复**：在每次调用 `walk` 前都加检查：

```c
if(va >= MAXVA) return -1;  // 或 p->killed = 1
```

---

### 困难 7：copyout 为什么不能依赖 usertrap 自动处理 COW？

**误解**：`memmove` 写 COW 页时会触发 Page Fault，自动跳到 `usertrap` 处理。

**真相**：`copyout` 运行在**内核态**，内核态的 Page Fault 跳转到 `kerneltrap`，而不是 `usertrap`。`kerneltrap` 没有 COW 处理逻辑，会直接 `panic`。

**解决**：在 `copyout` 的 while 循环内，每次迭代都**主动检测**当前页是否是 COW 页，若是则手动处理：

```c
pte_t *pte = walk(pagetable, va0, 0);
if(pte && (*pte & PTE_COW)) {
    if(cow_handle(pagetable, va0) < 0)
        return -1;
}
pa0 = walkaddr(pagetable, va0);  // 重新获取新页的物理地址
```

---

## 三、关键实现细节

### PTE_COW 标志位

使用 RISC-V PTE 的 RSW（Reserved for Software）位，在 `riscv.h` 中添加：

```c
#define PTE_COW (1L << 8)
```

**作用**：区分"本来就不可写的页（如代码段）"和"COW 暂时不可写的页"，两者都表现为写时触发 Page Fault，但处理方式不同。

---

### scause == 15 的判断

在 `usertrap` 中，必须先判断 `scause`，再判断 `PTE_COW`：

```c
} else if(r_scause() == 15) {  // store page fault
    uint64 va = r_stval();     // 出错的虚拟地址在 stval 寄存器
    if(va >= MAXVA) { p->killed = 1; }
    else {
        pte_t *pte = walk(p->pagetable, va, 0);
        if(pte == 0 || !(*pte & PTE_COW))
            p->killed = 1;
        else if(cow_handle(p->pagetable, va) < 0)
            p->killed = 1;
    }
}
```

| scause 值 | 含义 |
|-----------|------|
| 8 | 系统调用 (ecall) |
| 12 | 指令 Page Fault |
| 13 | Load Page Fault |
| **15** | **Store/AMO Page Fault ← COW 关注的** |

---

### exit(-1) vs p->killed = 1

| | `exit(-1)` | `p->killed = 1` |
|--|-----------|-----------------|
| 效果 | 立刻终止，不返回 | 设置标志，到安全检查点才退出 |
| 适用场景 | 可以立刻退出时 | 可能持有锁或处于不安全状态时 |
| xv6 惯用法 | ❌ 不推荐在 trap 中使用 | ✅ 推荐，末尾统一检查 |

---

### cow_handle 函数设计

抽取为独立函数，供 `usertrap` 和 `copyout` 共用：

```c
int cow_handle(pagetable_t pagetable, uint64 va) {
    if(va >= MAXVA) return -1;
    
    pte_t *pte = walk(pagetable, va, 0);
    if(pte == 0) return -1;

    char *mem = kalloc();           // 1. 分配新页
    if(mem == 0) return -1;

    uint64 oldpa = PTE2PA(*pte);
    memmove(mem, (char*)oldpa, PGSIZE);  // 2. 复制旧页内容
    kfree((void*)oldpa);            // 3. 放弃旧页引用（refcount-1）

    uint flags = PTE_FLAGS(*pte);
    flags |= PTE_W;                 // 4. 设置可写
    flags &= ~PTE_COW;              // 5. 清除 COW 标志
    *pte = PA2PTE((uint64)mem) | flags;  // 6. 更新 PTE

    return 0;
}
```

**为什么传 `(pagetable, va)` 而不是直接传 `pte`**：函数签名更清晰地表达语义"处理这个地址上的 COW 页"，且 `usertrap` 和 `copyout` 都可以统一调用。

---

## 四、完整实现步骤总结

```
Step 1: kalloc.c
├── 添加 cow_refcount[] 数组和 cow_lock 锁
├── kinit() 中初始化锁
├── kalloc()：分配页时设 refcount = 1
└── kfree()：refcount > 0 则减一，为 0 才真正释放

Step 2: riscv.h
└── 添加 #define PTE_COW (1L << 8)

Step 3: vm.c - uvmcopy()
├── 不再 kalloc 新页，直接映射父进程物理页
├── 父进程 PTE：清除 PTE_W，设置 PTE_COW
├── 子进程 PTE：继承同样的 flags（已含 COW，不含 W）
└── 映射成功后：cow_refcount[pa/PGSIZE]++

Step 4: trap.c - usertrap()
└── 新增 scause == 15 分支
    ├── 检查 va < MAXVA
    ├── 检查 PTE_COW 标志
    └── 调用 cow_handle()，失败则 p->killed = 1

Step 5: vm.c - cow_handle()
└── kalloc + memmove + kfree(旧页) + 更新 PTE

Step 6: vm.c - copyout()
└── while 循环内，walkaddr 之前
    ├── 检查 va0 < MAXVA
    ├── walk 拿 pte，检查 PTE_COW
    ├── 若是 COW 页则调用 cow_handle()
    └── 重新 walkaddr 获取新的 pa0
```

---

## 五、常见 Bug 速查

| Bug 现象 | 原因 | 修复 |
|---------|------|------|
| 启动卡死在 booting | `kfree` 对 refcount=0 的页执行减一，freelist 为空 | `kfree` 中加 `if(refcount > 0)` 判断 |
| `panic: walk` | 调用 `walk` 前未检查 `va >= MAXVA` | 在 `walk` 之前加 `if(va >= MAXVA)` 检查 |
| `mappages: remap` panic | COW fault 后用 `mappages` 更新已存在的 PTE | 直接修改 `*pte`，不用 `mappages` |
| copyout 写 COW 页 panic | 内核态 Page Fault 不走 `usertrap` | `copyout` 循环内主动检测并处理 COW |
| 内存错误释放 | 减一和判断不在同一锁内，存在竞态 | 两步操作放在同一 `acquire/release` 内 |

---

## 六、核心知识点

1. **用户态 vs 内核态 Page Fault 的处理路径不同**：用户态走 `usertrap`，内核态走 `kerneltrap`。
2. **RISC-V `scause` 寄存器**：标识 trap 的原因，`stval` 保存出错的虚拟地址。
3. **RSW 位的使用**：PTE 中第 8、9 位保留给软件，可用于自定义标志如 `PTE_COW`。
4. **引用计数的原子性**：减一和判断是否为零必须在同一把锁的保护下完成。
5. **`kfree` 语义的扩展**：从"立刻释放"变为"放弃一个引用，引用归零才释放"。
6. **接口设计原则**：`cow_handle` 只负责处理 COW，通过返回值传递错误，调用方决定如何响应。
