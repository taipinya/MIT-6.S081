# 6.S081 Lab: File System 复习笔记

> 本笔记总结本次完成 6.S081 File System Lab 过程中涉及的核心任务、实现思路、遇到的问题、调试过程和知识点。重点覆盖 **Large files** 和 **Symbolic links** 两个部分，便于后续快速复习和答辩式回顾。

---

## 一、Lab 总体目标

本次 File System Lab 的核心目标是理解并修改 xv6 文件系统中与 **inode、block 映射、路径查找、系统调用、日志事务** 相关的机制。

主要完成两个功能：

1. **Large files**
   - 原始 xv6 单个文件最大只能使用 `12 + 256 = 268` 个数据块。
   - 本实验通过为 inode 增加 **二级间接块 doubly-indirect block**，扩大单个文件可寻址的数据块数量。

2. **Symbolic links**
   - 为 xv6 增加符号链接机制。
   - 实现新的系统调用：
     ```c
     int symlink(char *target, char *path);
     ```
   - 让 `open()` 默认跟随符号链接，同时支持 `O_NOFOLLOW` 打开符号链接本身。

---

# Part 1：Large Files

## 二、背景知识：inode、direct block、indirect block

### 1. inode 是什么

在 xv6 文件系统中，`inode` 是文件的核心元数据结构。

一个文件并不是直接存放在 inode 里，而是：

```text
inode 保存文件元信息 + 文件数据块地址
真正的文件内容存放在 data block 中
```

inode 中通常包含：

- 文件类型
- 文件大小
- 链接数
- 数据块地址数组 `addrs[]`

也就是说，inode 的重要作用是：

> 告诉文件系统：这个文件的数据分别存放在哪些磁盘块中。

---

### 2. block 是什么

xv6 以 block 为单位管理磁盘。

```c
#define BSIZE 1024
```

即：

```text
1 个 block = 1024 字节
```

文件内容被拆分成多个 block 存储，inode 通过 block number 定位这些数据块。

---

### 3. direct block number

原始 xv6 inode 中有 12 个 direct block numbers：

```text
addrs[0]  -> data block
addrs[1]  -> data block
...
addrs[11] -> data block
```

direct block number 的特点是：

> inode 中直接保存数据块编号。

因此，12 个 direct block 最多可以直接索引：

```text
12 * 1024 = 12288 字节
```

约为 12KB。

---

### 4. singly-indirect block number

原始 xv6 还有 1 个 singly-indirect block number：

```text
addrs[12] -> indirect block -> 256 个 data block number
```

这个 indirect block 本身不是文件内容，而是一个“索引块”，里面存放更多的数据块编号。

由于一个 block 大小为 1024 字节，一个 `uint` 类型 block number 为 4 字节，因此一个 indirect block 可以存放：

```text
1024 / 4 = 256
```

个 block number。

---

### 5. 原始 xv6 最大文件大小

原始 xv6 的 inode 结构为：

```text
12 个 direct block
1 个 singly-indirect block，可指向 256 个 data block
```

所以最大可寻址数据块数为：

```text
12 + 256 = 268 blocks
```

最大文件大小约为：

```text
268 * 1024 = 274432 字节
```

这就是实验文档中所说的原始文件大小限制来源。

---

## 三、Large files 的修改目标

为了支持更大的文件，需要引入：

```text
doubly-indirect block
```

二级间接块结构如下：

```text
inode
 └── doubly-indirect block
      ├── indirect block 0
      │    ├── data block 0
      │    ├── data block 1
      │    └── ...
      ├── indirect block 1
      │    ├── data block
      │    └── ...
      └── ...
```

一个 doubly-indirect block 可以指向：

```text
256 个 singly-indirect blocks
```

每个 singly-indirect block 又可以指向：

```text
256 个 data blocks
```

所以二级间接块最多支持：

```text
256 * 256 = 65536
```

个数据块。

---

## 四、Large files 的核心修改

### 1. 修改 `NDIRECT`

