#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

void 
find(char* path,char* targ)
{
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  if((fd = open(path, 0)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
      printf("find: path too long\n");
      return;
    }
  strcpy(buf, path);
  p = buf+strlen(buf);
  *p++ = '/';
  while(read(fd, &de, sizeof(de)) == sizeof(de)){
    if(de.inum == 0 || strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
      continue;

    memmove(p, de.name, DIRSIZ);
    p[DIRSIZ] = 0;


    if(stat(buf, &st) < 0){
      printf("find: cannot stat %s\n", buf);
      continue;
    }

    if(strcmp(de.name, targ) == 0 && st.type == T_FILE){
      printf("%s\n", buf);
    }

    if(st.type == T_DIR){
      find(buf, targ);
    }
  }
}

int
main(int argc, char *argv[])
{
  if(argc != 3){
    fprintf(2, "usage: find <tick1> <tick2>");
    exit(1);
  }

  find(argv[1], argv[2]);
  exit(0);
}
