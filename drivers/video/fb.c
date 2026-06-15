/*
 * OS8 - virtio-gpu Framebuffer Driver
 * 
 * Simple framebuffer using virtio-gpu for QEMU/UTM display.
 */

#include "types.h"
#include "gui/font.h"
#include "media/media.h"
#include "printk.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

/* ===================================================================== */
/* virtio-gpu MMIO Base Addresses */
/* ===================================================================== */

/* QEMU virt machine virtio MMIO base (can have multiple devices) */
#define VIRTIO_MMIO_BASE        0x0A000000UL
#define VIRTIO_MMIO_SIZE        0x200

/* virtio magic value */
#define VIRTIO_MAGIC            0x74726976  /* "virt" */

/* Device types */
#define VIRTIO_DEV_NET          1
#define VIRTIO_DEV_BLK          2
#define VIRTIO_DEV_CONSOLE      3
#define VIRTIO_DEV_GPU          16

/* MMIO Register offsets */
#define VIRTIO_MMIO_MAGIC           0x000
#define VIRTIO_MMIO_VERSION         0x004
#define VIRTIO_MMIO_DEVICE_ID       0x008
#define VIRTIO_MMIO_VENDOR_ID       0x00C
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020
#define VIRTIO_MMIO_QUEUE_SEL       0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034
#define VIRTIO_MMIO_QUEUE_NUM       0x038
#define VIRTIO_MMIO_QUEUE_READY     0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK   0x064
#define VIRTIO_MMIO_STATUS          0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW  0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW 0x090
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW 0x0A0
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH 0x0A4

/* Device status bits */
#define VIRTIO_STATUS_ACK       1
#define VIRTIO_STATUS_DRIVER    2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8

/* ===================================================================== */
/* Simple Framebuffer (without full virtio-gpu) */
/* ===================================================================== */

/* For QEMU virt with ramfb or simple-framebuffer */
#define SIMPLE_FB_BASE      0x0C000000UL  /* ramfb memory */
#define SIMPLE_FB_WIDTH     1024
#define SIMPLE_FB_HEIGHT    768
#define SIMPLE_FB_BPP       32

static struct {
    uint32_t *buffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    bool initialized;
} framebuffer = {0};

static uint32_t fb_blend_rgb(uint32_t dst, uint32_t src, uint8_t alpha) {
    uint32_t inv = 255 - (uint32_t)alpha;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8) & 0xFF;
    uint32_t db = dst & 0xFF;
    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >> 8) & 0xFF;
    uint32_t sb = src & 0xFF;

    uint32_t r = (sr * alpha + dr * inv + 127) / 255;
    uint32_t g = (sg * alpha + dg * inv + 127) / 255;
    uint32_t b = (sb * alpha + db * inv + 127) / 255;
    return (r << 16) | (g << 8) | b;
}

/* ===================================================================== */
/* Framebuffer Operations */
/* ===================================================================== */

void fb_clear(uint32_t color)
{
    uint32_t stride;

    if (!framebuffer.initialized) return;

    stride = framebuffer.pitch ? (framebuffer.pitch / 4) : framebuffer.width;
    
    for (uint32_t y = 0; y < framebuffer.height; y++) {
        for (uint32_t x = 0; x < framebuffer.width; x++) {
            framebuffer.buffer[y * stride + x] = color;
        }
    }
}

void fb_put_pixel(int x, int y, uint32_t color)
{
    uint32_t stride;

    if (!framebuffer.initialized) return;
    if (x < 0 || x >= (int)framebuffer.width) return;
    if (y < 0 || y >= (int)framebuffer.height) return;

    stride = framebuffer.pitch ? (framebuffer.pitch / 4) : framebuffer.width;
    
    framebuffer.buffer[y * stride + x] = color;
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            fb_put_pixel(col, row, color);
        }
    }
}

void fb_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg)
{
    const uint8_t *glyph;

    if ((unsigned char)c < 32 || c == 127) c = ' ';
    glyph = font_data[(unsigned char)c];
    
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t line = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            uint32_t color = (line & (0x80 >> col)) ? fg : bg;
            fb_put_pixel(x + col, y + row, color);
        }
    }
}

void fb_draw_string(int x, int y, const char *str, uint32_t fg, uint32_t bg)
{
    while (*str) {
        fb_draw_char(x, y, *str++, fg, bg);
        x += FONT_WIDTH;
    }
}

