/*
 * OS8 Kernel - x86_64 PIT (Programmable Interval Timer) Driver
 * 
 * 8253/8254 PIT for system timer
 */

#include "arch/arch.h"
#include "printk.h"
#include "types.h"

/* ===================================================================== */
/* PIT Ports */
/* ===================================================================== */

#define PIT_CHANNEL0    0x40
#define PIT_CHANNEL1    0x41
#define PIT_CHANNEL2    0x42
#define PIT_COMMAND     0x43

/* Command register bits */
#define PIT_CMD_BINARY      0x00    /* Use binary counter */
#define PIT_CMD_BCD         0x01    /* Use BCD counter */
#define PIT_CMD_MODE0       0x00    /* Interrupt on terminal count */
#define PIT_CMD_MODE1       0x02    /* Hardware re-triggerable one-shot */
#define PIT_CMD_MODE2       0x04    /* Rate generator */
#define PIT_CMD_MODE3       0x06    /* Square wave generator */
#define PIT_CMD_MODE4       0x08    /* Software triggered strobe */
#define PIT_CMD_MODE5       0x0A    /* Hardware triggered strobe */
#define PIT_CMD_LATCH       0x00    /* Latch count value */
#define PIT_CMD_LSB         0x10    /* Read/write LSB only */
#define PIT_CMD_MSB         0x20    /* Read/write MSB only */
#define PIT_CMD_BOTH        0x30    /* Read/write LSB then MSB */
#define PIT_CMD_CHANNEL0    0x00
#define PIT_CMD_CHANNEL1    0x40
#define PIT_CMD_CHANNEL2    0x80
#define PIT_CMD_READBACK    0xC0

/* PIT frequency */
#define PIT_FREQUENCY   1193182     /* Hz */
#define TIMER_HZ        100         /* 100 Hz = 10ms tick */

static const uint16_t pit_reload = (uint16_t)(PIT_FREQUENCY / TIMER_HZ);

/* ===================================================================== */
/* Initialization */
/* ===================================================================== */

void pit_init(void)
{
    /* Set up channel 0 for the scheduler tick. */
    uint32_t divisor = pit_reload;
    
    printk(KERN_INFO "PIT: Initializing at %u Hz (divisor=%u)\n", TIMER_HZ, divisor);
    
    /* Send command byte */
    outb(PIT_COMMAND, PIT_CMD_CHANNEL0 | PIT_CMD_BOTH | PIT_CMD_MODE2 | PIT_CMD_BINARY);
    
    /* Send divisor */
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);

    /* Enable IRQ 0 (timer interrupt). */
    extern void pic_clear_mask(uint8_t irq);
    pic_clear_mask(0);
    
    printk(KERN_INFO "PIT: Initialized\n");
}

/* ===================================================================== */
/* Timer Functions */
/* ===================================================================== */

static volatile uint64_t pit_ticks = 0;
static uint16_t pit_last_count = 0;
static bool pit_poll_started = false;

static int pit_interrupts_enabled(void)
{
    unsigned long flags;
    asm volatile("pushfq; pop %0" : "=r"(flags) :: "memory");
    return (flags & (1UL << 9)) != 0;
}

static uint16_t pit_read_counter(void)
{
    uint8_t lo;
    uint8_t hi;

    outb(PIT_COMMAND, PIT_CMD_CHANNEL0 | PIT_CMD_LATCH);
    lo = inb(PIT_CHANNEL0);
    hi = inb(PIT_CHANNEL0);
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static void pit_poll_ticks(void)
{
    uint16_t count;

    if (pit_interrupts_enabled())
        return;

    count = pit_read_counter();
    if (!pit_poll_started) {
        pit_last_count = count;
        pit_poll_started = true;
        return;
    }

    if (count > pit_last_count)
        pit_ticks++;

    pit_last_count = count;
}

void pit_handler(void)
{
    pit_ticks++;
    pit_poll_started = false;
    extern void arch_timer_tick(void);
    arch_timer_tick();

    /* Acknowledge before switching so the PIC can deliver future ticks. */
    extern void pic_send_eoi(uint8_t irq);
    pic_send_eoi(0);

    extern void process_preempt_from_irq(void);
    process_preempt_from_irq();
}

uint64_t pit_get_ticks(void)
{
    pit_poll_ticks();
    return pit_ticks;
}

void pit_sleep(uint32_t ms)
{
    if (ms == 0)
        return;

    /*
     * Sleep against the architecture timer API so callers operate in true
     * milliseconds regardless of the underlying PIT tick rate.
     */
    uint64_t ticks_to_wait = ((uint64_t)ms * TIMER_HZ + 999ULL) / 1000ULL;
    if (ticks_to_wait == 0)
        ticks_to_wait = 1;
    uint64_t target = pit_get_ticks() + ticks_to_wait;
    while (pit_get_ticks() < target) {
        if (pit_interrupts_enabled())
            arch_idle();
        else
            asm volatile("pause");
    }
}