原始 xv6：

```c
#define NDIRECT 12
```

为了不扩大 inode 结构大小，需要牺牲一个 direct block 位置，用它来存放二级间接块地址。

修改为：

```c
#define NDIRECT 11
```

新的布局：

```text
addrs[0]  ~ addrs[10]       11 个 direct block
addrs[11]                   1 个 singly-indirect block
addrs[12]                   1 个 doubly-indirect block
```

---

### 2. 修改 `struct inode`

文件位置：

```text
kernel/file.h
```

修改：

```c
uint addrs[NDIRECT+2];
```

含义：

```text
NDIRECT 个 direct block
1 个 singly-indirect block
1 个 doubly-indirect block
```

由于 `NDIRECT = 11`，所以：

```text
NDIRECT + 2 = 13
```

仍然保持原来 `addrs[13]` 的总长度。

---

### 3. 修改 `struct dinode`

文件位置：

```text
kernel/fs.h
```

也要同步修改：

```c
uint addrs[NDIRECT+2];
```

这里非常关键。

`struct inode` 是内存中的 inode，而 `struct dinode` 是磁盘上的 inode。两者必须保持地址数组布局一致，否则文件系统读写磁盘 inode 时会出错。

---

### 4. 修改 `MAXFILE`

原始 xv6：

```c
#define MAXFILE (NDIRECT + NINDIRECT)
```

修改后应为：

```c
#define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT * NINDIRECT)
```

原因是最大文件块数变为：

```text
11 + 256 + 256 * 256 = 65803 blocks
```

---

## 五、实现 `bmap()` 的二级间接块映射

`bmap()` 的作用是：

> 根据文件内的逻辑块号 `bn`，找到或分配对应的磁盘物理块号。

原始逻辑处理：

1. direct block
2. singly-indirect block

本次新增第三种情况：

3. doubly-indirect block

核心逻辑：

```c
bn -= NINDIRECT;

if(bn < NINDIRECT * NINDIRECT){
  // 1. 找到或分配 doubly-indirect block
  if((addr = ip->addrs[NDIRECT + 1]) == 0)
    ip->addrs[NDIRECT + 1] = addr = balloc(ip->dev);

  bp = bread(ip->dev, addr);
  a = (uint*)bp->data;

  // 2. 根据 bn / NINDIRECT 找到第几个 singly-indirect block
  int index = bn / NINDIRECT;
  if((addr = a[index]) == 0){
    a[index] = addr = balloc(ip->dev);
    log_write(bp);
  }
  brelse(bp);

  // 3. 根据 bn % NINDIRECT 找到最终 data block
  bn %= NINDIRECT;
  final_bp = bread(ip->dev, addr);
  final_a = (uint*)final_bp->data;

  if((final_addr = final_a[bn]) == 0){
    final_a[bn] = final_addr = balloc(ip->dev);
    log_write(final_bp);
  }

  brelse(final_bp);
  return final_addr;
}
```

### 关键理解

对于二级间接块：

```text
bn / NINDIRECT
```

决定使用二级间接块中的第几个一级间接块。

```text
bn % NINDIRECT
```

决定使用该一级间接块中的第几个数据块地址。

---

## 六、Large files 遇到的问题与解决

## 问题 1：`mkfs` 报错

### 报错信息

