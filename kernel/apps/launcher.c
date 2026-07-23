/*
 * OS8 Application Launcher
 * 
 * Provides kernel API and launches embedded applications
 */

#include "apps/kapi.h"
#include "arch/arch.h"
#include "drivers/storage.h"
#include "fs/vfs.h"
#include "fs/vfs_compat.h"
#include "printk.h"
#include "sandbox/sandbox.h"
#include "mm/kmalloc.h"
#include "core/process.h"
#include "gui/gui.h"
#include "gui/font.h"

/* Display structure from window.c - MUST match exactly! */
struct display {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t *framebuffer;
    uint32_t *backbuffer;
};

/* External references */
extern struct display *gui_get_display(void);
extern void mouse_get_position(int *x, int *y);
extern int mouse_get_buttons(void);
extern int uart_getc_nonblock(void);
extern void uart_putc(char c);
#if CONFIG_INSTALLER_APP
extern int installer_app_main(kapi_t *api, int argc, char **argv);
#endif

/* Timer ticks counter */
static volatile uint64_t uptime_ticks = 0;
static uint64_t app_frame_last_ms = 0;

#define APP_FRAME_RATE_HZ 60U
#define APP_FRAME_INTERVAL_MS (1000U / APP_FRAME_RATE_HZ)

/* Global kernel API instance */
static kapi_t global_kapi;

static void kapi_sync_display_state(kapi_t *api) {
    struct display *d = gui_get_display();

    if (!api)
        return;

    api->fb_base = d ? d->framebuffer : NULL;
    api->fb_width = d ? d->width : 0;
    api->fb_height = d ? d->height : 0;
}

/* ===================================================================== */
/* KAPI Implementation Functions */
/* ===================================================================== */

static void kapi_putc(char c) {
    uart_putc(c);
}

static void kapi_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

/* Input Ring Buffer */
#define KAPI_INPUT_BUF_SIZE 128
static volatile int k_input_buf[KAPI_INPUT_BUF_SIZE];
static volatile int k_input_r = 0;
static volatile int k_input_w = 0;

void kapi_sys_key_event(int key) {
    int next = (k_input_w + 1) % KAPI_INPUT_BUF_SIZE;
    if (next != k_input_r) {
        k_input_buf[k_input_w] = key;
        k_input_w = next;
    }
}

static int kapi_getc(void) {
    /* Check buffer first */
    if (k_input_r != k_input_w) {
        int key = k_input_buf[k_input_r];
        k_input_r = (k_input_r + 1) % KAPI_INPUT_BUF_SIZE;
        return key;
    }
    /* Fallback to UART */
    return uart_getc_nonblock();
}

static int kapi_has_key(void) {
    if (k_input_r != k_input_w)
        return 1;
    return uart_getc_nonblock() >= 0 ? 1 : 0;
}

static void app_frame_wait(void) {
    uint64_t now;
    uint64_t deadline;

    now = arch_timer_get_ms();
    if (app_frame_last_ms == 0) {
        app_frame_last_ms = now;
        return;
    }

    deadline = app_frame_last_ms + APP_FRAME_INTERVAL_MS;
    while (now < deadline) {
        extern void process_yield(void);
        process_yield();
        now = arch_timer_get_ms();
    }

    app_frame_last_ms = now;
}

static void kapi_clear(void) {
    /* Clear framebuffer to black */
    struct display *d = gui_get_display();
    if (d && d->framebuffer) {
        for (uint32_t i = 0; i < d->width * d->height; i++) {
            d->framebuffer[i] = 0;
        }
    }
}

static void kapi_fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    struct display *d = gui_get_display();

    if (!d || !d->framebuffer)
        return;
    if (x >= d->width || y >= d->height)
        return;
    d->framebuffer[y * d->width + x] = color;
}

static void kapi_fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                              uint32_t color) {
    gui_draw_rect((int)x, (int)y, (int)w, (int)h, color);
}

static void kapi_fb_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg,
                              uint32_t bg) {
    gui_draw_char((int)x, (int)y, c, fg, bg);
}

static void kapi_fb_draw_string(uint32_t x, uint32_t y, const char *s,
                                uint32_t fg, uint32_t bg) {
    gui_draw_string((int)x, (int)y, s, fg, bg);
}

static int kapi_get_key(void) {
    return uart_getc_nonblock();
}

static void kapi_mouse_get_pos(int *x, int *y) {
#ifdef ARCH_X86_64
    if (x) *x = 0;
    if (y) *y = 0;
#else
    mouse_get_position(x, y);
#endif
}

static uint8_t kapi_mouse_get_buttons(void) {
#ifdef ARCH_X86_64
    return 0;
#else
    return (uint8_t)mouse_get_buttons();
#endif
}

static int last_mouse_x = 0, last_mouse_y = 0;
static void kapi_mouse_get_delta(int *dx, int *dy) {
#ifdef ARCH_X86_64
    if (dx) *dx = 0;
    if (dy) *dy = 0;
#else
    int x, y;
    mouse_get_position(&x, &y);
    *dx = x - last_mouse_x;
    *dy = y - last_mouse_y;
    last_mouse_x = x;
    last_mouse_y = y;
#endif
}

/* Sound implementation - forwards to Intel HDA driver */
#include "drivers/intel_hda.h"

extern int intel_hda_play_pcm(const void *data, uint32_t samples, uint8_t channels, uint32_t sample_rate);

static int kapi_sound_play_wav(const void *data, uint32_t size) {
    /* TODO: Parse WAV header */
    return 0; 
}

static void kapi_sound_stop(void) {
    /* TODO: Stop playback */
}

static int kapi_sound_is_playing(void) {
    /* TODO: Check driver */
    return 0;
}

static int kapi_sound_play_pcm(const void *data, uint32_t samples, uint8_t channels, uint32_t sample_rate) {
    /* Forward to driver */
    /* Check if HDA is active? */
    return intel_hda_play_pcm(data, samples, channels, sample_rate);
}

static int kapi_sound_play_pcm_async(const void *data, uint32_t samples, uint8_t channels, uint32_t sample_rate) {
    /* Same as sync for now, just non-blocking if possible */
    return kapi_sound_play_pcm(data, samples, channels, sample_rate);
}

