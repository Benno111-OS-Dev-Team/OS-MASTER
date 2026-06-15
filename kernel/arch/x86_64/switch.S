/*
 * x86_64 Context Switch Implementation
 */

.section .text

/*
 * void switch_context(cpu_context_t *old, cpu_context_t *new)
 *
 * x86_64 calling convention:
 *   rdi = first argument (old context)
 *   rsi = second argument (new context)
 */
.global switch_context
.type switch_context, @function
switch_context:
    /* Save old context using cpu_context_t offsets from arch.h. */
    mov %rax, 0(%rdi)      /* rax */
    mov %rbx, 8(%rdi)      /* rbx */
    mov %rcx, 16(%rdi)     /* rcx */
    mov %rdx, 24(%rdi)     /* rdx */
    mov %rsi, 32(%rdi)     /* rsi */
    mov %rdi, 40(%rdi)     /* rdi */
    mov %rbp, 48(%rdi)     /* rbp */

    /* Save stack pointer as it should look after returning to the caller. */
    lea 8(%rsp), %rax
    mov %rax, 56(%rdi)     /* rsp */
    mov %r8, 64(%rdi)      /* r8 */
    mov %r9, 72(%rdi)      /* r9 */
    mov %r10, 80(%rdi)     /* r10 */
    mov %r11, 88(%rdi)     /* r11 */
    mov %r12, 96(%rdi)     /* r12 */
    mov %r13, 104(%rdi)    /* r13 */
    mov %r14, 112(%rdi)    /* r14 */
    mov %r15, 120(%rdi)    /* r15 */

    /* Save return address (rip) */
    mov (%rsp), %rax
    mov %rax, 128(%rdi)    /* rip */

    /* Save flags */
    pushfq
    pop %rax
    mov %rax, 136(%rdi)    /* rflags */

    /* Restore new context */
    mov 8(%rsi), %rbx      /* rbx */
    mov 16(%rsi), %rcx     /* rcx */
    mov 24(%rsi), %rdx     /* rdx */
    mov 40(%rsi), %rdi     /* rdi */
    mov 48(%rsi), %rbp     /* rbp */
    mov 64(%rsi), %r8      /* r8 */
    mov 72(%rsi), %r9      /* r9 */
    mov 80(%rsi), %r10     /* r10 */
    mov 88(%rsi), %r11     /* r11 */
    mov 96(%rsi), %r12     /* r12 */
    mov 104(%rsi), %r13    /* r13 */
    mov 112(%rsi), %r14    /* r14 */
    mov 120(%rsi), %r15    /* r15 */

    /* Restore stack pointer */
    mov 56(%rsi), %rsp     /* rsp */

    /* Queue the resume address on the new stack so rax can be restored. */
    pushq 128(%rsi)         /* rip */

    /* Restore flags */
    mov 136(%rsi), %rax    /* rflags */
    push %rax
    popfq

    mov 0(%rsi), %rax      /* rax */
    mov 32(%rsi), %rsi     /* rsi */
    ret

/*
 * void _task_exit_stub(void)
 * Called when a task exits (internal stub)
 */
.global _task_exit_stub
.type _task_exit_stub, @function
_task_exit_stub:
    /* Disable interrupts */
    cli

    /* Call exit_task(0) from sched.c */
    xor %rdi, %rdi      /* exit code = 0 */
    call exit_task

    /* Should never reach here */
1:
    hlt
    jmp 1b
