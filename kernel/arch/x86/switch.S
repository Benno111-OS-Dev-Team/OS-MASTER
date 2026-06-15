/*
 * x86 32-bit Context Switch Implementation
 */

.section .text
.code32

/*
 * void switch_context(cpu_context_t *old, cpu_context_t *new)
 *
 * x86 32-bit calling convention (cdecl):
 *   Arguments on stack: [esp+4] = old, [esp+8] = new
 */
.global switch_context
.type switch_context, @function
switch_context:
    /* Get arguments from stack */
    movl 4(%esp), %eax  /* old context */
    movl 8(%esp), %edx  /* new context */

    /* Save old context using cpu_context_t offsets from arch.h. */
    movl %eax, 0(%eax)   /* eax */
    movl %ebx, 4(%eax)   /* ebx */
    movl %ecx, 8(%eax)   /* ecx */
    movl %edx, 12(%eax)  /* edx */
    movl %esi, 16(%eax)  /* esi */
    movl %edi, 20(%eax)  /* edi */
    movl %ebp, 24(%eax)  /* ebp */
    /* Save stack pointer as it should look after returning to the caller. */
    leal 4(%esp), %ecx
    movl %ecx, 28(%eax)  /* esp */

    /* Save return address (eip) */
    movl (%esp), %ecx
    movl %ecx, 32(%eax)  /* eip */

    /* Save flags */
    pushfl
    popl %ecx
    movl %ecx, 36(%eax)  /* eflags */

    /* Restore new context */
    movl 4(%edx), %ebx   /* ebx */
    movl 8(%edx), %ecx   /* ecx */
    movl 16(%edx), %esi  /* esi */
    movl 20(%edx), %edi  /* edi */
    movl 24(%edx), %ebp  /* ebp */
    movl 28(%edx), %esp  /* esp */

    /* Queue the resume address on the new stack so eax/edx can be restored. */
    pushl 32(%edx)         /* eip */

    /* Restore flags */
    movl 36(%edx), %ecx  /* eflags */
    pushl %ecx
    popfl

    movl 0(%edx), %eax   /* eax */
    movl 12(%edx), %edx  /* edx */
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
    pushl $0            /* exit code = 0 */
    call exit_task
    addl $4, %esp

    /* Should never reach here */
1:
    hlt
    jmp 1b