static void kapi_sound_pause(void) {}
static int kapi_sound_resume(void) { return 0; }
static int kapi_sound_is_paused(void) { return 0; }

static unsigned long kapi_get_uptime_ticks(void) {
    /* Read timer counter - architecture specific */
#ifdef ARCH_ARM64
    uint64_t cnt, freq;
    asm volatile("mrs %0, cntvct_el0" : "=r"(cnt));
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return (unsigned long)((cnt * 100ULL) / freq);
#elif defined(ARCH_X86_64) || defined(ARCH_X86)
    /* Use arch timer abstraction */
    extern uint64_t arch_timer_get_ticks(void);
    extern uint64_t arch_timer_get_frequency(void);
    uint64_t cnt = arch_timer_get_ticks();
    uint64_t freq = arch_timer_get_frequency();
    return (unsigned long)((cnt * 100ULL) / freq);
#else
    return 0;
#endif
}

static void kapi_sleep_ms(uint32_t ms) {
    uint64_t deadline;
    uint32_t delay_ms = ms;

    if (delay_ms < APP_FRAME_INTERVAL_MS)
        delay_ms = APP_FRAME_INTERVAL_MS;

    deadline = arch_timer_get_ms() + delay_ms;
    while (arch_timer_get_ms() < deadline) {
        extern void process_yield(void);
        process_yield();
    }
}

static void *kapi_malloc(size_t size) {
    return kmalloc(size);
}

static void kapi_free(void *ptr) {
    kfree(ptr);
}

/* Get file size from vfs_node_t */
static int get_vfs_file_size(void *node) {
    vfs_node_t *n = (vfs_node_t *)node;

    if (!n)
        return 0;
    return (int)n->size;
}

/* File I/O implemented with VFS */
static void *kapi_open(const char *path) {
    vfs_node_t *file;

    if (!path)
        return NULL;

    file = vfs_open_handle(path);
    if (file) {
        printk(KERN_INFO "[KAPI] open: %s -> found\\n", path);
        return file;
    }

    /* Try without leading slash as fallback */
    if (path[0] == '/') {
        file = vfs_open_handle(path + 1);
        if (file)
            return file;
    }

    printk(KERN_WARNING "[KAPI] open: %s -> NOT FOUND\\n", path);
    return NULL;
}

static void kapi_close(void *handle) {
    vfs_close_handle((vfs_node_t *)handle);
}

static int kapi_read(void *handle, char *buf, size_t count, size_t offset) {
    if (!handle || !buf)
        return -1;
    return vfs_read_compat((vfs_node_t *)handle, buf, (unsigned int)count,
                           (unsigned int)offset);
}

static int kapi_write(void *handle, const char *buf, size_t count) {
    if (!handle || !buf)
        return -1;
    return vfs_write_compat((vfs_node_t *)handle, buf, count);
}

static int kapi_file_size(void *handle) {
    return get_vfs_file_size(handle);
}

static int kapi_is_dir(void *handle) {
    return vfs_is_dir((vfs_node_t *)handle);
}

static void *kapi_create(const char *path) {
    return vfs_create_compat(path);
}

static void *kapi_mkdir(const char *path) {
    return vfs_mkdir_compat(path);
}

static int kapi_delete(const char *path) {
    return vfs_delete(path);
}

static int kapi_delete_dir(const char *path) {
    return vfs_delete_dir(path);
}

static int kapi_delete_recursive(const char *path) {
    return vfs_delete_recursive(path);
}

static int kapi_rename(const char *old, const char *new) {
    return vfs_rename_compat(old, new);
}

static int kapi_readdir(void *dir, int index, char *name, size_t name_size,
                        uint8_t *type) {
    return vfs_readdir_compat((vfs_node_t *)dir, index, name, name_size, type);
}

static int kapi_set_cwd(const char *path) {
    return vfs_set_cwd(path);
}

static int kapi_get_cwd(char *buf, size_t size) {
    return vfs_get_cwd_path(buf, size);
}

static int kapi_save_file(const char *path, const void *data, size_t size,
                          uint32_t flags) {
    uint32_t vfs_flags = 0;

    if (flags & OS8_SAVE_CREATE_PARENTS)
        vfs_flags |= VFS_SAVE_CREATE_PARENTS;
    if (flags & OS8_SAVE_APPEND)
        vfs_flags |= VFS_SAVE_APPEND;
    return vfs_save_file(path, data, size, vfs_flags);
}

static int kapi_disk_count(void) {
    return storage_get_disk_count();
}

static int kapi_disk_info(int disk_index, os8_disk_info_t *info) {
    if (!info)
        return -1;
    if (disk_index < 0 || disk_index >= storage_get_disk_count())
        return -1;

    info->location[0] = '\0';
    storage_get_disk_location(disk_index, info->location,
                              (int)sizeof(info->location));
    info->capacity_mib = storage_get_disk_capacity_mib(disk_index);
    info->removable = storage_disk_is_removable(disk_index);
    info->writable = storage_disk_supports_partition_writes(disk_index);
    return 0;
}

static int kapi_partition_count(int disk_index) {
    return storage_get_partition_count(disk_index);
}

static int kapi_partition_info(int disk_index, int partition_index,
                               os8_partition_info_t *info) {
    storage_partition_kind_t kind;
    storage_filesystem_kind_t filesystem;
    uint32_t start_lba;
    uint32_t sector_count;

    if (!info)
        return -1;
    if (storage_get_partition_info(disk_index, partition_index, &kind,
                                   info->label, (int)sizeof(info->label),
                                   &start_lba, &sector_count) != 0)
        return -1;

    info->kind = (uint32_t)kind;
    info->start_lba = start_lba;
    info->sector_count = sector_count;
    info->size_mib = sector_count / 2048U;
    if (info->size_mib == 0 && sector_count > 0)
        info->size_mib = 1;
    info->filesystem = OS8_FS_UNKNOWN;
    info->filesystem_label[0] = '\0';
    if (storage_get_partition_filesystem_info(disk_index, partition_index,
                                              &filesystem,
                                              info->filesystem_label,
                                              (int)sizeof(info->filesystem_label)) ==
        0) {
        info->filesystem = (uint32_t)filesystem;
    }
    return 0;
}

