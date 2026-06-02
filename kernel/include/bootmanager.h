#ifndef BOOTMANAGER_H
#define BOOTMANAGER_H

#include "types.h"

struct boot_config {
  uint32_t magic;
  uint32_t version;
  uint32_t timeout_seconds;
  uint32_t default_target;
  bool show_splash;
  bool verbose_boot;
  bool debug_mode;
  char kernel_cmdline[256];
  char default_kernel[128];
  char recovery_kernel[128];
  uint32_t splash_bg_color;
  uint32_t splash_fg_color;
  uint32_t progress_color;
};

typedef void (*progress_callback_t)(const char *stage, int percent);

int boot_add_entry(const char *name, const char *path, const char *cmdline);
void boot_set_progress_callback(progress_callback_t cb);
void boot_report_progress(const char *stage, int percent);
int boot_show_menu(void);
void boot_init(void);
struct boot_config *boot_get_config(void);
void boot_set_timeout(uint32_t seconds);
void boot_set_default(int target);
void boot_parse_cmdline(const char *cmdline);
int boot_is_usb_boot(void);
int boot_is_live_media(void);
int boot_is_installer_mode(void);
int boot_allow_xhci(void);
int boot_should_show_splash(void);
void boot_force_verbose_console(void);

#endif
