# 6.S081 Lab Trap 复习笔记

> 涵盖：RISC-V 汇编基础 · trampoline.S · trap.c · backtrace() · sigalarm/sigreturn

---

## 一、RISC-V 汇编基础

### 1.1 核心指令

| 指令 | 全称 | 含义 |
|------|------|------|
| `sd rs2, offset(rs1)` | Store Doubleword | 把 `rs2` 写入内存地址 `rs1+offset` |
| `ld rd, offset(rs1)` | Load Doubleword | 从内存地址 `rs1+offset` 读64位写入 `rd` |
| `csrrw rd, csr, rs1` | CSR Read & Write | **原子交换**：`rd←csr`，`csr←rs1` |
| `csrr rd, csr` | CSR Read | 读 CSR 寄存器到 `rd` |
| `csrw csr, rs1` | CSR Write | 把 `rs1` 写入 CSR 寄存器 |
| `auipc rd, imm` | Add Upper Imm to PC | `rd = PC + imm<<12`，用于PC相对寻址 |
| `jalr offset(rs1)` | Jump And Link Register | 跳转到 `rs1+offset`，同时 `ra = 下一条指令地址` |

### 1.2 关键寄存器

| 寄存器 | 类型 | 作用 |
|--------|------|------|
| `a0`~`a7` | 通用寄存器 | 函数参数/返回值 |
| `t0`~`t6` | 通用寄存器 | 临时中转，不保证跨函数调用保留 |
| `ra` | 通用寄存器 | 函数返回地址，`jalr` 自动写入 |
| `sp` | 通用寄存器 | 栈指针 |
| `s0/fp` | 通用寄存器 | 帧指针，指向当前栈帧顶部 |
| `satp` | **CSR** | 当前页表根地址，写入即切换地址空间 |
| `sscratch` | **CSR** | supervisor暂存器，trap切换时的"接力棒" |
| `sepc` | **CSR** | trap发生时保存的用户PC，`sret` 返回到此地址 |
| `scause` | **CSR** | trap原因（8=系统调用ecall） |
| `stvec` | **CSR** | trap处理入口地址 |

### 1.3 riscv.h 中的包装函数

这些函数本质是一条内联汇编 CSR 指令：

```c
// 命名规则：r_ = read，w_ = write，后跟寄存器名
uint64 r_sepc()    →  csrr %0, sepc
void   w_sepc(x)   →  csrw sepc, %0
uint64 r_satp()    →  csrr %0, satp
uint64 r_fp()      →  mv %0, s0     // 读帧指针
```

---

## 二、Stack Frame（栈帧）

### 2.1 内存布局

```
高地址
┌─────────────────┐
│      栈 stack   │  局部变量、返回地址、保存的寄存器（向下增长）
├─────────────────┤
│      堆 heap    │  malloc 动态分配（向上增长）
├─────────────────┤
│   数据段 data   │  全局变量、静态变量
├─────────────────┤
│   代码段 text   │  函数指令（机器码）← 指令在这里，不在栈里！
低地址
```

### 2.2 栈帧结构

每次函数调用在栈上分配一块专属区域：

```
┌──────────────┐ ← s0（帧指针，指向帧顶）
│  返回地址 ra │  fp-8
│  旧帧指针 s0 │  fp-16
│  局部变量    │
│  保存的寄存器│
└──────────────┘ ← sp（栈顶）
```

### 2.3 ra 和 s0 的分工

```
ra  →  返回后跳到哪条【指令】（代码段地址）
s0  →  调用者的【栈帧】在哪里（形成链表，支持栈回溯）
```

> ⚠️ ra 是寄存器（全局唯一），多层调用时必须把 ra 保存到栈上，否则被下层调用覆盖。

### 2.4 函数调用汇编模式

```asm
# 函数开头（建立栈帧）
addi sp, sp, -16
sd   ra, 8(sp)      # 保存返回地址
sd   s0, 0(sp)      # 保存旧帧指针
addi s0, sp, 16     # 设置新帧指针

# 函数结尾（销毁栈帧）
ld   ra, 8(sp)
ld   s0, 0(sp)
addi sp, sp, 16
ret
```