static int kapi_partition_create(int disk_index, uint32_t kind,
                                 uint32_t size_mib) {
    if (kind < OS8_PARTITION_EFI || kind > OS8_PARTITION_SWAP)
        return -1;
    return storage_create_partition(disk_index, (storage_partition_kind_t)kind,
                                    size_mib);
}

static int kapi_partition_update(int disk_index, int partition_index,
                                 uint32_t kind, uint32_t size_mib) {
    if (kind < OS8_PARTITION_EFI || kind > OS8_PARTITION_SWAP)
        return -1;
    return storage_update_partition(disk_index, partition_index,
                                    (storage_partition_kind_t)kind, size_mib);
}

static int kapi_partition_delete(int disk_index, int partition_index) {
    return storage_delete_partition(disk_index, partition_index);
}

static int kapi_partition_format(int disk_index, int partition_index,
                                 uint32_t filesystem) {
    if (filesystem < OS8_FS_FAT32 || filesystem > OS8_FS_SWAP)
        return -1;
    return storage_format_partition(disk_index, partition_index,
                                    (storage_filesystem_kind_t)filesystem);
}

static int kapi_installer_mode(void) {
    return gui_installer_mode();
}

static int kapi_installer_disk_label(int slot, char *buf, size_t size) {
    return gui_installer_disk_label(slot, buf, size);
}

static int kapi_installer_select_disk(int slot) {
    return gui_installer_select_disk(slot);
}

static int kapi_installer_select_disk_index(int disk_index) {
    return gui_installer_select_disk_index(disk_index);
}

static int kapi_installer_reboot(void) {
    return gui_installer_reboot_now();
}

static int kapi_installer_target_root(char *buf, size_t size) {
    return gui_installer_target_root(buf, size);
}

static int kapi_installer_target_physical_root(char *buf, size_t size) {
    return gui_installer_target_physical_root(buf, size);
}

static int kapi_installer_system_image_root(char *buf, size_t size) {
    return gui_installer_system_image_root(buf, size);
}

static int kapi_installer_boot_payload_root(char *buf, size_t size) {
    return gui_installer_boot_payload_root(buf, size);
}

static int kapi_installer_payload_is_archive(const char *path) {
    return gui_installer_payload_is_archive(path);
}

static int kapi_installer_has_raw_disk_image(void) {
    return gui_installer_has_raw_disk_image();
}

static int kapi_installer_apply_system_payload(void) {
    return gui_installer_apply_system_payload();
}

static int kapi_installer_apply_raw_disk_image(void) {
    return gui_installer_apply_raw_disk_image();
}

static void kapi_exit(int status) {
    printk(KERN_INFO "[APP] Exit with status %d\n", status);
    /* Return to kernel - in real userspace, this would terminate the process */
}

/* Run an app and wait for completion */
static int kapi_exec(const char *path) {
    printk(KERN_INFO "[KAPI] exec: %s\n", path);
    return app_run(path, 0, 0);
}

/* Run an app in background */
static int kapi_spawn(const char *path) {
    printk(KERN_INFO "[KAPI] spawn: %s\n", path);
    /* For now, same as exec - no true multitasking yet */
    return app_run(path, 0, 0);
}

/* Yield CPU to other tasks */
static void kapi_yield(void) {
    app_frame_wait();
    extern void process_yield(void);
    process_yield();
}

static void kapi_uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

/* ===================================================================== */
/* Initialize Kernel API */
/* ===================================================================== */

/* Unsupported capability handlers */
static void unsupported_void(void) {}
static void unsupported_void_int(int x) { (void)x; }
static int unsupported_int(void) { return 0; }
static int unsupported_int_int(int x) { (void)x; return 0; }
static uint32_t unsupported_uint32(void) { return 0; }
static void unsupported_set_color(uint32_t fg, uint32_t bg) { (void)fg; (void)bg; }
static void unsupported_set_cursor(int r, int c) { (void)r; (void)c; }
static void unsupported_clear_region(int r, int c, int w, int h) { (void)r; (void)c; (void)w; (void)h; }
static int unsupported_is_dir(void *n) { (void)n; return 0; }
static void *unsupported_ptr_path(const char *p) { (void)p; return NULL; }
static int unsupported_delete_path(const char *p) { (void)p; return -1; }
static int unsupported_readdir(void *d, int i, char *n, size_t ns, uint8_t *t) { (void)d; (void)i; (void)n; (void)ns; (void)t; return -1; }
static int unsupported_set_cwd(const char *p) { (void)p; return -1; }
static int unsupported_get_cwd(char *b, size_t s) { (void)b; (void)s; return -1; }
static int unsupported_exec_args(const char *p, int a, char **v) { (void)p; (void)a; (void)v; return -1; }
static int unsupported_spawn_args(const char *p, int a, char **v) { (void)p; (void)a; (void)v; return -1; }
static int unsupported_console_size(void) { return 25; }
static void unsupported_fb_pixel(uint32_t x, uint32_t y, uint32_t c) { (void)x; (void)y; (void)c; }
static void unsupported_fb_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
static void unsupported_fb_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) { (void)x; (void)y; (void)c; (void)fg; (void)bg; }
static void unsupported_fb_string(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg) { (void)x; (void)y; (void)s; (void)fg; (void)bg; }
static void unsupported_mouse_poll(void) {}
static void unsupported_mouse_set(int x, int y) { (void)x; (void)y; }
static int unsupported_win_create(int x, int y, int w, int h, const char *t) { (void)x; (void)y; (void)w; (void)h; (void)t; return -1; }
static void unsupported_win_destroy(int w) { (void)w; }
static uint32_t *unsupported_win_buf(int w, int *pw, int *ph) { (void)w; (void)pw; (void)ph; return NULL; }
static int unsupported_win_poll(int w, int *t, int *d1, int *d2, int *d3) { (void)w; (void)t; (void)d1; (void)d2; (void)d3; return 0; }
static void unsupported_win_inv(int w) { (void)w; }
static void unsupported_win_title(int w, const char *t) { (void)w; (void)t; }
static size_t unsupported_mem_info(void) { return 0; }
static uint32_t unsupported_timestamp(void) { return 0; }
static void unsupported_datetime(int *y, int *m, int *d, int *h, int *mi, int *s, int *w) { (void)y; (void)m; (void)d; (void)h; (void)mi; (void)s; (void)w; }
static void unsupported_wfi(void) { 
#ifdef ARCH_ARM64
    asm volatile("wfi");
#elif defined(ARCH_X86_64) || defined(ARCH_X86)
    asm volatile("hlt");
#endif
}
extern int usb_device_count(void);
extern int usb_device_info(int idx, uint16_t *vid, uint16_t *pid, char *name,
                           int name_len);
