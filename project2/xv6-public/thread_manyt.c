#include "types.h"
#include "stat.h"
#include "user.h"

#define NUM_THREAD 5

void *thread_main(void *arg)
{
  printf(1, "+");
  sleep(100);
  thread_exit((void*)1);
  return 0;
}

thread_t thread[100];

int main(int argc, char *argv[])
{
  int i;
  int j = 0;
  void **retval = (void **)&j;
  for (i = 0; i<100; i++){
    thread[i] = i + 2;
  }

  printf(1, "Thread many create test start\n");
  printf(1, "100 thread will be created. '+' should be written 100 times\n");
  for (i = 0; i < 100; i++) {
    thread_create(&thread[i], thread_main, (void *)i);
    if (i > 10) thread_join(i - 9, retval);
  }
  sleep(100);
  printf(1, "\n");
  proclist();
  exit();
}
