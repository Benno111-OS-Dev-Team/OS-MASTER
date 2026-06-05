.code64
.section .text.boot
.global _start
.extern bootscreen_entry_main
.extern __bss_start
.extern __bss_end
.extern __stack_top

_start:
    cli
    cld

    lea     __stack_top(%rip), %rsp
    xor     %rbp, %rbp

    lea     __bss_start(%rip), %rdi
    lea     __bss_end(%rip), %rcx
    sub     %rdi, %rcx
    xor     %rax, %rax
    rep     stosb

    call    bootscreen_entry_main

1:
    cli
    hlt
    jmp     1b

.section .bss, "aw", @nobits
.align 16
__bss_start:

.align 16
bootscreen_stack:
    .skip 65536
__stack_top:

__bss_end:
