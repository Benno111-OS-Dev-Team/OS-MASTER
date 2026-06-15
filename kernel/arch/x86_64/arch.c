/*
 * OS8 Kernel - x86_64 Architecture Implementation
 */

#include "arch/arch.h"
#include "acpi.h"
#include "printk.h"
#include "types.h"

/* ===================================================================== */
/* Legacy PIC support for x86_64 bring-up */
/* ===================================================================== */

#define PIC1_COMMAND    0x20
#define PIC1_DATA       0x21
#define PIC2_COMMAND    0xA0
#define PIC2_DATA       0xA1

#define PIC_EOI         0x20
#define ICW1_INIT       0x10
#define ICW1_ICW4       0x01
#define ICW4_8086       0x01

extern void x86_64_install_exception_handlers(void);

static inline uint8_t pic_inb(uint16_t port)
{
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void pic_io_wait(void)
{
    outb(0x80, 0);
}

void pic_init(void)
{
    uint8_t mask1 = 0xFF;
    uint8_t mask2 = 0xFF;

    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    pic_io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    pic_io_wait();

    outb(PIC1_DATA, 0x20);
    pic_io_wait();
    outb(PIC2_DATA, 0x28);
    pic_io_wait();

    outb(PIC1_DATA, 0x04);
    pic_io_wait();
    outb(PIC2_DATA, 0x02);
    pic_io_wait();

    outb(PIC1_DATA, ICW4_8086);
    pic_io_wait();
    outb(PIC2_DATA, ICW4_8086);
    pic_io_wait();

    /* Keep everything masked except cascade; PIT unmasks IRQ0 later. */
    mask1 &= (uint8_t)~(1u << 2);
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI);
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_clear_mask(uint8_t irq)
{
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }

    value = pic_inb(port) & (uint8_t)~(1u << irq);
    outb(port, value);
}

/* ===================================================================== */
/* Early Initialization */
/* ===================================================================== */

void arch_early_init(void)
{
    printk(KERN_INFO "x86_64: Early initialization complete (legacy PIC/PIT mode)\n");
}

void arch_init(void)
{
    printk(KERN_INFO "x86_64: Full initialization complete\n");
}

/* ===================================================================== */
/* Interrupt Management */
/* ===================================================================== */

void arch_irq_enable(void)
{
    asm volatile("sti");
}

void arch_irq_disable(void)
{
    asm volatile("cli");
}