---

## 三、Trampoline 与页表切换

### 3.1 为什么需要 Trampoline

trap 发生时硬件切换到 supervisor 模式，但**页表仍是用户页表**。切换到内核页表的代码本身需要在切换前后都可以执行，所以这段代码必须同时映射在两张页表的**相同虚拟地址**处。

```
物理内存中一块页：[物理地址 0x87fff000]

内核页表：虚拟地址 TRAMPOLINE → 0x87fff000
用户页表：虚拟地址 TRAMPOLINE → 0x87fff000
                   ↑ 完全相同     ↑ 完全相同

切换 satp 后 PC 不变，查新页表得到同一物理地址，执行不中断。
```

### 3.2 `w_stvec(TRAMPOLINE + (uservec - trampoline))` 的含义

```
trampoline        链接地址（页起始）
uservec           链接地址
uservec-trampoline  uservec 在页内的偏移量

TRAMPOLINE + (uservec - trampoline)  =  uservec 的虚拟地址
```

不能直接用 `(uint64)uservec`，那是链接地址，不是 CPU 运行时查页表用的虚拟地址。

---

## 四、uservec：用户态 → 内核态

### 4.1 前提状态

| 项目 | 状态 |
|------|------|
| 执行模式 | supervisor（刚切换） |
| 页表 | 用户页表 |
| 所有通用寄存器 | 用户值，不能破坏 |
| `sscratch` | 内核提前存好的 TRAPFRAME 地址 |

### 4.2 工作流程

```
① trap 发生，硬件跳到 uservec

② csrrw a0, sscratch, a0
   ┌─────────────────────────────────┐
   │ a0      ← sscratch (TRAPFRAME) │  a0 现在可作保存基址
   │ sscratch ← a0 (用户的a0值)     │  用户a0暂存
   └─────────────────────────────────┘

③ sd ra/sp/gp... 40(a0) ...
   把所有用户寄存器保存到 TRAPFRAME

④ csrr t0, sscratch
   sd t0, 112(a0)
   把暂存的用户a0也救出来存入 TRAPFRAME

⑤ 从 TRAPFRAME 恢复内核环境：
   ld sp, 8(a0)       ← 内核栈指针
   ld tp, 32(a0)      ← hartid
   ld t0, 16(a0)      ← usertrap() 地址

⑥ ld t1, 0(a0)
   csrw satp, t1      ← 切换内核页表
   sfence.vma         ← 刷新 TLB

⑦ jr t0              ← 跳入 usertrap()
```

---

## 五、userret：内核态 → 用户态

### 5.1 前提状态

| 项目 | 状态 |
|------|------|
| 执行模式 | supervisor（内核态）|
| 页表（satp） | **内核页表** |
| a0 | TRAPFRAME 地址 |
| a1 | 用户页表地址 |

### 5.2 工作流程

```
① csrw satp, a1      ← 切换回用户页表（trampoline双重映射保证不崩溃）
   sfence.vma

② ld t0, 112(a0)     ← 从TRAPFRAME取用户a0
   csrw sscratch, t0  ← 存入sscratch备用

③ ld ra/sp/gp/...    ← 从TRAPFRAME恢复所有寄存器（除a0）

④ csrrw a0, sscratch, a0
   ┌──────────────────────────────────────┐
   │ a0      ← sscratch（用户a0）✓       │
   │ sscratch ← a0（TRAPFRAME地址）       │ ← 为下次trap做准备
   └──────────────────────────────────────┘

⑤ sret               ← 返回用户模式，PC = sepc
```

### 5.3 sscratch 的两个角色

```
进内核时：存 TRAPFRAME 地址  →  给 uservec 找保存区域
出内核时：存 用户a0          →  给 userret 最后一步恢复用户a0
```

---

## 六、trap.c 各函数分工

### 6.1 整体分工

```
trampoline.S                    trap.c
────────────────                ──────────────────────
纯汇编，"搬运工"：               纯C，"决策者"：
・切换页表                       ・判断trap原因
・保存/恢复寄存器                 ・处理系统调用/中断/异常
・跳转                           ・准备返回用户态的环境
```