```text
mkfs: mkfs/mkfs.c:85: main: Assertion `(BSIZE % sizeof(struct dinode)) == 0' failed.
make: *** [Makefile:264: fs.img] Aborted (core dumped)
```

### 原因分析

这个报错发生在生成 `fs.img` 阶段，不是内核运行时报错。

`mkfs` 中有断言：

```c
assert((BSIZE % sizeof(struct dinode)) == 0);
```

意思是：

> 一个磁盘 block 必须刚好能放下整数个 `struct dinode`。

原始 xv6 中：

```text
struct dinode 大小 = 64 字节
BSIZE = 1024
1024 / 64 = 16
```

刚好能整除。

当把 `NDIRECT` 改成 11 后，如果 `struct dinode` 仍然是：

```c
uint addrs[NDIRECT+1];
```

那么 `addrs` 只有 12 个元素，`struct dinode` 大小变成：

```text
2 + 2 + 2 + 2 + 4 + 12 * 4 = 60 字节
```

于是：

```text
1024 % 60 != 0
```

触发断言失败。

### 解决办法

同步修改 `struct dinode`：

```c
uint addrs[NDIRECT+2];
```

保证：

```text
NDIRECT + 2 = 13
```

`struct dinode` 仍然是 64 字节。

---

## 问题 2：`bigfile` 只写到 267 blocks

### 测试输出

```text
$ bigfile
..
wrote 267 blocks
bigfile: file is too small
```

### 原因分析

这说明二级间接块逻辑没有真正生效，文件仍被限制在：

```text
11 + 256 = 267 blocks
```

最可能原因是 `MAXFILE` 没有修改，仍然是：

```c
#define MAXFILE (NDIRECT + NINDIRECT)
```

而 `writei()` 中有检查：

```c
if(off + n > MAXFILE*BSIZE)
  return -1;
```

所以当写到第 267 个 block 后，再继续写会被 `MAXFILE` 限制拦截，根本不会进入二级间接块逻辑。

### 解决办法

修改：

```c
#define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT * NINDIRECT)
```

---

## 问题 3：`itrunc()` 中二级间接块释放逻辑容易写错

`itrunc()` 的作用是：

> 删除或截断文件时，释放该文件占用的所有数据块。

新增二级间接块后，必须释放：

1. 二级间接块指向的所有一级间接块
2. 每个一级间接块指向的所有 data block
3. 一级间接块本身
4. 二级间接块本身

### 曾出现的错误

#### 错误一：读错二级间接块

错误写法：

```c
bp = bread(ip->dev, ip->addrs[NDIRECT]);
```

这里读的是一级间接块。

正确写法：

```c
bp = bread(ip->dev, ip->addrs[NDIRECT+1]);
```

---

#### 错误二：释放错数据块

错误写法：

```c
bfree(ip->dev, ip->addrs[NDIRECT]);
```

这会反复释放一级间接块地址，而不是释放真正的数据块。

正确写法：

```c
bfree(ip->dev, da[j]);
```

---

#### 错误三：没有判断 `a[i]` 是否为 0

错误写法：

```c
struct buf *dbp = bread(ip->dev, a[i]);
```

如果 `a[i] == 0`，会尝试读取 block 0，非常危险。

正确写法：

```c
if(a[i]){
  ...
}
```

### 正确思路

```c
if(ip->addrs[NDIRECT+1]){
  bp = bread(ip->dev, ip->addrs[NDIRECT+1]);
  a = (uint*)bp->data;

  for(i = 0; i < NINDIRECT; i++){
    if(a[i]){
      struct buf *dbp = bread(ip->dev, a[i]);
      uint *da = (uint*)dbp->data;

      for(j = 0; j < NINDIRECT; j++){
        if(da[j])
          bfree(ip->dev, da[j]);
      }

      brelse(dbp);
      bfree(ip->dev, a[i]);
      a[i] = 0;
    }
  }

  brelse(bp);
  bfree(ip->dev, ip->addrs[NDIRECT+1]);
  ip->addrs[NDIRECT+1] = 0;
}
```

---

# Part 2：Symbolic Links

## 七、背景知识：软链接与硬链接

### 1. hard link

xv6 原本支持硬链接：

```c
link(old, new)
```

硬链接的本质是：

```text
多个目录项指向同一个 inode
```

示意：

```text
a.txt  ----\
            > inode 23
