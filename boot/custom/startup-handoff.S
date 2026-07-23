.text
.globl startup_enter_kernel
startup_enter_kernel:
    cli
    mov %rcx, %cr3
    mov %r8, %rdi
    xor %rbp, %rbp
    jmp *%rdx
