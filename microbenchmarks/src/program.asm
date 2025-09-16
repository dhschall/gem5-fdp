section .data
    msg db "Hello, gem5!", 0xA
    len equ $ - msg

section .text
    global _start

_start:
    mov eax, 4             ; sys_write
    mov ebx, 1             ; stdout
    mov ecx, msg           ; message
    mov edx, len           ; length
    int 0x80               ; call kernel

    mov eax, 1             ; sys_exit
    xor ebx, ebx           ; exit code 0
    int 0x80
