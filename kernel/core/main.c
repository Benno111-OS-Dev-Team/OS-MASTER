/*
 * OS Kernel - Main Entry Point
 *
 * This is the C entry point called from boot.S after basic
 * hardware initialization is complete.
 */

#include "apps/embedded_apps.h"
#include "acpi.h"
#include "arch/arch.h"
#include "build_uuid.h"
#include "drivers/storage.h"
#include "drivers/pci.h"
#include "drivers/uart.h"
#include "drivers/vbox_net.h"
#include "drivers/wifi.h"
#include "fs/iso9660.h"
#include "fs/vfs.h"
#include "integrity/integrity.h"
#include "media/media.h"
#include "media/seed_assets.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "printk.h"
#include "sched/sched.h"
#include "string.h"
#include "types.h"
#include "gui/gui.h"
#include "gui/font.h"

/* Kernel version */
#define OS_VERSION_MAJOR 8
#define OS_VERSION_MINOR 0
#define OS_VERSION_PATCH 0

/* External symbols from linker script */
extern char __kernel_start[];
extern char __kernel_end[];
extern char __bss_start[];
extern char __bss_end[];

/* Forward declarations for GUI bring-up helpers used across this file. */
struct window;
struct terminal;

/* Forward declarations */
static void print_banner(void);
static void init_subsystems(void *dtb);
static void start_init_process(void);
static void populate_seed_filesystem(void);
static void populate_installer_payload(void);
static void import_staged_system_image(void);
static int import_boot_media_assets_from(const char *media_root);
static int staged_system_image_exists(void);
void refresh_external_storage_views(void);
static int boot_hdd_disk_index(void);
static void populate_seed_tree_at(const char *prefix);
static void ensure_boot_payload_dirs(const char *prefix);
static int copy_tree_to_prefix(const char *src_root, const char *dst_root,
                               int skip_payload_roots,
                               int skip_boot_root);
static int copy_tree_callback(void *ctx, const char *name, int len,
                              loff_t offset, ino_t ino, unsigned type);
static int build_seed_path(char *dst, size_t dst_size, const char *prefix,
                           const char *path);
static void seed_make_dir(const char *prefix, const char *path);
static void seed_write_text(const char *prefix, const char *path, mode_t mode,
                            const char *content);
static void seed_write_bytes(const char *prefix, const char *path, mode_t mode,
                             const uint8_t *data, size_t size);
static void setup_virtual_mountpoints(void);
static int verify_init_script(void);
static void keyboard_handler(int key);
static void keyboard_gui_handler(int key);
static int cmdline_has_token(const char *cmdline, const char *token);
static uint64_t profile_split_us(uint64_t *cursor_us) {
  uint64_t now_us = gui_monotonic_us();
  uint64_t delta_us = now_us - *cursor_us;

  *cursor_us = now_us;
  return delta_us;
}
static int gui_key_queue_pop(int *key_out);
#ifdef ARCH_X86_64
static void start_x86_64_bringup(void);
#endif

static int cmdline_has_token(const char *cmdline, const char *token) {
  size_t token_len;

  if (!cmdline || !token || !token[0])
    return 0;

  token_len = strlen(token);
  for (const char *p = cmdline; *p; p++) {
    int boundary_before = (p == cmdline || p[-1] == ' ' || p[-1] == '\t');
    int boundary_after;

    if (!boundary_before)
      continue;
    if (strncmp(p, token, token_len) != 0)
      continue;
    boundary_after = (p[token_len] == '\0' || p[token_len] == ' ' ||
                      p[token_len] == '\t');
    if (boundary_after)
      return 1;
  }

  return 0;
}

static void panic_append_char(char *buf, size_t max, size_t *idx, char c) {
  if (!buf || !idx || *idx >= max - 1)
    return;
  buf[(*idx)++] = c;
  buf[*idx] = '\0';
}

static void panic_append_str(char *buf, size_t max, size_t *idx, const char *src) {
  size_t i = 0;
  if (!buf || !idx || !src)
    return;
  while (src[i] && *idx < max - 1) {
    buf[(*idx)++] = src[i++];
  }
  buf[*idx] = '\0';
}

static void panic_append_u64(char *buf, size_t max, size_t *idx, uint64_t value) {
  char tmp[32];
  int ti = 0;

  if (value == 0) {
    panic_append_char(buf, max, idx, '0');
    return;
  }

  while (value > 0 && ti < (int)sizeof(tmp)) {
    tmp[ti++] = (char)('0' + (value % 10));
    value /= 10;
  }
  while (ti > 0)
    panic_append_char(buf, max, idx, tmp[--ti]);
}

static void panic_append_hex(char *buf, size_t max, size_t *idx, uint64_t value,
                             int width) {
  static const char hex[] = "0123456789ABCDEF";

  panic_append_str(buf, max, idx, "0x");
  for (int shift = (width - 1) * 4; shift >= 0; shift -= 4) {
    panic_append_char(buf, max, idx, hex[(value >> shift) & 0xF]);
  }
}

static void panic_make_kv_u64(char *buf, size_t max, const char *key,
                              uint64_t value, const char *suffix) {
  size_t idx = 0;
  if (!buf || max == 0)
    return;
  buf[0] = '\0';
  panic_append_str(buf, max, &idx, key);
  panic_append_str(buf, max, &idx, ": ");
  panic_append_u64(buf, max, &idx, value);
  if (suffix)
    panic_append_str(buf, max, &idx, suffix);
}

static void panic_make_kv_hex(char *buf, size_t max, const char *key,
                              uint64_t value, int width) {
  size_t idx = 0;
  if (!buf || max == 0)
    return;
  buf[0] = '\0';
  panic_append_str(buf, max, &idx, key);
  panic_append_str(buf, max, &idx, ": ");
  panic_append_hex(buf, max, &idx, value, width);
}

static void panic_fb_fill_rect(uint32_t *fb, uint32_t pitch_pixels, uint32_t fb_w,
                               uint32_t fb_h, int x, int y, int w, int h,
                               uint32_t color) {
  if (!fb || w <= 0 || h <= 0)
    return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > (int)fb_w)
    w = (int)fb_w - x;
  if (y + h > (int)fb_h)
    h = (int)fb_h - y;
  if (w <= 0 || h <= 0)
    return;

  for (int row = y; row < y + h; row++) {
    uint32_t *dst = fb + row * pitch_pixels + x;
    for (int col = 0; col < w; col++)
      dst[col] = color;
  }
}

static void panic_fb_draw_char(uint32_t *fb, uint32_t pitch_pixels, uint32_t fb_w,
                               uint32_t fb_h, int x, int y, char c,
                               uint32_t fg, uint32_t bg) {
  const uint8_t *glyph;
  const uint32_t shadow = 0x000000;

  (void)bg;

  if (!fb)
    return;
  glyph = font_data[(unsigned char)c];
  for (int row = 0; row < FONT_HEIGHT; row++) {
    for (int col = 0; col < FONT_WIDTH; col++) {
      if (!(glyph[row] & (0x80 >> col)))
        continue;

      
      int shadow_px = x + col + 1;
      int shadow_py = y + row + 1;
      if (shadow_px >= 0 && shadow_px < (int)fb_w &&
          shadow_py >= 0 && shadow_py < (int)fb_h) {
        fb[shadow_py * pitch_pixels + shadow_px] = shadow;
      }
      shadow_px = x + col - 1;
      shadow_py = y + row - 1;
      if (shadow_px >= 0 && shadow_px < (int)fb_w &&
          shadow_py >= 0 && shadow_py < (int)fb_h) {
        fb[shadow_py * pitch_pixels + shadow_px] = shadow;
      }
      shadow_px = x + col + 1;
      shadow_py = y + row - 1;
      if (shadow_px >= 0 && shadow_px < (int)fb_w &&
          shadow_py >= 0 && shadow_py < (int)fb_h) {
        fb[shadow_py * pitch_pixels + shadow_px] = shadow;
      }
      shadow_px = x + col - 1;
      shadow_py = y + row + 1;
      if (shadow_px >= 0 && shadow_px < (int)fb_w &&
          shadow_py >= 0 && shadow_py < (int)fb_h) {
        fb[shadow_py * pitch_pixels + shadow_px] = shadow;
      }
      
    }
  }

  for (int row = 0; row < FONT_HEIGHT; row++) {
    int py = y + row;
    if (py < 0 || py >= (int)fb_h)
      continue;
    for (int col = 0; col < FONT_WIDTH; col++) {
      int px = x + col;
      if (px < 0 || px >= (int)fb_w)
        continue;
      if (glyph[row] & (0x80 >> col))
        fb[py * pitch_pixels + px] = fg;
    }
  }
}

static void panic_fb_draw_string(uint32_t *fb, uint32_t pitch_pixels, uint32_t fb_w,
                                 uint32_t fb_h, int x, int y, const char *str,
                                 uint32_t fg, uint32_t bg) {
  int dx = x;
  while (str && *str) {
    panic_fb_draw_char(fb, pitch_pixels, fb_w, fb_h, dx, y, *str, fg, bg);
    dx += FONT_WIDTH;
    str++;
  }
}

static void panic_fb_draw_wrapped(uint32_t *fb, uint32_t pitch_pixels,
                                  uint32_t fb_w, uint32_t fb_h, int x, int y,
                                  int max_chars, const char *str, uint32_t fg,
                                  uint32_t bg, int max_lines) {
  char line[96];
  int line_len = 0;
  int lines_drawn = 0;

  if (!str || max_chars <= 0 || max_lines <= 0)
    return;

  for (size_t i = 0;; i++) {
    char c = str[i];
    int flush = 0;

    if (c == '\0' || c == '\n' || line_len >= max_chars) {
      flush = 1;
    } else {
      line[line_len++] = c;
    }

    if (flush) {
      line[line_len] = '\0';
      panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, x,
                           y + lines_drawn * (FONT_HEIGHT + 2), line, fg, bg);
      lines_drawn++;
      line_len = 0;
      if (lines_drawn >= max_lines || c == '\0')
        break;
      if (c == '\n')
        continue;
      if (c != '\0')
        line[line_len++] = c;
    }
  }
}

static void panic_halt_forever(void) __attribute__((noreturn));
static volatile int kernel_panic_fence_active = 0;