### 6.2 usertrap()

```c
void usertrap(void) {
    // 1. 确认来自用户态
    // 2. 把 stvec 改成 kernelvec（现在在内核，trap走内核处理）
    w_stvec((uint64)kernelvec);
    // 3. 保存用户PC（sepc可能被覆盖）
    p->trapframe->epc = r_sepc();
    // 4. 判断原因
    if(r_scause() == 8)      // 系统调用
        p->trapframe->epc += 4;  // 跳过 ecall 指令
    // 5. 处理完毕
    usertrapret();
}
```

### 6.3 usertrapret() ★ 保存内核信息的关键

```c
void usertrapret(void) {
    // 把下次进内核需要的信息存入 trapframe
    // （下次 uservec 进来时从这里读）
    p->trapframe->kernel_satp   = r_satp();
    p->trapframe->kernel_sp     = p->kstack + PGSIZE;
    p->trapframe->kernel_trap   = (uint64)usertrap;
    p->trapframe->kernel_hartid = r_tp();

    // 设置 sret 返回状态
    // sstatus.SPP=0 → 返回用户模式
    // sstatus.SPIE=1 → 返回后开中断
    w_sepc(p->trapframe->epc);  // sret 跳到这里

    // 跳入 userret
    uint64 fn = TRAMPOLINE + (userret - trampoline);
    ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}
```

> **为什么在这里存而不是进内核时存？**
> 内核栈、hartid 在每次返回用户态时可能变化（进程被调度到不同CPU核），必须在"即将返回"时写入最新值。

### 6.4 完整生命周期

```
用户 ecall
  → [硬件] 切换supervisor模式，跳 stvec(uservec)
  → [trampoline.S] 保存用户寄存器，切换内核页表，跳 usertrap()
  → [trap.c] 判断处理，调用 usertrapret()
  → [trap.c] 存内核信息到trapframe，设置sepc，跳 userret
  → [trampoline.S] 切换用户页表，恢复寄存器，sret
  → 用户程序继续执行
```

---

## 七、Lab: Backtrace

### 7.1 原理

每个栈帧固定布局：
```
fp-8  : 返回地址 ra
fp-16 : 上一帧的 fp
```

从当前 fp 出发，不断向上（高地址）追溯，打印每帧的 ra，直到超出当前栈页。

### 7.2 终止条件

xv6 每个内核栈恰好占一页（PAGE对齐）：

```c
PGROUNDUP(fp)   // 栈页的高地址边界（终止条件）
PGROUNDDOWN(fp) // 栈页的低地址边界
```

```
高地址
PGROUNDUP(fp) = 0x88000000  ← fp 超过这里就停止
┌──────────────┐
│  最早的帧    │  fp 最大
│  中间帧      │
│  当前帧      │  fp 最小
└──────────────┘
PGROUNDDOWN(fp) = 0x87fff000
低地址
```

### 7.3 实现

```c
void backtrace(void) {
    uint64 fp = r_fp();
    uint64 top = PGROUNDUP(fp);

    while(fp < top) {
        uint64 ra = *(uint64*)(fp - 8);   // 解引用！不是 fp-8 本身
        printf("%p\n", ra);
        fp = *(uint64*)(fp - 16);          // 跳到上一帧
    }
}
```

> ⚠️ **常见错误**：`fp - 8` 是地址值，`*(uint64*)(fp-8)` 才是地址里存的内容。
> 必须先强转为指针类型才能解引用。

### 7.4 在 panic() 中调用

```c
void panic(char *s) {
    printf("panic: %s\n", s);
    backtrace();   // ← 加这里
    panicked = 1;
    for(;;);
}
```

---

## 八、Lab: Alarm（sigalarm / sigreturn）

### 8.1 整体目标

`sigalarm(n, handler)`：每隔 n 个 timer tick，在用户态执行一次 `handler` 函数。

### 8.2 proc 结构体新增字段