static int unsupported_sound(const void *d, uint32_t s) { (void)d; (void)s; return -1; }
static int unsupported_sound_pcm(const void *d, uint32_t s, uint8_t c, uint32_t r) { (void)d; (void)s; (void)c; (void)r; return -1; }
static int unsupported_proc_info(int i, char *n, int ns, int *st) { (void)i; (void)n; (void)ns; (void)st; return 0; }
static uint64_t unsupported_heap_addr(void) { return 0; }
static int unsupported_net_ping(uint32_t ip, uint16_t seq, uint32_t to) { (void)ip; (void)seq; (void)to; return -1; }
static uint32_t unsupported_net_ip(void) { return 0; }
static void unsupported_net_mac(uint8_t *m) { (void)m; }
static uint32_t unsupported_dns(const char *h) { (void)h; return 0; }
static int unsupported_tcp_connect(uint32_t ip, uint16_t port) { (void)ip; (void)port; return -1; }
static int unsupported_tcp_send(int s, const void *d, uint32_t l) { (void)s; (void)d; (void)l; return -1; }
static int unsupported_tcp_recv(int s, void *b, uint32_t m) { (void)s; (void)b; (void)m; return -1; }
static void unsupported_tcp_close(int s) { (void)s; }
static int unsupported_tls_connect(uint32_t ip, uint16_t port, const char *h) { (void)ip; (void)port; (void)h; return -1; }
static void *unsupported_ttf_glyph(int c, int s, int st) { (void)c; (void)s; (void)st; return NULL; }
static int unsupported_ttf_adv(int c, int s) { (void)c; (void)s; return 0; }
static int unsupported_ttf_kern(int c1, int c2, int s) { (void)c1; (void)c2; (void)s; return 0; }
static void unsupported_ttf_metrics(int s, int *a, int *d, int *lg) { (void)s; (void)a; (void)d; (void)lg; }
static int unsupported_usb_info(int i, uint16_t *v, uint16_t *p, char *n, int nl) { (void)i; (void)v; (void)p; (void)n; (void)nl; return 0; }
static size_t kapi_klog_read(char *b, size_t o, size_t s) { return printk_log_read(b, o, s); }
static size_t kapi_klog_size(void) { return printk_log_size(); }
static uint32_t *unsupported_backbuf(void) { return NULL; }
static int unsupported_dma_copy(void *d, const void *s, uint32_t l) { (void)d; (void)s; (void)l; return -1; }
static int unsupported_dma_2d(void *d, uint32_t dp, const void *s, uint32_t sp, uint32_t w, uint32_t h) { (void)d; (void)dp; (void)s; (void)sp; (void)w; (void)h; return -1; }
static int unsupported_dma_fb(uint32_t *d, const uint32_t *s, uint32_t w, uint32_t h) { (void)d; (void)s; (void)w; (void)h; return -1; }
static int unsupported_dma_fill(void *d, uint32_t v, uint32_t l) { (void)d; (void)v; (void)l; return -1; }
static const char *unsupported_cpu_name(void) { return "ARM Cortex-A72"; }

