#include "types.h"
#include "user.h"

int
main()
{
    char com[100];
    char *arg[2];
    int i, j;

    while (1){
        for (i = 0; i < 100; i++) com[i] = 0;
        arg[0] = arg[1] = 0;
        // read and parse command and arg
        read(0, com, 100);
        j = 0;
        for (i = 0; i < 100; i++){
            if (com[i] == ' '){
                com[i] = 0;
                arg[j] = com + i + 1;
                j++;
            }else if (com[i] == '\n'){
                com[i] = 0;
                break;
            }
        }

        // system call
        if (strcmp(com, "list") == 0){
            proclist();
        }else if (strcmp(com, "kill") == 0){
            if (kill(atoi(arg[0])) == 0) printf(1, "kill succeed\n");
            else printf(1, "kill failed\n");
        }else if (strcmp(com, "execute") == 0){
            if (fork() == 0) {
                if (execute(arg[0], atoi(arg[1])) == -1){
                    printf(1, "execution failed\n");
                    exit();
                }
            }
        }else if (strcmp(com, "memlim") == 0){
            if (setmemorylimit(atoi(arg[0]), atoi(arg[1])) == 0) printf(1, "setting succeed\n");
            else printf(1, "setting failed\n");
        }else if (strcmp(com, "exit") == 0){
            exit();
        }

    }
    
    exit();
}