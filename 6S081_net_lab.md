# 6.S081 Net Lab 学习笔记
> MIT 6.S081 Fall 2021 — Lab: Networking (E1000 网卡驱动)

---

## 一、核心知识点

### 1. DMA（Direct Memory Access）
网卡可以**直接读写主内存**，不需要 CPU 参与数据搬运。

| 方向 | 流程 |
|------|------|
| 发送 | CPU 把数据写到内存 → 告诉 E1000 地址 → E1000 自己去读并发出 |
| 接收 | E1000 收到包 → 自己写入内存 → 通知 CPU（中断） |

CPU 和网卡的协调方式：**共享内存 + 寄存器通知**。

---

### 2. 描述符（Descriptor）
E1000 不直接认识 mbuf，认识的是描述符——一个小结构体，是 CPU 和硬件之间的**协议接口**。

**TX 描述符关键字段：**
```c
struct tx_desc {
  uint64 addr;    // 数据地址（m->head）
  uint16 length;  // 数据长度（m->len）
  uint8  cmd;     // 命令标志：EOP | RS
  uint8  status;  // 状态标志：DD 位
};
```

**RX 描述符关键字段：**
```c
struct rx_desc {
  uint64 addr;    // 数据地址（mbuf->head）
  uint16 length;  // E1000 填入的实际包长
  uint8  status;  // 状态标志：DD 位
};
```

**关键标志位：**
- `E1000_TXD_STAT_DD`：TX 发送完成
- `E1000_RXD_STAT_DD`：RX 收到新包
- `E1000_TXD_CMD_RS`：发完后让 E1000 把 DD 置 1
- `E1000_TXD_CMD_EOP`：这是一个完整的以太网帧

---

### 3. 环形缓冲区（Ring Buffer）
TX 和 RX 各有一个大小为 16 的循环队列，通过寄存器里的 tail 索引协调。

```
TX Ring:
[ desc0 | desc1 | desc2 | ... | desc15 ]
           ↑TDH（E1000控制）   ↑TDT（软件控制）

RX Ring:
[ desc0 | desc1 | desc2 | ... | desc15 ]
           ↑RDT（软件控制）    ↑RDH（E1000控制）
```

- **TDT**：软件填完描述符后移动，通知 E1000 有新包
- **RDT**：软件处理完一个槽后移动，告诉 E1000 这个槽可以复用

---

### 4. 两个并行数组的设计
```
tx_ring[i]    → E1000 能看到的描述符（硬件接口）
tx_mbufs[i]   → 软件记录的 mbuf 指针（用于释放）
```
两者共享同一个下标 `i`，永远对应同一个槽位。E1000 不认识 mbuf，`tx_mbufs` 完全是软件自己维护的。

---

### 5. 锁（Spinlock vs Mutex）

| | Mutex（线程 lab） | Spinlock（net lab） |
|---|---|---|
| 运行环境 | 用户空间 | 内核空间 |
| 等待方式 | 睡眠 | 忙等（spin） |
| 适用场景 | 持锁时间较长 | 持锁时间极短 |

内核用 spinlock 是因为中断处理程序中**不能睡眠**。

---

## 二、实现思路

### `e1000_transmit()`

```
1. 加锁
2. 读 regs[E1000_TDT] 得到当前槽索引 idx
3. 检查 tx_ring[idx].status & DD
   → DD=0：上一轮未完成，释放锁，返回 -1
4. DD=1：释放 tx_mbufs[idx] 的旧 mbuf（注意判断是否为 NULL）
5. 填入新描述符：
   - tx_ring[idx].addr   = m->head
   - tx_ring[idx].length = m->len
   - tx_ring[idx].cmd    = RS | EOP
6. tx_mbufs[idx] = m（记录指针供以后释放）
7. regs[E1000_TDT] = (idx + 1) % TX_RING_SIZE（通知 E1000）
8. 释放锁
```

### `e1000_recv()`

```
1. 加锁
2. 循环：
   a. idx = (regs[E1000_RDT] + 1) % RX_RING_SIZE
      注意：RDT 指向最后一个已处理槽，下一个是 RDT+1
   b. 检查 rx_ring[idx].status & DD
      → DD=0：没有新包，break
   c. 取出 rx_mbufs[idx]，从描述符读 length 更新 mbuf->len
   d. 分配新 mbuf，填入 rx_ring[idx].addr，清零 status
      更新 rx_mbufs[idx] = new_mbuf
   e. 更新 regs[E1000_RDT] = idx
   f. 释放锁 → net_rx(mbuf) → 重新加锁
      （因为 net_rx 内部可能触发 transmit，会再次 acquire 同一把锁）
3. 释放锁
```

---

## 三、踩过的坑

### 坑1：运算符优先级
```c
// 错误：== 优先级高于 &
if(rx_ring[idx].status & E1000_RXD_STAT_DD == 0)

// 正确：加括号
if((rx_ring[idx].status & E1000_RXD_STAT_DD) == 0)
```

### 坑2：addr 应填 head 而不是 mbuf 本身
```c
// 错误：E1000 不认识 mbuf 结构体
tx_ring[idx].addr = (uint64)m;

// 正确：填裸数据地址
tx_ring[idx].addr = (uint64)m->head;
```

### 坑3：net_rx 后不能再 mbuffree
`net_rx()` 会接管 mbuf 所有权，内部自己释放。再次 `mbuffree` 会导致双重释放崩溃。

### 坑4：写错寄存器索引
```c
// 错误：把新 tail 写到了 regs[idx]（第 idx 号寄存器）
regs[idx] = (idx + 1) % TX_RING_SIZE;

// 正确：应该写到 TDT 寄存器
regs[E1000_TDT] = (idx + 1) % TX_RING_SIZE;
```
E1000 的 TDT 没有更新，网卡不知道有新包，包根本不会发出去。

### 坑5：先清零描述符再读 length
```c
// 错误：先清零，length 变成 0
rx_ring[idx].status = 0;
mbuf->len = rx_ring[idx].length;  // 读到 0

// 正确：先读，再清零
mbuf->len = rx_ring[idx].length;
rx_ring[idx].status = 0;
```

### 坑6：锁内调用 net_rx 导致死锁
`net_rx` 最终会调用 `e1000_transmit`，而 transmit 也会 `acquire` 同一把锁，持锁时再 acquire 会死锁。解决方式：调用 net_rx 前先 release，调用后再 acquire。

### 坑7：debug printf 污染 make grade 输出
`make grade` 用正则匹配终端输出，debug 打印会导致匹配失败。提交前务必删除所有 printf。

---

## 四、整体数据流

```
发送：
  用户程序
    → e1000_transmit()
    → 填 tx_ring[idx]（addr/len/cmd）
    → 更新 regs[E1000_TDT]
    → E1000 从内存取数据发出

接收：
  E1000 收到以太网帧
    → DMA 写入 rx_ring[idx].addr 指向的 mbuf
    → 触发中断 → e1000_intr() → e1000_recv()
    → 取出 mbuf，更新 len
    → net_rx(mbuf) 交给网络栈
    → 补上新 mbuf，更新 regs[E1000_RDT]
```

---

## 五、关键 API 速查

```c
mbufalloc(0)          // 分配新 mbuf
mbuffree(m)           // 释放 mbuf
net_rx(m)             // 将收到的 mbuf 交给网络栈

regs[E1000_TDT]       // TX tail 寄存器（读/写）
regs[E1000_RDT]       // RX tail 寄存器（读/写）

acquire(&e1000_lock)  // 加锁
release(&e1000_lock)  // 释放锁
```
