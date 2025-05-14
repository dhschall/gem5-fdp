#include <stdio.h>
#include <signal.h>
#include <unistd.h>

void sigill_handler(int sig){
    printf("Process %d received SIGILL %d.\n", getpid(), sig);
    kill(getpid(), SIGTERM);
}

int main() {
    signal(SIGILL, sigill_handler);

    asm volatile(".byte 0x0F, 0x04, 0x66, 0x00\n");
    return 0;
}