b.txt  ----/
```

特点：

- 两个文件名地位平等
- 删除其中一个目录项，不一定删除文件数据
- 只能链接同一个文件系统中的文件

---

### 2. symbolic link

符号链接，也叫软链接，本质是：

```text
一个特殊文件，文件内容是目标路径字符串
```

例如：

```text
link.txt -> /home/a.txt
```

在 xv6 中可以设计为：

```text
link.txt 的 inode 类型 = T_SYMLINK
link.txt 的 data block 内容 = "/home/a.txt\0"
```

当执行：

```c
open("link.txt", O_RDONLY)
```

内核发现它是 symlink，于是读出其中保存的路径，再打开目标文件。

---

## 八、Symbolic links 的目标

实现系统调用：

```c
int symlink(char *target, char *path);
```

作用：

```text
在 path 处创建一个符号链接文件，
这个符号链接指向 target。
```

例如：

```c
symlink("/a/b.txt", "/link");
```

创建：

```text
/link -> /a/b.txt
```

注意：

> `target` 不需要在创建 symlink 时存在。

即：

```c
symlink("/not/exist", "/link");
```

也应该成功。  
只有后续 `open("/link")` 时，如果目标不存在，`open` 才失败。

---

## 九、Symbolic links 需要修改的文件

### 1. 添加系统调用号

文件：

```text
kernel/syscall.h
```

新增：

```c
#define SYS_symlink ...
```

---

### 2. 注册系统调用处理函数

文件：

```text
kernel/syscall.c
```

新增声明：

```c
extern uint64 sys_symlink(void);
```

在系统调用表中添加：

```c
[SYS_symlink] sys_symlink,
```

---

### 3. 添加用户态入口

文件：

```text
user/usys.pl
```

新增：

```perl
entry("symlink");
```

---

### 4. 添加用户态函数声明

文件：

```text
user/user.h
```

新增：

```c
int symlink(char*, char*);
```

---

### 5. 添加文件类型

文件：

```text
kernel/stat.h
```

新增：

```c
#define T_SYMLINK 4
```

表示 symlink 类型的 inode。

---

### 6. 添加 `O_NOFOLLOW`

文件：

```text
kernel/fcntl.h
```

新增：

```c
#define O_NOFOLLOW 0x800
```

注意：

> open flag 通过按位或组合，所以 `O_NOFOLLOW` 不能和已有 flag 重叠。

例如：

```c
open("link", O_RDONLY | O_NOFOLLOW);
```

表示打开 symlink 本身，而不是跟随它。

---

### 7. 修改 Makefile

在 `UPROGS` 中加入：

```makefile
$U/_symlinktest\
```

注意 Makefile 的续行符 `\` 和缩进格式，否则会导致 make 解析错误。

---

## 十、实现 `sys_symlink()`

### 核心思路

`sys_symlink()` 做 4 件事：

1. 从用户态取出 `target`
2. 从用户态取出 `path`
3. 在 `path` 处创建类型为 `T_SYMLINK` 的 inode
4. 把 `target` 字符串写入该 inode 的数据块中

### 实现代码

```c
uint64
sys_symlink(void)
{
  struct inode *ip;
  char target[MAXPATH], path[MAXPATH];

  if(argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)
    return -1;

  begin_op();

  ip = create(path, T_SYMLINK, 0, 0);
  if(ip == 0){
    end_op();
    return -1;
  }

  int len = strlen(target) + 1;
  if(writei(ip, 0, (uint64)target, 0, len) != len){
    iunlockput(ip);
    end_op();
    return -1;
  }

  iunlockput(ip);
  end_op();

  return 0;
}
```

### 关键理解

```c
writei(ip, 0, (uint64)target, 0, strlen(target) + 1);
```

这句话就是把 `target` 路径写入 symlink inode 的数据块。

写入后，symlink inode 的结构可以理解为：

```text
inode type = T_SYMLINK
inode data = "/a/b.txt\0"
```

后续 `open()` 跟随 symlink 时，就通过 `readi()` 读出这个字符串。

---

## 十一、修改 `sys_open()` 跟随 symlink

### 目标行为

默认情况下：

```c
open("link", O_RDONLY)
```

如果 `link` 是 symlink，则打开它指向的目标文件。

如果带有：

```c
O_NOFOLLOW
```

例如：

```c
open("link", O_RDONLY | O_NOFOLLOW)
```

则打开 symlink 本身，不跟随目标路径。

---

### 基本流程

```text
1. namei(path) 找到 inode
2. ilock(ip)
3. 如果 ip->type == T_SYMLINK 且没有 O_NOFOLLOW：
   3.1 从 symlink inode 中 readi() 读出 target
   3.2 iunlockput(ip) 释放当前 symlink inode
   3.3 namei(target) 找到目标 inode
   3.4 ilock(ip)
   3.5 如果目标仍是 symlink，继续循环