unsigned long arch_irq_save(void)
{
    unsigned long flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

void arch_irq_restore(unsigned long flags)
{
    asm volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
}

void arch_irq_init(void)
{
    x86_64_install_exception_handlers();
    printk(KERN_INFO "x86_64: IDT loaded for exceptions and IRQs\n");
    printk(KERN_INFO "x86_64: Initializing legacy PIC for bring-up\n");
    pic_init();
}

/* ===================================================================== */
/* Timer Management */
/* ===================================================================== */

static uint64_t timer_frequency = 100; /* PIT scheduler tick frequency */

void arch_timer_init(void)
{
    /* Initialize PIT/APIC timer - implemented in separate driver */
    extern void pit_init(void);
    pit_init();
}

uint64_t arch_timer_get_ticks(void)
{
    extern uint64_t pit_get_ticks(void);
    return pit_get_ticks();
}

uint64_t arch_timer_get_frequency(void)
{
    return timer_frequency;
}

uint64_t arch_timer_get_ms(void)
{
    return (arch_timer_get_ticks() * 1000) / timer_frequency;
}

void arch_timer_tick(void)
{
    /* PIT owns the x86_64 scheduler tick counter. */
}

/* ===================================================================== */
/* Memory Management */
/* ===================================================================== */

void arch_mmu_init(void)
{
    /* MMU is already enabled by bootloader */
    printk(KERN_INFO "x86_64: MMU already enabled\n");
}

void arch_mmu_enable(void)
{
    /* Already enabled */
}

void arch_mmu_switch_context(phys_addr_t pgd)
{
    asm volatile("mov %0, %%cr3" :: "r"(pgd) : "memory");
}

void arch_mmu_invalidate_tlb(virt_addr_t vaddr)
{
    if (vaddr == 0) {
        /* Invalidate all */
        uint64_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    } else {
        /* Invalidate single page */
        asm volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
    }
}

/* ===================================================================== */
/* Context Switching */
/* ===================================================================== */

void arch_context_switch(cpu_context_t *old_ctx, cpu_context_t *new_ctx)
{
    /* Save current context */
    asm volatile(
        "mov %%rax, %0\n"
        "mov %%rbx, %1\n"
        "mov %%rcx, %2\n"
        "mov %%rdx, %3\n"
        "mov %%rsi, %4\n"
        "mov %%rdi, %5\n"
        "mov %%rbp, %6\n"
        "mov %%rsp, %7\n"
        "mov %%r8, %8\n"
        "mov %%r9, %9\n"
        "mov %%r10, %10\n"
        "mov %%r11, %11\n"
        "mov %%r12, %12\n"
        "mov %%r13, %13\n"
        "mov %%r14, %14\n"
        "mov %%r15, %15\n"
        : "=m"(old_ctx->rax), "=m"(old_ctx->rbx), "=m"(old_ctx->rcx), "=m"(old_ctx->rdx),
          "=m"(old_ctx->rsi), "=m"(old_ctx->rdi), "=m"(old_ctx->rbp), "=m"(old_ctx->rsp),
          "=m"(old_ctx->r8), "=m"(old_ctx->r9), "=m"(old_ctx->r10), "=m"(old_ctx->r11),
          "=m"(old_ctx->r12), "=m"(old_ctx->r13), "=m"(old_ctx->r14), "=m"(old_ctx->r15)
    );
    
    /* Save RIP and RFLAGS */
    asm volatile(
        "lea 1f(%%rip), %%rax\n"
        "mov %%rax, %0\n"
        "pushfq\n"
        "pop %1\n"
        "1:\n"
        : "=m"(old_ctx->rip), "=m"(old_ctx->rflags)
        :: "rax"
    );
    
    /* Restore new context */
    asm volatile(
        "mov %0, %%rax\n"
        "mov %1, %%rbx\n"
        "mov %2, %%rcx\n"
        "mov %3, %%rdx\n"
        "mov %4, %%rsi\n"
        "mov %5, %%rdi\n"
        "mov %6, %%rbp\n"
        "mov %7, %%rsp\n"
        "mov %8, %%r8\n"
        "mov %9, %%r9\n"
        "mov %10, %%r10\n"
        "mov %11, %%r11\n"
        "mov %12, %%r12\n"
        "mov %13, %%r13\n"
        "mov %14, %%r14\n"
        "mov %15, %%r15\n"
        "push %16\n"
        "popfq\n"
        "jmp *%17\n"
        ::  "m"(new_ctx->rax), "m"(new_ctx->rbx), "m"(new_ctx->rcx), "m"(new_ctx->rdx),
            "m"(new_ctx->rsi), "m"(new_ctx->rdi), "m"(new_ctx->rbp), "m"(new_ctx->rsp),
            "m"(new_ctx->r8), "m"(new_ctx->r9), "m"(new_ctx->r10), "m"(new_ctx->r11),
            "m"(new_ctx->r12), "m"(new_ctx->r13), "m"(new_ctx->r14), "m"(new_ctx->r15),
            "m"(new_ctx->rflags), "m"(new_ctx->rip)
    );
}

void arch_context_init(cpu_context_t *ctx, void (*entry)(void*), void *stack, void *arg)
{
    /* Zero out context */
    for (int i = 0; i < sizeof(cpu_context_t); i++) {
        ((uint8_t*)ctx)[i] = 0;
    }
    
    /* Set up initial state */
    ctx->rip = (uint64_t)entry;
    ctx->rsp = (uint64_t)stack;
    ctx->rdi = (uint64_t)arg;  /* First argument in x86_64 calling convention */
    ctx->rflags = 0x202;       /* IF (interrupts enabled) */
    ctx->cs = 0x08;            /* Kernel code segment */
    ctx->ss = 0x10;            /* Kernel data segment */
}

/* ===================================================================== */
/* CPU Information */
/* ===================================================================== */

uint32_t arch_cpu_id(void)
{
    /* For now, single-core */
    return 0;
}

uint32_t arch_cpu_count(void)
{
    uint32_t cpu_count = acpi_madt_get_cpu_count();

    return cpu_count ? cpu_count : 1;
}

/* ===================================================================== */
/* SMP (Symmetric Multi-Processing) */
/* ===================================================================== */

void smp_init(void)
{
    uint32_t cpu_count = arch_cpu_count();

    printk(KERN_INFO "SMP: Initializing multiprocessor support (x86_64)\n");
    printk(KERN_INFO "SMP: Boot CPU (CPU 0) initialized\n");
    printk(KERN_INFO "SMP: ACPI reports %u CPU(s)\n", cpu_count);
}

/* ===================================================================== */
/* Userspace Entry */
/* ===================================================================== */

/**
 * arch_enter_userspace - Jump to userspace execution (Ring 3)
 * @entry: Entry point address in userspace
 * @sp: User stack pointer
 * @argc: Argument count (passed in rdi)
 * @argv: Argument vector pointer (passed in rsi)
 *
 * This function uses IRETQ to jump to Ring 3 userspace. It does not return.
 */
void arch_enter_userspace(uint64_t entry, uint64_t sp, uint64_t argc, uint64_t argv)
{
    printk(KERN_INFO "x86_64: Entering userspace at 0x%llx, sp=0x%llx\n",
           (unsigned long long)entry, (unsigned long long)sp);
    
    /*
     * IRETQ expects on stack (from bottom to top):
     *   SS     - User stack segment (0x23 = Ring 3 data)
     *   RSP    - User stack pointer
     *   RFLAGS - Flags (IF=1 for interrupts)
     *   CS     - User code segment (0x1B = Ring 3 code)
     *   RIP    - Entry point
     */
    
    /* Use separate asm blocks to avoid register pressure */
    
    /* First, set argc and argv in registers that won't be clobbered */
    register uint64_t r_argc asm("rdi") = argc;
    register uint64_t r_argv asm("rsi") = argv;
    
    /* Suppress unused warnings */
    (void)r_argc;
    (void)r_argv;
    
    asm volatile(
        /* Build IRETQ frame on stack */
        "pushq $0x23\n"      /* SS - Ring 3 data segment */
        "pushq %0\n"         /* RSP - user stack */
        "pushq $0x202\n"     /* RFLAGS - IF=1 */
        "pushq $0x1B\n"      /* CS - Ring 3 code segment */
        "pushq %1\n"         /* RIP - entry point */
        
        /* Clear other registers for security */
        "xor %%rax, %%rax\n"
        "xor %%rbx, %%rbx\n"
        "xor %%rcx, %%rcx\n"
        "xor %%rdx, %%rdx\n"
        "xor %%rbp, %%rbp\n"
        "xor %%r8, %%r8\n"
        "xor %%r9, %%r9\n"
        "xor %%r10, %%r10\n"
        "xor %%r11, %%r11\n"
        "xor %%r12, %%r12\n"
        "xor %%r13, %%r13\n"
        "xor %%r14, %%r14\n"
        "xor %%r15, %%r15\n"
        
        /* Jump to userspace */
        "iretq\n"
        :
        : "r" (sp), "r" (entry)
        : "memory", "rax", "rbx", "rcx", "rdx", "rbp",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
    );
    
    /* Should never reach here */
    __builtin_unreachable();
}

void arch_cpu_info(char *buf, size_t size)
{
    /* Use CPUID to get CPU info */
    uint32_t eax, ebx, ecx, edx;
    char vendor[13] = {0};
    
    /* Get vendor string */
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0));
    
    *(uint32_t*)(vendor + 0) = ebx;
    *(uint32_t*)(vendor + 4) = edx;
    *(uint32_t*)(vendor + 8) = ecx;
    
    /* Simple copy to buffer */
    size_t i;
    for (i = 0; i < size - 1 && vendor[i]; i++) {
        buf[i] = vendor[i];
    }
    buf[i] = '\0';
}