static void panic_halt_forever(void) {
  for (;;) {
    arch_irq_disable();
#if defined(ARCH_X86_64) || defined(ARCH_X86)
    asm volatile("hlt");
#elif defined(ARCH_ARM64)
    asm volatile("wfi");
#else
    arch_halt();
#endif
  }
}

static void panic_uart_put_hex(uint64_t value) {
  static const char hex[] = "0123456789ABCDEF";
  uart_puts("0x");
  for (int shift = 60; shift >= 0; shift -= 4)
    uart_putc(hex[(value >> shift) & 0xF]);
}

int kernel_panic_fence_is_active(void) { return kernel_panic_fence_active != 0; }

void kernel_panic_fence_fault(uint64_t fault_addr, uint64_t fault_type) {
  arch_irq_disable();
  uart_puts("\nKERNEL PANIC FENCE: fault while rendering/reporting panic\n");
  uart_puts("Fault address: ");
  panic_uart_put_hex(fault_addr);
  uart_puts(" type: ");
  panic_uart_put_hex(fault_type);
  uart_puts("\nHalting through panic fence.\n");
  panic_halt_forever();
}

static void panic_draw_screen(const char *msg, uintptr_t caller_hint,
                              uintptr_t stack_hint) {
  uint32_t *fb = NULL;
  uint32_t fb_w = 0;
  uint32_t fb_h = 0;
  uint32_t pitch = 0;
  uint32_t pitch_pixels;
  char cpu_info[96] = "unavailable";
  char stop_code_line[160];
  int panel_x;
  int panel_y;
  int panel_w;
  int panel_h;

  extern void fb_get_info(uint32_t **buffer, uint32_t *width, uint32_t *height);
  extern uint32_t fb_get_pitch(void);

  fb_get_info(&fb, &fb_w, &fb_h);
  pitch = fb_get_pitch();
#if defined(ARCH_X86_64)
  if ((!fb || !fb_w || !fb_h) || !pitch) {
    extern int limine_get_framebuffer(uint32_t **buffer, uint32_t *width,
                                      uint32_t *height, uint32_t *pitch);
    limine_get_framebuffer(&fb, &fb_w, &fb_h, &pitch);
  }
#else
  if (!fb || !fb_w || !fb_h) {
    extern int fb_init(void);
    if (fb_init() == 0) {
      fb_get_info(&fb, &fb_w, &fb_h);
      pitch = fb_get_pitch();
    }
  }
#endif
  if (!fb || !fb_w || !fb_h)
    return;

  pitch_pixels = pitch ? (pitch / 4) : fb_w;
  arch_cpu_info(cpu_info, sizeof(cpu_info));

  (void)caller_hint;
  (void)stack_hint;
  {
    size_t stop_code_idx = 0;
    const char *stop_code = (msg && msg[0]) ? msg : "KERNEL_PANIC";

    stop_code_line[0] = '\0';
    panic_append_str(stop_code_line, sizeof(stop_code_line), &stop_code_idx,
                     "Stop code: ");
    panic_append_str(stop_code_line, sizeof(stop_code_line), &stop_code_idx,
                     stop_code);
  }
//0x111273
  //panic_fb_fill_rect(fb, pitch_pixels, fb_w, fb_h, 0, 0, (int)fb_w, (int)fb_h,
  //                   0x111273);

  panel_x = 0;
  panel_y = 0;
  panel_w = (int)fb_w - 0;
  panel_h = (int)fb_h - 0;
  if (panel_w < 120 || panel_h < 120)
    return;
  panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x, panel_y + 0,
                       "OS ran into a problem and needed to stop.", 0xFFFFFF, 0x111273);
                       
  panic_fb_draw_wrapped(fb, pitch_pixels, fb_w, fb_h, panel_x, panel_y + 60,
                        (panel_w - 36) / FONT_WIDTH, stop_code_line, 0xFFFFFF,
                        0x111273, 2);
  panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x, panel_y + 80, "OS Version: 8.0.0", 0xEAF3FF, 0x111273);

  panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x, panel_y + 100,
                       "Build #:", 0xEAF3FF, 0x111273);
  panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x + 72, panel_y + 100,
                       BUILD_NUMBER, 0xFFFFFF, 0x111273);
  panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x, panel_y + 120,
                       "Branch:", 0xEAF3FF, 0x111273);
  panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x + 64, panel_y + 120,
                       BUILD_BRANCH, 0xFFFFFF, 0x111273);
  panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x, panel_y + 140,
                       "Compiled:", 0xEAF3FF, 0x111273);
  panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x + 80, panel_y + 140,
                       BUILD_COMPILE_TIME, 0xFFFFFF, 0x111273);
  panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x, panel_y + 160,
                       "Arch:", 0xEAF3FF, 0x111273);
  panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x + 44, panel_y + 160,
                       ARCH_NAME, 0xFFFFFF, 0x111273);
  panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x, panel_y + 180,
                       "CPU:", 0xEAF3FF, 0x111273);
  panic_fb_draw_wrapped(fb, pitch_pixels, fb_w, fb_h, panel_x + 36, panel_y + 180,
                        (panel_w - 92) / FONT_WIDTH, cpu_info, 0xFFFFFF, 0x111273,
                        2);
/*
 * panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x, panel_y + 220,
 *                      "Debug Info", 0xFFFFFF, 0x111273);
 * panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x, panel_y + 240,
 *                      line0, 0xEAF3FF, 0x111273);
 * panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x, panel_y + 240,
 *                      line1, 0xEAF3FF, 0x111273);
 * panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x, panel_y + 260,
 *                      line2, 0xEAF3FF, 0x111273);
 * panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x, panel_y + 260,
 *                      line3, 0xEAF3FF, 0x111273);
 * panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x, panel_y + 280,
 *                      line4, 0xEAF3FF, 0x111273);
 * panic_fb_draw_string(fb, pitch_pixels, fb_w, fb_h, panel_x, panel_y + 280,
 *                      line5, 0xEAF3FF, 0x111273);
 */
}

/*
 * kernel_main - Main kernel entry point
 * @dtb: Pointer to device tree blob passed by bootloader
 *
 * This function never returns. After initialization, it either:
 * 1. Starts the init process and enters the scheduler
 * 2. Panics if initialization fails
 */
void kernel_main(void *dtb) {
  /* Initialize early console for debugging */
  uart_early_init();

  /* Print boot banner */
  print_banner();

  (void)dtb; /* Suppress unused warning */
  (void)__kernel_start;
  (void)__kernel_end;

  printk(KERN_INFO "[INIT] architecture early init\n");
  arch_early_init();
  kintegrity_mark_phase(KINTEGRITY_PHASE_ARCH, "arch early init");

  printk(KERN_INFO "[INIT] architecture MMU init\n");
  arch_mmu_init();

#ifdef ARCH_X86_64
  start_x86_64_bringup();
#endif

  /* Initialize all kernel subsystems */
  init_subsystems(dtb);

  printk(KERN_INFO "All subsystems initialized successfully\n");
  printk(KERN_INFO "Starting init process...\n\n");

  /* Start the first userspace process */
  //panic("Debuging os kernel panic handler! to be commented out soon...");
  start_init_process();

  /* This point should never be reached */
  panic("kernel_main returned unexpectedly!");
}

/*
 * print_banner - Display kernel boot banner
 */
static void print_banner(void) {
  printk("\n");
  printk("OS 8\n");
  printk("\n");
#ifdef ARCH_X86_64
  printk("OS8 v%d.%d.%d - x86_64 bring-up\n", OS_VERSION_MAJOR,
         OS_VERSION_MINOR, OS_VERSION_PATCH);
  printk("A Unix-like operating system for x86_64\n");
#else
  printk("OS8 v%d.%d.%d - ARM64 with GUI\n", OS_VERSION_MAJOR,
         OS_VERSION_MINOR, OS_VERSION_PATCH);
  printk("A Unix-like operating system for ARM64\n");
#endif
  printk("Copyright (c) 2026 OS8 Project\n");
  printk("Build UUID: %s\n", BUILD_UUID);
  printk("Build: %s\n", BUILD_STRING);
  printk("Branch: %s\n", BUILD_BRANCH);
  printk("Compiled: %s\n", BUILD_COMPILE_TIME);
  printk("\n");
}

#ifdef ARCH_X86_64
static void start_x86_64_bringup(void) {
  printk(KERN_INFO "[INIT] x86_64 early bring-up\n");
  printk(KERN_INFO "  Using conservative Limine framebuffer path\n");

  extern void *limine_get_rsdp(void);
  extern int fb_init(void);
  extern const char *limine_get_kernel_cmdline(void);
  extern void boot_parse_cmdline(const char *cmdline);
  extern void fb_show_boot_log(void);

  boot_parse_cmdline(limine_get_kernel_cmdline());

  if (fb_init() != 0) {
    panic("Failed to initialize framebuffer on x86_64!");
  }

  fb_show_boot_log();

  printk(KERN_INFO "x86_64: initializing ACPI tables\n");
  acpi_init(limine_get_rsdp());
  printk(KERN_INFO "x86_64: ACPI CPU topology reports %u CPU(s)\n",
         arch_cpu_count());

  printk(KERN_INFO "x86_64: framebuffer bring-up stable, continuing boot\n");
}
#endif

static void populate_seed_filesystem(void) {
  populate_seed_tree_at("");
  /*
   * Import any staged image before creating the in-memory installer payload.
   * Otherwise the generated /install/system-image is immediately discovered
   * and copied back into /, making setup do a large duplicate tree copy.
   */
  import_staged_system_image();
#if CONFIG_EMBED_INSTALLER_PAYLOAD
  populate_installer_payload();
#endif
}

static int build_seed_path(char *dst, size_t dst_size, const char *prefix,
                           const char *path) {
  size_t idx = 0;
  const char *use_prefix = prefix ? prefix : "";
  const char *use_path = path ? path : "";

  if (!dst || dst_size == 0)
    return -1;

  if (!use_prefix[0]) {
    if (use_path[0] != '/' && idx < dst_size - 1)
      dst[idx++] = '/';
  } else {
    for (size_t i = 0; use_prefix[i] && idx < dst_size - 1; i++)
      dst[idx++] = use_prefix[i];
    if (idx > 0 && dst[idx - 1] == '/' && use_path[0] == '/')
      use_path++;
    else if (idx > 0 && dst[idx - 1] != '/' && use_path[0] != '/' &&
             idx < dst_size - 1)
      dst[idx++] = '/';
  }

  for (size_t i = 0; use_path[i] && idx < dst_size - 1; i++)
    dst[idx++] = use_path[i];
  dst[idx] = '\0';
  return 0;
}

