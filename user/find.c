#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

/*
 * 在以 path 为根的目录树中递归查找名为 targ 的普通文件。
 *
 * xv6 中“目录”本质上也是一个文件，目录文件的内容由一项一项
 * struct dirent 组成。每次 read(fd, &de, sizeof(de)) 都会读取
 * 一个目录项，并且文件描述符 fd 内部的偏移量会自动向后移动。
 * 当 read 返回 0 时，说明已经读到目录文件末尾，循环自然结束。
 */
void 
find(char* path,char* targ)
{
  /*
   * buf 用来拼接当前目录项的完整路径，例如：
   *   path = "a"
   *   de.name = "b"
   *   buf = "a/b"
   *
   * p 指向 buf 中路径末尾的位置，后面会把目录项名称拷贝到 p 处。
   */
  char buf[512], *p;
  int fd;
  struct dirent de;  // 保存从目录文件中读出的一个目录项
  struct stat st;    // 保存某个路径对应文件的类型、大小等状态信息

  // 打开当前路径。这里假设传进来的 path 是目录；打开失败则无法继续查找。
  if((fd = open(path, 0)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  /*
   * 检查路径缓冲区是否足够：
   *   strlen(path)  当前目录路径长度
   *   1             中间的 '/'
   *   DIRSIZ        xv6 中目录项文件名的最大长度
   *   1             字符串结尾的 '\0'
   *
   * 如果这些加起来超过 buf 的大小，就不能安全拼接完整路径。
   */
  if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
      printf("find: path too long\n");
      return;
    }

  // 先把当前目录路径复制到 buf 中，然后在末尾补一个 '/'。
  strcpy(buf, path);
  p = buf+strlen(buf); // p 指向当前字符串结尾的 '\0'
  *p++ = '/';          // 写入 '/' 后，p 移动到文件名应该开始的位置

  /*
   * 逐个读取当前目录下的目录项。
   *
   * read 返回 sizeof(de) 表示成功读到了一个完整目录项；
   * read 返回 0 表示目录文件读完；
   * 其他返回值表示读取异常或不足一个完整目录项。
   */
  while(read(fd, &de, sizeof(de)) == sizeof(de)){
    /*
     * de.inum == 0 表示这个目录项没有被使用。
     * "."  表示当前目录，递归进去会原地打转。
     * ".." 表示父目录，递归进去会在父子目录之间无限来回。
     * 所以这三种情况都必须跳过。
     */
    if(de.inum == 0 || strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
      continue;

    /*
     * 把目录项名称拼到 buf 的 '/' 后面，得到该目录项的完整路径。
     *
     * xv6 的 de.name 长度固定为 DIRSIZ，不保证自带 '\0' 结尾，
     * 所以这里先拷贝 DIRSIZ 个字节，再手动补 '\0'。
     */
    memmove(p, de.name, DIRSIZ);
    p[DIRSIZ] = 0;

    // 根据完整路径获取文件状态，从而判断它是普通文件还是目录。
    if(stat(buf, &st) < 0){
      printf("find: cannot stat %s\n", buf);
      continue;
    }

    /*
     * 如果当前目录项的名字和目标文件名相同，并且它是普通文件，
     * 就输出完整路径。这里使用 de.name 比较的是“文件名”，
     * 输出 buf 则能显示从起始目录到该文件的完整相对路径。
     */
    if(strcmp(de.name, targ) == 0 && st.type == T_FILE){
      printf("%s\n", buf);
    }

    // 如果当前目录项本身是目录，则递归进入这个子目录继续查找。
    if(st.type == T_DIR){
      find(buf, targ);
    }
  }
}

int
main(int argc, char *argv[])
{
  // find 需要两个参数：起始目录路径和要查找的文件名。
  if(argc != 3){
    fprintf(2, "usage: find <tick1> <tick2>");
    exit(1);
  }

  // 从 argv[1] 指定的目录开始，查找名为 argv[2] 的普通文件。
  find(argv[1], argv[2]);
  exit(0);
}
