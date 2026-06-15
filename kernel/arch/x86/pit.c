/*
 * x86 PIT (Programmable Interval Timer) Driver
 * 8253/8254 PIT for x86 32-bit
 */

#include "types.h"

/* PIT ports */
#define PIT_CHANNEL0    0x40
#define PIT_CHANNEL1    0x41
#define PIT_CHANNEL2    0x42
#define PIT_COMMAND     0x43

/* PIT frequency */
#define PIT_FREQUENCY   1193182  /* Hz */
#define TIMER_HZ        100      /* 100 Hz = 10ms tick */

#define PIT_CMD_LATCH       0x00
#define PIT_CMD_CHANNEL0    0x00

static const uint16_t pit_reload = (uint16_t)(PIT_FREQUENCY / TIMER_HZ);

/* I/O port operations */
static inline void outb(uint16_t port, uint8_t value)
{
    asm volatile("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void pit_init(void)
{
    /* Calculate divisor for desired frequency */
    uint32_t divisor = pit_reload;
    
    /* Send command byte */
    /* Channel 0, lobyte/hibyte, rate generator, binary mode */
    outb(PIT_COMMAND, 0x36);
    
    /* Send divisor */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
    
    /* Enable IRQ 0 (timer interrupt) */
    extern void pic_clear_mask(uint8_t irq);
    pic_clear_mask(0);
}

static volatile uint64_t pit_ticks = 0;
static uint16_t pit_last_count = 0;
static bool pit_poll_started = false;

static int pit_interrupts_enabled(void)
{
    uint32_t flags;
    asm volatile("pushfl; popl %0" : "=r"(flags) :: "memory");
    return (flags & (1U << 9)) != 0;
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

    /* Call architecture timer tick */
    extern void arch_timer_tick(void);
    arch_timer_tick();
    
    /* Send EOI to PIC */
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
    uint64_t ticks_to_wait = ((uint64_t)ms * TIMER_HZ + 999ULL) / 1000ULL;
    uint64_t target;

    if (ticks_to_wait == 0)
        ticks_to_wait = 1;

    target = pit_get_ticks() + ticks_to_wait;
    while (pit_get_ticks() < target) {
        if (pit_interrupts_enabled())
            asm volatile("hlt");
        else
            asm volatile("pause");
    }
}