static void seed_make_dir(const char *prefix, const char *path) {
  char full_path[256];
  extern int ramfs_create_dir(const char *path, mode_t mode);

  if (build_seed_path(full_path, sizeof(full_path), prefix, path) != 0)
    return;
  ramfs_create_dir(full_path, 0755);
}

static void seed_write_text(const char *prefix, const char *path, mode_t mode,
                            const char *content) {
  char full_path[256];
  extern int ramfs_create_file(const char *path, mode_t mode,
                               const char *content);

  if (build_seed_path(full_path, sizeof(full_path), prefix, path) != 0)
    return;
  ramfs_create_file(full_path, mode, content);
}

static void seed_write_bytes(const char *prefix, const char *path, mode_t mode,
                             const uint8_t *data, size_t size) {
  char full_path[256];
  extern int ramfs_create_file_bytes(const char *path, mode_t mode,
                                     const uint8_t *data, size_t size);

  if (build_seed_path(full_path, sizeof(full_path), prefix, path) != 0)
    return;
  ramfs_create_file_bytes(full_path, mode, data, size);
}

static void setup_virtual_mountpoints(void) {
  int ret;

  printk(KERN_INFO "  Preparing virtual filesystem mountpoints...\n");

  ret = vfs_mkdir("/proc", 0555);
  if (ret != 0 && ret != -EEXIST)
    printk(KERN_WARNING "  Failed to create /proc (%d)\n", ret);

  ret = vfs_mkdir("/sys", 0555);
  if (ret != 0 && ret != -EEXIST)
    printk(KERN_WARNING "  Failed to create /sys (%d)\n", ret);

  ret = vfs_mkdir("/dev", 0755);
  if (ret != 0 && ret != -EEXIST)
    printk(KERN_WARNING "  Failed to create /dev (%d)\n", ret);

  seed_make_dir("", "/sys/kernel");
  seed_write_text("", "/proc/version", 0444,
                  "OS8 8.0.0\n");
  seed_write_text("", "/sys/kernel/ostype", 0444,
                  "OS8\n");
  seed_write_text("", "/sys/kernel/osrelease", 0444,
                  "8.0.0\n");
  seed_write_text("", "/dev/null", 0666, "");
  seed_write_text("", "/dev/zero", 0666, "");
  seed_write_text("", "/dev/tty", 0666, "");
}

static uint32_t boot_sha256_rotr(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

static void boot_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
  static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
      0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
      0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
      0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
      0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint64_t bit_len = (uint64_t)len * 8;
  size_t padded_len = len + 1 + 8;
  if (padded_len & 63)
    padded_len = (padded_len + 63) & ~(size_t)63;

  for (size_t offset = 0; offset < padded_len; offset += 64) {
    uint8_t block[64];
    uint32_t w[64];

    memset(block, 0, sizeof(block));
    for (size_t i = 0; i < 64; i++) {
      size_t pos = offset + i;
      if (pos < len)
        block[i] = data[pos];
      else if (pos == len)
        block[i] = 0x80;
    }
    if (offset + 64 == padded_len) {
      for (int i = 0; i < 8; i++)
        block[63 - i] = (uint8_t)(bit_len >> (i * 8));
    }

    for (int i = 0; i < 16; i++) {
      w[i] = ((uint32_t)block[i * 4] << 24) |
             ((uint32_t)block[i * 4 + 1] << 16) |
             ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = boot_sha256_rotr(w[i - 15], 7) ^
                    boot_sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = boot_sha256_rotr(w[i - 2], 17) ^
                    boot_sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t s1 = boot_sha256_rotr(e, 6) ^ boot_sha256_rotr(e, 11) ^
                    boot_sha256_rotr(e, 25);
      uint32_t ch = (e & f) ^ ((~e) & g);
      uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
      uint32_t s0 = boot_sha256_rotr(a, 2) ^ boot_sha256_rotr(a, 13) ^
                    boot_sha256_rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t temp2 = s0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }

  for (int i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)h[i];
  }
}

static void boot_hex_digest(const uint8_t digest[32], char out[65]) {
  static const char hex[] = "0123456789abcdef";

  for (int i = 0; i < 32; i++) {
    out[i * 2] = hex[digest[i] >> 4];
    out[i * 2 + 1] = hex[digest[i] & 0xf];
  }
  out[64] = '\0';
}

static int boot_cfg_append(char *dst, size_t dst_size, size_t *idx,
                           const char *text) {
  if (!dst || !idx || !text || *idx >= dst_size)
    return -1;

  while (*text) {
    if (*idx + 1 >= dst_size)
      return -1;
    dst[(*idx)++] = *text++;
  }
  dst[*idx] = '\0';
  return 0;
}

static int build_os8_boot_config(const uint8_t *kernel_image,
                                 size_t kernel_size,
                                 const uint8_t *startup_image,
                                 size_t startup_size, char *dst,
                                 size_t dst_size) {
  uint8_t kernel_digest[32];
  uint8_t startup_digest[32];
  char kernel_hex[65];
  char startup_hex[65];
  size_t idx = 0;

  if (!kernel_image || kernel_size == 0 || !startup_image ||
      startup_size == 0 || !dst || dst_size == 0)
    return -1;

  boot_sha256(startup_image, startup_size, startup_digest);
  boot_sha256(kernel_image, kernel_size, kernel_digest);
  boot_hex_digest(startup_digest, startup_hex);
  boot_hex_digest(kernel_digest, kernel_hex);
  dst[0] = '\0';

  if (boot_cfg_append(dst, dst_size, &idx,
                      "version=1\n"
                      "input_timeout_ms=1500\n"
                      "startup_path=\\EFI\\OS8\\STARTUPX64.EFI\n"
                      "startup_sha256=") != 0 ||
      boot_cfg_append(dst, dst_size, &idx, startup_hex) != 0 ||
      boot_cfg_append(dst, dst_size, &idx,
                      "\n"
                      "kernel_path=\\boot\\main.sys\n"
                      "kernel_sha256=") != 0 ||
      boot_cfg_append(dst, dst_size, &idx, kernel_hex) != 0 ||
      boot_cfg_append(dst, dst_size, &idx,
                      "\n"
                      "trusted_key=os8-development\n"
                      "recovery_partition=auto\n"
                      "boot_options=normal\n") != 0)
    return -1;

  return 0;
}

static int verify_init_script(void) {
  static int cached_result = -1;
  uint8_t digest[32];

  if (cached_result >= 0)
    return cached_result;

  boot_sha256(init_bin, init_bin_len, digest);
  cached_result = 1;
  for (int i = 0; i < 32; i++) {
    if (digest[i] != init_bin_sha256[i]) {
      cached_result = 0;
      break;
    }
  }

  if (cached_result)
    printk(KERN_INFO "INIT: /sbin/init verification passed\n");
  else
    printk(KERN_ERR "INIT-0001: /sbin/init verification failed\n");
  return cached_result;
}

