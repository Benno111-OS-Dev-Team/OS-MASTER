/*
 * OS8 - Boot Manager
 *
 * Configurable boot with text logging and boot target selection.
 */

#include "bootmanager.h"
#include "printk.h"

#define BOOT_TIMEOUT_DEFAULT 5
#define BOOT_TARGET_KERNEL 0
#define BOOT_TARGET_RECOVERY 1
#define BOOT_TARGET_SHELL 2
#define MAX_BOOT_ENTRIES 8

struct boot_entry {
  char name[64];
  char path[128];
  char cmdline[256];
  bool is_default;
};

static struct boot_config boot_cfg = {
    .magic = 0xB007C0DE,
    .version = 1,
    .timeout_seconds = BOOT_TIMEOUT_DEFAULT,
    .default_target = BOOT_TARGET_KERNEL,
    .show_splash = false,
    .verbose_boot = false,
    .debug_mode = false,
    .kernel_cmdline = "console=ttyS0 rootwait",
    .default_kernel = "/boot/vib-os.elf",
    .recovery_kernel = "/boot/recovery.elf",
    .splash_bg_color = 0x1A1A2E,
    .splash_fg_color = 0xE94560,
    .progress_color = 0x16213E,
};
static bool boot_from_usb = false;
static bool boot_live_media = false;
static bool boot_installer_mode = false;
static bool boot_xhci_enabled = false;
static struct boot_entry boot_entries[MAX_BOOT_ENTRIES];
static int num_boot_entries = 0;
static progress_callback_t boot_progress_cb = NULL;

static int str_contains_token(const char *haystack, const char *needle) {
  int i = 0;
  int nlen = 0;
  while (needle[nlen])
    nlen++;
  if (!nlen)
    return 0;

  while (haystack && haystack[i]) {
    int j = 0;
    while (needle[j] && haystack[i + j] == needle[j])
      j++;
    if (j == nlen)
      return 1;
    i++;
  }
  return 0;
}

static void draw_progress_bar(int percent) {
  const int bar_width = 40;
  int filled = (bar_width * percent) / 100;

  printk("\r  [");
  for (int i = 0; i < bar_width; i++) {
    if (i < filled) {
      printk("=");
    } else if (i == filled) {
      printk(">");
    } else {
      printk(" ");
    }
  }
  printk("] %3d%%", percent);
}

int boot_add_entry(const char *name, const char *path, const char *cmdline) {
  struct boot_entry *entry;

  if (num_boot_entries >= MAX_BOOT_ENTRIES)
    return -1;

  entry = &boot_entries[num_boot_entries++];
  for (int i = 0; i < 63 && name[i]; i++) {
    entry->name[i] = name[i];
    entry->name[i + 1] = '\0';
  }
  for (int i = 0; i < 127 && path[i]; i++) {
    entry->path[i] = path[i];
    entry->path[i + 1] = '\0';
  }
  for (int i = 0; i < 255 && cmdline[i]; i++) {
    entry->cmdline[i] = cmdline[i];
    entry->cmdline[i + 1] = '\0';
  }
  entry->is_default = (num_boot_entries == 1);
  return 0;
}

void boot_set_progress_callback(progress_callback_t cb) {
  boot_progress_cb = cb;
}

void boot_report_progress(const char *stage, int percent) {
  if (stage && stage[0]) {
    if (percent >= 0)
      printk(KERN_INFO "BOOT: %-32s %3d%%\n", stage, percent);
    else
      printk(KERN_INFO "BOOT: %s\n", stage);
  } else {
    printk(KERN_INFO "BOOT: progress %d%%\n", percent);
  }

  if (boot_cfg.verbose_boot)
    draw_progress_bar(percent);

  if (boot_progress_cb)
    boot_progress_cb(stage, percent);
}

int boot_show_menu(void) {
  printk("\n");
  printk("  Boot Menu\n");
  printk("  =========\n\n");

  for (int i = 0; i < num_boot_entries; i++) {
    printk("    %d) %s%s\n", i + 1, boot_entries[i].name,
           boot_entries[i].is_default ? " [default]" : "");
  }

  printk("\n");
  printk("  Press 1-%d to select, or ENTER for default\n", num_boot_entries);
  printk("  Booting default in %d seconds...\n\n", boot_cfg.timeout_seconds);

  for (int i = 0; i < num_boot_entries; i++) {
    if (boot_entries[i].is_default)
      return i;
  }

  return 0;
}

