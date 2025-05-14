#include<stdio.h>


void sigint_handler(int sig){
    printf("Process %d received SIGINT %d.\n", getpid(), sig);
}

int main() {
    int * ptr = 0x11111111;
    printf("%d\n",*ptr);
    printf("%d\n",*ptr);
    printf("%d\n",*ptr);
    printf("%d\n",*ptr);
    printf("%d\n",*ptr);
    printf("%d\n",*ptr);
    printf("%d\n",*ptr);
    return 0;
}