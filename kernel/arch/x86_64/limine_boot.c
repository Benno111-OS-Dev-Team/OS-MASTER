/*
 * OS8 Limine boot entry for x86_64
 *
 * Based on working-os pattern that boots successfully on real hardware.
 * Uses the Limine Boot Protocol for clean 64-bit entry.
 */

#include "arch/x86_64/boot_handoff.h"
#include "types.h"

#define BOOT_FB_MAX_DIMENSION 16384ULL

/* ========== Limine Structures ========== */

struct limine_framebuffer {
    void *address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
    uint8_t unused[7];
    uint64_t edid_size;
    void *edid;
    uint64_t mode_count;
    void **modes;
};

struct limine_framebuffer_response {
    uint64_t revision;
    uint64_t framebuffer_count;
    struct limine_framebuffer **framebuffers;
};

struct limine_framebuffer_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_framebuffer_response *response;
};

struct limine_rsdp_response {
    uint64_t revision;
    void *address;
};

struct limine_rsdp_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_rsdp_response *response;
};

struct limine_hhdm_response {
    uint64_t revision;
    uint64_t offset;
};

struct limine_hhdm_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_hhdm_response *response;
};

struct limine_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct limine_memmap_response {
    uint64_t revision;
    uint64_t entry_count;
    struct limine_memmap_entry **entries;
};

struct limine_memmap_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_memmap_response *response;
};

struct limine_uuid {
    uint32_t a;
    uint16_t b;
    uint16_t c;
    uint8_t d[8];
};

struct limine_file {
    uint64_t revision;
    void *address;
    uint64_t size;
    char *path;
    char *cmdline;
    uint32_t media_type;
    uint32_t unused;
    uint32_t tftp_ip;
    uint32_t tftp_port;
    uint32_t partition_index;
    uint32_t mbr_disk_id;
    struct limine_uuid gpt_disk_uuid;
    struct limine_uuid gpt_part_uuid;
    struct limine_uuid part_uuid;
};

struct limine_kernel_file_response {
    uint64_t revision;
    struct limine_file *kernel_file;
};

struct limine_kernel_file_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_kernel_file_response *response;
};

/* ========== Limine Requests ========== */

/* Place requests in dedicated section - using direct magic values like working-os */
__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_requests_start_marker[4] = {
    0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf,
    0x785c6ed015d3e316, 0x181e920a7852b9d9
};

/* Base revision 2 - like working-os */
__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[3] = {
    0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, 2
};

/* Framebuffer request */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = {0xc7b1dd30df4c8b88, 0x0a82e883a194f07b,
           0x9d5827dcd881dd75, 0xa3148604f6fab11b},
    .revision = 0,
    .response = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = {0xc5e77b6b397e7b43, 0x27637845accdcf3c, 0, 0},
    .revision = 0,
    .response = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = {0x48dcf1cb8ad2b852, 0x63984e959a98244b, 0, 0},
    .revision = 0,
    .response = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = {0x67cf3d9d378a806f, 0xe304acdfc50c3c62, 0, 0},
    .revision = 0,
    .response = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_kernel_file_request kernel_file_request = {
    .id = {0xc7b1dd30df4c8b88, 0x0a82e883a194f07b,
           0xad97e90e83f1ed67, 0x31eb5d1c5ff23b69},
    .revision = 0,
    .response = 0
};

/* Request end marker */
__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_requests_end_marker[2] = {
    0xadc0e0531bb10d03, 0x9572709f31764c62
};

/* ========== Serial Debug (COM1) ========== */

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static void serial_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0);
    outb(COM1, c);
}

static void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

static void serial_puthex(uint64_t val) {
    const char *hex = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        serial_putc(hex[(val >> i) & 0xF]);
    }
}

/* ========== Globals ========== */

static struct limine_framebuffer *g_fb = 0;
static void *g_rsdp = 0;
static uint64_t g_hhdm_offset = 0;
static const char *g_kernel_cmdline = 0;
static void *g_bootstrap_file_addr = 0;
static uint64_t g_bootstrap_file_size = 0;
static void *g_kernel_file_addr = 0;
static uint64_t g_kernel_file_size = 0;
static uint64_t g_usable_memory_base = 0;
static uint64_t g_usable_memory_size = 0;
static uint64_t g_total_usable_memory = 0;