void kapi_init(kapi_t *api) {
    /* Zero entire struct first */
    char *p = (char *)api;
    for (size_t i = 0; i < sizeof(kapi_t); i++) p[i] = 0;
    
    api->version = 2;

    /* Console I/O - in order per vibe.h */
    api->putc = kapi_putc;
    api->puts = kapi_puts;
    api->uart_puts = kapi_uart_puts;
    api->getc = kapi_getc;
    api->set_color = unsupported_set_color;
    api->clear = kapi_clear;
    api->set_cursor = unsupported_set_cursor;
    api->set_cursor_enabled = unsupported_void_int;
    api->print_int = NULL;  /* app uses printf instead */
    api->print_hex = NULL;
    api->clear_to_eol = unsupported_void;
    api->clear_region = unsupported_clear_region;

    /* Keyboard */
    api->has_key = kapi_has_key;

    /* Memory */
    api->malloc = kapi_malloc;
    api->free = kapi_free;

    /* Filesystem */
    api->open = kapi_open;
    api->close = kapi_close;
    api->read = kapi_read;
    api->write = kapi_write;
    api->is_dir = kapi_is_dir;
    api->file_size = kapi_file_size;
    api->create = kapi_create;
    api->mkdir_fn = kapi_mkdir;
    api->delete = kapi_delete;
    api->delete_dir = kapi_delete_dir;
    api->delete_recursive = kapi_delete_recursive;
    api->rename = kapi_rename;
    api->readdir = kapi_readdir;
    api->set_cwd = kapi_set_cwd;
    api->get_cwd = kapi_get_cwd;

    /* Process */
    api->exit = kapi_exit;
    api->exec = kapi_exec;
    api->exec_args = unsupported_exec_args;
    api->yield = kapi_yield;
    api->spawn = kapi_spawn;
    api->spawn_args = unsupported_spawn_args;

    /* Console info */
    api->console_rows = unsupported_console_size;
    api->console_cols = unsupported_console_size;

    /* Framebuffer */
    kapi_sync_display_state(api);
    api->fb_put_pixel = kapi_fb_put_pixel;
    api->fb_fill_rect = kapi_fb_fill_rect;
    api->fb_draw_char = kapi_fb_draw_char;
    api->fb_draw_string = kapi_fb_draw_string;

    /* Font */
    api->font_data = NULL;

    /* Mouse */
    api->mouse_get_pos = kapi_mouse_get_pos;
    api->mouse_get_buttons = kapi_mouse_get_buttons;
    api->mouse_poll = unsupported_mouse_poll;
    api->mouse_set_pos = unsupported_mouse_set;
    api->mouse_get_delta = kapi_mouse_get_delta;

    /* Windows */
    api->window_create = unsupported_win_create;
    api->window_destroy = unsupported_win_destroy;
    api->window_get_buffer = unsupported_win_buf;
    api->window_poll_event = unsupported_win_poll;
    api->window_invalidate = unsupported_win_inv;
    api->window_set_title = unsupported_win_title;

    /* Stdio hooks - NULL means use console */
    api->stdio_putc = NULL;
    api->stdio_puts = NULL;
    api->stdio_getc = NULL;
    api->stdio_has_key = NULL;

    extern void input_poll(void);
    api->input_poll = input_poll;

    /* System info */
    api->get_uptime_ticks = kapi_get_uptime_ticks;
    api->get_mem_used = unsupported_mem_info;
    api->get_mem_free = unsupported_mem_info;

    /* RTC */
    api->get_timestamp = unsupported_timestamp;
    api->get_datetime = unsupported_datetime;

    /* Power/timing */
    api->wfi = unsupported_wfi;
    api->sleep_ms = kapi_sleep_ms;

    /* Sound */
    /* Sound */
    api->sound_play_wav = kapi_sound_play_wav;
    api->sound_stop = kapi_sound_stop;
    api->sound_is_playing = kapi_sound_is_playing;
    api->sound_play_pcm = kapi_sound_play_pcm;
    api->sound_play_pcm_async = kapi_sound_play_pcm_async;
    api->sound_pause = kapi_sound_pause;
    api->sound_resume = kapi_sound_resume;
    api->sound_is_paused = kapi_sound_is_paused;

    /* Process info */
    api->get_process_count = unsupported_int;
    api->get_process_info = unsupported_proc_info;

    /* Disk info */
    api->get_disk_total = unsupported_int;
    api->get_disk_free = unsupported_int;

    /* RAM info */
    api->get_ram_total = unsupported_mem_info;

    /* Debug memory */
    api->get_heap_start = unsupported_heap_addr;
    api->get_heap_end = unsupported_heap_addr;
    api->get_stack_ptr = unsupported_heap_addr;
    api->get_alloc_count = unsupported_int;

    /* Network */
    api->net_ping = unsupported_net_ping;
    api->net_poll = unsupported_void;
    api->net_get_ip = unsupported_net_ip;
    api->net_get_mac = unsupported_net_mac;
    api->dns_resolve = unsupported_dns;

    /* TCP */
    api->tcp_connect = unsupported_tcp_connect;
    api->tcp_send = unsupported_tcp_send;
    api->tcp_recv = unsupported_tcp_recv;
    api->tcp_close = unsupported_tcp_close;
    api->tcp_is_connected = unsupported_int_int;

    /* TLS */
    api->tls_connect = unsupported_tls_connect;
    api->tls_send = unsupported_tcp_send;
    api->tls_recv = unsupported_tcp_recv;
    api->tls_close = unsupported_tcp_close;
    api->tls_is_connected = unsupported_int_int;

    /* TTF */
    api->ttf_get_glyph = unsupported_ttf_glyph;
    api->ttf_get_advance = unsupported_ttf_adv;
    api->ttf_get_kerning = unsupported_ttf_kern;
    api->ttf_get_metrics = unsupported_ttf_metrics;
    api->ttf_is_ready = unsupported_int;

    /* LED */
    api->led_on = unsupported_void;
    api->led_off = unsupported_void;
    api->led_toggle = unsupported_void;
    api->led_status = unsupported_int;

    /* Process control */
    api->kill_process = unsupported_int_int;

    /* CPU info */
    api->get_cpu_name = unsupported_cpu_name;
    api->get_cpu_freq_mhz = unsupported_uint32;
    api->get_cpu_cores = unsupported_int;

    /* USB */
    api->usb_device_count = usb_device_count;
    api->usb_device_info = usb_device_info;

    /* Kernel log */
    api->klog_read = kapi_klog_read;
    api->klog_size = kapi_klog_size;

    /* HW double buffer */
    api->fb_has_hw_double_buffer = unsupported_int;
    api->fb_flip = unsupported_int_int;
    api->fb_get_backbuffer = unsupported_backbuf;

    /* DMA */
    api->dma_available = unsupported_int;
    api->dma_copy = unsupported_dma_copy;
    api->dma_copy_2d = unsupported_dma_2d;
    api->dma_fb_copy = unsupported_dma_fb;
    api->dma_fill = unsupported_dma_fill;

    /* Persistent file and disk APIs */
    api->save_file = kapi_save_file;
    api->disk_count = kapi_disk_count;
    api->disk_info = kapi_disk_info;
    api->partition_count = kapi_partition_count;
    api->partition_info = kapi_partition_info;
    api->partition_create = kapi_partition_create;
    api->partition_update = kapi_partition_update;
    api->partition_delete = kapi_partition_delete;
    api->partition_format = kapi_partition_format;
    api->installer_mode = kapi_installer_mode;
    api->installer_disk_label = kapi_installer_disk_label;
    api->installer_select_disk = kapi_installer_select_disk;
    api->installer_select_disk_index = kapi_installer_select_disk_index;
    api->installer_reboot = kapi_installer_reboot;
    api->installer_target_root = kapi_installer_target_root;
    api->installer_target_physical_root = kapi_installer_target_physical_root;
    api->installer_system_image_root = kapi_installer_system_image_root;
    api->installer_boot_payload_root = kapi_installer_boot_payload_root;
    api->installer_payload_is_archive = kapi_installer_payload_is_archive;
    api->installer_has_raw_disk_image = kapi_installer_has_raw_disk_image;
    api->installer_apply_system_payload = kapi_installer_apply_system_payload;
    api->installer_apply_raw_disk_image = kapi_installer_apply_raw_disk_image;

    printk(KERN_INFO "[KAPI] Kernel API initialized (fb=%dx%d)\\n", api->fb_width, api->fb_height);
    printk(KERN_INFO "[KAPI] fb_base = 0x%lx\\n", (unsigned long)(uintptr_t)api->fb_base);
}

/* ===================================================================== */
/* Application Registry - Embedded Apps */
/* ===================================================================== */

/* Tick counter for timing */
void kapi_tick(void) {
    uptime_ticks++;
}

void kapi_refresh_display_state(void) {
    kapi_sync_display_state(&global_kapi);
}