static void populate_seed_tree_at(const char *prefix) {
#if CONFIG_EMBED_SEED_ASSETS
  extern const unsigned char bootstrap_test_png[];
  extern const unsigned int bootstrap_test_png_len;
  extern const unsigned char bootstrap_logo_png[];
  extern const unsigned int bootstrap_logo_png_len;
#endif
  static const char dark_theme_text[] =
      "# OS8 theme preset\n"
      "name=Dark Theme\n"
      "mode=dark\n"
      "app_bg=1A1A2E\n"
      "app_fg=E4E4E7\n"
      "accent=6366F1\n"
      "accent_soft=EC4899\n"
      "surface=27272A\n"
      "surface_alt=1F2937\n"
      "card=252535\n"
      "border=52525B\n"
      "settings_bg=141824\n"
      "settings_panel=1F2937\n"
      "settings_text=F2F2F2\n"
      "settings_subtext=F8F8F8\n";
  static const char light_theme_text[] =
      "# OS8 theme preset\n"
      "name=Light Theme\n"
      "mode=light\n"
      "app_bg=F4F7FB\n"
      "app_fg=172033\n"
      "accent=2563EB\n"
      "accent_soft=DB2777\n"
      "surface=E9EEF5\n"
      "surface_alt=F6F9FC\n"
      "card=FFFFFF\n"
      "border=C9D4E5\n"
      "settings_bg=ECF3FA\n"
      "settings_panel=D7E2EF\n"
      "settings_text=1B2430\n"
      "settings_subtext=627084\n";

  seed_make_dir(prefix, "Documents");
  seed_make_dir(prefix, "Downloads");
  seed_make_dir(prefix, "Pictures");
  seed_make_dir(prefix, "assets");
  seed_make_dir(prefix, "assets/themes");
  seed_make_dir(prefix, "assets/wallpapers");
  seed_make_dir(prefix, "System");
  seed_make_dir(prefix, "Desktop");
  seed_make_dir(prefix, "System/Apps");
  seed_make_dir(prefix, "Desktop/System Apps");
  seed_make_dir(prefix, "/Desktop/Projects");

  seed_write_text(prefix, "/Desktop/notes.txt", 0644,
                  "Welcome to OS8!\n\nThis is your desktop - right-click "
                  "for options!\n");
  seed_write_text(prefix, "/Desktop/readme.txt", 0644,
                  "OS8 Desktop Manager\n\n- Double-click to open files\n- "
                  "Right-click for context menu\n");
  seed_write_text(prefix, "readme.txt", 0644,
                  "Welcome to OS8!\nThis is a real file in RamFS.");
  seed_write_text(prefix, "todo.txt", 0644,
                  "- Implement Browser\n- Fix Bugs\n- Sleep");
  seed_write_text(prefix, "assets/themes/dark.theme", 0644, dark_theme_text);
  seed_write_text(prefix, "assets/themes/light.theme", 0644, light_theme_text);
  seed_write_bytes(prefix, "sample.mp3", 0644, os_seed_mp3, os_seed_mp3_len);
#if CONFIG_EMBED_SEED_ASSETS
  seed_write_bytes(prefix, "assets/logo.png", 0644, bootstrap_logo_png,
                   bootstrap_logo_png_len);
  seed_write_bytes(prefix, "assets/wallpapers/landscape.png", 0644,
                   bootstrap_landscape_png, bootstrap_landscape_png_len);
  seed_write_bytes(prefix, "assets/wallpapers/nature.jpg", 0644,
                   bootstrap_nature_jpg, bootstrap_nature_jpg_len);
  seed_write_bytes(prefix, "assets/wallpapers/city.jpg", 0644,
                   bootstrap_city_jpg, bootstrap_city_jpg_len);
  seed_write_bytes(prefix, "assets/wallpapers/portrait.jpg", 0644,
                   bootstrap_portrait_jpg, bootstrap_portrait_jpg_len);
  seed_write_bytes(prefix, "assets/wallpapers/square.jpg", 0644,
                   bootstrap_square_jpg, bootstrap_square_jpg_len);
  seed_write_bytes(prefix, "assets/wallpapers/ducks.png", 0644,
                   bootstrap_ducks_png, bootstrap_ducks_png_len);
  seed_write_bytes(prefix, "assets/wallpapers/default.jpg", 0644,
                   bootstrap_default_jpg, bootstrap_default_jpg_len);
  seed_write_bytes(prefix, "assets/wallpapers/default.svg", 0644,
                   bootstrap_default_svg, bootstrap_default_svg_len);
  seed_write_bytes(prefix, "assets/cursor.svg", 0644, bootstrap_cursor_svg,
                   bootstrap_cursor_svg_len);
  seed_write_bytes(prefix, "Pictures/test.png", 0644, bootstrap_test_png,
                   bootstrap_test_png_len);
#endif

  seed_make_dir(prefix, "bin");
  seed_make_dir(prefix, "sbin");
  seed_make_dir(prefix, "usr");
  seed_make_dir(prefix, "usr/bin");

  if (!verify_init_script())
    panic("INIT-0001: /sbin/init verification failed");
  seed_write_bytes(prefix, "/sbin/init", 0755, init_bin, init_bin_len);
  seed_write_bytes(prefix, "/bin/login", 0755, login_bin, login_bin_len);
  seed_write_bytes(prefix, "/bin/sh", 0755, shell_bin, shell_bin_len);

  seed_make_dir(prefix, "examples");
  seed_write_text(prefix, "examples/hello.py", 0644,
                  "# Hello World in Python for OS8\n"
                  "# Run with: run hello.py\n\n"
                  "def greet(name):\n"
                  "    return 'Hello, ' + name + '!'\n\n"
                  "def main():\n"
                  "    print('Welcome to OS8 Python Demo')\n"
                  "    message = greet('OS8 User')\n"
                  "    print(message)\n\n"
                  "if __name__ == '__main__':\n"
                  "    main()\n");
  seed_write_text(prefix, "examples/fibonacci.py", 0644,
                  "# Fibonacci Sequence in Python\n"
                  "# Run with: run fibonacci.py\n\n"
                  "def fibonacci(n):\n"
                  "    if n <= 0: return []\n"
                  "    fib = [0, 1]\n"
                  "    for i in range(2, n):\n"
                  "        fib.append(fib[i-1] + fib[i-2])\n"
                  "    return fib\n\n"
                  "print(fibonacci(10))\n");
  seed_write_text(prefix, "examples/hello.nano", 0644,
                  "// Hello World in NanoLang\n"
                  "// Run with: run hello.nano\n\n"
                  "fn greet(name: str) -> str {\n"
                  "    return 'Hello, ' + name + '!';\n"
                  "}\n\n"
                  "fn main() {\n"
                  "    print('Welcome to NanoLang');\n"
                  "    let msg = greet('OS8');\n"
                  "    print(msg);\n"
                  "}\n");
  seed_write_text(prefix, "examples/calculator.nano", 0644,
                  "// Calculator in NanoLang\n"
                  "fn add(a: int, b: int) -> int { return a + b; }\n"
                  "fn main() {\n"
                  "    print('42 + 7 = ');\n"
                  "    print(add(42, 7));\n"
                  "}\n");
}

static void ensure_boot_payload_dirs(const char *prefix) {
  seed_make_dir(prefix, "boot");
  seed_make_dir(prefix, "EFI");
  seed_make_dir(prefix, "EFI/BOOT");
  seed_make_dir(prefix, "EFI/OS8");
  seed_make_dir(prefix, "limine");
}

typedef struct {
  const char *src_root;
  const char *dst_root;
  int skip_payload_roots;
  int skip_boot_root;
} seed_copy_ctx_t;

static int copy_tree_callback(void *ctx, const char *name, int len,
                              loff_t offset, ino_t ino, unsigned type) {
  seed_copy_ctx_t *copy = (seed_copy_ctx_t *)ctx;
  char src_path[256];
  char dst_path[256];
  int src_len = 0;
  int dst_len = 0;
  struct file *dir;
  seed_copy_ctx_t next;
  uint8_t *data = NULL;
  size_t size = 0;

  (void)offset;
  (void)ino;

  if (!copy || !name || len <= 0)
    return 0;
  if ((len == 1 && name[0] == '.') ||
      (len == 2 && name[0] == '.' && name[1] == '.'))
    return 0;
  if (copy->skip_payload_roots &&
      ((len == 7 && name[0] == 'i' && name[1] == 'n' && name[2] == 's' &&
        name[3] == 't' && name[4] == 'a' && name[5] == 'l' && name[6] == 'l') ||
       (len == 5 && name[0] == 's' && name[1] == 'e' && name[2] == 't' &&
        name[3] == 'u' && name[4] == 'p')))
    return 0;
  if (copy->skip_boot_root && len == 4 && name[0] == 'b' && name[1] == 'o' &&
      name[2] == 'o' && name[3] == 't')
    return 0;

  src_path[0] = '\0';
  if (copy->src_root) {
    for (src_len = 0;
         copy->src_root[src_len] && src_len < (int)sizeof(src_path) - 1;
         src_len++) {
      src_path[src_len] = copy->src_root[src_len];
    }
    src_path[src_len] = '\0';
  }
  if (!(src_len == 1 && src_path[0] == '/') && src_len < (int)sizeof(src_path) - 1)
    src_path[src_len++] = '/';
  for (int i = 0; i < len && src_len < (int)sizeof(src_path) - 1; i++)
    src_path[src_len++] = name[i];
  src_path[src_len] = '\0';

  dst_path[0] = '\0';
  if (copy->dst_root) {
    for (dst_len = 0;
         copy->dst_root[dst_len] && dst_len < (int)sizeof(dst_path) - 1;
         dst_len++) {
      dst_path[dst_len] = copy->dst_root[dst_len];
    }
    dst_path[dst_len] = '\0';
  }
  if (!(dst_len == 1 && dst_path[0] == '/') && dst_len < (int)sizeof(dst_path) - 1)
    dst_path[dst_len++] = '/';
  for (int i = 0; i < len && dst_len < (int)sizeof(dst_path) - 1; i++)
    dst_path[dst_len++] = name[i];
  dst_path[dst_len] = '\0';

  if (type == 4) {
    seed_make_dir("", dst_path);
    next.src_root = src_path;
    next.dst_root = dst_path;
    next.skip_payload_roots = 0;
    next.skip_boot_root = 0;
    dir = vfs_open(src_path, O_RDONLY, 0);
    if (!dir)
      return 0;
    vfs_readdir(dir, &next, copy_tree_callback);
    vfs_close(dir);
    return 0;
  }

  if (media_load_file(src_path, &data, &size) == 0) {
    media_install_file(dst_path, data, size);
    media_free_file(data);
  }
  return 0;
}

static int copy_tree_to_prefix(const char *src_root, const char *dst_root,
                               int skip_payload_roots,
                               int skip_boot_root) {
  struct file *dir;
  seed_copy_ctx_t ctx;

  dir = vfs_open(src_root, O_RDONLY, 0);
  if (!dir)
    return -1;

  ctx.src_root = src_root;
  ctx.dst_root = dst_root;
  ctx.skip_payload_roots = skip_payload_roots;
  ctx.skip_boot_root = skip_boot_root;
  vfs_readdir(dir, &ctx, copy_tree_callback);
  vfs_close(dir);
  return 0;
}

typedef struct {
  const char *root;
} seed_remove_ctx_t;

static int remove_tree_callback(void *ctx, const char *name, int len,
                                loff_t offset, ino_t ino, unsigned type) {
  seed_remove_ctx_t *remove = (seed_remove_ctx_t *)ctx;
  char path[256];
  int idx = 0;
  struct file *dir;

  (void)offset;
  (void)ino;

  if (!remove || !remove->root || !name || len <= 0)
    return 0;
  if ((len == 1 && name[0] == '.') ||
      (len == 2 && name[0] == '.' && name[1] == '.'))
    return 0;

  while (remove->root[idx] && idx < (int)sizeof(path) - 1) {
    path[idx] = remove->root[idx];
    idx++;
  }
  if (!(idx == 1 && path[0] == '/') && idx < (int)sizeof(path) - 1)
    path[idx++] = '/';
  for (int i = 0; i < len && idx < (int)sizeof(path) - 1; i++)
    path[idx++] = name[i];
  path[idx] = '\0';

  if (type == 4) {
    seed_remove_ctx_t child = {path};
    dir = vfs_open(path, O_RDONLY, 0);
    if (dir) {
      vfs_readdir(dir, &child, remove_tree_callback);
      vfs_close(dir);
    }
    vfs_rmdir(path);
    return 0;
  }

  vfs_unlink(path);
  return 0;
}

static void remove_tree_at_path(const char *root) {
  seed_remove_ctx_t ctx = {root};
  struct file *dir;

  if (!root || !root[0])
    return;
  dir = vfs_open(root, O_RDONLY, 0);
  if (!dir)
    return;
  vfs_readdir(dir, &ctx, remove_tree_callback);
  vfs_close(dir);
  vfs_rmdir(root);
}

static void import_staged_system_image(void) {
  printk(KERN_INFO "INSTALL: looking for staged system image at /install/system-image\n");
  if (!staged_system_image_exists()) {
    printk(KERN_INFO "INSTALL: staged system image not found\n");
    return;
  }

  printk(KERN_INFO "INSTALL: staged system image found\n");
  if (copy_tree_to_prefix("/install/system-image", "/", 0, 1) == 0) {
    printk(KERN_INFO
           "INSTALL: imported staged /install/system-image into live root (skipping /boot)\n");
  }
}