4. 找到非 symlink inode 后，执行普通 open 逻辑
```

---

### 示例代码

```c
if(ip->type == T_SYMLINK && !(omode & O_NOFOLLOW)){
  int depth = 0;
  char target[MAXPATH];

  while(ip->type == T_SYMLINK){
    if(depth++ >= 10){
      iunlockput(ip);
      end_op();
      return -1;
    }

    memset(target, 0, MAXPATH);

    if(readi(ip, 0, (uint64)target, 0, MAXPATH) <= 0){
      iunlockput(ip);
      end_op();
      return -1;
    }

    iunlockput(ip);

    if((ip = namei(target)) == 0){
      end_op();
      return -1;
    }

    ilock(ip);
  }
}
```

---

## 十二、为什么不要修改 `namei()`

题目要求：

> Other system calls must not follow symbolic links.

也就是说：

- `unlink("link")` 应删除 symlink 本身
- `link("link", "newlink")` 应操作 symlink 本身
- 其他系统调用不应默认跟随 symlink

所以不能把跟随 symlink 的逻辑写进 `namei()`。

如果改 `namei()`，那么所有路径查找都会跟随符号链接，反而违反题目要求。

正确做法是：

```text
只在 sys_open() 中处理 symlink 跟随逻辑
```

---

## 十三、Symbolic links 遇到的问题与解决

## 问题 1：`T_SYMLINK` 拼写错误

错误写法：

```c
ip = create(path, T_SYMINK, 0, 0);
```

问题：

```text
T_SYMINK 少了 L
```

正确写法：

```c
ip = create(path, T_SYMLINK, 0, 0);
```

---

## 问题 2：`end_op` 少括号

错误写法：

```c
end_op;
```

这只是一个函数名表达式，并不会调用函数。

正确写法：

```c
end_op();
```

---

## 问题 3：没有检查 `writei()` 返回值

虽然简单写法也可能通过部分测试，但更稳妥的是检查写入长度：

```c
if(writei(ip, 0, (uint64)target, 0, len) != len){
  iunlockput(ip);
  end_op();
  return -1;
}
```

这样如果写入 target 失败，系统调用能正确返回 `-1`。

---

## 问题 4：不理解 `iunlockput(ip)`、`ilock(ip)`、`end_op()`

### `ilock(ip)`

作用：

```text
锁住 inode，确保可以安全访问 ip->type、ip->size、ip->addrs 等字段。
```

`namei(path)` 返回的 inode 默认没有上锁，因此后续访问 inode 内容前必须：

```c
ilock(ip);
```

---

### `iunlockput(ip)`

作用：

```text
解锁 inode，并减少引用计数。
```

相当于：

```c
iunlock(ip);
iput(ip);
```

在 symlink 跟随过程中：

```text
读完 symlink inode 中保存的 target 后，
这个 symlink inode 已经不再需要，
因此要 iunlockput(ip) 释放它。
```

---

### `end_op()`

作用：

```text
结束一次文件系统日志事务。
```

xv6 中涉及文件系统修改的操作一般要包裹在：

```c
begin_op();
...
end_op();
```

之间。

---

### symlink 跟随中的典型流程

```text
当前 ip 是 /link 的 inode
readi(ip) 读出 "/a.txt"
iunlockput(ip) 释放 /link 的 inode