void boot_init(void) {
  printk(KERN_INFO "BOOT: Initializing boot manager\n");

  boot_add_entry("OS8", boot_cfg.default_kernel,
                 boot_cfg.kernel_cmdline);
  boot_add_entry("Recovery Mode", boot_cfg.recovery_kernel, "single recovery");
  boot_add_entry("Debug Shell", "/bin/sh", "debug");

  printk(KERN_INFO "BOOT: %d boot entries configured\n", num_boot_entries);
}

struct boot_config *boot_get_config(void) { return &boot_cfg; }

void boot_set_timeout(uint32_t seconds) { boot_cfg.timeout_seconds = seconds; }

void boot_set_default(int target) {
  for (int i = 0; i < num_boot_entries; i++)
    boot_entries[i].is_default = (i == target);
  boot_cfg.default_target = target;
}

void boot_parse_cmdline(const char *cmdline) {
  const char *p;

  if (!cmdline)
    cmdline = "";

  for (int i = 0; i < 255 && cmdline[i]; i++) {
    boot_cfg.kernel_cmdline[i] = cmdline[i];
    boot_cfg.kernel_cmdline[i + 1] = '\0';
  }
  if (!cmdline[0])
    boot_cfg.kernel_cmdline[0] = '\0';

  boot_from_usb = false;
  boot_live_media = false;
  boot_installer_mode = false;
  boot_xhci_enabled = false;
  if (str_contains_token(cmdline, "boot=usb") ||
      str_contains_token(cmdline, "usbboot") ||
      str_contains_token(cmdline, "root=/dev/sd") ||
      str_contains_token(cmdline, "root=/dev/usb")) {
    boot_from_usb = true;
  }
  if (str_contains_token(cmdline, "boot=cd") ||
      str_contains_token(cmdline, "boot=live") ||
      str_contains_token(cmdline, "livecd") ||
      str_contains_token(cmdline, "livemedia") ||
      str_contains_token(cmdline, "cdrom") ||
      str_contains_token(cmdline, "root=/dev/cd")) {
    boot_live_media = true;
  }
  if (str_contains_token(cmdline, "installer") ||
      str_contains_token(cmdline, "install") ||
      str_contains_token(cmdline, "mode=installer")) {
    boot_installer_mode = true;
  }
  if (str_contains_token(cmdline, "xhci=on") ||
      str_contains_token(cmdline, "xhci=1") ||
      str_contains_token(cmdline, "usb.xhci=on")) {
    boot_xhci_enabled = true;
  }
  if (str_contains_token(cmdline, "xhci=off") ||
      str_contains_token(cmdline, "xhci=0") ||
      str_contains_token(cmdline, "usb.xhci=off")) {
    boot_xhci_enabled = false;
  }

  p = cmdline;
  while (*p) {
    if (p[0] == 'v' && p[1] == 'e' && p[2] == 'r' && p[3] == 'b')
      boot_cfg.verbose_boot = true;
    if (p[0] == 'd' && p[1] == 'e' && p[2] == 'b' && p[3] == 'u')
      boot_cfg.debug_mode = true;
    if (p[0] == 'n' && p[1] == 'o' && p[2] == 's' && p[3] == 'p')
      boot_cfg.show_splash = false;
    p++;
  }

  printk(
      KERN_INFO
      "BOOT: Cmdline parsed - verbose=%d debug=%d splash=%d usb=%d installer=%d xhci=%d\n",
      boot_cfg.verbose_boot, boot_cfg.debug_mode, boot_cfg.show_splash,
      boot_from_usb, boot_installer_mode, boot_xhci_enabled);
}

int boot_is_usb_boot(void) { return boot_from_usb ? 1 : 0; }

int boot_is_live_media(void) { return boot_live_media ? 1 : 0; }

int boot_is_installer_mode(void) { return boot_installer_mode ? 1 : 0; }

int boot_allow_xhci(void) { return boot_xhci_enabled ? 1 : 0; }

int boot_should_show_splash(void) { return boot_cfg.show_splash ? 1 : 0; }

void boot_force_verbose_console(void) {
  boot_cfg.verbose_boot = true;
  boot_cfg.show_splash = false;
  printk(KERN_INFO "BOOT: verbose console forced by keyboard override\n");
}
