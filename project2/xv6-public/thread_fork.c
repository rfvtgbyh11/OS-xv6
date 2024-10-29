#include "types.h"
#include "stat.h"
#include "user.h"

#define NUM_THREAD 5

thread_t thread[100];

void *thread_fork(void *arg){
    sleep(100);
    thread_exit(arg);
    return 0;
}

void *thread_main(void *arg)
{
    int i;
    if (fork() == 0){
        for (i = 0; i < NUM_THREAD; i++) {
            thread_create(&thread[i], thread_fork, (void *)i);
        }
        sleep(300);
        exit();
    } else {
        sleep(200);
        thread_exit(arg);
    }
    return 0;
}


int main(int argc, char *argv[])
{
    int i;
    for (i = 0; i < NUM_THREAD; i++){
    thread[i] = i + 2;
    }

    for (i = 0; i < NUM_THREAD; i++) {
        thread_create(&thread[i], thread_main, (void *)i);
    }
    sleep(100);
    proclist();

    sleep(500);
    exit();
}