/* ===================================================================== */
/* Low-Level Utilities */
/* ===================================================================== */

void arch_halt(void)
{
    while (1) {
        asm volatile("cli; hlt");
    }
}

void arch_reboot(void)
{
    printk(KERN_INFO "POWER: reboot requested\n");

    if (acpi_reboot() == 0) {
        for (volatile int i = 0; i < 1000000; i++) {
        }
    }

    /* PCI reset control register */
    outb(0xCF9, 0x02);
    io_wait();
    outb(0xCF9, 0x06);
    io_wait();

    /* Legacy keyboard controller reset */
    outb(0x64, 0xFE);
    io_wait();

    arch_halt();
}

void arch_poweroff(void)
{
    printk(KERN_INFO "POWER: shutdown requested\n");

    if (acpi_poweroff() == 0) {
        for (volatile int i = 0; i < 1000000; i++) {
        }
    }

    /* Common QEMU/Bochs/VirtualBox poweroff ports */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);

    arch_halt();
}

void arch_idle(void)
{
    asm volatile("hlt");
}

void arch_barrier(void)
{
    asm volatile("mfence" ::: "memory");
}

void arch_dsb(void)
{
    asm volatile("mfence" ::: "memory");
}

void arch_isb(void)
{
    /* x86_64 doesn't have a direct equivalent, use serializing instruction */
    asm volatile("cpuid" ::: "eax", "ebx", "ecx", "edx", "memory");
}