static int g_boot_media_assets_imported = 0;

static int import_boot_media_assets_from(const char *media_root) {
  char asset_root[160];

  if (g_boot_media_assets_imported || !media_root || !media_root[0])
    return g_boot_media_assets_imported ? 0 : -1;

  if (build_seed_path(asset_root, sizeof(asset_root), media_root, "assets") != 0)
    return -1;

  if (copy_tree_to_prefix(asset_root, "/assets", 0, 0) != 0)
    return -1;

  g_boot_media_assets_imported = 1;
  printk(KERN_INFO "ASSETS: imported boot media assets from %s\n", asset_root);
  boot_splash_prepare();
  return 0;
}

static int staged_system_image_exists(void) {
  struct file *dir = vfs_open("/install/system-image", O_RDONLY, 0);
  if (!dir)
    return 0;
  vfs_close(dir);
  return 1;
}

static int boot_hdd_disk_index(void) {
  extern int storage_get_disk_count(void);
  extern int storage_get_disk_kind(int index);

  for (int i = 0; i < storage_get_disk_count(); i++) {
    int kind = storage_get_disk_kind(i);
    if (kind == STORAGE_KIND_CDROM || kind == STORAGE_KIND_USB_MASS_STORAGE)
      continue;
    return i;
  }
  return -1;
}

void refresh_external_storage_views(void) {
  extern int storage_get_disk_count(void);
  extern int storage_get_disk_kind(int index);
  extern int storage_get_disk_location(int index, char *buf, int max);
  char location[32];
  char mounted_root[128];
  char external_root[128];
  char source_root[128];
  int boot_disk = boot_hdd_disk_index();

  seed_make_dir("", "/mnt");
  seed_make_dir("", "/External");

  for (int i = 0; i < storage_get_disk_count(); i++) {
    int kind = storage_get_disk_kind(i);

    if (i == boot_disk && kind != STORAGE_KIND_CDROM &&
        kind != STORAGE_KIND_USB_MASS_STORAGE)
      continue;
    if (storage_get_disk_location(i, location, sizeof(location)) != 0)
      continue;

    build_seed_path(mounted_root, sizeof(mounted_root), "/mnt", location);
    seed_make_dir("", mounted_root);

    build_seed_path(external_root, sizeof(external_root), "/External", location);
    seed_make_dir("", external_root);

    if (kind == STORAGE_KIND_CDROM) {
      if (vfs_mount(location, mounted_root, "iso9660", 0, NULL) == 0) {
        printk(KERN_INFO
               "STORAGE: mounted CD-ROM '%s' on '%s'\n",
               location, mounted_root);
        {
          extern int boot_is_installer_mode(void);
          if (!boot_is_installer_mode())
            import_boot_media_assets_from(mounted_root);
        }
        printk(KERN_INFO "INSTALL: installer payload available at %s/install\n",
               mounted_root);
        continue;
      }
      printk(KERN_WARNING
             "STORAGE: CD-ROM '%s' is not ready for ISO9660 access\n",
             location);
      continue;
    }

    build_seed_path(source_root, sizeof(source_root), "/Installed", location);
    if (copy_tree_to_prefix(source_root, mounted_root, 0, 0) == 0) {
      copy_tree_to_prefix(mounted_root, external_root, 0, 0);
      continue;
    }

    build_seed_path(source_root, sizeof(source_root), "/Installed", location);
    if ((int)sizeof(source_root) > 0) {
      int len = 0;
      while (source_root[len] && len < (int)sizeof(source_root) - 1)
        len++;
      if (len < (int)sizeof(source_root) - 5) {
        source_root[len++] = '/';
        source_root[len++] = 'D';
        source_root[len++] = 'a';
        source_root[len++] = 't';
        source_root[len++] = 'a';
        source_root[len] = '\0';
        if (copy_tree_to_prefix(source_root, mounted_root, 0, 0) == 0)
          copy_tree_to_prefix(mounted_root, external_root, 0, 0);
      }
    }
  }

  printk(KERN_INFO
         "STORAGE: mounted disk views refreshed under /mnt with /External mirrors\n");
}