struct idt_entry64 {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

extern struct idt_entry64 idt64[256];
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);
extern void isr32(void);
extern void isr33(void);
extern void isr34(void);
extern void isr35(void);
extern void isr36(void);
extern void isr37(void);
extern void isr38(void);
extern void isr39(void);
extern void isr40(void);
extern void isr41(void);
extern void isr42(void);
extern void isr43(void);
extern void isr44(void);
extern void isr45(void);
extern void isr46(void);
extern void isr47(void);
extern void isr128(void);

struct idt_descriptor64 {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static uint16_t boot_read_cs_selector(void) {
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    return cs;
}

static void set_idt_gate(int vector, void (*handler)(void)) {
    uintptr_t addr = (uintptr_t)handler;

    idt64[vector].offset_low = (uint16_t)(addr & 0xFFFF);
    idt64[vector].selector = boot_read_cs_selector();
    idt64[vector].ist = 0;
    idt64[vector].type_attr = 0x8E;
    idt64[vector].offset_mid = (uint16_t)((addr >> 16) & 0xFFFF);
    idt64[vector].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt64[vector].reserved = 0;
}

void x86_64_boot_init_idt(void) {
    for (int i = 0; i < 256; i++) {
        idt64[i] = (struct idt_entry64){0};
    }

    set_idt_gate(0, isr0);
    set_idt_gate(1, isr1);
    set_idt_gate(2, isr2);
    set_idt_gate(3, isr3);
    set_idt_gate(4, isr4);
    set_idt_gate(5, isr5);
    set_idt_gate(6, isr6);
    set_idt_gate(7, isr7);
    set_idt_gate(8, isr8);
    set_idt_gate(9, isr9);
    set_idt_gate(10, isr10);
    set_idt_gate(11, isr11);
    set_idt_gate(12, isr12);
    set_idt_gate(13, isr13);
    set_idt_gate(14, isr14);
    set_idt_gate(15, isr15);
    set_idt_gate(16, isr16);
    set_idt_gate(17, isr17);
    set_idt_gate(18, isr18);
    set_idt_gate(19, isr19);
    set_idt_gate(20, isr20);
    set_idt_gate(21, isr21);
    set_idt_gate(22, isr22);
    set_idt_gate(23, isr23);
    set_idt_gate(24, isr24);
    set_idt_gate(25, isr25);
    set_idt_gate(26, isr26);
    set_idt_gate(27, isr27);
    set_idt_gate(28, isr28);
    set_idt_gate(29, isr29);
    set_idt_gate(30, isr30);
    set_idt_gate(31, isr31);
    set_idt_gate(32, isr32);
    set_idt_gate(33, isr33);
    set_idt_gate(34, isr34);
    set_idt_gate(35, isr35);
    set_idt_gate(36, isr36);
    set_idt_gate(37, isr37);
    set_idt_gate(38, isr38);
    set_idt_gate(39, isr39);
    set_idt_gate(40, isr40);
    set_idt_gate(41, isr41);
    set_idt_gate(42, isr42);
    set_idt_gate(43, isr43);
    set_idt_gate(44, isr44);
    set_idt_gate(45, isr45);
    set_idt_gate(46, isr46);
    set_idt_gate(47, isr47);
    set_idt_gate(128, isr128);
}

void x86_64_install_exception_handlers(void) {
    struct idt_descriptor64 idtr;
    x86_64_boot_init_idt();
    idtr.limit = (uint16_t)(sizeof(idt64) - 1);
    idtr.base = (uint64_t)(uintptr_t)idt64;
    __asm__ volatile("lidt %0" : : "m"(idtr));
}

/* ========== Framebuffer Info for kernel ========== */

int limine_get_framebuffer(uint32_t **buffer, uint32_t *width,
                           uint32_t *height, uint32_t *pitch) {
    if (!g_fb || !g_fb->address) {
        return -1;
    }
    if (buffer) *buffer = (uint32_t *)g_fb->address;
    if (width) *width = (uint32_t)g_fb->width;
    if (height) *height = (uint32_t)g_fb->height;
    if (pitch) *pitch = (uint32_t)g_fb->pitch;
    return 0;
}

void *limine_get_rsdp(void) { return g_rsdp; }

uint64_t limine_get_hhdm_offset(void) { return g_hhdm_offset; }

const char *limine_get_kernel_cmdline(void) { return g_kernel_cmdline; }

void *limine_get_bootstrap_file_addr(void) { return g_bootstrap_file_addr; }

uint64_t limine_get_bootstrap_file_size(void) { return g_bootstrap_file_size; }

void *limine_get_kernel_file_addr(void) { return g_kernel_file_addr; }

uint64_t limine_get_kernel_file_size(void) { return g_kernel_file_size; }

uint64_t limine_get_usable_memory_base(void) { return g_usable_memory_base; }

uint64_t limine_get_usable_memory_size(void) { return g_usable_memory_size; }

uint64_t limine_get_total_usable_memory(void) { return g_total_usable_memory; }

/* ========== Direct Screen Test ========== */

static int boot_framebuffer_is_sane(const struct limine_framebuffer *fb) {
    uint64_t min_pitch;

    if (!fb || !fb->address || !fb->width || !fb->height)
        return 0;
    if (fb->bpp != 32)
        return 0;
    if (fb->width > BOOT_FB_MAX_DIMENSION ||
        fb->height > BOOT_FB_MAX_DIMENSION)
        return 0;

    min_pitch = fb->width * 4ULL;
    if (fb->pitch < min_pitch)
        return 0;
    if (fb->pitch > min_pitch * 8ULL)
        return 0;

    return 1;
}

static void draw_test_pattern(void *fb_addr, uint64_t width, uint64_t height, uint64_t pitch) {
    volatile uint8_t *fb = (volatile uint8_t *)fb_addr;

    serial_puts("Drawing test pattern...\n");

    /* Fill entire screen with a gradient - direct pixel writes */
    for (uint64_t y = 0; y < height; y++) {
        volatile uint32_t *row = (volatile uint32_t *)(fb + y * pitch);
        for (uint64_t x = 0; x < width; x++) {
            /* Create a nice gradient: purple to blue */
            uint8_t r = 50;
            uint8_t g = (y * 50 / height) + 20;
            uint8_t b = 100 + (y * 100 / height);
            row[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }

    serial_puts("Test pattern complete!\n");

    /* Draw a white rectangle in center as focus point */
    uint64_t cx = width / 2 - 100;
    uint64_t cy = height / 2 - 50;
    for (uint64_t y = cy; y < cy + 100 && y < height; y++) {
        volatile uint32_t *row = (volatile uint32_t *)(fb + y * pitch);
        for (uint64_t x = cx; x < cx + 200 && x < width; x++) {
            row[x] = 0xFFFFFFFF; /* White */
        }
    }

    /* Draw "OS8" text approximation with colored blocks */
    uint64_t text_y = height / 2 - 20;
    uint64_t text_x = width / 2 - 80;
    volatile uint32_t *text_row = (volatile uint32_t *)(fb + text_y * pitch);

    /* V */
    for (int i = 0; i < 30; i++) text_row[text_x + i] = 0xFF00FF00;
    text_x += 35;
    /* I */
    for (int i = 0; i < 15; i++) text_row[text_x + i] = 0xFF00FF00;
    text_x += 20;
    /* B */
    for (int i = 0; i < 25; i++) text_row[text_x + i] = 0xFF00FF00;
    text_x += 30;
    /* - */
    for (int i = 0; i < 15; i++) text_row[text_x + i] = 0xFFFFFF00;
    text_x += 20;
    /* O */
    for (int i = 0; i < 25; i++) text_row[text_x + i] = 0xFF00FFFF;
    text_x += 30;
    /* S */
    for (int i = 0; i < 20; i++) text_row[text_x + i] = 0xFF00FFFF;
}

/* ========== Halt ========== */

static void halt(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void emergency_append_text(char **dst, uint64_t *remaining,
                                  const char *text) {
    if (!dst || !*dst || !remaining || *remaining == 0 || !text)
        return;
    while (*text && *remaining > 1) {
        **dst = *text++;
        (*dst)++;
        (*remaining)--;
    }
    **dst = '\0';
}

static void emergency_append_hex(char **dst, uint64_t *remaining,
                                 uint64_t value) {
    static const char hex[] = "0123456789ABCDEF";
    int started = 0;

    emergency_append_text(dst, remaining, "0x");
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

void x86_64_boot_emergency_exception(uint64_t int_no, uint64_t rip,
                                     uint64_t err_code, uint64_t cr2_valid,
                                     uint64_t cr2) {
    char panic_msg[256];
    char *dst = panic_msg;
    uint64_t remaining = sizeof(panic_msg);
    extern void panic_with_context(const char *msg, uintptr_t caller_hint,
                                   uintptr_t stack_hint);

    serial_init();
    serial_puts("\nEARLY EXCEPTION ");
    serial_puthex(int_no);
    serial_puts(" RIP=");
    serial_puthex(rip);
    serial_puts(" ERR=");
    serial_puthex(err_code);
    if (cr2_valid) {
        serial_puts(" CR2=");
        serial_puthex(cr2);
    }
    serial_puts("\n");

    panic_msg[0] = '\0';
    emergency_append_text(&dst, &remaining, "Early x86_64 exception ");
    emergency_append_hex(&dst, &remaining, int_no);
    emergency_append_text(&dst, &remaining, " at RIP=");
    emergency_append_hex(&dst, &remaining, rip);
    emergency_append_text(&dst, &remaining, " ERR=");
    emergency_append_hex(&dst, &remaining, err_code);
    if (cr2_valid) {
        emergency_append_text(&dst, &remaining, " CR2=");
        emergency_append_hex(&dst, &remaining, cr2);
    }

    panic_with_context(panic_msg, (uintptr_t)rip, (uintptr_t)0);
    halt();
}

/* ========== Kernel Main Declaration ========== */

extern void kernel_main(void *dtb);

static void limine_capture_memory_map(void) {
    struct limine_memmap_response *response = memmap_request.response;
    uint64_t best_base = 0;
    uint64_t best_size = 0;
    uint64_t total = 0;

    if (!response || !response->entries)
        return;

    for (uint64_t i = 0; i < response->entry_count; i++) {
        struct limine_memmap_entry *entry = response->entries[i];
        if (!entry || entry->type != 0 || entry->length == 0)
            continue;

        total += entry->length;
        if (entry->length > best_size) {
            best_base = entry->base;
            best_size = entry->length;
        }
    }

    g_usable_memory_base = best_base;
    g_usable_memory_size = best_size;
    g_total_usable_memory = total;
}

static void limine_capture_direct_boot_state(void) {
    if (rsdp_request.response) {
        g_rsdp = rsdp_request.response->address;
    }
    if (hhdm_request.response) {
        g_hhdm_offset = hhdm_request.response->offset;
    }
    if (kernel_file_request.response && kernel_file_request.response->kernel_file) {
        g_kernel_cmdline = kernel_file_request.response->kernel_file->cmdline;
        g_bootstrap_file_addr = kernel_file_request.response->kernel_file->address;
        g_bootstrap_file_size = kernel_file_request.response->kernel_file->size;
        g_kernel_file_addr = g_bootstrap_file_addr;
        g_kernel_file_size = g_bootstrap_file_size;
    }
    limine_capture_memory_map();
}

static int limine_seed_loader_handoff(const os8_boot_handoff_t *handoff) {
    if (!handoff || handoff->magic != OS8_BOOT_HANDOFF_MAGIC ||
        handoff->version != OS8_BOOT_HANDOFF_VERSION) {
        serial_puts("ERROR: Invalid loader handoff\n");
        return -1;
    }
    if (!handoff->kernel_file_addr || !handoff->kernel_file_size ||
        !handoff->bootstrap_file_addr || !handoff->bootstrap_file_size) {
        serial_puts("ERROR: Invalid loader payload handoff\n");
        return -1;
    }

    static struct limine_framebuffer loader_fb;
    loader_fb.address = (void *)(uintptr_t)handoff->framebuffer_addr;
    loader_fb.width = handoff->framebuffer_width;
    loader_fb.height = handoff->framebuffer_height;
    loader_fb.pitch = handoff->framebuffer_pitch;
    loader_fb.bpp = 32;
    if (!boot_framebuffer_is_sane(&loader_fb)) {
        serial_puts("ERROR: Invalid loader framebuffer handoff\n");
        return -1;
    }

    g_fb = &loader_fb;
    g_rsdp = (void *)(uintptr_t)handoff->rsdp_addr;
    g_hhdm_offset = handoff->hhdm_offset;
    g_kernel_cmdline = (const char *)(uintptr_t)handoff->cmdline_addr;
    g_bootstrap_file_addr = (void *)(uintptr_t)handoff->bootstrap_file_addr;
    g_bootstrap_file_size = handoff->bootstrap_file_size;
    g_kernel_file_addr = (void *)(uintptr_t)handoff->kernel_file_addr;
    g_kernel_file_size = handoff->kernel_file_size;
    g_usable_memory_base = 0;
    g_usable_memory_size = 0;
    g_total_usable_memory = 0;
    return 0;
}

/* ========== Entry Point ========== */

void limine_entry_main(void) {
    /* Initialize serial for debug output */
    serial_init();
    serial_puts("\n\n=== OS8 ===\n");
    serial_puts("Kernel entry point reached!\n");

    x86_64_install_exception_handlers();
    serial_puts("Boot IDT installed\n");

    /* Verify base revision was accepted */
    if (limine_base_revision[2] != 0) {
        serial_puts("ERROR: Limine base revision mismatch\n");
        serial_puts("Revision value: ");
        serial_puthex(limine_base_revision[2]);
        serial_puts("\n");
        halt();
    }
    serial_puts("Limine base revision OK\n");

    /* Get framebuffer */
    if (framebuffer_request.response == 0) {
        serial_puts("ERROR: No framebuffer response!\n");
        halt();
    }

    if (framebuffer_request.response->framebuffer_count < 1 ||
        !framebuffer_request.response->framebuffers) {
        serial_puts("ERROR: No framebuffers available!\n");
        halt();
    }

    g_fb = framebuffer_request.response->framebuffers[0];
    if (!boot_framebuffer_is_sane(g_fb)) {
        serial_puts("ERROR: Invalid framebuffer response!\n");
        halt();
    }
    limine_capture_direct_boot_state();

    serial_puts("Framebuffer acquired:\n");
    serial_puts("  Address: ");
    serial_puthex((uint64_t)g_fb->address);
    serial_puts("\n  Width: ");
    serial_puthex(g_fb->width);
    serial_puts("\n  Height: ");
    serial_puthex(g_fb->height);
    serial_puts("\n  Pitch: ");
    serial_puthex(g_fb->pitch);
    serial_puts("\n  BPP: ");
    serial_puthex(g_fb->bpp);
    serial_puts("\n  RSDP: ");
    serial_puthex((uint64_t)g_rsdp);
    serial_puts("\n  HHDM: ");
    serial_puthex(g_hhdm_offset);
    serial_puts("\n  Cmdline ptr: ");
    serial_puthex((uint64_t)g_kernel_cmdline);
    serial_puts("\n  Usable memory base: ");
    serial_puthex(g_usable_memory_base);
    serial_puts("\n  Usable memory size: ");
    serial_puthex(g_usable_memory_size);
    serial_puts("\n");

    serial_puts("Calling kernel_main...\n");

    /* Call kernel main - pass NULL for DTB on x86_64 */
    kernel_main(0);

    /* Should never reach here */
    serial_puts("kernel_main returned!\n");
    halt();
}

void limine_loader_entry_main(const os8_boot_handoff_t *handoff) {
    serial_init();
    serial_puts("\n\n=== OS8 second-stage kernel ===\n");

    x86_64_install_exception_handlers();
    serial_puts("Boot IDT installed from loader handoff\n");

    if (limine_seed_loader_handoff(handoff) != 0) {
        halt();
    }

    serial_puts("Framebuffer acquired from loader handoff:\n");
    serial_puts("  Address: ");
    serial_puthex((uint64_t)g_fb->address);
    serial_puts("\n  Width: ");
    serial_puthex(g_fb->width);
    serial_puts("\n  Height: ");
    serial_puthex(g_fb->height);
    serial_puts("\n  Pitch: ");
    serial_puthex(g_fb->pitch);
    serial_puts("\n  Kernel payload: ");
    serial_puthex((uint64_t)g_kernel_file_addr);
    serial_puts("\n");

    kernel_main(0);
    serial_puts("kernel_main returned!\n");
    halt();
}