/* ===================================================================== */
/* Exception/Interrupt Handlers */
/* ===================================================================== */

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} interrupt_frame_t;

static void exception_append_text(char **dst, size_t *remaining,
                                  const char *text)
{
    if (!dst || !*dst || !remaining || *remaining == 0 || !text)
        return;

    while (*text && *remaining > 1) {
        **dst = *text++;
        (*dst)++;
        (*remaining)--;
    }
    **dst = '\0';
}

static void exception_append_hex(char **dst, size_t *remaining, uint64_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    int started = 0;

    exception_append_text(dst, remaining, "0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint8_t digit = (uint8_t)((value >> shift) & 0xF);
        if (!started && digit == 0 && shift > 0)
            continue;
        started = 1;
        if (*remaining <= 1)
            break;
        **dst = hex[digit];
        (*dst)++;
        (*remaining)--;
        **dst = '\0';
    }
    if (!started && *remaining > 1) {
        **dst = '0';
        (*dst)++;
        (*remaining)--;
        **dst = '\0';
    }
}

void handle_exception(interrupt_frame_t *frame)
{
    char panic_msg[256];
    char *dst = panic_msg;
    size_t remaining = sizeof(panic_msg);
    uint64_t cr2 = 0;
    uint64_t cr2_valid = 0;
    extern void panic_with_context(const char *msg, uintptr_t caller_hint,
                                   uintptr_t stack_hint);

    if (!frame) {
        panic("x86_64 exception with null interrupt frame");
    }

    if (frame->int_no == 14) {
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        cr2_valid = 1;
    }

    panic_msg[0] = '\0';
    exception_append_text(&dst, &remaining, "Unhandled x86_64 exception ");
    exception_append_hex(&dst, &remaining, frame->int_no);
    exception_append_text(&dst, &remaining, " at RIP=");
    exception_append_hex(&dst, &remaining, frame->rip);
    exception_append_text(&dst, &remaining, " ERR=");
    exception_append_hex(&dst, &remaining, frame->err_code);
    exception_append_text(&dst, &remaining, " RSP=");
    exception_append_hex(&dst, &remaining, frame->rsp);
    if (cr2_valid) {
        exception_append_text(&dst, &remaining, " CR2=");
        exception_append_hex(&dst, &remaining, cr2);
    }

    panic_with_context(panic_msg, (uintptr_t)frame->rip, (uintptr_t)frame->rsp);
}

void handle_irq(interrupt_frame_t *frame)
{
    uint8_t irq;

    if (!frame || frame->int_no < 32 || frame->int_no > 47)
        return;

    irq = (uint8_t)(frame->int_no - 32);

    if (irq == 0) {
        extern void pit_handler(void);
        pit_handler();
        return;
    }

    pic_send_eoi(irq);
}