static void populate_installer_payload(void) {
#if defined(ARCH_X86_64) && CONFIG_EMBED_INSTALLER_PAYLOAD
  extern int boot_is_installer_mode(void);
  extern void *limine_get_kernel_file_addr(void);
  extern uint64_t limine_get_kernel_file_size(void);
  extern const unsigned char installer_payload_bootx64_efi[];
  extern const unsigned char installer_payload_bootx64_efi_end[];
  extern const unsigned char installer_payload_startupx64_efi[];
  extern const unsigned char installer_payload_startupx64_efi_end[];
  extern const unsigned char installer_payload_limine_bios_sys[];
  extern const unsigned char installer_payload_limine_bios_sys_end[];
  extern const unsigned char installer_payload_limine_bios_cd_bin[];
  extern const unsigned char installer_payload_limine_bios_cd_bin_end[];
  static const char *installed_limine_cfg =
      "# OS8 Boot Configuration\n"
      "# OS8 x64 legacy BIOS fallback\n"
      "\n"
      "timeout: 0\n"
      "\n"
      "/OS8\n"
      "    protocol: limine\n"
      "    kernel_path: boot():/boot/bootloader.sys\n";
  static const char *installer_limine_cfg =
      "# OS8 Boot Configuration\n"
      "# OS8 x64 graphical installer legacy BIOS fallback\n"
      "\n"
      "timeout: 5\n"
      "\n"
      "/OS8 Graphical Installer\n"
      "    protocol: limine\n"
      "    kernel_path: boot():/boot/bootloader.sys\n"
      "    cmdline: boot=usb mode=installer drivers=generic\n";
  static const char *image_info =
      "OS8 System Image\n"
      "\n"
      "This installer boot seeds bundled system and boot-file images so\n"
      "the GUI installer can copy a complete system to disk.\n";
  static const char *installed_bootable_cfg =
      "bootable=1\n"
      "loader=os8-custom\n"
      "source=installed-system\n";
  static const char *installed_bios_bootable_cfg =
      "bootable=1\n"
      "scheme=mbr\n"
      "active_partition=System\n"
      "loader=limine-bios\n"
      "source=installed-system\n";
  static const char *installed_installer_state =
      "installed=1\n"
      "profile=system-image\n"
      "source=installed-system\n"
      "first_boot_setup=1\n";
  static const char *installed_efi_boot_cfg =
      "bootable=1\n"
      "loader=os8-custom\n"
      "source=installed-system\n";
  static const char *installed_mbr_boot_cfg =
      "bootable=1\n"
      "scheme=mbr\n"
      "active_partition=System\n"
      "loader=limine-bios\n"
      "source=installed-system\n";
  static const char *installers_txt =
      "OS8 Graphical Installer\n"
      "\n"
      "This media boots directly into the OS8 graphical installer.\n"
      "The installer uses /install/system-image.zip and /install/boot-files.img\n"
      "to copy a complete system to the selected disk.\n";
  static const char *setup_info =
      "OS8 Installer Media\n"
      "\n"
      "This directory mirrors the bootable installer media contents while\n"
      "running in setup mode, including the boot-file image payload.\n";
  uint8_t *boot_archive_data = NULL;
  size_t boot_archive_size = 0;
  const uint8_t *kernel_image;
  size_t kernel_size;
  size_t bootx64_efi_size;
  size_t startupx64_efi_size;
  size_t limine_bios_sys_size;
  size_t limine_bios_cd_size;
  char os8boot_cfg[512];
  int installer_mode = boot_is_installer_mode();

  kernel_image = (const uint8_t *)limine_get_kernel_file_addr();
  kernel_size = (size_t)limine_get_kernel_file_size();
  if (!kernel_image || kernel_size == 0) {
    printk(KERN_ERR "INSTALL: kernel image unavailable for installer payload\n");
    return;
  }
  bootx64_efi_size = (size_t)(installer_payload_bootx64_efi_end -
                              installer_payload_bootx64_efi);
  startupx64_efi_size = (size_t)(installer_payload_startupx64_efi_end -
                                 installer_payload_startupx64_efi);
  limine_bios_sys_size = (size_t)(installer_payload_limine_bios_sys_end -
                                  installer_payload_limine_bios_sys);
  limine_bios_cd_size = (size_t)(installer_payload_limine_bios_cd_bin_end -
                                 installer_payload_limine_bios_cd_bin);
  if (build_os8_boot_config(kernel_image, kernel_size,
                            installer_payload_startupx64_efi,
                            startupx64_efi_size, os8boot_cfg,
                            sizeof(os8boot_cfg)) != 0) {
    printk(KERN_ERR "INSTALL: failed to generate OS8 boot configuration\n");
    return;
  }
  int staged_image_present = staged_system_image_exists();

  if (!staged_image_present) {
    seed_make_dir("", "/install");
    seed_make_dir("", "/install/system-image");
    ensure_boot_payload_dirs("/install/system-image");
    populate_seed_tree_at("/install/system-image");
  }

  if (!staged_image_present) {
    int install_seed_failed =
        media_install_file("/install/system-image/boot/main.sys", kernel_image,
                           kernel_size) != 0 ||
        media_install_file("/install/system-image/boot/bootloader.sys",
                           kernel_image, kernel_size) != 0 ||
        media_install_text_file("/install/system-image/limine.conf",
                                installed_limine_cfg) != 0 ||
        media_install_text_file("/install/system-image/boot/limine.conf",
                                installed_limine_cfg) != 0 ||
        media_install_text_file("/install/system-image/limine/limine.conf",
                                installed_limine_cfg) != 0 ||
        media_install_file("/install/system-image/boot/limine-bios.sys",
                           installer_payload_limine_bios_sys,
                           limine_bios_sys_size) != 0 ||
        media_install_file("/install/system-image/boot/limine-bios-cd.bin",
                           installer_payload_limine_bios_cd_bin,
                           limine_bios_cd_size) != 0 ||
        media_install_text_file("/install/system-image/INSTALLERS.TXT",
                                installers_txt) != 0 ||
        media_install_text_file("/install/system-image/BOOTABLE.CFG",
                                installed_bootable_cfg) != 0 ||
        media_install_text_file("/install/system-image/EFI/BOOT/BOOTABLE.CFG",
                                installed_bootable_cfg) != 0 ||
        media_install_text_file("/install/system-image/boot/BOOTABLE.CFG",
                                installed_bios_bootable_cfg) != 0 ||
        media_install_file("/install/system-image/EFI/BOOT/BOOTX64.EFI",
                           installer_payload_bootx64_efi,
                           bootx64_efi_size) != 0 ||
        media_install_file("/install/system-image/EFI/OS8/STARTUPX64.EFI",
                           installer_payload_startupx64_efi,
                           startupx64_efi_size) != 0 ||
        media_install_text_file("/install/system-image/EFI/OS8/os8boot.cfg",
                                os8boot_cfg) != 0 ||
        media_install_text_file("/install/system-image/System/installer-state.txt",
                                installed_installer_state) != 0 ||
        media_install_text_file("/install/system-image/System/efi-boot.cfg",
                                installed_efi_boot_cfg) != 0 ||
        media_install_text_file("/install/system-image/System/mbr-boot.cfg",
                                installed_mbr_boot_cfg) != 0 ||
        media_install_text_file("/install/system-image/IMAGE_INFO.txt",
                                image_info) != 0;
    if (install_seed_failed) {
      printk(KERN_ERR "INSTALL: failed to seed install disk payload\n");
      return;
    }
  }

  if (installer_mode) {
    int setup_seed_failed =
        media_install_file("/setup/boot/main.sys", kernel_image, kernel_size) !=
            0 ||
        media_install_file("/setup/boot/bootloader.sys", kernel_image,
                           kernel_size) != 0 ||
        media_install_text_file("/setup/limine.conf", installer_limine_cfg) !=
            0 ||
        media_install_text_file("/setup/boot/limine.conf",
                                installer_limine_cfg) != 0 ||
        media_install_text_file("/setup/limine/limine.conf",
                                installer_limine_cfg) != 0 ||
        media_install_file("/setup/boot/limine-bios.sys",
                           installer_payload_limine_bios_sys,
                           limine_bios_sys_size) != 0 ||
        media_install_file("/setup/boot/limine-bios-cd.bin",
                           installer_payload_limine_bios_cd_bin,
                           limine_bios_cd_size) != 0 ||
        media_install_file("/setup/EFI/BOOT/BOOTX64.EFI",
                           installer_payload_bootx64_efi, bootx64_efi_size) != 0 ||
        media_install_file("/setup/EFI/OS8/STARTUPX64.EFI",
                           installer_payload_startupx64_efi,
                           startupx64_efi_size) != 0 ||
        media_install_text_file("/setup/EFI/OS8/os8boot.cfg",
                                os8boot_cfg) != 0 ||
        media_install_text_file("/setup/SETUP_INFO.txt", setup_info) != 0 ||
        media_install_text_file("/setup/INSTALLERS.TXT", installers_txt) != 0;

    if (!staged_image_present) {
      setup_seed_failed =
          setup_seed_failed ||
          media_install_file("/setup/install/system-image/boot/main.sys",
                             kernel_image, kernel_size) != 0 ||
          media_install_file("/setup/install/system-image/boot/bootloader.sys",
                             kernel_image, kernel_size) != 0 ||
          media_install_text_file("/setup/install/system-image/limine.conf",
                                  installed_limine_cfg) != 0 ||
          media_install_text_file("/setup/install/system-image/boot/limine.conf",
                                  installed_limine_cfg) != 0 ||
          media_install_text_file("/setup/install/system-image/limine/limine.conf",
                                  installed_limine_cfg) != 0 ||
          media_install_file("/setup/install/system-image/boot/limine-bios.sys",
                             installer_payload_limine_bios_sys,
                             limine_bios_sys_size) != 0 ||
          media_install_file("/setup/install/system-image/boot/limine-bios-cd.bin",
                             installer_payload_limine_bios_cd_bin,
                             limine_bios_cd_size) != 0 ||
          media_install_text_file("/setup/install/system-image/INSTALLERS.TXT",
                                  installers_txt) != 0 ||
          media_install_text_file("/setup/install/system-image/BOOTABLE.CFG",
                                  installed_bootable_cfg) != 0 ||
          media_install_text_file(
              "/setup/install/system-image/EFI/BOOT/BOOTABLE.CFG",
              installed_bootable_cfg) != 0 ||
          media_install_text_file("/setup/install/system-image/boot/BOOTABLE.CFG",
                                  installed_bios_bootable_cfg) != 0 ||
          media_install_file("/setup/install/system-image/EFI/BOOT/BOOTX64.EFI",
                             installer_payload_bootx64_efi,
                             bootx64_efi_size) != 0 ||
          media_install_file("/setup/install/system-image/EFI/OS8/STARTUPX64.EFI",
                             installer_payload_startupx64_efi,
                             startupx64_efi_size) != 0 ||
          media_install_text_file(
              "/setup/install/system-image/EFI/OS8/os8boot.cfg",
              os8boot_cfg) != 0 ||
          media_install_text_file(
              "/setup/install/system-image/System/installer-state.txt",
              installed_installer_state) != 0 ||
          media_install_text_file("/setup/install/system-image/System/efi-boot.cfg",
                                  installed_efi_boot_cfg) != 0 ||
          media_install_text_file("/setup/install/system-image/System/mbr-boot.cfg",
                                  installed_mbr_boot_cfg) != 0 ||
          media_install_text_file("/setup/install/system-image/IMAGE_INFO.txt",
                                  image_info) != 0;
    }

    if (setup_seed_failed) {
      printk(KERN_ERR "INSTALL: failed to seed setup media payload\n");
      return;
    }
  }

  if (installer_mode) {
    if (copy_tree_to_prefix("/setup", "/setup/bootimage-src", 1, 0) != 0) {
      printk(KERN_ERR "INSTALL: failed to mirror boot files into setup boot image\n");
      return;
    }
    if (media_boot_image_pack_tree("/setup/bootimage-src", &boot_archive_data,
                                   &boot_archive_size) != 0 ||
        media_install_file("/setup/bootimage.img", boot_archive_data,
                           boot_archive_size) != 0) {
      media_free_file(boot_archive_data);
      remove_tree_at_path("/setup/bootimage-src");
      printk(KERN_ERR "INSTALL: failed to package setup boot image\n");
      return;
    }
    media_free_file(boot_archive_data);
    remove_tree_at_path("/setup/bootimage-src");
    boot_archive_data = NULL;
    boot_archive_size = 0;
  }

  if (installer_mode && !staged_image_present &&
      copy_tree_to_prefix("/install/system-image", "/setup/install/system-image",
                          0, 0) != 0) {
    printk(KERN_ERR "INSTALL: failed to mirror boot files into staged system image\n");
    return;
  }

  {
    uint8_t *archive_data = NULL;
    size_t archive_size = 0;

    if (!staged_image_present) {
      if (media_zip_pack_tree("/install/system-image", &archive_data,
                              &archive_size) != 0 ||
          media_install_file("/install/system-image.zip", archive_data,
                             archive_size) != 0) {
        media_free_file(archive_data);
        printk(KERN_ERR "INSTALL: failed to package install disk archive\n");
        return;
      }
      media_free_file(archive_data);

      if (installer_mode) {
        archive_data = NULL;
        archive_size = 0;
        if (media_zip_pack_tree("/setup/install/system-image", &archive_data,
                                &archive_size) != 0 ||
            media_install_file("/setup/install/system-image.zip", archive_data,
                               archive_size) != 0) {
          media_free_file(archive_data);
          printk(KERN_ERR "INSTALL: failed to package setup archive\n");
          return;
        }
        media_free_file(archive_data);
      }
    }
  }

  if (staged_image_present) {
    printk(KERN_INFO "INSTALL: staged system image sourced from install media\n");
  } else {
    printk(KERN_INFO "INSTALL: bundled system image payload seeded in RAMFS\n");
  }
  if (installer_mode)
    printk(KERN_INFO "INSTALL: setup media exposed at /setup/\n");
#else
  /* Non-embedded builds source installer payloads from external media only. */
  return;
#endif
}

/*
 * init_subsystems - Initialize all kernel subsystems
 * @dtb: Device tree blob for hardware discovery
 */
