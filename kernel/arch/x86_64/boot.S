/*
 * OS8 Kernel - x86_64 Boot Assembly
 * Entry point for the kernel after UEFI/GRUB bootloader
 * 
 * This file handles:
 * - Initial CPU state setup
 * - Stack initialization
 * - BSS clearing
 * - Jump to C kernel main
 */

.code64
.section .text.boot
.global _start
.global _start_from_loader
.global idt64
.extern limine_entry_main
.extern limine_loader_entry_main
.extern x86_64_boot_init_idt
.extern __bss_start
.extern __bss_end
.extern __stack_top

/*
 * Kernel entry point
 * Called by Limine in 64-bit mode with paging enabled.
 */
_start:
    /* ================================================================= */
    /* Disable interrupts during initialization */
    /* ================================================================= */
    cli
    cld                         # Limine may not guarantee DF=0 for string ops
    
    /* ================================================================= */
    /* Set up kernel stack */
    /* ================================================================= */
    lea     __stack_top(%rip), %rsp
    xor     %rbp, %rbp              # Clear frame pointer
    
    /* ================================================================= */
    /* Clear BSS section */
    /* ================================================================= */
    lea     __bss_start(%rip), %rdi
    lea     __bss_end(%rip), %rcx
    sub     %rdi, %rcx              # Calculate BSS size
    xor     %rax, %rax              # Zero value
    rep     stosb                   # Clear BSS
    
    /* ================================================================= */
    /* Continue in Limine-specific C bootstrap.
     * Keep the assembly handoff minimal and avoid touching descriptor
     * tables or control registers until serial diagnostics are available.
     */
    /* ================================================================= */
    call    limine_entry_main
    
    /* ================================================================= */
    /* kernel_main returned - halt the system */
    /* ================================================================= */
halt:
    cli
    hlt
    jmp     halt

/*
 * Alternate entry point used by the tiny pre-kernel bootscreen loader.
 * %rdi carries a pointer to os8_boot_handoff_t.
 */
_start_from_loader:
    cli
    cld

    mov     %rdi, %rbx
    lea     __stack_top(%rip), %rsp
    xor     %rbp, %rbp

    lea     __bss_start(%rip), %rdi
    lea     __bss_end(%rip), %rcx
    sub     %rdi, %rcx
    xor     %rax, %rax
    rep     stosb

    mov     %rbx, %rdi
    call    limine_loader_entry_main
    jmp     halt

/* ===================================================================== */
/* Global Descriptor Table (GDT) */
/* ===================================================================== */
.align 16
gdt64:
    .quad   0x0000000000000000      # Null descriptor
    .quad   0x00AF9A000000FFFF      # Code segment (64-bit)
    .quad   0x00CF92000000FFFF      # Data segment
    .quad   0x00AFFA000000FFFF      # User code segment
    .quad   0x00CFF2000000FFFF      # User data segment
gdt64_end:

gdt64_pointer:
    .word   gdt64_end - gdt64 - 1   # Limit
    .quad   gdt64                   # Base

/* ===================================================================== */
/* Interrupt Descriptor Table (IDT)
 * Must be writable because early C boot code populates it.
 * ===================================================================== */
.section .bss, "aw", @nobits
.align 16
idt64:
    .fill   256, 16, 0
idt64_end:

.section .rodata
idt64_pointer:
    .word   idt64_end - idt64 - 1   # Limit
    .quad   idt64                   # Base

/* ===================================================================== */
/* Exception/Interrupt Handlers */
/* ===================================================================== */

.extern handle_exception
.extern handle_irq
.extern handle_syscall

/* Macro to create exception handler with error code */
.macro ISR_ERROR_CODE num
.global isr\num
isr\num:
    push    $\num               # Push interrupt number
    jmp     isr_common
.endm

/* Macro to create exception handler without error code */
.macro ISR_NO_ERROR_CODE num
.global isr\num
isr\num:
    push    $0                  # Push dummy error code
    push    $\num               # Push interrupt number
    jmp     isr_common
.endm