```c
int alarm_interval;            // 每隔多少tick触发
void (*alarm_handler)();       // handler函数指针
int alarm_ticks;               // 倒计时计数器
int alarm_flag;                // 防止重入：1=可触发，0=handler执行中
struct trapframe *alarm_trapframe;  // 保存被打断时的完整现场
```

**初始化（allocproc）**：
```c
p->alarm_interval = 0;
p->alarm_ticks = 0;
p->alarm_handler = 0;
p->alarm_flag = 1;
if((p->alarm_trapframe = (struct trapframe*)kalloc()) == 0) { ... }
```

**释放（freeproc）**：
```c
if(p->alarm_trapframe)
    kfree((void*)p->alarm_trapframe);
p->alarm_trapframe = 0;
```

### 8.3 sys_sigalarm()

```c
uint64 sys_sigalarm(void) {
    int interval;
    uint64 handler;
    argint(0, &interval);
    argaddr(1, &handler);

    struct proc *p = myproc();
    p->alarm_interval = interval;
    p->alarm_handler = (void(*)())handler;
    p->alarm_ticks = interval;
    return 0;
}
```

### 8.4 usertrap() 中触发 handler ★ 核心 trick

```c
if(which_dev == 2) {  // timer interrupt
    if(p->alarm_interval != 0 && p->alarm_flag == 1) {
        p->alarm_ticks--;
        if(p->alarm_ticks == 0) {
            p->alarm_flag = 0;
            // 保存完整现场（值拷贝，不是指针赋值！）
            *p->alarm_trapframe = *p->trapframe;
            // 修改 epc，返回用户态时跳到 handler
            p->trapframe->epc = (uint64)p->alarm_handler;
            p->alarm_ticks = p->alarm_interval;
        }
    }
    yield();
}
```

> ⚠️ **不能直接调用 `p->alarm_handler()`**！
> handler 是用户空间函数，内核态直接调用会因页表/特权级不对而崩溃。
> 正确做法：修改 `trapframe->epc`，让 `sret` 返回时自然跳到 handler。

### 8.5 sys_sigreturn()

handler 执行完毕调用 `sigreturn`，恢复被打断时的完整现场：

```c
uint64 sys_sigreturn(void) {
    struct proc *p = myproc();
    // 恢复完整现场（值拷贝）
    *p->trapframe = *p->alarm_trapframe;
    // 解除重入锁
    p->alarm_flag = 1;
    return 0;
}
```

### 8.6 判断条件的选择

```c
// ❌ 错误：handler地址可能恰好是0（如alarmtest中periodic在地址0x0）
if(p->alarm_handler != 0) { ... }

// ✓ 正确：用interval判断是否设置了alarm
if(p->alarm_interval != 0) { ... }
```

### 8.7 完整流程

```
sigalarm(2, periodic) 被调用
  → alarm_interval=2, alarm_ticks=2, alarm_flag=1

timer interrupt（每个tick）：
  → alarm_ticks--
  → 为0时：
      保存 trapframe 现场
      修改 epc = handler 地址
      alarm_flag = 0（防重入）
      重置 alarm_ticks

sret 返回用户态 → 跳到 handler 执行

handler 执行完，调用 sigreturn：
  → 恢复 trapframe 现场
  → alarm_flag = 1（解锁）
  → 回到被打断的用户代码继续执行
```

---

## 九、关键易错点总结

| 错误 | 正确 |
|------|------|
| `p->alarm_trapframe = p->trapframe` | `*p->alarm_trapframe = *p->trapframe` |
| `p->trapframe = p->alarm_trapframe` | `*p->trapframe = *p->alarm_trapframe` |
| `p->alarm_handler()` 直接调用 | 修改 `p->trapframe->epc` |
| `uint64 ra = fp - 8` | `uint64 ra = *(uint64*)(fp - 8)` |
| `if(alarm_handler != 0)` 判断是否设置 | `if(alarm_interval != 0)` |
| `w_stvec((uint64)uservec)` 用链接地址 | `w_stvec(TRAMPOLINE + (uservec - trampoline))` |

---

*6.S081 xv6 Lab Trap · 复习笔记*