static void init_subsystems(void *dtb) {
  int ret;

  /* ================================================================= */
  /* Phase 1: Core Hardware */
  /* ================================================================= */

  printk(KERN_INFO "[INIT] Phase 1: Core Hardware\n");

  /* Parse device tree for hardware information */
  printk(KERN_INFO "  Parsing device tree...\n");
  (void)dtb; /* TODO: dtb_parse(dtb); */

  /* Initialize interrupt controller */
  printk(KERN_INFO "  Initializing interrupt controller...\n");
  arch_irq_init();

  /* Initialize system timer */
  printk(KERN_INFO "  Initializing timer...\n");
  arch_timer_init();

  /* ================================================================= */
  /* Phase 2: Memory Management */
  /* ================================================================= */

  printk(KERN_INFO "[INIT] Phase 2: Memory Management\n");

  /* Initialize physical memory manager */
  printk(KERN_INFO "  Initializing physical memory manager...\n");
  ret = pmm_init();
  if (ret < 0) {
    panic("Failed to initialize physical memory manager!");
  }
  printk(KERN_INFO "  About to init VMM...\n");

  /* Initialize virtual memory manager */
  printk(KERN_INFO "  Initializing virtual memory manager...\n");
  ret = vmm_init();
  if (ret < 0) {
    panic("Failed to initialize virtual memory manager!");
  }

  /* Initialize kernel heap */
  printk(KERN_INFO "  Initializing kernel heap...\n");
  extern void kmalloc_init(void);
  kmalloc_init();
  kintegrity_mark_phase(KINTEGRITY_PHASE_MEMORY, "memory managers ready");

  /* ================================================================= */
  /* Phase 3: Process Management */
  /* ================================================================= */

  printk(KERN_INFO "[INIT] Phase 3: Process Management\n");

  /* Initialize scheduler */
  printk(KERN_INFO "  Initializing scheduler...\n");
  sched_init();

  /* Initialize process subsystem */
  printk(KERN_INFO "  Initializing process subsystem...\n");
  extern void process_init(void);
  process_init();
  kintegrity_mark_phase(KINTEGRITY_PHASE_PROCESS, "process subsystem ready");

  /* ================================================================= */
  /* Phase 4: Filesystems */
  /* ================================================================= */

  printk(KERN_INFO "[INIT] Phase 4: Filesystems\n");

  /* Initialize Virtual Filesystem */
  printk(KERN_INFO "  Initializing VFS...\n");
  /* Initialize Virtual Filesystem */
  printk(KERN_INFO "  Initializing VFS...\n");
  vfs_init();

  /* Initialize and Register RamFS */
  printk(KERN_INFO "  Initializing RamFS...\n");
  extern int ramfs_init(void);
  ramfs_init();
  extern int iso9660_init(void);
  iso9660_init();

  /* Mount root filesystem */
  printk(KERN_INFO "  Mounting root filesystem...\n");
  if (vfs_mount("ramfs", "/", "ramfs", 0, NULL) != 0) {
    panic("Failed to mount root filesystem!");
  }
<<<<<<< HEAD

  populate_seed_filesystem();

  setup_virtual_mountpoints();
=======

  populate_seed_filesystem();
  kintegrity_mark_phase(KINTEGRITY_PHASE_FS, "filesystems ready");

  /* Mount proc, sys, dev (placeholders) */
  printk(KERN_INFO "  Mounting procfs...\n");

  printk(KERN_INFO "  Mounting sysfs...\n");
  printk(KERN_INFO "  Mounting devfs...\n");
>>>>>>> 8c9572f4cc7ca61e4a09950ae47b17008999ca1e

  /* ================================================================= */
  /* Phase 5: Device Drivers & GUI */
  /* ================================================================= */

  printk(KERN_INFO "[INIT] Phase 5: Device Drivers\n");
  kintegrity_mark_phase(KINTEGRITY_PHASE_DRIVERS, "driver bringup start");

  /* Initialize framebuffer driver */
  printk(KERN_INFO "  Loading framebuffer driver...\n");
  extern int fb_init(void);
  extern void fb_get_info(uint32_t **buffer, uint32_t *width, uint32_t *height);
  extern uint32_t fb_get_pitch(void);
  extern void pci_init(void);
  extern void storage_init(void);
  extern void gui_notify_storage_ready(void);
  extern int boot_is_installer_mode(void);
  extern void pit_sleep(uint32_t ms);
  extern int intel_gfx_detected(void);
  extern int intel_gfx_is_ready(void);
  extern int intel_gfx_is_supported_device(void);
  extern int intel_gfx_has_framebuffer(void);
  extern int intel_gfx_supports_gpu_rendering(void);
  extern int intel_gfx_is_using_framebuffer_fallback(void);
  extern const char *intel_gfx_get_name(void);
  extern int virtio_gpu_init(pci_device_t * pci);
  extern pci_device_t *pci_find_device(uint16_t vendor, uint16_t device);
  extern void gui_refresh_hardware_acceleration_policy(void);
  extern int boot_use_generic_drivers_only(void);
  int generic_drivers_only = boot_use_generic_drivers_only();
  fb_init();

  if (generic_drivers_only) {
    printk(KERN_INFO
           "  Generic driver mode enabled (cmdline: drivers=generic)\n");
  }

  /* Discover PCI GPUs before GUI startup so Intel handoff is ready in time. */
  printk(KERN_INFO "  Initializing PCI bus...\n");
  storage_init();
  refresh_external_storage_views();
  pci_init();
  if (boot_is_installer_mode())
    refresh_external_storage_views();

  printk(KERN_INFO "  Initializing GPU driver...\n");
  if (intel_gfx_is_ready()) {
    printk(KERN_INFO "  GPU: %s initialized%s%s\n", intel_gfx_get_name(),
           intel_gfx_has_framebuffer() ? " with framebuffer handoff" : "",
           intel_gfx_supports_gpu_rendering()
               ? " and full compositor acceleration"
               : "");
  } else if (intel_gfx_detected()) {
    printk(KERN_INFO "  GPU: %s detected%s\n", intel_gfx_get_name(),
           intel_gfx_is_supported_device()
               ? " but native Intel bring-up is not active"
               : " in framebuffer compatibility mode");
    if (intel_gfx_is_using_framebuffer_fallback()) {
      printk(KERN_INFO
             "  GPU: Default framebuffer fallback remains active for this Intel GPU\n");
    }
  }

  pci_device_t *gpu = pci_find_device(0x1AF4, 0x1050); /* virtio-gpu */
  if (!generic_drivers_only && gpu) {
    if (virtio_gpu_init(gpu) == 0) {
      printk(KERN_INFO "  GPU: virtio-gpu initialized with 3D acceleration\n");
    } else {
      printk(KERN_INFO "  GPU: virtio-gpu init failed\n");
    }
  } else if (generic_drivers_only && gpu) {
    printk(KERN_INFO "  GPU: generic driver mode skipping virtio-gpu acceleration\n");
  } else if (!intel_gfx_detected()) {
    printk(KERN_INFO "  GPU: No virtio-gpu found (software rendering)\n");
  }

  /* Initialize GUI windowing system */
  printk(KERN_INFO "  Initializing GUI...\n");
  extern int gui_init(uint32_t *framebuffer, uint32_t width, uint32_t height,
                      uint32_t pitch);
  extern struct window *gui_create_window(const char *title, int x, int y,
                                          int w, int h);
  extern void gui_compose(void);
  extern void gui_draw_cursor(void);
  extern int gui_needs_redraw(void);

  uint32_t *fb_buffer;
  uint32_t fb_width, fb_height;
  uint32_t fb_pitch;
  fb_get_info(&fb_buffer, &fb_width, &fb_height);
  fb_pitch = fb_get_pitch();

  if (fb_buffer) {
    gui_init(fb_buffer, fb_width, fb_height, fb_pitch);

    /* Create demo windows with working terminal */
    extern struct window *gui_create_file_manager(int x, int y);

    /* Create and focus a visible terminal window so keyboard input has a target. */
    {
      extern struct terminal *term_create(int x, int y, int cols, int rows);
      extern void term_set_active(struct terminal *term);
      extern void term_set_content_pos(struct terminal *term, int x, int y);
      extern void gui_set_window_userdata(struct window *win, void *data);
      extern void gui_focus_window(struct window *win);

      struct window *term_win = gui_create_window("Terminal", 50, 50, 450, 320);
      if (term_win) {
        int content_x = 50 + 2;
        int content_y = 50 + 28;
        struct terminal *term = term_create(content_x, content_y, 55, 16);
        if (term) {
          gui_set_window_userdata(term_win, term);
          term_set_active(term);
          term_set_content_pos(term, content_x, content_y);
          gui_focus_window(term_win);
        }
      }
    }

  }
  gui_refresh_hardware_acceleration_policy();

#ifdef ARCH_X86_64
  {
    extern const char *limine_get_kernel_cmdline(void);
    const char *cmdline = limine_get_kernel_cmdline();

    printk(KERN_INFO "GUI: Kernel cmdline at GUI start: %s\n",
           cmdline ? cmdline : "(null)");
    if (cmdline_has_token(cmdline, "resolution-self-test")) {
      printk(KERN_INFO "GUI: Boot cmdline requested resolution self-test\n");
      if (gui_run_resolution_self_test() != 0) {
        printk(KERN_WARNING "GUI: Resolution self-test reported failure\n");
      }
    }
  }
#endif

  printk(KERN_INFO "  Loading keyboard driver...\n");
  printk(KERN_INFO "  Loading NVMe driver...\n");
  printk(KERN_INFO "  Loading USB driver...\n");
  printk(KERN_INFO "  Loading network driver...\n");
  printk(KERN_INFO "  Loading Wi-Fi drivers...\n");
  extern void tcpip_init(void);
  extern int virtio_net_init(void);
  extern int vbox_net_init(void);
  tcpip_init();
  if (generic_drivers_only) {
    printk(KERN_INFO
           "  Generic driver mode skipping virtio-net, VBox net, and Wi-Fi drivers\n");
  } else {
    virtio_net_init();
    vbox_net_init();
    wifi_init();
  }

  if (fb_buffer) {
    /* Refresh the framebuffer-backed desktop after the early boot log screen. */
    gui_compose();
    gui_draw_cursor();
    printk(KERN_INFO "  GUI desktop ready!\n");
  }

  /*
   * Notify the GUI only after the first desktop compose is complete.
   * This avoids racing early storage and startup-flow setup with the
   * initial x86_64 desktop bring-up path.
   */
  gui_notify_storage_ready();

  /* ================================================================= */
  /* Phase 6: Enable Interrupts */
  /* ================================================================= */

  printk(KERN_INFO "[INIT] Enabling interrupts...\n");
#ifdef ARCH_X86_64
  /*
   * Real hardware x86_64 bring-up is still using a conservative polling path.
   * Enabling IRQs here can reset the machine immediately after the first
   * desktop frame, so leave interrupts disabled until the legacy IRQ path is
   * stabilized on real systems.
   */
  printk(KERN_WARNING
         "x86_64: leaving interrupts disabled on the desktop bring-up path\n");
#else
  /* Enable interrupts */
  arch_irq_enable();
#endif
  //printk(KERN_INFO
   //      "[INIT] Waiting 1 second after disk initialization before continuing boot...\n");
  //pit_sleep(1000);

  printk(KERN_INFO "[INIT] Kernel initialization complete!\n\n");
  kintegrity_mark_phase(KINTEGRITY_PHASE_READY, "kernel ready");
}

/*
 * start_init_process - Start the first userspace process (PID 1)
 */

#define GUI_KEY_QUEUE_SIZE 256
static volatile int g_gui_key_queue[GUI_KEY_QUEUE_SIZE];
static volatile int g_gui_key_r = 0;
static volatile int g_gui_key_w = 0;

/* Keyboard callback wrapper */
/* Keyboard callback wrapper */
static void keyboard_handler(int key) {
  /* gui_handle_key_event is now called via gui_key_callback, not here */

  /*
   * Send only canonical text/control keys to the KAPI input buffer.
   * Navigation/meta virtual keys are handled by the GUI callback path.
   */
  extern void kapi_sys_key_event(int key);
  if ((key >= 32 && key <= 126) || key == '\n' || key == '\r' || key == '\t' ||
      key == '\b' || key == 27) {
    kapi_sys_key_event(key);
  }
}