/* Get the global kapi */
kapi_t *kapi_get(void) {
    static int initialized = 0;
    if (!initialized) {
        kapi_init(&global_kapi);
        initialized = 1;
    }
    kapi_sync_display_state(&global_kapi);
    return &global_kapi;
}

/* ===================================================================== */
/* Demo Application: Clock */
/* ===================================================================== */
static int clock_app_main(kapi_t *api, int argc, char **argv) {
    (void)argc; (void)argv;
    
    api->puts("\n=== OS8 Clock ===\n");
    
    if (!api->fb_base) {
        api->puts("No framebuffer available\n");
        return -1;
    }
    
    /* Draw clock interface */
    int cx = api->fb_width / 2;
    int cy = api->fb_height / 2;
    int radius = 100;
    
    /* Draw clock face (circle) */
    for (int angle = 0; angle < 360; angle++) {
        /* Simplified circle using fixed-point math */
        int x = cx + (radius * (angle % 90 < 45 ? angle % 45 : 45 - (angle % 45))) / 45;
        int y = cy + (radius * (45 - (angle % 45))) / 45;
        if (y >= 0 && y < (int)api->fb_height && x >= 0 && x < (int)api->fb_width) {
            api->fb_base[y * api->fb_width + x] = 0xFFFFFF;
        }
    }
    
    /* Draw hour markers */
    for (int h = 0; h < 12; h++) {
        int mx = cx + ((h < 6 ? h : 12 - h) * radius / 6);
        int my = cy - (h < 3 || h > 9 ? radius - 10 : (h == 6 ? -radius + 10 : 0));
        if (my >= 0 && my < (int)api->fb_height && mx >= 0 && mx < (int)api->fb_width) {
            api->fb_base[my * api->fb_width + mx] = 0xFFFF00;
        }
    }
    
    /* Draw center dot */
    for (int dy = -3; dy <= 3; dy++) {
        for (int dx = -3; dx <= 3; dx++) {
            int px = cx + dx;
            int py = cy + dy;
            if (py >= 0 && py < (int)api->fb_height && px >= 0 && px < (int)api->fb_width) {
                api->fb_base[py * api->fb_width + px] = 0xFF0000;
            }
        }
    }
    
    /* Draw clock hands based on uptime */
    uint64_t ticks = api->get_uptime_ticks();
    int seconds = (ticks / 100) % 60;
    int minutes = (ticks / 6000) % 60;
    int hours = (ticks / 360000) % 12;
    
    /* Second hand (red, long) */
    for (int i = 0; i < radius - 10; i++) {
        int sx = cx + (i * (seconds % 30 < 15 ? seconds % 30 : 30 - seconds % 30)) / 30;
        int sy = cy - (i * (seconds < 30 ? 1 : -1));
        if (sy >= 0 && sy < (int)api->fb_height && sx >= 0 && sx < (int)api->fb_width) {
            api->fb_base[sy * api->fb_width + sx] = 0xFF0000;
        }
    }
    
    api->puts("Clock drawn! Uptime: ");
    char buf[32];
    int idx = 0;
    buf[idx++] = '0' + (hours / 10);
    buf[idx++] = '0' + (hours % 10);
    buf[idx++] = ':';
    buf[idx++] = '0' + (minutes / 10);
    buf[idx++] = '0' + (minutes % 10);
    buf[idx++] = ':';
    buf[idx++] = '0' + (seconds / 10);
    buf[idx++] = '0' + (seconds % 10);
    buf[idx++] = '\n';
    buf[idx] = '\0';
    api->puts(buf);
    
    return 0;
}

/* ===================================================================== */
/* Demo Application: Snake Game */
/* ===================================================================== */
#define SNAKE_GRID_SIZE 20
#define SNAKE_MAX_LEN 100

