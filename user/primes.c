#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void sieve(int fd)
{
  //read the first number in pipe, which is the prime number
  int prime;
  if(read(fd,&prime,sizeof(prime)) == 0) //if there is no number to read, close the pipe and return
  {
    close(fd);
    return;
  }
  
  printf("prime %d\n", prime);

  //create a new pipe for the child process
  int newfd[2];
  pipe(newfd);

  //fork a child process
  if(fork() == 0)
  {
    //child process
    close(newfd[1]); //child cannot write new pipe
    sieve(newfd[0]);//child process read from new pipe and sieve the numbers
    exit(0);
    return;
  }

  //sieve the numbers and write to the new pipe
  else{
    close(newfd[0]); //parent cannot read new pipe

    int num;
    while(read(fd,&num,sizeof(num)) > 0)
    {
      if(num % prime != 0)
      {
        write(newfd[1], &num, sizeof(num)); //write the number to new pipe if it is not divisible by prime
      }
    }
    close(fd);//close the read end of old pipe
    //close the write end of new pipe and wait for the child process to end
    close(newfd[1]);
    wait(0);
    return;
  }

}

int
main(int argc, char *argv[])
{
  if(argc != 1)
  {
    fprintf(2, "Usage: primes\n");
    exit(1);
  }

  int p[2];
  pipe(p);

  //first process write 2-35 to pipe
  int i;
  for(i = 2; i <= 35; i++)
  {
    write(p[1], &i, sizeof(i));
  }
  close(p[1]);
  
  if(fork() == 0){
    sieve(p[0]);
    exit(0);
  } else {
    close(p[0]);
    wait(0);
  }
  
  exit(0);

}
