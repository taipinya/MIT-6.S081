#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define MAXARG 100

int
main(int argc, char *argv[])
{
  //read from stdin
  char buf[1];//每次只存 1 个字符，因为代码是一个字符一个字符地从标准输入读。
  char line[100];//用来保存当前读到的一整行内容。
  int i;
  i = 0;

  //从标准输入中读取内容放到buf中
  while(read(0, buf, 1) > 0){
    //读到了换行符，说明一行结束了
    if(buf[0] == '\n'){
      //把当前行变成 C 字符串
      line[i] = 0;

      //get new_argv，这里把 xargs 后面的命令和参数复制到 new_argv
      char* new_argv[MAXARG];
      int idx = 0;
      for(int j = 1; j < argc; j++){
        new_argv[idx++] = argv[j];
      }

      //标准输入读到的这一行追加进去，并用 0 结尾
      new_argv[idx++] = line;
      new_argv[idx] = 0; //set 0 to the end

      //fork child and exec
      if(fork() == 0){

        exec(argv[1], new_argv); //child exec commands
        exit(1);
      }

      //parent wait finishment of child
      else{
        wait(0); 
      }

      i = 0; //reset the line
    }

    else{
      line[i++] = buf[0];
    }
  }

  exit(0);
  
}