static int snake_app_main(kapi_t *api, int argc, char **argv) {
    (void)argc; (void)argv;
    
    api->puts("\n=== OS8 Snake ===\n");
    api->puts("Use mouse to control direction!\n");
    
    if (!api->fb_base) {
        api->puts("No framebuffer available\n");
        return -1;
    }
    
    /* Snake state */
    int snake_x[SNAKE_MAX_LEN];
    int snake_y[SNAKE_MAX_LEN];
    int snake_len = 5;
    int dir_x = 1, dir_y = 0;
    
    /* Initialize snake in center */
    int grid_w = api->fb_width / SNAKE_GRID_SIZE;
    int grid_h = api->fb_height / SNAKE_GRID_SIZE;
    int start_x = grid_w / 2;
    int start_y = grid_h / 2;
    
    for (int i = 0; i < snake_len; i++) {
        snake_x[i] = start_x - i;
        snake_y[i] = start_y;
    }
    
    /* Food position */
    int food_x = start_x + 5;
    int food_y = start_y;
    
    int score = 0;
    int game_over = 0;
    
    /* Game loop - run for limited iterations */
    for (int frame = 0; frame < 200 && !game_over; frame++) {
        /* Clear screen to dark */
        for (uint32_t y = 0; y < api->fb_height; y++) {
            for (uint32_t x = 0; x < api->fb_width; x++) {
                api->fb_base[y * api->fb_width + x] = 0x1E1E2E;
            }
        }
        
        /* Check mouse for direction */
        int mx, my;
        api->mouse_get_pos(&mx, &my);
        int head_px = snake_x[0] * SNAKE_GRID_SIZE;
        int head_py = snake_y[0] * SNAKE_GRID_SIZE;
        
        /* Change direction based on mouse relative to head */
        if (mx > head_px + SNAKE_GRID_SIZE && dir_x == 0) { dir_x = 1; dir_y = 0; }
        else if (mx < head_px - SNAKE_GRID_SIZE && dir_x == 0) { dir_x = -1; dir_y = 0; }
        else if (my > head_py + SNAKE_GRID_SIZE && dir_y == 0) { dir_x = 0; dir_y = 1; }
        else if (my < head_py - SNAKE_GRID_SIZE && dir_y == 0) { dir_x = 0; dir_y = -1; }
        
        /* Move snake */
        int new_x = snake_x[0] + dir_x;
        int new_y = snake_y[0] + dir_y;
        
        /* Wrap around */
        if (new_x < 0) new_x = grid_w - 1;
        if (new_x >= grid_w) new_x = 0;
        if (new_y < 0) new_y = grid_h - 1;
        if (new_y >= grid_h) new_y = 0;
        
        /* Self collision check */
        for (int i = 0; i < snake_len; i++) {
            if (snake_x[i] == new_x && snake_y[i] == new_y) {
                game_over = 1;
                break;
            }
        }
        
        if (!game_over) {
            /* Move body */
            for (int i = snake_len - 1; i > 0; i--) {
                snake_x[i] = snake_x[i-1];
                snake_y[i] = snake_y[i-1];
            }
            snake_x[0] = new_x;
            snake_y[0] = new_y;
            
            /* Check food */
            if (snake_x[0] == food_x && snake_y[0] == food_y) {
                score++;
                if (snake_len < SNAKE_MAX_LEN) snake_len++;
                /* New food position */
                food_x = (food_x + 7) % grid_w;
                food_y = (food_y + 11) % grid_h;
            }
        }
        
        /* Draw snake */
        for (int i = 0; i < snake_len; i++) {
            uint32_t color = (i == 0) ? 0x00FF00 : 0x00AA00;  /* Head brighter */
            int sx = snake_x[i] * SNAKE_GRID_SIZE;
            int sy = snake_y[i] * SNAKE_GRID_SIZE;
            for (int dy = 1; dy < SNAKE_GRID_SIZE - 1; dy++) {
                for (int dx = 1; dx < SNAKE_GRID_SIZE - 1; dx++) {
                    int px = sx + dx;
                    int py = sy + dy;
                    if ((uint32_t)py < api->fb_height && (uint32_t)px < api->fb_width) {
                        api->fb_base[py * api->fb_width + px] = color;
                    }
                }
            }
        }
        
        /* Draw food (red) */
        for (int dy = 2; dy < SNAKE_GRID_SIZE - 2; dy++) {
            for (int dx = 2; dx < SNAKE_GRID_SIZE - 2; dx++) {
                int px = food_x * SNAKE_GRID_SIZE + dx;
                int py = food_y * SNAKE_GRID_SIZE + dy;
                if ((uint32_t)py < api->fb_height && (uint32_t)px < api->fb_width) {
                    api->fb_base[py * api->fb_width + px] = 0xFF0000;
                }
            }
        }
        
        api->sleep_ms(100);  /* Game speed */
    }
    
    /* Show score */
    api->puts("Game Over! Score: ");
    char sbuf[16];
    sbuf[0] = '0' + (score / 10);
    sbuf[1] = '0' + (score % 10);
    sbuf[2] = '\n';
    sbuf[3] = '\0';
    api->puts(sbuf);
    
    return 0;
}

/* ===================================================================== */
/* Demo Application: System Monitor */
/* ===================================================================== */
static int sysmon_app_main(kapi_t *api, int argc, char **argv) {
    (void)argc; (void)argv;
    
    api->puts("\n=== OS8 System Monitor ===\n\n");
    
    /* Display system information */
    api->puts("SYSTEM INFO\n");
    api->puts("-----------\n");
    api->puts("OS:       OS8 v8.0.0\n");
#ifdef ARCH_X86_64
    api->puts("Arch:     x86_64\n");
    api->puts("Platform: Limine / PC-compatible VM\n\n");
#else
    api->puts("Arch:     ARM64 (AArch64)\n");
    api->puts("Platform: QEMU virt\n\n");
#endif
    
    api->puts("DISPLAY\n");
    api->puts("-------\n");
    char buf[64];
    int idx = 0;
    api->puts("Resolution: ");
    idx = 0;
    uint32_t w = api->fb_width;
    buf[idx++] = '0' + (w / 1000) % 10;
    buf[idx++] = '0' + (w / 100) % 10;
    buf[idx++] = '0' + (w / 10) % 10;
    buf[idx++] = '0' + w % 10;
    buf[idx++] = 'x';
    uint32_t h = api->fb_height;
    buf[idx++] = '0' + (h / 1000) % 10;
    buf[idx++] = '0' + (h / 100) % 10;
    buf[idx++] = '0' + (h / 10) % 10;
    buf[idx++] = '0' + h % 10;
    buf[idx++] = '\n';
    buf[idx] = '\0';
    api->puts(buf);
    api->puts("Color:      32-bit ARGB\n");
    api->puts("Compositor: Double-buffered\n\n");
    
    api->puts("MEMORY\n");
    api->puts("------\n");
    api->puts("Heap:     8 MB\n");
    api->puts("PMM:      Buddy allocator\n\n");
    
    api->puts("UPTIME\n");
    api->puts("------\n");
    uint64_t ticks = api->get_uptime_ticks();
    int secs = (int)(ticks / 100);
    int mins = secs / 60;
    int hrs = mins / 60;
    secs %= 60;
    mins %= 60;
    
    idx = 0;
    buf[idx++] = '0' + (hrs / 10);
    buf[idx++] = '0' + (hrs % 10);
    buf[idx++] = ':';
    buf[idx++] = '0' + (mins / 10);
    buf[idx++] = '0' + (mins % 10);
    buf[idx++] = ':';
    buf[idx++] = '0' + (secs / 10);
    buf[idx++] = '0' + (secs % 10);
    buf[idx++] = '\n';
    buf[idx] = '\0';
    api->puts(buf);
    
    /* Draw system bars if framebuffer available */
    if (api->fb_base) {
        int bar_x = 50;
        int bar_y = api->fb_height - 150;
        int bar_w = 200;
        int bar_h = 20;
        
        /* CPU bar (simulated 45%) */
        for (int y = 0; y < bar_h; y++) {
            for (int x = 0; x < bar_w; x++) {
                int px = bar_x + x;
                int py = bar_y + y;
                uint32_t color = (x < bar_w * 45 / 100) ? 0x00FF00 : 0x333333;
                if ((uint32_t)py < api->fb_height && (uint32_t)px < api->fb_width) {
                    api->fb_base[py * api->fb_width + px] = color;
                }
            }
        }
        
        /* Memory bar (simulated 62%) */
        bar_y += 30;
        for (int y = 0; y < bar_h; y++) {
            for (int x = 0; x < bar_w; x++) {
                int px = bar_x + x;
                int py = bar_y + y;
                uint32_t color = (x < bar_w * 62 / 100) ? 0x00AAFF : 0x333333;
                if ((uint32_t)py < api->fb_height && (uint32_t)px < api->fb_width) {
                    api->fb_base[py * api->fb_width + px] = color;
                }
            }
        }
    }
    
    return 0;
}

