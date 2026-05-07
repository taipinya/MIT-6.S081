#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char **argv)
{
  if(argc != 1)
  {
    fprintf(2, "Usage: pingpong\n");
    exit(1);
  }

  int f1[2];
  int f2[2];
  pipe(f1);  //parent->child
  pipe(f2);  //child->parent

  if(fork() == 0)
  {
    //child process
    close(f1[1]); //child cannot write pipe1
    close(f2[0]); //child cannot read pipe2

    char c;
    read(f1[0],&c,1);
    printf("%d: received ping\n",getpid());//child read pipe

    write(f2[1],"a",1);

    close(f1[0]); //release
    close(f2[1]); 

    exit(0);
  }
  
  else
  {
    //parent process
    close(f1[0]); //parent cannot read pipe1
    close(f2[1]); //parent cannot write pipe2

    write(f1[1],"a",1);

    char p;
    read(f2[0],&p,1);
    printf("%d: received pong\n",getpid());//parent read pipe

    close(f1[1]); //release
    close(f2[0]); 
    wait(0);//to wait child's end
    exit(0);
  }

  
}