/* Define all ISRs */
ISR_NO_ERROR_CODE 0     # Divide by zero
ISR_NO_ERROR_CODE 1     # Debug
ISR_NO_ERROR_CODE 2     # NMI
ISR_NO_ERROR_CODE 3     # Breakpoint
ISR_NO_ERROR_CODE 4     # Overflow
ISR_NO_ERROR_CODE 5     # Bound range exceeded
ISR_NO_ERROR_CODE 6     # Invalid opcode
ISR_NO_ERROR_CODE 7     # Device not available
ISR_ERROR_CODE 8        # Double fault
ISR_NO_ERROR_CODE 9     # Coprocessor segment overrun
ISR_ERROR_CODE 10       # Invalid TSS
ISR_ERROR_CODE 11       # Segment not present
ISR_ERROR_CODE 12       # Stack segment fault
ISR_ERROR_CODE 13       # General protection fault
ISR_ERROR_CODE 14       # Page fault
ISR_NO_ERROR_CODE 15    # Reserved
ISR_NO_ERROR_CODE 16    # x87 FPU error
ISR_ERROR_CODE 17       # Alignment check
ISR_NO_ERROR_CODE 18    # Machine check
ISR_NO_ERROR_CODE 19    # SIMD floating point
ISR_NO_ERROR_CODE 20    # Virtualization
ISR_NO_ERROR_CODE 21    # Reserved
ISR_NO_ERROR_CODE 22    # Reserved
ISR_NO_ERROR_CODE 23    # Reserved
ISR_NO_ERROR_CODE 24    # Reserved
ISR_NO_ERROR_CODE 25    # Reserved
ISR_NO_ERROR_CODE 26    # Reserved
ISR_NO_ERROR_CODE 27    # Reserved
ISR_NO_ERROR_CODE 28    # Reserved
ISR_NO_ERROR_CODE 29    # Reserved
ISR_ERROR_CODE 30       # Security exception
ISR_NO_ERROR_CODE 31    # Reserved

/* IRQ handlers (32-47) */
.macro IRQ_NO_ERROR_CODE num
.global isr\num
isr\num:
    push    $0
    push    $\num
    jmp     isr_irq_common
.endm

.irp num,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
IRQ_NO_ERROR_CODE \num
.endr

/* System call handler (128) */
ISR_NO_ERROR_CODE 128

/* Common ISR handler */
isr_common:
    /* Save all registers */
    push    %rax
    push    %rbx
    push    %rcx
    push    %rdx
    push    %rsi
    push    %rdi
    push    %rbp
    push    %r8
    push    %r9
    push    %r10
    push    %r11
    push    %r12
    push    %r13
    push    %r14
    push    %r15
    
    /* Call C handler with SysV stack alignment preserved. */
    sub     $8, %rsp
    mov     %rsp, %rdi
    add     $8, %rdi
    call    handle_exception
    add     $8, %rsp
    
    /* Restore registers */
    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %r11
    pop     %r10
    pop     %r9
    pop     %r8
    pop     %rbp
    pop     %rdi
    pop     %rsi
    pop     %rdx
    pop     %rcx
    pop     %rbx
    pop     %rax
    
    /* Remove error code and interrupt number */
    add     $16, %rsp
    
    /* Return from interrupt */
    iretq

/* Common hardware IRQ handler */
isr_irq_common:
    push    %rax
    push    %rbx
    push    %rcx
    push    %rdx
    push    %rsi
    push    %rdi
    push    %rbp
    push    %r8
    push    %r9
    push    %r10
    push    %r11
    push    %r12
    push    %r13
    push    %r14
    push    %r15

    sub     $8, %rsp
    mov     %rsp, %rdi
    add     $8, %rdi
    call    handle_irq
    add     $8, %rsp

    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %r11
    pop     %r10
    pop     %r9
    pop     %r8
    pop     %rbp
    pop     %rdi
    pop     %rsi
    pop     %rdx
    pop     %rcx
    pop     %rbx
    pop     %rax

    add     $16, %rsp
    iretq