namei("/a.txt") 找到 /a.txt 的 inode
ilock(ip) 锁住 /a.txt 的 inode
继续判断它是不是 symlink
```

---

## 问题 5：Makefile 报错

### 报错信息

```text
Makefile:248: *** recipe commences before first target.  Stop.
```

### 原因分析

这不是 C 代码问题，而是 Makefile 格式错误。

含义是：

```text
Makefile 第 248 行出现了一条命令行，
但它前面没有对应的 target。
```

最常见原因是：

```text
添加 $U/_symlinktest\ 时位置或续行符写错
```

Makefile 中 `UPROGS` 一般长这样：

```makefile
UPROGS=\
  $U/_cat\
  $U/_echo\
  ...
  $U/_symlinktest\
```

如果前一行漏了 `\`，或者 `$U/_symlinktest\` 出现在 `UPROGS` 外面，就会导致 make 把它误认为命令。

### 排查命令

```bash
nl -ba Makefile | sed -n '240,252p'
```

作用：

```text
显示 Makefile 第 240 到 252 行，并带行号。
```

便于定位第 248 行到底写错在哪里。

---

# 十四、本次 Lab 学到的核心知识点

## 1. inode 有内存版和磁盘版

| 结构 | 文件位置 | 作用 |
|---|---|---|
| `struct inode` | `kernel/file.h` | 内核运行时使用 |
| `struct dinode` | `kernel/fs.h` | 磁盘中实际存储 |

修改 inode 结构时必须注意二者一致。

---

## 2. 文件内容不是存在 inode 里

inode 中保存的是：

```text
数据块编号
```

文件真正内容存在：

```text
data block
```

对于 symlink：

```text
symlink 的 target 路径字符串也存在 data block 中
```

---

## 3. direct / indirect / doubly-indirect 的层次

```text
direct:
inode -> data block

singly-indirect:
inode -> indirect block -> data block

