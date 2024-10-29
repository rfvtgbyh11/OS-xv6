#include "types.h"
#include "defs.h"

int execute(char *path, int stacksize){
    char *argv[] = {path, 0};
    return exec2(path, argv, stacksize);
}

int sys_proclist(void){
    proclist();
    return 0;
}

int sys_execute(void){
    int stacksize, path;
    if (argint(0, &path) != 0 || argint(1, &stacksize) != 0)
        return -1;
    return execute((char *)path, stacksize);
}

int sys_setmemorylimit(void){
    int pid, limit;
    if (argint(0, &pid) != 0 || argint(1, &limit) != 0)
        return -1;
    return setmemorylimit(pid, limit);
}

// syscall to test thread implementation
int sys_thread_create(void){
    int tid, start_routine, arg;
    if (argint(0, &tid) != 0 || argint(1, &start_routine) != 0 || argint(2, &arg) != 0)
        return -1;
    return thread_create((thread_t *)tid, (void *(*)(void *))start_routine, (void *)arg);
}

int sys_thread_exit(void){
    int retval;
    if (argint(0, &retval) != 0)
        return -1;
    thread_exit((void *)retval);
    return 0;
}

int sys_thread_join(void){
    int tid, retval;
    if (argint(0, &tid) != 0 || argint(1, &retval) != 0)
        return -1;
    return thread_join((thread_t)tid, (void **)retval);
}
