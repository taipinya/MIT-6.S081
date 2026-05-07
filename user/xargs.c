#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define MAXARG 100

int
main(int argc, char *argv[])
{
  //read from stdin
  char buf[1];
  char line[100];
  int i;
  i = 0;

  while(read(0, buf, 1) > 0){
    if(buf[0] == '\n'){
      line[i] = 0;

      //get new_argv
      char* new_argv[MAXARG];
      int idx = 0;
      for(int j = 1; j < argc; j++){
        new_argv[idx++] = argv[j];
      }

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