static void fb_draw_image_scaled(int x, int y, int w, int h,
                                 const media_image_t *image) {
    uint32_t stride;

    if (!image || !image->pixels || image->width == 0 || image->height == 0 ||
        w <= 0 || h <= 0)
        return;

    stride = framebuffer.pitch ? (framebuffer.pitch / 4) : framebuffer.width;
    if (!stride)
        stride = framebuffer.width;

    for (int dy = 0; dy < h; dy++) {
        uint32_t src_y = ((uint32_t)dy * image->height) / (uint32_t)h;
        int py = y + dy;

        if (py < 0 || py >= (int)framebuffer.height)
            continue;

        for (int dx = 0; dx < w; dx++) {
            uint32_t src_x = ((uint32_t)dx * image->width) / (uint32_t)w;
            uint32_t src = image->pixels[src_y * image->width + src_x];
            int px = x + dx;
            uint32_t *dst;

            if (px < 0 || px >= (int)framebuffer.width)
                continue;

            dst = &framebuffer.buffer[py * stride + px];
            if ((src >> 24) == 0xFF) {
                *dst = src & 0x00FFFFFF;
            } else if ((src >> 24) != 0x00) {
                *dst = fb_blend_rgb(*dst, src & 0x00FFFFFF,
                                    (uint8_t)(src >> 24));
            }
        }
    }
}

/* ===================================================================== */
/* Boot Log Screen */
/* ===================================================================== */

static void fb_draw_wrapped_text(int x, int y, int max_chars, const char *text,
                                 uint32_t fg, uint32_t bg, int max_lines) {
    char line[128];
    int line_len = 0;
    int lines_drawn = 0;

    if (!text || max_chars <= 0 || max_lines <= 0)
        return;
    if (max_chars > (int)sizeof(line) - 1)
        max_chars = (int)sizeof(line) - 1;

    for (size_t i = 0;; i++) {
        char c = text[i];
        int flush = 0;

        if (c == '\0' || c == '\n' || line_len >= max_chars) {
            flush = 1;
        } else {
            line[line_len++] = c;
        }

        if (flush) {
            line[line_len] = '\0';
            fb_draw_string(x, y + lines_drawn * (FONT_HEIGHT + 2), line, fg, bg);
            lines_drawn++;
            line_len = 0;
            if (lines_drawn >= max_lines || c == '\0')
                break;
            if (c != '\0' && c != '\n' && line_len < max_chars)
                line[line_len++] = c;
        }
    }
}

void fb_show_boot_log(void) {
    char log_buf[4096];
    size_t log_size;
    size_t log_offset;
    size_t copied;
    int text_x;
    int text_y;
    int max_chars;
    int max_lines;
    int panel_x;
    int panel_y;
    int panel_w;
    int panel_h;

    if (!framebuffer.initialized)
        return;

    fb_clear(0x0B1020);
    fb_fill_rect(24, 24, (int)framebuffer.width - 48,
                 (int)framebuffer.height - 48, 0x111827);
    fb_fill_rect(24, 24, (int)framebuffer.width - 48, 2, 0x4F46E5);
    fb_fill_rect(24, 24, 2, (int)framebuffer.height - 48, 0x4F46E5);
    fb_fill_rect(24, (int)framebuffer.height - 26, (int)framebuffer.width - 48,
                 2, 0x4F46E5);

    fb_draw_string(40, 40, "OS8 BOOT LOG", 0xFFFFFF, 0x111827);
    fb_draw_string(40, 58, "Visible startup log, no splash screen", 0x93C5FD,
                   0x111827);

    panel_x = 40;
    panel_y = 88;
    panel_w = (int)framebuffer.width - 80;
    panel_h = (int)framebuffer.height - 128;
    if (panel_w < 80 || panel_h < 80)
        return;

    fb_fill_rect(panel_x, panel_y, panel_w, panel_h, 0x0F172A);
    fb_fill_rect(panel_x, panel_y, panel_w, 1, 0x334155);
    fb_fill_rect(panel_x, panel_y, 1, panel_h, 0x334155);

    log_size = printk_log_size();
    log_offset = log_size > sizeof(log_buf) - 1
                     ? log_size - (sizeof(log_buf) - 1)
                     : 0;
    copied = printk_log_read(log_buf, log_offset, sizeof(log_buf) - 1);
    log_buf[copied] = '\0';

    text_x = panel_x + 16;
    text_y = panel_y + 16;
    max_chars = (panel_w - 32) / FONT_WIDTH;
    max_lines = (panel_h - 32) / (FONT_HEIGHT + 2);
    if (max_chars < 8 || max_lines < 4)
        return;

    fb_draw_wrapped_text(text_x, text_y, max_chars, log_buf, 0xC7D2FE,
                         0x0F172A, max_lines);
}

