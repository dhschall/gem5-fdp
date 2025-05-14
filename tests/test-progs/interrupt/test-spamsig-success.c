#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <m5_mmap.h>
#define TGT_SIGUSR1 0x00000a
#define TGKILL 234

void m5_exit_addr(int);

void sigint_handler(int sig)
{
    printf("Process %d received SIGINT %d.\n", getpid(), sig);
    m5_exit_addr(0);
}

int main()
{
    map_m5_mem();
    signal(TGT_SIGUSR1, sigint_handler);
    m5_exit_addr(0);
    syscall(TGKILL, getpid(), gettid(), TGT_SIGUSR1);
    return 0;
}