doubly-indirect:
inode -> doubly-indirect block -> indirect block -> data block
```

---

## 4. `bmap()` 是文件块映射的核心

`bmap()` 负责：

```text
逻辑块号 bn -> 磁盘物理块号
```

如果目标块不存在，`bmap()` 会根据需要调用 `balloc()` 分配新块。

---

## 5. `MAXFILE` 会限制 `writei()`

即使 `bmap()` 支持二级间接，如果 `MAXFILE` 没改，`writei()` 仍然会拒绝写入更大文件。

因此 Large files 的修改不是只改 `bmap()`，还包括：

```text
NDIRECT
struct inode
struct dinode
MAXFILE
itrunc
```

---

## 6. `itrunc()` 必须完整释放所有层级

新增二级间接后，释放逻辑必须和分配逻辑对称。

分配时：

```text
二级间接块 -> 一级间接块 -> 数据块
```

释放时：

```text
先释放所有数据块
再释放一级间接块
最后释放二级间接块
```

---

## 7. symlink 不应在 `namei()` 中统一跟随

原因：

```text
题目要求 link、unlink 等系统调用操作 symlink 本身
```

所以只有 `open()` 默认跟随 symlink。

---

## 8. `O_NOFOLLOW` 控制是否跟随 symlink

```c
open("link", O_RDONLY);
```

默认跟随，打开目标文件。

```c
open("link", O_RDONLY | O_NOFOLLOW);
```

不跟随，打开 symlink 本身。

---

## 9. symlink 需要处理循环链接

例如：

```text
a -> b
b -> a
```

如果不限制深度，会无限循环。

实验允许使用深度阈值近似判断：

```text
最多跟随 10 层，超过则返回 -1
```

---

## 10. xv6 文件系统修改常见固定模式

涉及磁盘修改：

```c
begin_op();
...
end_op();
```

访问 inode 字段：

```c
ilock(ip);
...
iunlock(ip);
```

用完 inode：

```c
iunlockput(ip);
```

路径查找：

```c
ip = namei(path);
ilock(ip);
```

---

# 十五、最终复习清单

## Large files 必查点

- [ ] `NDIRECT` 是否改为 11
- [ ] `struct inode` 是否为 `addrs[NDIRECT+2]`
- [ ] `struct dinode` 是否为 `addrs[NDIRECT+2]`
- [ ] `MAXFILE` 是否包含 `NINDIRECT * NINDIRECT`
- [ ] `bmap()` 是否处理 direct、singly-indirect、doubly-indirect 三种情况
- [ ] 二级间接中是否正确使用：
  - [ ] `bn / NINDIRECT`
  - [ ] `bn % NINDIRECT`
- [ ] `itrunc()` 是否释放：
  - [ ] data block
  - [ ] singly-indirect block
  - [ ] doubly-indirect block
- [ ] `make clean && make qemu` 后 `bigfile` 是否不再停在 267 blocks

---

## Symbolic links 必查点

- [ ] `kernel/syscall.h` 添加 `SYS_symlink`
- [ ] `kernel/syscall.c` 注册 `sys_symlink`
- [ ] `user/usys.pl` 添加 `entry("symlink")`
- [ ] `user/user.h` 声明 `int symlink(char*, char*)`
- [ ] `kernel/stat.h` 添加 `T_SYMLINK`
- [ ] `kernel/fcntl.h` 添加 `O_NOFOLLOW`
- [ ] `Makefile` 添加 `$U/_symlinktest\`
- [ ] `sys_symlink()` 是否创建 `T_SYMLINK` 类型 inode
- [ ] `sys_symlink()` 是否把 `target` 写入 inode 数据块
- [ ] `sys_open()` 是否默认跟随 symlink
- [ ] `sys_open()` 是否支持 `O_NOFOLLOW`
- [ ] 是否处理嵌套 symlink
- [ ] 是否限制最大跟随深度，避免循环链接
- [ ] 是否没有修改 `namei()` 的全局语义

---

# 十六、这次 Lab 的工作量体现

本次 File System Lab 的工作量不只是“补几行代码”，而是围绕 xv6 文件系统完成了一次较完整的结构性修改。

## 1. 修改了文件系统的核心数据结构

包括：

```text
struct inode
struct dinode
NDIRECT
MAXFILE
```

这类修改会影响文件系统的磁盘布局和运行时行为，需要理解内存 inode 与磁盘 inode 的区别。

---

## 2. 扩展了文件块寻址机制

从原来的：

```text
direct + singly-indirect
```

扩展为：

```text
direct + singly-indirect + doubly-indirect
```

涉及：

- 块号映射
- 按需分配 block
- buffer cache 读写
- 日志写入 `log_write`
- 文件截断释放 `itrunc`

---

## 3. 新增了完整系统调用

`symlink()` 的实现涉及从用户态取参、创建 inode、写入 inode 数据块、返回错误码等完整系统调用流程。

---

## 4. 修改了 `open()` 的路径解析行为

实现 symlink 默认跟随机制，并兼顾：

- `O_NOFOLLOW`
- 嵌套链接
- 循环链接检测
- 目标不存在时失败
- 其他系统调用不跟随 symlink

---

## 5. 进行了多轮错误定位和调试

包括：

- `mkfs` 断言失败
- `bigfile` 写入上限仍为 267 blocks
- `itrunc()` 释放逻辑错误
- `T_SYMLINK` 拼写错误
- `end_op()` 调用错误
- Makefile 续行/缩进错误

这些问题覆盖了：

```text
C 代码逻辑错误
文件系统结构错误
磁盘布局错误
Makefile 构建错误
系统调用接入错误
```

---

# 十七、一句话总结

本次 File System Lab 的核心收获是：

> 通过实现大文件和符号链接，系统理解了 xv6 文件系统中 inode 如何索引数据块、文件大小限制如何产生、路径查找与 open 语义如何配合，以及文件系统修改为什么必须同时考虑磁盘布局、内存结构、日志事务和并发锁机制。