void fb_show_splash(void)
{
    fb_show_boot_log();
}

void fb_show_x86_64_bringup_screen(void)
{
    fb_show_boot_log();
}

/* ===================================================================== */
/* Initialization */
/* ===================================================================== */

static uint32_t virtio_read32(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static void virtio_write32(volatile uint8_t *base, uint32_t offset, uint32_t val)
{
    *(volatile uint32_t *)(base + offset) = val;
}

int fb_init(void)
{
    printk(KERN_INFO "FB: Initializing framebuffer\n");

#ifdef ARCH_X86_64
    extern int limine_get_framebuffer(uint32_t **buffer, uint32_t *width,
                                      uint32_t *height, uint32_t *pitch);
    uint32_t *limine_buffer = NULL;
    uint32_t limine_width = 0;
    uint32_t limine_height = 0;
    uint32_t limine_pitch = 0;

    if (limine_get_framebuffer(&limine_buffer, &limine_width, &limine_height,
                               &limine_pitch) == 0 &&
        limine_buffer && limine_width && limine_height) {
        framebuffer.buffer = limine_buffer;
        framebuffer.width = limine_width;
        framebuffer.height = limine_height;
        framebuffer.pitch = limine_pitch ? limine_pitch : (limine_width * 4);
        framebuffer.initialized = true;

        printk(KERN_INFO "FB: Using Limine framebuffer %ux%u at 0x%lx\n",
               framebuffer.width, framebuffer.height,
               (unsigned long)framebuffer.buffer);
        fb_show_boot_log();
        return 0;
    }

    printk(KERN_WARNING "FB: Limine framebuffer unavailable, using fallback buffer\n");
#endif

    /* Use static buffer in BSS */
    static uint32_t static_framebuffer[1024 * 768] __attribute__((aligned(4096)));
    
    framebuffer.buffer = static_framebuffer;
    framebuffer.width = SIMPLE_FB_WIDTH;
    framebuffer.height = SIMPLE_FB_HEIGHT;
    framebuffer.pitch = SIMPLE_FB_WIDTH * 4;
    framebuffer.initialized = true;
    
    printk(KERN_INFO "FB: Framebuffer %ux%u at 0x%lx\n",
           framebuffer.width, framebuffer.height, (unsigned long)framebuffer.buffer);
    
    /* Clear to dark blue */
    fb_clear(0x1E1E2E);
    /* Configure QEMU ramfb to display our framebuffer */
#ifndef ARCH_X86_64
    extern int ramfb_init(uint32_t *framebuffer, uint32_t width, uint32_t height);
    if (ramfb_init(framebuffer.buffer, framebuffer.width, framebuffer.height) == 0) {
        printk(KERN_INFO "FB: QEMU ramfb display connected\n");
    } else {
        printk(KERN_WARNING "FB: ramfb not available, display may not work\n");
    }
#endif
    
    fb_show_boot_log();
    
    printk(KERN_INFO "FB: Initialization complete\n");
    
    return 0;
}

/* Get framebuffer info */
void fb_get_info(uint32_t **buffer, uint32_t *width, uint32_t *height)
{
    if (buffer) *buffer = framebuffer.buffer;
    if (width) *width = framebuffer.width;
    if (height) *height = framebuffer.height;
}

uint32_t fb_get_pitch(void)
{
    return framebuffer.pitch;
}

void fb_set_info(uint32_t *buffer, uint32_t width, uint32_t height,
                 uint32_t pitch)
{
    framebuffer.buffer = buffer;
    framebuffer.width = width;
    framebuffer.height = height;
    framebuffer.pitch = pitch ? pitch : (width * 4);
    framebuffer.initialized = (buffer != NULL && width != 0 && height != 0);
}