/* ===================================================================== */
/* Demo Application: Mandelbrot Fractal */
/* ===================================================================== */
static int mandelbrot_app_main(kapi_t *api, int argc, char **argv) {
    (void)argc; (void)argv;
    
    api->puts("\n=== OS8 Mandelbrot Viewer ===\n");
    api->puts("Rendering fractal...\n");
    
    if (!api->fb_base) {
        api->puts("No framebuffer available\n");
        return -1;
    }
    
    int width = api->fb_width;
    int height = api->fb_height;
    int max_iter = 50;
    
    /* Fixed-point math (16.16 format) */
    #define FP_SHIFT 16
    #define FP_ONE (1 << FP_SHIFT)
    #define FP_MUL(a, b) (((long long)(a) * (b)) >> FP_SHIFT)
    
    /* View: x from -2.5 to 1, y from -1 to 1 */
    int x_min = -2 * FP_ONE - FP_ONE / 2;  /* -2.5 */
    int x_max = 1 * FP_ONE;                 /* 1.0 */
    int y_min = -1 * FP_ONE;                /* -1.0 */
    int y_max = 1 * FP_ONE;                 /* 1.0 */
    
    int x_scale = (x_max - x_min) / width;
    int y_scale = (y_max - y_min) / height;
    
    /* Color palette */
    uint32_t palette[16] = {
        0x000764, 0x206BCB, 0xEDFFFF, 0xFFAA00,
        0x000200, 0x0C2161, 0x1E81B0, 0x76E5FC,
        0xFBFECC, 0xED8A0A, 0x9A0200, 0x280000,
        0x2D0070, 0x6600AA, 0x9900FF, 0xCC00FF
    };
    
    for (int py = 0; py < height; py++) {
        int y0 = y_min + py * y_scale;
        
        for (int px = 0; px < width; px++) {
            int x0 = x_min + px * x_scale;
            
            int x = 0, y = 0;
            int iter = 0;
            
            while (iter < max_iter) {
                int x2 = FP_MUL(x, x);
                int y2 = FP_MUL(y, y);
                
                if (x2 + y2 > 4 * FP_ONE) break;
                
                int xtemp = x2 - y2 + x0;
                y = 2 * FP_MUL(x, y) + y0;
                x = xtemp;
                iter++;
            }
            
            uint32_t color;
            if (iter == max_iter) {
                color = 0x000000;  /* Black for set */
            } else {
                color = palette[iter % 16];
            }
            
            api->fb_base[py * width + px] = color;
        }
        
        /* Yield every 50 rows to keep system responsive */
        if (py % 50 == 0) {
            api->yield();
        }
    }
    
    #undef FP_SHIFT
    #undef FP_ONE
    #undef FP_MUL
    
    api->puts("Fractal rendered!\n");
    return 0;
}

/* ===================================================================== */
/* Simple test app */
/* ===================================================================== */
static int test_app_main(kapi_t *api, int argc, char **argv) {
    (void)argc; (void)argv;
    
    api->puts("Hello from test app!\n");
    api->puts("Framebuffer: ");
    
    /* Draw a red rectangle on screen */
    if (api->fb_base) {
        for (int y = 100; y < 200; y++) {
            for (int x = 100; x < 300; x++) {
                api->fb_base[y * api->fb_width + x] = 0xFF0000;  /* Red */
            }
        }
        api->puts("Drew red rectangle!\n");
    }
    
    return 0;
}

/* ===================================================================== */
/* App Registry */
/* ===================================================================== */
typedef struct {
    const char *name;
    app_main_fn main_fn;
} app_entry_t;

static app_entry_t app_registry[] = {
#if CONFIG_INSTALLER_APP
    { "installer",  installer_app_main },
#endif
    { "test",       test_app_main },
    { "clock",      clock_app_main },
    { "snake",      snake_app_main },
    { "sysmon",     sysmon_app_main },
    { "mandelbrot", mandelbrot_app_main },
    { NULL, NULL }
};

#define APP_RING_FENCE_STACK_SIZE (128 * 1024)

typedef struct app_ring_fence_call {
    app_main_fn main_fn;
    kapi_t *api;
    int argc;
    char **argv;
} app_ring_fence_call_t;

static int app_ring_fence_entry(void *arg, void *result, size_t result_size) {
    (void)result;
    (void)result_size;

    app_ring_fence_call_t *call = (app_ring_fence_call_t *)arg;
    if (!call || !call->main_fn || !call->api)
        return -1;

    return call->main_fn(call->api, call->argc, call->argv);
}

/* Run an embedded application by name */
int app_run(const char *name, int argc, char **argv) {
    printk(KERN_INFO "[APP] Running: %s\n", name);
    app_frame_last_ms = 0;
    
    /* Find app in registry */
    for (int i = 0; app_registry[i].name != NULL; i++) {
        /* Simple strcmp */
        const char *a = name;
        const char *b = app_registry[i].name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) {
            sandbox_ctx_t ctx;
            app_ring_fence_call_t call;
            int ret;

            call.main_fn = app_registry[i].main_fn;
            call.api = kapi_get();
            call.argc = argc;
            call.argv = argv;

            if (sandbox_init(&ctx, APP_RING_FENCE_STACK_SIZE, 0) != 0) {
                printk(KERN_ERR "[APP] Ring fence unavailable for %s\n", name);
                return -1;
            }

            ret = sandbox_execute(&ctx, app_ring_fence_entry, &call);
            if (ctx.faulted) {
                printk(KERN_WARNING
                       "[APP] %s faulted inside ring fence; system kept running\n",
                       name);
            } else if (ret < 0) {
                printk(KERN_WARNING "[APP] %s exited with error %d\n", name, ret);
            }

            sandbox_destroy(&ctx);
            return ret;
        }
    }
    
    printk(KERN_WARNING "[APP] App not found: %s\n", name);
    return -1;
}