static void keyboard_gui_handler(int key) {
  int next;

  if (key < 0 || key > 0x1FF)
    return;

  next = (g_gui_key_w + 1) % GUI_KEY_QUEUE_SIZE;
  if (next == g_gui_key_r) {
    /* Drop newest key if full; keep system responsive and non-crashing. */
    return;
  }

  g_gui_key_queue[g_gui_key_w] = key;
  g_gui_key_w = next;
}

static int gui_key_queue_pop(int *key_out) {
  int key;

  if (!key_out || g_gui_key_r == g_gui_key_w)
    return 0;

  key = g_gui_key_queue[g_gui_key_r];
  g_gui_key_r = (g_gui_key_r + 1) % GUI_KEY_QUEUE_SIZE;
  *key_out = key;
  return 1;
}

static void start_init_process(void) {
#ifdef ARCH_X86_64
  printk(KERN_INFO
         "x86_64: keeping the desktop loop for now; /sbin/init stays on the ARM64 userspace path\n");
#else
  /* Create and start init process asynchronously */
  printk(KERN_INFO "Spawning /sbin/init...\n");

  extern int process_create(const char *path, int argc, char **argv);
  extern int process_start(int pid);

  char *argv[] = {"/sbin/init", NULL};
  int pid = process_create("/sbin/init", 1, argv);
  if (pid > 0) {
    process_start(pid);
    printk(KERN_INFO "Started init process (pid %d)\n", pid);
  } else {
    printk(KERN_ERR "Failed to start /sbin/init\n");
  }
#endif

  printk(KERN_INFO "System ready.\n\n");

  /* Set up input handling */
  extern int input_init(void);
  extern void input_poll(void);
  extern void virtio_net_poll(void);
  extern void vbox_net_poll(void);
  extern void input_set_key_callback(void (*callback)(int key));
  extern void input_set_gui_key_callback(void (*callback)(int key));
  extern void gui_compose(void);
  extern void gui_draw_cursor(void);
  extern int gui_needs_redraw(void);

  input_init();
  g_gui_key_r = 0;
  g_gui_key_w = 0;

  /* Connect keyboard input to terminal */
  input_set_key_callback(keyboard_handler);
  input_set_gui_key_callback(keyboard_gui_handler);

  printk(KERN_INFO "GUI: Event loop started - type in terminal!\\n");

  /* Initial render */
  gui_compose();
  gui_draw_cursor();

#if CONFIG_INSTALLER_APP
  {
    extern int boot_is_installer_mode(void);
    extern int app_run(const char *name, int argc, char **argv);

    if (boot_is_installer_mode()) {
      printk(KERN_INFO
             "INSTALL: launching compiler-flagged installer app\n");
      app_run("installer", 0, 0);
    }
  }
#endif

  /* Main GUI event loop with proper flicker-free refresh */
  uint32_t frame = 0;
  int last_mx = 0, last_my = 0;
  int last_buttons = 0;
  uint64_t last_kernel_slice_ms = arch_timer_get_ms();
  uint64_t last_usb_scan_ms = last_kernel_slice_ms;
  /*
   * Keep background slices aligned with the desktop frame cadence so
   * cursor movement and drawing stay smooth under load.
   */
  const uint64_t KERNEL_SLICE_MS = 16;
  const uint64_t USB_SCAN_MS = 250;
  gui_frame_profile_t frame_profile = {0};

  {
    extern void mouse_get_position(int *x, int *y);
    extern int mouse_get_buttons(void);

    mouse_get_position(&last_mx, &last_my);
    last_buttons = mouse_get_buttons();
    if (last_buttons < 0)
      last_buttons = 0;
    last_buttons &= 0x1F;
  }

  while (1) {
    uint64_t frame_start_us = gui_monotonic_us();
    uint64_t step_start_us = frame_start_us;

    gui_desktop_frame_profiler_clear_notes();
    frame_profile.input_poll_us = 0;
    frame_profile.net_poll_us = 0;
    frame_profile.uart_key_us = 0;
    frame_profile.queued_keys_us = 0;
    frame_profile.mouse_us = 0;
    frame_profile.compose_us = 0;
    frame_profile.kernel_slice_us = 0;
    frame_profile.wait_next_frame_us = 0;
    frame_profile.total_us = 0;

    /* Poll input devices once per iteration. */
    input_poll();
    frame_profile.input_poll_us = profile_split_us(&step_start_us);

    virtio_net_poll();
    vbox_net_poll();
    frame_profile.net_poll_us = profile_split_us(&step_start_us);

    {
      extern void xhci_poll_ports(void);
      uint64_t now_for_usb = arch_timer_get_ms();
      if (now_for_usb - last_usb_scan_ms >= USB_SCAN_MS) {
        xhci_poll_ports();
        last_usb_scan_ms = now_for_usb;
      }
    }

    /* Poll for keyboard input from UART as well */
    extern int uart_getc_nonblock(void);
    extern void gui_handle_key_event(int key);
    int c = uart_getc_nonblock();
    if (c >= 0) {
      /* Route to focused window */
      gui_handle_key_event(c);
    }
    frame_profile.uart_key_us = profile_split_us(&step_start_us);

    {
      int queued_key;
      while (gui_key_queue_pop(&queued_key)) {
        gui_handle_key_event(queued_key);
      }
    }
    frame_profile.queued_keys_us = profile_split_us(&step_start_us);

    /* Get mouse state (updated by input_poll) */
    extern void mouse_get_position(int *x, int *y);
    extern int mouse_get_buttons(void);
    extern void gui_handle_mouse_event(int x, int y, int buttons);

    int mx, my;
    mouse_get_position(&mx, &my);
    int mbuttons = mouse_get_buttons();
    static int warned_bad_mouse_buttons = 0;
    if (mbuttons < 0) {
      if (!warned_bad_mouse_buttons) {
        printk(KERN_WARNING "INPUT: Ignoring invalid mouse buttons value %d\n",
               mbuttons);
        warned_bad_mouse_buttons = 1;
      }
      mbuttons = 0;
    }
    mbuttons &= 0x1F;

    /* Check if mouse changed */
    if (mx != last_mx || my != last_my || mbuttons != last_buttons) {
      /* Always call mouse event handler for hover support */
      gui_handle_mouse_event(mx, my, mbuttons);

      last_mx = mx;
      last_my = my;
      last_buttons = mbuttons;
    }
    frame_profile.mouse_us = profile_split_us(&step_start_us);

    {
      extern void gui_installer_background_tick(void);
      gui_installer_background_tick();
    }

    if (gui_needs_redraw()) {
      uint64_t compose_start_us = gui_monotonic_us();
      gui_compose(); /* Cursor is drawn inside compose, before blit */
      frame_profile.compose_us = gui_monotonic_us() - compose_start_us;
    }

    {
      uint64_t now_for_slice = arch_timer_get_ms();
      if (now_for_slice - last_kernel_slice_ms >= KERNEL_SLICE_MS) {
        extern int process_run_kernel_slice(void);
        uint64_t slice_start_us = gui_monotonic_us();
        if (process_run_kernel_slice()) {
          last_kernel_slice_ms = now_for_slice;
        }
        kintegrity_periodic();
        frame_profile.kernel_slice_us =
            gui_monotonic_us() - slice_start_us;
      }
    }

    frame++;
    (void)frame;

    /* Check if we should yield to let userspace run */
    /* If no input events processed, yield CPU */
    extern void process_schedule_from_irq(void); // Or just wait for IRQ?
    // User processes run preemptively via timer IRQ, so we just loop here
    // But we should yield to be nice if not rendering

    /* Short yield - allows input polling without slowing mouse */
    uint64_t wait_start_us = gui_monotonic_us();
    for (volatile int i = 0; i < 500; i++) {
    }
    frame_profile.wait_next_frame_us = gui_monotonic_us() - wait_start_us;
    frame_profile.total_us = gui_monotonic_us() - frame_start_us;
    gui_desktop_frame_profiler_submit(&frame_profile);
  }
}

/*
 * panic - Halt the system with an error message
 * @msg: Error message to display
 */
void panic_with_context(const char *msg, uintptr_t caller_hint,
                        uintptr_t stack_hint) {
  static int panic_in_progress = 0;

  /* Disable interrupts */
  arch_irq_disable();

  if (panic_in_progress) {
    printk(KERN_EMERG "\n");
    printk(KERN_EMERG "Recursive kernel panic detected!\n");
    printk(KERN_EMERG "Latest panic: %s\n", msg ? msg : "(null)");
    printk(KERN_EMERG "Caller: 0x%lx Stack: 0x%lx\n", (unsigned long)caller_hint,
           (unsigned long)stack_hint);
    panic_halt_forever();
  }
  panic_in_progress = 1;
  kernel_panic_fence_active = 1;

  printk(KERN_EMERG "\n");
  printk(KERN_EMERG "============================================\n");
  printk(KERN_EMERG "KERNEL PANIC!\n");
  printk(KERN_EMERG "============================================\n");
  printk(KERN_EMERG "%s\n", msg ? msg : "(null)");
  printk(KERN_EMERG "Build UUID: %s\n", BUILD_UUID);
  printk(KERN_EMERG "Arch: %s\n", ARCH_NAME);
  printk(KERN_EMERG "Caller: 0x%lx\n", (unsigned long)caller_hint);
  printk(KERN_EMERG "Stack: 0x%lx\n", (unsigned long)stack_hint);
  printk(KERN_EMERG "Kernel start: 0x%lx\n", (unsigned long)(uintptr_t)__kernel_start);
  printk(KERN_EMERG "Kernel end: 0x%lx\n", (unsigned long)(uintptr_t)__kernel_end);
  printk(KERN_EMERG "============================================\n");
  printk(KERN_EMERG "Rendering panic screen...\n");

  panic_draw_screen(msg, caller_hint, stack_hint);

  printk(KERN_EMERG "System halted.\n");

  panic_halt_forever();
}

void panic(const char *msg) {
  uintptr_t caller_hint = (uintptr_t)__builtin_return_address(0);
  uintptr_t stack_hint = (uintptr_t)&msg;
  panic_with_context(msg, caller_hint, stack_hint);
}
