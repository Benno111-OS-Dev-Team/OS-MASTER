/*
 * OS8 - GUI Windowing System
 *
 * Complete window manager with compositor and widget toolkit.
 */

#include "build_uuid.h"
#include "arch/arch.h"
#include "desktop.h"         /* Desktop manager */
#include "gui/gui.h"
#include "drivers/wifi.h"
#include "drivers/pci.h"
#include "dock_icons.h"      /* Dock icons (PNG-based) */
#include "drivers/uart.h"
#include "fs/vfs.h"          /* VFS headers */
#include "icons.h"           /* Icon bitmaps */
#include "drivers/storage.h"
#include "media/media.h"
#include "media/seed_assets.h"
#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "printk.h"
#include "password_hash.h"
#include "toolbar_icons.h" /* Toolbar icons for image viewer */
#include "string.h"
#include "types.h"
#include "window_skin.h"

#ifndef BUILD_STRING
#define BUILD_STRING "unknown build"
#endif

#ifndef BUILD_BRANCH
#define BUILD_BRANCH "unknown"
#endif

#ifndef BUILD_COMPILE_TIME
#define BUILD_COMPILE_TIME "unknown"
#endif

#define GUI_DISPLAY_CONFIG_PATH "/System/display.cfg"
#define GUI_THEME_CONFIG_PATH "/System/theme.cfg"
#define GUI_THEME_DARK_PATH "/assets/themes/dark.theme"
#define GUI_THEME_LIGHT_PATH "/assets/themes/light.theme"
#define GUI_ACTIVATION_PATH "/System/activation.cfg"
#define GUI_ACTIVATION_GRACE_DAYS 30

struct window *gui_create_file_manager(int x, int y);
struct window *gui_create_file_manager_path(int x, int y, const char *path);
void gui_open_notepad(const char *path);
int gui_launch_app_by_id(const char *app_id);
extern int bochs_init(uint32_t width, uint32_t height);
extern void bochs_get_info(uint32_t **buffer, uint32_t *width, uint32_t *height);
extern void input_set_mouse_bounds(int width, int height);

/* Forward declarations for drawing helpers used before their definitions. */
void gui_draw_rect(int x, int y, int w, int h, uint32_t color);
void gui_draw_rect_outline(int x, int y, int w, int h, uint32_t color,
                           int thickness);
void gui_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void gui_draw_circle(int cx, int cy, int r, uint32_t color, bool filled);
void gui_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void gui_draw_string(int x, int y, const char *str, uint32_t fg, uint32_t bg);
int gui_draw_system_app_icon(const char *app_id, int x, int y, int size);
static void gui_fill_rect_alpha(int x, int y, int w, int h, uint32_t color);
static void gui_draw_glass_panel(int x, int y, int w, int h, uint32_t tint,
                                 uint32_t glow, uint32_t border,
                                 int blur_stride);
static uint32_t gui_make_opaque_color(uint32_t color);
static void gui_fill_visual_effect_rect(int x, int y, int w, int h,
                                        uint32_t color);
static uint32_t gui_contrast_title_color(uint32_t rgb);
static void draw_rounded_rect(int x, int y, int w, int h, int r,
                              uint32_t color);
static void draw_top_rounded_rect_alpha(int x, int y, int w, int h, int r,
                                        uint32_t color);
static void draw_filled_circle(int cx, int cy, int r, uint32_t color);
static int startup_flow_active(void);
static int startup_setup_account_active(void);
static int startup_setup_welcome_active(void);
static int startup_setup_account_form_active(void);
static int startup_setup_storage_active(void);
static void startup_close_other_windows(void);
static void startup_get_setup_layout(int content_x, int content_y, int content_w,
                                     int content_h, int *panel_x,
                                     int *panel_y, int *panel_w, int *panel_h,
                                     int *rail_w, int *card_x, int *card_y,
                                     int *card_w, int *card_h);
static void startup_get_setup_button_rect(int content_x, int content_y,
                                          int content_w, int content_h, int *x,
                                          int *y, int *w, int *h);
static void startup_get_setup_field_rect(int content_x, int content_y,
                                         int content_w, int content_h,
                                         int field_index, int *x, int *y,
                                         int *w, int *h);
static void installer_refresh_disk_inventory(void);
static const char *installer_selected_disk_label(void);
static int installer_write_target_config(void);
static void installer_append_to_buf(char *buf, int max, const char *text);
static void installer_log(const char *line);
static int load_install_target_disk_location(char *buf, int max);
static int installer_journal_install_write(const char *path,
                                           const uint8_t *data, size_t size);
static const char *installed_system_bootable_cfg(void);
static const char *installed_system_bios_bootable_cfg(void);
static const char *installed_system_installer_state(int first_boot_setup);
static const char *installed_system_efi_boot_cfg(void);
static const char *installed_system_mbr_boot_cfg(void);
static int installer_refresh_bootloader_state(const char *target_root,
                                              int first_boot_setup);
static void open_partition_manager_window(int x, int y);
static void draw_partition_manager_window(int content_x, int content_y,
                                          int content_w, int content_h);
static void partition_manager_refresh_partitions(void);
static void installer_ensure_parent_dirs(const char *path);
static int write_text_file(const char *path, const char *content);
static int read_text_file(const char *path, char *buf, int max);
static int manifest_get_value(const char *manifest, const char *key, char *out,
                              int out_max);
static uint64_t parse_u64(const char *text);
static void gui_refresh_activation_state(void);
static int gui_activation_requires_sign_in(void);
static const char *gui_activation_status_text(void);
static const char *gui_activation_detail_text(void);
static void append_decimal(char *buf, int *idx, int value);
static void notepad_append_to_buf(char *dst, int max, const char *src);
static int installer_selected_disk_index(void);
static void installer_fail_background(const char *status, const char *log_line);
static void installer_start_background_install(void);
static void installer_process_autorun(void);
static void installer_process_background_install(void);
static void gui_flush_account_state_before_power_transition(void);
static void str_copy_safe(char *dst, const char *src, int max);
static int str_cmp(const char *s1, const char *s2);
static int str_ends_with_ci(const char *name, const char *ext);
static void invalidate_desktop_surface(void);
static const char *resolve_user_storage_path(const char *path, char *buf,
                                             int max);
static void ensure_user_storage_dirs(void);
static int user_storage_mkdir(const char *path, mode_t mode);
static int user_storage_unlink(const char *path);
static int user_storage_rmdir(const char *path);
static int user_storage_rename(const char *old_path, const char *new_path);
static int path_is_active_account_home(const char *path);
static int path_is_active_account_home_root(const char *path);
static void mount_active_user_home(void);
static void unmount_active_user_home(void);
static int read_account_manifest(const char *username, char *manifest, int max);
static int parse_account_manifest(const char *manifest, const char *fallback_name,
                                  char *username, int username_max,
                                  char *password_hash, int password_hash_max,
                                  char *role, int role_max,
                                  int *wallpaper_index,
                                  char *partition_label, int partition_label_max,
                                  char *disk_location, int disk_location_max);
static void account_unix_home_path(const char *username, char *buf, int max);
static int account_write_unix_databases(void);
static void runtime_sync_log_line(const char *line);
static void runtime_sync_flush_best_effort(const char *path);
void gui_open_image_viewer(const char *path);
static void gui_play_mp3_file(const char *path);
void compositor_mark_dirty(int x, int y, int w, int h);
void compositor_mark_full_redraw(void);
void gui_invalidate_rect(int x, int y, int w, int h);
void gui_invalidate_screen(void);
void gui_set_blur_effects_enabled(int enabled);
int gui_blur_effects_requested(void);
int gui_are_blur_effects_enabled(void);
int gui_is_gpu_rendering_enabled(void);
int gui_can_apply_resolution_live(void);
int gui_is_supported_resolution(uint32_t width, uint32_t height);
int gui_get_resolution_option_count(void);
int gui_get_resolution_option(int index, uint32_t *width, uint32_t *height,
                              const char **label);
int gui_set_resolution(uint32_t width, uint32_t height);
int gui_save_resolution_preference(uint32_t width, uint32_t height);
void gui_start_partial_redraw_clear_debug(void);
int gui_partial_redraw_clear_debug_enabled(void);
static const char *gui_blur_effects_button_label(void);
static const char *gui_blur_effects_status_label(void);
static const char *gui_blur_effects_detail_message(void);
static void gui_cycle_blur_effects_mode(void);
static void compositor_mark_screen_rect_dirty(void);
static inline void fast_memcpy_line(uint32_t *dst, uint32_t *src, int width);
static inline void fast_fill_line(uint32_t *dst, int width, uint32_t color);

/* Blur/compositor state is defined later but used by early draw helpers. */
typedef enum {
  GUI_BLUR_EFFECTS_OFF = 0,
  GUI_BLUR_EFFECTS_AUTO = 1,
  GUI_BLUR_EFFECTS_FORCE = 2
} gui_blur_effects_mode_t;

static gui_blur_effects_mode_t g_blur_effects_mode;
static int g_blur_effects_enabled;
static int g_partial_redraw_clear_debug_frames;
static char g_gpu_backend_name[32];

typedef struct {
  int activated;
  int grace_expired;
  int clock_valid;
  uint64_t first_use_day;
  uint64_t current_day;
  int days_used;
  int days_remaining;
  char key[32];
  char status[96];
  char detail[128];
} gui_activation_state_t;

static gui_activation_state_t g_activation_state;


/* Terminal functions from terminal.c */
struct terminal;
extern struct terminal *term_get_active(void);
extern struct terminal *term_create(int x, int y, int cols, int rows);
extern void term_set_active(struct terminal *term);
extern void term_handle_key(struct terminal *term, int key);
extern int term_get_input_len(struct terminal *t);
extern char term_get_input_char(struct terminal *t, int idx);
extern void term_render(struct terminal *term);
extern void term_set_content_pos(struct terminal *t, int x, int y);

/* ===================================================================== */
/* Display and Color */
/* ===================================================================== */

#define COLOR_BLACK 0x000000
#define COLOR_WHITE 0xFFFFFF
#define COLOR_RED 0xFF0000
#define COLOR_GREEN 0x00FF00
#define COLOR_BLUE 0x0000FF
#define COLOR_GRAY 0x808080
#define COLOR_DARK_GRAY 0x404040
#define COLOR_LIGHT_GRAY 0xC0C0C0

typedef enum {
  GUI_THEME_DARK = 0,
  GUI_THEME_LIGHT = 1
} gui_theme_mode_t;

typedef struct {
  uint32_t app_bg;
  uint32_t app_fg;
  uint32_t app_muted;
  uint32_t accent;
  uint32_t accent_soft;
  uint32_t surface;
  uint32_t surface_alt;
  uint32_t card;
  uint32_t border;
  uint32_t title_text;
  uint32_t title_glow;
  uint32_t title_tint_focused;
  uint32_t title_tint_inactive;
  uint32_t title_veil_focused;
  uint32_t title_veil_inactive;
  uint32_t title_line_top;
  uint32_t title_line_bottom;
  uint32_t window_glass_focused;
  uint32_t window_glass_inactive;
  uint32_t window_glow_focused;
  uint32_t window_glow_inactive;
  uint32_t dock_fill;
  uint32_t dock_top_line;
  uint32_t dock_top_shadow;
  uint32_t dock_bottom_line;
  uint32_t dock_launcher;
  uint32_t dock_launcher_hover;
  uint32_t dock_launcher_active;
  uint32_t dock_launcher_border;
  uint32_t menu_tint;
  uint32_t menu_glow;
  uint32_t menu_border;
  uint32_t menu_title;
  uint32_t menu_left;
  uint32_t menu_right;
  uint32_t menu_divider;
  uint32_t settings_bg;
  uint32_t settings_panel;
  uint32_t settings_border;
  uint32_t settings_text;
  uint32_t settings_subtext;
  uint32_t settings_tab_active;
  uint32_t settings_tab_inactive;
  uint32_t settings_card;
  uint32_t settings_card_text;
  uint32_t settings_card_subtext;
  uint32_t about_bg;
  uint32_t about_hero;
  uint32_t about_card;
  uint32_t about_text;
  uint32_t about_subtext;
  uint32_t about_footer;
  uint32_t file_bg;
  uint32_t file_toolbar;
  uint32_t file_path;
  uint32_t file_sidebar;
  uint32_t file_list;
  uint32_t file_details;
  uint32_t file_header;
  uint32_t file_row_even;
  uint32_t file_row_odd;
  uint32_t file_row_selected;
  uint32_t file_text;
  uint32_t file_subtext;
} gui_theme_palette_t;

static gui_theme_palette_t g_theme_dark = {
    0x1A1A2E, 0xE4E4E7, 0xA1A1AA, 0x6366F1, 0xEC4899, 0x27272A, 0x1F2937,
    0x252535, 0x52525B, 0xFFFDFEFF, 0x90FFFFFF, 0x4A4E6A8A, 0x3F3D4A5D,
    0x18324860, 0x142A3442, 0x34FFFFFF, 0x50313C4E, 0x5A34383F, 0x50323338,
    0x26FFFFFF, 0x16FFFFFF, 0xA118202C, 0x72FFFFFF, 0x28495D78, 0x64060A10,
    0x2437455B, 0x36566F92, 0x5A78A9DA, 0x7EA7D8, 0x6A2C3446, 0x42FFFFFF,
    0x8C75839A, 0x344D6488, 0x141824, 0x1F2937, 0x30566C86, 0xF2F2F2, 0xF8F8F8,
    0x111111, 0x111111, 0x4A4A4A, 0x111111, 0xF2F2F2, 0x252535, 0xFFFFFF,
    0xCBD5E1, 0xEEF2F7, 0x1F2937, 0xFFFFFF, 0x111827, 0x6B7280, 0xE5E7EB,
    0x171A24, 0x111827, 0x1F2937, 0x111827, 0x0F172A, 0x111827, 0x172033,
    0x0F172A, 0x111827, 0x1D4ED8, 0xFFFFFF, 0x94A3B8};

static gui_theme_palette_t g_theme_light = {
    0xF4F7FB, 0x172033, 0x5F6E82, 0x2563EB, 0xDB2777, 0xE9EEF5, 0xF6F9FC,
    0xFFFFFF, 0xC9D4E5, 0x1A2535, 0x70486A8C, 0xD9E7F4FA, 0xC8D8E7F1,
    0x60FFFFFF, 0x4CEAF1F7, 0x88FFFFFF, 0x708BA0B5, 0x3CE7EDF6, 0x30D7DFEA,
    0x1EFFFFFF, 0x10FFFFFF, 0xCDE8EEF7, 0x88FFFFFF, 0x4CC9D7E6, 0x6094A3B5,
    0xD7E5F3, 0xE5EEF8, 0xC5DDFB, 0x8FB7E8, 0xCFE4F6, 0x54FFFFFF, 0x90B7C8DA,
    0xC3D9EE, 0xECF3FA, 0xF7FAFD, 0xD7E2EF, 0xF7F9FC, 0xFFFFFF, 0xC8D3E0,
    0x1B2430, 0x1B2430, 0x627084, 0x1B2430, 0xFFFFFF, 0x1B2430, 0x627084,
    0xF5F8FC, 0xE7F0FB, 0xFFFFFF, 0x18212E, 0x66758A, 0xDEE6F0, 0xEEF3F9,
    0xE2EAF3, 0xEEF3F9, 0xF7FAFD, 0xFFFFFF, 0xEEF3F9, 0xE3EBF5, 0xFFFFFF,
    0xF7FAFD, 0xDCE8F8, 0x18212E, 0x6B7A90};

static gui_theme_mode_t g_theme_mode = GUI_THEME_DARK;

static const gui_theme_palette_t *gui_theme_palette(void) {
  return g_theme_mode == GUI_THEME_LIGHT ? &g_theme_light : &g_theme_dark;
}

static void gui_set_theme_mode(gui_theme_mode_t mode) {
  g_theme_mode = mode == GUI_THEME_LIGHT ? GUI_THEME_LIGHT : GUI_THEME_DARK;
  compositor_mark_full_redraw();
}

static const char *gui_theme_mode_name(gui_theme_mode_t mode) {
  return mode == GUI_THEME_LIGHT ? "Light" : "Dark";
}

static int gui_theme_mode_from_text(const char *text) {
  if (!text)
    return -1;
  if (str_cmp(text, "light") == 0 || str_cmp(text, "Light") == 0)
    return GUI_THEME_LIGHT;
  if (str_cmp(text, "dark") == 0 || str_cmp(text, "Dark") == 0)
    return GUI_THEME_DARK;
  return -1;
}

static gui_theme_palette_t *gui_active_theme_palette(void) {
  return g_theme_mode == GUI_THEME_LIGHT ? &g_theme_light : &g_theme_dark;
}

static uint32_t *gui_theme_slot_ptr(gui_theme_palette_t *palette, int slot) {
  if (!palette)
    return NULL;
  switch (slot) {
  case 0:
    return &palette->app_bg;
  case 1:
    return &palette->app_fg;
  case 2:
    return &palette->accent;
  case 3:
    return &palette->accent_soft;
  case 4:
    return &palette->surface;
  case 5:
    return &palette->surface_alt;
  case 6:
    return &palette->card;
  case 7:
    return &palette->border;
  case 8:
    return &palette->settings_bg;
  case 9:
    return &palette->settings_panel;
  case 10:
    return &palette->settings_text;
  case 11:
    return &palette->settings_subtext;
  default:
    return NULL;
  }
}

static uint32_t gui_theme_slot_value(const gui_theme_palette_t *palette, int slot) {
  if (!palette)
    return 0;
  switch (slot) {
  case 0:
    return palette->app_bg;
  case 1:
    return palette->app_fg;
  case 2:
    return palette->accent;
  case 3:
    return palette->accent_soft;
  case 4:
    return palette->surface;
  case 5:
    return palette->surface_alt;
  case 6:
    return palette->card;
  case 7:
    return palette->border;
  case 8:
    return palette->settings_bg;
  case 9:
    return palette->settings_panel;
  case 10:
    return palette->settings_text;
  case 11:
    return palette->settings_subtext;
  default:
    return 0;
  }
}

static uint32_t gui_parse_hex_color(const char *text) {
  uint32_t value = 0;

  if (!text)
    return 0;
  if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    text += 2;
  if (*text == '#')
    text++;

  while (*text) {
    char c = *text++;
    uint32_t digit;

    if (c >= '0' && c <= '9')
      digit = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f')
      digit = 10U + (uint32_t)(c - 'a');
    else if (c >= 'A' && c <= 'F')
      digit = 10U + (uint32_t)(c - 'A');
    else
      break;

    value = (value << 4) | digit;
  }

  return value & 0x00FFFFFFU;
}

static void gui_append_hex_color(char *buf, int max, int *idx, uint32_t value) {
  static const char hex[] = "0123456789ABCDEF";

  if (!buf || !idx || max <= 0)
    return;

  if (*idx < max - 1)
    buf[(*idx)++] = '0';
  if (*idx < max - 1)
    buf[(*idx)++] = 'x';
  for (int shift = 20; shift >= 0 && *idx < max - 1; shift -= 4)
    buf[(*idx)++] = hex[(value >> shift) & 0xF];
}

static int gui_load_theme_palette(const char *path, gui_theme_palette_t *palette) {
  uint8_t *manifest_data = NULL;
  size_t manifest_size = 0;
  char manifest[768];
  char value[32];
  static const char *keys[] = {
      "app_bg",      "app_fg",      "accent",      "accent_soft",
      "surface",     "surface_alt", "card",        "border",
      "settings_bg", "settings_panel", "settings_text", "settings_subtext",
  };
  uint32_t *slots[] = {
      &palette->app_bg,      &palette->app_fg,      &palette->accent,
      &palette->accent_soft,  &palette->surface,     &palette->surface_alt,
      &palette->card,         &palette->border,      &palette->settings_bg,
      &palette->settings_panel, &palette->settings_text,
      &palette->settings_subtext,
  };

  if (!path || !palette)
    return -1;
  if (media_load_file(path, &manifest_data, &manifest_size) != 0)
    return -1;
  if (!manifest_data || manifest_size == 0 || manifest_size >= sizeof(manifest)) {
    media_free_file(manifest_data);
    return -1;
  }

  for (size_t i = 0; i < manifest_size; i++)
    manifest[i] = (char)manifest_data[i];
  manifest[manifest_size] = '\0';
  media_free_file(manifest_data);

  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (manifest_get_value(manifest, keys[i], value, sizeof(value)) == 0 &&
        value[0]) {
      *slots[i] = gui_parse_hex_color(value);
    }
  }

  return 0;
}

static void gui_save_theme_palette(const char *path,
                                   const gui_theme_palette_t *palette) {
  char manifest[768];
  int idx = 0;
  static const char *keys[] = {
      "app_bg",      "app_fg",      "accent",      "accent_soft",
      "surface",     "surface_alt", "card",        "border",
      "settings_bg", "settings_panel", "settings_text", "settings_subtext",
  };
  uint32_t values[] = {
      palette->app_bg,      palette->app_fg,      palette->accent,
      palette->accent_soft,  palette->surface,     palette->surface_alt,
      palette->card,         palette->border,      palette->settings_bg,
      palette->settings_panel, palette->settings_text,
      palette->settings_subtext,
  };

  if (!path || !palette)
    return;

  vfs_mkdir("/System", 0755);

  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    const char *key = keys[i];
    while (*key && idx < (int)sizeof(manifest) - 1)
      manifest[idx++] = *key++;
    if (idx < (int)sizeof(manifest) - 1)
      manifest[idx++] = '=';
    gui_append_hex_color(manifest, (int)sizeof(manifest), &idx, values[i]);
    if (idx < (int)sizeof(manifest) - 1)
      manifest[idx++] = '\n';
  }
  if (idx < (int)sizeof(manifest))
    manifest[idx] = '\0';
  else
    manifest[sizeof(manifest) - 1] = '\0';

  media_install_text_file(path, manifest);
}

static int gui_load_saved_theme(void) {
  char manifest[64];
  char mode[16];
  uint8_t *data = NULL;
  size_t size = 0;

  if (gui_load_theme_palette(GUI_THEME_DARK_PATH, &g_theme_dark) != 0) {
  }
  if (gui_load_theme_palette(GUI_THEME_LIGHT_PATH, &g_theme_light) != 0) {
  }

  if (media_load_file(GUI_THEME_CONFIG_PATH, &data, &size) == 0 && data &&
      size > 0 && size < sizeof(manifest)) {
    for (size_t i = 0; i < size; i++)
      manifest[i] = (char)data[i];
    manifest[size] = '\0';
    if (manifest_get_value(manifest, "mode", mode, sizeof(mode)) == 0) {
      int parsed = gui_theme_mode_from_text(mode);
      if (parsed >= 0)
        g_theme_mode = (gui_theme_mode_t)parsed;
    }
    media_free_file(data);
  } else if (data) {
    media_free_file(data);
  }

  return 0;
}

static void gui_save_theme_preference(void) {
  char manifest[64];
  int idx = 0;

  vfs_mkdir("/System", 0755);
  for (const char *p = "mode="; *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  for (const char *p = gui_theme_mode_name(g_theme_mode); *p &&
       idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  if (idx < (int)sizeof(manifest) - 1)
    manifest[idx++] = '\n';
  if (idx < (int)sizeof(manifest))
    manifest[idx] = '\0';
  else
    manifest[sizeof(manifest) - 1] = '\0';
  media_install_text_file(GUI_THEME_CONFIG_PATH, manifest);
}

/* UI Theme Colors - mapped through current palette */
#define THEME_BG (gui_theme_palette()->app_bg)
#define THEME_FG (gui_theme_palette()->app_fg)
#define THEME_ACCENT (gui_theme_palette()->accent)
#define THEME_ACCENT2 (gui_theme_palette()->accent_soft)
#define THEME_TITLEBAR (gui_theme_palette()->surface)
#define THEME_TITLEBAR_INACTIVE (gui_theme_palette()->surface_alt)
#define THEME_BORDER (gui_theme_palette()->border)
#define THEME_BUTTON (gui_theme_palette()->surface_alt)
#define THEME_BUTTON_HOVER (gui_theme_palette()->surface)

/* macOS Traffic Light Colors - Slightly desaturated for premium look */
#define COLOR_BTN_CLOSE 0xEF4444    /* Red */
#define COLOR_BTN_MINIMIZE 0xF59E0B /* Amber */
#define COLOR_BTN_ZOOM 0x22C55E     /* Green */

/* Menu Bar - Frosted glass effect */
#define COLOR_MENU_BG (gui_theme_palette()->menu_right)
#define COLOR_MENU_TEXT (gui_theme_palette()->app_fg)
#define MENU_BAR_HEIGHT 0

/* Dock - Modern glass dock */
#define COLOR_DOCK_BG (gui_theme_palette()->dock_fill)
#define COLOR_DOCK_BORDER (gui_theme_palette()->border)
#define COLOR_DOCK_GLASS (gui_theme_palette()->surface)
#define DOCK_HEIGHT 70

#define KEY_WINDOW_SWITCHER 0x110
#define KEY_CTRL_ALT_DEL 0x111
#define KEY_UP 0x100
#define KEY_DOWN 0x101
#define KEY_LEFT 0x102
#define KEY_RIGHT 0x103
#define KEY_LEFT_SUPER 125
#define KEY_RIGHT_SUPER 126

static int session_authenticated = 1;
static char account_username[32];
static char account_password[33];
static char startup_input_username[32];
static int user_home_mount_active = 0;
static char user_home_mounted_username[32];
static int startup_login_user_dropdown_open = 0;
static void startup_begin_login_flow(const char *message, int preserve_username);

static int dock_is_visible(void) {
  return session_authenticated && !startup_flow_active();
}

static int dock_reserved_height(void) { return dock_is_visible() ? DOCK_HEIGHT : 0; }

int gui_dock_reserved_height(void) { return dock_reserved_height(); }

static int desktop_session_active(void) {
  return session_authenticated && !startup_flow_active();
}

static int session_can_logout(void) {
  extern int boot_is_live_media(void);
  extern int boot_is_usb_boot(void);

  if (!desktop_session_active())
    return 0;
  if (!account_username[0] || !account_password[0])
    return 0;
  return !(boot_is_live_media() || boot_is_usb_boot());
}

static int gui_is_installer_mode(void) {
  extern int boot_is_installer_mode(void);
  return boot_is_installer_mode();
}

static int gui_installer_text_mode(void) {
  return 0;
}

static char installer_status[96] = "Ready to install the system image.";
static int installer_has_run = 0;
static int installer_active = 0;
static int installer_autorun_attempted = 0;
static int installer_show_restart_screen = 0;
enum {
  INSTALLER_PAGE_WELCOME = 0,
  INSTALLER_PAGE_TARGET = 1,
  INSTALLER_PAGE_REVIEW = 2,
  INSTALLER_PAGE_PROGRESS = 3,
  INSTALLER_PAGE_COMPLETE = 4
};
static int installer_page = INSTALLER_PAGE_WELCOME;
static int installer_phase = 0;
static int installer_progress_done = 0;
static int installer_progress_total = 100;
static int installer_copied_files = 0;
static int installer_failed_files = 0;
static int installer_ensured_changes = 0;
static uint64_t installer_reboot_deadline_ms = 0;
static int installer_progress_total_files = 0;
static int installer_progress_processed_files = 0;
static int installer_install_journal_ready = 0;
static int installer_install_journal_disk_index = -1;
static int installer_target_disk_index = -1;
static uint32_t installer_install_journal_next_lba = 0;
static uint32_t installer_install_journal_last_lba = 0;
static char installer_target_root[96];
static char bootscource[96];
static char installer_efi_root[128];
static char installer_update_root[128];
static char installer_progress_stage[64] = "Ready";
static char installer_progress_detail[160] =
    "The installer is waiting to start.";
static char installer_progress_current_item[160] = "";
static char installer_system_image_payload_path[96] = "";
static char installer_boot_payload_path[96] = "";
static char partition_manager_status[96] = "Select a real disk to manage.";
static int installer_disk_count = 0;
static int installer_selected_disk = 0;
static char installer_disk_labels[8][80];
static int installer_disk_indices[8];
static int partition_manager_partition_count = 0;
static int partition_manager_selected_partition = 0;
static char partition_manager_labels[8][96];
static int disk_imager_selected_disk = -1;
static int disk_imager_selected_partition = 0;
static char disk_imager_status[128] = "Select a disk to image.";
static char disk_imager_disk_label[128] = "";
static char disk_imager_partition_label[160] = "";
static char disk_imager_disk_path[256] = "";
static char disk_imager_partition_path[256] = "";
static int window_switcher_frames = 0;
static char window_switcher_title[64] = "No windows";
static int secure_attention_open = 0;
static int secure_attention_selection = 0;

#define SECURE_ACTION_CANCEL 0
#define SECURE_ACTION_RESTART 1
#define SECURE_ACTION_SHUTDOWN 2
/* ===================================================================== */
/* Wallpaper Manager                                                     */
/* ===================================================================== */
#define NUM_WALLPAPERS 11
#define DEFAULT_WALLPAPER_INDEX 5
static int current_wallpaper = DEFAULT_WALLPAPER_INDEX;

/* Wallpaper types: 0 = gradient, 1 = image */
/* Wallpaper types: 0 = gradient, 1 = image */
#define WALLPAPER_FIT_COVER 0
#define WALLPAPER_FIT_STRETCH 1
static struct {
  int type;           /* 0 = gradient, 1 = JPEG image */
  uint8_t tr, tg, tb; /* Gradient: Top color */
  uint8_t br, bg, bb; /* Gradient: Bottom color */
  int fit_mode;       /* Image fit mode */
  const char *name;   /* Display name */
  const char *path;   /* Image path (for type=1) */
} wallpapers[NUM_WALLPAPERS] = {
    {1, 38, 72, 120, 16, 30, 58, WALLPAPER_FIT_COVER, "Landscape", "/assets/wallpapers/landscape.png"},
    {1, 26, 92, 82, 9, 37, 48, WALLPAPER_FIT_COVER, "Nature", "/assets/wallpapers/nature.jpg"},
    {1, 84, 108, 148, 26, 33, 52, WALLPAPER_FIT_COVER, "City", "/assets/wallpapers/city.jpg"},
    {1, 124, 82, 126, 48, 28, 64, WALLPAPER_FIT_COVER, "Portrait", "/assets/wallpapers/portrait.jpg"},
    {1, 58, 88, 118, 22, 28, 46, WALLPAPER_FIT_STRETCH, "Ducks", "/assets/wallpapers/ducks.png"},
    {1, 58, 88, 118, 22, 28, 46, WALLPAPER_FIT_STRETCH, "Default", "/assets/wallpapers/default.jpg"},
    {0, 30, 27, 75, 15, 27, 62, WALLPAPER_FIT_COVER, "Indigo Night", NULL},
    {0, 20, 60, 100, 10, 30, 60, WALLPAPER_FIT_COVER, "Ocean Blue", NULL},
    {0, 60, 20, 60, 30, 15, 45, WALLPAPER_FIT_COVER, "Purple Haze", NULL},
    {0, 20, 20, 20, 5, 5, 10, WALLPAPER_FIT_COVER, "Midnight", NULL},
    {0, 80, 60, 30, 40, 30, 20, WALLPAPER_FIT_COVER, "Golden Hour", NULL},
};

/* Cached wallpaper image for desktop background */
static media_image_t wallpaper_image = {0, 0, NULL};
static int wallpaper_loaded = -1; /* Which wallpaper is currently loaded */
static int wallpaper_image_heap_allocated = 0;

/* Cached thumbnails for Background Settings window */
static media_image_t thumbnail_cache[NUM_WALLPAPERS] = {{0}};
static int thumbnails_loaded = 0;
static int background_settings_window_count = 0;

#define WALLPAPER_BUFFER_MAX_PIXELS (2048 * 2048)
#define WALLPAPER_THUMBNAIL_W 96
#define WALLPAPER_THUMBNAIL_H 64

static int path_is_supported_image_file(const char *path) {
  return path &&
         (str_ends_with_ci(path, ".png") || str_ends_with_ci(path, ".jpg") ||
          str_ends_with_ci(path, ".jpeg") || str_ends_with_ci(path, ".svg"));
}

static int decode_jpeg_image(const uint8_t *data, size_t size, media_image_t *out,
                             uint32_t *jpeg_buffer, size_t jpeg_buffer_size,
                             int *heap_allocated) {
  int ret;

  if (!data || !out)
    return -EINVAL;

  out->width = 0;
  out->height = 0;
  out->pixels = NULL;

  if (jpeg_buffer && jpeg_buffer_size) {
    ret = media_decode_jpeg_buffer(data, size, out, jpeg_buffer, jpeg_buffer_size);
    if (ret == -ENOMEM) {
      out->width = 0;
      out->height = 0;
      out->pixels = NULL;
      ret = media_decode_jpeg(data, size, out);
    }
  } else {
    ret = media_decode_jpeg(data, size, out);
  }

  if (heap_allocated) {
    if (ret == 0) {
      *heap_allocated = !(jpeg_buffer && out->pixels == jpeg_buffer);
    } else {
      *heap_allocated = 0;
    }
  }

  return ret;
}

static int decode_image_file_for_path(const char *path, const uint8_t *data,
                                      size_t size, media_image_t *out,
                                      uint32_t *jpeg_buffer,
                                      size_t jpeg_buffer_size,
                                      int *heap_allocated) {
  int ret = -EINVAL;

  if (!data || !size || !out)
    return -EINVAL;

  if (heap_allocated)
    *heap_allocated = 0;

  if (size >= 4 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' &&
      data[3] == 'G') {
    ret = media_decode_png(data, size, out);
    if (heap_allocated)
      *heap_allocated = (ret == 0);
    return ret;
  }

  if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
    return decode_jpeg_image(data, size, out, jpeg_buffer, jpeg_buffer_size,
                             heap_allocated);
  }

  if (path && str_ends_with_ci(path, ".svg")) {
    ret = media_decode_svg(data, size, out);
    if (heap_allocated)
      *heap_allocated = (ret == 0);
    return ret;
  }

  if (path && str_ends_with_ci(path, ".png")) {
    ret = media_decode_png(data, size, out);
    if (heap_allocated)
      *heap_allocated = (ret == 0);
    return ret;
  }

  if (path && (str_ends_with_ci(path, ".jpg") ||
               str_ends_with_ci(path, ".jpeg"))) {
    return decode_jpeg_image(data, size, out, jpeg_buffer, jpeg_buffer_size,
                             heap_allocated);
  }

  ret = media_decode_svg(data, size, out);
  if (ret == 0) {
    if (heap_allocated)
      *heap_allocated = 1;
    return ret;
  }

  out->width = 0;
  out->height = 0;
  out->pixels = NULL;
  ret = media_decode_png(data, size, out);
  if (ret == 0) {
    if (heap_allocated)
      *heap_allocated = 1;
    return ret;
  }

  return decode_jpeg_image(data, size, out, jpeg_buffer, jpeg_buffer_size,
                           heap_allocated);
}

static int wallpaper_build_thumbnail(media_image_t *dest,
                                     const media_image_t *src) {
  size_t thumb_pixels =
      (size_t)WALLPAPER_THUMBNAIL_W * (size_t)WALLPAPER_THUMBNAIL_H;
  uint32_t *pixels;

  if (!dest || !src || !src->pixels || !src->width || !src->height)
    return -1;

  pixels = (uint32_t *)kmalloc(thumb_pixels * sizeof(uint32_t), GFP_KERNEL);
  if (!pixels)
    return -ENOMEM;

  for (int y = 0; y < WALLPAPER_THUMBNAIL_H; y++) {
    uint32_t src_y = ((uint32_t)y * src->height) / WALLPAPER_THUMBNAIL_H;
    if (src_y >= src->height)
      src_y = src->height - 1;

    for (int x = 0; x < WALLPAPER_THUMBNAIL_W; x++) {
      uint32_t src_x = ((uint32_t)x * src->width) / WALLPAPER_THUMBNAIL_W;
      if (src_x >= src->width)
        src_x = src->width - 1;

      pixels[y * WALLPAPER_THUMBNAIL_W + x] =
          src->pixels[src_y * src->width + src_x];
    }
  }

  dest->width = WALLPAPER_THUMBNAIL_W;
  dest->height = WALLPAPER_THUMBNAIL_H;
  dest->pixels = pixels;
  return 0;
}

static int wallpaper_load_asset_file(const char *path, uint8_t **data,
                                     size_t *size) {
  char boot_path[256];

  if (!path || !data || !size)
    return -EINVAL;

  if (media_load_file(path, data, size) == 0)
    return 0;

  if (path[0] == '/' && path[1] == 'a' && path[2] == 's' && path[3] == 's' &&
      path[4] == 'e' && path[5] == 't' && path[6] == 's' && path[7] == '/') {
    str_copy_safe(boot_path, "/setup/boot", sizeof(boot_path));
    installer_append_to_buf(boot_path, sizeof(boot_path), path);
    if (media_load_file(boot_path, data, size) == 0)
      return 0;

    str_copy_safe(boot_path, "/boot", sizeof(boot_path));
    installer_append_to_buf(boot_path, sizeof(boot_path), path);
    if (media_load_file(boot_path, data, size) == 0)
      return 0;
  }

  return -ENOENT;
}

static void free_thumbnails(void) {
  for (int i = 0; i < NUM_WALLPAPERS; i++) {
    if (thumbnail_cache[i].pixels) {
      media_free_image(&thumbnail_cache[i]);
      thumbnail_cache[i].pixels = NULL;
      thumbnail_cache[i].width = 0;
      thumbnail_cache[i].height = 0;
    }
  }
  thumbnails_loaded = 0;
}

static void background_settings_window_on_close(struct window *win) {
  (void)win;

  if (background_settings_window_count > 0)
    background_settings_window_count--;
  if (background_settings_window_count == 0)
    free_thumbnails();
}

/* Load all thumbnails once */
static void load_thumbnails(void) {
  if (thumbnails_loaded)
    return;

  for (int i = 0; i < NUM_WALLPAPERS; i++) {
    if (wallpapers[i].type == 1 && wallpapers[i].path) {
      uint8_t *data = NULL;
      size_t size = 0;
      if (wallpaper_load_asset_file(wallpapers[i].path, &data, &size) == 0) {
        media_image_t decoded = {0, 0, NULL};
        int decode_ok = decode_image_file_for_path(
            wallpapers[i].path, data, size, &decoded, NULL, 0, NULL);

        if (decode_ok == 0) {
          if (wallpaper_build_thumbnail(&thumbnail_cache[i], &decoded) != 0) {
            thumbnail_cache[i] = decoded;
            decoded.pixels = NULL;
            decoded.width = 0;
            decoded.height = 0;
          }
          media_free_image(&decoded);
        }
        media_free_file(data);
      }
    }
  }
  thumbnails_loaded = 1;
}
/* Keep common desktop wallpapers off the heap during early rendering. */
static uint32_t wallpaper_buffer[WALLPAPER_BUFFER_MAX_PIXELS];

/* Load wallpaper image if needed */
static void wallpaper_ensure_loaded(void) {
  if (wallpapers[current_wallpaper].type != 1)
    return; /* Gradient, no load */
  if (wallpaper_loaded == current_wallpaper && wallpaper_image.pixels != NULL)
    return; /* Already loaded */

  if (wallpaper_image_heap_allocated && wallpaper_image.pixels != NULL) {
    media_free_image(&wallpaper_image);
    wallpaper_image_heap_allocated = 0;
  }

  /* Reset previous image state (don't free valid static buffer, just reuse it)
   */
  wallpaper_image.width = 0;
  wallpaper_image.height = 0;
  wallpaper_image.pixels = NULL;
  wallpaper_loaded = -1;

  /* Load new image */
  const char *path = wallpapers[current_wallpaper].path;
  uint8_t *data = NULL;
  size_t size = 0;

  if (wallpaper_load_asset_file(path, &data, &size) == 0) {
    int decode_ok = decode_image_file_for_path(
        path, data, size, &wallpaper_image, wallpaper_buffer,
        sizeof(wallpaper_buffer), &wallpaper_image_heap_allocated);
    if (decode_ok == 0) {
      wallpaper_loaded = current_wallpaper;
    } else {
      wallpaper_loaded = -1;
    }
    media_free_file(data);
  } else {
    wallpaper_loaded = -1;
  }
}

/* Calculator state (global for click handling) */
static long calc_display = 0;
static long calc_pending = 0;
static char calc_op = 0;
static int calc_clear_next = 0;

static void calc_button_click(char key) {
  if (key >= '0' && key <= '9') {
    int digit = key - '0';
    if (calc_clear_next) {
      calc_display = digit;
      calc_clear_next = 0;
    } else {
      calc_display = calc_display * 10 + digit;
    }
  } else if (key == 'C') {
    calc_display = 0;
    calc_pending = 0;
    calc_op = 0;
    calc_clear_next = 0;
  } else if (key == '=') {
    if (calc_op == '+')
      calc_display = calc_pending + calc_display;
    else if (calc_op == '-')
      calc_display = calc_pending - calc_display;
    else if (calc_op == '*')
      calc_display = calc_pending * calc_display;
    else if (calc_op == '/' && calc_display != 0)
      calc_display = calc_pending / calc_display;
    calc_op = 0;
    calc_clear_next = 1;
  } else if (key == '+' || key == '-' || key == '*' || key == '/') {
    if (calc_op) {
      /* Chain operations */
      if (calc_op == '+')
        calc_display = calc_pending + calc_display;
      else if (calc_op == '-')
        calc_display = calc_pending - calc_display;
      else if (calc_op == '*')
        calc_display = calc_pending * calc_display;
      else if (calc_op == '/' && calc_display != 0)
        calc_display = calc_pending / calc_display;
    }
    calc_pending = calc_display;
    calc_op = key;
    calc_clear_next = 1;
  }
}

/* Notepad state (global for keyboard input) */
#define NOTEPAD_MAX_TEXT 2048
static char notepad_text[NOTEPAD_MAX_TEXT];
static char notepad_filepath[256]; /* Track open file */
static int notepad_cursor = 0;
static int notepad_selection_anchor = -1;
static int notepad_selection_cursor = -1;
static int notepad_selecting_with_mouse = 0;
static char notepad_status[96] = "Ready";
static int notepad_dirty = 0;

#define NOTEPAD_DIALOG_NONE 0
#define NOTEPAD_DIALOG_OPEN 1
#define NOTEPAD_DIALOG_SAVE 2
static int notepad_dialog_mode = NOTEPAD_DIALOG_NONE;
static char notepad_dialog_dir[256] = "/Documents";
static char notepad_dialog_input[256];
static char notepad_dialog_selected[64];
static struct window *notepad_find_window(void);
static void notepad_update_window_title(void);

/* Rename State */
static char rename_text[256];
static char rename_path[512];
static int rename_cursor = 0;

/* System Clipboard */
#define CLIPBOARD_MAX 1024
static char clipboard_buffer[CLIPBOARD_MAX];
static int clipboard_len = 0;

/* Terminal state (global for keyboard input) */
#define TERM_INPUT_MAX 256
#define TERM_HISTORY_LINES 16
static char term_input[TERM_INPUT_MAX];
static int term_input_len = 0;
static char term_history[TERM_HISTORY_LINES][80];
static int term_history_count = 0;

/* Bowling game state */
#define BOWLING_LANE_COLUMNS 7
#define BOWLING_PIN_COUNT 10
#define BOWLING_LANE_PROGRESS_MAX 100
static const int bowling_pin_layout_x[BOWLING_PIN_COUNT] = {3, 2, 4, 1, 3,
                                                            5, 0, 2, 4, 6};
static const int bowling_pin_layout_y[BOWLING_PIN_COUNT] = {0, 1, 1, 2, 2,
                                                            2, 3, 3, 3, 3};
static int bowling_pin_standing[BOWLING_PIN_COUNT];
static int bowling_ball_column = 3;
static int bowling_ball_progress = 0;
static int bowling_ball_rolling = 0;
static int bowling_ball_spin = 0;
static int bowling_ball_power = 3;
static int bowling_total_score = 0;
static int bowling_last_knocked = 0;
static int bowling_frame = 1;
static int bowling_roll = 1;
static int bowling_first_roll_pins = 0;
static int bowling_game_over = 0;

/* Mouse state (global for hover effects) */
static int mouse_x = 512, mouse_y = 384;
static int mouse_buttons = 0;
static int settings_active_tab = 0;
static char settings_status[96] = "Tune your desktop experience.";
static int settings_theme_active_slot = 2;
static char settings_user_new_name[32] = "";
static char settings_user_new_password[32] = "";
static char settings_user_selected[32] = "";
static int settings_user_new_role_idx = 1;
static int settings_user_active_field = 0;

static const char *settings_theme_slots[] = {
    "App BG",       "App FG",       "Accent",      "Accent Soft",
    "Surface",      "Surface Alt",  "Card",        "Border",
    "Settings BG",  "Settings Panel","Settings Text","Settings Subtext",
};

static const uint32_t settings_theme_chips[] = {
    0x0B1020, 0x111827, 0x1F2937, 0x334155, 0x475569, 0x64748B,
    0x94A3B8, 0xCBD5E1, 0xE2E8F0, 0xF8FAFC, 0x2563EB, 0x3B82F6,
    0x14B8A6, 0x22C55E, 0xF59E0B, 0xEF4444, 0xDB2777, 0x8B5CF6,
    0xFDE68A, 0xF9A8D4, 0xA6E3A1, 0x93C5FD, 0xF5F5F5, 0x1E293B,
};

#define SETTINGS_THEME_SLOT_COUNT \
  ((int)(sizeof(settings_theme_slots) / sizeof(settings_theme_slots[0])))

#define SETTINGS_THEME_CHIP_COUNT \
  ((int)(sizeof(settings_theme_chips) / sizeof(settings_theme_chips[0])))

typedef struct {
  uint16_t width;
  uint16_t height;
  const char *label;
} settings_resolution_option_t;

static const settings_resolution_option_t settings_resolution_options[] = {
    {1024, 768, "1024x768"},
    {1280, 720, "1280x720"},
    {1600, 900, "1600x900"},
    {1920, 1080, "1920x1080"},
};

#define SETTINGS_RESOLUTION_OPTION_COUNT \
  ((int)(sizeof(settings_resolution_options) / \
         sizeof(settings_resolution_options[0])))

#define SETTINGS_ACCOUNT_LIST_INITIAL_CAPACITY 8

static const char *settings_user_role_options[] = {"admin", "user", "child"};

#define SETTINGS_USER_ROLE_COUNT                                               \
  ((int)(sizeof(settings_user_role_options) /                                 \
         sizeof(settings_user_role_options[0])))

#define SETTINGS_MENU_COUNT 12

static const char *settings_menu_labels[SETTINGS_MENU_COUNT] = {
    "HOME",    "NETWORK",  "STORAGE", "DISPLAY",
    "APPS",    "USERS",    "PERSONAL","UPDATES",
    "RECOVERY","INFO",     "SIDEBAR", "DEVELOPER",
};

static int settings_resolution_current_idx = -1;
static int settings_resolution_pending_idx = -1;
static int settings_resolution_saved_idx = -1;
static char wifi_password_draft[32] = "";
static int settings_wifi_password_active = 0;
static int wifi_tray_password_active = 0;
static uint32_t *g_saved_backbuffer;
static int wallpaper_cached;
static int wallpaper_cached_idx;
static int settings_find_resolution_index(uint32_t width, uint32_t height);
static void gui_clamp_windows_to_display(void);
static int gui_apply_resolution(uint32_t width, uint32_t height);
static int gui_load_saved_resolution(uint32_t *width, uint32_t *height);
static void gui_apply_saved_boot_resolution(uint32_t **framebuffer,
                                            uint32_t *width,
                                            uint32_t *height,
                                            uint32_t *pitch);
static int gui_theme_mode_from_text(const char *text);
static const char *gui_theme_mode_name(gui_theme_mode_t mode);
static gui_theme_palette_t *gui_active_theme_palette(void);
static uint32_t *gui_theme_slot_ptr(gui_theme_palette_t *palette, int slot);
static uint32_t gui_theme_slot_value(const gui_theme_palette_t *palette, int slot);
static int gui_load_theme_palette(const char *path, gui_theme_palette_t *palette);
static void gui_save_theme_palette(const char *path,
                                   const gui_theme_palette_t *palette);
static void ensure_gui_app_dirs(void);
static int settings_add_user_account(void);
static int settings_remove_selected_user_account(void);
static int account_role_is_admin(void);

static const char *settings_page_heading(int page) {
  switch (page) {
  case 0:
    return "HOME";
  case 1:
    return "NETWORK";
  case 2:
    return "STORAGE";
  case 3:
    return "DISPLAY";
  case 4:
    return "APPS";
  case 5:
    return "USERS";
  case 6:
    return "PERSONAL";
  case 7:
    return "UPDATES";
  case 8:
    return "RECOVERY";
  case 10:
    return "SIDEBAR";
  case 11:
    return "DEVELOPER";
  default:
    return "INFO";
  }
}

static const char *settings_default_status_message(int page) {
  switch (page) {
  case 0:
    return "Tune your desktop experience.";
  case 1:
    return "Review the current network state.";
  case 2:
    return "Inspect storage tools and launch disk utilities.";
  case 3:
    return "Adjust wallpapers, themes, effects, and display presets.";
  case 4:
    return "Browse installed tools and launch desktop apps quickly.";
  case 5:
    return "Manage the active system session and local users.";
  case 6:
    return "Personalize the desktop surface and theme behavior.";
  case 7:
    return "System updates are not available yet.";
  case 8:
    return "Recovery tools and reset actions.";
  case 10:
    return "Configure the sidebar widget launcher and its layout.";
  case 11:
    return "Tune theme slots, accent colors, and developer visuals.";
  default:
    return "System build and environment details.";
  }
}

static void theme_builder_apply_slot_color(int slot, uint32_t color) {
  gui_theme_palette_t *palette = gui_active_theme_palette();
  uint32_t *target = gui_theme_slot_ptr(palette, slot);

  if (!target)
    return;
  *target = color & 0x00FFFFFFU;
  compositor_mark_full_redraw();
}

static void theme_builder_save_current_theme(void) {
  gui_save_theme_preference();
  str_copy_safe(settings_status, "Theme saved to /System/theme.cfg.",
                sizeof(settings_status));
}

static void draw_theme_builder_window(int content_x, int content_y,
                                      int content_w, int content_h) {
  const gui_theme_palette_t *theme = gui_theme_palette();
  int preview_x = content_x + 14;
  int preview_y = content_y + 14;
  int preview_w = content_w - 28;
  int preview_h = 102;
  int slots_x = content_x + 14;
  int slots_y = preview_y + preview_h + 14;
  int slot_gap = 8;
  int slot_w = (content_w - 28 - slot_gap * 3) / 4;
  int slot_h = 40;
  int chips_x = content_x + 14;
  int chips_y = slots_y + ((slot_h + slot_gap) * 3) + 14;
  int chip_gap = 8;
  int chip_w = 32;
  int chip_h = 26;
  int chip_cols = 6;
  int title_bg = theme->settings_panel;
  int selected_slot = settings_theme_active_slot;

  if (selected_slot < 0 || selected_slot >= SETTINGS_THEME_SLOT_COUNT)
    selected_slot = 0;

  gui_draw_rect(content_x, content_y, content_w, content_h, theme->settings_bg);
  gui_draw_string(content_x + 14, content_y + 12, "Theme Presets",
                  theme->settings_text, theme->settings_bg);
  gui_draw_string(content_x + 14, content_y + 30,
                  "Pick a slot, preview the palette, then save it to disk.",
                  theme->settings_subtext, theme->settings_bg);

  gui_draw_rect(preview_x, preview_y, preview_w, preview_h, title_bg);
  gui_draw_rect(preview_x, preview_y, preview_w, 3, theme->accent);
  gui_draw_string(preview_x + 12, preview_y + 10, "Live Preview",
                  theme->settings_text, title_bg);
  gui_draw_string(preview_x + 12, preview_y + 28,
                  gui_theme_mode_name(g_theme_mode), theme->accent_soft, title_bg);
  gui_draw_string(preview_x + 12, preview_y + 48,
                  "The palette below edits the active mode in memory.",
                  theme->settings_subtext, title_bg);
  gui_draw_rect(preview_x + preview_w - 196, preview_y + 12, 52, 26,
                g_theme_mode == GUI_THEME_LIGHT ? 0x2563EB : 0x475569);
  gui_draw_string(preview_x + preview_w - 186, preview_y + 20, "Light",
                  0xFFFFFF, g_theme_mode == GUI_THEME_LIGHT ? 0x2563EB : 0x475569);
  gui_draw_rect(preview_x + preview_w - 136, preview_y + 12, 52, 26,
                g_theme_mode == GUI_THEME_DARK ? 0x111827 : 0x475569);
  gui_draw_string(preview_x + preview_w - 126, preview_y + 20, "Dark",
                  0xFFFFFF, g_theme_mode == GUI_THEME_DARK ? 0x111827 : 0x475569);
  gui_draw_rect(preview_x + preview_w - 74, preview_y + 12, 60, 26, 0x2563EB);
  gui_draw_string(preview_x + preview_w - 62, preview_y + 20, "Save",
                  0xFFFFFF, 0x2563EB);

  gui_draw_rect(preview_x + 12, preview_y + 64, preview_w - 24, 24,
                theme->surface_alt);
  gui_draw_string(preview_x + 24, preview_y + 71,
                  settings_theme_slots[selected_slot], theme->app_fg,
                  theme->surface_alt);
  gui_draw_string(preview_x + preview_w - 230, preview_y + 71,
                  "Selected slot", theme->app_muted, theme->surface_alt);

  for (int i = 0; i < SETTINGS_THEME_SLOT_COUNT; i++) {
    int col = i % 4;
    int row = i / 4;
    int sx = slots_x + col * (slot_w + slot_gap);
    int sy = slots_y + row * (slot_h + slot_gap);
    uint32_t color = gui_theme_slot_value(theme, i);
    uint32_t text_color = gui_contrast_title_color(color);
    uint32_t outline = i == selected_slot ? theme->accent : theme->border;

    gui_draw_rect(sx, sy, slot_w, slot_h, color);
    gui_draw_rect_outline(sx, sy, slot_w, slot_h, outline, i == selected_slot ? 2 : 1);
    gui_draw_string(sx + 8, sy + 8, settings_theme_slots[i], text_color, color);
    gui_draw_string(sx + 8, sy + 22, "click to edit", text_color, color);
  }

  gui_draw_rect(chips_x, chips_y - 4, preview_w, 24, theme->surface_alt);
  gui_draw_string(chips_x + 12, chips_y + 4, "Color chips", theme->app_fg,
                  theme->surface_alt);
  gui_draw_string(chips_x + 118, chips_y + 4,
                  "Click a chip to update the selected slot.",
                  theme->app_muted, theme->surface_alt);

  for (int i = 0; i < SETTINGS_THEME_CHIP_COUNT; i++) {
    int col = i % chip_cols;
    int row = i / chip_cols;
    int cx = chips_x + col * (chip_w + chip_gap);
    int cy = chips_y + 28 + row * (chip_h + chip_gap);
    uint32_t color = settings_theme_chips[i];

    gui_draw_rect(cx, cy, chip_w, chip_h, color);
    gui_draw_rect_outline(cx, cy, chip_w, chip_h, theme->border, 1);
  }
}

static int settings_user_editor_is_visible(void) {
  return settings_active_tab == 5;
}

static void notepad_append_to_buf(char *dst, int max, const char *src) {
  if (!dst || max <= 0)
    return;
  if (!src)
    return;
  (void)strlcat(dst, src, (size_t)max);
}

static const char *notepad_basename(const char *path) {
  const char *name = path;

  if (!path || !path[0])
    return "Untitled";
  for (int i = 0; path[i]; i++) {
    if (path[i] == '/' && path[i + 1])
      name = &path[i + 1];
  }
  return name;
}

static void notepad_extract_parent_dir(const char *path, char *out, int out_max) {
  int last_slash = -1;

  if (!out || out_max <= 0)
    return;
  if (!path || !path[0]) {
    str_copy_safe(out, "/Documents", out_max);
    return;
  }

  for (int i = 0; path[i]; i++) {
    if (path[i] == '/')
      last_slash = i;
  }

  if (last_slash <= 0) {
    str_copy_safe(out, "/", out_max);
    return;
  }

  for (int i = 0; i < last_slash && i < out_max - 1; i++)
    out[i] = path[i];
  out[last_slash < out_max ? last_slash : out_max - 1] = '\0';
}

static void notepad_set_status(const char *msg) {
  str_copy_safe(notepad_status, msg, sizeof(notepad_status));
}

static int notepad_text_length(void) {
  int len = 0;
  while (notepad_text[len])
    len++;
  return len;
}

static void notepad_clear_selection(void) {
  notepad_selection_anchor = -1;
  notepad_selection_cursor = -1;
}

static int notepad_has_selection(void) {
  return notepad_selection_anchor >= 0 && notepad_selection_cursor >= 0 &&
         notepad_selection_anchor != notepad_selection_cursor;
}

static void notepad_get_selection_bounds(int *start, int *end) {
  int a = notepad_selection_anchor;
  int b = notepad_selection_cursor;

  if (a < 0 || b < 0) {
    if (start)
      *start = 0;
    if (end)
      *end = 0;
    return;
  }

  if (a > b) {
    int tmp = a;
    a = b;
    b = tmp;
  }

  if (start)
    *start = a;
  if (end)
    *end = b;
}

static void notepad_mark_dirty(const char *status) {
  notepad_dirty = 1;
  notepad_set_status(status);
  notepad_update_window_title();
}

static void notepad_copy_range_to_clipboard(int start, int end) {
  int out = 0;
  int len = notepad_text_length();

  if (start < 0)
    start = 0;
  if (end < start)
    end = start;
  if (end > len)
    end = len;

  for (int i = start; i < end && out < CLIPBOARD_MAX - 1; i++)
    clipboard_buffer[out++] = notepad_text[i];
  clipboard_buffer[out] = '\0';
  clipboard_len = out;
}

static int notepad_copy_selection_to_clipboard(void) {
  int start, end;

  if (!notepad_has_selection())
    return 0;

  notepad_get_selection_bounds(&start, &end);
  notepad_copy_range_to_clipboard(start, end);
  return 1;
}

static int notepad_delete_range(int start, int end) {
  int len = notepad_text_length();
  int remove_len;

  if (start < 0)
    start = 0;
  if (end < start)
    end = start;
  if (end > len)
    end = len;
  if (start >= end)
    return 0;

  remove_len = end - start;
  for (int i = start; i + remove_len <= len; i++)
    notepad_text[i] = notepad_text[i + remove_len];

  notepad_cursor = start;
  notepad_clear_selection();
  return 1;
}

static int notepad_delete_selection(void) {
  int start, end;

  if (!notepad_has_selection())
    return 0;
  notepad_get_selection_bounds(&start, &end);
  return notepad_delete_range(start, end);
}

static int notepad_insert_bytes(const char *src, int count) {
  int len = notepad_text_length();

  if (!src || count <= 0)
    return 0;

  if (notepad_has_selection())
    notepad_delete_selection();
  len = notepad_text_length();

  if (count > NOTEPAD_MAX_TEXT - 1 - len)
    count = NOTEPAD_MAX_TEXT - 1 - len;
  if (count <= 0)
    return 0;

  for (int i = len; i >= notepad_cursor; i--)
    notepad_text[i + count] = notepad_text[i];
  for (int i = 0; i < count; i++)
    notepad_text[notepad_cursor + i] = src[i];
  notepad_cursor += count;
  return count;
}

static int notepad_insert_char(char c) {
  return notepad_insert_bytes(&c, 1);
}

static int notepad_column_for_index(int index) {
  int col = 0;

  if (index < 0)
    index = 0;
  for (int i = index - 1; i >= 0; i--) {
    if (notepad_text[i] == '\n')
      break;
    col++;
  }
  return col;
}

static int notepad_line_start_for_index(int index) {
  int len = notepad_text_length();

  if (index < 0)
    index = 0;
  if (index > len)
    index = len;
  for (int start = index; start > 0; start--) {
    if (notepad_text[start - 1] == '\n')
      return start;
  }
  return 0;
}

static int notepad_move_vertical(int direction) {
  int len = notepad_text_length();
  int line_start = notepad_line_start_for_index(notepad_cursor);
  int column = notepad_column_for_index(notepad_cursor);
  int target_start = 0;
  int target_end = 0;
  int target_cursor = 0;

  if (direction < 0) {
    if (line_start == 0)
      return notepad_cursor;
    target_end = line_start - 1;
    target_start = notepad_line_start_for_index(target_end);
  } else {
    int current_end = line_start;
    while (current_end < len && notepad_text[current_end] &&
           notepad_text[current_end] != '\n')
      current_end++;
    if (current_end >= len || !notepad_text[current_end])
      return notepad_cursor;
    target_start = current_end + 1;
    target_end = target_start;
    while (target_end < len && notepad_text[target_end] &&
           notepad_text[target_end] != '\n')
      target_end++;
  }

  target_cursor = target_start + column;
  if (target_cursor > target_end)
    target_cursor = target_end;
  return target_cursor;
}

static void notepad_move_cursor(int new_cursor, int extend_selection) {
  int len = notepad_text_length();

  if (new_cursor < 0)
    new_cursor = 0;
  if (new_cursor > len)
    new_cursor = len;

  if (extend_selection) {
    if (notepad_selection_anchor < 0)
      notepad_selection_anchor = notepad_cursor;
    notepad_selection_cursor = new_cursor;
  } else {
    notepad_clear_selection();
  }

  notepad_cursor = new_cursor;
}

static int notepad_cursor_from_point(int rel_x, int rel_y, int text_x, int text_y,
                                     int max_x, int max_y) {
  int len = notepad_text_length();
  int tx = text_x;
  int ty = text_y;
  int char_w = 8;
  int line_h = 16;

  if (rel_y < text_y)
    return 0;
  if (rel_y >= max_y)
    return len;
  if (rel_x < text_x)
    rel_x = text_x;

  for (int i = 0; i <= len; i++) {
    char c = notepad_text[i];

    if (rel_y < ty + line_h) {
      if (rel_x < tx + char_w / 2)
        return i;
      if (c == '\0' || c == '\n')
        return i;
      if (rel_x < tx + char_w + char_w / 2)
        return i + 1;
    }

    if (c == '\0')
      return i;
    if (c == '\n') {
      tx = text_x;
      ty += line_h;
      continue;
    }

    tx += char_w;
    if (tx >= max_x) {
      tx = text_x;
      ty += line_h;
    }
  }

  return len;
}

static void notepad_reset_document(void) {
  notepad_text[0] = '\0';
  notepad_cursor = 0;
  notepad_clear_selection();
  notepad_selecting_with_mouse = 0;
  notepad_filepath[0] = '\0';
  notepad_dirty = 0;
  notepad_dialog_mode = NOTEPAD_DIALOG_NONE;
  notepad_dialog_selected[0] = '\0';
  notepad_dialog_input[0] = '\0';
  str_copy_safe(notepad_dialog_dir, "/Documents", sizeof(notepad_dialog_dir));
  notepad_set_status("New document");
  notepad_update_window_title();
}

static int notepad_load_file(const char *path) {
  struct file *f;
  char resolved_path[256];
  const char *open_path;
  int bytes;

  if (!path || !path[0]) {
    notepad_set_status("Open failed: no file selected");
    return -1;
  }

  open_path = resolve_user_storage_path(path, resolved_path,
                                        sizeof(resolved_path));
  f = vfs_open(open_path, O_RDONLY, 0);
  if (!f) {
    notepad_set_status("Open failed");
    return -1;
  }

  bytes = vfs_read(f, notepad_text, NOTEPAD_MAX_TEXT - 1);
  vfs_close(f);
  if (bytes < 0) {
    notepad_set_status("Open failed");
    return -1;
  }

  notepad_text[bytes] = '\0';
  notepad_cursor = bytes;
  notepad_clear_selection();
  notepad_selecting_with_mouse = 0;
  str_copy_safe(notepad_filepath, path, sizeof(notepad_filepath));
  notepad_dirty = 0;
  notepad_dialog_mode = NOTEPAD_DIALOG_NONE;
  notepad_dialog_selected[0] = '\0';
  notepad_dialog_input[0] = '\0';
  notepad_extract_parent_dir(path, notepad_dialog_dir, sizeof(notepad_dialog_dir));
  notepad_set_status("File opened");
  notepad_update_window_title();
  return 0;
}

static int notepad_save_to_path(const char *path) {
  if (!path || !path[0]) {
    notepad_set_status("Save failed: no path");
    return -1;
  }

  installer_ensure_parent_dirs(path);
  if (write_text_file(path, notepad_text) != 0) {
    notepad_set_status("Save failed");
    return -1;
  }

  str_copy_safe(notepad_filepath, path, sizeof(notepad_filepath));
  notepad_extract_parent_dir(path, notepad_dialog_dir, sizeof(notepad_dialog_dir));
  notepad_dirty = 0;
  notepad_dialog_mode = NOTEPAD_DIALOG_NONE;
  notepad_dialog_selected[0] = '\0';
  notepad_set_status("File saved");
  notepad_update_window_title();
  return 0;
}

static void notepad_begin_dialog(int mode) {
  char default_name[256];

  notepad_dialog_mode = mode;
  notepad_dialog_selected[0] = '\0';

  if (notepad_filepath[0]) {
    str_copy_safe(notepad_dialog_input, notepad_filepath,
                  sizeof(notepad_dialog_input));
    notepad_extract_parent_dir(notepad_filepath, notepad_dialog_dir,
                               sizeof(notepad_dialog_dir));
  } else {
    str_copy_safe(notepad_dialog_dir, "/Documents", sizeof(notepad_dialog_dir));
    str_copy_safe(default_name, notepad_dialog_dir, sizeof(default_name));
    notepad_append_to_buf(default_name, sizeof(default_name), "/untitled.txt");
    str_copy_safe(notepad_dialog_input, default_name, sizeof(notepad_dialog_input));
  }

  notepad_set_status(mode == NOTEPAD_DIALOG_OPEN ? "Choose a file to open"
                                                 : "Choose where to save");
}

static void notepad_close_dialog(void) {
  notepad_dialog_mode = NOTEPAD_DIALOG_NONE;
  notepad_dialog_selected[0] = '\0';
  notepad_dialog_input[0] = '\0';
}

static void notepad_confirm_dialog(void) {
  if (notepad_dialog_mode == NOTEPAD_DIALOG_OPEN) {
    if (notepad_load_file(notepad_dialog_input) == 0)
      notepad_close_dialog();
  } else if (notepad_dialog_mode == NOTEPAD_DIALOG_SAVE) {
    if (notepad_save_to_path(notepad_dialog_input) == 0)
      notepad_close_dialog();
  }
}

/* Trig tables for Clock (fixed point 8.8, scale 256) */
/* 0..59 corresponds to 0..360 degrees clockwise from top */
/* x = sin(angle), y = -cos(angle) */
static const int clock_sin[60] = {
    0,    26,   53,   79,   104,  128,  150,  171,  189,  205,  219,  231,
    240,  248,  253,  256,  253,  248,  240,  231,  219,  205,  189,  171,
    150,  128,  104,  79,   53,   26,   0,    -26,  -53,  -79,  -104, -128,
    -150, -171, -189, -205, -219, -231, -240, -248, -253, -256, -253, -248,
    -240, -231, -219, -205, -189, -171, -150, -128, -104, -79,  -53,  -26};
static const int clock_cos[60] = {
    -256, -253, -248, -240, -231, -219, -205, -189, -171, -150, -128, -104,
    -79,  -53,  -26,  0,    26,   53,   79,   104,  128,  150,  171,  189,
    205,  219,  231,  240,  248,  253,  256,  253,  248,  240,  231,  219,
    205,  189,  171,  150,  128,  104,  79,   53,   26,   0,    -26,  -53,
    -79,  -104, -128, -150, -171, -189, -205, -219, -231, -240, -248, -253};

#if defined(ARCH_X86_64) || defined(ARCH_X86)
static uint8_t clock_cmos_read(uint8_t reg) {
  outb(0x70, reg);
  io_wait();
  return inb(0x71);
}

static int clock_bcd_to_int(uint8_t value) {
  return ((value >> 4) * 10) + (value & 0x0F);
}

static void clock_read_rtc_time(int *hours24, int *minutes, int *seconds) {
  uint8_t sec;
  uint8_t min;
  uint8_t hour;
  uint8_t reg_b;

  if (!hours24 || !minutes || !seconds)
    return;

  while (clock_cmos_read(0x0A) & 0x80) {
  }

  sec = clock_cmos_read(0x00);
  min = clock_cmos_read(0x02);
  hour = clock_cmos_read(0x04);
  reg_b = clock_cmos_read(0x0B);

  if (!(reg_b & 0x04)) {
    sec = (uint8_t)clock_bcd_to_int(sec);
    min = (uint8_t)clock_bcd_to_int(min);
    hour = (uint8_t)(((hour & 0x80) ? 0x80 : 0) | clock_bcd_to_int(hour & 0x7F));
  }

  if (!(reg_b & 0x02)) {
    int pm = hour & 0x80;
    hour &= 0x7F;
    if (pm && hour < 12)
      hour = (uint8_t)(hour + 12);
    else if (!pm && hour == 12)
      hour = 0;
  }

  *seconds = sec % 60;
  *minutes = min % 60;
  *hours24 = hour % 24;
}

static int clock_try_read_rtc_date(int *year, int *month, int *day) {
  uint8_t reg_b;
  uint8_t raw_day;
  uint8_t raw_month;
  uint8_t raw_year;

  if (!year || !month || !day)
    return -1;

  while (clock_cmos_read(0x0A) & 0x80) {
  }

  raw_day = clock_cmos_read(0x07);
  raw_month = clock_cmos_read(0x08);
  raw_year = clock_cmos_read(0x09);
  reg_b = clock_cmos_read(0x0B);

  if (!(reg_b & 0x04)) {
    raw_day = (uint8_t)clock_bcd_to_int(raw_day);
    raw_month = (uint8_t)clock_bcd_to_int(raw_month);
    raw_year = (uint8_t)clock_bcd_to_int(raw_year);
  }

  *year = 2000 + (int)raw_year;
  *month = (int)raw_month;
  *day = (int)raw_day;

  if (*month < 1 || *month > 12 || *day < 1 || *day > 31)
    return -1;

  return 0;
}
#else
static int clock_try_read_rtc_date(int *year, int *month, int *day) {
  (void)year;
  (void)month;
  (void)day;
  return -1;
}
#endif

static int64_t activation_days_from_civil(int year, unsigned month,
                                          unsigned day) {
  year -= month <= 2;
  {
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned yoe = (unsigned)(year - era * 400);
    unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
  }
}

static int activation_key_normalize(const char *src, char *dst, int max) {
  int out = 0;

  if (!src || !dst || max < 24)
    return -1;

  for (int i = 0; src[i] && out < max - 1; i++) {
    char c = src[i];

    if (c == '-')
      continue;
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
      continue;
    if (out > 0 && out % 5 == 0) {
      if (out >= max - 2)
        return -1;
      dst[out++] = '-';
    }
    dst[out++] = c;
  }

  dst[out] = '\0';
  return out == 23 ? 0 : -1;
}

static int activation_key_is_valid(const char *key) {
  char normalized[32];
  char hash[33];
  int digit_sum = 0;

  if (activation_key_normalize(key, normalized, sizeof(normalized)) != 0)
    return 0;
  if (normalized[5] != '-' || normalized[11] != '-' || normalized[17] != '-')
    return 0;

  if (str_cmp(normalized, "e3yyj-m242m-r7hpu-4x72e") == 0)
    return 1;

  vib_password_hash_hex("os8-activation", normalized, hash, sizeof(hash));
  for (int i = 0; hash[i]; i++) {
    if (hash[i] >= '1' && hash[i] <= '9')
      digit_sum += hash[i] - '0';
  }
  return digit_sum % 10 == 5;
}

static void activation_append_u64(char *buf, int max, uint64_t value) {
  int idx = 0;

  if (!buf || max <= 0)
    return;
  while (buf[idx] && idx < max - 1)
    idx++;
  append_decimal(buf, &idx, (int)value);
}

static void activation_set_text(char *dst, int max, const char *prefix,
                                const char *value) {
  if (!dst || max <= 0)
    return;
  dst[0] = '\0';
  if (prefix)
    str_copy_safe(dst, prefix, max);
  if (value)
    notepad_append_to_buf(dst, max, value);
}

static void activation_save_state(int activated, uint64_t first_use_day,
                                  const char *key) {
  char manifest[256];
  int idx = 0;

  vfs_mkdir("/System", 0755);

  for (const char *p = "activated="; *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  manifest[idx++] = activated ? '1' : '0';
  manifest[idx++] = '\n';

  for (const char *p = "first_use_day=";
       *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  append_decimal(manifest, &idx, (int)first_use_day);
  manifest[idx++] = '\n';

  for (const char *p = "activation_key=";
       *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; key && key[i] && idx < (int)sizeof(manifest) - 1; i++)
    manifest[idx++] = key[i];
  manifest[idx++] = '\n';
  manifest[idx] = '\0';

  write_text_file(GUI_ACTIVATION_PATH, manifest);
}

static void gui_refresh_activation_state(void) {
  char manifest[256];
  char key_buf[32];
  char first_use_buf[32];
  int year = 0;
  int month = 0;
  int day = 0;
  uint64_t first_use_day = 0;
  uint64_t current_day = 0;
  int activated = 0;
  int dirty = 0;

  g_activation_state.activated = 0;
  g_activation_state.grace_expired = 0;
  g_activation_state.clock_valid = 0;
  g_activation_state.first_use_day = 0;
  g_activation_state.current_day = 0;
  g_activation_state.days_used = 0;
  g_activation_state.days_remaining = GUI_ACTIVATION_GRACE_DAYS;
  g_activation_state.key[0] = '\0';

  key_buf[0] = '\0';
  first_use_buf[0] = '\0';
  if (read_text_file(GUI_ACTIVATION_PATH, manifest, sizeof(manifest)) >= 0) {
    if (manifest_get_value(manifest, "activation_key", key_buf,
                           sizeof(key_buf)) == 0 &&
        key_buf[0]) {
      activation_key_normalize(key_buf, g_activation_state.key,
                               sizeof(g_activation_state.key));
    }
    if (manifest_get_value(manifest, "first_use_day", first_use_buf,
                           sizeof(first_use_buf)) == 0) {
      first_use_day = parse_u64(first_use_buf);
    }
  }

  if (clock_try_read_rtc_date(&year, &month, &day) == 0) {
    int64_t days = activation_days_from_civil(year, (unsigned)month, (unsigned)day);
    if (days >= 0) {
      current_day = (uint64_t)days;
      g_activation_state.clock_valid = 1;
    }
  }

  if (g_activation_state.clock_valid && first_use_day == 0) {
    first_use_day = current_day;
    dirty = 1;
  }

  if (g_activation_state.key[0] && activation_key_is_valid(g_activation_state.key))
    activated = 1;

  g_activation_state.activated = activated;
  g_activation_state.first_use_day = first_use_day;
  g_activation_state.current_day = current_day;

  if (g_activation_state.clock_valid && first_use_day > 0 &&
      current_day >= first_use_day) {
    uint64_t elapsed = current_day - first_use_day;
    if (elapsed > 0x7FFFFFFF)
      elapsed = 0x7FFFFFFF;
    g_activation_state.days_used = (int)elapsed;
    g_activation_state.days_remaining =
        GUI_ACTIVATION_GRACE_DAYS - g_activation_state.days_used;
    if (g_activation_state.days_remaining < 0)
      g_activation_state.days_remaining = 0;
  }

  g_activation_state.grace_expired =
      !g_activation_state.activated && g_activation_state.clock_valid &&
      g_activation_state.days_used >= GUI_ACTIVATION_GRACE_DAYS;

  if (dirty) {
    activation_save_state(g_activation_state.activated, first_use_day,
                          g_activation_state.key);
  } else if (read_text_file(GUI_ACTIVATION_PATH, manifest, sizeof(manifest)) >= 0) {
    char activated_buf[8];
    activated_buf[0] = '\0';
    if (manifest_get_value(manifest, "activated", activated_buf,
                           sizeof(activated_buf)) != 0 ||
        activated_buf[0] != (g_activation_state.activated ? '1' : '0')) {
      activation_save_state(g_activation_state.activated, first_use_day,
                            g_activation_state.key);
    }
  } else if (g_activation_state.clock_valid || g_activation_state.key[0]) {
    activation_save_state(g_activation_state.activated, first_use_day,
                          g_activation_state.key);
  }

  if (g_activation_state.activated) {
    activation_set_text(g_activation_state.status,
                        sizeof(g_activation_state.status), "Activated", NULL);
    activation_set_text(g_activation_state.detail,
                        sizeof(g_activation_state.detail),
                        "Key accepted from /System/activation.cfg", NULL);
    return;
  }

  if (!g_activation_state.clock_valid) {
    activation_set_text(g_activation_state.status,
                        sizeof(g_activation_state.status),
                        "Grace period pending", NULL);
    activation_set_text(g_activation_state.detail,
                        sizeof(g_activation_state.detail),
                        "RTC date unavailable. Add activation_key=... to /System/activation.cfg.",
                        NULL);
    return;
  }

  if (g_activation_state.grace_expired) {
    activation_set_text(g_activation_state.status,
                        sizeof(g_activation_state.status),
                        "Activation required", NULL);
    activation_set_text(g_activation_state.detail,
                        sizeof(g_activation_state.detail),
                        "30-day grace period expired. Add activation_key=... to /System/activation.cfg.",
                        NULL);
    return;
  }

  activation_set_text(g_activation_state.status, sizeof(g_activation_state.status),
                      "Grace period active", NULL);
  g_activation_state.detail[0] = '\0';
  activation_append_u64(g_activation_state.detail,
                        sizeof(g_activation_state.detail),
                        (uint64_t)g_activation_state.days_remaining);
  notepad_append_to_buf(g_activation_state.detail,
                        sizeof(g_activation_state.detail),
                        " day(s) remaining before activation is required.");
}

static int gui_activation_requires_sign_in(void) {
  gui_refresh_activation_state();
  return g_activation_state.grace_expired;
}

static const char *gui_activation_status_text(void) {
  gui_refresh_activation_state();
  return g_activation_state.status;
}

static const char *gui_activation_detail_text(void) {
  gui_refresh_activation_state();
  return g_activation_state.detail;
}

static void clock_get_time(int *hours24, int *minutes, int *seconds) {
#if defined(ARCH_X86_64) || defined(ARCH_X86)
  clock_read_rtc_time(hours24, minutes, seconds);
  return;
#else
  int64_t secs;
  volatile uint32_t *pl031_data = (volatile uint32_t *)0x09010000;
  secs = *pl031_data;

  while (secs < 0) {
    secs += 24 * 3600;
  }

  if (hours24)
    *hours24 = (int)((secs / 3600) % 24);
  if (minutes)
    *minutes = (int)((secs / 60) % 60);
  if (seconds)
    *seconds = (int)(secs % 60);
#endif
}

static void clock_format_time(char *buf, int hours24, int minutes, int seconds) {
  buf[0] = '0' + (hours24 / 10);
  buf[1] = '0' + (hours24 % 10);
  buf[2] = ':';
  buf[3] = '0' + (minutes / 10);
  buf[4] = '0' + (minutes % 10);
  buf[5] = ':';
  buf[6] = '0' + (seconds / 10);
  buf[7] = '0' + (seconds % 10);
  buf[8] = '\0';
}

static void draw_clock_face(int cx, int cy, int radius, uint32_t face_color,
                            uint32_t rim_color, uint32_t tick_color) {
  gui_draw_circle(cx, cy, radius, face_color, true);
  gui_draw_circle(cx, cy, radius, rim_color, false);
  gui_draw_circle(cx, cy, radius - 1, rim_color, false);

  for (int i = 0; i < 60; i++) {
    int outer = radius - 4;
    int inner = (i % 5 == 0) ? radius - 14 : radius - 8;
    int x1 = cx + inner * clock_sin[i] / 256;
    int y1 = cy + inner * clock_cos[i] / 256;
    int x2 = cx + outer * clock_sin[i] / 256;
    int y2 = cy + outer * clock_cos[i] / 256;
    gui_draw_line(x1, y1, x2, y2, tick_color);
  }
}

static void draw_clock_hands(int cx, int cy, int radius, int hours24,
                             int minutes, int seconds) {
  int hours12 = hours24 % 12;
  int h_idx = (hours12 * 5 + minutes / 12) % 60;
  int hour_len = radius * 52 / 100;
  int minute_len = radius * 76 / 100;
  int second_len = radius * 84 / 100;

  int hx = cx + hour_len * clock_sin[h_idx] / 256;
  int hy = cy + hour_len * clock_cos[h_idx] / 256;
  int mx = cx + minute_len * clock_sin[minutes] / 256;
  int my = cy + minute_len * clock_cos[minutes] / 256;
  int sx = cx + second_len * clock_sin[seconds] / 256;
  int sy = cy + second_len * clock_cos[seconds] / 256;

  gui_draw_line(cx - 1, cy, hx - 1, hy, 0x202020);
  gui_draw_line(cx, cy, hx, hy, 0x202020);
  gui_draw_line(cx + 1, cy, hx + 1, hy, 0x202020);

  gui_draw_line(cx, cy, mx, my, 0x404040);
  gui_draw_line(cx + 1, cy, mx + 1, my, 0x404040);

  gui_draw_line(cx, cy, sx, sy, 0xD02020);
  gui_draw_circle(cx, cy, 4, 0xD02020, true);
}

static void draw_clock_widget(int content_x, int content_y, int content_w,
                              int content_h, uint32_t panel_bg) {
  const gui_theme_palette_t *theme = gui_theme_palette();
  int hours24, minutes, seconds;
  char time_str[9];
  int cx = content_x + content_w / 2;
  int cy = content_y + content_h / 2 - 8;
  int radius = (content_w < content_h ? content_w : content_h) / 2 - 20;

  if (radius < 28) {
    radius = 28;
  }

  clock_get_time(&hours24, &minutes, &seconds);
  clock_format_time(time_str, hours24, minutes, seconds);

  draw_clock_face(cx, cy, radius,
                  g_theme_mode == GUI_THEME_LIGHT ? 0xFFFFFF : 0xF8FAFC,
                  theme->accent,
                  g_theme_mode == GUI_THEME_LIGHT ? 0x94A3B8 : 0x334155);
  draw_clock_hands(cx, cy, radius, hours24, minutes, seconds);

  gui_draw_string(cx - 32, cy + radius + 10, time_str, theme->app_fg, panel_bg);
}

static void bowling_reset_pins(void) {
  for (int i = 0; i < BOWLING_PIN_COUNT; i++) {
    bowling_pin_standing[i] = 1;
  }
}

static void bowling_request_redraw(void) { compositor_mark_full_redraw(); }

static int bowling_count_standing_pins(void) {
  int count = 0;
  for (int i = 0; i < BOWLING_PIN_COUNT; i++) {
    if (bowling_pin_standing[i])
      count++;
  }
  return count;
}

static void bowling_prepare_next_roll(void) {
  bowling_ball_column = 3;
  bowling_ball_progress = 0;
  bowling_ball_rolling = 0;
  bowling_ball_spin = 0;
  bowling_ball_power = 3;
}

static void bowling_advance_frame(void) {
  bowling_roll = 1;
  bowling_first_roll_pins = 0;
  bowling_prepare_next_roll();
  bowling_reset_pins();
  if (bowling_frame >= 10) {
    bowling_game_over = 1;
  } else {
    bowling_frame++;
  }
  bowling_request_redraw();
}

static void bowling_init(void) {
  bowling_total_score = 0;
  bowling_last_knocked = 0;
  bowling_frame = 1;
  bowling_roll = 1;
  bowling_first_roll_pins = 0;
  bowling_game_over = 0;
  bowling_reset_pins();
  bowling_prepare_next_roll();
  bowling_request_redraw();
}

static void bowling_resolve_roll(void) {
  int impact_x = bowling_ball_column + bowling_ball_spin;
  int knocked = 0;

  if (impact_x < 0)
    impact_x = 0;
  if (impact_x >= BOWLING_LANE_COLUMNS)
    impact_x = BOWLING_LANE_COLUMNS - 1;

  for (int i = 0; i < BOWLING_PIN_COUNT; i++) {
    int dx;
    int metric;
    if (!bowling_pin_standing[i])
      continue;
    dx = bowling_pin_layout_x[i] - impact_x;
    if (dx < 0)
      dx = -dx;
    metric = dx * 2 + bowling_pin_layout_y[i];
    if (metric <= bowling_ball_power + 1) {
      bowling_pin_standing[i] = 0;
      knocked++;
    }
  }

  bowling_last_knocked = knocked;
  bowling_total_score += knocked;

  if (bowling_roll == 1) {
    bowling_first_roll_pins = knocked;
    if (knocked == 10) {
      bowling_advance_frame();
      return;
    }
    bowling_roll = 2;
    bowling_prepare_next_roll();
    bowling_request_redraw();
    return;
  }

  bowling_advance_frame();
}

static void bowling_update(void) {
  if (!bowling_ball_rolling || bowling_game_over)
    return;

  bowling_ball_progress += bowling_ball_power + 2;
  bowling_request_redraw();
  if (bowling_ball_progress >= BOWLING_LANE_PROGRESS_MAX) {
    bowling_ball_progress = BOWLING_LANE_PROGRESS_MAX;
    bowling_ball_rolling = 0;
    bowling_resolve_roll();
  }
}

static void bowling_key(int key) {
  if (key == 'r' || key == 'R') {
    bowling_init();
    return;
  }

  if (bowling_game_over || bowling_ball_rolling)
    return;

  if (key == 0x102 || key == 'a' || key == 'A') {
    if (bowling_ball_column > 0)
      bowling_ball_column--;
  } else if (key == 0x103 || key == 'd' || key == 'D') {
    if (bowling_ball_column < BOWLING_LANE_COLUMNS - 1)
      bowling_ball_column++;
  } else if (key == 0x100 || key == 'w' || key == 'W') {
    if (bowling_ball_power < 5)
      bowling_ball_power++;
  } else if (key == 0x101 || key == 's' || key == 'S') {
    if (bowling_ball_power > 2)
      bowling_ball_power--;
  } else if (key == 'q' || key == 'Q') {
    bowling_ball_spin = -1;
  } else if (key == 'e' || key == 'E') {
    bowling_ball_spin = 1;
  } else if (key == 'x' || key == 'X') {
    bowling_ball_spin = 0;
  } else if (key == ' ' || key == '\n' || key == '\r') {
    bowling_ball_rolling = 1;
    bowling_ball_progress = 0;
  }
  bowling_request_redraw();
}

static void notepad_key(int key) {
  int len;

  if (notepad_dialog_mode != NOTEPAD_DIALOG_NONE) {
    if (key == 27) {
      notepad_close_dialog();
      notepad_set_status("Dialog closed");
      return;
    }
    if (key == '\n' || key == '\r') {
      notepad_confirm_dialog();
      return;
    }
    if (key == '\b' || key == 127) {
      int len = 0;
      while (notepad_dialog_input[len])
        len++;
      if (len > 0)
        notepad_dialog_input[len - 1] = '\0';
      return;
    }
    if (key >= 32 && key < 127) {
      int len = 0;
      while (notepad_dialog_input[len])
        len++;
      if (len < (int)sizeof(notepad_dialog_input) - 1) {
        notepad_dialog_input[len++] = (char)key;
        notepad_dialog_input[len] = '\0';
      }
    }
    return;
  }

  if (key == 14) { /* Ctrl+N */
    notepad_reset_document();
    return;
  }

  if (key == 15) { /* Ctrl+O */
    notepad_begin_dialog(NOTEPAD_DIALOG_OPEN);
    return;
  }

  if (key == 19) { /* Ctrl+S */
    if (notepad_filepath[0])
      notepad_save_to_path(notepad_filepath);
    else
      notepad_begin_dialog(NOTEPAD_DIALOG_SAVE);
    return;
  }

  if (key == 3) { /* ASCII 3 = Ctrl+C */
    if (notepad_copy_selection_to_clipboard())
      notepad_set_status("Copied selection");
    return;
  }

  if (key == 22) { /* ASCII 22 = Ctrl+V */
    if (clipboard_len > 0 && notepad_insert_bytes(clipboard_buffer, clipboard_len) >
                                 0) {
      notepad_mark_dirty("Pasted from clipboard");
    }
    return;
  }

  if (key == 1) { /* ASCII 1 = Ctrl+A */
    len = notepad_text_length();
    notepad_cursor = len;
    notepad_selection_anchor = 0;
    notepad_selection_cursor = len;
    notepad_set_status("Selected all");
    return;
  }

  if (key == 24) { /* Ctrl+X */
    if (notepad_copy_selection_to_clipboard() && notepad_delete_selection()) {
      notepad_mark_dirty("Cut selection to clipboard");
    }
    return;
  }

  if (key == KEY_LEFT) {
    if (notepad_cursor > 0)
      notepad_move_cursor(notepad_cursor - 1, 0);
    return;
  }

  if (key == KEY_RIGHT) {
    if (notepad_cursor < notepad_text_length())
      notepad_move_cursor(notepad_cursor + 1, 0);
    return;
  }

  if (key == KEY_UP) {
    notepad_move_cursor(notepad_move_vertical(-1), 0);
    return;
  }

  if (key == KEY_DOWN) {
    notepad_move_cursor(notepad_move_vertical(1), 0);
    return;
  }

  if (key == '\b' || key == 127) { /* Backspace */
    if (notepad_delete_selection()) {
      notepad_mark_dirty("Edited");
    } else if (notepad_cursor > 0 && notepad_delete_range(notepad_cursor - 1,
                                                           notepad_cursor)) {
      notepad_mark_dirty("Edited");
    }
  } else if (key >= 32 && key < 127) { /* Printable */
    if (notepad_insert_char((char)key) > 0)
      notepad_mark_dirty("Edited");
  } else if (key == '\n' || key == '\r') { /* Enter */
    if (notepad_insert_char('\n') > 0)
      notepad_mark_dirty("Edited");
  }
}

static void rename_key(int key) {
  if (key == '\b' || key == 127) { /* Backspace */
    if (rename_cursor > 0) {
      rename_cursor--;
      rename_text[rename_cursor] = '\0';
    }
  } else if (key >= 32 && key < 127) { /* Printable */
    if (rename_cursor < 255) {
      rename_text[rename_cursor++] = (char)key;
      rename_text[rename_cursor] = '\0';
    }
  }
}

/* Terminal key handler */
static void terminal_key(int key) {
  if (key == '\b' || key == 127) { /* Backspace */
    if (term_input_len > 0) {
      term_input_len--;
      term_input[term_input_len] = '\0';
    }
  } else if (key == '\n' || key == '\r') { /* Enter - execute command */
    if (term_input_len > 0) {
      /* Save to history */
      if (term_history_count < TERM_HISTORY_LINES) {
        for (int i = 0; i < term_input_len && i < 79; i++) {
          term_history[term_history_count][i] = term_input[i];
        }
        term_history[term_history_count]
                    [term_input_len < 79 ? term_input_len : 79] = '\0';
        term_history_count++;
      }

      /* Check for commands */
      if (term_input[0] == 'h' && term_input[1] == 'e' &&
          term_input[2] == 'l' && term_input[3] == 'p') {
        /* Help command */
      } else if (term_input[0] == 'c' && term_input[1] == 'l' &&
                 term_input[2] == 'e' && term_input[3] == 'a' &&
                 term_input[4] == 'r') {
        term_history_count = 0;
      }
      /* Clear input */
      term_input_len = 0;
      term_input[0] = '\0';
    }
  } else if (key >= 32 && key < 127) { /* Printable */
    if (term_input_len < TERM_INPUT_MAX - 1) {
      term_input[term_input_len++] = (char)key;
      term_input[term_input_len] = '\0';
    }
  }
}

/* ===================================================================== */
/* Display Driver Interface */
/* ===================================================================== */

struct display {
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint32_t bpp;
  uint32_t *framebuffer;
  uint32_t *backbuffer;
};

static struct display primary_display = {0};

struct gui_render_target {
  uint32_t *pixels;
  int width;
  int height;
  int pitch_pixels;
  int origin_x;
  int origin_y;
};

static struct gui_render_target g_render_target = {0};

static uint64_t ui_bytes_to_mib(size_t bytes) {
  return ((uint64_t)bytes) / (1024 * 1024);
}

static int ui_append_char(char *buf, int buf_size, int idx, char c) {
  if (!buf || buf_size <= 0)
    return idx;
  if (idx < buf_size - 1)
    buf[idx++] = c;
  buf[idx < buf_size ? idx : buf_size - 1] = '\0';
  return idx;
}

static int ui_append_str(char *buf, int buf_size, int idx, const char *str) {
  if (!buf || buf_size <= 0 || !str)
    return idx;
  while (*str && idx < buf_size - 1)
    buf[idx++] = *str++;
  buf[idx] = '\0';
  return idx;
}

static int ui_append_u64(char *buf, int buf_size, int idx, uint64_t value) {
  char tmp[24];
  int ti = 0;

  do {
    tmp[ti++] = (char)('0' + (value % 10));
    value /= 10;
  } while (value > 0 && ti < (int)sizeof(tmp));

  while (ti > 0 && idx < buf_size - 1)
    buf[idx++] = tmp[--ti];
  if (idx < buf_size)
    buf[idx] = '\0';
  return idx;
}

static void ui_format_uptime_string(char *buf, int buf_size) {
  uint64_t total_seconds;
  uint64_t total_minutes;
  uint64_t days;
  uint64_t hours;
  uint64_t minutes;
  int idx = 0;
  char tmp[24];
  int ti;

  if (!buf || buf_size <= 0)
    return;

  total_seconds = arch_timer_get_ms() / 1000;
  total_minutes = total_seconds / 60;
  days = total_minutes / (24 * 60);
  hours = (total_minutes / 60) % 24;
  minutes = total_minutes % 60;
  buf[0] = '\0';

  if (total_minutes == 0) {
    idx = ui_append_u64(buf, buf_size, idx, total_seconds);
    ui_append_str(buf, buf_size, idx, " s");
    return;
  }

  if (days > 0) {
    ti = 0;
    do {
      tmp[ti++] = (char)('0' + (days % 10));
      days /= 10;
    } while (days > 0 && ti < (int)sizeof(tmp) - 1);
    while (ti > 0 && idx < buf_size - 1)
      buf[idx++] = tmp[--ti];
    idx = ui_append_str(buf, buf_size, idx, " d");
    if (hours > 0 || minutes > 0)
      idx = ui_append_str(buf, buf_size, idx, "  ");
  }

  if (hours > 0) {
    ti = 0;
    do {
      tmp[ti++] = (char)('0' + (hours % 10));
      hours /= 10;
    } while (hours > 0 && ti < (int)sizeof(tmp) - 1);
    while (ti > 0 && idx < buf_size - 1)
      buf[idx++] = tmp[--ti];
    idx = ui_append_str(buf, buf_size, idx, " h");
    if (minutes > 0)
      idx = ui_append_str(buf, buf_size, idx, "  ");
  }

  ti = 0;
  do {
    tmp[ti++] = (char)('0' + (minutes % 10));
    minutes /= 10;
  } while (minutes > 0 && ti < (int)sizeof(tmp) - 1);
  while (ti > 0 && idx < buf_size - 1)
    buf[idx++] = tmp[--ti];
  idx = ui_append_str(buf, buf_size, idx, " m");
}

static void ui_build_memory_strings(char *phys_buf, int phys_buf_size,
                                    char *heap_buf, int heap_buf_size) {
  size_t phys_total = pmm_get_total_memory();
  size_t phys_free = pmm_get_free_memory();
  size_t heap_total = 0;
  size_t heap_used = 0;
  size_t heap_free = 0;
  uint64_t phys_used_mib;
  uint64_t phys_total_mib;
  uint64_t heap_used_mib;
  uint64_t heap_total_mib;
  int idx = 0;

  kmalloc_get_stats(&heap_total, &heap_used, &heap_free);
  (void)heap_free;

  phys_used_mib =
      ui_bytes_to_mib(phys_total > phys_free ? phys_total - phys_free : 0);
  phys_total_mib = ui_bytes_to_mib(phys_total);
  heap_used_mib = ui_bytes_to_mib(heap_used);
  heap_total_mib = ui_bytes_to_mib(heap_total);

  if (phys_buf && phys_buf_size > 0) {
    phys_buf[0] = '\0';
    idx = ui_append_u64(phys_buf, phys_buf_size, 0, phys_used_mib);
    idx = ui_append_str(phys_buf, phys_buf_size, idx, " MiB / ");
    idx = ui_append_u64(phys_buf, phys_buf_size, idx, phys_total_mib);
    ui_append_str(phys_buf, phys_buf_size, idx, " MiB physical");
  }

  if (heap_buf && heap_buf_size > 0) {
    heap_buf[0] = '\0';
    idx = ui_append_u64(heap_buf, heap_buf_size, 0, heap_used_mib);
    idx = ui_append_str(heap_buf, heap_buf_size, idx, " MiB / ");
    idx = ui_append_u64(heap_buf, heap_buf_size, idx, heap_total_mib);
    ui_append_str(heap_buf, heap_buf_size, idx, " MiB heap");
  }
}

static inline int wallpaper_stretch_coord(int dst, int dst_size, int src_size) {
  if (dst_size <= 1 || src_size <= 1)
    return 0;

  return (int)(((uint64_t)dst * (uint64_t)(src_size - 1)) /
               (uint64_t)(dst_size - 1));
}

/*
 * Map a destination coordinate into source image space using a centered
 * cover scale so photo wallpapers fill the screen without distortion.
 */
static inline int wallpaper_cover_coord(int dst, int dst_size, int src_size,
                                        int screen_w, int screen_h,
                                        int image_w, int image_h) {
  uint64_t scale_num;
  uint64_t scale_den;
  uint64_t scaled_size;
  uint64_t crop;
  uint64_t pos;
  int src_coord;

  if (dst_size <= 0 || src_size <= 0 || screen_w <= 0 || screen_h <= 0 ||
      image_w <= 0 || image_h <= 0)
    return 0;

  if ((uint64_t)screen_w * (uint64_t)image_h >=
      (uint64_t)screen_h * (uint64_t)image_w) {
    scale_num = (uint64_t)screen_w;
    scale_den = (uint64_t)image_w;
  } else {
    scale_num = (uint64_t)screen_h;
    scale_den = (uint64_t)image_h;
  }

  scaled_size =
      ((uint64_t)src_size * scale_num + scale_den - 1) / scale_den;
  if (scaled_size == 0)
    return 0;

  crop = scaled_size > (uint64_t)dst_size
             ? (scaled_size - (uint64_t)dst_size) / 2
             : 0;
  pos = (uint64_t)dst + crop;
  if (pos >= scaled_size)
    pos = scaled_size - 1;

  src_coord = (int)((pos * (uint64_t)src_size) / scaled_size);
  if (src_coord < 0)
    src_coord = 0;
  if (src_coord >= src_size)
    src_coord = src_size - 1;
  return src_coord;
}

/* Get wallpaper pixel color at position */
static uint32_t wallpaper_get_pixel(int x, int y, int height) {
  int idx = current_wallpaper;
  int width = (int)primary_display.width;

  if (wallpapers[idx].type == 1 && wallpaper_image.pixels && width > 0 &&
      height > 0) {
    int img_x;
    int img_y;
    if (wallpapers[idx].fit_mode == WALLPAPER_FIT_STRETCH) {
      img_x = wallpaper_stretch_coord(x, width, (int)wallpaper_image.width);
      img_y = wallpaper_stretch_coord(y, height, (int)wallpaper_image.height);
    } else {
      img_x = wallpaper_cover_coord(x, width, (int)wallpaper_image.width,
                                    width, height,
                                    (int)wallpaper_image.width,
                                    (int)wallpaper_image.height);
      img_y = wallpaper_cover_coord(y, height, (int)wallpaper_image.height,
                                    width, height,
                                    (int)wallpaper_image.width,
                                    (int)wallpaper_image.height);
    }
    if (img_x >= 0 && img_x < (int)wallpaper_image.width && img_y >= 0 &&
        img_y < (int)wallpaper_image.height) {
      return wallpaper_image.pixels[img_y * wallpaper_image.width + img_x];
    }
  }

  {
    int progress = (y * 256) / height;
    if (progress < 0)
      progress = 0;
    if (progress > 255)
      progress = 255;

    uint8_t r = wallpapers[idx].tr +
                ((wallpapers[idx].br - wallpapers[idx].tr) * progress) / 256;
    uint8_t g = wallpapers[idx].tg +
                ((wallpapers[idx].bg - wallpapers[idx].tg) * progress) / 256;
    uint8_t b = wallpapers[idx].tb +
                ((wallpapers[idx].bb - wallpapers[idx].tb) * progress) / 256;

    return (r << 16) | (g << 8) | b;
  }
}

struct gui_clip_state {
  int enabled;
  int x0;
  int y0;
  int x1;
  int y1;
};

static struct gui_clip_state g_clip = {0, 0, 0, 0, 0};

static struct gui_clip_state gui_set_clip_rect(int x, int y, int w, int h) {
  struct gui_clip_state prev = g_clip;
  int x1 = x + w;
  int y1 = y + h;

  g_clip.enabled = 1;
  g_clip.x0 = x;
  g_clip.y0 = y;
  g_clip.x1 = x1;
  g_clip.y1 = y1;

  if (prev.enabled) {
    if (g_clip.x0 < prev.x0)
      g_clip.x0 = prev.x0;
    if (g_clip.y0 < prev.y0)
      g_clip.y0 = prev.y0;
    if (g_clip.x1 > prev.x1)
      g_clip.x1 = prev.x1;
    if (g_clip.y1 > prev.y1)
      g_clip.y1 = prev.y1;
  }

  return prev;
}

static void gui_restore_clip_rect(struct gui_clip_state prev) { g_clip = prev; }

static int gui_target_visible_rect(int src_x, int src_y, int src_w, int src_h,
                                   int *clip_x, int *clip_y, int *clip_w,
                                   int *clip_h);

static struct gui_render_target
gui_set_render_target(uint32_t *pixels, int width, int height, int pitch_pixels,
                      int origin_x, int origin_y) {
  struct gui_render_target prev = g_render_target;

  g_render_target.pixels = pixels;
  g_render_target.width = width;
  g_render_target.height = height;
  g_render_target.pitch_pixels = pitch_pixels;
  g_render_target.origin_x = origin_x;
  g_render_target.origin_y = origin_y;
  return prev;
}

static void gui_use_display_render_target(void) {
  uint32_t *pixels = primary_display.backbuffer ? primary_display.backbuffer
                                                : primary_display.framebuffer;
  (void)gui_set_render_target(pixels, (int)primary_display.width,
                              (int)primary_display.height,
                              (int)(primary_display.pitch / 4), 0, 0);
}

/* ===================================================================== */
/* Basic Drawing Functions */
/* ===================================================================== */

static inline void draw_pixel(int x, int y, uint32_t color) {
  if (g_clip.enabled &&
      (x < g_clip.x0 || x >= g_clip.x1 || y < g_clip.y0 || y >= g_clip.y1))
    return;

  if (!g_render_target.pixels)
    return;

  x -= g_render_target.origin_x;
  y -= g_render_target.origin_y;

  if (x < 0 || x >= g_render_target.width)
    return;
  if (y < 0 || y >= g_render_target.height)
    return;

  g_render_target.pixels[y * g_render_target.pitch_pixels + x] = color;
}

static inline uint32_t gui_blend_rgb_over(uint32_t dst_color, uint32_t src_color,
                                          uint32_t alpha) {
  uint32_t src_r = (src_color >> 16) & 0xFF;
  uint32_t src_g = (src_color >> 8) & 0xFF;
  uint32_t src_b = src_color & 0xFF;
  uint32_t dst_r = (dst_color >> 16) & 0xFF;
  uint32_t dst_g = (dst_color >> 8) & 0xFF;
  uint32_t dst_b = dst_color & 0xFF;
  uint32_t inv_alpha = 255 - alpha;
  uint32_t out_r = (src_r * alpha + dst_r * inv_alpha) / 255;
  uint32_t out_g = (src_g * alpha + dst_g * inv_alpha) / 255;
  uint32_t out_b = (src_b * alpha + dst_b * inv_alpha) / 255;
  return (out_r << 16) | (out_g << 8) | out_b;
}

static inline void gui_plot_local_pixel(uint32_t *target, int pitch, int x, int y,
                                        int clip_x0, int clip_y0, int clip_x1,
                                        int clip_y1, uint32_t color) {
  if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1)
    return;
  target[y * pitch + x] = color;
}

static inline void draw_pixel_alpha(int x, int y, uint32_t color) {
  if (g_clip.enabled &&
      (x < g_clip.x0 || x >= g_clip.x1 || y < g_clip.y0 || y >= g_clip.y1))
    return;

  if (!g_render_target.pixels)
    return;

  x -= g_render_target.origin_x;
  y -= g_render_target.origin_y;

  if (x < 0 || x >= g_render_target.width)
    return;
  if (y < 0 || y >= g_render_target.height)
    return;

  uint32_t alpha = (color >> 24) & 0xFF;
  if (alpha == 0) {
    return;
  }
  if (alpha == 0xFF) {
    g_render_target.pixels[y * g_render_target.pitch_pixels + x] =
        color & 0xFFFFFF;
    return;
  }

  uint32_t *dst = &g_render_target.pixels[y * g_render_target.pitch_pixels + x];
  *dst = gui_blend_rgb_over(*dst, color, alpha);
}

static inline uint32_t *gui_draw_target(void) {
  return g_render_target.pixels;
}

static inline void draw_image_pixel(int x, int y, uint32_t color) {
  uint32_t alpha = color >> 24;

  if (g_clip.enabled &&
      (x < g_clip.x0 || x >= g_clip.x1 || y < g_clip.y0 || y >= g_clip.y1))
    return;

  if (!g_render_target.pixels)
    return;

  x -= g_render_target.origin_x;
  y -= g_render_target.origin_y;

  if (x < 0 || x >= g_render_target.width)
    return;
  if (y < 0 || y >= g_render_target.height)
    return;

  if (alpha == 0)
    return;

  uint32_t *dst = &g_render_target.pixels[y * g_render_target.pitch_pixels + x];
  if (alpha == 0xFF) {
    *dst = color & 0xFFFFFF;
  } else {
    *dst = gui_blend_rgb_over(*dst, color, alpha);
  }
}

static void gui_fill_rect_alpha(int x, int y, int w, int h, uint32_t color) {
  int clip_x;
  int clip_y;
  int clip_w;
  int clip_h;

  if (!gui_target_visible_rect(x, y, w, h, &clip_x, &clip_y, &clip_w, &clip_h))
    return;

  uint32_t *target = gui_draw_target();
  if (!target)
    return;

  uint32_t alpha = (color >> 24) & 0xFF;
  if (alpha == 0)
    return;

  int local_x = clip_x - g_render_target.origin_x;
  int local_y = clip_y - g_render_target.origin_y;
  int pitch = g_render_target.pitch_pixels;

  if (alpha == 0xFF) {
    uint32_t solid = color & 0xFFFFFF;

    for (int row = 0; row < clip_h; row++) {
      uint32_t *dst = target + (local_y + row) * pitch + local_x;
      for (int col = 0; col < clip_w; col++) {
        dst[col] = solid;
      }
    }
    return;
  }

  for (int row = 0; row < clip_h; row++) {
    uint32_t *dst = target + (local_y + row) * pitch + local_x;
    for (int col = 0; col < clip_w; col++) {
      dst[col] = gui_blend_rgb_over(dst[col], color, alpha);
    }
  }
}

static void gui_apply_backdrop_blur(int x, int y, int w, int h, int stride) {
  uint32_t *target = gui_draw_target();
  int clip_x;
  int clip_y;
  int clip_w;
  int clip_h;
  if (!target || w <= 0 || h <= 0)
    return;

  if (!gui_target_visible_rect(x, y, w, h, &clip_x, &clip_y, &clip_w, &clip_h))
    return;

  int pitch = g_render_target.pitch_pixels;
  if (stride < 1)
    stride = 1;

  for (int row = clip_y; row < clip_y + clip_h; row += stride) {
    int local_row = row - g_render_target.origin_y;
    for (int col = clip_x; col < clip_x + clip_w; col += stride) {
      int local_col = col - g_render_target.origin_x;

      uint32_t sum_r = 0, sum_g = 0, sum_b = 0, count = 0;
      for (int sy = -2; sy <= 2; sy += 2) {
        for (int sx = -2; sx <= 2; sx += 2) {
          int sample_x = col + sx;
          int sample_y = row + sy;
          int local_sample_x = sample_x - g_render_target.origin_x;
          int local_sample_y = sample_y - g_render_target.origin_y;
          if (local_sample_x < 0 || local_sample_x >= g_render_target.width ||
              local_sample_y < 0 || local_sample_y >= g_render_target.height)
            continue;
          uint32_t px = target[local_sample_y * pitch + local_sample_x];
          sum_r += (px >> 16) & 0xFF;
          sum_g += (px >> 8) & 0xFF;
          sum_b += px & 0xFF;
          count++;
        }
      }

      if (!count)
        continue;

      uint32_t blurred =
          (((sum_r / count) & 0xFF) << 16) | (((sum_g / count) & 0xFF) << 8) |
          ((sum_b / count) & 0xFF);

      int fill_h = stride;
      int fill_w = stride;
      if (row + fill_h > clip_y + clip_h)
        fill_h = clip_y + clip_h - row;
      if (col + fill_w > clip_x + clip_w)
        fill_w = clip_x + clip_w - col;

      for (int fy = 0; fy < fill_h; fy++) {
        uint32_t *dst = target + (local_row + fy) * pitch + local_col;
        for (int fx = 0; fx < fill_w; fx++) {
          dst[fx] = blurred;
        }
      }
    }
  }
}

static int gui_adjust_blur_stride_for_area(int w, int h, int stride) {
  uint64_t area;

  if (stride < 1)
    stride = 1;
  if (w <= 0 || h <= 0)
    return stride;

  area = (uint64_t)w * (uint64_t)h;
  if (area >= 320000)
    return 0;
  if (area >= 180000 && stride < 4)
    return 4;
  if (area >= 90000 && stride < 3)
    return 3;
  return stride;
}

static void gui_draw_glass_panel(int x, int y, int w, int h, uint32_t tint,
                                 uint32_t glow, uint32_t border,
                                 int blur_stride) {
  if (g_blur_effects_enabled) {
    blur_stride = gui_adjust_blur_stride_for_area(w, h, blur_stride);
    if (blur_stride > 0)
      gui_apply_backdrop_blur(x, y, w, h, blur_stride);
  }
  if (g_blur_effects_mode != GUI_BLUR_EFFECTS_OFF) {
    gui_fill_rect_alpha(x, y, w, h, tint);
    gui_fill_rect_alpha(x, y, w, 1, glow);
    gui_fill_rect_alpha(x, y, 1, h, glow);
    gui_fill_rect_alpha(x, y + h - 1, w, 1, border);
    gui_fill_rect_alpha(x + w - 1, y, 1, h, border);
    return;
  }

  gui_draw_rect(x, y, w, h, gui_make_opaque_color(tint));
  gui_draw_rect(x, y, w, 1, gui_make_opaque_color(glow));
  gui_draw_rect(x, y, 1, h, gui_make_opaque_color(glow));
  gui_draw_rect(x, y + h - 1, w, 1, gui_make_opaque_color(border));
  gui_draw_rect(x + w - 1, y, 1, h, gui_make_opaque_color(border));
}

typedef enum {
  GUI_BUTTON_NEUTRAL = 0,
  GUI_BUTTON_PRIMARY = 1,
  GUI_BUTTON_SUCCESS = 2,
  GUI_BUTTON_DANGER = 3
} gui_button_variant_t;

static uint32_t gui_argb(uint8_t alpha, uint32_t rgb) {
  return ((uint32_t)alpha << 24) | (rgb & 0xFFFFFF);
}

static uint32_t gui_make_opaque_color(uint32_t color) {
  return 0xFF000000 | (color & 0x00FFFFFF);
}

static void gui_fill_visual_effect_rect(int x, int y, int w, int h,
                                        uint32_t color) {
  if (g_blur_effects_mode != GUI_BLUR_EFFECTS_OFF) {
    gui_fill_rect_alpha(x, y, w, h, color);
    return;
  }

  gui_draw_rect(x, y, w, h, gui_make_opaque_color(color));
}

static void gui_draw_system_button(int x, int y, int w, int h,
                                   const char *label,
                                   gui_button_variant_t variant, int enabled,
                                   int active) {
  const gui_theme_palette_t *theme = gui_theme_palette();
  uint32_t base = theme->surface_alt;
  uint32_t tint;
  uint32_t glow;
  uint32_t border;
  uint32_t text = enabled ? 0xFFFFFF : theme->app_muted;
  uint32_t shadow = 0x00000000;
  int label_len = 0;
  int label_x;
  int label_y;

  if (!label || w <= 0 || h <= 0)
    return;

  switch (variant) {
  case GUI_BUTTON_PRIMARY:
    base = theme->accent;
    text = gui_contrast_title_color(base);
    break;
  case GUI_BUTTON_SUCCESS:
    base = 0x16A34A;
    text = gui_contrast_title_color(base);
    break;
  case GUI_BUTTON_DANGER:
    base = 0xB42318;
    text = gui_contrast_title_color(base);
    break;
  case GUI_BUTTON_NEUTRAL:
  default:
    base = theme->surface_alt;
    text = enabled ? gui_contrast_title_color(base) : theme->app_muted;
    break;
  }

  if (!enabled) {
    tint = gui_argb(0xB8, theme->surface_alt);
    glow = gui_argb(0x22, 0xFFFFFF);
    border = gui_argb(0x70, theme->border);
  } else if (variant == GUI_BUTTON_NEUTRAL) {
    tint = gui_argb(active ? 0xD2 : 0xBA, base);
    glow = gui_argb(active ? 0x3E : 0x24, 0xFFFFFF);
    border = gui_argb(active ? 0xB4 : 0x8A, theme->border);
    if (active)
      text = 0xFFFFFF;
  } else {
    tint = gui_argb(active ? 0xDE : 0xCA, base);
    glow = gui_argb(active ? 0x40 : 0x28, 0xFFFFFF);
    border = gui_argb(active ? 0xC8 : 0x96, base);
  }

  shadow = enabled ? gui_argb(0x70, text == 0xFFFFFF ? 0x000000 : 0xFFFFFF)
                   : 0x00000000;

  gui_draw_glass_panel(x, y, w, h, tint, glow, border, 2);
  gui_fill_rect_alpha(x + 1, y + 1, w - 2, h / 2, gui_argb(0x18, 0xFFFFFF));

  while (label[label_len])
    label_len++;
  label_x = x + (w - label_len * 8) / 2;
  label_y = y + (h - 16) / 2;
  if (enabled) {
    gui_draw_string(label_x + 1, label_y + 1, label, shadow, 0x00000000);
  }
  gui_draw_string(label_x, label_y, label, text, 0x00000000);
}

void gui_draw_rect(int x, int y, int w, int h, uint32_t color) {
  int clip_x;
  int clip_y;
  int clip_w;
  int clip_h;
  int dst_x;
  int dst_y;
  uint32_t *target = gui_draw_target();

  if (!gui_target_visible_rect(x, y, w, h, &clip_x, &clip_y, &clip_w, &clip_h))
    return;
  if (!target)
    return;

  dst_x = clip_x - g_render_target.origin_x;
  dst_y = clip_y - g_render_target.origin_y;
  for (int row = 0; row < clip_h; row++) {
    uint32_t *dst =
        target + (dst_y + row) * g_render_target.pitch_pixels + dst_x;
    fast_fill_line(dst, clip_w, color);
  }
}

void gui_draw_rect_outline(int x, int y, int w, int h, uint32_t color,
                           int thickness) {
  /* Top */
  gui_draw_rect(x, y, w, thickness, color);
  /* Bottom */
  gui_draw_rect(x, y + h - thickness, w, thickness, color);
  /* Left */
  gui_draw_rect(x, y, thickness, h, color);
  /* Right */
  gui_draw_rect(x + w - thickness, y, thickness, h, color);
}

void gui_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
  int clip_x;
  int clip_y;
  int clip_w;
  int clip_h;
  uint32_t *target;
  int clip_x0;
  int clip_y0;
  int clip_x1;
  int clip_y1;
  int pitch;

  if (y0 == y1) {
    int start_x = x0 < x1 ? x0 : x1;
    int width = (x0 < x1 ? x1 - x0 : x0 - x1) + 1;
    gui_draw_rect(start_x, y0, width, 1, color);
    return;
  }

  if (x0 == x1) {
    int start_y = y0 < y1 ? y0 : y1;
    int height = (y0 < y1 ? y1 - y0 : y0 - y1) + 1;
    gui_draw_rect(x0, start_y, 1, height, color);
    return;
  }

  if (!gui_target_visible_rect((x0 < x1 ? x0 : x1), (y0 < y1 ? y0 : y1),
                               ((x0 < x1 ? x1 - x0 : x0 - x1) + 1),
                               ((y0 < y1 ? y1 - y0 : y0 - y1) + 1), &clip_x,
                               &clip_y, &clip_w, &clip_h))
    return;

  target = gui_draw_target();
  if (!target)
    return;

  clip_x0 = clip_x - g_render_target.origin_x;
  clip_y0 = clip_y - g_render_target.origin_y;
  clip_x1 = clip_x0 + clip_w;
  clip_y1 = clip_y0 + clip_h;
  pitch = g_render_target.pitch_pixels;

  int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
  int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
  int sx = (x0 < x1) ? 1 : -1;
  int sy = (y0 < y1) ? 1 : -1;
  int err = dx - dy;

  while (1) {
    int local_x = x0 - g_render_target.origin_x;
    int local_y = y0 - g_render_target.origin_y;
    if (local_x >= clip_x0 && local_x < clip_x1 && local_y >= clip_y0 &&
        local_y < clip_y1) {
      target[local_y * pitch + local_x] = color;
    }
    if (x0 == x1 && y0 == y1)
      break;
    int e2 = 2 * err;
    if (e2 > -dy) {
      err -= dy;
      x0 += sx;
    }
    if (e2 < dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void gui_draw_circle(int cx, int cy, int r, uint32_t color, bool filled) {
  int x = 0, y = r;
  int d = 3 - 2 * r;
  int clip_x;
  int clip_y;
  int clip_w;
  int clip_h;
  uint32_t *target;
  int local_cx;
  int local_cy;
  int clip_x0;
  int clip_y0;
  int clip_x1;
  int clip_y1;
  int pitch;

  if (r < 0)
    return;

  if (filled) {
    while (y >= x) {
      gui_draw_line(cx - x, cy + y, cx + x, cy + y, color);
      gui_draw_line(cx - x, cy - y, cx + x, cy - y, color);
      gui_draw_line(cx - y, cy + x, cx + y, cy + x, color);
      gui_draw_line(cx - y, cy - x, cx + y, cy - x, color);

      x++;
      if (d > 0) {
        y--;
        d = d + 4 * (x - y) + 10;
      } else {
        d = d + 4 * x + 6;
      }
    }
    return;
  }

  if (!gui_target_visible_rect(cx - r, cy - r, r * 2 + 1, r * 2 + 1, &clip_x,
                               &clip_y, &clip_w, &clip_h))
    return;

  target = gui_draw_target();
  if (!target)
    return;

  local_cx = cx - g_render_target.origin_x;
  local_cy = cy - g_render_target.origin_y;
  clip_x0 = clip_x - g_render_target.origin_x;
  clip_y0 = clip_y - g_render_target.origin_y;
  clip_x1 = clip_x0 + clip_w;
  clip_y1 = clip_y0 + clip_h;
  pitch = g_render_target.pitch_pixels;

  while (y >= x) {
    gui_plot_local_pixel(target, pitch, local_cx + x, local_cy + y, clip_x0,
                         clip_y0, clip_x1, clip_y1, color);
    gui_plot_local_pixel(target, pitch, local_cx - x, local_cy + y, clip_x0,
                         clip_y0, clip_x1, clip_y1, color);
    gui_plot_local_pixel(target, pitch, local_cx + x, local_cy - y, clip_x0,
                         clip_y0, clip_x1, clip_y1, color);
    gui_plot_local_pixel(target, pitch, local_cx - x, local_cy - y, clip_x0,
                         clip_y0, clip_x1, clip_y1, color);

    if (x != y) {
      gui_plot_local_pixel(target, pitch, local_cx + y, local_cy + x, clip_x0,
                           clip_y0, clip_x1, clip_y1, color);
      gui_plot_local_pixel(target, pitch, local_cx - y, local_cy + x, clip_x0,
                           clip_y0, clip_x1, clip_y1, color);
      gui_plot_local_pixel(target, pitch, local_cx + y, local_cy - x, clip_x0,
                           clip_y0, clip_x1, clip_y1, color);
      gui_plot_local_pixel(target, pitch, local_cx - y, local_cy - x, clip_x0,
                           clip_y0, clip_x1, clip_y1, color);
    }

    x++;
    if (d > 0) {
      y--;
      d = d + 4 * (x - y) + 10;
    } else {
      d = d + 4 * x + 6;
    }
  }
}

static void gui_draw_os_logo(int x, int y, int scale, uint32_t fg,
                             uint32_t accent, uint32_t bg) {
  int s = scale < 1 ? 1 : scale;
  int outer = 14 * s;
  int inset = 3 * s;
  int inner = outer - inset * 2;
  int cutout = inner / 2;
  int cutout_x = x + (outer - cutout) / 2;
  int cutout_y = y + (outer - cutout) / 2;
  int has_bg = bg != 0x00000000;

  if (has_bg)
    gui_draw_rect(x, y, outer, outer, bg);
  gui_draw_rect_outline(x, y, outer, outer, fg, s);
  gui_draw_rect(x + inset, y + inset, inner, inner, accent);

  if (has_bg) {
    gui_draw_rect(cutout_x, cutout_y, cutout, cutout, bg);
  } else {
    gui_draw_rect_outline(cutout_x, cutout_y, cutout, cutout, fg, s > 1 ? 2 : 1);
  }

  for (int i = 0; i < s; i++) {
    gui_draw_line(x + 2 * s, y + 2 * s + i, x + outer - 3 * s, y + outer - 3 * s + i,
                  fg);
    gui_draw_line(x + outer - 3 * s, y + 2 * s + i, x + 2 * s, y + outer - 3 * s + i,
                  fg);
  }
}

static void gui_draw_image_scaled(int x, int y, int w, int h,
                                  const media_image_t *image) {
  int clip_x;
  int clip_y;
  int clip_w;
  int clip_h;

  if (!image || !image->pixels || image->width == 0 || image->height == 0 ||
      w <= 0 || h <= 0)
    return;

  if (!gui_target_visible_rect(x, y, w, h, &clip_x, &clip_y, &clip_w, &clip_h))
    return;

  uint32_t *target = gui_draw_target();
  if (!target)
    return;

  int local_x = clip_x - g_render_target.origin_x;
  int local_y = clip_y - g_render_target.origin_y;
  int start_dx = clip_x - x;
  int start_dy = clip_y - y;
  int pitch = g_render_target.pitch_pixels;

  for (int dy = 0; dy < clip_h; dy++) {
    uint32_t src_y =
        ((uint32_t)(start_dy + dy) * image->height) / (uint32_t)h;
    const uint32_t *src_row = image->pixels + src_y * image->width;
    uint32_t *dst_row = target + (local_y + dy) * pitch + local_x;

    for (int dx = 0; dx < clip_w; dx++) {
      uint32_t src_x =
          ((uint32_t)(start_dx + dx) * image->width) / (uint32_t)w;
      uint32_t color = src_row[src_x];
      uint32_t alpha = color >> 24;

      if (alpha == 0)
        continue;
      if (alpha == 0xFF) {
        dst_row[dx] = color & 0xFFFFFF;
      } else {
        dst_row[dx] = gui_blend_rgb_over(dst_row[dx], color, alpha);
      }
    }
  }
}

static int gui_draw_embedded_logo(int x, int y, int w, int h) {
  const media_image_t *embedded_logo;

  if (w <= 0 || h <= 0)
    return -1;

  embedded_logo = boot_splash_get_logo();
  if (!embedded_logo || !embedded_logo->pixels)
    return -1;

  gui_draw_image_scaled(x, y, w, h, embedded_logo);
  return 0;
}

void gui_wait_for_boot_splash(uint64_t duration_ms) {
  (void)duration_ms;
  return;

  uint64_t start_ms = arch_timer_get_ms();
  uint64_t last_ms = start_ms;
  uint32_t stalled_loops = 0;
  const uint32_t max_stalled_loops = 20000000;

  while ((arch_timer_get_ms() - start_ms) < duration_ms) {
    uint64_t now_ms = arch_timer_get_ms();
    if (now_ms != last_ms) {
      last_ms = now_ms;
      stalled_loops = 0;
      continue;
    }

    stalled_loops++;
    if (stalled_loops >= max_stalled_loops) {
      /* Timer is not advancing yet on this boot path, so don't deadlock. */
      break;
    }
  }
}

static int gui_min3(int a, int b, int c) {
  int min = a < b ? a : b;
  return min < c ? min : c;
}

static int gui_max3(int a, int b, int c) {
  int max = a > b ? a : b;
  return max > c ? max : c;
}

static int gui_triangle_edge(int ax, int ay, int bx, int by, int cx, int cy) {
  return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

static void gui_fill_triangle(int x1, int y1, int x2, int y2, int x3, int y3,
                              uint32_t color) {
  int min_x = gui_min3(x1, x2, x3);
  int max_x = gui_max3(x1, x2, x3);
  int min_y = gui_min3(y1, y2, y3);
  int max_y = gui_max3(y1, y2, y3);
  int area = gui_triangle_edge(x1, y1, x2, y2, x3, y3);
  int clip_x;
  int clip_y;
  int clip_w;
  int clip_h;

  if (area == 0)
    return;

  if (!gui_target_visible_rect(min_x, min_y, max_x - min_x + 1,
                               max_y - min_y + 1, &clip_x, &clip_y, &clip_w,
                               &clip_h))
    return;

  uint32_t *target = gui_draw_target();
  if (!target)
    return;

  uint32_t alpha = (color >> 24) & 0xFF;
  if (alpha == 0)
    return;

  int local_x0 = clip_x - g_render_target.origin_x;
  int local_y0 = clip_y - g_render_target.origin_y;
  int pitch = g_render_target.pitch_pixels;

  for (int row = 0; row < clip_h; row++) {
    int y = clip_y + row;
    int span_start = -1;
    int span_end = -1;

    for (int x = clip_x; x < clip_x + clip_w; x++) {
      int w0 = gui_triangle_edge(x2, y2, x3, y3, x, y);
      int w1 = gui_triangle_edge(x3, y3, x1, y1, x, y);
      int w2 = gui_triangle_edge(x1, y1, x2, y2, x, y);

      if ((area > 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) ||
          (area < 0 && w0 <= 0 && w1 <= 0 && w2 <= 0)) {
        if (span_start < 0)
          span_start = x;
        span_end = x;
      }
    }

    if (span_start < 0)
      continue;

    uint32_t *dst =
        target + (local_y0 + row) * pitch + (local_x0 + (span_start - clip_x));
    int span_w = span_end - span_start + 1;

    if (alpha == 0xFF) {
      uint32_t solid = color & 0xFFFFFF;
      for (int col = 0; col < span_w; col++) {
        dst[col] = solid;
      }
    } else {
      for (int col = 0; col < span_w; col++) {
        dst[col] = gui_blend_rgb_over(dst[col], color, alpha);
      }
    }
  }
}

static void gui_stage_point_to_screen(int stage_x, int stage_y, int stage_x0,
                                      int stage_y0, int stage_scale,
                                      int *screen_x, int *screen_y) {
  if (screen_x) {
    *screen_x = stage_x0 + ((stage_x + 240) * stage_scale);
  }
  if (screen_y) {
    *screen_y = stage_y0 + ((180 - stage_y) * stage_scale);
  }
}

static void gui_draw_boot_logo_stamp(const media_image_t *logo, int center_x,
                                     int center_y, int logo_w, int brightness,
                                     int glow_alpha) {
  int logo_h;
  int x;
  int y;
  int pad = 0;

  if (logo && logo->pixels && logo->width && logo->height) {
    logo_h = (int)(((uint64_t)logo_w * logo->height) / logo->width);
    if (logo_h <= 0)
      logo_h = 1;
    x = center_x - logo_w / 2;
    y = center_y - logo_h / 2;
    pad = logo_w / 40;
    if (pad < 2)
      pad = 2;

    if (glow_alpha > 0) {
      int glow = gui_argb((uint8_t)glow_alpha, 0xFFFFFF);
      gui_fill_rect_alpha(x - pad, y - pad, logo_w + pad * 2, pad, glow);
      gui_fill_rect_alpha(x - pad, y + logo_h, logo_w + pad * 2, pad, glow);
      gui_fill_rect_alpha(x - pad, y, pad, logo_h, glow);
      gui_fill_rect_alpha(x + logo_w, y, pad, logo_h, glow);
    }

    gui_draw_image_scaled(x, y, logo_w, logo_h, logo);

    if (brightness < 0) {
      int alpha = (-brightness * 255) / 100;
      if (alpha > 255)
        alpha = 255;
      gui_fill_rect_alpha(x, y, logo_w, logo_h, gui_argb((uint8_t)alpha, 0x000000));
    }
    return;
  }

  {
    int fallback_scale = logo_w / 14;
    if (fallback_scale < 1)
      fallback_scale = 1;
    x = center_x - (14 * fallback_scale) / 2;
    y = center_y - (14 * fallback_scale) / 2;
    gui_draw_os_logo(x, y, fallback_scale, 0xFFFFFF, 0x89B4FA,
                     0x00000000);
    if (brightness < 0) {
      int alpha = (-brightness * 255) / 100;
      if (alpha > 255)
        alpha = 255;
      gui_fill_rect_alpha(x, y, 14 * fallback_scale, 14 * fallback_scale,
                          gui_argb((uint8_t)alpha, 0x000000));
    }
  }
}

void gui_play_old_boot_sequence(uint32_t width, uint32_t height) {
  (void)width;
  (void)height;
  return;

  const media_image_t *boot_logo = boot_splash_get_logo();
  int stage_scale;
  int stage_w;
  int stage_h;
  int stage_x;
  int stage_y;
  int logo_w;

  stage_scale = (int)(width / 480);
  if ((int)(height / 360) < stage_scale)
    stage_scale = (int)(height / 360);
  if (stage_scale < 1)
    stage_scale = 1;

  stage_w = 480 * stage_scale;
  stage_h = 360 * stage_scale;
  stage_x = ((int)width - stage_w) / 2;
  stage_y = ((int)height - stage_h) / 2;
  logo_w = 120 * stage_scale;

  /* Match the old script's short startup pause. */
  gui_wait_for_boot_splash(120);

  for (int frame = 0; frame < 20; frame++) {
    int sprite_x = -55 + (frame * 55) / 19;
    int brightness = -100 + (frame * 100) / 19;
    int center_x;
    int center_y;
    int tx1;
    int ty1;
    int tx2;
    int ty2;
    int tx3;
    int ty3;

    gui_draw_rect(0, 0, (int)width, (int)height, 0x000000);

    gui_stage_point_to_screen(-200, -100, stage_x, stage_y, stage_scale,
                              &tx1, &ty1);
    gui_stage_point_to_screen(200, -200, stage_x, stage_y, stage_scale,
                              &tx2, &ty2);
    gui_stage_point_to_screen(200, 0, stage_x, stage_y, stage_scale,
                              &tx3, &ty3);
    gui_fill_triangle(tx1, ty1, tx2, ty2, tx3, ty3, gui_argb(235, 0x05070A));
    gui_draw_line(tx1, ty1, tx2, ty2, 0x0B0D10);
    gui_draw_line(tx2, ty2, tx3, ty3, 0x0B0D10);
    gui_draw_line(tx3, ty3, tx1, ty1, 0x0B0D10);

    gui_stage_point_to_screen(sprite_x, 0, stage_x, stage_y, stage_scale,
                              &center_x, &center_y);
    gui_draw_boot_logo_stamp(boot_logo, center_x, center_y, logo_w, brightness,
                             0);

    if (frame < 3) {
      gui_draw_boot_logo_stamp(boot_logo, center_x + stage_scale * 3,
                               center_y, logo_w, brightness + 8, 24);
    }

    gui_wait_for_boot_splash(28);
  }

  {
    int center_x;
    int center_y;
    int tx1;
    int ty1;
    int tx2;
    int ty2;
    int tx3;
    int ty3;

    gui_draw_rect(0, 0, (int)width, (int)height, 0x000000);

    gui_stage_point_to_screen(-200, -100, stage_x, stage_y, stage_scale,
                              &tx1, &ty1);
    gui_stage_point_to_screen(200, -200, stage_x, stage_y, stage_scale,
                              &tx2, &ty2);
    gui_stage_point_to_screen(200, 0, stage_x, stage_y, stage_scale,
                              &tx3, &ty3);
    gui_fill_triangle(tx1, ty1, tx2, ty2, tx3, ty3, gui_argb(235, 0x05070A));
    gui_draw_line(tx1, ty1, tx2, ty2, 0x0B0D10);
    gui_draw_line(tx2, ty2, tx3, ty3, 0x0B0D10);
    gui_draw_line(tx3, ty3, tx1, ty1, 0x0B0D10);

    gui_stage_point_to_screen(0, 0, stage_x, stage_y, stage_scale, &center_x,
                              &center_y);
    gui_draw_boot_logo_stamp(boot_logo, center_x, center_y, logo_w, 0, 40);
    gui_draw_boot_logo_stamp(boot_logo, center_x + stage_scale, center_y,
                             logo_w + stage_scale * 6, 0, 64);
    gui_draw_boot_logo_stamp(boot_logo, center_x, center_y, logo_w, 0, 0);
  }

  gui_wait_for_boot_splash(180);
}

/* ===================================================================== */
/* 8x16 Font - use external complete font */
/* ===================================================================== */

#define FONT_WIDTH 8
#define FONT_HEIGHT 16

/* External font data from font.c - 256 characters */
extern const uint8_t font_data[256][16];

void gui_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
  int clip_x;
  int clip_y;
  int clip_w;
  int clip_h;

  if (!gui_target_visible_rect(x, y, FONT_WIDTH, FONT_HEIGHT, &clip_x, &clip_y,
                               &clip_w, &clip_h)) {
    return;
  }

  uint32_t *target = gui_draw_target();
  if (!target)
    return;

  unsigned char idx = (unsigned char)c;
  const uint8_t *glyph = font_data[idx];
  int local_x = clip_x - g_render_target.origin_x;
  int local_y = clip_y - g_render_target.origin_y;
  int start_col = clip_x - x;
  int start_row = clip_y - y;
  uint32_t fg_alpha = (fg >> 24) & 0xFF;
  uint32_t bg_alpha = (bg >> 24) & 0xFF;

  for (int row = 0; row < clip_h; row++) {
    uint8_t line = glyph[start_row + row];
    uint32_t *dst =
        target + (local_y + row) * g_render_target.pitch_pixels + local_x;

    for (int col = 0; col < clip_w; col++) {
      int glyph_col = start_col + col;
      uint8_t mask = (uint8_t)(0x80 >> glyph_col);
      int bit_set = (line & mask) != 0;
      uint32_t color = bit_set ? fg : bg;
      uint32_t alpha = bit_set ? fg_alpha : bg_alpha;

      if (alpha != 0) {
        if (alpha == 0xFF) {
          dst[col] = color & 0xFFFFFF;
        } else {
          dst[col] = gui_blend_rgb_over(dst[col], color, alpha);
        }
      } else if (bit_set || bg != 0x00000000) {
        dst[col] = color;
      }
    }
  }
}

void gui_draw_string(int x, int y, const char *str, uint32_t fg, uint32_t bg) {
  int start_x = x;
  uint32_t effective_bg = ((bg >> 24) != 0) ? bg : 0x00000000;
  while (*str) {
    if (*str == '\n') {
      x = start_x;
      y += FONT_HEIGHT;
    } else {
      gui_draw_char(x, y, *str, fg, effective_bg);
      x += FONT_WIDTH;
    }
    str++;
  }
}

static void append_decimal(char *buf, int *idx, int value) {
  char digits[16];
  int count = 0;

  if (value == 0) {
    buf[(*idx)++] = '0';
    return;
  }

  if (value < 0) {
    buf[(*idx)++] = '-';
    value = -value;
  }

  while (value > 0 && count < (int)sizeof(digits)) {
    digits[count++] = '0' + (value % 10);
    value /= 10;
  }

  while (count > 0) {
    buf[(*idx)++] = digits[--count];
  }
}

static void build_resolution_string(char *buf, uint32_t width, uint32_t height) {
  int idx = 0;
  append_decimal(buf, &idx, (int)width);
  buf[idx++] = ' ';
  buf[idx++] = 'x';
  buf[idx++] = ' ';
  append_decimal(buf, &idx, (int)height);
  buf[idx] = '\0';
}

void gui_build_display_mode_string(char *buf, size_t size) {
  int idx = 0;

  if (!buf || size == 0)
    return;

  build_resolution_string(buf, primary_display.width, primary_display.height);
  while (buf[idx] && idx < (int)size - 1)
    idx++;
  if (idx < (int)size - 3) {
    buf[idx++] = ' ';
    buf[idx++] = '@';
    buf[idx++] = ' ';
  }
  if (idx < (int)size - 1)
    append_decimal(buf, &idx, (int)primary_display.bpp);
  if (idx < (int)size - 5) {
    buf[idx++] = '-';
    buf[idx++] = 'b';
    buf[idx++] = 'i';
    buf[idx++] = 't';
  }
  buf[idx < (int)size ? idx : (int)size - 1] = '\0';
}

static int settings_find_resolution_index(uint32_t width, uint32_t height) {
  for (int i = 0; i < SETTINGS_RESOLUTION_OPTION_COUNT; i++) {
    if (settings_resolution_options[i].width == width &&
        settings_resolution_options[i].height == height) {
      return i;
    }
  }
  return -1;
}

static void settings_sync_resolution_picker(void) {
  uint32_t saved_width = 0;
  uint32_t saved_height = 0;

  settings_resolution_current_idx =
      settings_find_resolution_index(primary_display.width, primary_display.height);

  if (gui_load_saved_resolution(&saved_width, &saved_height) == 0) {
    settings_resolution_saved_idx =
        settings_find_resolution_index(saved_width, saved_height);
  } else {
    settings_resolution_saved_idx = -1;
  }

  if (settings_resolution_pending_idx < 0 ||
      settings_resolution_pending_idx >= SETTINGS_RESOLUTION_OPTION_COUNT) {
    if (settings_resolution_saved_idx >= 0)
      settings_resolution_pending_idx = settings_resolution_saved_idx;
    else if (settings_resolution_current_idx >= 0)
      settings_resolution_pending_idx = settings_resolution_current_idx;
    else
      settings_resolution_pending_idx = 0;
  }
}

static int settings_resolution_button_bounds(int panel_x, int panel_y, int index,
                                             int *x, int *y, int *w, int *h) {
  int card_y = panel_y + 72 + 104 + 84;

  if (index < 0 || index >= SETTINGS_RESOLUTION_OPTION_COUNT)
    return 0;

  if (x)
    *x = panel_x + 16 + index * 92;
  if (y)
    *y = card_y + 42;
  if (w)
    *w = 84;
  if (h)
    *h = 22;
  return 1;
}

static void build_device_ports_string(char *buf, int connected, int total) {
  int idx = 0;
  append_decimal(buf, &idx, connected);
  buf[idx++] = '/';
  append_decimal(buf, &idx, total);
  buf[idx++] = ' ';
  buf[idx++] = 'p';
  buf[idx++] = 'o';
  buf[idx++] = 'r';
  buf[idx++] = 't';
  if (total != 1) {
    buf[idx++] = 's';
  }
  buf[idx] = '\0';
}

/* ===================================================================== */
/* Window System */
/* ===================================================================== */

#define MAX_WINDOWS 64
#define TITLEBAR_HEIGHT 28
#define BORDER_WIDTH 2

typedef enum {
  WINDOW_NORMAL,
  WINDOW_MINIMIZED,
  WINDOW_MAXIMIZED,
  WINDOW_FULLSCREEN
} window_state_t;

typedef enum {
  WINDOW_ANIM_NONE = 0,
  WINDOW_ANIM_OPEN,
  WINDOW_ANIM_CLOSE
} window_animation_t;

struct window {
  int id;
  char title[64];
  int x, y;
  int width, height;
  window_state_t state;
  bool visible;
  bool focused;
  bool has_titlebar;
  bool resizable;
  uint32_t *content_buffer;
  int surface_width;
  int surface_height;
  int surface_valid;
  int surface_dirty;
  int surface_dirty_x;
  int surface_dirty_y;
  int surface_dirty_w;
  int surface_dirty_h;
  void *userdata;
  gui_window_layout_kind_t layout_kind;
  gui_window_chrome_kind_t chrome_kind;
  window_animation_t animation;
  int anim_frame;
  int anim_total_frames;

  /* Saved position for restore from maximize */
  int saved_x, saved_y;
  int saved_width, saved_height;

  /* Callbacks */
  void (*on_draw)(struct window *win);
  void (*on_key)(struct window *win, int key);
  void (*on_mouse)(struct window *win, int x, int y, int buttons);
  void (*on_close)(struct window *win);

  struct window *next;
};

static struct window windows[MAX_WINDOWS];
static struct window *window_stack = NULL; /* Z-order, top is focused */
static struct window *focused_window = NULL;
static struct window *chrome_hot_window = NULL;
static struct window *chrome_pressed_window = NULL;
static SkinHit chrome_hot_hit = SKIN_HIT_NONE;
static SkinHit chrome_pressed_hit = SKIN_HIT_NONE;
static SkinDragState chrome_drag_state = {0};
static int startup_window_opening = 0;
static int next_window_id = 1;
static void window_mark_surface_dirty_full(struct window *win);
static void gui_invalidate_window(struct window *win);

static SkinColor gui_skin_color_from_u32(uint32_t color) {
  uint8_t alpha = (uint8_t)((color >> 24) & 0xFF);
  if ((color & 0xFF000000U) == 0)
    alpha = 0xFF;
  return skin_rgba((uint8_t)((color >> 16) & 0xFF),
                   (uint8_t)((color >> 8) & 0xFF),
                   (uint8_t)(color & 0xFF), alpha);
}

static int window_uses_newwindows_chrome(const struct window *win) {
  return win && (win->chrome_kind == GUI_WINDOW_CHROME_SYSTEM ||
                 win->chrome_kind == GUI_WINDOW_CHROME_MINIMAL ||
                 win->chrome_kind == GUI_WINDOW_CHROME_FRAMEBUFFER);
}

static void gui_clear_chrome_visual_state_for_window(struct window *win) {
  int dirty = 0;

  if (!win)
    return;

  if (chrome_hot_window == win) {
    chrome_hot_window = NULL;
    chrome_hot_hit = SKIN_HIT_NONE;
    dirty = 1;
  }
  if (chrome_pressed_window == win) {
    chrome_pressed_window = NULL;
    chrome_pressed_hit = SKIN_HIT_NONE;
    dirty = 1;
  }
  if (dirty)
    gui_invalidate_window(win);
}

static uint32_t gui_skin_color_to_argb(SkinColor color) {
  return ((uint32_t)color.a << 24) | ((uint32_t)color.r << 16) |
         ((uint32_t)color.g << 8) | (uint32_t)color.b;
}

static void gui_skin_fill_rect(void *userdata, SkinRect rect, SkinColor color) {
  (void)userdata;
  if (rect.width <= 0 || rect.height <= 0)
    return;
  if (color.a >= 0xFF) {
    gui_draw_rect(rect.x, rect.y, rect.width, rect.height,
                  gui_skin_color_to_argb(color) & 0x00FFFFFFU);
    return;
  }
  gui_fill_rect_alpha(rect.x, rect.y, rect.width, rect.height,
                      gui_skin_color_to_argb(color));
}

static void gui_skin_draw_rect_outline(void *userdata, SkinRect rect,
                                       SkinColor color) {
  uint32_t argb = gui_skin_color_to_argb(color);
  (void)userdata;
  if (rect.width <= 0 || rect.height <= 0)
    return;
  if (color.a >= 0xFF) {
    gui_draw_rect_outline(rect.x, rect.y, rect.width, rect.height,
                          argb & 0x00FFFFFFU, 1);
    return;
  }
  gui_fill_rect_alpha(rect.x, rect.y, rect.width, 1, argb);
  gui_fill_rect_alpha(rect.x, rect.y + rect.height - 1, rect.width, 1, argb);
  gui_fill_rect_alpha(rect.x, rect.y, 1, rect.height, argb);
  gui_fill_rect_alpha(rect.x + rect.width - 1, rect.y, 1, rect.height, argb);
}

static void gui_skin_draw_line(void *userdata, int x1, int y1, int x2, int y2,
                               SkinColor color) {
  (void)userdata;
  gui_draw_line(x1, y1, x2, y2, gui_skin_color_to_argb(color) & 0x00FFFFFFU);
}

static void gui_skin_draw_text(void *userdata, int x, int y, const char *text,
                               SkinColor color) {
  (void)userdata;
  gui_draw_string(x, y - 6, text, gui_skin_color_to_argb(color) & 0x00FFFFFFU,
                  0x00000000);
}

static SkinWindow gui_skin_window_from_window(const struct window *win) {
  SkinWindow skin_window;
  skin_window.x = win->x;
  skin_window.y = win->y;
  skin_window.width = win->width;
  skin_window.height = win->height;
  skin_window.focused = win->focused;
  skin_window.resizable = win->resizable;
  skin_window.movable = true;
  skin_window.title = win->title;
  return skin_window;
}

static int gui_newwindows_allows_button_row(const struct window *win) {
  return win && win->has_titlebar &&
         win->chrome_kind != GUI_WINDOW_CHROME_FRAMEBUFFER;
}

static WindowSkin gui_newwindows_skin_for_window(const struct window *win) {
  WindowSkin skin = skin_make_aurora_sample();
  const gui_theme_palette_t *theme = gui_theme_palette();

  skin.metrics.titlebar_height = win->has_titlebar ? TITLEBAR_HEIGHT : 0;
  skin.metrics.border_left = BORDER_WIDTH;
  skin.metrics.border_right = BORDER_WIDTH;
  skin.metrics.border_top = BORDER_WIDTH;
  skin.metrics.border_bottom = BORDER_WIDTH;
  skin.metrics.button_width = gui_newwindows_allows_button_row(win) ? 20 : 0;
  skin.metrics.button_height =
      gui_newwindows_allows_button_row(win) ? TITLEBAR_HEIGHT : 0;
  skin.metrics.button_spacing = 0;
  skin.metrics.title_padding = 12;
  skin.metrics.shadow_size = 12;
  skin.metrics.resize_grip_size = 12;

  skin.border_style = SKIN_BORDER_FLAT;
  skin.shadow_style = SKIN_SHADOW_SOFT;
  skin.button_texture = SKIN_TEXTURE_GLASS;
  skin.button_texture_strength = 0.18f;
  skin.border_blur = g_blur_effects_enabled ? 10.0f : 0.0f;
  skin.border_transparency =
      g_blur_effects_mode != GUI_BLUR_EFFECTS_OFF ? 0.82f : 1.0f;
  skin.border_reflection = 0.26f;
  skin.buttons_on_left = true;
  skin.show_icon = false;
  skin.show_title = win->has_titlebar;
  skin.translucent_titlebar = g_blur_effects_mode != GUI_BLUR_EFFECTS_OFF;

  skin.button_order[0] = SKIN_BUTTON_CLOSE;
  skin.button_order[1] = SKIN_BUTTON_MINIMIZE;
  skin.button_order[2] = SKIN_BUTTON_MAXIMIZE;

  if (win->chrome_kind == GUI_WINDOW_CHROME_MINIMAL) {
    skin.metrics.titlebar_height = win->has_titlebar ? 24 : 0;
    skin.metrics.button_width = gui_newwindows_allows_button_row(win) ? 18 : 0;
    skin.metrics.button_height =
        gui_newwindows_allows_button_row(win) ? 24 : 0;
    skin.metrics.title_padding = 10;
    skin.metrics.shadow_size = 8;
    skin.button_texture = SKIN_TEXTURE_NONE;
    skin.button_texture_strength = 0.0f;
    skin.border_transparency =
        g_blur_effects_mode != GUI_BLUR_EFFECTS_OFF ? 0.92f : 1.0f;
    skin.border_reflection = 0.10f;
  } else if (win->chrome_kind == GUI_WINDOW_CHROME_FRAMEBUFFER) {
    skin.metrics.border_left = 1;
    skin.metrics.border_right = 1;
    skin.metrics.border_top = 1;
    skin.metrics.border_bottom = 1;
    skin.metrics.titlebar_height = 0;
    skin.metrics.button_width = 0;
    skin.metrics.button_height = 0;
    skin.metrics.shadow_size = 0;
    skin.metrics.resize_grip_size = 10;
    skin.shadow_style = SKIN_SHADOW_NONE;
    skin.button_texture = SKIN_TEXTURE_NONE;
    skin.button_texture_strength = 0.0f;
    skin.border_blur = 0.0f;
    skin.border_transparency = 1.0f;
    skin.border_reflection = 0.0f;
    skin.show_title = false;
    skin.translucent_titlebar = false;
  }

  skin.colors.frame = gui_skin_color_from_u32(
      g_blur_effects_mode != GUI_BLUR_EFFECTS_OFF
          ? (win->focused ? theme->window_glass_focused
                          : theme->window_glass_inactive)
          : gui_make_opaque_color(win->focused ? theme->window_glass_focused
                                               : theme->window_glass_inactive));
  skin.colors.frame_inactive =
      gui_skin_color_from_u32(g_blur_effects_mode != GUI_BLUR_EFFECTS_OFF
                                  ? theme->window_glass_inactive
                                  : gui_make_opaque_color(
                                        theme->window_glass_inactive));
  skin.colors.titlebar =
      gui_skin_color_from_u32(g_blur_effects_mode != GUI_BLUR_EFFECTS_OFF
                                  ? (win->focused ? theme->title_tint_focused
                                                  : theme->title_tint_inactive)
                                  : gui_make_opaque_color(
                                        win->focused ? theme->title_tint_focused
                                                     : theme->title_tint_inactive));
  skin.colors.titlebar_inactive =
      gui_skin_color_from_u32(g_blur_effects_mode != GUI_BLUR_EFFECTS_OFF
                                  ? theme->title_tint_inactive
                                  : gui_make_opaque_color(
                                        theme->title_tint_inactive));
  skin.colors.title_text = gui_skin_color_from_u32(theme->settings_text);
  skin.colors.title_text_inactive =
      gui_skin_color_from_u32(theme->settings_subtext);
  skin.colors.client_background = gui_skin_color_from_u32(0x98171A26);
  skin.colors.border_light =
      gui_skin_color_from_u32(win->focused ? theme->accent : theme->border);
  skin.colors.border_dark = gui_skin_color_from_u32(theme->border);
  skin.colors.button = gui_skin_color_from_u32(0xC4475567);
  skin.colors.button_hover = gui_skin_color_from_u32(0xE05F738C);
  skin.colors.button_pressed = gui_skin_color_from_u32(0xFF3B4757);
  skin.colors.close_button = gui_skin_color_from_u32(COLOR_BTN_CLOSE);
  skin.colors.close_button_hover = gui_skin_color_from_u32(0xFFF87171);
  skin.colors.button_icon = gui_skin_color_from_u32(0xFFF8FAFC);
  skin.colors.shadow = gui_skin_color_from_u32(0x50000000);
  skin.colors.accent = gui_skin_color_from_u32(theme->accent);

  if (win->chrome_kind == GUI_WINDOW_CHROME_MINIMAL) {
    skin.colors.frame =
        gui_skin_color_from_u32(win->focused ? theme->surface : theme->surface_alt);
    skin.colors.frame_inactive = gui_skin_color_from_u32(theme->surface_alt);
    skin.colors.titlebar =
        gui_skin_color_from_u32(win->focused ? theme->surface_alt : theme->surface);
    skin.colors.titlebar_inactive = gui_skin_color_from_u32(theme->surface);
    skin.colors.button = gui_skin_color_from_u32(0xFF3F4D5C);
    skin.colors.button_hover = gui_skin_color_from_u32(0xFF536579);
    skin.colors.button_pressed = gui_skin_color_from_u32(0xFF2D3947);
    skin.colors.shadow = gui_skin_color_from_u32(0x38000000);
  } else if (win->chrome_kind == GUI_WINDOW_CHROME_FRAMEBUFFER) {
    skin.colors.frame =
        gui_skin_color_from_u32(win->focused ? theme->accent : theme->border);
    skin.colors.frame_inactive = gui_skin_color_from_u32(theme->border);
    skin.colors.titlebar = gui_skin_color_from_u32(0x00000000);
    skin.colors.titlebar_inactive = gui_skin_color_from_u32(0x00000000);
    skin.colors.client_background = gui_skin_color_from_u32(0x00000000);
    skin.colors.border_light =
        gui_skin_color_from_u32(win->focused ? theme->accent : theme->border);
    skin.colors.border_dark = gui_skin_color_from_u32(theme->border);
    skin.colors.shadow = gui_skin_color_from_u32(0x00000000);
  }

  return skin;
}

static SkinHit gui_newwindows_hit_test(const struct window *win, int x, int y) {
  SkinWindow skin_window = gui_skin_window_from_window(win);
  WindowSkin skin = gui_newwindows_skin_for_window(win);
  return skin_hit_test(&skin_window, &skin, x, y);
}

static void gui_newwindows_content_insets(const struct window *win, int *left,
                                          int *top, int *right, int *bottom) {
  WindowSkin skin;

  if (!win) {
    if (left)
      *left = 0;
    if (top)
      *top = 0;
    if (right)
      *right = 0;
    if (bottom)
      *bottom = 0;
    return;
  }

  if (!window_uses_newwindows_chrome(win)) {
    if (left)
      *left = BORDER_WIDTH;
    if (top)
      *top = BORDER_WIDTH + (win->has_titlebar ? TITLEBAR_HEIGHT : 0);
    if (right)
      *right = BORDER_WIDTH;
    if (bottom)
      *bottom = BORDER_WIDTH;
    return;
  }

  skin = gui_newwindows_skin_for_window(win);
  if (left)
    *left = skin.metrics.border_left;
  if (top)
    *top = skin.metrics.border_top + skin.metrics.titlebar_height;
  if (right)
    *right = skin.metrics.border_right;
  if (bottom)
    *bottom = skin.metrics.border_bottom;
}

static int window_content_origin_y(const struct window *win) {
  int top = 0;
  if (!win)
    return 0;
  gui_newwindows_content_insets(win, NULL, &top, NULL, NULL);
  return top;
}

static int window_content_origin_x(const struct window *win) {
  int left = 0;
  if (!win)
    return 0;
  gui_newwindows_content_insets(win, &left, NULL, NULL, NULL);
  return left;
}

void gui_get_window_content_rect(const struct window *win, int *x, int *y, int *w,
                                 int *h) {
  int origin_x = window_content_origin_x(win);
  int origin_y = window_content_origin_y(win);
  int inset_right = 0;
  int inset_bottom = 0;
  int width = 0;
  int height = 0;

  if (!win || win->id == 0) {
    if (x)
      *x = 0;
    if (y)
      *y = 0;
    if (w)
      *w = 0;
    if (h)
      *h = 0;
    return;
  }

  gui_newwindows_content_insets(win, NULL, NULL, &inset_right, &inset_bottom);
  width = win->width - origin_x - inset_right;
  height = win->height - origin_y - inset_bottom;

  if (width < 0)
    width = 0;
  if (height < 0)
    height = 0;

  if (x)
    *x = win->x + origin_x;
  if (y)
    *y = win->y + origin_y;
  if (w)
    *w = width;
  if (h)
    *h = height;
}

static void gui_get_window_content_rect_local(const struct window *win, int *x,
                                              int *y, int *w, int *h) {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  int width = 0;
  int height = 0;

  if (!win || win->id == 0) {
    if (x)
      *x = 0;
    if (y)
      *y = 0;
    if (w)
      *w = 0;
    if (h)
      *h = 0;
    return;
  }

  gui_newwindows_content_insets(win, &left, &top, &right, &bottom);
  width = win->width - left - right;
  height = win->height - top - bottom;
  if (width < 0)
    width = 0;
  if (height < 0)
    height = 0;

  if (x)
    *x = left;
  if (y)
    *y = top;
  if (w)
    *w = width;
  if (h)
    *h = height;
}

static int window_titlebar_drag_limit(const struct window *win) {
  WindowSkin skin;

  if (!win)
    return 0;

  skin = gui_newwindows_skin_for_window(win);
  return skin.metrics.border_top + skin.metrics.titlebar_height;
}

void gui_set_window_layout_kind(struct window *win, gui_window_layout_kind_t kind) {
  if (!win)
    return;
  win->layout_kind = kind;
  window_mark_surface_dirty_full(win);
  gui_invalidate_window(win);
}

void gui_set_window_chrome_kind(struct window *win, gui_window_chrome_kind_t kind) {
  if (!win)
    return;
  gui_clear_chrome_visual_state_for_window(win);
  win->chrome_kind = kind;
  win->has_titlebar = kind != GUI_WINDOW_CHROME_FRAMEBUFFER;
  window_mark_surface_dirty_full(win);
  gui_invalidate_window(win);
}

static void window_get_draw_rect(const struct window *win, int *x, int *y, int *w,
                                 int *h) {
  int draw_x;
  int draw_y;
  int draw_w;
  int draw_h;
  if (!win) {
    if (x)
      *x = 0;
    if (y)
      *y = 0;
    if (w)
      *w = 0;
    if (h)
      *h = 0;
    return;
  }

  draw_x = win->x;
  draw_y = win->y;
  draw_w = win->width;
  draw_h = win->height;

  if (win->animation != WINDOW_ANIM_NONE && win->anim_total_frames > 0) {
    int progress = (win->anim_frame * 256) / win->anim_total_frames;
    int scale;
    int travel_y;
    if (progress < 0)
      progress = 0;
    if (progress > 256)
      progress = 256;

    if (win->animation == WINDOW_ANIM_OPEN) {
      scale = 224 + (progress * 32) / 256;
      travel_y = ((256 - progress) * 14) / 256;
    } else {
      scale = 256 - (progress * 32) / 256;
      travel_y = (progress * 14) / 256;
    }

    draw_w = (win->width * scale) / 256;
    draw_h = (win->height * scale) / 256;
    if (draw_w < 80)
      draw_w = 80;
    if (draw_h < 56)
      draw_h = 56;
    draw_x = win->x + (win->width - draw_w) / 2;
    draw_y = win->y + (win->height - draw_h) / 2 + travel_y;
  }

  if (x)
    *x = draw_x;
  if (y)
    *y = draw_y;
  if (w)
    *w = draw_w;
  if (h)
    *h = draw_h;
}

static void gui_destroy_window_immediate(struct window *win);

static void window_mark_surface_dirty(struct window *win, int x, int y, int w,
                                      int h) {
  int x2;
  int y2;
  int dirty_x2;
  int dirty_y2;

  if (!win || win->id == 0)
    return;
  if (w <= 0 || h <= 0)
    return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x >= win->width || y >= win->height)
    return;
  if (x + w > win->width)
    w = win->width - x;
  if (y + h > win->height)
    h = win->height - y;
  if (w <= 0 || h <= 0)
    return;

  if (!win->surface_dirty) {
    win->surface_dirty = 1;
    win->surface_dirty_x = x;
    win->surface_dirty_y = y;
    win->surface_dirty_w = w;
    win->surface_dirty_h = h;
    return;
  }

  x2 = x + w;
  y2 = y + h;
  dirty_x2 = win->surface_dirty_x + win->surface_dirty_w;
  dirty_y2 = win->surface_dirty_y + win->surface_dirty_h;

  if (x < win->surface_dirty_x)
    win->surface_dirty_x = x;
  if (y < win->surface_dirty_y)
    win->surface_dirty_y = y;
  if (x2 > dirty_x2)
    dirty_x2 = x2;
  if (y2 > dirty_y2)
    dirty_y2 = y2;

  win->surface_dirty_w = dirty_x2 - win->surface_dirty_x;
  win->surface_dirty_h = dirty_y2 - win->surface_dirty_y;
}

static void window_mark_surface_dirty_full(struct window *win) {
  if (!win || win->id == 0)
    return;
  win->surface_valid = 0;
  window_mark_surface_dirty(win, 0, 0, win->width, win->height);
}

static int window_ensure_surface_storage(struct window *win) {
  uint32_t *new_surface;
  size_t pixel_count;

  if (!win || win->id == 0)
    return -1;
  if (win->width <= 0 || win->height <= 0)
    return -1;
  if (win->content_buffer && win->surface_width == win->width &&
      win->surface_height == win->height)
    return 0;

  if ((size_t)win->height > ((size_t)-1) / (size_t)win->width)
    return -ENOMEM;
  pixel_count = (size_t)win->width * (size_t)win->height;
  if (pixel_count > ((size_t)-1) / sizeof(uint32_t))
    return -ENOMEM;
  new_surface = kmalloc(pixel_count * sizeof(uint32_t));
  if (!new_surface)
    return -ENOMEM;

  if (win->content_buffer)
    kfree(win->content_buffer);

  win->content_buffer = new_surface;
  win->surface_width = win->width;
  win->surface_height = win->height;
  win->surface_valid = 0;
  win->surface_dirty = 0;
  window_mark_surface_dirty_full(win);
  return 0;
}

static void gui_mark_windows_dirty_for_rect(int x, int y, int w, int h) {
  int rx2 = x + w;
  int ry2 = y + h;

  for (struct window *win = window_stack; win; win = win->next) {
    int wx2;
    int wy2;
    int ix;
    int iy;
    int ix2;
    int iy2;

    if (!win->visible || win->id == 0)
      continue;

    wx2 = win->x + win->width;
    wy2 = win->y + win->height;
    ix = x > win->x ? x : win->x;
    iy = y > win->y ? y : win->y;
    ix2 = rx2 < wx2 ? rx2 : wx2;
    iy2 = ry2 < wy2 ? ry2 : wy2;
    if (ix2 <= ix || iy2 <= iy)
      continue;

    window_mark_surface_dirty(win, ix - win->x, iy - win->y, ix2 - ix,
                              iy2 - iy);
  }
}

static void gui_invalidate_window(struct window *win) {
  int dirty_x;
  int dirty_y;
  int dirty_w;
  int dirty_h;

  if (!win || win->id == 0 || !win->visible)
    return;

  window_mark_surface_dirty_full(win);
  window_get_draw_rect(win, &dirty_x, &dirty_y, &dirty_w, &dirty_h);
  compositor_mark_dirty(dirty_x, dirty_y, dirty_w, dirty_h);
}

static void gui_begin_window_close(struct window *win) {
  if (!win || win->id == 0)
    return;
  if (win->animation == WINDOW_ANIM_CLOSE)
    return;
  gui_invalidate_window(win);
  win->animation = WINDOW_ANIM_CLOSE;
  win->anim_frame = 0;
  win->anim_total_frames = 8;
  gui_invalidate_window(win);
}

static void gui_update_window_animations(void) {
  struct window *win = window_stack;
  while (win) {
    struct window *next = win->next;
    int old_x = 0, old_y = 0, old_w = 0, old_h = 0;
    if (win->animation != WINDOW_ANIM_NONE) {
      window_get_draw_rect(win, &old_x, &old_y, &old_w, &old_h);
      if (win->anim_frame < win->anim_total_frames)
        win->anim_frame++;
      if (win->anim_frame >= win->anim_total_frames) {
        if (win->animation == WINDOW_ANIM_CLOSE) {
          gui_destroy_window_immediate(win);
        } else {
          win->animation = WINDOW_ANIM_NONE;
          win->anim_frame = 0;
          win->anim_total_frames = 0;
        }
      }
      compositor_mark_dirty(old_x, old_y, old_w, old_h);
      if (win->id != 0 && win->visible) {
        int new_x, new_y, new_w, new_h;
        window_get_draw_rect(win, &new_x, &new_y, &new_w, &new_h);
        compositor_mark_dirty(new_x, new_y, new_w, new_h);
      }
    }
    win = next;
  }
}

static int gui_has_active_animation(void) {
  struct window *win = window_stack;

  if (window_switcher_frames > 0)
    return 1;

  while (win) {
    if (win->animation != WINDOW_ANIM_NONE)
      return 1;
    win = win->next;
  }

  return 0;
}

static void gui_clamp_windows_to_display(void) {
  int max_y = (int)primary_display.height - dock_reserved_height() - 12;

  for (struct window *win = window_stack; win; win = win->next) {
    if (!win->visible)
      continue;

    if (win->state == WINDOW_MAXIMIZED) {
      win->x = 0;
      win->y = MENU_BAR_HEIGHT;
      win->width = primary_display.width;
      win->height = primary_display.height - MENU_BAR_HEIGHT -
                    dock_reserved_height() - 12;
      continue;
    }

    if (win->width > (int)primary_display.width)
      win->width = primary_display.width;
    if (win->height > (int)primary_display.height - dock_reserved_height())
      win->height = primary_display.height - dock_reserved_height();
    if (win->x + win->width > (int)primary_display.width)
      win->x = (int)primary_display.width - win->width;
    if (win->x < 0)
      win->x = 0;
    if (win->y < MENU_BAR_HEIGHT)
      win->y = MENU_BAR_HEIGHT;
    if (win->y + win->height > max_y)
      win->y = max_y - win->height;
    if (win->y < MENU_BAR_HEIGHT)
      win->y = MENU_BAR_HEIGHT;
  }
}

static int gui_resolution_live_supported_internal(void) {
#if !defined(ARCH_X86_64) && !defined(ARCH_X86)
  return 0;
#else
  if (str_cmp(g_gpu_backend_name, "bochs-vbe") == 0)
    return 1;
  return pci_find_device(0x1234, 0x1111) != NULL;
#endif
}

static int gui_apply_resolution(uint32_t width, uint32_t height) {
  uint32_t *new_framebuffer = NULL;
  uint32_t new_width = 0;
  uint32_t new_height = 0;
  uint32_t *new_backbuffer = NULL;
  uint32_t new_pitch;
  uint32_t *old_backbuffer = primary_display.backbuffer;
  int new_idx;

  if (width == primary_display.width && height == primary_display.height)
    return 0;

  if (!gui_resolution_live_supported_internal())
    return -1;

  if (bochs_init(width, height) != 0)
    return -1;

  bochs_get_info(&new_framebuffer, &new_width, &new_height);
  if (!new_framebuffer || new_width != width || new_height != height)
    return -1;

  new_pitch = new_width * 4;
  new_backbuffer = kmalloc(new_pitch * new_height);
  if (!new_backbuffer)
    return -1;

  {
    extern void fb_set_info(uint32_t *buffer, uint32_t width, uint32_t height,
                            uint32_t pitch);
    fb_set_info(new_framebuffer, new_width, new_height, new_pitch);
  }

  primary_display.framebuffer = new_framebuffer;
  primary_display.width = new_width;
  primary_display.height = new_height;
  primary_display.pitch = new_pitch;
  primary_display.backbuffer = new_backbuffer;
  g_saved_backbuffer = new_backbuffer;
  gui_use_display_render_target();

  {
    extern void kapi_refresh_display_state(void);
    kapi_refresh_display_state();
  }

  if (old_backbuffer)
    kfree(old_backbuffer);

  wallpaper_cached = 0;
  wallpaper_cached_idx = -1;
  invalidate_desktop_surface();

  input_set_mouse_bounds((int)new_width, (int)new_height);
  mouse_x = (int)new_width / 2;
  mouse_y = (int)new_height / 2;
  gui_clamp_windows_to_display();
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != 0)
      window_mark_surface_dirty_full(&windows[i]);
  }
  new_idx = settings_find_resolution_index(new_width, new_height);
  settings_resolution_current_idx = new_idx;
  if (new_idx >= 0)
    settings_resolution_pending_idx = new_idx;
  compositor_mark_full_redraw();

  printk(KERN_INFO "GUI: Resolution changed to %ux%u\n", new_width, new_height);
  return 0;
}

static int gui_load_saved_resolution(uint32_t *width, uint32_t *height) {
  char manifest[96];
  char width_buf[16];
  char height_buf[16];
  uint32_t parsed_width;
  uint32_t parsed_height;
  uint8_t *manifest_data = NULL;
  size_t manifest_size = 0;

  if (width)
    *width = 0;
  if (height)
    *height = 0;

  if (media_load_file(GUI_DISPLAY_CONFIG_PATH, &manifest_data, &manifest_size) < 0)
    return -1;
  if (!manifest_data || manifest_size == 0 || manifest_size >= sizeof(manifest)) {
    media_free_file(manifest_data);
    return -1;
  }
  for (size_t i = 0; i < manifest_size; i++)
    manifest[i] = (char)manifest_data[i];
  manifest[manifest_size] = '\0';
  media_free_file(manifest_data);
  if (manifest_get_value(manifest, "width", width_buf, sizeof(width_buf)) != 0 ||
      manifest_get_value(manifest, "height", height_buf,
                         sizeof(height_buf)) != 0) {
    return -1;
  }

  parsed_width = (uint32_t)parse_u64(width_buf);
  parsed_height = (uint32_t)parse_u64(height_buf);
  if (!parsed_width || !parsed_height)
    return -1;

  for (int i = 0; i < SETTINGS_RESOLUTION_OPTION_COUNT; i++) {
    if (settings_resolution_options[i].width == parsed_width &&
        settings_resolution_options[i].height == parsed_height) {
      if (width)
        *width = parsed_width;
      if (height)
        *height = parsed_height;
      return 0;
    }
  }

  return -1;
}

int gui_save_resolution_preference(uint32_t width, uint32_t height) {
  char manifest[128];
  int idx = 0;

  if (!gui_is_supported_resolution(width, height))
    return -1;

  vfs_mkdir("/System", 0755);

  for (const char *p = "width="; *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  append_decimal(manifest, &idx, (int)width);
  manifest[idx++] = '\n';
  for (const char *p = "height="; *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  append_decimal(manifest, &idx, (int)height);
  manifest[idx++] = '\n';
  for (const char *p = "blur="; *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  {
    const char *mode =
        g_blur_effects_mode == GUI_BLUR_EFFECTS_FORCE
            ? "force"
            : (g_blur_effects_mode == GUI_BLUR_EFFECTS_OFF ? "off" : "auto");
    for (const char *p = mode; *p && idx < (int)sizeof(manifest) - 1; p++)
      manifest[idx++] = *p;
  }
  manifest[idx++] = '\n';
  manifest[idx] = '\0';

  media_install_text_file(GUI_DISPLAY_CONFIG_PATH, manifest);
  settings_resolution_saved_idx = settings_find_resolution_index(width, height);
  return 0;
}

static void gui_apply_saved_boot_resolution(uint32_t **framebuffer,
                                            uint32_t *width,
                                            uint32_t *height,
                                            uint32_t *pitch) {
  uint32_t saved_width = 0;
  uint32_t saved_height = 0;
  uint32_t *new_framebuffer = NULL;
  uint32_t new_width = 0;
  uint32_t new_height = 0;

  if (!framebuffer || !*framebuffer || !width || !height || !pitch)
    return;
  if (gui_load_saved_resolution(&saved_width, &saved_height) != 0)
    return;
  if (saved_width == *width && saved_height == *height)
    return;
#if !defined(ARCH_X86_64) && !defined(ARCH_X86)
  return;
#endif
  if (!pci_find_device(0x1234, 0x1111))
    return;
  if (bochs_init(saved_width, saved_height) != 0)
    return;
  bochs_get_info(&new_framebuffer, &new_width, &new_height);
  if (!new_framebuffer || new_width != saved_width || new_height != saved_height)
    return;

  {
    extern void fb_set_info(uint32_t *buffer, uint32_t width, uint32_t height,
                            uint32_t pitch);
    fb_set_info(new_framebuffer, new_width, new_height, new_width * 4);
  }

  *framebuffer = new_framebuffer;
  *width = new_width;
  *height = new_height;
  *pitch = new_width * 4;
}

static struct window *notepad_find_window(void) {
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != 0 && windows[i].visible && windows[i].title[0] == 'N' &&
        windows[i].title[1] == 'o' && windows[i].title[2] == 't') {
      return &windows[i];
    }
  }
  return NULL;
}

static void notepad_update_window_title(void) {
  struct window *win = notepad_find_window();
  int idx = 0;

  if (!win)
    return;

  str_copy_safe(win->title, "Notepad", sizeof(win->title));
  idx = 7;
  if (notepad_filepath[0] && idx < (int)sizeof(win->title) - 3) {
    win->title[idx++] = ' ';
    win->title[idx++] = '-';
    win->title[idx++] = ' ';
    win->title[idx] = '\0';
    notepad_append_to_buf(win->title, sizeof(win->title),
                          notepad_basename(notepad_filepath));
  }
  if (notepad_dirty) {
    notepad_append_to_buf(win->title, sizeof(win->title), " *");
  }
}

static int window_title_equals(const struct window *win, const char *title) {
  int i = 0;

  if (!win || !title)
    return 0;

  while (win->title[i] && title[i]) {
    if (win->title[i] != title[i])
      return 0;
    i++;
  }

  return win->title[i] == '\0' && title[i] == '\0';
}

static int window_close_disabled(const struct window *win) {
  return gui_is_installer_mode() && window_title_equals(win, "Installer");
}

static int window_minimize_disabled(const struct window *win) {
  return gui_is_installer_mode() && window_title_equals(win, "Installer");
}

struct window *gui_create_window(const char *title, int x, int y, int w, int h);
void gui_focus_window(struct window *win);
static void installer_window_on_key(struct window *win, int key);

static void gui_open_installer_window(void) {
  struct window *win;
  int win_w;
  int win_h;
  int win_x;
  int win_y;

  if (!gui_is_installer_mode())
    return;

  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != 0 && window_title_equals(&windows[i], "Installer")) {
      windows[i].visible = true;
      windows[i].resizable = false;
      gui_focus_window(&windows[i]);
      return;
    }
  }

  win_w = (int)primary_display.width - 220;
  win_h = (int)primary_display.height - 160;
  if (win_w < 760)
    win_w = (int)primary_display.width - 40;
  if (win_h < 520)
    win_h = (int)primary_display.height - 40;
  if (win_w < 640)
    win_w = (int)primary_display.width;
  if (win_h < 420)
    win_h = (int)primary_display.height;
  win_x = ((int)primary_display.width - win_w) / 2;
  win_y = ((int)primary_display.height - win_h) / 2;

  win = gui_create_window("Installer", win_x, win_y, win_w, win_h);
  if (win) {
    win->resizable = false;
    win->on_key = installer_window_on_key;
    gui_focus_window(win);
  }
}


static void build_windows_string(char *buf) {
  int idx = 0;
  int count = 0;
  struct window *iter = window_stack;

  while (iter) {
    if (iter->visible) {
      count++;
    }
    iter = iter->next;
  }

  append_decimal(buf, &idx, count);
  buf[idx++] = ' ';
  buf[idx++] = 'o';
  buf[idx++] = 'p';
  buf[idx++] = 'e';
  buf[idx++] = 'n';
  buf[idx++] = ' ';
  buf[idx++] = 'w';
  buf[idx++] = 'i';
  buf[idx++] = 'n';
  buf[idx++] = 'd';
  buf[idx++] = 'o';
  buf[idx++] = 'w';
  if (count != 1) {
    buf[idx++] = 's';
  }
  buf[idx] = '\0';
}

/* Create a new window */
struct window *gui_create_window(const char *title, int x, int y, int w,
                                 int h) {
  if (startup_setup_account_active() && !startup_window_opening) {
    printk(KERN_INFO "GUI: Blocked window '%s' during account setup\n", title);
    return NULL;
  }

  /* Find free slot */
  struct window *win = NULL;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == 0) {
      win = &windows[i];
      break;
    }
  }

  if (!win) {
    printk(KERN_ERR "GUI: No free window slots\n");
    return NULL;
  }

  win->id = next_window_id++;
  for (int i = 0; i < 63 && title[i]; i++) {
    win->title[i] = title[i];
    win->title[i + 1] = '\0';
  }
  win->x = x;
  win->y = y;
  win->width = w;
  win->height = h;
  win->state = WINDOW_NORMAL;
  win->visible = true;
  win->focused = false;
  win->has_titlebar = true;
  win->resizable = true;
  win->layout_kind = GUI_WINDOW_LAYOUT_DEFAULT;
  win->chrome_kind = GUI_WINDOW_CHROME_SYSTEM;
  win->animation = WINDOW_ANIM_OPEN;
  win->anim_frame = 0;
  win->anim_total_frames = 8;
  win->content_buffer = NULL;
  win->surface_width = 0;
  win->surface_height = 0;
  win->surface_valid = 0;
  win->surface_dirty = 0;
  win->surface_dirty_x = 0;
  win->surface_dirty_y = 0;
  win->surface_dirty_w = 0;
  win->surface_dirty_h = 0;

  /* Reset all callbacks and userdata - critical to prevent stale pointers */
  win->on_draw = NULL;
  win->on_mouse = NULL;
  win->on_key = NULL;
  win->on_close = NULL;
  win->userdata = NULL;

  if (title[0] == 'B' && title[1] == 'a' && title[2] == 'c') {
    win->on_close = background_settings_window_on_close;
    background_settings_window_count++;
  }

  window_ensure_surface_storage(win);

  /* Add to stack */
  win->next = window_stack;
  window_stack = win;

  printk(KERN_INFO "GUI: Created window '%s' (%dx%d)\n", title, w, h);
  gui_invalidate_window(win);

  return win;
}

void gui_set_window_userdata(struct window *win, void *data) {
  if (win) {
    win->userdata = data;
  }
}

static void gui_clear_focus(void) {
  if (focused_window) {
    focused_window->focused = false;
    focused_window = NULL;
  }
}

static void gui_destroy_window_immediate(struct window *win) {
  int dirty_x, dirty_y, dirty_w, dirty_h;

  if (!win || win->id == 0)
    return;

  window_get_draw_rect(win, &dirty_x, &dirty_y, &dirty_w, &dirty_h);
  compositor_mark_dirty(dirty_x, dirty_y, dirty_w, dirty_h);
  gui_clear_chrome_visual_state_for_window(win);

  if (win->on_close) {
    win->on_close(win);
  }

  /* Remove from stack */
  if (window_stack == win) {
    window_stack = win->next;
  } else {
    struct window *prev = window_stack;
    while (prev && prev->next != win) {
      prev = prev->next;
    }
    if (prev) {
      prev->next = win->next;
    }
  }

  if (win->content_buffer) {
    kfree(win->content_buffer);
    win->content_buffer = NULL;
  }
  win->surface_width = 0;
  win->surface_height = 0;
  win->surface_valid = 0;
  win->surface_dirty = 0;
  win->surface_dirty_x = 0;
  win->surface_dirty_y = 0;
  win->surface_dirty_w = 0;
  win->surface_dirty_h = 0;

  if (focused_window == win)
    gui_clear_focus();

  win->visible = false;
  win->focused = false;
  win->animation = WINDOW_ANIM_NONE;
  win->anim_frame = 0;
  win->anim_total_frames = 0;
  win->on_draw = NULL;
  win->on_key = NULL;
  win->on_mouse = NULL;
  win->on_close = NULL;
  win->userdata = NULL;
  win->next = NULL;
  win->id = 0;
}

void gui_destroy_window(struct window *win) {
  if (!win || win->id == 0)
    return;
  gui_begin_window_close(win);
}

void gui_focus_window(struct window *win) {
  int dirty_x, dirty_y, dirty_w, dirty_h;

  if (!win)
    return;

  if (focused_window) {
    window_mark_surface_dirty_full(focused_window);
    window_get_draw_rect(focused_window, &dirty_x, &dirty_y, &dirty_w,
                         &dirty_h);
    compositor_mark_dirty(dirty_x, dirty_y, dirty_w, dirty_h);
    focused_window->focused = false;
  }

  window_mark_surface_dirty_full(win);
  window_get_draw_rect(win, &dirty_x, &dirty_y, &dirty_w, &dirty_h);
  compositor_mark_dirty(dirty_x, dirty_y, dirty_w, dirty_h);

  /* Move to top of stack */
  if (window_stack != win) {
    struct window *prev = window_stack;
    while (prev && prev->next != win) {
      prev = prev->next;
    }
    if (prev) {
      prev->next = win->next;
      win->next = window_stack;
      window_stack = win;
    }
  }

  win->focused = true;
  focused_window = win;
  window_mark_surface_dirty_full(win);
  window_get_draw_rect(win, &dirty_x, &dirty_y, &dirty_w, &dirty_h);
  compositor_mark_dirty(dirty_x, dirty_y, dirty_w, dirty_h);

  if (win->title[0] == 'T' && win->title[1] == 'e' && win->title[2] == 'r' &&
      win->userdata) {
    term_set_active((struct terminal *)win->userdata);
  }
}

int gui_window_id(const struct window *win) { return win ? win->id : 0; }

struct window *gui_find_window_by_id(int id) {
  if (id <= 0)
    return NULL;

  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == id)
      return &windows[i];
  }

  return NULL;
}

uint32_t *gui_window_get_buffer(struct window *win, int *w, int *h) {
  if (!win || win->id == 0)
    return NULL;
  if (window_ensure_surface_storage(win) != 0)
    return NULL;

  if (w)
    *w = win->surface_width;
  if (h)
    *h = win->surface_height;
  return win->content_buffer;
}

void gui_window_invalidate(struct window *win) {
  if (!win || win->id == 0)
    return;

  window_mark_surface_dirty_full(win);
  gui_invalidate_window(win);
}

void gui_window_set_title(struct window *win, const char *title) {
  if (!win || win->id == 0 || !title)
    return;

  (void)strlcpy(win->title, title, sizeof(win->title));
  window_mark_surface_dirty_full(win);
  gui_invalidate_window(win);
}

static void copy_window_title(char *dst, const char *src) {
  int i = 0;
  if (!dst)
    return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  for (; i < 63 && src[i]; i++) {
    dst[i] = src[i];
  }
  dst[i] = '\0';
}

static int count_visible_windows(void) {
  int count = 0;
  for (struct window *win = window_stack; win; win = win->next) {
    if (win->visible)
      count++;
  }
  return count;
}

static int gui_window_in_stack(struct window *target) {
  if (!target || target->id == 0)
    return 0;
  for (struct window *win = window_stack; win; win = win->next) {
    if (win == target)
      return 1;
  }
  return 0;
}

static struct window *find_next_switchable_window(void) {
  struct window *first_visible = NULL;
  int seen_focused = 0;

  for (struct window *win = window_stack; win; win = win->next) {
    if (!win->visible)
      continue;
    if (!first_visible)
      first_visible = win;
    if (seen_focused)
      return win;
    if (win == focused_window)
      seen_focused = 1;
  }

  return first_visible;
}

static void activate_window_switcher(void) {
  if (count_visible_windows() <= 0)
    return;

  struct window *target = find_next_switchable_window();
  if (!target)
    return;

  gui_focus_window(target);
  copy_window_title(window_switcher_title, target->title);
  window_switcher_frames = 75;
  compositor_mark_screen_rect_dirty();
}

static void execute_secure_attention_action(int action) {
  if (action == SECURE_ACTION_CANCEL) {
    secure_attention_open = 0;
    compositor_mark_screen_rect_dirty();
    return;
  }

  if (action == SECURE_ACTION_RESTART) {
    extern void arch_reboot(void);
    gui_flush_account_state_before_power_transition();
    arch_reboot();
    return;
  }

  if (action == SECURE_ACTION_SHUTDOWN) {
    extern void arch_poweroff(void);
    gui_flush_account_state_before_power_transition();
    arch_poweroff();
    return;
  }
}

static void open_secure_attention(void) {
  secure_attention_open = 1;
  secure_attention_selection = SECURE_ACTION_CANCEL;
  window_switcher_frames = 0;
  compositor_mark_screen_rect_dirty();
}

static int secure_attention_button_hit(int x, int y) {
  int panel_w = 420;
  int panel_h = 220;
  int panel_x = ((int)primary_display.width - panel_w) / 2;
  int panel_y = ((int)primary_display.height - panel_h) / 2;
  int button_y = panel_y + 156;
  int button_w = 108;
  int button_h = 34;
  int button_gap = 18;
  int start_x = panel_x + (panel_w - (button_w * 3 + button_gap * 2)) / 2;

  for (int i = 0; i < 3; i++) {
    int bx = start_x + i * (button_w + button_gap);
    if (x >= bx && x < bx + button_w && y >= button_y && y < button_y + button_h)
      return i;
  }

  return -1;
}

static void draw_window_switcher_overlay(void) {
  if (window_switcher_frames <= 0)
    return;

  int panel_w = 360;
  int panel_h = 136;
  int panel_x = ((int)primary_display.width - panel_w) / 2;
  int panel_y = MENU_BAR_HEIGHT + 36;
  char info[64];

  gui_fill_rect_alpha(0, 0, primary_display.width, primary_display.height,
                      0x18000000);
  gui_draw_glass_panel(panel_x, panel_y, panel_w, panel_h, 0x9A303A50,
                       0x38FFFFFF, 0x90728298, 2);
  gui_draw_string(panel_x + 22, panel_y + 20, "Window Switcher", 0xFFFFFF,
                  0x00000000);
  gui_draw_string(panel_x + 22, panel_y + 56, window_switcher_title, 0xEAF2FF,
                  0x00000000);

  build_windows_string(info);
  gui_draw_string(panel_x + 22, panel_y + 88, info, 0xB8C4D8, 0x00000000);
  gui_draw_string(panel_x + 22, panel_y + 108, "Press Alt+Tab again to cycle",
                  0x95A4BC, 0x00000000);
}

static void draw_secure_attention_overlay(void) {
  if (!secure_attention_open)
    return;

  int panel_w = 420;
  int panel_h = 220;
  int panel_x = ((int)primary_display.width - panel_w) / 2;
  int panel_y = ((int)primary_display.height - panel_h) / 2;
  int button_y = panel_y + 156;
  int button_w = 108;
  int button_h = 34;
  int button_gap = 18;
  int start_x = panel_x + (panel_w - (button_w * 3 + button_gap * 2)) / 2;
  const char *labels[3] = {"Cancel", "Restart", "Shut Down"};

  gui_fill_rect_alpha(0, 0, primary_display.width, primary_display.height,
                      0x58000000);
  gui_draw_glass_panel(panel_x, panel_y, panel_w, panel_h, 0xAE2A3448,
                       0x42FFFFFF, 0xA07E8CA2, 2);

  gui_draw_string(panel_x + 24, panel_y + 24, "Ctrl+Alt+Delete", 0xFFFFFF,
                  0x00000000);
  gui_draw_string(panel_x + 24, panel_y + 64, "System controls", 0xD7E3F6,
                  0x00000000);
  gui_draw_string(panel_x + 24, panel_y + 92,
                  "Choose an action for this session.", 0xB6C3D8, 0x00000000);

  if (focused_window && focused_window->visible) {
    gui_draw_string(panel_x + 24, panel_y + 118, "Active window:", 0x8FA0BA,
                    0x00000000);
    gui_draw_string(panel_x + 126, panel_y + 118, focused_window->title,
                    0xEFF5FF, 0x00000000);
  }

  for (int i = 0; i < 3; i++) {
    int bx = start_x + i * (button_w + button_gap);
    gui_button_variant_t variant = GUI_BUTTON_NEUTRAL;
    if (i == SECURE_ACTION_RESTART)
      variant = GUI_BUTTON_PRIMARY;
    else if (i == SECURE_ACTION_SHUTDOWN)
      variant = GUI_BUTTON_DANGER;
    gui_draw_system_button(bx, button_y, button_w, button_h, labels[i], variant,
                           1, i == secure_attention_selection);
  }
}

/* Draw a single window */
/* ===================================================================== */
/* Render Helpers */
/* ===================================================================== */

extern struct file *vfs_open(const char *path, int flags, mode_t mode);
extern int vfs_close(struct file *file);
extern int vfs_readdir(struct file *file, void *ctx,
                       int (*filldir)(void *, const char *, int, loff_t, ino_t,
                                      unsigned));

/* Forward declaration */
/* Helper for string compare */
static int str_cmp(const char *s1, const char *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static void append_uint_to_buf(char *buf, int max, int value) {
  char tmp[16];
  int idx = 0;
  int len = 0;
  if (!buf || max <= 0)
    return;
  if (value < 0)
    value = 0;
  while (len < max && buf[len])
    len++;
  if (len >= max - 1)
    return;
  if (value == 0) {
    buf[len++] = '0';
    buf[len] = '\0';
    return;
  }
  while (value > 0 && idx < (int)sizeof(tmp)) {
    tmp[idx++] = (char)('0' + (value % 10));
    value /= 10;
  }
  while (idx > 0 && len < max - 1) {
    buf[len++] = tmp[--idx];
  }
  buf[len] = '\0';
}

static char to_lower(char c) {
  if (c >= 'A' && c <= 'Z')
    return (char)(c + 32);
  return c;
}

static int str_ends_with_ci(const char *name, const char *ext) {
  if (!name || !ext)
    return 0;
  int nlen = 0;
  int elen = 0;
  while (name[nlen])
    nlen++;
  while (ext[elen])
    elen++;
  if (elen == 0 || nlen < elen)
    return 0;
  for (int i = 0; i < elen; i++) {
    if (to_lower(name[nlen - elen + i]) != to_lower(ext[i]))
      return 0;
  }
  return 1;
}

static void str_copy_safe(char *dst, const char *src, int max) {
  if (!dst || max <= 0)
    return;
  (void)strlcpy(dst, src ? src : "", (size_t)max);
}

typedef enum gui_app_kind {
  GUI_APP_TERMINAL,
  GUI_APP_FILES,
  GUI_APP_CALCULATOR,
  GUI_APP_NOTES,
  GUI_APP_SETTINGS,
  GUI_APP_CLOCK,
  GUI_APP_SNAKE,
  GUI_APP_HELP,
  GUI_APP_BROWSER,
  GUI_APP_APPSTORE,
  GUI_APP_DISK_IMAGER
} gui_app_kind_t;

static int window_matches_app_kind(const struct window *win,
                                   gui_app_kind_t kind) {
  if (!win || !win->visible)
    return 0;

  switch (kind) {
  case GUI_APP_TERMINAL:
    return win->title[0] == 'T' && win->title[1] == 'e' && win->title[2] == 'r';
  case GUI_APP_FILES:
    return win->title[0] == 'F' && win->title[1] == 'i' &&
           win->title[2] == 'l';
  case GUI_APP_CALCULATOR:
    return win->title[0] == 'C' && win->title[1] == 'a' &&
           win->title[2] == 'l';
  case GUI_APP_NOTES:
    return win->title[0] == 'N' && win->title[1] == 'o' &&
           win->title[2] == 't';
  case GUI_APP_SETTINGS:
    return win->title[0] == 'S' && win->title[1] == 'e' &&
           win->title[2] == 't';
  case GUI_APP_CLOCK:
    return win->title[0] == 'C' && win->title[1] == 'l' &&
           win->title[2] == 'o';
  case GUI_APP_SNAKE:
    return win->title[0] == 'B' && win->title[1] == 'o' &&
           win->title[2] == 'w';
  case GUI_APP_HELP:
    return win->title[0] == 'H' && win->title[1] == 'e';
  case GUI_APP_BROWSER:
    return win->title[0] == 'B' && win->title[1] == 'r' &&
           win->title[2] == 'o';
  case GUI_APP_APPSTORE:
    return win->title[0] == 'A' && win->title[1] == 'p' &&
           win->title[2] == 'p' && win->title[3] == ' ';
  case GUI_APP_DISK_IMAGER:
    return win->title[0] == 'D' && win->title[1] == 'i' &&
           win->title[2] == 's';
  }
  return 0;
}

static int count_windows_for_app_kind(gui_app_kind_t kind) {
  int count = 0;
  for (struct window *win = window_stack; win; win = win->next) {
    if (window_matches_app_kind(win, kind))
      count++;
  }
  return count;
}

static struct window *find_window_for_app_kind(gui_app_kind_t kind) {
  for (struct window *win = window_stack; win; win = win->next) {
    if (window_matches_app_kind(win, kind))
      return win;
  }
  return NULL;
}

typedef struct {
  char id[32];
  char label[48];
  char shortcut_name[48];
  gui_app_kind_t kind;
  const uint32_t *icon_data;
  uint32_t icon_color;
  int default_dock;
  int visible_in_store;
} dock_app_def_t;

/* Forward declarations for startup/app-state helpers referenced before their
 * definitions. */
static void ensure_gui_app_dirs(void);
static void ensure_app_manifest(const dock_app_def_t *app);
static void dock_add_item(const dock_app_def_t *app);
static void dock_add_missing_preinstalled_apps(void);
static void save_dock_config(void);
static void dock_add_all_system_apps(void);

typedef struct {
  const char *id;
  const char *label;
  const char *shortcut_name;
  gui_app_kind_t kind;
  int default_dock;
  int visible_in_store;
} system_app_seed_t;

#define GUI_SYSTEM_DIR "/System"
#define GUI_SYSTEM_APPS_DIR "/System/Apps"
#define GUI_APPS_DIR "/Applications"
#define GUI_SYSTEM_APPS_FOLDER "/Desktop/System Apps"
#define GUI_DOCK_CONFIG_PATH "/System/dock.cfg"
#define GUI_SETUP_STATE_PATH "/System/setup-state.cfg"
#define GUI_ACCOUNT_PATH "/System/account.cfg"
#define GUI_ACCOUNTS_DIR "/System/Accounts"
#define GUI_ETC_DIR "/etc"
#define GUI_PASSWD_PATH "/etc/passwd"
#define GUI_SHADOW_PATH "/etc/shadow"
#define GUI_GROUP_PATH "/etc/group"
#define GUI_HOME_ROOT "/home"
#define GUI_VERSION_PATH "/System/version.cfg"
#define ACCOUNT_RAW_SECTORS 16
#define ACCOUNT_RAW_BYTES (ACCOUNT_RAW_SECTORS * 512)
#define MAX_SYSTEM_APPS 24
#define MAX_DOCK_ITEMS 16
#define APP_STORE_CARD_HEIGHT 54

static const system_app_seed_t app_catalog_seed[] = {
    {"terminal", "Terminal", "Terminal.app", GUI_APP_TERMINAL, 1, 1},
    {"files", "Files", "Files.app", GUI_APP_FILES, 1, 1},
    {"calculator", "Calculator", "Calculator.app", GUI_APP_CALCULATOR, 1, 1},
    {"notes", "Notes", "Notes.app", GUI_APP_NOTES, 1, 1},
    {"settings", "Settings", "Settings.app", GUI_APP_SETTINGS, 0, 1},
    {"clock", "Clock", "Clock.app", GUI_APP_CLOCK, 0, 1},
    {"snake", "Bowling", "Bowling.app", GUI_APP_SNAKE, 0, 1},
    {"help", "Help", "Help.app", GUI_APP_HELP, 0, 1},
    {"browser", "Browser", "Browser.app", GUI_APP_BROWSER, 0, 1},
    {"appstore", "App Store", "App Store.app", GUI_APP_APPSTORE, 1, 0},
    {"diskimager", "Disk Imager", "Disk Imager.app", GUI_APP_DISK_IMAGER, 0,
     1},
};

#define APP_CATALOG_SEED_COUNT                                                \
  ((int)(sizeof(app_catalog_seed) / sizeof(app_catalog_seed[0])))

static dock_app_def_t app_catalog[MAX_SYSTEM_APPS];
static int app_catalog_count = 0;
static int app_catalog_loaded = 0;
static const dock_app_def_t *dock_items[MAX_DOCK_ITEMS];
static int dock_item_count = 0;
static int dock_loaded = 0;

typedef enum {
  STARTUP_FLOW_NONE = 0,
  STARTUP_FLOW_COMPLETING_INSTALL,
  STARTUP_FLOW_SETUP_ACCOUNT,
  STARTUP_FLOW_LOGIN
} startup_flow_t;

typedef enum {
  STARTUP_SETUP_PAGE_WELCOME = 0,
  STARTUP_SETUP_PAGE_ACCOUNT,
  STARTUP_SETUP_PAGE_STORAGE
} startup_setup_page_t;

static startup_flow_t startup_flow = STARTUP_FLOW_NONE;
static startup_setup_page_t startup_setup_page = STARTUP_SETUP_PAGE_WELCOME;
static char account_username[32] = "";
static char account_password[33] = "";
static char account_role[16] = "admin";
static int account_wallpaper = DEFAULT_WALLPAPER_INDEX;
static char account_partition_label[32] = "";
static char account_disk_location[32] = "";
static int account_partition_storage_ready = 0;
static int account_state_persist_pending = 0;
static char startup_input_username[32] = "";
static char startup_input_password[32] = "";
static int startup_active_field = 0;
static char startup_status[96] = "";
static struct window *startup_window = NULL;
static uint64_t startup_completion_started_ms = 0;
static uint64_t startup_completion_deadline_ms = 0;

static int account_role_is_admin(void) {
  return str_cmp(account_role, "admin") == 0;
}

static const char *account_role_label(void) {
  if (str_cmp(account_role, "child") == 0)
    return "Child";
  if (str_cmp(account_role, "user") == 0)
    return "User";
  return "Admin";
}

static const char *settings_role_label_from_index(int idx) {
  if (idx < 0 || idx >= SETTINGS_USER_ROLE_COUNT)
    idx = 1;
  if (str_cmp(settings_user_role_options[idx], "child") == 0)
    return "Child";
  if (str_cmp(settings_user_role_options[idx], "user") == 0)
    return "User";
  return "Admin";
}

static int clamp_wallpaper_index(int idx) {
  if (idx < 0 || idx >= NUM_WALLPAPERS)
    return DEFAULT_WALLPAPER_INDEX;
  return idx;
}

static void apply_account_wallpaper(int idx) {
  current_wallpaper = clamp_wallpaper_index(idx);
  wallpaper_cached = 0;
  wallpaper_cached_idx = -1;
  wallpaper_loaded = -1;
  invalidate_desktop_surface();
  wallpaper_ensure_loaded();
  compositor_mark_dirty(0, 0, (int)primary_display.width,
                        (int)primary_display.height);
}

static uint64_t parse_u64(const char *text) {
  uint64_t value = 0;
  int i = 0;
  while (text && text[i] >= '0' && text[i] <= '9') {
    value = value * 10 + (uint64_t)(text[i] - '0');
    i++;
  }
  return value;
}

static void load_system_app_catalog(void);

static const uint32_t *icon_data_for_kind(gui_app_kind_t kind) {
  switch (kind) {
  case GUI_APP_TERMINAL:
    return dock_icon_terminal;
  case GUI_APP_FILES:
    return dock_icon_folder;
  case GUI_APP_CALCULATOR:
    return dock_icon_calculator;
  case GUI_APP_NOTES:
    return dock_icon_notes;
  case GUI_APP_SETTINGS:
    return dock_icon_settings;
  case GUI_APP_CLOCK:
    return dock_icon_clock;
  case GUI_APP_SNAKE:
    return dock_icon_notes;
  case GUI_APP_HELP:
    return dock_icon_settings;
  case GUI_APP_BROWSER:
  case GUI_APP_APPSTORE:
    return dock_icon_folder;
  case GUI_APP_DISK_IMAGER:
    return dock_icon_settings;
  }
  return dock_icon_terminal;
}

static uint32_t icon_color_for_kind(gui_app_kind_t kind) {
  switch (kind) {
  case GUI_APP_TERMINAL:
    return 0x1E1E1E;
  case GUI_APP_FILES:
    return 0x3B82F6;
  case GUI_APP_CALCULATOR:
    return 0xFF9500;
  case GUI_APP_NOTES:
    return 0xFFCC00;
  case GUI_APP_SETTINGS:
    return 0x8E8E93;
  case GUI_APP_CLOCK:
    return 0x000000;
  case GUI_APP_SNAKE:
    return 0xDC2626;
  case GUI_APP_HELP:
    return 0x3B82F6;
  case GUI_APP_BROWSER:
    return 0x0EA5E9;
  case GUI_APP_APPSTORE:
    return 0x7C3AED;
  case GUI_APP_DISK_IMAGER:
    return 0x2563EB;
  }
  return 0x3B82F6;
}

static void draw_system_app_icon_kind(gui_app_kind_t kind, int x, int y,
                                      int size);

static const char *kind_to_string(gui_app_kind_t kind) {
  switch (kind) {
  case GUI_APP_TERMINAL:
    return "terminal";
  case GUI_APP_FILES:
    return "files";
  case GUI_APP_CALCULATOR:
    return "calculator";
  case GUI_APP_NOTES:
    return "notes";
  case GUI_APP_SETTINGS:
    return "settings";
  case GUI_APP_CLOCK:
    return "clock";
  case GUI_APP_SNAKE:
    return "bowling";
  case GUI_APP_HELP:
    return "help";
  case GUI_APP_BROWSER:
    return "browser";
  case GUI_APP_APPSTORE:
    return "appstore";
  case GUI_APP_DISK_IMAGER:
    return "diskimager";
  }
  return "terminal";
}

static gui_app_kind_t kind_from_string(const char *kind) {
  if (!kind)
    return GUI_APP_TERMINAL;
  if (str_cmp(kind, "files") == 0)
    return GUI_APP_FILES;
  if (str_cmp(kind, "calculator") == 0)
    return GUI_APP_CALCULATOR;
  if (str_cmp(kind, "notes") == 0)
    return GUI_APP_NOTES;
  if (str_cmp(kind, "settings") == 0)
    return GUI_APP_SETTINGS;
  if (str_cmp(kind, "clock") == 0)
    return GUI_APP_CLOCK;
  if (str_cmp(kind, "snake") == 0 || str_cmp(kind, "bowling") == 0)
    return GUI_APP_SNAKE;
  if (str_cmp(kind, "help") == 0)
    return GUI_APP_HELP;
  if (str_cmp(kind, "browser") == 0)
    return GUI_APP_BROWSER;
  if (str_cmp(kind, "appstore") == 0)
    return GUI_APP_APPSTORE;
  if (str_cmp(kind, "diskimager") == 0)
    return GUI_APP_DISK_IMAGER;
  return GUI_APP_TERMINAL;
}

static void fill_runtime_app(dock_app_def_t *app, const char *id,
                             const char *label, const char *shortcut_name,
                             gui_app_kind_t kind, int default_dock,
                             int visible_in_store) {
  str_copy_safe(app->id, id, sizeof(app->id));
  str_copy_safe(app->label, label, sizeof(app->label));
  str_copy_safe(app->shortcut_name, shortcut_name, sizeof(app->shortcut_name));
  app->kind = kind;
  app->icon_data = icon_data_for_kind(kind);
  app->icon_color = icon_color_for_kind(kind);
  app->default_dock = default_dock;
  app->visible_in_store = visible_in_store;
}

static const dock_app_def_t *find_catalog_app(const char *id) {
  load_system_app_catalog();
  if (!id)
    return NULL;
  for (int i = 0; i < app_catalog_count; i++) {
    if (str_cmp(app_catalog[i].id, id) == 0)
      return &app_catalog[i];
  }
  return NULL;
}

static int app_manifest_path(const dock_app_def_t *app, char *path, int max) {
  if (!app || !path || max < 32)
    return -1;
  str_copy_safe(path, GUI_APPS_DIR, max);
  int idx = 0;
  while (path[idx])
    idx++;
  if (idx >= max - 1)
    return -1;
  path[idx++] = '/';
  for (int i = 0; app->id[i] && idx < max - 5; i++) {
    path[idx++] = app->id[i];
  }
  path[idx++] = '.';
  path[idx++] = 'a';
  path[idx++] = 'p';
  path[idx++] = 'p';
  path[idx] = '\0';
  return 0;
}

static int build_app_shortcut_path(const char *dir, const char *shortcut_name,
                                   char *path, int max) {
  int idx = 0;

  if (!dir || !shortcut_name || !path || max < 8)
    return -1;

  str_copy_safe(path, dir, max);
  while (path[idx])
    idx++;
  if (idx >= max - 1)
    return -1;
  if (idx > 0 && path[idx - 1] != '/')
    path[idx++] = '/';
  for (int i = 0; shortcut_name[i] && idx < max - 1; i++)
    path[idx++] = shortcut_name[i];
  path[idx] = '\0';
  return 0;
}

static int write_text_file(const char *path, const char *content) {
  char resolved_path[256];
  char log_line[384];
  const char *target;
  int ret;

  if (!path || !content)
    return -1;

  target = resolve_user_storage_path(path, resolved_path, sizeof(resolved_path));
  if (path_is_active_account_home(path) && str_cmp(target, path) != 0 &&
      !path_is_active_account_home_root(path)) {
    ret = media_install_text_file(target, content);
    if (ret != 0) {
      str_copy_safe(log_line, "sync mounted write failed: ", sizeof(log_line));
      installer_append_to_buf(log_line, sizeof(log_line), target);
      runtime_sync_log_line(log_line);
      return ret;
    }
    runtime_sync_flush_best_effort(target);
    str_copy_safe(log_line, "sync mounted write complete: ", sizeof(log_line));
    installer_append_to_buf(log_line, sizeof(log_line), path);
    installer_append_to_buf(log_line, sizeof(log_line), " -> ");
    installer_append_to_buf(log_line, sizeof(log_line), target);
    runtime_sync_log_line(log_line);
    return 0;
  }

  ret = media_install_text_file(path, content);
  if (ret != 0) {
    str_copy_safe(log_line, "sync write failed: ", sizeof(log_line));
    installer_append_to_buf(log_line, sizeof(log_line), path);
    runtime_sync_log_line(log_line);
    return ret;
  }

  if (str_cmp(target, path) != 0) {
    ret = media_install_text_file(target, content);
    if (ret != 0) {
      str_copy_safe(log_line, "sync mirror write failed: ", sizeof(log_line));
      installer_append_to_buf(log_line, sizeof(log_line), target);
      runtime_sync_log_line(log_line);
      return ret;
    }
  }

  runtime_sync_flush_best_effort(path);

  str_copy_safe(log_line, "sync write complete: ", sizeof(log_line));
  installer_append_to_buf(log_line, sizeof(log_line), path);
  if (str_cmp(target, path) != 0) {
    installer_append_to_buf(log_line, sizeof(log_line), " -> ");
    installer_append_to_buf(log_line, sizeof(log_line), target);
  }
  runtime_sync_log_line(log_line);
  return 0;
}

static int installer_get_persistent_root(char *buf, int max) {
  char disk_location[32];
  char mounted_root[96];
  static const char *roots[] = {"/Persist", "/persist", "/disk", "/mnt/disk"};
  struct file *dir;

  if (!buf || max <= 0)
    return -1;

  if (load_install_target_disk_location(disk_location,
                                        sizeof(disk_location)) == 0 &&
      disk_location[0]) {
    int idx = 0;
    str_copy_safe(mounted_root, "/mnt", sizeof(mounted_root));
    while (mounted_root[idx] && idx < (int)sizeof(mounted_root) - 1)
      idx++;
    if (idx < (int)sizeof(mounted_root) - 1)
      mounted_root[idx++] = '/';
    for (int i = 0; disk_location[i] && idx < (int)sizeof(mounted_root) - 1;
         i++)
      mounted_root[idx++] = disk_location[i];
    mounted_root[idx] = '\0';
    dir = vfs_open(mounted_root, O_RDONLY, 0);
    if (dir) {
      vfs_close(dir);
      str_copy_safe(buf, mounted_root, max);
      return 0;
    }
  }

  for (int i = 0; i < (int)(sizeof(roots) / sizeof(roots[0])); i++) {
    dir = vfs_open(roots[i], O_RDONLY, 0);
    if (!dir)
      continue;
    vfs_close(dir);
    str_copy_safe(buf, roots[i], max);
    return 0;
  }

  buf[0] = '\0';
  return -1;
}

static int installer_journal_init(void) {
  int selected_disk_index;
  int partition_count;

  if (installer_install_journal_ready)
    return 0;
  if (installer_install_journal_disk_index >= 0)
    return 0;

  selected_disk_index = installer_selected_disk_index();
  if (selected_disk_index < 0)
    return -1;

  partition_count = storage_get_partition_count(selected_disk_index);
  for (int i = 0; i < partition_count; i++) {
    storage_partition_kind_t kind;
    uint32_t start_lba = 0;
    uint32_t sector_count = 0;

    if (storage_get_partition_info(selected_disk_index, i, &kind, NULL, 0,
                                   &start_lba, &sector_count) != 0)
      continue;
    if (kind != STORAGE_PARTITION_SYSTEM)
      continue;
    if (sector_count < 8)
      continue;

    installer_install_journal_disk_index = selected_disk_index;
    installer_install_journal_next_lba = start_lba;
    installer_install_journal_last_lba = start_lba + sector_count;
    installer_install_journal_ready = 1;
    return 0;
  }

  return -1;
}

static int installer_journal_write_sector(uint32_t lba, const uint8_t *src,
                                          size_t len) {
  uint8_t sector[512];

  for (int i = 0; i < 512; i++)
    sector[i] = 0;
  if (src && len > 0) {
    if (len > sizeof(sector))
      len = sizeof(sector);
    for (size_t i = 0; i < len; i++)
      sector[i] = src[i];
  }
  if (storage_write_block(installer_install_journal_disk_index, lba, sector,
                          512) != 0)
    return -1;
  return 0;
}

static int installer_journal_install_write(const char *path,
                                           const uint8_t *data, size_t size) {
  uint8_t header[512];
  size_t path_len = 0;
  size_t remaining = size;
  uint32_t lba;

  if (!path || !path[0] || (size > 0 && !data))
    return -1;
  if (installer_journal_init() != 0)
    return -1;
  if (installer_install_journal_next_lba >= installer_install_journal_last_lba)
    return -1;

  while (path[path_len] && path_len < 255)
    path_len++;

  for (int i = 0; i < 512; i++)
    header[i] = 0;
  header[0] = 'O';
  header[1] = 'S';
  header[2] = '8';
  header[3] = 'J';
  header[4] = 'R';
  header[5] = 'N';
  header[6] = '1';
  header[7] = 0;
  header[8] = (uint8_t)(path_len & 0xFF);
  header[9] = (uint8_t)((path_len >> 8) & 0xFF);
  header[10] = (uint8_t)((path_len >> 16) & 0xFF);
  header[11] = (uint8_t)((path_len >> 24) & 0xFF);
  header[12] = (uint8_t)(size & 0xFF);
  header[13] = (uint8_t)((size >> 8) & 0xFF);
  header[14] = (uint8_t)((size >> 16) & 0xFF);
  header[15] = (uint8_t)((size >> 24) & 0xFF);
  header[16] = (uint8_t)((size >> 32) & 0xFF);
  header[17] = (uint8_t)((size >> 40) & 0xFF);
  header[18] = (uint8_t)((size >> 48) & 0xFF);
  header[19] = (uint8_t)((size >> 56) & 0xFF);

  lba = installer_install_journal_next_lba++;
  if (installer_journal_write_sector(lba, header, sizeof(header)) != 0)
    return -1;

  if (path_len > 0) {
    const uint8_t *path_bytes = (const uint8_t *)path;
    size_t path_written = 0;
    while (path_written < path_len) {
      size_t chunk = path_len - path_written;
      if (chunk > 512)
        chunk = 512;
      if (installer_install_journal_next_lba >= installer_install_journal_last_lba)
        return -1;
      lba = installer_install_journal_next_lba++;
      if (installer_journal_write_sector(lba, path_bytes + path_written,
                                         chunk) != 0)
        return -1;
      path_written += chunk;
    }
  }

  while (remaining > 0) {
    size_t chunk = remaining > 512 ? 512 : remaining;
    if (installer_install_journal_next_lba >= installer_install_journal_last_lba)
      return -1;
    lba = installer_install_journal_next_lba++;
    if (installer_journal_write_sector(lba, data + (size - remaining), chunk) !=
        0)
      return -1;
    remaining -= chunk;
  }

  return 0;
}

static int mounted_disk_root_path_for_location(const char *location, char *buf,
                                               int max) {
  int idx = 0;

  if (!location || !location[0] || !buf || max <= 0)
    return -1;

  str_copy_safe(buf, "/mnt", max);
  while (buf[idx] && idx < max - 1)
    idx++;
  if (idx < max - 1)
    buf[idx++] = '/';
  for (int i = 0; location[i] && idx < max - 1; i++)
    buf[idx++] = location[i];
  buf[idx] = '\0';
  return 0;
}

static int boot_storage_root_path(char *buf, int max) {
  char disk_location[32];

  if (!buf || max <= 0)
    return -1;
  buf[0] = '\0';

  {
    extern int boot_is_usb_boot(void);
    if (boot_is_usb_boot())
      return -1;
  }

  if (account_disk_location[0]) {
    str_copy_safe(disk_location, account_disk_location, sizeof(disk_location));
  } else {
    if (load_install_target_disk_location(disk_location,
                                          sizeof(disk_location)) != 0) {
      return -1;
    }
  }

  if (mounted_disk_root_path_for_location(disk_location, buf, max) != 0)
    return -1;

  str_copy_safe(bootscource, buf, sizeof(bootscource));
  return 0;
}

static int path_starts_with(const char *path, const char *prefix) {
  int i = 0;

  if (!path || !prefix)
    return 0;

  while (prefix[i]) {
    if (path[i] != prefix[i])
      return 0;
    i++;
  }

  return 1;
}

static int account_storage_root_path(char *buf, int max) {
  int idx = 0;

  if (!buf || max <= 0)
    return -1;
  buf[0] = '\0';

  if (!account_disk_location[0] || !account_partition_label[0])
    return -1;

  if (mounted_disk_root_path_for_location(account_disk_location, buf, max) != 0)
    return -1;
  while (buf[idx] && idx < max - 1)
    idx++;
  if (idx < max - 1)
    buf[idx++] = '/';
  for (int i = 0; account_partition_label[i] && idx < max - 1; i++)
    buf[idx++] = account_partition_label[i];
  buf[idx] = '\0';
  return 0;
}

static int path_is_active_account_home(const char *path) {
  char home_prefix[96];
  int idx = 0;

  if (!path || !user_home_mount_active || !user_home_mounted_username[0])
    return 0;

  str_copy_safe(home_prefix, path_starts_with(path, GUI_HOME_ROOT) ?
                            "/home/" : "/Users/",
                sizeof(home_prefix));
  idx = path_starts_with(path, GUI_HOME_ROOT) ? 6 : 7;
  for (int i = 0; user_home_mounted_username[i] &&
                  idx < (int)sizeof(home_prefix) - 1;
       i++)
    home_prefix[idx++] = user_home_mounted_username[i];
  home_prefix[idx] = '\0';

  if (!path_starts_with(path, home_prefix))
    return 0;

  return path[idx] == '\0' || path[idx] == '/';
}

static int path_is_active_account_home_root(const char *path) {
  char home_path[96];
  int idx = 7;

  if (!path || !user_home_mount_active || !user_home_mounted_username[0])
    return 0;

  str_copy_safe(home_path, path_starts_with(path, GUI_HOME_ROOT) ?
                           "/home/" : "/Users/",
                sizeof(home_path));
  idx = path_starts_with(path, GUI_HOME_ROOT) ? 6 : 7;
  for (int i = 0; user_home_mounted_username[i] &&
                  idx < (int)sizeof(home_path) - 1;
       i++)
    home_path[idx++] = user_home_mounted_username[i];
  home_path[idx] = '\0';
  return str_cmp(path, home_path) == 0;
}

static int path_is_user_storage(const char *path) {
  if (!path)
    return 0;
  return str_cmp(path, "/Users") == 0 || str_cmp(path, GUI_HOME_ROOT) == 0 ||
         (path[0] == '/' && path[1] == 'U' && path[2] == 's' &&
          path[3] == 'e' && path[4] == 'r' && path[5] == 's' &&
          path[6] == '/') ||
         (path[0] == '/' && path[1] == 'h' && path[2] == 'o' &&
          path[3] == 'm' && path[4] == 'e' && path[5] == '/');
}

static int path_is_runtime_mutable(const char *path) {
  static const char *prefixes[] = {"/System", "/Applications", "/Desktop",
                                   "/Documents", "/Pictures", "/Music",
                                   "/Users", GUI_HOME_ROOT, GUI_ETC_DIR};

  if (!path || path[0] != '/')
    return 0;
  if (path_starts_with(path, "/mnt") || path_starts_with(path, "/Installed") ||
      path_starts_with(path, "/External") || path_starts_with(path, "/Media") ||
      path_starts_with(path, "/boot") ||
      path_starts_with(path, "/EFI"))
    return 0;

  for (int i = 0; i < (int)(sizeof(prefixes) / sizeof(prefixes[0])); i++) {
    const char *prefix = prefixes[i];
    int j = 0;
    while (prefix[j] && path[j] == prefix[j])
      j++;
    if (!prefix[j] && (path[j] == '\0' || path[j] == '/'))
      return 1;
  }
  return 0;
}

static const char *resolve_user_storage_path(const char *path, char *buf,
                                             int max) {
  char account_root[160];
  char boot_root[96];
  char home_prefix[96];
  char persistent_root[64];
  int path_idx = 0;
  int idx = 0;

  if (!path || !buf || max <= 0)
    return path;
  /*
   * The install-target record is the bootstrap input for boot_storage_root_path().
   * Resolving it through the runtime-mutable path logic would recurse back into
   * load_install_target_disk_location() while we are already trying to read it.
   */
  if (str_cmp(path, "/System/install-target.cfg") == 0)
    return path;
  if (path_is_active_account_home(path) &&
      account_storage_root_path(account_root, sizeof(account_root)) == 0) {
    str_copy_safe(home_prefix, path_starts_with(path, GUI_HOME_ROOT) ?
                               "/home/" : "/Users/",
                  sizeof(home_prefix));
    path_idx = path_starts_with(path, GUI_HOME_ROOT) ? 6 : 7;
    while (home_prefix[path_idx] && path_idx < (int)sizeof(home_prefix) - 1)
      path_idx++;
    for (int i = 0; user_home_mounted_username[i] &&
                    path_idx < (int)sizeof(home_prefix) - 1;
         i++) {
      home_prefix[path_idx++] = user_home_mounted_username[i];
    }
    home_prefix[path_idx] = '\0';
    path_idx = 0;
    while (home_prefix[path_idx] && path[path_idx] == home_prefix[path_idx])
      path_idx++;

    for (int i = 0; account_root[i] && idx < max - 1; i++)
      buf[idx++] = account_root[i];
    while (path[path_idx] && idx < max - 1)
      buf[idx++] = path[path_idx++];
    buf[idx] = '\0';
    return buf;
  }
  if (path_is_runtime_mutable(path) &&
      boot_storage_root_path(boot_root, sizeof(boot_root)) == 0) {
    idx = 0;
    for (int i = 0; boot_root[i] && idx < max - 1; i++)
      buf[idx++] = boot_root[i];
    for (int i = 0; path[i] && idx < max - 1; i++)
      buf[idx++] = path[i];
    buf[idx] = '\0';
    return buf;
  }

  if (path_is_user_storage(path) &&
      installer_get_persistent_root(persistent_root, sizeof(persistent_root)) ==
          0) {
    idx = 0;
    for (int i = 0; persistent_root[i] && idx < max - 1; i++)
      buf[idx++] = persistent_root[i];
    for (int i = 0; path[i] && idx < max - 1; i++)
      buf[idx++] = path[i];
    buf[idx] = '\0';
    return buf;
  }
  return path;
}

static void ensure_user_storage_dirs(void) {
  char account_root[160];
  char user_home[96];
  char persistent_path[160];
  int idx = 0;

  vfs_mkdir("/Users", 0755);
  vfs_mkdir(GUI_HOME_ROOT, 0755);
  resolve_user_storage_path("/Users", persistent_path, sizeof(persistent_path));
  if (str_cmp(persistent_path, "/Users") != 0)
    vfs_mkdir(persistent_path, 0755);
  resolve_user_storage_path(GUI_HOME_ROOT, persistent_path, sizeof(persistent_path));
  if (str_cmp(persistent_path, GUI_HOME_ROOT) != 0)
    vfs_mkdir(persistent_path, 0755);

  if (!account_username[0])
    return;

  str_copy_safe(user_home, "/Users/", sizeof(user_home));
  idx = 7;
  for (int i = 0; account_username[i] && idx < (int)sizeof(user_home) - 1; i++)
    user_home[idx++] = account_username[i];
  user_home[idx] = '\0';

  resolve_user_storage_path(user_home, persistent_path, sizeof(persistent_path));
  if (user_home_mount_active &&
      str_cmp(user_home_mounted_username, account_username) == 0) {
    vfs_mkdir(user_home, 0755);
  }
  if (str_cmp(persistent_path, user_home) != 0)
    vfs_mkdir(persistent_path, 0755);
  account_unix_home_path(account_username, user_home, sizeof(user_home));
  vfs_mkdir(user_home, 0755);
  resolve_user_storage_path(user_home, persistent_path, sizeof(persistent_path));
  if (str_cmp(persistent_path, user_home) != 0)
    vfs_mkdir(persistent_path, 0755);
  if (account_storage_root_path(account_root, sizeof(account_root)) == 0) {
    installer_ensure_parent_dirs(account_root);
    vfs_mkdir(account_root, 0755);
  }
}

static void mount_active_user_home(void) {
  char user_home[96];
  int idx = 7;

  if (!account_username[0])
    return;

  user_home_mount_active = 1;
  str_copy_safe(user_home_mounted_username, account_username,
                sizeof(user_home_mounted_username));
  ensure_user_storage_dirs();

  str_copy_safe(user_home, "/Users/", sizeof(user_home));
  for (int i = 0; account_username[i] && idx < (int)sizeof(user_home) - 1; i++)
    user_home[idx++] = account_username[i];
  user_home[idx] = '\0';
  vfs_mkdir("/Users", 0755);
  vfs_mkdir(user_home, 0755);
  account_unix_home_path(account_username, user_home, sizeof(user_home));
  vfs_mkdir(GUI_HOME_ROOT, 0755);
  vfs_mkdir(user_home, 0755);
}

static void unmount_active_user_home(void) {
  char user_home[96];
  int idx = 7;
  extern int vfs_rmdir(const char *path);

  if (!user_home_mount_active || !user_home_mounted_username[0])
    return;

  str_copy_safe(user_home, "/Users/", sizeof(user_home));
  for (int i = 0; user_home_mounted_username[i] &&
                  idx < (int)sizeof(user_home) - 1;
       i++)
    user_home[idx++] = user_home_mounted_username[i];
  user_home[idx] = '\0';

  vfs_rmdir(user_home);
  user_home_mount_active = 0;
  user_home_mounted_username[0] = '\0';
}

static int user_storage_mkdir(const char *path, mode_t mode) {
  char resolved[256];
  const char *target = resolve_user_storage_path(path, resolved, sizeof(resolved));
  int ret;

  if (path_is_user_storage(path)) {
    vfs_mkdir("/Users", 0755);
    vfs_mkdir(GUI_HOME_ROOT, 0755);
  }
  if (path_is_active_account_home(path) && str_cmp(target, path) != 0 &&
      !path_is_active_account_home_root(path))
    return vfs_mkdir(target, mode);
  ret = vfs_mkdir(path, mode);
  if (str_cmp(target, path) != 0)
    vfs_mkdir(target, mode);
  return ret;
}

static int user_storage_unlink(const char *path) {
  char resolved[256];
  const char *target = resolve_user_storage_path(path, resolved, sizeof(resolved));
  if (path_is_active_account_home(path) && str_cmp(target, path) != 0 &&
      !path_is_active_account_home_root(path))
    return vfs_unlink(target);
  int ret = vfs_unlink(path);
  if (str_cmp(target, path) != 0)
    vfs_unlink(target);
  return ret;
}

static int user_storage_rmdir(const char *path) {
  char resolved[256];
  extern int vfs_rmdir(const char *path);
  const char *target = resolve_user_storage_path(path, resolved, sizeof(resolved));
  if (path_is_active_account_home(path) && str_cmp(target, path) != 0 &&
      !path_is_active_account_home_root(path))
    return vfs_rmdir(target);
  int ret = vfs_rmdir(path);
  if (str_cmp(target, path) != 0)
    vfs_rmdir(target);
  return ret;
}

static int user_storage_rename(const char *old_path, const char *new_path) {
  char resolved_old[256];
  char resolved_new[256];
  extern int vfs_rename(const char *old, const char *new);
  const char *old_target =
      resolve_user_storage_path(old_path, resolved_old, sizeof(resolved_old));
  const char *new_target =
      resolve_user_storage_path(new_path, resolved_new, sizeof(resolved_new));
  if (path_is_active_account_home(old_path) && path_is_active_account_home(new_path) &&
      (str_cmp(old_target, old_path) != 0 || str_cmp(new_target, new_path) != 0) &&
      !path_is_active_account_home_root(old_path) &&
      !path_is_active_account_home_root(new_path)) {
    return vfs_rename(old_target, new_target);
  }
  int ret = vfs_rename(old_path, new_path);
  if (str_cmp(old_target, old_path) != 0 || str_cmp(new_target, new_path) != 0)
    vfs_rename(old_target, new_target);
  return ret;
}

static int installer_translate_target_path(const char *path, char *buf,
                                           int max) {
  char persistent_root[64];
  int prefix_len = 0;
  int idx = 0;

  if (!path || !buf || max <= 0 || !installer_target_root[0])
    return -1;
  if (installer_get_persistent_root(persistent_root,
                                    sizeof(persistent_root)) != 0)
    return -1;

  while (installer_target_root[prefix_len])
    prefix_len++;
  for (int i = 0; i < prefix_len; i++) {
    if (path[i] != installer_target_root[i])
      return -1;
  }
  if (path[prefix_len] && path[prefix_len] != '/')
    return -1;

  for (int i = 0; persistent_root[i] && idx < max - 1; i++)
    buf[idx++] = persistent_root[i];
  for (int i = prefix_len; path[i] && idx < max - 1; i++)
    buf[idx++] = path[i];
  buf[idx] = '\0';
  return 0;
}

static int installer_write_raw_file(const char *path, const uint8_t *data,
                                    size_t size) {
  struct file *f;
  ssize_t written;

  if (!path || (!data && size > 0))
    return -1;

  installer_ensure_parent_dirs(path);
  vfs_unlink(path);
  f = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (!f)
    return -1;
  written = vfs_write(f, (const char *)data, size);
  vfs_close(f);
  if (written < 0)
    return (int)written;
  if ((size_t)written != size)
    return -EIO;
  return 0;
}

static int installer_write_target_file(const char *logical_path,
                                       const uint8_t *data, size_t size) {
  char physical_path[256];
  int wrote_physical = 0;

  if (installer_write_raw_file(logical_path, data, size) != 0)
    return -1;
  if (installer_journal_install_write(logical_path, data, size) != 0) {
    char msg[320];
    str_copy_safe(msg, "journal write unavailable: ", sizeof(msg));
    installer_append_to_buf(msg, sizeof(msg), logical_path);
    installer_log(msg);
  }
  if (installer_translate_target_path(logical_path, physical_path,
                                      sizeof(physical_path)) == 0) {
    if (installer_write_raw_file(physical_path, data, size) != 0)
      return -1;
    wrote_physical = 1;
  }
  runtime_sync_flush_best_effort(logical_path);
  if (wrote_physical)
    runtime_sync_flush_best_effort(physical_path);
  return 0;
}

static int installer_write_target_text_file(const char *logical_path,
                                            const char *content) {
  size_t len = 0;
  if (!content)
    return -1;
  while (content[len])
    len++;
  return installer_write_target_file(logical_path, (const uint8_t *)content,
                                     len);
}

static int read_text_file(const char *path, char *buf, int max) {
  uint8_t *data;
  size_t size;
  char resolved_path[256];
  const char *target;

  if (!path || !buf || max <= 1)
    return -1;

  target = resolve_user_storage_path(path, resolved_path, sizeof(resolved_path));
  if (media_load_file(target, &data, &size) != 0) {
    if (str_cmp(target, path) == 0 || media_load_file(path, &data, &size) != 0)
      return -1;
  }
  if ((int)size >= max)
    size = (size_t)(max - 1);
  for (size_t i = 0; i < size; i++) {
    buf[i] = (char)data[i];
  }
  buf[size] = '\0';
  media_free_file(data);
  return (int)size;
}

typedef struct {
  char src_root[256];
  char dst_root[256];
} runtime_sync_ctx_t;

static void runtime_sync_append_log_raw(const char *path, const char *line) {
  struct file *f;
  int len = 0;

  if (!path || !line)
    return;

  while (line[len])
    len++;

  installer_ensure_parent_dirs(path);
  f = vfs_open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (!f)
    return;

  if (len > 0)
    vfs_write(f, line, (size_t)len);
  vfs_write(f, "\n", 1);
  vfs_close(f);
}

static void runtime_sync_log_line(const char *line) {
  char resolved_path[256];
  const char *target;

  if (!line || !line[0])
    return;

  printk(KERN_INFO "SYNC: %s\n", line);
  runtime_sync_append_log_raw("/System/sync.log", line);

  target =
      resolve_user_storage_path("/System/sync.log", resolved_path, sizeof(resolved_path));
  if (str_cmp(target, "/System/sync.log") != 0)
    runtime_sync_append_log_raw(target, line);
}

static void runtime_sync_flush_best_effort(const char *path) {
  char log_line[384];
  extern int ext4_vfs_sync(void);
  int sync_ret = ext4_vfs_sync();

  if (sync_ret == 0) {
    str_copy_safe(log_line, "sync flush complete: ", sizeof(log_line));
    installer_append_to_buf(log_line, sizeof(log_line), path);
    runtime_sync_log_line(log_line);
    return;
  }

  str_copy_safe(log_line, "sync flush unavailable: ", sizeof(log_line));
  installer_append_to_buf(log_line, sizeof(log_line), path);
  runtime_sync_log_line(log_line);
}

static int runtime_sync_raw_file(const char *src_path, const char *dst_path) {
  uint8_t *data = NULL;
  size_t size = 0;
  char log_line[384];
  struct file *dst;

  if (media_load_file(src_path, &data, &size) != 0) {
    str_copy_safe(log_line, "sync source read failed: ", sizeof(log_line));
    installer_append_to_buf(log_line, sizeof(log_line), src_path);
    runtime_sync_log_line(log_line);
    return -1;
  }

  installer_ensure_parent_dirs(dst_path);
  if (size == 0) {
    vfs_unlink(dst_path);
    dst = vfs_open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst)
      vfs_close(dst);
    media_free_file(data);
    if (dst) {
      str_copy_safe(log_line, "sync created empty file: ", sizeof(log_line));
      installer_append_to_buf(log_line, sizeof(log_line), dst_path);
      runtime_sync_log_line(log_line);
      return 0;
    }
    str_copy_safe(log_line, "sync empty file create failed: ", sizeof(log_line));
    installer_append_to_buf(log_line, sizeof(log_line), dst_path);
    runtime_sync_log_line(log_line);
    return -1;
  }

  if (installer_write_raw_file(dst_path, data, size) != 0) {
    media_free_file(data);
    str_copy_safe(log_line, "sync file write failed: ", sizeof(log_line));
    installer_append_to_buf(log_line, sizeof(log_line), dst_path);
    runtime_sync_log_line(log_line);
    return -1;
  }
  media_free_file(data);
  runtime_sync_flush_best_effort(dst_path);
  str_copy_safe(log_line, "sync copied file: ", sizeof(log_line));
  installer_append_to_buf(log_line, sizeof(log_line), src_path);
  installer_append_to_buf(log_line, sizeof(log_line), " -> ");
  installer_append_to_buf(log_line, sizeof(log_line), dst_path);
  runtime_sync_log_line(log_line);
  return 0;
}

static int runtime_sync_tree_callback(void *ctx, const char *name, int len,
                                      loff_t offset, ino_t ino, unsigned type) {
  runtime_sync_ctx_t *sync = (runtime_sync_ctx_t *)ctx;
  char src_path[256];
  char dst_path[256];
  int src_idx = 0;
  int dst_idx = 0;

  (void)offset;
  (void)ino;

  if (!sync || !name || len <= 0)
    return 0;
  if ((len == 1 && name[0] == '.') ||
      (len == 2 && name[0] == '.' && name[1] == '.'))
    return 0;

  str_copy_safe(src_path, sync->src_root, sizeof(src_path));
  while (src_path[src_idx] && src_idx < (int)sizeof(src_path) - 1)
    src_idx++;
  if (src_idx < (int)sizeof(src_path) - 1)
    src_path[src_idx++] = '/';
  for (int i = 0; i < len && src_idx < (int)sizeof(src_path) - 1; i++)
    src_path[src_idx++] = name[i];
  src_path[src_idx] = '\0';

  str_copy_safe(dst_path, sync->dst_root, sizeof(dst_path));
  while (dst_path[dst_idx] && dst_idx < (int)sizeof(dst_path) - 1)
    dst_idx++;
  if (dst_idx < (int)sizeof(dst_path) - 1)
    dst_path[dst_idx++] = '/';
  for (int i = 0; i < len && dst_idx < (int)sizeof(dst_path) - 1; i++)
    dst_path[dst_idx++] = name[i];
  dst_path[dst_idx] = '\0';

  if (type == 4) {
    struct file *dir;
    runtime_sync_ctx_t next;

    installer_ensure_parent_dirs(dst_path);
    vfs_mkdir(dst_path, 0755);
    dir = vfs_open(src_path, O_RDONLY, 0);
    if (!dir)
      return 0;
    str_copy_safe(next.src_root, src_path, sizeof(next.src_root));
    str_copy_safe(next.dst_root, dst_path, sizeof(next.dst_root));
    vfs_readdir(dir, &next, runtime_sync_tree_callback);
    vfs_close(dir);
    return 0;
  }

  runtime_sync_raw_file(src_path, dst_path);
  return 0;
}

static void runtime_sync_dir_from_boot_storage(const char *relative_path) {
  char boot_root[96];
  char log_line[384];
  char src_root[256];
  char dst_root[256];
  struct file *dir;
  runtime_sync_ctx_t ctx;

  if (!relative_path || !relative_path[0] ||
      boot_storage_root_path(boot_root, sizeof(boot_root)) != 0)
    return;

  str_copy_safe(src_root, boot_root, sizeof(src_root));
  installer_append_to_buf(src_root, sizeof(src_root), "/");
  installer_append_to_buf(src_root, sizeof(src_root), relative_path);

  dst_root[0] = '/';
  dst_root[1] = '\0';
  installer_append_to_buf(dst_root, sizeof(dst_root), relative_path);

  dir = vfs_open(src_root, O_RDONLY, 0);
  if (!dir) {
    return;
  }

  installer_ensure_parent_dirs(dst_root);
  vfs_mkdir(dst_root, 0755);
  str_copy_safe(ctx.src_root, src_root, sizeof(ctx.src_root));
  str_copy_safe(ctx.dst_root, dst_root, sizeof(ctx.dst_root));
  str_copy_safe(log_line, "sync directory start: ", sizeof(log_line));
  installer_append_to_buf(log_line, sizeof(log_line), src_root);
  installer_append_to_buf(log_line, sizeof(log_line), " -> ");
  installer_append_to_buf(log_line, sizeof(log_line), dst_root);
  runtime_sync_log_line(log_line);
  vfs_readdir(dir, &ctx, runtime_sync_tree_callback);
  vfs_close(dir);
  str_copy_safe(log_line, "sync directory complete: ", sizeof(log_line));
  installer_append_to_buf(log_line, sizeof(log_line), src_root);
  runtime_sync_log_line(log_line);
}

static void runtime_sync_boot_storage_to_live(void) {
  static const char *dirs[] = {"System", "Applications", "Desktop",
                               "Documents", "Pictures", "Music", "Users"};

  {
    extern int boot_is_usb_boot(void);
    if (boot_is_usb_boot())
      return;
  }

  for (int i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])); i++)
    runtime_sync_dir_from_boot_storage(dirs[i]);
}

static int manifest_get_value(const char *manifest, const char *key, char *out,
                              int max) {
  int key_len = 0;
  int i = 0;

  if (!manifest || !key || !out || max <= 0)
    return -1;

  while (key[key_len])
    key_len++;

  while (manifest[i]) {
    int j = 0;
    while (j < key_len && manifest[i + j] == key[j])
      j++;
    if (j == key_len && manifest[i + j] == '=') {
      int out_idx = 0;
      i += key_len + 1;
      while (manifest[i] && manifest[i] != '\n' && manifest[i] != '\r' &&
             out_idx < max - 1) {
        out[out_idx++] = manifest[i++];
      }
      out[out_idx] = '\0';
      return 0;
    }
    while (manifest[i] && manifest[i] != '\n')
      i++;
    if (manifest[i] == '\n')
      i++;
  }

  out[0] = '\0';
  return -1;
}

static int app_is_installed(const dock_app_def_t *app) {
  char path[128];
  uint8_t *data = NULL;
  size_t size = 0;
  if (!app)
    return 0;
  if (app_manifest_path(app, path, sizeof(path)) != 0)
    return 0;
  if (media_load_file(path, &data, &size) != 0)
    return 0;
  media_free_file(data);
  return 1;
}

static int load_app_id_from_manifest_file(const char *path, const char *fallback,
                                          char *app_id, int max) {
  char manifest[160];

  if (!app_id || max <= 0)
    return -1;
  app_id[0] = '\0';

  if (path && read_text_file(path, manifest, sizeof(manifest)) >= 0 &&
      manifest_get_value(manifest, "id", app_id, max) == 0 && app_id[0]) {
    return 0;
  }

  if (fallback) {
    int out = 0;
    for (int i = 0; fallback[i] && fallback[i] != '.' && out < max - 1; i++) {
      char c = fallback[i];
      if (c >= 'A' && c <= 'Z')
        c = (char)(c + 32);
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        app_id[out++] = c;
    }
    app_id[out] = '\0';
  }

  return app_id[0] ? 0 : -1;
}

typedef struct {
  char path[160];
  int found;
} account_scan_ctx_t;

typedef struct {
  char(*names)[32];
  int count;
  int capacity;
} settings_account_list_t;

static void settings_account_list_init(settings_account_list_t *list) {
  if (!list)
    return;
  list->names = NULL;
  list->count = 0;
  list->capacity = 0;
}

static void settings_account_list_free(settings_account_list_t *list) {
  if (!list)
    return;
  if (list->names)
    kfree(list->names);
  list->names = NULL;
  list->count = 0;
  list->capacity = 0;
}

static int settings_account_list_append(settings_account_list_t *list,
                                        const char *name) {
  char(*new_names)[32];
  int new_capacity;

  if (!list || !name || !name[0])
    return -1;

  if (list->count >= list->capacity) {
    new_capacity = list->capacity > 0 ? list->capacity * 2
                                      : SETTINGS_ACCOUNT_LIST_INITIAL_CAPACITY;
    new_names = kmalloc(sizeof(*new_names) * new_capacity);
    if (!new_names)
      return -1;
    for (int i = 0; i < list->count; i++)
      str_copy_safe(new_names[i], list->names[i], sizeof(new_names[i]));
    if (list->names)
      kfree(list->names);
    list->names = new_names;
    list->capacity = new_capacity;
  }

  str_copy_safe(list->names[list->count], name, sizeof(list->names[list->count]));
  list->count++;
  return 0;
}

static int settings_account_list_index_of(const settings_account_list_t *list,
                                          const char *name) {
  if (!list || !name || !name[0])
    return -1;
  for (int i = 0; i < list->count; i++) {
    if (str_cmp(list->names[i], name) == 0)
      return i;
  }
  return -1;
}

static int settings_account_list_visible_start(
    const settings_account_list_t *list, const char *selected, int max_visible) {
  int selected_idx;
  int start;

  if (!list || list->count <= 0 || max_visible <= 0 || list->count <= max_visible)
    return 0;

  selected_idx = settings_account_list_index_of(list, selected);
  if (selected_idx < 0)
    return 0;

  start = selected_idx - max_visible / 2;
  if (start < 0)
    start = 0;
  if (start > list->count - max_visible)
    start = list->count - max_visible;
  return start;
}

static int find_first_account_manifest_callback(void *raw_ctx, const char *name,
                                                int len, loff_t offset,
                                                ino_t ino, unsigned type) {
  account_scan_ctx_t *scan = (account_scan_ctx_t *)raw_ctx;
  int idx = 0;

  (void)offset;
  (void)ino;

  if (!scan || scan->found || !name || len < 5 || type == 4)
    return 0;
  if (name[len - 4] != '.' || name[len - 3] != 'c' || name[len - 2] != 'f' ||
      name[len - 1] != 'g')
    return 0;

  str_copy_safe(scan->path, GUI_ACCOUNTS_DIR, sizeof(scan->path));
  while (scan->path[idx] && idx < (int)sizeof(scan->path) - 1)
    idx++;
  if (idx < (int)sizeof(scan->path) - 1)
    scan->path[idx++] = '/';
  for (int i = 0; i < len && idx < (int)sizeof(scan->path) - 1; i++)
    scan->path[idx++] = name[i];
  scan->path[idx] = '\0';
  scan->found = 1;
  return 0;
}

static int settings_collect_accounts_callback(void *raw_ctx, const char *name,
                                              int len, loff_t offset,
                                              ino_t ino, unsigned type) {
  settings_account_list_t *list = (settings_account_list_t *)raw_ctx;
  char account_name[32];
  int copy_len = 0;

  (void)offset;
  (void)ino;

  if (!list || !name || type == 4 || len < 5)
    return 0;
  if (name[len - 4] != '.' || name[len - 3] != 'c' || name[len - 2] != 'f' ||
      name[len - 1] != 'g')
    return 0;

  copy_len = len - 4;
  if (copy_len > 31)
    copy_len = 31;
  for (int i = 0; i < copy_len; i++)
    account_name[i] = name[i];
  account_name[copy_len] = '\0';
  settings_account_list_append(list, account_name);
  return 0;
}

static void settings_collect_accounts(settings_account_list_t *list) {
  struct file *dir;

  if (!list)
    return;
  settings_account_list_free(list);
  settings_account_list_init(list);

  dir = vfs_open(GUI_ACCOUNTS_DIR, O_RDONLY, 0);
  if (dir) {
    vfs_readdir(dir, list, settings_collect_accounts_callback);
    vfs_close(dir);
  }

  if (account_username[0]) {
    int found = 0;
    for (int i = 0; i < list->count; i++) {
      if (str_cmp(list->names[i], account_username) == 0) {
        found = 1;
        break;
      }
    }
    if (!found)
      settings_account_list_append(list, account_username);
  }
}

static void settings_sync_selected_user(void) {
  settings_account_list_t list;
  int found = 0;

  settings_account_list_init(&list);
  settings_collect_accounts(&list);
  if (!settings_user_selected[0]) {
    if (list.count > 0)
      str_copy_safe(settings_user_selected, list.names[0],
                    sizeof(settings_user_selected));
    settings_account_list_free(&list);
    return;
  }
  for (int i = 0; i < list.count; i++) {
    if (str_cmp(settings_user_selected, list.names[i]) == 0) {
      found = 1;
      break;
    }
  }
  if (!found) {
    if (list.count > 0)
      str_copy_safe(settings_user_selected, list.names[0],
                    sizeof(settings_user_selected));
    else
      settings_user_selected[0] = '\0';
  }
  settings_account_list_free(&list);
}

static const char *settings_user_role_for_name(const char *username,
                                               char *buf, int max) {
  char manifest[256];
  char parsed_username[32];
  char parsed_password[33];
  char parsed_role[16];
  int parsed_wallpaper = DEFAULT_WALLPAPER_INDEX;
  char parsed_partition[32];
  char parsed_disk[32];

  if (!buf || max <= 0)
    return "";
  buf[0] = '\0';
  if (!username || !username[0])
    return "";

  if (account_username[0] && str_cmp(username, account_username) == 0) {
    str_copy_safe(buf, account_role_label(), max);
    return buf;
  }

  if (read_account_manifest(username, manifest, sizeof(manifest)) != 0 ||
      parse_account_manifest(manifest, username, parsed_username,
                             sizeof(parsed_username), parsed_password,
                             sizeof(parsed_password), parsed_role,
                             sizeof(parsed_role), &parsed_wallpaper, parsed_partition,
                             sizeof(parsed_partition), parsed_disk,
                             sizeof(parsed_disk)) != 0) {
    str_copy_safe(buf, "Unknown", max);
    return buf;
  }

  if (str_cmp(parsed_role, "child") == 0)
    str_copy_safe(buf, "Child", max);
  else if (str_cmp(parsed_role, "user") == 0)
    str_copy_safe(buf, "User", max);
  else
    str_copy_safe(buf, "Admin", max);
  return buf;
}

static int account_manifest_path(const char *username, char *path, int max) {
  int idx = 0;

  if (!username || !username[0] || !path || max < 32)
    return -1;

  str_copy_safe(path, GUI_ACCOUNTS_DIR, max);
  while (path[idx])
    idx++;
  if (idx >= max - 1)
    return -1;
  path[idx++] = '/';
  for (int i = 0; username[i] && idx < max - 5; i++)
    path[idx++] = username[i];
  path[idx++] = '.';
  path[idx++] = 'c';
  path[idx++] = 'f';
  path[idx++] = 'g';
  path[idx] = '\0';
  return 0;
}


static void account_unix_home_path(const char *username, char *buf, int max) {
  int idx = 0;

  if (!buf || max <= 0)
    return;
  if (username && str_cmp(username, "root") == 0) {
    str_copy_safe(buf, "/root", max);
    return;
  }
  str_copy_safe(buf, GUI_HOME_ROOT, max);
  while (buf[idx] && idx < max - 1)
    idx++;
  if (idx < max - 1)
    buf[idx++] = '/';
  for (int i = 0; username && username[i] && idx < max - 1; i++)
    buf[idx++] = username[i];
  buf[idx] = '\0';
}

static uint32_t account_unix_uid_for_name(const char *username, int ordinal) {
  if (username && str_cmp(username, "root") == 0)
    return 0;
  if (ordinal < 0)
    ordinal = 0;
  return 1000U + (uint32_t)ordinal;
}

static void account_append_decimal(char *buf, int max, uint64_t value) {
  int idx = 0;

  if (!buf || max <= 0)
    return;
  while (buf[idx] && idx < max - 1)
    idx++;
  append_decimal(buf, &idx, value);
  buf[idx] = '\0';
}

static void account_append_line(char *buf, int max, const char *text) {
  installer_append_to_buf(buf, max, text);
  installer_append_to_buf(buf, max, "\n");
}

static int account_unix_line_matches_user(const char *line, const char *username) {
  int i = 0;

  if (!line || !username || !username[0])
    return 0;
  while (username[i] && line[i] == username[i])
    i++;
  return username[i] == '\0' && line[i] == ':';
}

static int account_unix_get_field(const char *line, int field, char *out,
                                  int out_max) {
  int current = 0;
  int idx = 0;

  if (!line || !out || out_max <= 0 || field < 0)
    return -1;
  out[0] = '\0';
  for (int i = 0; line[i] && line[i] != '\n' && line[i] != '\r'; i++) {
    if (line[i] == ':') {
      if (current == field) {
        out[idx] = '\0';
        return 0;
      }
      current++;
      idx = 0;
      continue;
    }
    if (current == field && idx < out_max - 1)
      out[idx++] = line[i];
  }
  if (current == field) {
    out[idx] = '\0';
    return 0;
  }
  return -1;
}

static int account_read_unix_manifest(const char *username, char *manifest,
                                      int max) {
  char passwd[1024];
  char shadow[1024];
  char line[256];
  char selected[256];
  char hash[64];
  char uid_buf[16];
  char gid_buf[16];
  char gecos[64];
  char home[96];
  char shell[32];
  int found = 0;
  int idx = 0;

  if (!username || !username[0] || !manifest || max <= 0)
    return -1;
  manifest[0] = '\0';
  if (read_text_file(GUI_PASSWD_PATH, passwd, sizeof(passwd)) < 0)
    return -1;

  selected[0] = '\0';
  for (int i = 0; passwd[i];) {
    int len = 0;
    while (passwd[i] && passwd[i] != '\n' && len < (int)sizeof(line) - 1)
      line[len++] = passwd[i++];
    while (passwd[i] && passwd[i] != '\n')
      i++;
    if (passwd[i] == '\n')
      i++;
    line[len] = '\0';
    if (account_unix_line_matches_user(line, username)) {
      str_copy_safe(selected, line, sizeof(selected));
      found = 1;
      break;
    }
  }
  if (!found)
    return -1;

  hash[0] = '\0';
  if (read_text_file(GUI_SHADOW_PATH, shadow, sizeof(shadow)) >= 0) {
    for (int i = 0; shadow[i];) {
      int len = 0;
      while (shadow[i] && shadow[i] != '\n' && len < (int)sizeof(line) - 1)
        line[len++] = shadow[i++];
      while (shadow[i] && shadow[i] != '\n')
        i++;
      if (shadow[i] == '\n')
        i++;
      line[len] = '\0';
      if (account_unix_line_matches_user(line, username)) {
        account_unix_get_field(line, 1, hash, sizeof(hash));
        break;
      }
    }
  }
  if (!hash[0])
    account_unix_get_field(selected, 1, hash, sizeof(hash));
  if (!hash[0] || str_cmp(hash, "x") == 0)
    return -1;

  account_unix_get_field(selected, 2, uid_buf, sizeof(uid_buf));
  account_unix_get_field(selected, 3, gid_buf, sizeof(gid_buf));
  account_unix_get_field(selected, 4, gecos, sizeof(gecos));
  account_unix_get_field(selected, 5, home, sizeof(home));
  account_unix_get_field(selected, 6, shell, sizeof(shell));

  for (const char *p = "username="; *p && idx < max - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; username[i] && idx < max - 1; i++)
    manifest[idx++] = username[i];
  manifest[idx++] = '\n';
  for (const char *p = "password_hash="; *p && idx < max - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; hash[i] && idx < max - 1; i++)
    manifest[idx++] = hash[i];
  manifest[idx++] = '\n';
  for (const char *p = "role="; *p && idx < max - 1; p++)
    manifest[idx++] = *p;
  {
    int admin_role = uid_buf[0] == '0' && uid_buf[1] == '\0';
    for (int i = 0; gecos[i]; i++) {
      if (gecos[i] == 'a' && gecos[i + 1] == 'd' && gecos[i + 2] == 'm' &&
          gecos[i + 3] == 'i' && gecos[i + 4] == 'n')
        admin_role = 1;
    }
    if (admin_role)
      for (const char *p = "admin"; *p && idx < max - 1; p++)
        manifest[idx++] = *p;
    else
      for (const char *p = "user"; *p && idx < max - 1; p++)
        manifest[idx++] = *p;
  }
  manifest[idx++] = '\n';
  for (const char *p = "wallpaper=4\npartition_label=\ndisk_location=\n";
       *p && idx < max - 1; p++)
    manifest[idx++] = *p;
  manifest[idx] = '\0';
  (void)gid_buf;
  (void)gecos;
  (void)home;
  (void)shell;
  return 0;
}

static int read_account_manifest(const char *username, char *manifest, int max) {
  char path[160];

  if (!manifest || max <= 0)
    return -1;

  if (username && username[0]) {
    if (account_read_unix_manifest(username, manifest, max) == 0)
      return 0;
    if (account_manifest_path(username, path, sizeof(path)) != 0)
      return -1;
    return read_text_file(path, manifest, max) >= 0 ? 0 : -1;
  }

  if (read_text_file(GUI_ACCOUNT_PATH, manifest, max) >= 0)
    return 0;

  {
    struct file *dir = vfs_open(GUI_ACCOUNTS_DIR, O_RDONLY, 0);
    if (dir) {
      account_scan_ctx_t ctx;
      ctx.path[0] = '\0';
      ctx.found = 0;
      vfs_readdir(dir, &ctx, find_first_account_manifest_callback);
      vfs_close(dir);
      if (ctx.found && read_text_file(ctx.path, manifest, max) >= 0)
        return 0;
    }
  }
  return -1;
}

static int account_partition_manifest_info(const char *preferred_label,
                                           int *disk_index_out,
                                           uint32_t *start_lba_out,
                                           uint32_t *sector_count_out) {
  char disk_location[32];
  int disk_index;
  int partition_count;
  int first_data_index = -1;
  char label[32];
  storage_partition_kind_t kind;
  uint32_t start_lba = 0;
  uint32_t sector_count = 0;

  extern int storage_get_disk_index_by_location(const char *location);
  extern int storage_get_partition_count(int disk_index);
  extern int storage_get_partition_info(int disk_index, int partition_index,
                                        storage_partition_kind_t *kind,
                                        char *label, int label_max,
                                        uint32_t *start_lba,
                                        uint32_t *sector_count);

  disk_location[0] = '\0';
  if (account_disk_location[0]) {
    str_copy_safe(disk_location, account_disk_location, sizeof(disk_location));
  } else if (load_install_target_disk_location(disk_location,
                                               sizeof(disk_location)) != 0) {
    return -1;
  }

  disk_index = storage_get_disk_index_by_location(disk_location);
  if (disk_index < 0)
    return -1;

  partition_count = storage_get_partition_count(disk_index);
  for (int i = 0; i < partition_count; i++) {
    label[0] = '\0';
    if (storage_get_partition_info(disk_index, i, &kind, label, sizeof(label),
                                   &start_lba, &sector_count) != 0)
      continue;
    if (kind != STORAGE_PARTITION_DATA)
      continue;
    if (first_data_index < 0)
      first_data_index = i;
    if (!preferred_label || !preferred_label[0] ||
        str_cmp(label, preferred_label) == 0) {
      if (disk_index_out)
        *disk_index_out = disk_index;
      if (start_lba_out)
        *start_lba_out = start_lba;
      if (sector_count_out)
        *sector_count_out = sector_count;
      return 0;
    }
  }

  if (first_data_index >= 0 &&
      storage_get_partition_info(disk_index, first_data_index, &kind, label,
                                 sizeof(label), &start_lba,
                                 &sector_count) == 0 &&
      kind == STORAGE_PARTITION_DATA) {
    if (disk_index_out)
      *disk_index_out = disk_index;
    if (start_lba_out)
      *start_lba_out = start_lba;
    if (sector_count_out)
      *sector_count_out = sector_count;
    return 0;
  }

  return -1;
}

static uint32_t account_partition_manifest_lba(uint32_t start_lba,
                                               uint32_t sector_count) {
  if (sector_count < ACCOUNT_RAW_SECTORS)
    return start_lba;
  /* Keep the raw manifest away from partition headers and boot sectors. */
  return start_lba + sector_count - ACCOUNT_RAW_SECTORS;
}

static int load_account_manifest_from_partition(char *manifest, int max) {
  uint8_t *buf;
  uint32_t start_lba = 0;
  uint32_t sector_count = 0;
  uint32_t manifest_lba = 0;
  uint32_t manifest_len;
  int disk_index = -1;
  int ret = -1;

  extern int storage_read_block(int disk_index, uint32_t lba, void *buffer,
                                uint32_t block_size);

  if (!manifest || max <= 1)
    return -1;
  if (account_partition_manifest_info(account_partition_label, &disk_index,
                                      &start_lba, &sector_count) != 0)
    return -1;
  if (sector_count < ACCOUNT_RAW_SECTORS)
    return -1;
  manifest_lba = account_partition_manifest_lba(start_lba, sector_count);

  buf = (uint8_t *)kmalloc(ACCOUNT_RAW_BYTES, GFP_KERNEL);
  if (!buf)
    return -1;

  for (int sector = 0; sector < ACCOUNT_RAW_SECTORS; sector++) {
    if (storage_read_block(disk_index, manifest_lba + (uint32_t)sector,
                           buf + sector * 512, 512) != 0)
      goto done;
  }

  if (buf[0] != 'O' || buf[1] != 'S' || buf[2] != '8' || buf[3] != 'A' ||
      buf[4] != 'C' || buf[5] != 'C' || buf[6] != 'T' || buf[7] != '1') {
    for (int sector = 0; sector < ACCOUNT_RAW_SECTORS; sector++) {
      if (storage_read_block(disk_index, start_lba + (uint32_t)sector,
                             buf + sector * 512, 512) != 0)
        goto done;
    }
    if (buf[0] != 'O' || buf[1] != 'S' || buf[2] != '8' || buf[3] != 'A' ||
        buf[4] != 'C' || buf[5] != 'C' || buf[6] != 'T' || buf[7] != '1')
      goto done;
  }

  manifest_len = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) |
                 ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
  if (manifest_len == 0 || manifest_len >= (uint32_t)max ||
      manifest_len > (uint32_t)(ACCOUNT_RAW_BYTES - 16))
    goto done;

  for (uint32_t i = 0; i < manifest_len; i++)
    manifest[i] = (char)buf[16 + i];
  manifest[manifest_len] = '\0';
  ret = 0;

done:
  kfree(buf);
  return ret;
}

static int save_account_manifest_to_partition(const char *manifest) {
  uint8_t *buf;
  uint32_t start_lba = 0;
  uint32_t sector_count = 0;
  uint32_t manifest_lba = 0;
  uint32_t manifest_len = 0;
  int disk_index = -1;
  int ret = -1;

  extern int storage_write_block(int disk_index, uint32_t lba,
                                 const void *buffer, uint32_t block_size);

  if (!manifest)
    return -1;
  while (manifest[manifest_len])
    manifest_len++;
  if (manifest_len == 0 || manifest_len > (uint32_t)(ACCOUNT_RAW_BYTES - 16))
    return -1;
  if (account_partition_manifest_info(account_partition_label, &disk_index,
                                      &start_lba, &sector_count) != 0)
    return -1;
  if (sector_count < ACCOUNT_RAW_SECTORS)
    return -1;
  manifest_lba = account_partition_manifest_lba(start_lba, sector_count);

  buf = (uint8_t *)kmalloc(ACCOUNT_RAW_BYTES, GFP_KERNEL);
  if (!buf)
    return -1;
  for (int i = 0; i < ACCOUNT_RAW_BYTES; i++)
    buf[i] = 0;

  buf[0] = 'O';
  buf[1] = 'S';
  buf[2] = '8';
  buf[3] = 'A';
  buf[4] = 'C';
  buf[5] = 'C';
  buf[6] = 'T';
  buf[7] = '1';
  buf[8] = (uint8_t)(manifest_len & 0xFF);
  buf[9] = (uint8_t)((manifest_len >> 8) & 0xFF);
  buf[10] = (uint8_t)((manifest_len >> 16) & 0xFF);
  buf[11] = (uint8_t)((manifest_len >> 24) & 0xFF);
  for (uint32_t i = 0; i < manifest_len; i++)
    buf[16 + i] = (uint8_t)manifest[i];

  for (int sector = 0; sector < ACCOUNT_RAW_SECTORS; sector++) {
    if (storage_write_block(disk_index, manifest_lba + (uint32_t)sector,
                            buf + sector * 512, 512) != 0)
      goto done;
  }

  ret = 0;
done:
  kfree(buf);
  return ret;
}

static int parse_account_manifest(const char *manifest, const char *fallback_name,
                                  char *username, int username_max,
                                  char *password_hash, int password_hash_max,
                                  char *role, int role_max,
                                  int *wallpaper_index,
                                  char *partition_label, int partition_label_max,
                                  char *disk_location, int disk_location_max) {
  char legacy_password[32];
  char wallpaper_buf[16];

  if (!manifest)
    return -1;

  if (username && username_max > 0)
    username[0] = '\0';
  if (password_hash && password_hash_max > 0)
    password_hash[0] = '\0';
  if (role && role_max > 0)
    str_copy_safe(role, "admin", role_max);
  if (wallpaper_index)
    *wallpaper_index = DEFAULT_WALLPAPER_INDEX;
  if (partition_label && partition_label_max > 0)
    partition_label[0] = '\0';
  if (disk_location && disk_location_max > 0)
    disk_location[0] = '\0';

  if (username && username_max > 0) {
    manifest_get_value(manifest, "username", username, username_max);
    if (!username[0] && fallback_name)
      str_copy_safe(username, fallback_name, username_max);
  }
  if (partition_label && partition_label_max > 0)
    manifest_get_value(manifest, "partition_label", partition_label,
                       partition_label_max);
  if (role && role_max > 0 && manifest_get_value(manifest, "role", role,
                                                 role_max) == 0) {
    if (str_cmp(role, "admin") != 0 && str_cmp(role, "user") != 0 &&
        str_cmp(role, "child") != 0) {
      str_copy_safe(role, "user", role_max);
    }
  }
  wallpaper_buf[0] = '\0';
  if (wallpaper_index &&
      manifest_get_value(manifest, "wallpaper", wallpaper_buf,
                         sizeof(wallpaper_buf)) == 0 &&
      wallpaper_buf[0]) {
    *wallpaper_index = clamp_wallpaper_index((int)parse_u64(wallpaper_buf));
  }
  if (disk_location && disk_location_max > 0)
    manifest_get_value(manifest, "disk_location", disk_location,
                       disk_location_max);
  if (password_hash && password_hash_max > 0 &&
      manifest_get_value(manifest, "password_hash", password_hash,
                         password_hash_max) == 0 &&
      password_hash[0]) {
    return 0;
  }

  legacy_password[0] = '\0';
  if (password_hash && password_hash_max > 0 && username && username[0] &&
      manifest_get_value(manifest, "password", legacy_password,
                         sizeof(legacy_password)) == 0 &&
      legacy_password[0]) {
    vib_password_hash_hex(username, legacy_password, password_hash,
                          password_hash_max);
    return 0;
  }

  return -1;
}

static int settings_add_user_account(void) {
  char manifest[256];
  char path[160];
  char password_hash[33];
  char user_home[96];
  const char *role_value;
  int wallpaper_value;
  int idx = 0;

  if (!account_role_is_admin()) {
    str_copy_safe(settings_status, "Admin mode is required to add users.",
                  sizeof(settings_status));
    return -1;
  }
  if (!settings_user_new_name[0] || !settings_user_new_password[0]) {
    str_copy_safe(settings_status, "Enter a username and password first.",
                  sizeof(settings_status));
    return -1;
  }
  if (account_manifest_path(settings_user_new_name, path, sizeof(path)) != 0) {
    str_copy_safe(settings_status, "Username is invalid or too long.",
                  sizeof(settings_status));
    return -1;
  }
  if (read_text_file(path, manifest, sizeof(manifest)) >= 0 ||
      (account_username[0] &&
       str_cmp(account_username, settings_user_new_name) == 0)) {
    str_copy_safe(settings_status, "That user already exists.",
                  sizeof(settings_status));
    return -1;
  }

  vib_password_hash_hex(settings_user_new_name, settings_user_new_password,
                        password_hash, sizeof(password_hash));
  role_value = settings_user_role_options[settings_user_new_role_idx];
  wallpaper_value = clamp_wallpaper_index(DEFAULT_WALLPAPER_INDEX);
  idx = 0;
  for (const char *p = "username="; *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; settings_user_new_name[i] &&
                  idx < (int)sizeof(manifest) - 2;
       i++)
    manifest[idx++] = settings_user_new_name[i];
  manifest[idx++] = '\n';
  for (const char *p = "password_hash=";
       *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; password_hash[i] && idx < (int)sizeof(manifest) - 2; i++)
    manifest[idx++] = password_hash[i];
  manifest[idx++] = '\n';
  for (const char *p = "role="; *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; role_value[i] && idx < (int)sizeof(manifest) - 2; i++)
    manifest[idx++] = role_value[i];
  manifest[idx++] = '\n';
  for (const char *p = "wallpaper="; *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  manifest[idx++] = (char)('0' + wallpaper_value);
  manifest[idx++] = '\n';
  for (const char *p = "partition_label=";
       *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; account_partition_label[i] &&
                  idx < (int)sizeof(manifest) - 2;
       i++)
    manifest[idx++] = account_partition_label[i];
  manifest[idx++] = '\n';
  for (const char *p = "disk_location=";
       *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; account_disk_location[i] &&
                  idx < (int)sizeof(manifest) - 2;
       i++)
    manifest[idx++] = account_disk_location[i];
  manifest[idx++] = '\n';
  manifest[idx] = '\0';

  ensure_gui_app_dirs();
  if (write_text_file(path, manifest) != 0) {
    str_copy_safe(settings_status, "Failed to save the new user.",
                  sizeof(settings_status));
    return -1;
  }

  str_copy_safe(user_home, "/Users/", sizeof(user_home));
  idx = 7;
  for (int i = 0; settings_user_new_name[i] && idx < (int)sizeof(user_home) - 1;
       i++)
    user_home[idx++] = settings_user_new_name[i];
  user_home[idx] = '\0';
  user_storage_mkdir(user_home, 0755);
  account_write_unix_databases();

  str_copy_safe(settings_user_selected, settings_user_new_name,
                sizeof(settings_user_selected));
  settings_user_new_name[0] = '\0';
  settings_user_new_password[0] = '\0';
  settings_user_new_role_idx = 1;
  settings_user_active_field = 0;
  str_copy_safe(settings_status, "User added successfully.",
                sizeof(settings_status));
  return 0;
}

static int settings_remove_selected_user_account(void) {
  char path[160];
  char user_home[96];
  int idx = 0;

  if (!account_role_is_admin()) {
    str_copy_safe(settings_status, "Admin mode is required to remove users.",
                  sizeof(settings_status));
    return -1;
  }
  if (!settings_user_selected[0]) {
    str_copy_safe(settings_status, "Select a user to remove.",
                  sizeof(settings_status));
    return -1;
  }
  if (account_username[0] &&
      str_cmp(settings_user_selected, account_username) == 0) {
    str_copy_safe(settings_status, "Sign out before removing the current user.",
                  sizeof(settings_status));
    return -1;
  }
  if (account_manifest_path(settings_user_selected, path, sizeof(path)) != 0) {
    str_copy_safe(settings_status, "Could not resolve that user account.",
                  sizeof(settings_status));
    return -1;
  }
  if (vfs_unlink(path) != 0) {
    str_copy_safe(settings_status, "Failed to remove the selected user.",
                  sizeof(settings_status));
    return -1;
  }

  str_copy_safe(user_home, "/Users/", sizeof(user_home));
  idx = 7;
  for (int i = 0; settings_user_selected[i] &&
                  idx < (int)sizeof(user_home) - 1;
       i++)
    user_home[idx++] = settings_user_selected[i];
  user_home[idx] = '\0';
  user_storage_rmdir(user_home);
  account_write_unix_databases();

  settings_user_selected[0] = '\0';
  settings_sync_selected_user();
  str_copy_safe(settings_status,
                "User removed. Existing files were left in place if not empty.",
                sizeof(settings_status));
  return 0;
}

static void set_startup_status(const char *message) {
  str_copy_safe(startup_status, message, sizeof(startup_status));
}

static int startup_account_system_ready(void) {
  extern int boot_is_live_media(void);
  extern int boot_is_usb_boot(void);

  if (account_partition_storage_ready)
    return 1;

  return boot_is_live_media() || boot_is_usb_boot();
}

static int startup_flow_active(void) {
  return startup_flow != STARTUP_FLOW_NONE;
}

static int startup_setup_account_active(void) {
  return startup_flow == STARTUP_FLOW_SETUP_ACCOUNT;
}

static int startup_setup_welcome_active(void) {
  return startup_setup_account_active() &&
         startup_setup_page == STARTUP_SETUP_PAGE_WELCOME;
}

static int startup_setup_account_form_active(void) {
  return startup_setup_account_active() &&
         startup_setup_page == STARTUP_SETUP_PAGE_ACCOUNT;
}

static int startup_setup_storage_active(void) {
  return startup_setup_account_active() &&
         startup_setup_page == STARTUP_SETUP_PAGE_STORAGE;
}

static void startup_close_other_windows(void) {
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != 0) {
      gui_destroy_window(&windows[i]);
    }
  }
  focused_window = NULL;
  startup_window = NULL;
}

static void startup_get_setup_layout(int content_x, int content_y, int content_w,
                                     int content_h, int *panel_x,
                                     int *panel_y, int *panel_w, int *panel_h,
                                     int *rail_w, int *card_x, int *card_y,
                                     int *card_w, int *card_h) {
  int margin_x = content_w > 900 ? 64 : 24;
  int margin_y = content_h > 640 ? 48 : 24;
  int local_panel_x = content_x + margin_x;
  int local_panel_y = content_y + margin_y;
  int local_panel_w = content_w - margin_x * 2;
  int local_panel_h = content_h - margin_y * 2;
  int local_rail_w = local_panel_w > 760 ? 260 : 210;
  int local_card_x;
  int local_card_y;
  int local_card_w;
  int local_card_h;

  if (local_panel_w < 320)
    local_panel_w = 320;
  if (local_panel_h < 220)
    local_panel_h = 220;

  local_card_x = local_panel_x + local_rail_w + 24;
  local_card_y = local_panel_y + 24;
  local_card_w = local_panel_w - local_rail_w - 48;
  local_card_h = local_panel_h - 48;

  if (local_card_w < 220)
    local_card_w = 220;
  if (local_card_h < 160)
    local_card_h = 160;

  if (panel_x)
    *panel_x = local_panel_x;
  if (panel_y)
    *panel_y = local_panel_y;
  if (panel_w)
    *panel_w = local_panel_w;
  if (panel_h)
    *panel_h = local_panel_h;
  if (rail_w)
    *rail_w = local_rail_w;
  if (card_x)
    *card_x = local_card_x;
  if (card_y)
    *card_y = local_card_y;
  if (card_w)
    *card_w = local_card_w;
  if (card_h)
    *card_h = local_card_h;
}

static void startup_get_setup_button_rect(int content_x, int content_y,
                                          int content_w, int content_h, int *x,
                                          int *y, int *w, int *h) {
  int card_x = 0, card_y = 0, card_w = 0, card_h = 0;
  startup_get_setup_layout(content_x, content_y, content_w, content_h, NULL,
                           NULL, NULL, NULL, NULL, &card_x, &card_y, &card_w,
                           &card_h);
  if (x)
    *x = card_x + 36;
  if (y)
    *y = card_y + card_h - 78;
  if (w)
    *w = 220;
  if (h)
    *h = 42;
}

static void startup_get_setup_field_rect(int content_x, int content_y,
                                         int content_w, int content_h,
                                         int field_index, int *x, int *y,
                                         int *w, int *h) {
  int card_x = 0, card_y = 0, card_w = 0;
  startup_get_setup_layout(content_x, content_y, content_w, content_h, NULL,
                           NULL, NULL, NULL, NULL, &card_x, &card_y, &card_w,
                           NULL);
  if (x)
    *x = card_x + 36;
  if (y)
    *y = card_y + (field_index == 0 ? 132 : 216);
  if (w)
    *w = card_w - 72;
  if (h)
    *h = 42;
}

static const char *installed_system_bootable_cfg(void) {
  return "bootable=1\nloader=limine\nsource=installed-system\n";
}

static const char *installed_system_bios_bootable_cfg(void) {
  return "bootable=1\nscheme=mbr\nactive_partition=System\nloader=limine\n"
         "source=installed-system\n";
}

static const char *installed_system_installer_state(int first_boot_setup) {
  return first_boot_setup
             ? "installed=1\nprofile=system-image\nsource=installed-system\n"
               "first_boot_setup=1\n"
             : "installed=1\nprofile=system-image\nsource=installed-system\n"
               "first_boot_setup=0\n";
}

static const char *installed_system_efi_boot_cfg(void) {
  return "bootable=1\nloader=limine\nsource=installed-system\n";
}

static const char *installed_system_mbr_boot_cfg(void) {
  return "bootable=1\nscheme=mbr\nactive_partition=System\nloader=limine\n"
         "source=installed-system\n";
}

static int installer_refresh_bootloader_state(const char *target_root,
                                              int first_boot_setup) {
  char path[192];
  const char *bootable_cfg = installed_system_bootable_cfg();
  const char *bios_bootable_cfg = installed_system_bios_bootable_cfg();
  const char *installer_state =
      installed_system_installer_state(first_boot_setup);
  const char *efi_boot_cfg = installed_system_efi_boot_cfg();
  const char *mbr_boot_cfg = installed_system_mbr_boot_cfg();

  if (target_root && target_root[0]) {
    str_copy_safe(path, target_root, sizeof(path));
    installer_append_to_buf(path, sizeof(path), "/BOOTABLE.CFG");
    if (installer_write_target_text_file(path, bootable_cfg) != 0)
      return -1;

    str_copy_safe(path, target_root, sizeof(path));
    installer_append_to_buf(path, sizeof(path), "/EFI/BOOT/BOOTABLE.CFG");
    if (installer_write_target_text_file(path, bootable_cfg) != 0)
      return -1;

    str_copy_safe(path, target_root, sizeof(path));
    installer_append_to_buf(path, sizeof(path), "/boot/BOOTABLE.CFG");
    if (installer_write_target_text_file(path, bios_bootable_cfg) != 0)
      return -1;

    str_copy_safe(path, target_root, sizeof(path));
    installer_append_to_buf(path, sizeof(path), "/System/installer-state.txt");
    if (installer_write_target_text_file(path, installer_state) != 0)
      return -1;

    str_copy_safe(path, target_root, sizeof(path));
    installer_append_to_buf(path, sizeof(path), "/System/efi-boot.cfg");
    if (installer_write_target_text_file(path, efi_boot_cfg) != 0)
      return -1;

    str_copy_safe(path, target_root, sizeof(path));
    installer_append_to_buf(path, sizeof(path), "/System/mbr-boot.cfg");
    if (installer_write_target_text_file(path, mbr_boot_cfg) != 0)
      return -1;
    return 0;
  }

  if (write_text_file("/BOOTABLE.CFG", bootable_cfg) != 0)
    return -1;
  if (write_text_file("/EFI/BOOT/BOOTABLE.CFG", bootable_cfg) != 0)
    return -1;
  if (write_text_file("/boot/BOOTABLE.CFG", bios_bootable_cfg) != 0)
    return -1;
  if (write_text_file("/System/installer-state.txt", installer_state) != 0)
    return -1;
  if (write_text_file("/System/efi-boot.cfg", efi_boot_cfg) != 0)
    return -1;
  if (write_text_file("/System/mbr-boot.cfg", mbr_boot_cfg) != 0)
    return -1;
  return 0;
}

static void installer_clear_first_boot_setup_flag(void) {
  installer_refresh_bootloader_state("", 0);
}

static int installer_first_boot_setup_pending(void) {
  char manifest[192];
  char value[16];

  if (read_text_file("/System/installer-state.txt", manifest, sizeof(manifest)) <
      0)
    return 0;
  if (manifest_get_value(manifest, "first_boot_setup", value, sizeof(value)) !=
      0)
    return 0;
  return value[0] == '1';
}

static void mask_secret(const char *src, char *dst, int max) {
  int idx = 0;
  if (!dst || max <= 0)
    return;
  while (src && src[idx] && idx < max - 1) {
    dst[idx] = '*';
    idx++;
  }
  dst[idx] = '\0';
}

static void append_input_char(char *buf, int max, int key) {
  int len = 0;
  if (!buf || max <= 1)
    return;
  while (buf[len])
    len++;
  if (key == 8) {
    if (len > 0)
      buf[len - 1] = '\0';
    return;
  }
  if (key < 32 || key > 126 || len >= max - 1)
    return;
  buf[len++] = (char)key;
  buf[len] = '\0';
}

static void load_account_state(void) {
  char manifest[256];
  int storage_ready = account_partition_storage_ready;
  char username[32];
  char password[33];
  char role[16];
  int wallpaper_index = DEFAULT_WALLPAPER_INDEX;
  char partition_label[32];
  char disk_location[32];
  int parsed = -1;

  account_username[0] = '\0';
  account_password[0] = '\0';
  str_copy_safe(account_role, "admin", sizeof(account_role));
  account_wallpaper = DEFAULT_WALLPAPER_INDEX;
  account_partition_label[0] = '\0';
  account_disk_location[0] = '\0';
  account_state_persist_pending = 0;
  if (read_account_manifest(NULL, manifest, sizeof(manifest)) == 0) {
    parsed = parse_account_manifest(manifest, NULL, username, sizeof(username),
                                    password, sizeof(password), role,
                                    sizeof(role), &wallpaper_index, partition_label,
                                    sizeof(partition_label), disk_location,
                                    sizeof(disk_location));
  }

  if (parsed != 0) {
    if (!storage_ready) {
      extern int storage_get_disk_count(void);
      extern int boot_is_live_media(void);
      extern int boot_is_usb_boot(void);
      storage_ready = boot_is_live_media() || boot_is_usb_boot() ||
                      storage_get_disk_count() > 0;
    }
    if (!storage_ready)
      return;
    if (load_account_manifest_from_partition(manifest, sizeof(manifest)) != 0)
      return;
    parsed = parse_account_manifest(manifest, NULL, username, sizeof(username),
                                    password, sizeof(password), role,
                                    sizeof(role), &wallpaper_index, partition_label,
                                    sizeof(partition_label), disk_location,
                                    sizeof(disk_location));
    if (parsed != 0)
      return;
  }

  str_copy_safe(account_username, username, sizeof(account_username));
  str_copy_safe(account_password, password, sizeof(account_password));
  str_copy_safe(account_role, role, sizeof(account_role));
  account_wallpaper = clamp_wallpaper_index(wallpaper_index);
  str_copy_safe(account_partition_label, partition_label,
                sizeof(account_partition_label));
  str_copy_safe(account_disk_location, disk_location,
                sizeof(account_disk_location));

  if (account_username[0] && account_password[0] && !account_disk_location[0]) {
    load_install_target_disk_location(account_disk_location,
                                      sizeof(account_disk_location));
  }
  apply_account_wallpaper(account_wallpaper);
}

static void load_setup_state(int *setup_complete, int *apps_seeded,
                             int *state_found) {
  char manifest[192];
  char value[16];

  if (setup_complete)
    *setup_complete = 0;
  if (apps_seeded)
    *apps_seeded = 0;
  if (state_found)
    *state_found = 0;

  if (read_text_file(GUI_SETUP_STATE_PATH, manifest, sizeof(manifest)) < 0)
    return;

  if (state_found)
    *state_found = 1;

  if (setup_complete &&
      manifest_get_value(manifest, "setup_complete", value, sizeof(value)) ==
          0) {
    *setup_complete = value[0] == '1';
  }
  if (apps_seeded &&
      manifest_get_value(manifest, "apps_seeded", value, sizeof(value)) == 0) {
    *apps_seeded = value[0] == '1';
  }
}

static void save_setup_state(int setup_complete, int apps_seeded) {
  char manifest[96];
  int idx = 0;
  const char *setup_value = setup_complete ? "1" : "0";
  const char *apps_value = apps_seeded ? "1" : "0";

  for (const char *p = "setup_complete=";
       *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  manifest[idx++] = setup_value[0];
  manifest[idx++] = '\n';

  for (const char *p = "apps_seeded="; *p && idx < (int)sizeof(manifest) - 1;
       p++)
    manifest[idx++] = *p;
  manifest[idx++] = apps_value[0];
  manifest[idx++] = '\n';
  manifest[idx] = '\0';

  write_text_file(GUI_SETUP_STATE_PATH, manifest);
}


static void account_unix_append_user(char *passwd, int passwd_max,
                                     char *shadow, int shadow_max,
                                     char *admin_members, int admin_max,
                                     char *user_members, int user_max,
                                     const char *username,
                                     const char *password_hash,
                                     const char *role, int ordinal) {
  char home[96];
  uint32_t uid;
  uint32_t gid;

  if (!passwd || !shadow || !username || !username[0] || !password_hash ||
      !password_hash[0])
    return;

  uid = account_unix_uid_for_name(username, ordinal);
  gid = uid == 0 ? 0U : 100U;
  account_unix_home_path(username, home, sizeof(home));

  installer_append_to_buf(passwd, passwd_max, username);
  installer_append_to_buf(passwd, passwd_max, ":x:");
  account_append_decimal(passwd, passwd_max, uid);
  installer_append_to_buf(passwd, passwd_max, ":");
  account_append_decimal(passwd, passwd_max, gid);
  installer_append_to_buf(passwd, passwd_max, ":OS8 ");
  installer_append_to_buf(passwd, passwd_max,
                          role && role[0] ? role : "user");
  installer_append_to_buf(passwd, passwd_max, ":");
  installer_append_to_buf(passwd, passwd_max, home);
  installer_append_to_buf(passwd, passwd_max, ":/bin/sh\n");

  installer_append_to_buf(shadow, shadow_max, username);
  installer_append_to_buf(shadow, shadow_max, ":");
  installer_append_to_buf(shadow, shadow_max, password_hash);
  installer_append_to_buf(shadow, shadow_max, ":19000:0:99999:7:::\n");

  if (uid != 0) {
    if (user_members && user_max > 0) {
      if (user_members[0])
        installer_append_to_buf(user_members, user_max, ",");
      installer_append_to_buf(user_members, user_max, username);
    }
    if (role && str_cmp(role, "admin") == 0 && admin_members && admin_max > 0) {
      if (admin_members[0])
        installer_append_to_buf(admin_members, admin_max, ",");
      installer_append_to_buf(admin_members, admin_max, username);
    }
  }

  vfs_mkdir(GUI_HOME_ROOT, 0755);
  vfs_mkdir(home, 0755);
}

static int account_write_unix_databases(void) {
  settings_account_list_t list;
  char passwd[1024];
  char shadow[1024];
  char group[512];
  char admin_members[256];
  char user_members[256];
  int ordinal = 0;

  passwd[0] = '\0';
  shadow[0] = '\0';
  group[0] = '\0';
  admin_members[0] = '\0';
  user_members[0] = '\0';

  ensure_gui_app_dirs();
  vfs_mkdir(GUI_ETC_DIR, 0755);
  vfs_mkdir(GUI_HOME_ROOT, 0755);

  settings_account_list_init(&list);
  settings_collect_accounts(&list);

  if (account_username[0] && account_password[0] &&
      settings_account_list_index_of(&list, account_username) < 0) {
    settings_account_list_append(&list, account_username);
  }

  for (int i = 0; i < list.count; i++) {
    char manifest[256];
    char username[32];
    char password_hash[33];
    char role[16];
    int wallpaper = DEFAULT_WALLPAPER_INDEX;
    char partition_label[32];
    char disk_location[32];

    username[0] = '\0';
    password_hash[0] = '\0';
    str_copy_safe(role, "user", sizeof(role));
    partition_label[0] = '\0';
    disk_location[0] = '\0';

    if (account_username[0] && str_cmp(list.names[i], account_username) == 0) {
      str_copy_safe(username, account_username, sizeof(username));
      str_copy_safe(password_hash, account_password, sizeof(password_hash));
      str_copy_safe(role, account_role, sizeof(role));
    } else {
      char path[160];
      if (account_manifest_path(list.names[i], path, sizeof(path)) == 0 &&
          read_text_file(path, manifest, sizeof(manifest)) >= 0 &&
          parse_account_manifest(manifest, list.names[i], username,
                                 sizeof(username), password_hash,
                                 sizeof(password_hash), role, sizeof(role),
                                 &wallpaper, partition_label,
                                 sizeof(partition_label), disk_location,
                                 sizeof(disk_location)) == 0) {
        (void)wallpaper;
      }
    }

    if (username[0] && password_hash[0]) {
      account_unix_append_user(passwd, sizeof(passwd), shadow, sizeof(shadow),
                               admin_members, sizeof(admin_members),
                               user_members, sizeof(user_members), username,
                               password_hash, role, ordinal);
      if (str_cmp(username, "root") != 0)
        ordinal++;
    }
  }

  if (!passwd[0]) {
    settings_account_list_free(&list);
    return -1;
  }

  account_append_line(group, sizeof(group), "root:x:0:root");
  installer_append_to_buf(group, sizeof(group), "wheel:x:10:root");
  if (admin_members[0]) {
    installer_append_to_buf(group, sizeof(group), ",");
    installer_append_to_buf(group, sizeof(group), admin_members);
  }
  installer_append_to_buf(group, sizeof(group), "\nusers:x:100:");
  installer_append_to_buf(group, sizeof(group), user_members);
  installer_append_to_buf(group, sizeof(group), "\n");

  write_text_file(GUI_PASSWD_PATH, passwd);
  write_text_file(GUI_SHADOW_PATH, shadow);
  write_text_file(GUI_GROUP_PATH, group);
  settings_account_list_free(&list);
  return 0;
}

static void save_account_state(void) {
  char manifest[256];
  char per_user_path[160];
  int idx = 0;
  int partition_save_ok = 0;

  for (const char *p = "username="; *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; account_username[i] && idx < (int)sizeof(manifest) - 2; i++)
    manifest[idx++] = account_username[i];
  manifest[idx++] = '\n';
  for (const char *p = "password_hash=";
       *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; account_password[i] && idx < (int)sizeof(manifest) - 2; i++)
    manifest[idx++] = account_password[i];
  manifest[idx++] = '\n';
  for (const char *p = "role="; *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; account_role[i] && idx < (int)sizeof(manifest) - 2; i++)
    manifest[idx++] = account_role[i];
  manifest[idx++] = '\n';
  for (const char *p = "wallpaper="; *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  {
    int wallpaper_value = clamp_wallpaper_index(account_wallpaper);
    manifest[idx++] = (char)('0' + wallpaper_value);
  }
  manifest[idx++] = '\n';
  for (const char *p = "partition_label=";
       *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; account_partition_label[i] &&
                  idx < (int)sizeof(manifest) - 2;
       i++)
    manifest[idx++] = account_partition_label[i];
  manifest[idx++] = '\n';
  for (const char *p = "disk_location=";
       *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; account_disk_location[i] &&
                  idx < (int)sizeof(manifest) - 2;
       i++)
    manifest[idx++] = account_disk_location[i];
  manifest[idx++] = '\n';
  manifest[idx] = '\0';

  account_state_persist_pending = 1;
  ensure_gui_app_dirs();
  write_text_file(GUI_ACCOUNT_PATH, manifest);
  if (account_manifest_path(account_username, per_user_path,
                            sizeof(per_user_path)) == 0) {
    write_text_file(per_user_path, manifest);
  }
  account_write_unix_databases();
  if (account_partition_storage_ready)
    partition_save_ok = save_account_manifest_to_partition(manifest) == 0;
  if (partition_save_ok)
    account_state_persist_pending = 0;
}

static void gui_flush_account_state_before_power_transition(void) {
  if (account_username[0] && account_password[0])
    save_account_state();
}

static int load_install_target_disk_location(char *buf, int max) {
  char manifest[256];
  int fallback_disk = -1;
  int fixed_disk_count = 0;

  if (!buf || max <= 0)
    return -1;
  buf[0] = '\0';

  if (read_text_file("/System/install-target.cfg", manifest, sizeof(manifest)) <
      0) {
    extern int storage_get_disk_count(void);
    extern int storage_get_disk_kind(int index);
    extern int storage_get_disk_location(int index, char *buf, int max);
    int disk_count = storage_get_disk_count();

    for (int i = 0; i < disk_count; i++) {
      int kind = storage_get_disk_kind(i);
      if (kind == STORAGE_KIND_CDROM || kind == STORAGE_KIND_USB_MASS_STORAGE)
        continue;
      fallback_disk = i;
      fixed_disk_count++;
    }

    if (fixed_disk_count == 1 && fallback_disk >= 0 &&
        storage_get_disk_location(fallback_disk, buf, max) == 0 && buf[0]) {
      return 0;
    }
    return -1;
  }

  if (manifest_get_value(manifest, "disk_location", buf, max) != 0 || !buf[0])
    return -1;
  return 0;
}

static void startup_default_data_label(int ordinal, char *buf, int max) {
  if (!buf || max <= 0)
    return;
  buf[0] = '\0';
  str_copy_safe(buf, "Data", max);
  if (ordinal > 0) {
    int idx = 0;
    while (buf[idx] && idx < max - 1)
      idx++;
    if (idx < max - 1)
      buf[idx++] = ' ';
    append_decimal(buf, &idx, ordinal + 1);
  }
}

static int startup_assign_account_partition(void) {
  char disk_location[32];
  char partition_label[32];
  int disk_index;
  int data_partitions;

  extern int storage_get_disk_index_by_location(const char *location);
  extern int storage_disk_supports_partition_writes(int disk_index);
  extern int storage_count_partitions_of_kind(int disk_index,
                                              storage_partition_kind_t kind);
  extern int storage_create_partition(int disk_index,
                                      storage_partition_kind_t kind,
                                      uint32_t size_mib);
  extern int storage_get_disk_location(int index, char *buf, int max);

  if (account_partition_label[0] && account_disk_location[0])
    return 0;

  if (load_install_target_disk_location(disk_location, sizeof(disk_location)) !=
      0) {
    set_startup_status("Setup warning: install disk not recorded.");
    return -1;
  }

  disk_index = storage_get_disk_index_by_location(disk_location);
  if (disk_index < 0) {
    set_startup_status("Setup warning: target disk is unavailable.");
    return -1;
  }

  data_partitions =
      storage_count_partitions_of_kind(disk_index, STORAGE_PARTITION_DATA);
  if (data_partitions == 0) {
    if (!storage_disk_supports_partition_writes(disk_index) ||
        storage_create_partition(disk_index, STORAGE_PARTITION_DATA, 4096) !=
            0) {
      set_startup_status("Setup warning: user partition could not be created.");
      return -1;
    }
    startup_default_data_label(0, partition_label, sizeof(partition_label));
  } else {
    startup_default_data_label(data_partitions - 1, partition_label,
                               sizeof(partition_label));
  }

  str_copy_safe(account_partition_label, partition_label,
                sizeof(account_partition_label));
  storage_get_disk_location(disk_index, account_disk_location,
                            sizeof(account_disk_location));
  return 0;
}

static void seed_all_system_apps_once(void) {
  int apps_seeded = 0;
  int setup_complete = 0;
  int state_found = 0;

  load_setup_state(&setup_complete, &apps_seeded, &state_found);
  if (state_found && apps_seeded) {
    return;
  }

  load_system_app_catalog();
  dock_item_count = 0;
  dock_loaded = 1;
  dock_add_missing_preinstalled_apps();
  save_dock_config();
  save_setup_state(1, 1);
}

static void startup_open_modal_window(void) {
  int setup_active = startup_setup_account_active();
  int completion_active = startup_flow == STARTUP_FLOW_COMPLETING_INSTALL;
  int full_screen = setup_active || completion_active;
  int win_w = full_screen ? (int)primary_display.width : 520;
  int win_h = full_screen ? (int)primary_display.height : 280;
  int win_x = full_screen ? 0 : ((int)primary_display.width - win_w) / 2;
  int win_y = full_screen ? 0 : ((int)primary_display.height - win_h) / 2;
  const char *title = startup_setup_welcome_active()
                          ? "Welcome"
                          : startup_setup_storage_active()
                                ? "Prepare Storage"
                          : startup_flow == STARTUP_FLOW_SETUP_ACCOUNT
                                ? "Setup Account"
                          : completion_active
                                ? "Completing Installation"
                          : "Login";

  if (full_screen) {
    desktop_hide_context_menu();
    secure_attention_open = 0;
    startup_close_other_windows();
  }

  startup_window_opening = 1;
  startup_window = gui_create_window(title, win_x, win_y, win_w, win_h);
  startup_window_opening = 0;
  if (startup_window) {
    startup_window->has_titlebar = false;
    startup_window->resizable = false;
    gui_focus_window(startup_window);
  }
}

static void startup_begin_login_flow(const char *message, int preserve_username) {
  unmount_active_user_home();
  session_authenticated = 0;
  startup_flow = STARTUP_FLOW_LOGIN;
  startup_login_user_dropdown_open = 0;
  startup_setup_page = STARTUP_SETUP_PAGE_WELCOME;
  startup_input_password[0] = '\0';
  startup_active_field = preserve_username ? 1 : 0;
  if (!preserve_username)
    startup_input_username[0] = '\0';
  set_startup_status(message ? message : "");
  desktop_hide_context_menu();
  secure_attention_open = 0;
  startup_close_other_windows();
  startup_open_modal_window();
}

static void startup_begin_first_boot_completion(void) {
  session_authenticated = 0;
  startup_flow = STARTUP_FLOW_COMPLETING_INSTALL;
  startup_completion_started_ms = arch_timer_get_ms();
  startup_completion_deadline_ms = startup_completion_started_ms + 1800;
  set_startup_status("Completing installation from the installed disk.");
  startup_close_other_windows();
  startup_open_modal_window();
}

static void ensure_startup_flow(void) {
  int needs_account_setup = 0;
  int setup_complete = 0;
  int apps_seeded = 0;
  int setup_state_found = 0;
  int live_disk_boot = 0;
  int account_configured = 0;
  int storage_ready = 0;

  if (gui_is_installer_mode())
    return;

  {
    extern int boot_is_live_media(void);
    extern int boot_is_usb_boot(void);
    extern int storage_get_disk_count(void);
    live_disk_boot = boot_is_live_media() || boot_is_usb_boot();
    storage_ready = live_disk_boot || storage_get_disk_count() > 0;
  }

  if (startup_flow == STARTUP_FLOW_COMPLETING_INSTALL)
    return;

  if (!live_disk_boot && installer_first_boot_setup_pending()) {
    startup_begin_first_boot_completion();
    return;
  }

  load_account_state();
  load_setup_state(&setup_complete, &apps_seeded, &setup_state_found);
  account_configured = account_username[0] && account_password[0];
  startup_input_username[0] = '\0';
  startup_input_password[0] = '\0';
  startup_setup_page = STARTUP_SETUP_PAGE_WELCOME;
  startup_active_field = 0;
  set_startup_status("");
  ensure_user_storage_dirs();

  if (!setup_state_found) {
    setup_complete = account_configured;
  }

  if (live_disk_boot) {
    session_authenticated = 1;
    startup_flow = STARTUP_FLOW_NONE;
    installer_clear_first_boot_setup_flag();
    if (!apps_seeded)
      seed_all_system_apps_once();
    if (startup_window) {
      gui_destroy_window(startup_window);
      startup_window = NULL;
    }
    desktop_refresh();
    return;
  }

  if (account_configured) {
    setup_complete = 1;
    installer_clear_first_boot_setup_flag();
  }

  needs_account_setup = !account_configured || !setup_complete;
  if (!needs_account_setup) {
    if (!apps_seeded)
      seed_all_system_apps_once();
    str_copy_safe(startup_input_username, account_username,
                  sizeof(startup_input_username));
    startup_begin_login_flow("", 1);
    return;
  }

  session_authenticated = 0;
  startup_flow = STARTUP_FLOW_SETUP_ACCOUNT;
  startup_setup_page = STARTUP_SETUP_PAGE_WELCOME;
  startup_window = NULL;
  if (storage_ready)
    save_setup_state(0, 0);
  startup_open_modal_window();
}

static void complete_startup_auth(void) {
  int setup_complete = 0;
  int apps_seeded = 0;
  int state_found = 0;

  mount_active_user_home();
  session_authenticated = 1;
  startup_flow = STARTUP_FLOW_NONE;
  set_startup_status("");
  load_setup_state(&setup_complete, &apps_seeded, &state_found);
  save_setup_state(1, apps_seeded);
  installer_clear_first_boot_setup_flag();
  if (account_username[0] && account_password[0])
    save_account_state();
  if (startup_window) {
    gui_destroy_window(startup_window);
    startup_window = NULL;
  }
  seed_all_system_apps_once();
  desktop_refresh();
}

static void startup_process_first_boot_completion(void) {
  if (startup_flow != STARTUP_FLOW_COMPLETING_INSTALL)
    return;

  if (!startup_completion_deadline_ms)
    startup_completion_deadline_ms = arch_timer_get_ms() + 1800;
  if (arch_timer_get_ms() < startup_completion_deadline_ms)
    return;

  set_startup_status("Finalizing installed system configuration.");
  runtime_sync_boot_storage_to_live();
  ensure_user_storage_dirs();
  seed_all_system_apps_once();
  save_setup_state(0, 1);
  installer_clear_first_boot_setup_flag();
  startup_completion_started_ms = 0;
  startup_completion_deadline_ms = 0;
  startup_flow = STARTUP_FLOW_NONE;
  if (startup_window) {
    gui_destroy_window(startup_window);
    startup_window = NULL;
  }
  ensure_startup_flow();
}

static void submit_startup_flow(void) {
  if (startup_flow == STARTUP_FLOW_SETUP_ACCOUNT) {
    if (startup_setup_welcome_active()) {
      startup_setup_page = STARTUP_SETUP_PAGE_ACCOUNT;
      startup_active_field = 0;
      set_startup_status("Create your account to continue.");
      return;
    }

    if (startup_setup_storage_active()) {
      if (startup_assign_account_partition() == 0) {
        ensure_user_storage_dirs();
        save_account_state();
        startup_begin_login_flow("Setup complete. Sign in to finish booting.",
                                 1);
      }
      return;
    }

    if (!startup_setup_account_form_active()) {
      set_startup_status("Setup is waiting for the next screen.");
      return;
    }

    char password_hash[33];

    if (!startup_input_username[0] || !startup_input_password[0]) {
      set_startup_status("Enter both a username and password.");
      return;
    }
    str_copy_safe(account_username, startup_input_username,
                  sizeof(account_username));
    vib_password_hash_hex(account_username, startup_input_password,
                          password_hash, sizeof(password_hash));
    str_copy_safe(account_password, password_hash, sizeof(account_password));
    str_copy_safe(account_role, "admin", sizeof(account_role));
    account_wallpaper = DEFAULT_WALLPAPER_INDEX;
    apply_account_wallpaper(account_wallpaper);
    startup_setup_page = STARTUP_SETUP_PAGE_STORAGE;
    startup_active_field = 0;
    set_startup_status("Prepare a personal data partition for this account.");
    return;
  }

  if (!startup_account_system_ready()) {
    set_startup_status("Account system is still starting up. Please wait.");
    return;
  }
  if (gui_activation_requires_sign_in()) {
    set_startup_status(gui_activation_detail_text());
    return;
  }

  {
    char manifest[256];
    char login_username[32];
    char login_password_hash[33];
    char login_role[16];
    int login_wallpaper = DEFAULT_WALLPAPER_INDEX;
    char login_partition_label[32];
    char login_disk_location[32];
    char password_hash[33];

    if (account_username[0] && account_password[0] &&
        str_cmp(startup_input_username, account_username) == 0) {
      vib_password_hash_hex(account_username, startup_input_password, password_hash,
                            sizeof(password_hash));
      if (vib_secure_string_eq(password_hash, account_password)) {
        ensure_user_storage_dirs();
        complete_startup_auth();
        return;
      }
    }

    if (read_account_manifest(startup_input_username, manifest,
                              sizeof(manifest)) == 0 &&
        parse_account_manifest(manifest, startup_input_username, login_username,
                               sizeof(login_username), login_password_hash,
                               sizeof(login_password_hash), login_role,
                               sizeof(login_role), &login_wallpaper,
                               login_partition_label,
                               sizeof(login_partition_label),
                               login_disk_location,
                               sizeof(login_disk_location)) == 0) {
      vib_password_hash_hex(login_username, startup_input_password, password_hash,
                            sizeof(password_hash));
      if (vib_secure_string_eq(password_hash, login_password_hash)) {
        str_copy_safe(account_username, login_username, sizeof(account_username));
        str_copy_safe(account_password, login_password_hash,
                      sizeof(account_password));
        str_copy_safe(account_role, login_role, sizeof(account_role));
        account_wallpaper = clamp_wallpaper_index(login_wallpaper);
        str_copy_safe(account_partition_label, login_partition_label,
                      sizeof(account_partition_label));
        str_copy_safe(account_disk_location, login_disk_location,
                      sizeof(account_disk_location));
        apply_account_wallpaper(account_wallpaper);
        ensure_user_storage_dirs();
        complete_startup_auth();
        return;
      }
    }
  }

  set_startup_status("Login failed. Check your username and password.");
}

static void startup_handle_key(int key) {
  char *target;

  if (startup_setup_welcome_active()) {
    if (key == '\r' || key == '\n' || key == ' ')
      submit_startup_flow();
    return;
  }

  if (startup_setup_storage_active()) {
    if (key == '\r' || key == '\n' || key == ' ')
      submit_startup_flow();
    return;
  }

  if (startup_flow == STARTUP_FLOW_LOGIN && startup_active_field == 0) {
    settings_account_list_t accounts;
    settings_account_list_init(&accounts);
    settings_collect_accounts(&accounts);
    if (key == KEY_DOWN || key == KEY_UP) {
      int current_idx = -1;
      if (accounts.count <= 0) {
        settings_account_list_free(&accounts);
        return;
      }
      for (int i = 0; i < accounts.count; i++) {
        if (str_cmp(startup_input_username, accounts.names[i]) == 0) {
          current_idx = i;
          break;
        }
      }
      if (current_idx < 0)
        current_idx = 0;
      if (key == KEY_DOWN)
        current_idx = (current_idx + 1) % accounts.count;
      else
        current_idx = (current_idx + accounts.count - 1) % accounts.count;
      str_copy_safe(startup_input_username, accounts.names[current_idx],
                    sizeof(startup_input_username));
      startup_login_user_dropdown_open = 0;
      settings_account_list_free(&accounts);
      return;
    }
    settings_account_list_free(&accounts);
  }

  target = startup_active_field == 0 ? startup_input_username
                                     : startup_input_password;

  if (key == '\t') {
    startup_active_field = 1 - startup_active_field;
    return;
  }
  if (key == '\r' || key == '\n') {
    submit_startup_flow();
    return;
  }
  append_input_char(target, 32, key);
}

int gui_requires_login(void) { return startup_flow_active() || !session_authenticated; }

static void ensure_gui_app_dirs(void) {
  vfs_mkdir(GUI_SYSTEM_DIR, 0755);
  vfs_mkdir(GUI_ACCOUNTS_DIR, 0755);
  vfs_mkdir(GUI_SYSTEM_APPS_DIR, 0755);
  vfs_mkdir(GUI_APPS_DIR, 0755);
  vfs_mkdir("/Desktop", 0755);
  vfs_mkdir(GUI_SYSTEM_APPS_FOLDER, 0755);
}

static int system_manifest_path_by_id(const char *app_id, char *path, int max) {
  int idx = 0;

  if (!app_id || !path || max < 32)
    return -1;

  str_copy_safe(path, GUI_SYSTEM_APPS_DIR, max);
  while (path[idx])
    idx++;
  if (idx >= max - 1)
    return -1;
  path[idx++] = '/';
  for (int i = 0; app_id[i] && idx < max - 5; i++) {
    path[idx++] = app_id[i];
  }
  path[idx++] = '.';
  path[idx++] = 'a';
  path[idx++] = 'p';
  path[idx++] = 'p';
  path[idx] = '\0';
  return 0;
}

static void write_system_app_seed(const system_app_seed_t *seed) {
  char path[128];
  char manifest[256];
  int idx = 0;
  const char *kind_str;

  if (!seed)
    return;
  if (system_manifest_path_by_id(seed->id, path, sizeof(path)) != 0)
    return;
  {
    struct file *existing = vfs_open(path, O_RDONLY, 0);
    if (existing) {
      vfs_close(existing);
      return;
    }
  }

  kind_str = kind_to_string(seed->kind);
  idx = 0;

  str_copy_safe(manifest, "id=", sizeof(manifest));
  idx = 3;
  for (int i = 0; seed->id[i] && idx < (int)sizeof(manifest) - 2; i++)
    manifest[idx++] = seed->id[i];
  manifest[idx++] = '\n';

  for (const char *p = "label="; *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; seed->label[i] && idx < (int)sizeof(manifest) - 2; i++)
    manifest[idx++] = seed->label[i];
  manifest[idx++] = '\n';

  for (const char *p = "shortcut="; *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; seed->shortcut_name[i] && idx < (int)sizeof(manifest) - 2;
       i++)
    manifest[idx++] = seed->shortcut_name[i];
  manifest[idx++] = '\n';

  for (const char *p = "kind="; *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; kind_str[i] && idx < (int)sizeof(manifest) - 2; i++)
    manifest[idx++] = kind_str[i];
  manifest[idx++] = '\n';

  for (const char *p = "default_dock=";
       *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  manifest[idx++] = seed->default_dock ? '1' : '0';
  manifest[idx++] = '\n';

  for (const char *p = "store="; *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  manifest[idx++] = seed->visible_in_store ? '1' : '0';
  manifest[idx++] = '\n';
  manifest[idx] = '\0';

  write_text_file(path, manifest);
}

static int load_system_app_from_manifest(const char *path) {
  char manifest[256];
  char id[32];
  char label[48];
  char shortcut_name[48];
  char kind_buf[32];
  char dock_buf[8];
  char store_buf[8];

  if (app_catalog_count >= MAX_SYSTEM_APPS)
    return -1;
  if (read_text_file(path, manifest, sizeof(manifest)) < 0)
    return -1;
  if (manifest_get_value(manifest, "id", id, sizeof(id)) != 0 || !id[0])
    return -1;
  for (int i = 0; i < app_catalog_count; i++) {
    if (str_cmp(app_catalog[i].id, id) == 0)
      return 0;
  }

  if (manifest_get_value(manifest, "label", label, sizeof(label)) != 0)
    str_copy_safe(label, id, sizeof(label));
  if (manifest_get_value(manifest, "shortcut", shortcut_name,
                         sizeof(shortcut_name)) != 0) {
    str_copy_safe(shortcut_name, label, sizeof(shortcut_name));
  }
  if (manifest_get_value(manifest, "kind", kind_buf, sizeof(kind_buf)) != 0)
    str_copy_safe(kind_buf, "terminal", sizeof(kind_buf));
  if (manifest_get_value(manifest, "default_dock", dock_buf, sizeof(dock_buf)) !=
      0)
    str_copy_safe(dock_buf, "0", sizeof(dock_buf));
  if (manifest_get_value(manifest, "store", store_buf, sizeof(store_buf)) != 0)
    str_copy_safe(store_buf, "1", sizeof(store_buf));

  fill_runtime_app(&app_catalog[app_catalog_count++], id, label, shortcut_name,
                   kind_from_string(kind_buf), dock_buf[0] == '1',
                   store_buf[0] == '1');
  return 0;
}

static int system_app_dir_scan(void *ctx, const char *name, int len,
                               loff_t offset, ino_t ino, unsigned type) {
  char path[128];
  int idx = 0;

  (void)ctx;
  (void)offset;
  (void)ino;
  (void)type;

  if ((len == 1 && name[0] == '.') ||
      (len == 2 && name[0] == '.' && name[1] == '.')) {
    return 0;
  }
  if (!str_ends_with_ci(name, ".app"))
    return 0;

  str_copy_safe(path, GUI_SYSTEM_APPS_DIR, sizeof(path));
  while (path[idx])
    idx++;
  path[idx++] = '/';
  for (int i = 0; i < len && idx < (int)sizeof(path) - 1; i++)
    path[idx++] = name[i];
  path[idx] = '\0';

  load_system_app_from_manifest(path);
  return 0;
}

static void load_system_app_catalog(void) {
  struct file *dir;

  if (app_catalog_loaded)
    return;

  app_catalog_loaded = 1;
  app_catalog_count = 0;
  ensure_gui_app_dirs();

  for (int i = 0; i < APP_CATALOG_SEED_COUNT; i++) {
    write_system_app_seed(&app_catalog_seed[i]);
    if (app_catalog_count < MAX_SYSTEM_APPS) {
      fill_runtime_app(&app_catalog[app_catalog_count++], app_catalog_seed[i].id,
                       app_catalog_seed[i].label,
                       app_catalog_seed[i].shortcut_name,
                       app_catalog_seed[i].kind,
                       app_catalog_seed[i].default_dock,
                       app_catalog_seed[i].visible_in_store);
    }
  }

  dir = vfs_open(GUI_SYSTEM_APPS_DIR, O_RDONLY, 0);
  if (dir) {
    vfs_readdir(dir, NULL, system_app_dir_scan);
    vfs_close(dir);
  }
}

static void ensure_app_manifest(const dock_app_def_t *app) {
  char manifest_path[128];
  char shortcut_path[128];
  char folder_shortcut_path[160];
  char manifest[128] = "id=";
  int idx = 3;

  if (!app)
    return;
  ensure_gui_app_dirs();

  for (int i = 0; app->id[i] && idx < (int)sizeof(manifest) - 2; i++) {
    manifest[idx++] = app->id[i];
  }
  manifest[idx++] = '\n';
  manifest[idx] = '\0';

  if (app_manifest_path(app, manifest_path, sizeof(manifest_path)) == 0) {
    write_text_file(manifest_path, manifest);
  }

  if (build_app_shortcut_path("/Desktop", app->shortcut_name, shortcut_path,
                              sizeof(shortcut_path)) == 0) {
    vfs_unlink(shortcut_path);
  }
  if (build_app_shortcut_path(GUI_SYSTEM_APPS_FOLDER, app->shortcut_name,
                              folder_shortcut_path,
                              sizeof(folder_shortcut_path)) == 0) {
    write_text_file(folder_shortcut_path, manifest);
  }
}

static void dock_add_item(const dock_app_def_t *app) {
  if (!app || dock_item_count >= MAX_DOCK_ITEMS)
    return;
  for (int i = 0; i < dock_item_count; i++) {
    if (dock_items[i] == app)
      return;
  }
  dock_items[dock_item_count++] = app;
}

static void dock_add_all_system_apps(void) {
  for (int i = 0; i < app_catalog_count; i++) {
    ensure_app_manifest(&app_catalog[i]);
    dock_add_item(&app_catalog[i]);
  }
}

static void dock_add_missing_preinstalled_apps(void) {
  for (int i = 0; i < app_catalog_count; i++) {
    const dock_app_def_t *app = &app_catalog[i];
    if (!app->default_dock)
      continue;
    ensure_app_manifest(app);
    dock_add_item(app);
  }
}

static void save_dock_config(void) {
  char buf[512];
  int idx = 0;
  ensure_gui_app_dirs();
  for (int i = 0; i < dock_item_count && idx < (int)sizeof(buf) - 2; i++) {
    const char *id = dock_items[i]->id;
    for (int j = 0; id[j] && idx < (int)sizeof(buf) - 2; j++) {
      buf[idx++] = id[j];
    }
    buf[idx++] = '\n';
  }
  buf[idx] = '\0';
  write_text_file(GUI_DOCK_CONFIG_PATH, buf);
}

static void load_dock_config(void) {
  char buf[512];
  int bytes = -1;

  if (dock_loaded)
    return;

  load_system_app_catalog();
  dock_loaded = 1;
  dock_item_count = 0;
  ensure_gui_app_dirs();

  for (int i = 0; i < app_catalog_count; i++) {
    ensure_app_manifest(&app_catalog[i]);
  }

  bytes = read_text_file(GUI_DOCK_CONFIG_PATH, buf, sizeof(buf));

  if (bytes <= 0) {
    dock_add_missing_preinstalled_apps();
    save_dock_config();
    return;
  }

  buf[bytes] = '\0';
  int start = 0;
  for (int i = 0;; i++) {
    if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\0') {
      if (i > start) {
        char id[32];
        int len = i - start;
        if (len >= (int)sizeof(id))
          len = sizeof(id) - 1;
        for (int j = 0; j < len; j++) {
          id[j] = buf[start + j];
        }
        id[len] = '\0';
        dock_add_item(find_catalog_app(id));
      }
      if (buf[i] == '\0')
        break;
      start = i + 1;
    }
  }

  if (dock_item_count == 0) {
    dock_add_missing_preinstalled_apps();
    save_dock_config();
  }
}

static int install_app(const dock_app_def_t *app, int pin_to_dock) {
  if (!app)
    return -1;
  ensure_app_manifest(app);
  if (pin_to_dock) {
    load_dock_config();
    dock_add_item(app);
    save_dock_config();
  }
  desktop_refresh();
  return 0;
}

int gui_launch_app_by_id(const char *app_id) {
  static int spawn_x = 100;
  static int spawn_y = 80;
  const dock_app_def_t *app = find_catalog_app(app_id);

  if (startup_flow_active())
    return -1;
  if (!app)
    return -1;

  switch (app->kind) {
  case GUI_APP_TERMINAL: {
    struct window *win =
        gui_create_window("Terminal", spawn_x, spawn_y, 450, 320);
    int content_x = 0;
    int content_y = 0;
    gui_get_window_content_rect(win, &content_x, &content_y, NULL, NULL);
    struct terminal *term = term_create(content_x, content_y, 55, 16);
    if (win && term) {
      win->userdata = term;
      term_set_active(term);
      term_set_content_pos(term, content_x, content_y);
    }
    break;
  }
  case GUI_APP_FILES:
    gui_create_file_manager(spawn_x + 30, spawn_y + 20);
    break;
  case GUI_APP_CALCULATOR:
    gui_create_window("Calculator", spawn_x + 60, spawn_y + 40, 260, 380);
    break;
  case GUI_APP_NOTES:
    gui_open_notepad(NULL);
    break;
  case GUI_APP_SETTINGS:
    gui_create_window("Settings", spawn_x + 20, spawn_y + 30, 560, 420);
    break;
  case GUI_APP_CLOCK:
    gui_create_window("Clock", spawn_x + 50, spawn_y + 40, 260, 200);
    break;
  case GUI_APP_SNAKE:
    bowling_init();
    gui_create_window("Bowling", spawn_x + 70, spawn_y + 50, 420, 320);
    break;
  case GUI_APP_HELP:
    gui_create_window("Help", spawn_x + 120, spawn_y + 80, 350, 280);
    break;
  case GUI_APP_BROWSER:
    gui_create_window("Browser", spawn_x + 150, spawn_y + 90, 600, 450);
    break;
  case GUI_APP_APPSTORE:
    gui_create_window("App Store", spawn_x + 40, spawn_y + 30, 520, 390);
    break;
  case GUI_APP_DISK_IMAGER:
    gui_create_window("Disk Imager", spawn_x + 50, spawn_y + 40, 620, 440);
    break;
  }

  spawn_x = (spawn_x + 40) % 250 + 80;
  spawn_y = (spawn_y + 30) % 150 + 60;
  return 0;
}

static int gui_focus_or_launch_app_by_id(const char *app_id) {
  const dock_app_def_t *app = find_catalog_app(app_id);
  struct window *existing;

  if (!app)
    return -1;

  existing = find_window_for_app_kind(app->kind);
  if (existing) {
    existing->visible = true;
    if (existing->state == WINDOW_MINIMIZED)
      existing->state = WINDOW_NORMAL;
    gui_focus_window(existing);
    return 0;
  }

  return gui_launch_app_by_id(app_id);
}

static void draw_app_store(int content_x, int content_y, int content_w,
                           int content_h) {
  const gui_theme_palette_t *theme = gui_theme_palette();
  load_system_app_catalog();
  int y = content_y + 12;
  (void)content_h;

  gui_draw_rect(content_x, content_y, content_w, content_h, theme->app_bg);
  gui_draw_string(content_x + 12, y, "App Store", theme->app_fg, THEME_BG);
  y += 18;
  gui_draw_string(content_x + 12, y,
                  "Install apps to create shortcuts and pin them to the dock.",
                  theme->app_muted, THEME_BG);
  y += 24;

  for (int i = 0; i < app_catalog_count; i++) {
    const dock_app_def_t *app = &app_catalog[i];
    if (!app->visible_in_store)
      continue;

    uint32_t row_bg = theme->card;
    int installed = app_is_installed(app);
    int button_w = installed ? 72 : 88;
    int button_x = content_x + content_w - button_w - 18;
    int row_w = content_w - 24;

    gui_draw_rect(content_x + 12, y, row_w, APP_STORE_CARD_HEIGHT, row_bg);
    gui_draw_rect(content_x + 24, y + 10, 34, 34, app->icon_color);

    draw_system_app_icon_kind(app->kind, content_x + 29, y + 15, 24);

    gui_draw_string(content_x + 70, y + 11, app->label, theme->app_fg, row_bg);
    gui_draw_string(content_x + 70, y + 29,
                    installed ? "Installed" : "Available to install",
                    installed ? 0xA6E3A1 : theme->app_muted, row_bg);

    gui_draw_system_button(button_x, y + 13, button_w, 28,
                           installed ? "Open" : "Install",
                           installed ? GUI_BUTTON_PRIMARY
                                     : GUI_BUTTON_SUCCESS,
                           1, 0);

    y += APP_STORE_CARD_HEIGHT + 8;
  }
}

static void installer_set_status(const char *message) {
  str_copy_safe(installer_status, message, sizeof(installer_status));
}

static void installer_set_progress_state(int done, const char *stage,
                                         const char *status,
                                         const char *detail) {
  if (done < 0)
    done = 0;
  if (done > installer_progress_total)
    done = installer_progress_total;
  installer_progress_done = done;
  if (stage)
    str_copy_safe(installer_progress_stage, stage,
                  sizeof(installer_progress_stage));
  if (status)
    installer_set_status(status);
  if (detail)
    str_copy_safe(installer_progress_detail, detail,
                  sizeof(installer_progress_detail));
}

static const char *installer_path_basename(const char *path) {
  const char *base = path;

  if (!path)
    return "";
  for (const char *p = path; *p; p++) {
    if (*p == '/')
      base = p + 1;
  }
  return base;
}

static void installer_note_copy_progress(const char *path, int success) {
  char detail[160];
  int idx = 0;
  int percent = 20;

  installer_progress_processed_files++;
  if (installer_progress_total_files > 0)
    percent += (installer_progress_processed_files * 70) /
               installer_progress_total_files;
  if (percent > 90)
    percent = 90;

  str_copy_safe(detail, success ? "Copied " : "Skipped ", sizeof(detail));
  installer_append_to_buf(detail, sizeof(detail), installer_path_basename(path));
  installer_append_to_buf(detail, sizeof(detail), "  ");
  while (detail[idx] && idx < (int)sizeof(detail) - 1)
    idx++;
  append_decimal(detail, &idx, installer_progress_processed_files);
  installer_append_to_buf(detail, sizeof(detail), "/");
  idx = 0;
  while (detail[idx] && idx < (int)sizeof(detail) - 1)
    idx++;
  append_decimal(detail, &idx, installer_progress_total_files);
  installer_append_to_buf(detail, sizeof(detail), " files processed");

  str_copy_safe(installer_progress_current_item, path,
                sizeof(installer_progress_current_item));
  installer_set_progress_state(
      percent, "Copying Files",
      success ? "Copying system files..." :
                "Copying system files with warnings...",
      detail);
}

typedef struct {
  const char *src_root;
  char dst_root[256];
  int copied_files;
  int failed_files;
} installer_copy_ctx_t;

typedef struct {
  int files;
} installer_count_ctx_t;

static char installer_log_buffer[4096];
static char installer_log_target_root[256];
static int installer_log_depth = 0;

static void installer_append_to_buf(char *buf, int max, const char *text) {
  if (!buf || max <= 0 || !text)
    return;
  (void)strlcat(buf, text, (size_t)max);
}

static int installer_selected_disk_index(void) {
  if ((installer_active || installer_show_restart_screen) &&
      installer_target_disk_index >= 0)
    return installer_target_disk_index;
  installer_refresh_disk_inventory();
  if (installer_disk_count == 0)
    return -1;
  if (installer_selected_disk < 0 || installer_selected_disk >= installer_disk_count)
    return -1;
  return installer_disk_indices[installer_selected_disk];
}

static void installer_log_clear(void) {
  installer_log_buffer[0] = '\0';
  installer_log_target_root[0] = '\0';
}

static void installer_log_append_path(const char *path, const char *line) {
  struct file *f;
  int len = 0;

  if (!path || !line || !path[0])
    return;

  while (line[len])
    len++;

  installer_ensure_parent_dirs(path);
  f = vfs_open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (!f)
    return;

  if (len > 0)
    vfs_write(f, line, (size_t)len);
  vfs_write(f, "\n", 1);
  vfs_close(f);
}

static void installer_log_send_to_host(const char *line) {
  if (!line)
    return;
  uart_puts("[INSTALL] ");
  uart_puts(line);
  uart_puts("\n");
}

static void installer_log(const char *line) {
  int idx = 0;

  if (!line)
    return;

  printk(KERN_INFO "INSTALL: %s\n", line);
  installer_log_send_to_host(line);
  while (installer_log_buffer[idx] && idx < (int)sizeof(installer_log_buffer) - 1)
    idx++;
  for (int i = 0; line[i] && idx < (int)sizeof(installer_log_buffer) - 2; i++)
    installer_log_buffer[idx++] = line[i];
  if (idx < (int)sizeof(installer_log_buffer) - 1)
    installer_log_buffer[idx++] = '\n';
  installer_log_buffer[idx] = '\0';

  if (installer_log_depth > 0)
    return;

  installer_log_depth++;
  installer_log_append_path("/System/install.log", line);
  if (installer_log_target_root[0]) {
    char target_log[320];
    str_copy_safe(target_log, installer_log_target_root, sizeof(target_log));
    installer_append_to_buf(target_log, sizeof(target_log), "/install.log");
    installer_log_append_path(target_log, line);
  }
  installer_log_depth--;
}

static void installer_normalize_path(const char *src, char *dst, int max) {
  int idx = 0;

  if (!dst || max <= 0)
    return;
  if (!src) {
    dst[0] = '\0';
    return;
  }

  while (src[idx] && idx < max - 1) {
    dst[idx] = src[idx];
    idx++;
  }
  dst[idx] = '\0';

  while (idx > 1 && dst[idx - 1] == '/') {
    dst[idx - 1] = '\0';
    idx--;
  }
}

static int installer_try_make_dir(const char *path) {
  struct file *existing;
  char normalized[256];
  int ret;

  if (!path || !path[0])
    return 0;
  installer_normalize_path(path, normalized, sizeof(normalized));
  if (!normalized[0])
    return 0;
  existing = vfs_open(normalized, O_RDONLY, 0);
  if (existing) {
    vfs_close(existing);
    return 0;
  }
  installer_ensure_parent_dirs(normalized);
  ret = vfs_mkdir(normalized, 0755);
  if (ret < 0) {
    existing = vfs_open(normalized, O_RDONLY, 0);
    if (existing) {
      vfs_close(existing);
      return 0;
    }
    char msg[320];
    str_copy_safe(msg, "mkdir failed: ", sizeof(msg));
    installer_append_to_buf(msg, sizeof(msg), normalized);
    installer_log(msg);
  }
  return ret;
}

static void installer_selected_disk_id(char *buf, int max) {
  const char *label = installer_selected_disk_label();
  int idx = 0;
  int inside = 0;

  if (!buf || max <= 1) {
    return;
  }

  buf[0] = '\0';
  for (int i = 0; label[i] && idx < max - 1; i++) {
    if (label[i] == '[') {
      inside = 1;
      continue;
    }
    if (label[i] == ']')
      break;
    if (inside)
      buf[idx++] = label[i];
  }
  buf[idx] = '\0';

  if (idx == 0) {
    str_copy_safe(buf, "disk0", max);
  }
}

static void installer_target_root_path(char *buf, int max) {
  char disk_id[32];
  installer_selected_disk_id(disk_id, sizeof(disk_id));
  if (mounted_disk_root_path_for_location(disk_id, buf, max) != 0)
    buf[0] = '\0';
}

static void installer_partition_root_path(char *buf, int max,
                                          const char *partition_name) {
  installer_target_root_path(buf, max);
  if (!partition_name || !partition_name[0])
    return;
  installer_append_to_buf(buf, max, "/");
  installer_append_to_buf(buf, max, partition_name);
}

static void installer_ensure_parent_dirs(const char *path) {
  char partial[256];
  int idx = 0;
  int last_non_slash = 0;

  if (!path)
    return;

  while (path[last_non_slash])
    last_non_slash++;
  while (last_non_slash > 1 && path[last_non_slash - 1] == '/')
    last_non_slash--;

  for (int i = 0; path[i] && idx < (int)sizeof(partial) - 1; i++) {
    partial[idx++] = path[i];
    partial[idx] = '\0';
    if (i > 0 && path[i] == '/' && i < last_non_slash - 1)
      installer_try_make_dir(partial);
  }
}

static int installer_build_payload_fallback(const char *src_path, char *alt,
                                            int max) {
  static const char *install_prefix = "/install/system-image";
  static const char *setup_prefix = "/setup/install/system-image";
  int idx = 0;
  const char *replacement = NULL;
  int prefix_len = 0;

  if (!src_path || !alt || max <= 0)
    return -1;

  if (src_path[0] == '/' && src_path[1] == 'i' && src_path[2] == 'n' &&
      src_path[3] == 's' && src_path[4] == 't' && src_path[5] == 'a' &&
      src_path[6] == 'l' && src_path[7] == 'l' && src_path[8] == '/' &&
      src_path[9] == 's' && src_path[10] == 'y' && src_path[11] == 's' &&
      src_path[12] == 't' && src_path[13] == 'e' && src_path[14] == 'm' &&
      src_path[15] == '-' && src_path[16] == 'i' && src_path[17] == 'm' &&
      src_path[18] == 'a' && src_path[19] == 'g' && src_path[20] == 'e' &&
      (src_path[21] == '\0' || src_path[21] == '/')) {
    replacement = setup_prefix;
    prefix_len = 21;
  } else if (src_path[0] == '/' && src_path[1] == 's' && src_path[2] == 'e' &&
             src_path[3] == 't' && src_path[4] == 'u' && src_path[5] == 'p' &&
             src_path[6] == '/' && src_path[7] == 'i' && src_path[8] == 'n' &&
             src_path[9] == 's' && src_path[10] == 't' && src_path[11] == 'a' &&
             src_path[12] == 'l' && src_path[13] == 'l' && src_path[14] == '/' &&
             src_path[15] == 's' && src_path[16] == 'y' && src_path[17] == 's' &&
             src_path[18] == 't' && src_path[19] == 'e' && src_path[20] == 'm' &&
             src_path[21] == '-' && src_path[22] == 'i' && src_path[23] == 'm' &&
             src_path[24] == 'a' && src_path[25] == 'g' && src_path[26] == 'e' &&
             (src_path[27] == '\0' || src_path[27] == '/')) {
    replacement = install_prefix;
    prefix_len = 27;
  } else {
    return -1;
  }

  for (int i = 0; replacement[i] && idx < max - 1; i++)
    alt[idx++] = replacement[i];
  for (int i = prefix_len; src_path[i] && idx < max - 1; i++)
    alt[idx++] = src_path[i];
  alt[idx] = '\0';
  return 0;
}

static int installer_build_boot_fallback(const char *src_path, char *alt,
                                         int max) {
  static const char *setup_boot_prefix = "/setup/boot/";
  const char *boot_tail = NULL;
  int idx = 0;

  if (!src_path || !alt || max <= 0)
    return -1;

  boot_tail = src_path;
  while (*boot_tail) {
    if (boot_tail[0] == '/' && boot_tail[1] == 'b' && boot_tail[2] == 'o' &&
        boot_tail[3] == 'o' && boot_tail[4] == 't' && boot_tail[5] == '/') {
      boot_tail += 6;
      break;
    }
    boot_tail++;
  }

  if (!boot_tail || !boot_tail[0])
    return -1;

  if (src_path[0] == '/' && src_path[1] == 's' && src_path[2] == 'e' &&
      src_path[3] == 't' && src_path[4] == 'u' && src_path[5] == 'p' &&
      src_path[6] == '/' && src_path[7] == 'b' && src_path[8] == 'o' &&
      src_path[9] == 'o' && src_path[10] == 't' && src_path[11] == '/') {
    return -1;
  }

  for (int i = 0; setup_boot_prefix[i] && idx < max - 1; i++)
    alt[idx++] = setup_boot_prefix[i];
  for (int i = 0; boot_tail[i] && idx < max - 1; i++)
    alt[idx++] = boot_tail[i];
  alt[idx] = '\0';
  return 0;
}

static int installer_str_equal(const char *a, const char *b) {
  int i = 0;

  if (!a || !b)
    return 0;
  while (a[i] && b[i]) {
    if (a[i] != b[i])
      return 0;
    i++;
  }
  return a[i] == '\0' && b[i] == '\0';
}

static int installer_copy_file(const char *src_path, const char *dst_path) {
  uint8_t *data = NULL;
  size_t size = 0;
  char alt_src[256];
  char boot_alt_src[256];
  const char *used_src = NULL;
  char msg[320];
  int ret;

  alt_src[0] = '\0';
  boot_alt_src[0] = '\0';
  if (media_load_file(src_path, &data, &size) != 0) {
    if ((installer_build_payload_fallback(src_path, alt_src,
                                          sizeof(alt_src)) == 0 &&
         media_load_file(alt_src, &data, &size) == 0 &&
         (used_src = alt_src, 1)) ||
        (installer_build_boot_fallback(src_path, boot_alt_src,
                                       sizeof(boot_alt_src)) == 0 &&
         media_load_file(boot_alt_src, &data, &size) == 0 &&
         (used_src = boot_alt_src, 1))) {
      str_copy_safe(msg, "read fallback: ", sizeof(msg));
      installer_append_to_buf(msg, sizeof(msg), used_src ? used_src : src_path);
      installer_append_to_buf(msg, sizeof(msg), " (from ");
      installer_append_to_buf(msg, sizeof(msg), src_path);
      installer_append_to_buf(msg, sizeof(msg), ")");
      installer_log(msg);
    } else {
      str_copy_safe(msg, "read failed: ", sizeof(msg));
      installer_append_to_buf(msg, sizeof(msg), src_path);
      installer_log(msg);
      return -1;
    }
  }

  installer_ensure_parent_dirs(dst_path);
  ret = installer_write_target_file(dst_path, data, size);
  if (ret != 0 && size == 0) {
    struct file *dst = vfs_open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst) {
      vfs_close(dst);
      ret = 0;
    }
  }
  if (ret != 0) {
    str_copy_safe(msg, "write failed: ", sizeof(msg));
    installer_append_to_buf(msg, sizeof(msg), dst_path);
    installer_log(msg);
    media_free_file(data);
    return -1;
  }

  str_copy_safe(msg, "copied ", sizeof(msg));
  installer_append_to_buf(msg, sizeof(msg), src_path);
  installer_append_to_buf(msg, sizeof(msg), " -> ");
  installer_append_to_buf(msg, sizeof(msg), dst_path);
  installer_log(msg);
  media_free_file(data);
  return 0;
}

static int installer_copy_tree_callback(void *ctx, const char *name, int len,
                                        loff_t offset, ino_t ino,
                                        unsigned type) {
  installer_copy_ctx_t *copy = (installer_copy_ctx_t *)ctx;
  char src_path[256];
  char dst_path[256];
  int copy_result;
  int src_len = 0;
  int dst_len = 0;

  (void)offset;
  (void)ino;

  if (!copy || !name || len <= 0)
    return 0;
  if ((len == 1 && name[0] == '.') ||
      (len == 2 && name[0] == '.' && name[1] == '.'))
    return 0;
  if (len == 14 && name[0] == 'I' && name[1] == 'M' && name[2] == 'A' &&
      name[3] == 'G' && name[4] == 'E' && name[5] == '_' &&
      name[6] == 'I' && name[7] == 'N' && name[8] == 'F' &&
      name[9] == 'O' && name[10] == '.' && name[11] == 't' &&
      name[12] == 'x' && name[13] == 't')
    return 0;

  str_copy_safe(src_path, copy->src_root, sizeof(src_path));
  while (src_path[src_len])
    src_len++;
  if (src_len < (int)sizeof(src_path) - 1)
    src_path[src_len++] = '/';
  for (int i = 0; i < len && src_len < (int)sizeof(src_path) - 1; i++)
    src_path[src_len++] = name[i];
  src_path[src_len] = '\0';

  str_copy_safe(dst_path, copy->dst_root, sizeof(dst_path));
  while (dst_path[dst_len])
    dst_len++;
  if (dst_len < (int)sizeof(dst_path) - 1)
    dst_path[dst_len++] = '/';
  for (int i = 0; i < len && dst_len < (int)sizeof(dst_path) - 1; i++)
    dst_path[dst_len++] = name[i];
  dst_path[dst_len] = '\0';

  if (type == 4) {
    if (installer_try_make_dir(dst_path) != 0)
      copy->failed_files++;
    {
      struct file *dir = vfs_open(src_path, O_RDONLY, 0);
      if (dir) {
        installer_copy_ctx_t next = {src_path, "", copy->copied_files,
                                     copy->failed_files};
        str_copy_safe(next.dst_root, dst_path, sizeof(next.dst_root));
        vfs_readdir(dir, &next, installer_copy_tree_callback);
        copy->copied_files = next.copied_files;
        copy->failed_files = next.failed_files;
        vfs_close(dir);
      } else {
        char msg[320];
        str_copy_safe(msg, "open dir failed: ", sizeof(msg));
        installer_append_to_buf(msg, sizeof(msg), src_path);
        installer_log(msg);
        copy->failed_files++;
      }
    }
    return 0;
  }

  copy_result = installer_copy_file(src_path, dst_path);
  if (copy_result == 0)
    copy->copied_files++;
  else
    copy->failed_files++;
  installer_note_copy_progress(dst_path, copy_result == 0);
  return 0;
}

static const char *installer_system_image_root_path(void);
static const char *installer_boot_payload_root_path(void);
static int installer_system_image_is_archive(const char *path);
static int installer_boot_payload_is_image(const char *path);
static int installer_payload_is_packaged(const char *path);
static int installer_copy_tree_to_root(const char *src_root, const char *dst_root,
                                       int *copied_files, int *failed_files,
                                       const char *log_label);
static int installer_copy_boot_payload_to_root(const char *target_root,
                                               int *copied_files,
                                               int *failed_files);
static int installer_copy_boot_aliases(const char *target_root, int *copied_files,
                                       int *failed_files);
static int installer_payload_file_exists(const char *path);
static const char *installer_system_disk_image_path(void);
static int installer_apply_system_disk_image(int disk_index);

static int installer_copy_system_image_to_root(const char *target_root,
                                               int *copied_files,
                                               int *failed_files) {
  const char *installer_system_image_root = installer_system_image_root_path();
  if (installer_system_image_is_archive(installer_system_image_root)) {
    if (media_zip_extract_file_to_root(installer_system_image_root, target_root,
                                       copied_files, failed_files) != 0)
      return -1;
  } else if (installer_copy_tree_to_root(installer_system_image_root,
                                         target_root, copied_files,
                                         failed_files, "system image") != 0) {
    return -1;
  }
  return 0;
}

static int installer_payload_file_exists(const char *path) {
  struct file *f;

  if (!path || !path[0])
    return 0;
  f = vfs_open(path, O_RDONLY, 0);
  if (!f)
    return 0;
  vfs_close(f);
  return 1;
}

static int installer_system_image_is_archive(const char *path) {
  size_t len = 0;

  if (!path)
    return 0;
  while (path[len])
    len++;
  return (len >= 4 && path[len - 4] == '.' && path[len - 3] == 'z' &&
          path[len - 2] == 'i' && path[len - 1] == 'p');
}

static int installer_boot_payload_is_image(const char *path) {
  size_t len = 0;

  if (!path)
    return 0;
  while (path[len])
    len++;
  return (len >= 4 && path[len - 4] == '.' && path[len - 3] == 'i' &&
          path[len - 2] == 'm' && path[len - 1] == 'g');
}

static int installer_payload_is_packaged(const char *path) {
  return installer_system_image_is_archive(path) ||
         installer_boot_payload_is_image(path);
}

static int installer_payload_any_file_exists(const char **paths, int count) {
  if (!paths || count <= 0)
    return 0;

  for (int i = 0; i < count; i++) {
    if (installer_payload_file_exists(paths[i]))
      return 1;
  }
  return 0;
}

static int installer_count_tree_callback(void *ctx, const char *name, int len,
                                         loff_t offset, ino_t ino,
                                         unsigned type) {
  installer_count_ctx_t *count = (installer_count_ctx_t *)ctx;

  (void)offset;
  (void)ino;

  if (!count || !name || len <= 0)
    return 0;
  if ((len == 1 && name[0] == '.') ||
      (len == 2 && name[0] == '.' && name[1] == '.'))
    return 0;
  if (type == 4)
    return 0;
  if (len == 14 && name[0] == 'I' && name[1] == 'M' && name[2] == 'A' &&
      name[3] == 'G' && name[4] == 'E' && name[5] == '_' &&
      name[6] == 'I' && name[7] == 'N' && name[8] == 'F' &&
      name[9] == 'O' && name[10] == '.' && name[11] == 't' &&
      name[12] == 'x' && name[13] == 't')
    return 0;
  count->files++;
  return 0;
}

static int installer_count_tree_files(const char *src_root) {
  struct file *dir;
  installer_count_ctx_t ctx = {0};

  if (!src_root || !src_root[0])
    return 0;
  if (installer_boot_payload_is_image(src_root)) {
    int count = media_boot_image_count_file_entries(src_root);
    return (count > 0) ? count : 0;
  }
  if (installer_system_image_is_archive(src_root)) {
    int count = media_zip_count_file_entries(src_root);
    return (count > 0) ? count : 0;
  }
  dir = vfs_open(src_root, O_RDONLY, 0);
  if (!dir)
    return 0;
  vfs_readdir(dir, &ctx, installer_count_tree_callback);
  vfs_close(dir);
  return ctx.files;
}

static int installer_boot_alias_copy_count(const char *target_root) {
  char boot_bios_path[192];

  if (!target_root || !target_root[0])
    return 0;
  str_copy_safe(boot_bios_path, target_root, sizeof(boot_bios_path));
  installer_append_to_buf(boot_bios_path, sizeof(boot_bios_path),
                          "/boot/limine-bios.sys");
  return installer_payload_file_exists(boot_bios_path) ? 3 : 0;
}

static const char *installer_system_disk_image_path(void) {
  if (installer_payload_file_exists("/install/system.img"))
    return "/install/system.img";
  if (installer_payload_file_exists("/setup/install/system.img"))
    return "/setup/install/system.img";
  if (installer_payload_file_exists("/mnt/cd1/install/system.img"))
    return "/mnt/cd1/install/system.img";
  return NULL;
}

static const char *installer_system_image_root_path(void) {
  if (installer_system_image_payload_path[0] &&
      installer_payload_file_exists(installer_system_image_payload_path))
    return installer_system_image_payload_path;
  if (installer_payload_file_exists("/install/system-image"))
    return "/install/system-image";
  if (installer_payload_file_exists("/setup/install/system-image"))
    return "/setup/install/system-image";
  if (installer_payload_file_exists("/install/system-image.zip"))
    return "/install/system-image.zip";
  if (installer_payload_file_exists("/setup/install/system-image.zip"))
    return "/setup/install/system-image.zip";
  if (installer_payload_file_exists("/mnt/cd1/install/system-image"))
    return "/mnt/cd1/install/system-image";
  if (installer_payload_file_exists("/mnt/cd1/install/system-image.zip"))
    return "/mnt/cd1/install/system-image.zip";
  return "/setup/install/system-image";
}

static void installer_select_system_image_payload(const char *path) {
  if (!path || !path[0]) {
    installer_system_image_payload_path[0] = '\0';
    return;
  }
  str_copy_safe(installer_system_image_payload_path, path,
                sizeof(installer_system_image_payload_path));
}

static const char *installer_boot_payload_root_path(void) {
  if (installer_boot_payload_path[0] &&
      installer_payload_file_exists(installer_boot_payload_path))
    return installer_boot_payload_path;
  if (installer_payload_file_exists("/install/boot-files.img"))
    return "/install/boot-files.img";
  if (installer_payload_file_exists("/setup/bootimage.img"))
    return "/setup/bootimage.img";
  if (installer_payload_file_exists("/mnt/cd1/install/boot-files.img"))
    return "/mnt/cd1/install/boot-files.img";
  return installer_system_image_root_path();
}

static void installer_select_boot_payload(const char *path) {
  if (!path || !path[0]) {
    installer_boot_payload_path[0] = '\0';
    return;
  }
  str_copy_safe(installer_boot_payload_path, path,
                sizeof(installer_boot_payload_path));
}

static int installer_validate_system_image_candidate(const char *payload_root) {
  static const char *required_suffixes[] = {
      "/boot/main.sys",
      "/boot/bootloader.sys",
      "/boot/limine-bios.sys",
      "/boot/limine-bios-cd.bin",
      "/BOOTABLE.CFG",
      "/boot/BOOTABLE.CFG",
      "/EFI/BOOT/BOOTX64.EFI",
      "/EFI/OS8/STARTUPX64.EFI",
      "/EFI/OS8/os8boot.cfg",
      "/EFI/BOOT/BOOTABLE.CFG",
      "/System/installer-state.txt",
      "/System/efi-boot.cfg",
      "/System/mbr-boot.cfg",
  };
  static const char *limine_cfg_suffixes[] = {
      "/limine.conf",
      "/boot/limine.conf",
      "/limine/limine.conf",
  };
  char full_path[192];
  char msg[320];

  if (!payload_root || !payload_root[0])
    return -1;

  if (installer_boot_payload_is_image(payload_root)) {
    for (int i = 0;
         i < (int)(sizeof(required_suffixes) / sizeof(required_suffixes[0]));
         i++) {
      if (media_boot_image_file_has_entry(payload_root, required_suffixes[i]))
        continue;
      str_copy_safe(msg, "boot image unusable: ", sizeof(msg));
      installer_append_to_buf(msg, sizeof(msg), payload_root);
      installer_append_to_buf(msg, sizeof(msg), required_suffixes[i]);
      installer_log(msg);
      return -1;
    }

    for (int i = 0;
         i < (int)(sizeof(limine_cfg_suffixes) / sizeof(limine_cfg_suffixes[0]));
         i++) {
      if (media_boot_image_file_has_entry(payload_root, limine_cfg_suffixes[i]))
        return 0;
    }

    str_copy_safe(msg, "boot image unusable: no legacy BIOS config in ",
                  sizeof(msg));
    installer_append_to_buf(msg, sizeof(msg), payload_root);
    installer_log(msg);
    return -1;
  }

  if (installer_system_image_is_archive(payload_root)) {
    for (int i = 0;
         i < (int)(sizeof(required_suffixes) / sizeof(required_suffixes[0]));
         i++) {
      if (media_zip_file_has_entry(payload_root, required_suffixes[i]))
        continue;
      str_copy_safe(msg, "install archive unusable: ", sizeof(msg));
      installer_append_to_buf(msg, sizeof(msg), payload_root);
      installer_append_to_buf(msg, sizeof(msg), required_suffixes[i]);
      installer_log(msg);
      return -1;
    }

    for (int i = 0;
         i < (int)(sizeof(limine_cfg_suffixes) / sizeof(limine_cfg_suffixes[0]));
         i++) {
      if (media_zip_file_has_entry(payload_root, limine_cfg_suffixes[i]))
        return 0;
    }

    str_copy_safe(msg, "install archive unusable: no legacy BIOS config in ",
                  sizeof(msg));
    installer_append_to_buf(msg, sizeof(msg), payload_root);
    installer_log(msg);
    return -1;
  }

  for (int i = 0;
       i < (int)(sizeof(required_suffixes) / sizeof(required_suffixes[0]));
       i++) {
    str_copy_safe(full_path, payload_root, sizeof(full_path));
    installer_append_to_buf(full_path, sizeof(full_path), required_suffixes[i]);
    if (installer_payload_file_exists(full_path))
      continue;
    str_copy_safe(msg, "install payload missing: ", sizeof(msg));
    installer_append_to_buf(msg, sizeof(msg), full_path);
    installer_log(msg);
    return -1;
  }

  for (int i = 0;
       i < (int)(sizeof(limine_cfg_suffixes) / sizeof(limine_cfg_suffixes[0]));
       i++) {
    str_copy_safe(full_path, payload_root, sizeof(full_path));
    installer_append_to_buf(full_path, sizeof(full_path),
                            limine_cfg_suffixes[i]);
    if (installer_payload_file_exists(full_path))
      return 0;
  }

  str_copy_safe(msg, "install payload missing: no legacy BIOS config in ",
                sizeof(msg));
  installer_append_to_buf(msg, sizeof(msg), payload_root);
  installer_log(msg);
  return -1;
}

static int installer_validate_system_image_payload(void) {
  static const char *payload_candidates[] = {
      "/install/system-image",
      "/setup/install/system-image",
      "/install/system-image.zip",
      "/setup/install/system-image.zip",
      "/mnt/cd1/install/system-image",
      "/mnt/cd1/install/system-image.zip",
  };

  installer_select_system_image_payload(NULL);
  for (int i = 0;
       i < (int)(sizeof(payload_candidates) / sizeof(payload_candidates[0]));
       i++) {
    if (!installer_payload_file_exists(payload_candidates[i]))
      continue;
    if (installer_validate_system_image_candidate(payload_candidates[i]) == 0) {
      installer_select_system_image_payload(payload_candidates[i]);
      return 0;
    }
  }

  installer_log("install payload missing: no usable system image payload found");
  return -1;
}

static int installer_validate_boot_payload_candidate(const char *payload_root) {
  static const char *required_suffixes[] = {
      "/boot/main.sys",
      "/boot/bootloader.sys",
      "/boot/limine-bios.sys",
      "/boot/limine-bios-cd.bin",
      "/BOOTABLE.CFG",
      "/boot/BOOTABLE.CFG",
      "/EFI/BOOT/BOOTX64.EFI",
      "/EFI/OS8/STARTUPX64.EFI",
      "/EFI/OS8/os8boot.cfg",
      "/EFI/BOOT/BOOTABLE.CFG",
      "/System/installer-state.txt",
      "/System/efi-boot.cfg",
      "/System/mbr-boot.cfg",
  };
  static const char *limine_cfg_suffixes[] = {
      "/limine.conf",
      "/boot/limine.conf",
      "/limine/limine.conf",
  };
  char full_path[192];
  char msg[320];

  if (!payload_root || !payload_root[0])
    return -1;

  if (installer_boot_payload_is_image(payload_root)) {
    for (int i = 0;
         i < (int)(sizeof(required_suffixes) / sizeof(required_suffixes[0]));
         i++) {
      if (media_boot_image_file_has_entry(payload_root, required_suffixes[i]))
        continue;
      str_copy_safe(msg, "boot image unusable: ", sizeof(msg));
      installer_append_to_buf(msg, sizeof(msg), payload_root);
      installer_append_to_buf(msg, sizeof(msg), required_suffixes[i]);
      installer_log(msg);
      return -1;
    }

    for (int i = 0;
         i < (int)(sizeof(limine_cfg_suffixes) / sizeof(limine_cfg_suffixes[0]));
         i++) {
      if (media_boot_image_file_has_entry(payload_root, limine_cfg_suffixes[i]))
        return 0;
    }

    str_copy_safe(msg, "boot image unusable: no legacy BIOS config in ",
                  sizeof(msg));
    installer_append_to_buf(msg, sizeof(msg), payload_root);
    installer_log(msg);
    return -1;
  }

  if (installer_system_image_is_archive(payload_root)) {
    for (int i = 0;
         i < (int)(sizeof(required_suffixes) / sizeof(required_suffixes[0]));
         i++) {
      if (media_zip_file_has_entry(payload_root, required_suffixes[i]))
        continue;
      str_copy_safe(msg, "boot archive unusable: ", sizeof(msg));
      installer_append_to_buf(msg, sizeof(msg), payload_root);
      installer_append_to_buf(msg, sizeof(msg), required_suffixes[i]);
      installer_log(msg);
      return -1;
    }

    for (int i = 0;
         i < (int)(sizeof(limine_cfg_suffixes) / sizeof(limine_cfg_suffixes[0]));
         i++) {
      if (media_zip_file_has_entry(payload_root, limine_cfg_suffixes[i]))
        return 0;
    }

    str_copy_safe(msg, "boot archive unusable: no legacy BIOS config in ",
                  sizeof(msg));
    installer_append_to_buf(msg, sizeof(msg), payload_root);
    installer_log(msg);
    return -1;
  }

  for (int i = 0;
       i < (int)(sizeof(required_suffixes) / sizeof(required_suffixes[0]));
       i++) {
    str_copy_safe(full_path, payload_root, sizeof(full_path));
    installer_append_to_buf(full_path, sizeof(full_path), required_suffixes[i]);
    if (installer_payload_file_exists(full_path))
      continue;
    str_copy_safe(msg, "boot payload missing: ", sizeof(msg));
    installer_append_to_buf(msg, sizeof(msg), full_path);
    installer_log(msg);
    return -1;
  }

  for (int i = 0;
       i < (int)(sizeof(limine_cfg_suffixes) / sizeof(limine_cfg_suffixes[0]));
       i++) {
    str_copy_safe(full_path, payload_root, sizeof(full_path));
    installer_append_to_buf(full_path, sizeof(full_path),
                            limine_cfg_suffixes[i]);
    if (installer_payload_file_exists(full_path))
      return 0;
  }

  str_copy_safe(msg, "boot payload missing: no legacy BIOS config in ",
                sizeof(msg));
  installer_append_to_buf(msg, sizeof(msg), payload_root);
  installer_log(msg);
  return -1;
}

static int installer_validate_boot_payload(void) {
  static const char *payload_candidates[] = {
      "/install/boot-files.img",
      "/setup/bootimage.img",
      "/install/system-image.zip",
      "/setup/install/system-image.zip",
      "/install/system-image",
      "/setup/install/system-image",
      "/mnt/cd1/install/system-image",
      "/mnt/cd1/install/boot-files.img",
      "/mnt/cd1/install/system-image.zip",
  };

  installer_select_boot_payload(NULL);
  if (installer_system_image_payload_path[0]) {
    installer_select_boot_payload(installer_system_image_payload_path);
    return 0;
  }
  for (int i = 0;
       i < (int)(sizeof(payload_candidates) / sizeof(payload_candidates[0]));
       i++) {
    if (!installer_payload_file_exists(payload_candidates[i]))
      continue;
    if (installer_validate_boot_payload_candidate(payload_candidates[i]) == 0) {
      installer_select_boot_payload(payload_candidates[i]);
      return 0;
    }
  }

  installer_log("install payload missing: no usable boot payload found");
  return -1;
}

static int installer_validate_raw_system_disk_image_payload(void) {
  const char *image_path = installer_system_disk_image_path();
  char msg[320];

  if (image_path && installer_payload_file_exists(image_path))
    return 0;

  str_copy_safe(msg, "install payload missing: ", sizeof(msg));
  installer_append_to_buf(msg, sizeof(msg),
                          image_path ? image_path : "/install/system.img");
  installer_log(msg);
  return -1;
}

static int installer_copy_tree_to_root(const char *src_root, const char *dst_root,
                                       int *copied_files, int *failed_files,
                                       const char *log_label) {
  installer_copy_ctx_t ctx = {"", "", 0, 0};
  struct file *dir;
  char msg[320];

  if (!src_root || !src_root[0] || !dst_root || !dst_root[0])
    return -1;

  ctx.src_root = src_root;
  str_copy_safe(ctx.dst_root, dst_root, sizeof(ctx.dst_root));
  if (installer_try_make_dir(dst_root) != 0) {
    str_copy_safe(msg, "install failed: target root creation failed for ",
                  sizeof(msg));
    installer_append_to_buf(msg, sizeof(msg), dst_root);
    installer_log(msg);
    return -1;
  }

  dir = vfs_open(ctx.src_root, O_RDONLY, 0);
  if (!dir) {
    str_copy_safe(msg, "install failed: source payload missing: ",
                  sizeof(msg));
    installer_append_to_buf(msg, sizeof(msg), src_root);
    installer_log(msg);
    return -1;
  }

  str_copy_safe(msg, "copying ", sizeof(msg));
  installer_append_to_buf(msg, sizeof(msg), log_label ? log_label : "payload");
  installer_append_to_buf(msg, sizeof(msg), " from ");
  installer_append_to_buf(msg, sizeof(msg), src_root);
  installer_append_to_buf(msg, sizeof(msg), " to ");
  installer_append_to_buf(msg, sizeof(msg), dst_root);
  installer_log(msg);
  vfs_readdir(dir, &ctx, installer_copy_tree_callback);
  vfs_close(dir);

  if (copied_files)
    *copied_files += ctx.copied_files;
  if (failed_files)
    *failed_files += ctx.failed_files;
  return (ctx.copied_files > 0 && ctx.failed_files == 0) ? 0 : -1;
}

static int installer_copy_boot_payload_to_root(const char *target_root,
                                               int *copied_files,
                                               int *failed_files) {
  const char *boot_payload_root = installer_boot_payload_root_path();

  if (!boot_payload_root || !boot_payload_root[0])
    return installer_copy_boot_aliases(target_root, copied_files, failed_files);

  if (installer_boot_payload_is_image(boot_payload_root)) {
    if (media_boot_image_extract_file_to_root(boot_payload_root, target_root,
                                              copied_files, failed_files) != 0)
      return -1;
  } else if (installer_system_image_is_archive(boot_payload_root)) {
    if (media_zip_extract_file_to_root(boot_payload_root, target_root,
                                       copied_files, failed_files) != 0)
      return -1;
  } else if (installer_copy_tree_to_root(boot_payload_root, target_root,
                                         copied_files, failed_files,
                                         "boot payload") != 0) {
    return -1;
  }

  return installer_copy_boot_aliases(target_root, copied_files, failed_files);
}

static int installer_copy_boot_aliases(const char *target_root, int *copied_files,
                                       int *failed_files) {
  char boot_bios_path[192];
  char root_bios_path[192];
  char limine_dir_path[192];
  char limine_bios_path[192];
  char boot_limine_dir_path[192];
  char boot_limine_bios_path[192];
  int copied = 0;
  int failed = 0;

  if (!target_root || !target_root[0])
    return -1;

  str_copy_safe(boot_bios_path, target_root, sizeof(boot_bios_path));
  installer_append_to_buf(boot_bios_path, sizeof(boot_bios_path),
                          "/boot/limine-bios.sys");
  if (!installer_payload_file_exists(boot_bios_path))
    return 0;

  str_copy_safe(root_bios_path, target_root, sizeof(root_bios_path));
  installer_append_to_buf(root_bios_path, sizeof(root_bios_path),
                          "/limine-bios.sys");
  if (installer_copy_file(boot_bios_path, root_bios_path) == 0) {
    copied++;
    installer_note_copy_progress(root_bios_path, 1);
  } else {
    failed++;
    installer_note_copy_progress(root_bios_path, 0);
  }

  str_copy_safe(limine_dir_path, target_root, sizeof(limine_dir_path));
  installer_append_to_buf(limine_dir_path, sizeof(limine_dir_path), "/limine");
  if (installer_try_make_dir(limine_dir_path) == 0) {
    str_copy_safe(limine_bios_path, limine_dir_path, sizeof(limine_bios_path));
    installer_append_to_buf(limine_bios_path, sizeof(limine_bios_path),
                            "/limine-bios.sys");
    if (installer_copy_file(boot_bios_path, limine_bios_path) == 0) {
      copied++;
      installer_note_copy_progress(limine_bios_path, 1);
    } else {
      failed++;
      installer_note_copy_progress(limine_bios_path, 0);
    }
  } else {
    failed++;
  }

  str_copy_safe(boot_limine_dir_path, target_root, sizeof(boot_limine_dir_path));
  installer_append_to_buf(boot_limine_dir_path, sizeof(boot_limine_dir_path),
                          "/boot/limine");
  if (installer_try_make_dir(boot_limine_dir_path) == 0) {
    str_copy_safe(boot_limine_bios_path, boot_limine_dir_path,
                  sizeof(boot_limine_bios_path));
    installer_append_to_buf(boot_limine_bios_path,
                            sizeof(boot_limine_bios_path), "/limine-bios.sys");
    if (installer_copy_file(boot_bios_path, boot_limine_bios_path) == 0) {
      copied++;
      installer_note_copy_progress(boot_limine_bios_path, 1);
    } else {
      failed++;
      installer_note_copy_progress(boot_limine_bios_path, 0);
    }
  } else {
    failed++;
  }

  if (copied_files)
    *copied_files += copied;
  if (failed_files)
    *failed_files += failed;
  return failed == 0 ? 0 : -1;
}

static int installer_apply_system_disk_image(int disk_index) {
  const char *image_path = installer_system_disk_image_path();
  char msg[192];
  extern int storage_write_disk_image_file(int disk_index, const char *path);
  extern void refresh_external_storage_views(void);

  if (!image_path || disk_index < 0)
    return -1;

  str_copy_safe(msg, "writing raw system disk image from ", sizeof(msg));
  installer_append_to_buf(msg, sizeof(msg), image_path);
  installer_log(msg);

  if (storage_write_disk_image_file(disk_index, image_path) != 0) {
    installer_log("install failed: raw disk image write failed");
    return -1;
  }

  refresh_external_storage_views();
  installer_log("raw system disk image written to target disk");
  return 0;
}

static int installer_apply_system_image_payload(const char *target_root) {
  int copied = 0;
  int failed = 0;
  char msg[160];
  int idx = 0;
  const char *system_payload_root = installer_system_image_root_path();
  const char *boot_payload_root = installer_boot_payload_root_path();

  if (installer_copy_system_image_to_root(target_root, &copied, &failed) != 0) {
    installer_log("install failed: extracted system image copy failed");
    return -1;
  }
  if (!installer_str_equal(boot_payload_root, system_payload_root) &&
      installer_copy_boot_payload_to_root(target_root, &copied, &failed) != 0) {
    installer_log("install failed: boot payload copy failed");
    return -1;
  }
  if (installer_str_equal(boot_payload_root, system_payload_root) &&
      installer_copy_boot_aliases(target_root, &copied, &failed) != 0) {
    installer_log("install failed: boot alias copy failed");
    return -1;
  }

  installer_copied_files = copied;
  installer_failed_files = failed;
  str_copy_safe(msg, "installed extracted system image files: ", sizeof(msg));
  while (msg[idx] && idx < (int)sizeof(msg) - 1)
    idx++;
  append_decimal(msg, &idx, copied);
  installer_append_to_buf(msg, sizeof(msg), " copied");
  if (failed > 0) {
    installer_append_to_buf(msg, sizeof(msg), ", ");
    idx = 0;
    while (msg[idx] && idx < (int)sizeof(msg) - 1)
      idx++;
    append_decimal(msg, &idx, failed);
    installer_append_to_buf(msg, sizeof(msg), " failed");
  }
  installer_log(msg);
  return 0;
}

static int installer_reboot_seconds_remaining(void) {
  uint64_t now;
  uint64_t remaining_ms;

  if (!installer_reboot_deadline_ms)
    return 0;

  now = arch_timer_get_ms();
  if (now >= installer_reboot_deadline_ms)
    return 0;

  remaining_ms = installer_reboot_deadline_ms - now;
  return (int)((remaining_ms + 999) / 1000);
}

static const char *installer_page_title(void) {
  switch (installer_page) {
  case INSTALLER_PAGE_TARGET:
    return "Step 2  Choose Disk";
  case INSTALLER_PAGE_REVIEW:
    return "Step 3  Review";
  case INSTALLER_PAGE_PROGRESS:
    return "Step 4  Install";
  case INSTALLER_PAGE_COMPLETE:
    return "Step 5  Finish";
  default:
    return "Step 1  Welcome";
  }
}

static void installer_window_on_key(struct window *win, int key) {
  (void)win;

  if (!gui_is_installer_mode())
    return;

  if (installer_show_restart_screen)
    installer_page = INSTALLER_PAGE_COMPLETE;
  else if (installer_active)
    installer_page = INSTALLER_PAGE_PROGRESS;

  if (installer_page == INSTALLER_PAGE_COMPLETE) {
    if (key >= 0) {
      extern void arch_reboot(void);
      installer_reboot_deadline_ms = 0;
      gui_flush_account_state_before_power_transition();
      arch_reboot();
    }
    return;
  }

  if (installer_page == INSTALLER_PAGE_PROGRESS)
    return;

  if (installer_page == INSTALLER_PAGE_WELCOME &&
      (key == 'p' || key == 'P')) {
    extern void arch_reboot(void);
    gui_flush_account_state_before_power_transition();
    arch_reboot();
    return;
  }

  if (installer_page == INSTALLER_PAGE_TARGET) {
    installer_refresh_disk_inventory();
    if (installer_disk_count > 0) {
      if (key == KEY_UP) {
        installer_selected_disk =
            (installer_selected_disk + installer_disk_count - 1) %
            installer_disk_count;
        installer_set_status("Installer target disk updated.");
        return;
      }
      if (key == KEY_DOWN) {
        installer_selected_disk =
            (installer_selected_disk + 1) % installer_disk_count;
        installer_set_status("Installer target disk updated.");
        return;
      }
    }
  }

  if ((installer_page == INSTALLER_PAGE_TARGET ||
       installer_page == INSTALLER_PAGE_REVIEW) &&
      (key == 'p' || key == 'P')) {
    open_partition_manager_window(72, 54);
    return;
  }

  if (key == 27) {
    if (installer_page > INSTALLER_PAGE_WELCOME &&
        installer_page < INSTALLER_PAGE_PROGRESS) {
      installer_page--;
    }
    return;
  }

  if (key != '\r' && key != '\n' && key != ' ')
    return;

  if (installer_page == INSTALLER_PAGE_WELCOME) {
    installer_page = INSTALLER_PAGE_TARGET;
    return;
  }
  if (installer_page == INSTALLER_PAGE_TARGET) {
    if (!installer_has_run && !installer_active)
      installer_start_background_install();
    return;
  }
  if (installer_page == INSTALLER_PAGE_REVIEW && !installer_has_run &&
      !installer_active) {
    installer_start_background_install();
  }
}

static int installer_finalize_install(void) {
  char summary[96];
  int selected_disk_index;
  int user_partition_result = 0;
  int install_partition_result = 0;
  int raw_disk_image_install = installer_system_disk_image_path() != NULL;

  extern int storage_prepare_user_partition(int disk_index);
  extern int storage_ensure_install_partitions(int disk_index);

  dock_loaded = 0;
  load_dock_config();
  desktop_refresh();
  selected_disk_index = installer_selected_disk_index();
  if (selected_disk_index < 0) {
    installer_log("install failed: target disk index unavailable");
    return -1;
  }
  if (!raw_disk_image_install) {
    install_partition_result =
        storage_ensure_install_partitions(selected_disk_index);
    if (install_partition_result < 0) {
      installer_log("install failed: could not prepare boot partitions");
      return -1;
    } else if (install_partition_result > 0) {
      installer_log("prepared EFI/system boot partitions for the target disk");
    }

    user_partition_result =
        storage_prepare_user_partition(selected_disk_index);
    if (user_partition_result > 0) {
      installer_log("created HDD user data partition for first boot");
    } else if (user_partition_result == 0) {
      installer_log("user data partition already present or not required");
    } else {
      installer_log("warning: could not prepare HDD user data partition");
    }
  } else {
    installer_log("raw disk image install complete; keeping on-disk partition layout unchanged");
  }

  if (installer_write_target_config() != 0) {
    installer_log("install failed: could not persist install target config");
    return -1;
  }

  if (installer_refresh_bootloader_state("", 1) != 0) {
    installer_log("install failed: could not refresh live bootloader state");
    return -1;
  }
  if (installer_refresh_bootloader_state(installer_target_root, 1) != 0) {
    installer_log("install failed: could not persist target bootloader state");
    return -1;
  }

  summary[0] = '\0';
  str_copy_safe(summary, "Installed system image; final setup runs after reboot",
                sizeof(summary));
  installer_log("install complete");
  installer_log("selected hard disk now contains the bootable system image");
  installer_log("first HDD boot will complete OS configuration");
  installer_progress_current_item[0] = '\0';
  installer_set_progress_state(
      100, "Ready To Reboot", summary,
      "Rebooting to the hard disk for the final installation phase.");
  installer_log("reboot scheduled in 3 seconds");
  installer_has_run = 1;
  installer_show_restart_screen = 1;
  installer_page = INSTALLER_PAGE_COMPLETE;
  installer_active = 0;
  installer_phase = 0;
  installer_reboot_deadline_ms = arch_timer_get_ms() + 3000;
  return 0;
}

static void installer_start_background_install(void) {
  extern void refresh_external_storage_views(void);

  installer_log_clear();
  refresh_external_storage_views();
  installer_refresh_disk_inventory();
  installer_target_disk_index = installer_selected_disk_index();
  if (installer_target_disk_index < 0) {
    installer_fail_background("Install blocked. No valid target disk is selected.",
                              "install blocked: selected target disk missing");
    return;
  }
  installer_has_run = 0;
  installer_show_restart_screen = 0;
  installer_active = 1;
  installer_page = INSTALLER_PAGE_PROGRESS;
  installer_phase = 1;
  installer_progress_done = 0;
  installer_progress_total = 100;
  installer_progress_total_files = 0;
  installer_progress_processed_files = 0;
  installer_copied_files = 0;
  installer_failed_files = 0;
  installer_ensured_changes = 0;
  installer_reboot_deadline_ms = 0;
  installer_install_journal_ready = 0;
  installer_install_journal_disk_index = -1;
  installer_install_journal_next_lba = 0;
  installer_install_journal_last_lba = 0;
  installer_target_root[0] = '\0';
  installer_efi_root[0] = '\0';
  installer_update_root[0] = '\0';
  installer_progress_current_item[0] = '\0';
  installer_select_system_image_payload(NULL);
  installer_select_boot_payload(NULL);
  installer_set_progress_state(2, "Preparing", "Preparing install...",
                               "Loading installer context and refreshing storage.");
  installer_log("starting system image install");
}

static void installer_process_autorun(void) {
  extern int boot_cmdline_has_token(const char *token);

  if (!gui_is_installer_mode())
    return;
  if (!boot_cmdline_has_token("installer.autorun=1"))
    return;
  if (installer_autorun_attempted || installer_has_run || installer_active)
    return;

  installer_refresh_disk_inventory();
  if (installer_disk_count <= 0)
    return;

  installer_autorun_attempted = 1;
  installer_set_status("Autorun stress install started.");
  installer_start_background_install();
}

static void installer_fail_background(const char *status, const char *log_line) {
  installer_active = 0;
  installer_phase = 0;
  installer_show_restart_screen = 0;
  installer_target_disk_index = -1;
  installer_page = INSTALLER_PAGE_REVIEW;
  installer_set_status(status);
  if (log_line)
    installer_log(log_line);
}

static void installer_process_background_install(void) {
  if (installer_reboot_deadline_ms &&
      arch_timer_get_ms() >= installer_reboot_deadline_ms) {
    installer_reboot_deadline_ms = 0;
    {
      extern void arch_reboot(void);
      gui_flush_account_state_before_power_transition();
      arch_reboot();
    }
    return;
  }

  if (!installer_active)
    return;

  switch (installer_phase) {
  case 1:
    printk(KERN_INFO "INSTALL: phase 1 scanning target disks\n");
    installer_refresh_disk_inventory();
    if (installer_disk_count <= 0) {
      installer_fail_background("Install blocked. No real target disk is available.",
                                "install blocked: no real target disk");
      return;
    }
    installer_set_progress_state(
        8, "Scanning Disks", "Checking target disk...",
        "Validating that a real writable target disk is available.");
    installer_phase = 2;
    return;
  case 2: {
    extern int storage_disk_supports_partition_writes(int disk_index);
    printk(KERN_INFO "INSTALL: phase 2 validating target and payload\n");
    const char *raw_image_path = installer_system_disk_image_path();
    int selected_disk_index = installer_selected_disk_index();
    if (selected_disk_index < 0) {
      installer_fail_background("Install blocked. No valid target disk is selected.",
                                "install blocked: selected target disk missing");
      return;
    }
    printk(KERN_INFO "INSTALL: checking target disk write support index=%d\n",
           selected_disk_index);
    if (!storage_disk_supports_partition_writes(selected_disk_index)) {
      installer_fail_background("Install blocked. Target disk is not writable.",
                                "install blocked: target disk is not writable");
      return;
    }
    installer_target_root_path(installer_target_root,
                               sizeof(installer_target_root));
    installer_partition_root_path(installer_efi_root, sizeof(installer_efi_root),
                                  "EFI");
    installer_partition_root_path(installer_update_root,
                                  sizeof(installer_update_root), "boot");
    str_copy_safe(installer_log_target_root, installer_target_root,
                  sizeof(installer_log_target_root));
    if (raw_image_path) {
      if (installer_validate_raw_system_disk_image_payload() != 0) {
        installer_fail_background("Install blocked. Raw system disk image is missing from the installer image.",
                                  "install blocked: raw system disk image payload missing");
        return;
      }
      installer_progress_total_files = 1;
      installer_progress_processed_files = 0;
      installer_set_progress_state(
          18, "Validating Payload", "Preparing raw system disk image...",
          "Bootable disk image found. The installer will write it directly to the target disk.");
    } else {
      printk(KERN_INFO "INSTALL: validating extracted system payload candidates\n");
      if (installer_validate_system_image_payload() != 0) {
        installer_fail_background("Install blocked. Boot files are missing from the installer image.",
                                  "install blocked: boot payload incomplete");
        return;
      }
      printk(KERN_INFO "INSTALL: selected system payload %s\n",
             installer_system_image_root_path());
      installer_select_boot_payload(installer_system_image_root_path());
      installer_progress_total_files =
          installer_count_tree_files(installer_system_image_root_path());
      printk(KERN_INFO "INSTALL: system payload count=%d\n",
             installer_progress_total_files);
      if (!installer_str_equal(installer_boot_payload_root_path(),
                               installer_system_image_root_path())) {
        printk(KERN_INFO "INSTALL: selected boot payload %s\n",
               installer_boot_payload_root_path());
        installer_progress_total_files +=
            installer_count_tree_files(installer_boot_payload_root_path());
        printk(KERN_INFO "INSTALL: combined payload count=%d\n",
               installer_progress_total_files);
      }
      installer_progress_total_files +=
          installer_boot_alias_copy_count(installer_target_root);
      if (installer_progress_total_files <= 0)
        installer_progress_total_files = 1;
      installer_progress_processed_files = 0;
      installer_set_progress_state(
          18, "Validating Payload", "Preparing extracted system image...",
          "Payload verified. Counting files before copy begins.");
    }
    installer_phase = 3;
    return;
  }
  case 3:
    if (installer_system_disk_image_path()) {
      int selected_disk_index = installer_selected_disk_index();
      if (installer_apply_system_disk_image(selected_disk_index) != 0) {
        installer_fail_background("Install failed. Raw system disk image write failed.",
                                  "install failed: raw system disk image write failed");
        return;
      }
      installer_copied_files = 1;
      installer_failed_files = 0;
      installer_progress_processed_files = installer_progress_total_files;
    } else if (installer_apply_system_image_payload(installer_target_root) != 0) {
      installer_fail_background("Install failed. Extracted system image copy failed.",
                                "install failed: extracted system image copy failed");
      return;
    }
    installer_set_progress_state(92, "Finalizing", "Finalizing install...",
                                 "Writing boot state and preparing first boot.");
    installer_phase = 4;
    return;
  case 4:
    installer_set_progress_state(
        97, "Scheduling Reboot", "Wrapping up installation...",
        "Syncing install metadata and scheduling reboot.");
    if (installer_finalize_install() != 0) {
      installer_fail_background("Install failed during finalization.",
                                "install failed: finalization failed");
    }
    return;
  default:
    return;
  }
}

void gui_installer_background_tick(void) {
  if (!startup_setup_account_active()) {
    installer_process_autorun();
    installer_process_background_install();
  }
}

static int installer_stage_value(int start, int end) {
  int done = installer_progress_done;

  if (done <= start)
    return 0;
  if (done >= end)
    return 100;
  return ((done - start) * 100) / (end - start);
}

static int installer_text_progress_percent(void) {
  if (installer_progress_total <= 0)
    return 0;
  return (installer_progress_done * 100) / installer_progress_total;
}

static int installer_text_strlen(const char *text) {
  int len = 0;
  if (!text)
    return 0;
  while (text[len])
    len++;
  return len;
}

static void installer_text_draw_centered(int x, int y, int w, const char *text,
                                         uint32_t fg, uint32_t bg) {
  int text_w = installer_text_strlen(text) * FONT_WIDTH;
  int draw_x = x + (w - text_w) / 2;
  if (draw_x < x)
    draw_x = x;
  gui_draw_string(draw_x, y, text, fg, bg);
}

static void installer_text_build_disk_label(int row, char *buf, int max) {
  int idx = 0;
  if (!buf || max <= 0)
    return;
  buf[0] = '\0';
  for (const char *p = "DISK "; *p && idx < max - 1; p++)
    buf[idx++] = *p;
  append_decimal(buf, &idx, row);
  if (row == installer_selected_disk) {
    if (idx < max - 2) {
      buf[idx++] = ' ';
      buf[idx++] = '-';
    }
  }
  buf[idx] = '\0';
}

static void installer_text_draw_footer(int content_x, int content_y, int content_w,
                                       int content_h, const char *text) {
  int footer_h = 38;
  int text_y = content_y + content_h - footer_h + 8;
  gui_draw_rect(content_x, content_y + content_h - footer_h, content_w, footer_h,
                0x5A5A5A);
  if (text && text[0])
    gui_draw_string(content_x + 8, text_y, text, 0xFFFFFF, 0x5A5A5A);
}

static void installer_text_draw_progress_bar(int x, int y, int w, int h,
                                             int percent, uint32_t fill_color) {
  int fill_w;
  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;
  gui_draw_rect(x, y, w, h, 0x5A5A5A);
  fill_w = (w * percent) / 100;
  if (fill_w > 0)
    gui_draw_rect(x, y, fill_w, h, fill_color);
}

static void draw_installation_stage_bar(int x, int y, int w, int h,
                                        int percent) {
  int fill_w;

  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;

  draw_rounded_rect(x, y, w, h, h / 2, 0x050505);
  fill_w = (w * percent) / 100;
  if (fill_w > 0)
    draw_rounded_rect(x, y, fill_w < h ? h : fill_w, h, h / 2, 0xA6A6A6);
}

static void draw_installation_stage_layout(int content_x, int content_y,
                                           int content_w, int content_h,
                                           int copy_percent,
                                           int expand_percent,
                                           int configure_percent,
                                           int complete_percent) {
  int card_w = content_w > 660 ? 604 : content_w - 48;
  int card_h = 482;
  int card_x;
  int card_y;
  int text_x;
  int bar_x;
  int bar_w;
  int row_y;
  int top_r = 0x9A;
  int top_g = 0xF7;
  int top_b = 0xEE;
  int bottom_r = 0x08;
  int bottom_g = 0x78;
  int bottom_b = 0xB8;

  if (card_w < 360)
    card_w = content_w - 24;
  if (card_h > content_h - 56)
    card_h = content_h - 56;

  card_x = content_x + (content_w - card_w) / 2;
  card_y = content_y + (content_h - card_h) / 2;
  text_x = card_x + 36;
  bar_x = card_x + 30;
  bar_w = card_w - 72;

  for (int y = 0; y < content_h; y++) {
    int denom = content_h > 1 ? content_h - 1 : 1;
    uint32_t r = (uint32_t)(top_r + ((bottom_r - top_r) * y) / denom);
    uint32_t g = (uint32_t)(top_g + ((bottom_g - top_g) * y) / denom);
    uint32_t b = (uint32_t)(top_b + ((bottom_b - top_b) * y) / denom);
    gui_draw_rect(content_x, content_y + y, content_w, 1,
                  (r << 16) | (g << 8) | b);
  }

  gui_fill_rect_alpha(card_x + 6, card_y + 8, card_w, card_h, 0x22000000);
  draw_rounded_rect(card_x, card_y, card_w, card_h, 18, 0xF6F6F6);

  row_y = card_y + 30;
  gui_draw_string(text_x, row_y, "Copying OS files...", 0x000000, 0xF6F6F6);
  draw_installation_stage_bar(bar_x, row_y + 64, bar_w, 10, copy_percent);

  row_y += 108;
  gui_draw_string(text_x, row_y, "Expanding Files...", 0x000000, 0xF6F6F6);
  draw_installation_stage_bar(bar_x, row_y + 72, bar_w, 8, expand_percent);

  row_y += 108;
  gui_draw_string(text_x, row_y, "Configuring OS...", 0x000000, 0xF6F6F6);
  draw_installation_stage_bar(bar_x, row_y + 72, bar_w, 8, configure_percent);

  row_y += 112;
  gui_draw_string(text_x, row_y, "Completing installation...", 0x000000,
                  0xF6F6F6);
  draw_installation_stage_bar(bar_x, row_y + 72, bar_w, 8, complete_percent);
}

static void draw_installer_window(int content_x, int content_y, int content_w,
                                  int content_h) {
  const gui_theme_palette_t *theme = gui_theme_palette();
  int text_mode = gui_installer_text_mode();
  if (!installer_active && !installer_show_restart_screen)
    installer_refresh_disk_inventory();
  if (installer_show_restart_screen)
    installer_page = INSTALLER_PAGE_COMPLETE;
  else if (installer_active)
    installer_page = INSTALLER_PAGE_PROGRESS;

  if (text_mode) {
    uint32_t bg = 0x0000AA;
    uint32_t fg = 0xFFFFFF;
    uint32_t bar_blue = 0x0000FF;
    uint32_t bar_red = 0x7A0000;
    int body_x = content_x;
    int body_y = content_y;
    int body_w = content_w;
    int body_h = content_h - 38;
    int progress_percent = installer_text_progress_percent();
    int bar_w = body_w > 360 ? body_w - 220 : body_w - 60;
    int bar_x = body_x + (body_w - bar_w) / 2;

    gui_draw_rect(content_x, content_y, content_w, content_h, bg);
    gui_draw_string(body_x + 6, body_y + 8, "OS SETUP", fg, bg);

    if (installer_page == INSTALLER_PAGE_WELCOME && !installer_active) {
      installer_text_draw_centered(body_x, body_y + body_h / 2 - 18, body_w,
                                   "WELCOME TO SETUP", fg, bg);
      installer_text_draw_centered(body_x, body_y + body_h / 2 + 10, body_w,
                                   "TO SETUP OS PRESS ENTER", fg, bg);
      installer_text_draw_centered(body_x, body_y + body_h / 2 + 38, body_w,
                                   "OR PRESS P TO RESTART YOUR COMPUTER", fg,
                                   bg);
      installer_text_draw_footer(content_x, content_y, content_w, content_h, "");
      return;
    }

    if (installer_active && installer_phase <= 2) {
      installer_text_draw_footer(content_x, content_y, content_w, content_h,
                                 "LOADING DISKS");
      return;
    }

    if (!installer_active && installer_page == INSTALLER_PAGE_TARGET) {
      char disk_label[48];
      int list_y = body_y + body_h / 2 - 20;
      installer_text_draw_centered(body_x, body_y + body_h / 2 - 92, body_w,
                                   "SELECT PARTITION TO INSTALL OS TO", fg, bg);
      for (int i = 0; i < installer_disk_count && i < 6; i++) {
        installer_text_build_disk_label(i, disk_label, sizeof(disk_label));
        installer_text_draw_centered(body_x, list_y + i * 30, body_w, disk_label,
                                     fg, bg);
      }
      installer_text_draw_footer(content_x, content_y, content_w, content_h, "");
      return;
    }

    if (installer_page == INSTALLER_PAGE_PROGRESS) {
      const char *line1 = "SETUP IS COPYING OS FILES";
      const char *line2 = "SETUP IS FORMATING...";
      const char *footer = installer_status;
      if (installer_progress_done >= 92) {
        line1 = "";
        line2 = "";
        footer = "SETUP IS SETTINGUP OS CONFIG FILES...";
      } else if (installer_progress_done >= 20) {
        line1 = "SETUP IS COPYING OS FILES";
        line2 = "TO THE OS INSTALLATING FOLDERS...";
        footer = "SETUP IS COPYING OS FILES TO YOUR COMUTER...";
      }

      if (line1[0])
        installer_text_draw_centered(body_x, body_y + 154, body_w, line1, fg, bg);
      if (line2[0])
        installer_text_draw_centered(body_x, body_y + 184, body_w, line2, fg, bg);
      if (installer_progress_done < 92) {
        installer_text_draw_progress_bar(bar_x, body_y + 250, bar_w, 64,
                                         progress_percent, bar_blue);
      }
      installer_text_draw_footer(content_x, content_y, content_w, content_h,
                                 footer);
      return;
    }

    if (installer_page == INSTALLER_PAGE_COMPLETE) {
      uint64_t now = arch_timer_get_ms();
      int countdown_percent = 100;
      if (installer_reboot_deadline_ms > now) {
        uint64_t total_ms = 3000;
        uint64_t remaining_ms = installer_reboot_deadline_ms - now;
        if (remaining_ms > total_ms)
          remaining_ms = total_ms;
        countdown_percent =
            (int)(((total_ms - remaining_ms) * 100) / total_ms);
      }
      installer_text_draw_centered(body_x, body_y + 154, body_w,
                                   "YOU NEED TO RESTART YOUR COMPUTER", fg, bg);
      installer_text_draw_centered(body_x, body_y + 184, body_w,
                                   "PRESS ANY KEY TO SPEED UP THE PROCESS", fg,
                                   bg);
      installer_text_draw_progress_bar(bar_x, body_y + 250, bar_w, 64,
                                       countdown_percent, bar_red);
      if (installer_reboot_deadline_ms > now) {
        installer_text_draw_footer(content_x, content_y, content_w, content_h,
                                   "");
      } else {
        installer_text_draw_footer(content_x, content_y, content_w, content_h,
                                   "RESTARTING YOUR COMPUTER");
      }
      return;
    }

    installer_text_draw_footer(content_x, content_y, content_w, content_h, "");
    return;
  }

  if (installer_page == INSTALLER_PAGE_PROGRESS ||
      installer_page == INSTALLER_PAGE_COMPLETE) {
    int copy_percent = installer_stage_value(18, 74);
    int expand_percent = installer_stage_value(74, 92);
    int configure_percent = installer_stage_value(92, 98);

    if (installer_page == INSTALLER_PAGE_COMPLETE) {
      copy_percent = 100;
      expand_percent = 100;
      configure_percent = 100;
    }
    draw_installation_stage_layout(content_x, content_y, content_w, content_h,
                                   copy_percent, expand_percent,
                                   configure_percent, 0);
    return;
  }

  {
    int panel_x = content_x + 20;
    int panel_y = content_y + 18;
    int panel_w = content_w - 40;
    int panel_h = content_h - 36;
    int rail_w = 180;
    int rail_x = panel_x;
    int rail_y = panel_y;
    int body_x = panel_x + rail_w + 18;
    int body_y = panel_y + 18;
    int body_w = panel_w - rail_w - 36;
    int footer_y = panel_y + panel_h - 54;
    int primary_x = body_x;
    int secondary_x = body_x + 154;
    int utility_x = body_x + body_w - 166;
    int button_w = 140;
    int button_h = 34;
    int progress_w = body_w - 36;
    int progress_percent = installer_progress_total > 0
                               ? (installer_progress_done * 100) /
                                     installer_progress_total
                               : 0;
    int fill_w = installer_progress_total > 0
                     ? (progress_w * installer_progress_done) /
                           installer_progress_total
                     : 0;
    char progress_label[64];
    const char *steps[] = {"Welcome", "Choose Disk", "Review", "Install", "Finish"};
    const char *selected_disk = installer_selected_disk_label();

    str_copy_safe(progress_label, "Progress ", sizeof(progress_label));
    {
      int idx = 0;
      while (progress_label[idx] && idx < (int)sizeof(progress_label) - 1)
        idx++;
      append_decimal(progress_label, &idx, progress_percent);
      installer_append_to_buf(progress_label, sizeof(progress_label), "%");
    }

    gui_draw_rect(panel_x, panel_y, panel_w, panel_h, theme->surface_alt);
    gui_draw_rect_outline(panel_x, panel_y, panel_w, panel_h, theme->border, 1);

    gui_draw_rect(rail_x, rail_y, rail_w, panel_h, theme->surface);
    gui_draw_string(rail_x + 20, rail_y + 24, "OS8 Installer", theme->app_fg,
                    theme->surface);
    gui_draw_string(rail_x + 20, rail_y + 48, "Dedicated setup pages", 0x93C5FD,
                    theme->surface);

    for (int i = 0; i < 5; i++) {
      int step_y = rail_y + 96 + i * 54;
      uint32_t bg = i == installer_page
                        ? theme->accent
                        : (i < installer_page ? 0x15324F : theme->surface_alt);
      char step_label[24] = "";
      int idx = 0;
      step_label[idx++] = '1' + i;
      step_label[idx++] = ' ';
      step_label[idx++] = ' ';
      for (int j = 0; steps[i][j] && idx < (int)sizeof(step_label) - 1; j++)
        step_label[idx++] = steps[i][j];
      step_label[idx] = '\0';
      gui_draw_rect(rail_x + 16, step_y, rail_w - 32, 40, bg);
      gui_draw_string(rail_x + 28, step_y + 13, step_label, 0xFFFFFF, bg);
    }

    gui_draw_string(rail_x + 20, rail_y + panel_h - 98, "Current Target",
                    0x93C5FD, 0x0F172A);
    gui_draw_string(rail_x + 20, rail_y + panel_h - 72, selected_disk, 0xE5E7EB,
                    theme->surface);
    gui_draw_string(rail_x + 20, rail_y + panel_h - 44, installer_progress_stage,
                    theme->app_muted, theme->surface);

    gui_draw_string(body_x, body_y, installer_page_title(), 0x93C5FD,
                    theme->surface_alt);

    if (installer_page == INSTALLER_PAGE_WELCOME) {
      gui_draw_string(body_x, body_y + 30, "Install OS8 in guided steps",
                      0xFFFFFF, 0x111827);
      gui_draw_string(body_x, body_y + 64,
                      "This installer now uses dedicated pages for each step.",
                      0xCBD5E1, 0x111827);
      gui_draw_string(body_x, body_y + 88,
                      "You will choose a disk, review what will happen,",
                      0xCBD5E1, 0x111827);
      gui_draw_string(body_x, body_y + 112,
                      "then start the extracted system image install.", 0xCBD5E1,
                      0x111827);

      gui_draw_rect(body_x, body_y + 156, body_w - 24, 122, 0x172033);
      gui_draw_string(body_x + 18, body_y + 178, "What this installer does",
                      0xFFFFFF, 0x172033);
      gui_draw_string(body_x + 18, body_y + 206,
                      "- overwrites the selected hard disk", 0xE5E7EB, 0x172033);
      gui_draw_string(body_x + 18, body_y + 228,
                      "- copies the extracted BIOS+UEFI system files", 0xE5E7EB,
                      0x172033);
      gui_draw_string(body_x + 18, body_y + 250,
                      "- prepares first-boot account setup", 0xE5E7EB, 0x172033);
    } else if (installer_page == INSTALLER_PAGE_TARGET) {
      gui_draw_string(body_x, body_y + 30, "Choose the install target disk",
                      0xFFFFFF, 0x111827);
      gui_draw_string(body_x, body_y + 58,
                      "Only real writable disks should be selected here.",
                      0xCBD5E1, 0x111827);

      for (int i = 0; i < installer_disk_count && i < 5; i++) {
        int row_y = body_y + 96 + i * 34;
        uint32_t row_bg = i == installer_selected_disk ? 0x334155 : 0x172033;
        gui_draw_rect(body_x, row_y, body_w - 18, 28, row_bg);
        gui_draw_string(body_x + 14, row_y + 8, installer_disk_labels[i], 0xFFFFFF,
                        row_bg);
      }

      gui_draw_rect(body_x, body_y + 286, body_w - 18, 64, 0x172033);
      gui_draw_string(body_x + 16, body_y + 306, "Selected disk", 0x93C5FD,
                      0x172033);
      gui_draw_string(body_x + 16, body_y + 330, selected_disk, 0xFFFFFF, 0x172033);
    } else if (installer_page == INSTALLER_PAGE_REVIEW) {
      gui_draw_string(body_x, body_y + 30, "Review the install plan",
                      0xFFFFFF, 0x111827);
      gui_draw_string(body_x, body_y + 58, "Target disk:", 0x93C5FD, 0x111827);
      gui_draw_string(body_x + 98, body_y + 58, selected_disk, 0xFFFFFF, 0x111827);
      gui_draw_string(body_x, body_y + 96, "Actions", 0x93C5FD, 0x111827);
      gui_draw_string(body_x + 16, body_y + 122,
                      "1. Copy the extracted system image to the target disk.",
                      0xE5E7EB, 0x111827);
      gui_draw_string(body_x + 16, body_y + 146,
                      "2. Preserve bootability for BIOS and UEFI startup.",
                      0xE5E7EB, 0x111827);
      gui_draw_string(body_x + 16, body_y + 170,
                      "3. Prepare a user data area for first boot.", 0xE5E7EB,
                      0x111827);
      gui_draw_string(body_x + 16, body_y + 194,
                      "4. Schedule reboot into the installed system.", 0xE5E7EB,
                      0x111827);
      gui_draw_rect(body_x, body_y + 236, body_w - 18, 64, 0x3A1D1D);
      gui_draw_string(body_x + 16, body_y + 256, "Warning", 0xFCA5A5, 0x3A1D1D);
      gui_draw_string(body_x + 16, body_y + 280,
                      "This step overwrites the selected target disk.",
                      0xFDE2E2, 0x3A1D1D);
      gui_draw_rect(body_x, body_y + 318, body_w - 18, 36, 0x172033);
      gui_draw_string(body_x + 16, body_y + 330, installer_status, 0xFFFFFF,
                      0x172033);
    } else if (installer_page == INSTALLER_PAGE_PROGRESS) {
      gui_draw_string(body_x, body_y + 30, "Installing the system image",
                      0xFFFFFF, 0x111827);
      gui_draw_string(body_x, body_y + 58, installer_progress_stage, 0x93C5FD,
                      0x111827);
      gui_draw_rect(body_x, body_y + 98, progress_w, 16, 0x172033);
      if (fill_w > 0)
        gui_draw_rect(body_x, body_y + 98, fill_w, 16, 0x22C55E);
      gui_draw_string(body_x, body_y + 124, progress_label, 0xA7F3D0, 0x111827);
      gui_draw_string(body_x, body_y + 148, installer_status, 0xFFFFFF, 0x111827);
      gui_draw_string(body_x, body_y + 174, installer_progress_detail, 0xCBD5E1,
                      0x111827);
      if (installer_progress_current_item[0]) {
        gui_draw_rect(body_x, body_y + 204, body_w - 18, 44, 0x172033);
        gui_draw_string(body_x + 16, body_y + 220, "Current file", 0x93C5FD,
                        0x172033);
        gui_draw_string(body_x + 16, body_y + 236,
                        installer_path_basename(installer_progress_current_item),
                        0xFFFFFF, 0x172033);
      }
      gui_draw_rect(body_x, body_y + 264, body_w - 18, 112, 0x172033);
      gui_draw_string(body_x + 16, body_y + 284, "Install log", 0x93C5FD,
                      0x172033);
      gui_draw_string(body_x + 16, body_y + 312, installer_log_buffer[0]
                                                     ? installer_log_buffer
                                                     : "Waiting for installer output...",
                      0xCBD5E1, 0x172033);
    } else {
      char countdown[96];
      int seconds = installer_reboot_seconds_remaining();
      gui_draw_rect(body_x, body_y + 26, body_w - 18, 76, 0x123B2A);
      gui_draw_string(body_x + 18, body_y + 46, "Installation Complete",
                      0xFFFFFF, 0x123B2A);
      gui_draw_string(body_x + 18, body_y + 72, installer_progress_detail,
                      0xD1FAE5, 0x123B2A);
      gui_draw_rect(body_x, body_y + 122, progress_w, 16, 0x172033);
      if (fill_w > 0)
        gui_draw_rect(body_x, body_y + 122, fill_w, 16, 0x22C55E);
      gui_draw_string(body_x, body_y + 148, progress_label, 0xA7F3D0, 0x111827);
      gui_draw_string(body_x, body_y + 178, "Next step", 0x93C5FD, 0x111827);
      gui_draw_string(body_x + 16, body_y + 204,
                      "Restart to boot from the installed disk.", 0xE5E7EB,
                      0x111827);
      gui_draw_string(body_x + 16, body_y + 228,
                      "Remove installer media if the machine boots setup again.",
                      0xE5E7EB, 0x111827);
      gui_draw_rect(body_x, body_y + 274, body_w - 18, 36, 0x172033);
      gui_draw_string(body_x + 16, body_y + 286, installer_status, 0xFFFFFF,
                      0x172033);
      str_copy_safe(countdown, "Automatic restart in ", sizeof(countdown));
      {
        int idx = 0;
        while (countdown[idx] && idx < (int)sizeof(countdown) - 1)
          idx++;
        append_decimal(countdown, &idx, seconds > 0 ? seconds : 0);
        installer_append_to_buf(countdown, sizeof(countdown), " seconds...");
      }
      gui_draw_string(body_x, body_y + 334, countdown, 0xA6E3A1, 0x111827);
    }

    gui_draw_rect(panel_x, footer_y, panel_w, 1, 0x334155);

    if (installer_page != INSTALLER_PAGE_PROGRESS &&
        installer_page != INSTALLER_PAGE_COMPLETE) {
      if (installer_page > INSTALLER_PAGE_WELCOME) {
        gui_draw_system_button(primary_x, footer_y + 12, button_w, button_h,
                               "Back", GUI_BUTTON_NEUTRAL, 1, 0);
      }

      if (installer_page == INSTALLER_PAGE_REVIEW) {
        gui_draw_system_button(secondary_x, footer_y + 12, button_w, button_h,
                               "Install System Image", GUI_BUTTON_SUCCESS,
                               installer_active ? 0 : 1, 0);
      } else {
        gui_draw_system_button(secondary_x, footer_y + 12, button_w, button_h,
                               "Next", GUI_BUTTON_PRIMARY, 1, 0);
      }
    }

    if (installer_page == INSTALLER_PAGE_TARGET ||
        installer_page == INSTALLER_PAGE_REVIEW) {
      gui_draw_system_button(utility_x, footer_y + 12, 150, button_h,
                             "Partition Manager", GUI_BUTTON_PRIMARY, 1, 0);
    }

    if (installer_page == INSTALLER_PAGE_COMPLETE) {
      gui_draw_system_button(primary_x, footer_y + 12, button_w, button_h,
                             "Restart Now", GUI_BUTTON_SUCCESS, 1, 0);
    }
  }
}

static void draw_startup_auth_window(struct window *win, int content_x,
                                     int content_y, int content_w,
                                     int content_h) {
  char masked_password[32];
  settings_account_list_t startup_accounts;
  int startup_dropdown_rows = 0;
  int startup_dropdown_start = 0;
  int startup_login_extra_y = 0;
  uint32_t user_bg = startup_active_field == 0 ? 0x31314A : 0x232337;
  uint32_t pass_bg = startup_active_field == 1 ? 0x31314A : 0x232337;
  const char *title =
      startup_flow == STARTUP_FLOW_SETUP_ACCOUNT ? "Setup Account"
                                                  : "Sign In";
  const char *button_label =
      startup_flow == STARTUP_FLOW_SETUP_ACCOUNT
          ? "Finish Setup"
          : (startup_account_system_ready() ? "Login" : "Starting...");

  (void)win;
  if (startup_flow == STARTUP_FLOW_COMPLETING_INSTALL) {
    int complete_percent = 0;
    uint64_t now = arch_timer_get_ms();
    if (startup_completion_started_ms && startup_completion_deadline_ms &&
        startup_completion_deadline_ms > startup_completion_started_ms) {
      uint64_t total =
          startup_completion_deadline_ms - startup_completion_started_ms;
      uint64_t elapsed =
          now > startup_completion_started_ms
              ? now - startup_completion_started_ms
              : 0;
      if (elapsed > total)
        elapsed = total;
      complete_percent = (int)((elapsed * 100) / total);
    }
    draw_installation_stage_layout(content_x, content_y, content_w, content_h,
                                   100, 100, 100, complete_percent);
    return;
  }

  settings_account_list_init(&startup_accounts);
  if (startup_setup_account_active()) {
    int panel_x = 0, panel_y = 0, panel_w = 0, panel_h = 0;
    int rail_w = 0, card_x = 0, card_y = 0, card_w = 0, card_h = 0;
    int button_x = 0, button_y = 0, button_w = 0, button_h = 0;
    int user_x = 0, user_y = 0, user_w = 0, user_h = 0;
    int pass_x = 0, pass_y = 0, pass_w = 0, pass_h = 0;
    const char *step_title = startup_setup_welcome_active()
                                 ? "Welcome"
                                 : startup_setup_storage_active()
                                       ? "Create Storage"
                                       : "Create Account";
    const char *headline = startup_setup_welcome_active()
                               ? "Set up your OS8 account"
                               : startup_setup_storage_active()
                                     ? "Finish the storage setup"
                                     : "Create the owner account";
    const char *body_1 = startup_setup_welcome_active()
                             ? "This machine is not ready for sign-in yet."
                             : startup_setup_storage_active()
                                   ? "A private data area will be prepared for"
                                   : "Choose the username and password that";
    const char *body_2 = startup_setup_welcome_active()
                             ? "Setup runs in a locked screen with no extra"
                             : startup_setup_storage_active()
                                   ? account_username
                                   : "will be used every time this system boots.";
    const char *body_3 = startup_setup_welcome_active()
                             ? "windows, apps, or desktop menus available."
                             : startup_setup_storage_active()
                                   ? "The account partition keeps user data"
                                   : "The dock and background apps stay off";
    const char *body_4 = startup_setup_welcome_active()
                             ? "Continue to begin account creation."
                             : startup_setup_storage_active()
                                   ? "isolated on the HDD for later boots."
                                   : "until setup is finished.";
    const char *button_text = startup_setup_welcome_active()
                                  ? "Start Setup"
                                  : startup_setup_storage_active()
                                        ? "Create Storage"
                                        : "Continue";

    startup_get_setup_layout(content_x, content_y, content_w, content_h,
                             &panel_x, &panel_y, &panel_w, &panel_h, &rail_w,
                             &card_x, &card_y, &card_w, &card_h);
    startup_get_setup_button_rect(content_x, content_y, content_w, content_h,
                                  &button_x, &button_y, &button_w, &button_h);
    startup_get_setup_field_rect(content_x, content_y, content_w, content_h, 0,
                                 &user_x, &user_y, &user_w, &user_h);
    startup_get_setup_field_rect(content_x, content_y, content_w, content_h, 1,
                                 &pass_x, &pass_y, &pass_w, &pass_h);
    mask_secret(startup_input_password, masked_password, sizeof(masked_password));

    gui_draw_rect(content_x, content_y, content_w, content_h, 0x0B1020);
    gui_fill_rect_alpha(panel_x, panel_y, panel_w, panel_h, 0x10203CC8);
    gui_draw_rect_outline(panel_x, panel_y, panel_w, panel_h, 0x3B82F6, 1);

    gui_fill_rect_alpha(panel_x, panel_y, rail_w, panel_h, 0x0C1730CC);
    gui_draw_rect(panel_x + 24, panel_y + 28, 56, 56, 0x2563EB);
    gui_draw_string(panel_x + 100, panel_y + 36, "OS8 Setup", 0xFFFFFF,
                    0x0C1730);
    gui_draw_string(panel_x + 100, panel_y + 58, "Locked setup workspace",
                    0xBFDBFE, 0x0C1730);

    gui_draw_string(panel_x + 24, panel_y + 118, "Progress", 0x93C5FD,
                    0x0C1730);
    gui_draw_rect(panel_x + 24, panel_y + 146, rail_w - 48, 44,
                  startup_setup_welcome_active() ? 0x1D4ED8 : 0x15233D);
    gui_draw_string(panel_x + 40, panel_y + 160, "1  Welcome", 0xFFFFFF,
                    startup_setup_welcome_active() ? 0x1D4ED8 : 0x15233D);
    gui_draw_rect(panel_x + 24, panel_y + 198, rail_w - 48, 44,
                  startup_setup_account_form_active() ? 0x1D4ED8 : 0x15233D);
    gui_draw_string(panel_x + 40, panel_y + 212, "2  Account", 0xFFFFFF,
                    startup_setup_account_form_active() ? 0x1D4ED8 : 0x15233D);
    gui_draw_rect(panel_x + 24, panel_y + 250, rail_w - 48, 44,
                  startup_setup_storage_active() ? 0x1D4ED8 : 0x15233D);
    gui_draw_string(panel_x + 40, panel_y + 264, "3  Storage", 0xFFFFFF,
                    startup_setup_storage_active() ? 0x1D4ED8 : 0x15233D);

    gui_draw_string(panel_x + 24, panel_y + panel_h - 118, "Restrictions",
                    0x93C5FD, 0x0C1730);
    gui_draw_string(panel_x + 24, panel_y + panel_h - 90,
                    "Only this setup window is available.", 0xE2E8F0,
                    0x0C1730);
    gui_draw_string(panel_x + 24, panel_y + panel_h - 64,
                    "Right-click desktop menus stay disabled.", 0xE2E8F0,
                    0x0C1730);
    gui_draw_string(panel_x + 24, panel_y + panel_h - 38,
                    "Changes are saved to disk for later boots.", 0xE2E8F0,
                    0x0C1730);

    gui_fill_rect_alpha(card_x, card_y, card_w, card_h, 0x111827E4);
    gui_draw_rect_outline(card_x, card_y, card_w, card_h, 0x334155, 1);
    gui_draw_string(card_x + 36, card_y + 34, step_title, 0x93C5FD, 0x111827);
    gui_draw_string(card_x + 36, card_y + 64, headline, 0xFFFFFF, 0x111827);
    gui_draw_string(card_x + 36, card_y + 98, body_1, 0xCBD5E1, 0x111827);
    gui_draw_string(card_x + 36, card_y + 122, body_2, 0xFFFFFF, 0x111827);
    gui_draw_string(card_x + 36, card_y + 146, body_3, 0xCBD5E1, 0x111827);
    gui_draw_string(card_x + 36, card_y + 170, body_4, 0xCBD5E1, 0x111827);

    if (startup_setup_account_form_active()) {
      gui_draw_string(user_x, user_y - 22, "Username", 0x93C5FD, 0x111827);
      gui_draw_rect(user_x, user_y, user_w, user_h, user_bg);
      gui_draw_string(user_x + 12, user_y + 14,
                      startup_input_username[0] ? startup_input_username
                                                : "enter username",
                      startup_input_username[0] ? 0xFFFFFF : 0x64748B, user_bg);

      gui_draw_string(pass_x, pass_y - 22, "Password", 0x93C5FD, 0x111827);
      gui_draw_rect(pass_x, pass_y, pass_w, pass_h, pass_bg);
      gui_draw_string(pass_x + 12, pass_y + 14,
                      masked_password[0] ? masked_password : "enter password",
                      masked_password[0] ? 0xFFFFFF : 0x64748B, pass_bg);
    }

    gui_draw_system_button(button_x, button_y, button_w, button_h, button_text,
                           GUI_BUTTON_PRIMARY, 1, 0);
    gui_draw_string(button_x + button_w + 18, button_y + 14, startup_status,
                    0xCBD5E1, 0x111827);
    settings_account_list_free(&startup_accounts);
    return;
  }

  if (startup_setup_welcome_active()) {
    gui_draw_rect(content_x, content_y, content_w, 56, 0x181827);
    gui_draw_string(content_x + 20, content_y + 18, "Welcome to OS8",
                    0xFFFFFF, 0x181827);
    gui_draw_string(content_x + 20, content_y + 78,
                    "This setup will create your account before login.",
                    0xCDD6F4, THEME_BG);
    gui_draw_string(content_x + 20, content_y + 104,
                    "No other apps will open until setup is done.", 0xA6ADC8,
                    THEME_BG);
    gui_draw_string(content_x + 20, content_y + 130,
                    "Press Enter or click Continue to start.", 0xA6ADC8,
                    THEME_BG);
    gui_draw_rect(content_x + 20, content_y + 198, 170, 34, 0x2563EB);
    gui_draw_string(content_x + 56, content_y + 210, "Continue", 0xFFFFFF,
                    0x2563EB);
    gui_draw_string(content_x + 210, content_y + 209, startup_status, 0xCDD6F4,
                    THEME_BG);
    settings_account_list_free(&startup_accounts);
    return;
  }

  if (startup_setup_storage_active()) {
    gui_draw_rect(content_x, content_y, content_w, 56, 0x181827);
    gui_draw_string(content_x + 20, content_y + 18, "Prepare Account Storage",
                    0xFFFFFF, 0x181827);
    gui_draw_string(content_x + 20, content_y + 78,
                    "First boot will create a personal data partition for",
                    0xCDD6F4, THEME_BG);
    gui_draw_string(content_x + 20, content_y + 102, account_username,
                    0xFFFFFF, THEME_BG);
    gui_draw_string(content_x + 20, content_y + 128,
                    "This keeps each account separated on the HDD.", 0xA6ADC8,
                    THEME_BG);
    gui_draw_string(content_x + 20, content_y + 154,
                    "Press Enter or click Continue to provision storage.",
                    0xA6ADC8, THEME_BG);
    gui_draw_rect(content_x + 20, content_y + 204, 170, 34, 0x2563EB);
    gui_draw_string(content_x + 56, content_y + 216, "Continue", 0xFFFFFF,
                    0x2563EB);
    gui_draw_string(content_x + 210, content_y + 215, startup_status, 0xCDD6F4,
                    THEME_BG);
    settings_account_list_free(&startup_accounts);
    return;
  }

  mask_secret(startup_input_password, masked_password, sizeof(masked_password));
  settings_collect_accounts(&startup_accounts);
  if (startup_flow == STARTUP_FLOW_LOGIN && !startup_input_username[0] &&
      startup_accounts.count > 0) {
    str_copy_safe(startup_input_username, startup_accounts.names[0],
                  sizeof(startup_input_username));
  }
  if (startup_flow == STARTUP_FLOW_LOGIN && startup_login_user_dropdown_open &&
      startup_accounts.count > 0) {
    startup_dropdown_rows = startup_accounts.count < 4 ? startup_accounts.count : 4;
    startup_dropdown_start = settings_account_list_visible_start(
        &startup_accounts, startup_input_username, startup_dropdown_rows);
    startup_login_extra_y = 8 + startup_dropdown_rows * 22;
  }

  gui_draw_rect(content_x, content_y, content_w, 56, 0x181827);
  gui_draw_string(content_x + 20, content_y + 18, title, 0xFFFFFF, 0x181827);

  gui_draw_string(content_x + 20, content_y + 60,
                  startup_setup_account_active()
                      ? "Setup account username"
                      : "Username",
                  0xA6ADC8,
                  THEME_BG);
  gui_draw_rect(content_x + 20, content_y + 80, content_w - 40, 34, user_bg);
  gui_draw_string(content_x + 30, content_y + 92,
                  startup_input_username[0] ? startup_input_username
                                            : "enter username",
                  startup_input_username[0] ? 0xFFFFFF : 0x6C7086, user_bg);
  if (startup_flow == STARTUP_FLOW_LOGIN) {
    gui_draw_string(content_x + content_w - 58, content_y + 92, "v",
                    0xCBD5E1, user_bg);
    if (startup_dropdown_rows > 0) {
      int dropdown_h = 8 + startup_dropdown_rows * 22;
      gui_draw_rect(content_x + 20, content_y + 118, content_w - 40, dropdown_h,
                    0x1E293B);
      for (int i = 0; i < startup_dropdown_rows; i++) {
        int account_idx = startup_dropdown_start + i;
        int row_y = content_y + 122 + i * 22;
        uint32_t row_bg =
            str_cmp(startup_input_username, startup_accounts.names[account_idx]) == 0
                ? 0x2563EB
                : 0x1E293B;
        gui_draw_rect(content_x + 24, row_y, content_w - 48, 18, row_bg);
        gui_draw_string(content_x + 34, row_y + 5,
                        startup_accounts.names[account_idx], 0xFFFFFF, row_bg);
      }
    }
  }

  gui_draw_string(content_x + 20, content_y + 128,
                  startup_setup_account_active()
                      ? "Setup account password"
                      : "Password",
                  0xA6ADC8,
                  THEME_BG);
  gui_draw_rect(content_x + 20, content_y + 148 + startup_login_extra_y,
                content_w - 40, 34, pass_bg);
  gui_draw_string(content_x + 30, content_y + 160 + startup_login_extra_y,
                  masked_password[0] ? masked_password : "enter password",
                  masked_password[0] ? 0xFFFFFF : 0x6C7086, pass_bg);

  gui_draw_system_button(
      content_x + 20, content_y + 204 + startup_login_extra_y, 170, 34,
      button_label, GUI_BUTTON_PRIMARY,
      (!startup_setup_account_active() && !startup_account_system_ready()) ||
              (startup_flow == STARTUP_FLOW_LOGIN && gui_activation_requires_sign_in())
          ? 0
          : 1,
      0);
  if (startup_flow == STARTUP_FLOW_LOGIN) {
    gui_draw_system_button(content_x + 200, content_y + 204 + startup_login_extra_y,
                           96, 34, "Restart", GUI_BUTTON_NEUTRAL, 1, 0);
    gui_draw_system_button(content_x + 306, content_y + 204 + startup_login_extra_y,
                           108, 34, "Shut Down", GUI_BUTTON_DANGER, 1, 0);
    gui_draw_string(content_x + 20, content_y + 252 + startup_login_extra_y,
                    "Power options are available before sign-in.",
                    0x94A3B8, THEME_BG);
    gui_draw_string(content_x + 20, content_y + 272 + startup_login_extra_y,
                    gui_activation_detail_text(), 0xCBD5E1, THEME_BG);
  }
  gui_draw_string(content_x + 210, content_y + 215 + startup_login_extra_y,
                  startup_status, 0xCDD6F4, THEME_BG);
  settings_account_list_free(&startup_accounts);
}

static void draw_icon(int x, int y, int size, const unsigned char *icon,
                      uint32_t fg_color, uint32_t bg_color);

struct fm_state {
  char path[256];
  char selected[256];
  int scroll_y;
  int context_menu_visible;
  int context_menu_x;
  int context_menu_y;
  int context_menu_target_type;
  int context_menu_target_on_item;
  char context_menu_target[256];
};

#define FM_MAX_ITEMS 96

struct fm_item {
  char name[64];
  unsigned type;
  uint64_t size_bytes;
};

struct fm_collect_ctx {
  struct fm_item *items;
  int count;
  int max_items;
};

static int fm_name_length(const char *name) {
  int len = 0;
  while (name && name[len])
    len++;
  return len;
}

static void fm_set_window_title(struct window *win, const char *path) {
  if (!win)
    return;

  str_copy_safe(win->title, "File Manager", sizeof(win->title));
  if (!path || !path[0])
    return;

  int idx = 12;
  if (idx < (int)sizeof(win->title) - 1) {
    win->title[idx++] = ' ';
    win->title[idx++] = '-';
    win->title[idx++] = ' ';
  }

  for (int i = 0; path[i] && idx < (int)sizeof(win->title) - 1; i++)
    win->title[idx++] = path[i];
  win->title[idx] = '\0';
}

static void fm_join_path(const char *base, const char *name, char *out,
                         int out_max) {
  int idx = 0;
  if (!out || out_max <= 0)
    return;
  out[0] = '\0';

  if (base) {
    while (base[idx] && idx < out_max - 1) {
      out[idx] = base[idx];
      idx++;
    }
  }

  if (idx == 0) {
    out[idx++] = '/';
  } else if (out[idx - 1] != '/' && idx < out_max - 1) {
    out[idx++] = '/';
  }

  for (int i = 0; name && name[i] && idx < out_max - 1; i++)
    out[idx++] = name[i];
  out[idx] = '\0';
}

static int fm_path_exists(const char *path) {
  char resolved_path[256];
  const char *open_path =
      resolve_user_storage_path(path, resolved_path, sizeof(resolved_path));
  struct file *f = vfs_open(open_path, O_RDONLY, 0);
  if (!f)
    return 0;
  vfs_close(f);
  return 1;
}

static void fm_build_unique_child_path(const char *dir_path, const char *base,
                                       const char *ext, char *out,
                                       int out_max) {
  char candidate[512];
  char name[96];

  for (int attempt = 0; attempt < 32; attempt++) {
    int idx = 0;
    const char *prefix = base ? base : "New Item";
    for (int i = 0; prefix[i] && idx < (int)sizeof(name) - 1; i++)
      name[idx++] = prefix[i];

    if (attempt > 0 && idx < (int)sizeof(name) - 3) {
      name[idx++] = ' ';
      if (attempt >= 10) {
        name[idx++] = (char)('0' + (attempt / 10));
      }
      name[idx++] = (char)('0' + (attempt % 10));
    }

    for (int i = 0; ext && ext[i] && idx < (int)sizeof(name) - 1; i++)
      name[idx++] = ext[i];
    name[idx] = '\0';

    fm_join_path(dir_path, name, candidate, sizeof(candidate));
    if (!fm_path_exists(candidate)) {
      str_copy_safe(out, candidate, out_max);
      return;
    }
  }

  fm_join_path(dir_path, "New Item", out, out_max);
}

static void fm_navigate_to(struct window *win, struct fm_state *st,
                           const char *path) {
  if (!st)
    return;
  if (!path || !path[0]) {
    st->path[0] = '/';
    st->path[1] = '\0';
  } else {
    str_copy_safe(st->path, path, sizeof(st->path));
  }
  st->selected[0] = '\0';
  st->scroll_y = 0;
  st->context_menu_visible = 0;
  fm_set_window_title(win, st->path);
  gui_invalidate_window(win);
}

static void fm_go_parent(struct window *win, struct fm_state *st) {
  int len = 0;
  if (!st)
    return;
  while (st->path[len])
    len++;
  if (len <= 1)
    return;

  while (len > 0 && st->path[len - 1] != '/')
    len--;
  if (len > 1)
    len--;
  st->path[len] = '\0';
  if (len == 0) {
    st->path[0] = '/';
    st->path[1] = '\0';
  }
  st->selected[0] = '\0';
  st->scroll_y = 0;
  st->context_menu_visible = 0;
  fm_set_window_title(win, st->path);
  gui_invalidate_window(win);
}

static void fm_hide_context_menu(struct fm_state *st) {
  if (!st)
    return;
  st->context_menu_visible = 0;
  st->context_menu_target_on_item = 0;
  st->context_menu_target_type = 0;
  st->context_menu_target[0] = '\0';
}

static int fm_collect_callback(void *ctx, const char *name, int len, loff_t off,
                               ino_t ino, unsigned type) {
  (void)off;
  (void)ino;
  struct fm_collect_ctx *collect = (struct fm_collect_ctx *)ctx;

  if (!collect || !name)
    return 0;
  if ((len == 1 && name[0] == '.') ||
      (len == 2 && name[0] == '.' && name[1] == '.'))
    return 0;
  if (collect->count >= collect->max_items)
    return 1;

  int insert_at = collect->count;
  if (type == 4) {
    insert_at = 0;
    while (insert_at < collect->count && collect->items[insert_at].type == 4)
      insert_at++;
    for (int i = collect->count; i > insert_at; i--)
      collect->items[i] = collect->items[i - 1];
  }

  int copy_len = len;
  if (copy_len >= (int)sizeof(collect->items[insert_at].name))
    copy_len = (int)sizeof(collect->items[insert_at].name) - 1;
  for (int i = 0; i < copy_len; i++)
    collect->items[insert_at].name[i] = name[i];
  collect->items[insert_at].name[copy_len] = '\0';
  collect->items[insert_at].type = type;
  collect->items[insert_at].size_bytes = 0;
  collect->count++;
  return 0;
}

static uint64_t fm_item_size_bytes(const char *dir_path, const char *name,
                                   unsigned type) {
  char full_path[512];
  char resolved_path[512];
  const char *open_path;
  struct file *f;
  uint64_t size = 0;

  if (!dir_path || !name || type == 4)
    return 0;

  fm_join_path(dir_path, name, full_path, sizeof(full_path));
  open_path =
      resolve_user_storage_path(full_path, resolved_path, sizeof(resolved_path));
  f = vfs_open(open_path, O_RDONLY, 0);
  if (!f)
    return 0;

  if (f->f_dentry && f->f_dentry->d_inode && f->f_dentry->d_inode->i_size > 0)
    size = (uint64_t)f->f_dentry->d_inode->i_size;
  vfs_close(f);
  return size;
}

static int fm_collect_items(const char *path, struct fm_item *items,
                            int max_items) {
  char resolved_path[256];
  const char *open_path =
      resolve_user_storage_path(path, resolved_path, sizeof(resolved_path));
  struct file *dir = vfs_open(open_path, O_RDONLY, 0);
  if (!dir)
    return -1;

  struct fm_collect_ctx ctx;
  ctx.items = items;
  ctx.count = 0;
  ctx.max_items = max_items;
  vfs_readdir(dir, &ctx, fm_collect_callback);
  vfs_close(dir);
  for (int i = 0; i < ctx.count; i++) {
    items[i].size_bytes = fm_item_size_bytes(path, items[i].name, items[i].type);
  }
  return ctx.count;
}

static void fm_open_item(struct window *win, struct fm_state *st, const char *name,
                         unsigned type) {
  char full_path[512];

  if (!win || !st || !name || !name[0])
    return;

  fm_join_path(st->path, name, full_path, sizeof(full_path));

  if (type == 4) {
    fm_navigate_to(win, st, full_path);
    return;
  }

  if (str_ends_with_ci(name, ".txt") || str_ends_with_ci(name, ".log")) {
    gui_open_notepad(full_path);
  } else if (str_ends_with_ci(name, ".app")) {
    char app_id[32];
    if (load_app_id_from_manifest_file(full_path, name, app_id, sizeof(app_id)) ==
        0) {
      gui_launch_app_by_id(app_id);
    }
  } else if (path_is_supported_image_file(name)) {
    gui_open_image_viewer(full_path);
  } else if (str_ends_with_ci(name, ".mp3")) {
    gui_play_mp3_file(full_path);
  } else if (str_ends_with_ci(name, ".py") || str_ends_with_ci(name, ".nano")) {
    extern void term_set_active(struct terminal * term);
    extern void term_puts(struct terminal * term, const char *str);
    extern void term_execute_command(struct terminal * term, const char *cmd);
    extern void term_set_content_pos(struct terminal * t, int x, int y);
    static int term_spawn_x = 120;
    static int term_spawn_y = 100;

    struct window *term_win =
        gui_create_window("Terminal", term_spawn_x, term_spawn_y, 500, 350);
    if (term_win) {
      int content_x = 0;
      int content_y = 0;
      gui_get_window_content_rect(term_win, &content_x, &content_y, NULL, NULL);
      struct terminal *term = term_create(content_x, content_y, 60, 18);
      if (term) {
        term_win->userdata = term;
        term_set_active(term);
        term_set_content_pos(term, content_x, content_y);

        char run_cmd[300] = "run ";
        int j = 4;
        for (int i = 0; full_path[i] && j < 298; i++)
          run_cmd[j++] = full_path[i];
        run_cmd[j] = '\0';
        term_execute_command(term, run_cmd);
        term_puts(term, "\n\033[32mos\033[0m:\033[34m~\033[0m$ ");
      }
    }

    term_spawn_x = (term_spawn_x + 40) % 300 + 80;
    term_spawn_y = (term_spawn_y + 35) % 200 + 70;
  }
}

static void fm_delete_context_target(struct fm_state *st) {
  char full_path[512];
  int ret;

  if (!st || !st->context_menu_target_on_item || !st->context_menu_target[0])
    return;

  fm_join_path(st->path, st->context_menu_target, full_path, sizeof(full_path));
  if (st->context_menu_target_type == 4) {
    ret = user_storage_rmdir(full_path);
  } else {
    ret = user_storage_unlink(full_path);
  }

  if (ret == 0 && str_cmp(st->selected, st->context_menu_target) == 0)
    st->selected[0] = '\0';
}

static const unsigned char *fm_icon_for_item(const char *name, unsigned type,
                                             uint32_t *color_out) {
  uint32_t color = 0xD1D5DB;
  const unsigned char *bmp = icon_notepad;

  if (type == 4) {
    bmp = icon_files;
    color = 0x60A5FA;
  } else if (str_ends_with_ci(name, ".app")) {
    bmp = icon_files;
    color = 0xC4B5FD;
  } else if (str_ends_with_ci(name, ".py")) {
    bmp = icon_python;
    color = 0xFACC15;
  } else if (str_ends_with_ci(name, ".nano")) {
    bmp = icon_nano;
    color = 0x4ADE80;
  } else if (path_is_supported_image_file(name)) {
    color = 0xF9E2AF;
  } else if (str_ends_with_ci(name, ".mp3")) {
    color = 0x86EFAC;
  }

  if (color_out)
    *color_out = color;
  return bmp;
}

static const char *fm_type_label(const char *name, unsigned type) {
  if (type == 4)
    return "Folder";
  if (str_ends_with_ci(name, ".app"))
    return "App";
  if (str_ends_with_ci(name, ".txt") || str_ends_with_ci(name, ".log"))
    return "Text";
  if (str_ends_with_ci(name, ".py"))
    return "Python";
  if (str_ends_with_ci(name, ".nano"))
    return "NanoLang";
  if (path_is_supported_image_file(name))
    return "Image";
  if (str_ends_with_ci(name, ".mp3"))
    return "Audio";
  return "File";
}

static void fm_truncate_label(const char *src, char *dst, int max_chars) {
  int len = fm_name_length(src);
  if (!dst || max_chars <= 0)
    return;
  if (!src) {
    dst[0] = '\0';
    return;
  }

  if (len <= max_chars) {
    str_copy_safe(dst, src, max_chars + 1);
    return;
  }

  int keep = max_chars - 3;
  if (keep < 1)
    keep = 1;
  int i = 0;
  for (; i < keep && src[i]; i++)
    dst[i] = src[i];
  dst[i++] = '.';
  dst[i++] = '.';
  dst[i++] = '.';
  dst[i] = '\0';
}

static void fm_truncate_label_px(const char *src, char *dst, int dst_max,
                                 int max_px) {
  int max_chars;

  if (!dst || dst_max <= 0) {
    return;
  }
  dst[0] = '\0';
  if (!src) {
    return;
  }

  if (max_px <= 0) {
    str_copy_safe(dst, "...", dst_max);
    return;
  }

  max_chars = max_px / 8;
  if (max_chars <= 0) {
    str_copy_safe(dst, "...", dst_max);
    return;
  }

  fm_truncate_label(src, dst, max_chars);
}

static void fm_format_size(uint64_t size_bytes, unsigned type, char *dst,
                           int dst_max) {
  const char *suffix = " B";
  uint64_t whole = size_bytes;
  uint64_t tenths = 0;

  if (!dst || dst_max <= 0)
    return;
  dst[0] = '\0';

  if (type == 4) {
    str_copy_safe(dst, "--", dst_max);
    return;
  }

  if (size_bytes >= (uint64_t)1024 * 1024 * 1024) {
    suffix = " GB";
    whole = size_bytes / ((uint64_t)1024 * 1024 * 1024);
    tenths = (size_bytes % ((uint64_t)1024 * 1024 * 1024)) * 10 /
             ((uint64_t)1024 * 1024 * 1024);
  } else if (size_bytes >= (uint64_t)1024 * 1024) {
    suffix = " MB";
    whole = size_bytes / ((uint64_t)1024 * 1024);
    tenths = (size_bytes % ((uint64_t)1024 * 1024)) * 10 /
             ((uint64_t)1024 * 1024);
  } else if (size_bytes >= 1024) {
    suffix = " KB";
    whole = size_bytes / 1024;
    tenths = (size_bytes % 1024) * 10 / 1024;
  }

  if (suffix[1] == 'B' && suffix[0] == ' ') {
    char digits[32];
    int count = 0;
    int out = 0;
    if (whole == 0) {
      digits[count++] = '0';
    } else {
      while (whole > 0 && count < (int)sizeof(digits)) {
        digits[count++] = (char)('0' + (whole % 10));
        whole /= 10;
      }
    }
    while (count > 0 && out < dst_max - 1)
      dst[out++] = digits[--count];
    if (out < dst_max - 1)
      dst[out++] = ' ';
    if (out < dst_max - 1)
      dst[out++] = 'B';
    dst[out] = '\0';
    return;
  }

  {
    char digits[32];
    int count = 0;
    int out = 0;
    if (whole == 0) {
      digits[count++] = '0';
    } else {
      while (whole > 0 && count < (int)sizeof(digits)) {
        digits[count++] = (char)('0' + (whole % 10));
        whole /= 10;
      }
    }
    while (count > 0 && out < dst_max - 1)
      dst[out++] = digits[--count];
    if (tenths > 0 && out < dst_max - 3) {
      dst[out++] = '.';
      dst[out++] = (char)('0' + tenths);
    }
    for (int i = 0; suffix[i] && out < dst_max - 1; i++)
      dst[out++] = suffix[i];
    dst[out] = '\0';
  }
}

static void installer_refresh_disk_inventory(void) {
  extern int storage_get_disk_count(void);
  extern int storage_get_disk_kind(int index);
  extern int storage_describe_disk(int index, char *buf, int max);
  int count = storage_get_disk_count();

  installer_disk_count = 0;
  for (int i = 0; i < count && installer_disk_count < 8; i++) {
    int kind = storage_get_disk_kind(i);
    if (kind == STORAGE_KIND_CDROM || kind == STORAGE_KIND_USB_MASS_STORAGE)
      continue;
    if (storage_describe_disk(i, installer_disk_labels[installer_disk_count],
                              sizeof(installer_disk_labels[0])) == 0) {
      installer_disk_indices[installer_disk_count] = i;
      installer_disk_count++;
    }
  }

  if (installer_disk_count == 0) {
    str_copy_safe(installer_disk_labels[0], "No real disks detected",
                  sizeof(installer_disk_labels[0]));
    installer_selected_disk = 0;
    return;
  }

  if (installer_selected_disk >= installer_disk_count)
    installer_selected_disk = installer_disk_count - 1;
  if (installer_selected_disk < 0)
    installer_selected_disk = 0;
}

static const char *installer_selected_disk_label(void) {
  installer_refresh_disk_inventory();
  if (installer_disk_count == 0)
    return "No real disks detected";
  if (installer_selected_disk < 0 || installer_selected_disk >= installer_disk_count)
    return "No real disks detected";
  return installer_disk_labels[installer_selected_disk];
}

static int installer_write_target_config(void) {
  char manifest[256];
  char target_root[96];
  char target_cfg[128];
  char disk_location[32];
  int selected_disk_index;
  int idx = 0;
  const char *disk = installer_selected_disk_label();
  int partition_count = 0;

  extern int storage_get_disk_location(int index, char *buf, int max);

  selected_disk_index = installer_selected_disk_index();
  if (selected_disk_index < 0)
    return -1;

  for (const char *p = "disk="; *p && idx < (int)sizeof(manifest) - 1; p++)
    manifest[idx++] = *p;
  for (int i = 0; disk[i] && idx < (int)sizeof(manifest) - 2; i++)
    manifest[idx++] = disk[i];
  manifest[idx++] = '\n';
  disk_location[0] = '\0';
  if (storage_get_disk_location(selected_disk_index, disk_location,
                                sizeof(disk_location)) == 0 &&
      disk_location[0]) {
    for (const char *p = "disk_location=";
         *p && idx < (int)sizeof(manifest) - 1; p++)
      manifest[idx++] = *p;
    for (int i = 0; disk_location[i] && idx < (int)sizeof(manifest) - 2; i++)
      manifest[idx++] = disk_location[i];
    manifest[idx++] = '\n';
  }
  {
    extern int storage_get_partition_count(int disk_index);
    extern int storage_has_efi_partition(int disk_index);
    partition_count = storage_get_partition_count(selected_disk_index);
    for (const char *p = "partitions=";
         *p && idx < (int)sizeof(manifest) - 1; p++)
      manifest[idx++] = *p;
    append_decimal(manifest, &idx, partition_count);
    manifest[idx++] = '\n';
    for (const char *p = "efi="; *p && idx < (int)sizeof(manifest) - 1; p++)
      manifest[idx++] = *p;
    append_decimal(manifest, &idx,
                   storage_has_efi_partition(selected_disk_index) ? 1 : 0);
    manifest[idx++] = '\n';
  }
  manifest[idx] = '\0';

  if (write_text_file("/System/install-target.cfg", manifest) != 0)
    return -1;
  installer_target_root_path(target_root, sizeof(target_root));
  str_copy_safe(target_cfg, target_root, sizeof(target_cfg));
  idx = 0;
  while (target_cfg[idx] && idx < (int)sizeof(target_cfg) - 1)
    idx++;
  if (idx < (int)sizeof(target_cfg) - 1)
    target_cfg[idx++] = '/';
  for (const char *p = "install-target.cfg";
       *p && idx < (int)sizeof(target_cfg) - 1; p++)
    target_cfg[idx++] = *p;
  target_cfg[idx] = '\0';
  return installer_write_target_text_file(target_cfg, manifest);
}

static void partition_manager_refresh_partitions(void) {
  extern int storage_get_partition_count(int disk_index);
  extern int storage_describe_partition(int disk_index, int partition_index,
                                        char *buf, int max);
  int selected_disk_index = installer_selected_disk_index();

  partition_manager_partition_count = 0;
  for (int i = 0; selected_disk_index >= 0 &&
                  i < storage_get_partition_count(selected_disk_index) &&
                  partition_manager_partition_count < 8;
       i++) {
    if (storage_describe_partition(selected_disk_index, i,
                                   partition_manager_labels
                                       [partition_manager_partition_count],
                                   sizeof(partition_manager_labels[0])) == 0) {
      partition_manager_partition_count++;
    }
  }
  if (partition_manager_partition_count == 0) {
    str_copy_safe(partition_manager_labels[0], "No partitions on selected disk",
                  sizeof(partition_manager_labels[0]));
    partition_manager_selected_partition = 0;
    return;
  }
  if (partition_manager_selected_partition >= partition_manager_partition_count)
    partition_manager_selected_partition = partition_manager_partition_count - 1;
  if (partition_manager_selected_partition < 0)
    partition_manager_selected_partition = 0;
}

static void disk_imager_set_status(const char *msg) {
  str_copy_safe(disk_imager_status, msg ? msg : "", sizeof(disk_imager_status));
}

static int disk_imager_capacity_sectors(int disk_index, uint32_t *out_sectors) {
  uint64_t sectors;

  if (!out_sectors)
    return -1;
  if (disk_index < 0)
    return -1;

  sectors = (uint64_t)storage_get_disk_capacity_mib(disk_index) * 2048ULL;
  if (sectors == 0 || sectors > 0xFFFFFFFFULL)
    return -1;

  *out_sectors = (uint32_t)sectors;
  return 0;
}

static int disk_imager_image_sector_count(const char *path, loff_t *out_file_size,
                                          uint32_t *out_sector_count) {
  struct file *file;
  loff_t file_size;
  uint64_t sectors;

  if (!path || !path[0] || !out_file_size || !out_sector_count)
    return -1;

  file = vfs_open(path, O_RDONLY, 0);
  if (!file)
    return -1;

  file_size = vfs_lseek(file, 0, SEEK_END);
  vfs_close(file);
  if (file_size <= 0)
    return -1;

  sectors = ((uint64_t)file_size + 511ULL) / 512ULL;
  if (sectors == 0 || sectors > 0xFFFFFFFFULL)
    return -1;

  *out_file_size = file_size;
  *out_sector_count = (uint32_t)sectors;
  return 0;
}

static void disk_imager_sanitize_component(const char *src, char *dst, int max) {
  int idx = 0;

  if (!dst || max <= 0)
    return;
  dst[0] = '\0';
  if (!src)
    return;

  for (int i = 0; src[i] && idx < max - 1; i++) {
    char c = src[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
      dst[idx++] = c;
    } else {
      dst[idx++] = '_';
    }
  }
  dst[idx] = '\0';
}

static void disk_imager_append_path(char *path, int max, const char *piece) {
  if (!path || max <= 0 || !piece)
    return;
  (void)strlcat(path, piece, (size_t)max);
}

static void disk_imager_build_backup_path(int disk_index, int partition_index,
                                          int partition_mode, char *path,
                                          int max) {
  char location[64];
  char label[128];
  char component[128];

  if (!path || max <= 0)
    return;
  str_copy_safe(path, "/Backups/", max);

  if (disk_index >= 0 && storage_get_disk_location(disk_index, location,
                                                   sizeof(location)) == 0) {
    disk_imager_sanitize_component(location, component, sizeof(component));
  } else {
    component[0] = '\0';
  }

  if (partition_mode) {
    storage_partition_kind_t kind = STORAGE_PARTITION_UNKNOWN;
    uint32_t start_lba = 0;
    uint32_t sector_count = 0;

    if (disk_index >= 0 &&
        storage_get_partition_info(disk_index, partition_index, &kind, label,
                                   sizeof(label), &start_lba, &sector_count) ==
            0) {
      char label_component[128];
      (void)kind;
      (void)start_lba;
      (void)sector_count;
      disk_imager_sanitize_component(label, label_component,
                                     sizeof(label_component));
      disk_imager_append_path(path, max, "part-");
      disk_imager_append_path(path, max, component[0] ? component : "disk");
      disk_imager_append_path(path, max, "-");
      disk_imager_append_path(path, max,
                              label_component[0] ? label_component : "part");
      disk_imager_append_path(path, max, ".img");
      return;
    }
  }

  disk_imager_append_path(path, max, "disk-");
  disk_imager_append_path(path, max, component[0] ? component : "disk");
  disk_imager_append_path(path, max, ".img");
}

static void disk_imager_refresh_state(void) {
  int disk_count = storage_get_disk_count();

  if (disk_count <= 0) {
    disk_imager_selected_disk = -1;
    disk_imager_selected_partition = 0;
    disk_imager_disk_label[0] = '\0';
    disk_imager_partition_label[0] = '\0';
    disk_imager_disk_path[0] = '\0';
    disk_imager_partition_path[0] = '\0';
    disk_imager_set_status("No storage disks detected.");
    return;
  }

  if (disk_imager_selected_disk < 0)
    disk_imager_selected_disk = installer_selected_disk_index();
  if (disk_imager_selected_disk < 0 || disk_imager_selected_disk >= disk_count)
    disk_imager_selected_disk = 0;

  if (storage_describe_disk(disk_imager_selected_disk, disk_imager_disk_label,
                            sizeof(disk_imager_disk_label)) != 0) {
    str_copy_safe(disk_imager_disk_label, "Unknown disk",
                  sizeof(disk_imager_disk_label));
  }

  {
    int partition_count = storage_get_partition_count(disk_imager_selected_disk);
    if (partition_count <= 0) {
      disk_imager_selected_partition = 0;
      str_copy_safe(disk_imager_partition_label, "No partitions on disk",
                    sizeof(disk_imager_partition_label));
    } else {
      if (disk_imager_selected_partition < 0)
        disk_imager_selected_partition = 0;
      if (disk_imager_selected_partition >= partition_count)
        disk_imager_selected_partition = partition_count - 1;
      if (storage_describe_partition(disk_imager_selected_disk,
                                     disk_imager_selected_partition,
                                     disk_imager_partition_label,
                                     sizeof(disk_imager_partition_label)) != 0) {
        str_copy_safe(disk_imager_partition_label, "Partition unavailable",
                      sizeof(disk_imager_partition_label));
      }
    }
  }

  disk_imager_build_backup_path(disk_imager_selected_disk, 0, 0,
                                disk_imager_disk_path,
                                sizeof(disk_imager_disk_path));
  disk_imager_build_backup_path(disk_imager_selected_disk,
                                disk_imager_selected_partition, 1,
                                disk_imager_partition_path,
                                sizeof(disk_imager_partition_path));
  if (disk_imager_status[0] == '\0')
    disk_imager_set_status("Ready.");
}

static int disk_imager_write_range(int disk_index, uint32_t start_lba,
                                   uint32_t sector_count, const char *path) {
  uint8_t sector[512];
  struct file *file;
  uint32_t disk_capacity;
  extern void process_yield(void);

  if (disk_index < 0 || !path || path[0] == '\0' || sector_count == 0)
    return -1;
  if (storage_get_disk_kind(disk_index) == STORAGE_KIND_CDROM) {
    disk_imager_set_status("Optical media cannot be imaged as 512-byte sectors.");
    return -1;
  }

  if (disk_imager_capacity_sectors(disk_index, &disk_capacity) != 0) {
    disk_imager_set_status("Selected disk capacity is invalid for imaging.");
    return -1;
  }
  if ((uint64_t)start_lba + (uint64_t)sector_count > (uint64_t)disk_capacity) {
    disk_imager_set_status("Requested backup range exceeds disk capacity.");
    return -1;
  }

  installer_ensure_parent_dirs(path);
  file = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (!file)
    return -1;

  for (uint32_t i = 0; i < sector_count; i++) {
    if (storage_read_block(disk_index, start_lba + i, sector, sizeof(sector)) !=
        0) {
      vfs_close(file);
      return -1;
    }
    if (vfs_write(file, (const char *)sector, sizeof(sector)) !=
        (ssize_t)sizeof(sector)) {
      vfs_close(file);
      return -1;
    }
    if ((i & 127U) == 127U)
      process_yield();
  }

  vfs_close(file);
  return 0;
}

static int disk_imager_read_range(int disk_index, uint32_t start_lba,
                                  uint32_t sector_count, const char *path) {
  uint8_t sector[512];
  struct file *file;
  uint32_t disk_capacity;
  uint32_t image_sectors;
  loff_t file_size;
  extern void process_yield(void);

  if (disk_index < 0 || !path || path[0] == '\0' || sector_count == 0)
    return -1;
  if (!storage_disk_supports_partition_writes(disk_index)) {
    disk_imager_set_status("Selected disk is not writable.");
    return -1;
  }

  if (storage_get_disk_kind(disk_index) == STORAGE_KIND_CDROM) {
    disk_imager_set_status("Optical media cannot be restored as block targets.");
    return -1;
  }
  if (disk_imager_capacity_sectors(disk_index, &disk_capacity) != 0) {
    disk_imager_set_status("Selected disk capacity is invalid for imaging.");
    return -1;
  }
  if ((uint64_t)start_lba + (uint64_t)sector_count > (uint64_t)disk_capacity) {
    disk_imager_set_status("Requested restore range exceeds disk capacity.");
    return -1;
  }
  if (disk_imager_image_sector_count(path, &file_size, &image_sectors) != 0) {
    disk_imager_set_status("Disk image file is missing or invalid.");
    return -1;
  }
  if (image_sectors > sector_count) {
    disk_imager_set_status("Disk image is larger than the selected target range.");
    return -1;
  }

  file = vfs_open(path, O_RDONLY, 0);
  if (!file)
    return -1;

  for (uint32_t i = 0; i < sector_count; i++) {
    ssize_t read_len = vfs_read(file, (char *)sector, sizeof(sector));
    if (read_len < 0) {
      vfs_close(file);
      return -1;
    }
    if (read_len < (ssize_t)sizeof(sector)) {
      for (size_t j = (size_t)read_len; j < sizeof(sector); j++)
        sector[j] = 0;
    }
    if (storage_write_block(disk_index, start_lba + i, sector, sizeof(sector)) !=
        0) {
      vfs_close(file);
      return -1;
    }
    if (read_len == 0) {
      for (uint32_t rest = i + 1; rest < sector_count; rest++) {
        for (size_t j = 0; j < sizeof(sector); j++)
          sector[j] = 0;
        if (storage_write_block(disk_index, start_lba + rest, sector,
                                sizeof(sector)) != 0) {
          vfs_close(file);
          return -1;
        }
      }
      break;
    }
    if ((i & 127U) == 127U)
      process_yield();
  }

  vfs_close(file);
  return 0;
}

static int disk_imager_backup_disk(void) {
  uint32_t sectors;

  if (disk_imager_selected_disk < 0)
    return -1;
  if (disk_imager_capacity_sectors(disk_imager_selected_disk, &sectors) != 0) {
    disk_imager_set_status("Selected disk is too large or unavailable.");
    return -1;
  }
  return disk_imager_write_range(disk_imager_selected_disk, 0, sectors,
                                 disk_imager_disk_path);
}

static int disk_imager_restore_disk(void) {
  uint32_t sectors;

  if (disk_imager_selected_disk < 0)
    return -1;
  if (disk_imager_capacity_sectors(disk_imager_selected_disk, &sectors) != 0) {
    disk_imager_set_status("Selected disk is too large or unavailable.");
    return -1;
  }
  return disk_imager_read_range(disk_imager_selected_disk, 0, sectors,
                                disk_imager_disk_path);
}

static int disk_imager_backup_partition(void) {
  uint32_t start_lba = 0;
  uint32_t sector_count = 0;
  storage_partition_kind_t kind = STORAGE_PARTITION_UNKNOWN;
  char label[160];

  if (disk_imager_selected_disk < 0)
    return -1;
  if (storage_get_partition_info(disk_imager_selected_disk,
                                 disk_imager_selected_partition, &kind, label,
                                 sizeof(label), &start_lba, &sector_count) !=
      0)
    return -1;
  (void)kind;
  return disk_imager_write_range(disk_imager_selected_disk, start_lba,
                                 sector_count, disk_imager_partition_path);
}

static int disk_imager_restore_partition(void) {
  uint32_t start_lba = 0;
  uint32_t sector_count = 0;
  storage_partition_kind_t kind = STORAGE_PARTITION_UNKNOWN;
  char label[160];

  if (disk_imager_selected_disk < 0)
    return -1;
  if (storage_get_partition_info(disk_imager_selected_disk,
                                 disk_imager_selected_partition, &kind, label,
                                 sizeof(label), &start_lba, &sector_count) !=
      0)
    return -1;
  (void)kind;
  return disk_imager_read_range(disk_imager_selected_disk, start_lba,
                                sector_count, disk_imager_partition_path);
}

static void open_disk_imager_window(int x, int y) {
  disk_imager_refresh_state();
  if (disk_imager_selected_disk < 0)
    disk_imager_selected_disk = installer_selected_disk_index();
  if (disk_imager_selected_disk < 0 && storage_get_disk_count() > 0)
    disk_imager_selected_disk = 0;
  if (disk_imager_selected_partition < 0)
    disk_imager_selected_partition = 0;
  vfs_mkdir("/Backups", 0755);
  gui_create_window("Disk Imager", x, y, 620, 440);
}

static void draw_disk_imager_window(int content_x, int content_y, int content_w,
                                    int content_h) {
  const gui_theme_palette_t *theme = gui_theme_palette();
  int list_x = content_x + 18;
  int list_y = content_y + 78;
  int list_w = (content_w - 54) / 2;
  int list_h = content_h - 166;
  int right_x = list_x + list_w + 18;
  int row_h = 24;
  int disk_count = storage_get_disk_count();
  int partition_count = 0;

  disk_imager_refresh_state();
  if (disk_imager_selected_disk >= 0)
    partition_count = storage_get_partition_count(disk_imager_selected_disk);

  gui_draw_rect(content_x, content_y, content_w, content_h, theme->app_bg);
  gui_draw_rect(content_x + 12, content_y + 12, content_w - 24, 56,
                theme->card);
  gui_draw_string(content_x + 24, content_y + 24, "Disk Imager", theme->app_fg,
                  theme->card);
  gui_draw_string(content_x + 24, content_y + 44,
                  "Backup or restore a whole disk or a partition to a raw image.",
                  theme->app_muted, theme->card);

  gui_draw_rect(list_x, list_y, list_w, list_h, theme->card);
  gui_draw_rect(right_x, list_y, list_w, list_h, theme->card);
  gui_draw_string(list_x + 12, list_y + 12, "Disks", 0x89B4FA, theme->card);
  gui_draw_string(right_x + 12, list_y + 12, "Partitions", 0x89B4FA,
                  theme->card);

  for (int i = 0; i < disk_count && i < 6; i++) {
    int row_y = list_y + 36 + i * 28;
    uint32_t row_bg = (i == disk_imager_selected_disk) ? theme->file_row_selected
                                                       : theme->surface_alt;
    char row_label[160];
    if (storage_describe_disk(i, row_label, sizeof(row_label)) != 0)
      str_copy_safe(row_label, "Unknown disk", sizeof(row_label));
    gui_draw_rect(list_x + 10, row_y, list_w - 20, row_h, row_bg);
    gui_draw_string(list_x + 18, row_y + 5, row_label, theme->app_fg, row_bg);
  }

  for (int i = 0; i < partition_count && i < 6; i++) {
    int row_y = list_y + 36 + i * 28;
    uint32_t row_bg =
        (i == disk_imager_selected_partition) ? theme->file_row_selected
                                              : theme->surface_alt;
    gui_draw_rect(right_x + 10, row_y, list_w - 20, row_h, row_bg);
    gui_draw_string(right_x + 18, row_y + 5, partition_manager_labels[i],
                    theme->app_fg, row_bg);
  }

  gui_draw_string(content_x + 24, content_y + content_h - 108, "Disk image:",
                  0x93C5FD, theme->card);
  gui_draw_string(content_x + 116, content_y + content_h - 108,
                  disk_imager_disk_path, theme->app_fg, theme->card);
  gui_draw_string(content_x + 24, content_y + content_h - 86, "Partition image:",
                  0x93C5FD, theme->card);
  gui_draw_string(content_x + 116, content_y + content_h - 86,
                  disk_imager_partition_path, theme->app_fg, theme->card);
  gui_draw_string(content_x + 24, content_y + content_h - 64, "Status:",
                  0x93C5FD, theme->card);
  gui_draw_string(content_x + 92, content_y + content_h - 64,
                  disk_imager_status, theme->accent, theme->card);

  gui_draw_rect(content_x + 24, content_y + content_h - 44, 104, 28, 0x2563EB);
  gui_draw_string(content_x + 40, content_y + content_h - 35, "Backup Disk",
                  0xFFFFFF, 0x2563EB);
  gui_draw_rect(content_x + 136, content_y + content_h - 44, 104, 28, 0x0F766E);
  gui_draw_string(content_x + 150, content_y + content_h - 35, "Restore Disk",
                  0xFFFFFF, 0x0F766E);
  gui_draw_rect(content_x + 248, content_y + content_h - 44, 116, 28, 0x1D4ED8);
  gui_draw_string(content_x + 263, content_y + content_h - 35, "Backup Part",
                  0xFFFFFF, 0x1D4ED8);
  gui_draw_rect(content_x + 372, content_y + content_h - 44, 116, 28, 0x7C2D12);
  gui_draw_string(content_x + 386, content_y + content_h - 35, "Restore Part",
                  0xFFFFFF, 0x7C2D12);
  gui_draw_rect(content_x + 496, content_y + content_h - 44, 84, 28, 0x4B5563);
  gui_draw_string(content_x + 512, content_y + content_h - 35, "Refresh",
                  0xFFFFFF, 0x4B5563);
}

static void open_partition_manager_window(int x, int y) {
  installer_refresh_disk_inventory();
  partition_manager_refresh_partitions();
  gui_create_window("Partition Manager", x, y, 560, 360);
}

static void draw_partition_manager_window(int content_x, int content_y,
                                          int content_w, int content_h) {
  const gui_theme_palette_t *theme = gui_theme_palette();
  extern int storage_disk_supports_partition_writes(int disk_index);
  int selected_disk_index = installer_selected_disk_index();
  installer_refresh_disk_inventory();
  partition_manager_refresh_partitions();

  gui_draw_rect(content_x, content_y, content_w, content_h, theme->app_bg);
  gui_draw_rect(content_x + 12, content_y + 12, content_w - 24, content_h - 24,
                theme->card);
  gui_draw_string(content_x + 24, content_y + 22, "Partition Manager",
                  theme->app_fg, theme->card);
  gui_draw_string(content_x + 24, content_y + 44,
                  "Detect, create, delete, auto-layout, and format partitions.",
                  theme->app_muted, theme->card);
  gui_draw_string(content_x + 24, content_y + 58,
                  (selected_disk_index >= 0 &&
                   storage_disk_supports_partition_writes(selected_disk_index))
                      ? "Selected disk supports on-disk partition writes."
                      : "Selected disk has no real partition-write backend yet.",
                  0xF9E2AF, theme->card);

  gui_draw_string(content_x + 24, content_y + 76, "Detected disks", 0x89B4FA,
                  theme->card);
  for (int i = 0; i < installer_disk_count && i < 6; i++) {
    int row_y = content_y + 96 + i * 28;
    uint32_t row_bg = i == installer_selected_disk ? theme->file_row_selected
                                                   : theme->surface_alt;
    gui_draw_rect(content_x + 24, row_y, content_w - 48, 24, row_bg);
    gui_draw_string(content_x + 36, row_y + 5, installer_disk_labels[i],
                    theme->app_fg, row_bg);
    if (i == installer_selected_disk) {
      gui_draw_string(content_x + content_w - 170, row_y + 5,
                      "Installer Target", 0xA6E3A1, row_bg);
    }
  }

  gui_draw_string(content_x + 24, content_y + 274, "Selected target:",
                  0x89B4FA, theme->card);
  gui_draw_string(content_x + 150, content_y + 274,
                  installer_selected_disk_label(), theme->app_fg, theme->card);

  gui_draw_string(content_x + 24, content_y + 176, "Partitions", 0x89B4FA,
                  theme->card);
  for (int i = 0; i < partition_manager_partition_count && i < 4; i++) {
    int row_y = content_y + 196 + i * 22;
    uint32_t row_bg = i == partition_manager_selected_partition
                          ? theme->file_row_selected
                          : theme->surface;
    gui_draw_rect(content_x + 24, row_y, content_w - 48, 18, row_bg);
    gui_draw_string(content_x + 34, row_y + 4, partition_manager_labels[i],
                    theme->app_fg, row_bg);
  }

  gui_draw_rect(content_x + 24, content_y + 298, 120, 30, 0x2563EB);
  gui_draw_string(content_x + 42, content_y + 307, "Use For Install",
                  0xFFFFFF, 0x2563EB);
  gui_draw_rect(content_x + 152, content_y + 298, 78, 30, 0x0F766E);
  gui_draw_string(content_x + 170, content_y + 307, "New EFI", 0xFFFFFF,
                  0x0F766E);
  gui_draw_rect(content_x + 238, content_y + 298, 94, 30, 0x166534);
  gui_draw_string(content_x + 250, content_y + 307, "New System", 0xFFFFFF,
                  0x166534);
  gui_draw_rect(content_x + 340, content_y + 298, 84, 30, 0x1D4ED8);
  gui_draw_string(content_x + 356, content_y + 307, "New Data", 0xFFFFFF,
                  0x1D4ED8);
  gui_draw_rect(content_x + 432, content_y + 298, 96, 30, 0x7C2D12);
  gui_draw_string(content_x + 450, content_y + 307, "Delete", 0xFFFFFF,
                  0x7C2D12);

  gui_draw_rect(content_x + 24, content_y + 332, 100, 30, 0x166534);
  gui_draw_string(content_x + 40, content_y + 341, "Fmt ext4", 0xFFFFFF,
                  0x166534);
  gui_draw_rect(content_x + 132, content_y + 332, 110, 30, 0x0F766E);
  gui_draw_string(content_x + 148, content_y + 341, "Fmt FAT32", 0xFFFFFF,
                  0x0F766E);
  gui_draw_rect(content_x + 250, content_y + 332, 90, 30, 0x7C3AED);
  gui_draw_string(content_x + 268, content_y + 341, "Fmt Swap", 0xFFFFFF,
                  0x7C3AED);
  gui_draw_rect(content_x + 348, content_y + 332, 110, 30, 0x3B82F6);
  gui_draw_string(content_x + 370, content_y + 341, "Auto Layout", 0xFFFFFF,
                  0x3B82F6);
  gui_draw_rect(content_x + 466, content_y + 332, 62, 30, 0x4B5563);
  gui_draw_string(content_x + 474, content_y + 341, "Refresh", 0xFFFFFF,
                  0x4B5563);

  gui_draw_string(content_x + 24, content_y + content_h - 52,
                  partition_manager_status, theme->app_muted, theme->card);
}

struct image_viewer_state {
  media_image_t image;
};

/* Forward declarations for modern image viewer (defined later in file) */
struct modern_image_viewer_state;
static struct {
  media_image_t image;
  media_image_t rotated;
  int loaded;
  int zoom_pct;
  int offset_x;
  int offset_y;
  int dragging;
  int drag_start_x;
  int drag_start_y;
  char current_file[256];
  int current_image_index;
  int rotation;
  int fullscreen;
  int show_toolbar;
  int toolbar_timer;
  int crop_mode;
  int crop_x1, crop_y1;
  int crop_x2, crop_y2;
  /* Folder-based navigation */
  char folder_path[256];
  char file_list[32][64]; /* Up to 32 files, 64 chars each */
  int file_count;
  int file_index; /* Current index in folder */
} g_imgview = {0};
static void image_viewer_on_draw(struct window *win);
static void image_viewer_on_mouse(struct window *win, int x, int y,
                                  int buttons);

void gui_open_image_viewer(const char *path);
static void gui_play_mp3_file(const char *path);

/* File Manager Mouse Handler */
static void fm_on_mouse(struct window *win, int x, int y, int buttons) {
  struct fm_state *st = (struct fm_state *)win->userdata;
  int is_right_click = (buttons & 2) != 0;
  if (!st)
    return;

  int content_x = 0;
  int content_y = 0;
  int content_w = 0;
  int content_h = 0;
  int toolbar_h = 52;
  int info_h = 54;
  int sidebar_w = 118;
  int details_w = 130;
  gui_get_window_content_rect_local(win, &content_x, &content_y, &content_w,
                                    &content_h);
  int list_x = content_x + sidebar_w + 10;
  int list_y = content_y + toolbar_h + info_h + 8;
  int list_w = content_w - sidebar_w - details_w - 24;
  int row_h = 44;
  int menu_x = st->context_menu_x;
  int menu_y = st->context_menu_y;
  int menu_w = st->context_menu_target_on_item ? 132 : 120;
  int menu_h = st->context_menu_target_on_item ? 78 : 54;

  if (menu_x + menu_w > content_x + content_w)
    menu_x = content_x + content_w - menu_w;
  if (menu_y + menu_h > content_y + content_h)
    menu_y = content_y + content_h - menu_h;
  if (menu_x < content_x + 4)
    menu_x = content_x + 4;
  if (menu_y < content_y + 4)
    menu_y = content_y + 4;

  if (st->context_menu_visible) {
    if (x >= menu_x && x < menu_x + menu_w && y >= menu_y && y < menu_y + menu_h) {
      int item_idx = (y - menu_y - 6) / 24;
      if (st->context_menu_target_on_item) {
        if (item_idx == 0) {
          fm_open_item(win, st, st->context_menu_target,
                       (unsigned)st->context_menu_target_type);
        } else if (item_idx == 1) {
          char full_path[512];
          extern void gui_open_rename(const char *path);
          fm_join_path(st->path, st->context_menu_target, full_path,
                       sizeof(full_path));
          gui_open_rename(full_path);
        } else if (item_idx == 2) {
          fm_delete_context_target(st);
        }
      } else {
        if (item_idx == 0) {
          char new_path[512];
          fm_build_unique_child_path(st->path, "New Folder", "", new_path,
                                     sizeof(new_path));
          user_storage_mkdir(new_path, 0755);
        } else if (item_idx == 1) {
          char new_path[512];
          fm_build_unique_child_path(st->path, "New File", ".txt", new_path,
                                     sizeof(new_path));
          write_text_file(new_path, "");
        }
      }
      fm_hide_context_menu(st);
      return;
    }

    if (!is_right_click)
      fm_hide_context_menu(st);
  }

  if (y >= content_y + 10 && y < content_y + 42) {
    if (is_right_click) {
      fm_hide_context_menu(st);
      return;
    }
    if (x >= content_x + 10 && x < content_x + 74) {
      fm_go_parent(win, st);
      return;
    }
    if (x >= content_x + 82 && x < content_x + 166) {
      char new_path[512];
      fm_build_unique_child_path(st->path, "New Folder", "", new_path,
                                 sizeof(new_path));
      user_storage_mkdir(new_path, 0755);
      return;
    }
    if (x >= content_x + 174 && x < content_x + 248) {
      char new_path[512];
      fm_build_unique_child_path(st->path, "New File", ".txt", new_path,
                                 sizeof(new_path));
      write_text_file(new_path, "");
      return;
    }
    if (x >= content_x + 256 && x < content_x + 328) {
      if (st->selected[0]) {
        char full_path[512];
        extern void gui_open_rename(const char *path);
        fm_join_path(st->path, st->selected, full_path, sizeof(full_path));
        gui_open_rename(full_path);
      }
      return;
    }
    if (x >= content_x + 336 && x < content_x + 410) {
      open_partition_manager_window(win->x + 40, win->y + 30);
      return;
    }
  }

  if (x >= content_x + 10 && x < content_x + sidebar_w - 10) {
    if (is_right_click) {
      fm_hide_context_menu(st);
      return;
    }
    if (y >= content_y + 88 && y < content_y + 116)
      fm_navigate_to(win, st, "/");
    else if (y >= content_y + 118 && y < content_y + 146)
      fm_navigate_to(win, st, "/Desktop");
    else if (y >= content_y + 148 && y < content_y + 176)
      fm_navigate_to(win, st, "/Documents");
    else if (y >= content_y + 178 && y < content_y + 206)
      fm_navigate_to(win, st, "/Pictures");
    else if (y >= content_y + 208 && y < content_y + 236)
      fm_navigate_to(win, st, "/Music");
    else if (y >= content_y + 238 && y < content_y + 266)
      fm_navigate_to(win, st, "/Users");
    else if (y >= content_y + 268 && y < content_y + 296)
      fm_navigate_to(win, st, "/External");
    else if (y >= content_y + 298 && y < content_y + 326)
      fm_navigate_to(win, st, GUI_SYSTEM_APPS_FOLDER);
    return;
  }

  struct fm_item items[FM_MAX_ITEMS];
  int item_count = fm_collect_items(st->path, items, FM_MAX_ITEMS);
  if (item_count <= 0) {
    if (is_right_click) {
      st->context_menu_visible = 1;
      st->context_menu_target_on_item = 0;
      st->context_menu_x = x;
      st->context_menu_y = y;
      st->selected[0] = '\0';
    }
    return;
  }

  for (int i = 0; i < item_count; i++) {
    int row_y = list_y + i * row_h;
    if (x >= list_x && x < list_x + list_w && y >= row_y && y < row_y + row_h) {
      int was_selected = str_cmp(st->selected, items[i].name) == 0;
      str_copy_safe(st->selected, items[i].name, sizeof(st->selected));

      if (is_right_click) {
        st->context_menu_visible = 1;
        st->context_menu_target_on_item = 1;
        st->context_menu_x = x;
        st->context_menu_y = y;
        st->context_menu_target_type = (int)items[i].type;
        str_copy_safe(st->context_menu_target, items[i].name,
                      sizeof(st->context_menu_target));
        return;
      }

      fm_hide_context_menu(st);

      if (!was_selected) {
        gui_invalidate_window(win);
        return;
      }

      fm_open_item(win, st, items[i].name, items[i].type);
      return;
    }
  }

  if (is_right_click) {
    st->context_menu_visible = 1;
    st->context_menu_target_on_item = 0;
    st->context_menu_x = x;
    st->context_menu_y = y;
    st->selected[0] = '\0';
    return;
  }

  fm_hide_context_menu(st);
}

static void image_viewer_on_close(struct window *win) {
  if (!win || !win->userdata)
    return;
  struct image_viewer_state *st = (struct image_viewer_state *)win->userdata;
  media_free_image(&st->image);
  kfree(st);
  win->userdata = NULL;
}

static void draw_image_viewer(struct window *win, int content_x, int content_y,
                              int content_w, int content_h) {
  const gui_theme_palette_t *theme = gui_theme_palette();
  if (!win || !win->userdata)
    return;
  struct image_viewer_state *st = (struct image_viewer_state *)win->userdata;
  if (!st->image.pixels || st->image.width == 0 || st->image.height == 0)
    return;

  gui_draw_rect(content_x, content_y, content_w, content_h, theme->app_bg);

  int img_w = (int)st->image.width;
  int img_h = (int)st->image.height;
  int draw_w = img_w;
  int draw_h = img_h;

  if (draw_w > content_w) {
    draw_w = content_w;
    draw_h = (img_h * draw_w) / img_w;
  }
  if (draw_h > content_h) {
    draw_h = content_h;
    draw_w = (img_w * draw_h) / img_h;
  }
  if (draw_w <= 0 || draw_h <= 0)
    return;

  int offset_x = content_x + (content_w - draw_w) / 2;
  int offset_y = content_y + (content_h - draw_h) / 2;

  gui_draw_rect(offset_x - 6, offset_y - 6, draw_w + 12, draw_h + 12, theme->card);
  gui_draw_rect_outline(offset_x - 6, offset_y - 6, draw_w + 12, draw_h + 12,
                        theme->border, 1);

  gui_draw_image_scaled(offset_x, offset_y, draw_w, draw_h, &st->image);
}

void gui_open_image_viewer(const char *path) {
  if (!path)
    return;

  /* Load image file */
  uint8_t *data = NULL;
  size_t size = 0;
  if (media_load_file(path, &data, &size) != 0) {
    printk("Image Viewer: Failed to read %s\n", path);
    return;
  }

  /* Free previous image if loaded */
  if (g_imgview.loaded) {
    media_free_image(&g_imgview.image);
    g_imgview.loaded = 0;
  }

  /* Decode new image using the shared wallpaper/image loader. */
  int decode_ret =
      decode_image_file_for_path(path, data, size, &g_imgview.image, NULL, 0, NULL);
  if (decode_ret != 0) {
    printk("Image Viewer: image decode failed for %s\n", path);
    media_free_file(data);
    return;
  }
  media_free_file(data);

  /* Set up viewer state */
  g_imgview.loaded = 1;
  g_imgview.zoom_pct = 0; /* Auto-fit */
  g_imgview.offset_x = 0;
  g_imgview.offset_y = 0;
  g_imgview.rotation = 0;
  g_imgview.fullscreen = 0;
  g_imgview.current_image_index = -1; /* -1 means file-loaded, not bootstrap */

  /* Extract folder path and filename */
  int i = 0;
  int last_slash = -1;
  while (path[i]) {
    if (path[i] == '/')
      last_slash = i;
    i++;
  }

  /* Copy folder path */
  for (int j = 0; j <= last_slash && j < 255; j++) {
    g_imgview.folder_path[j] = path[j];
  }
  g_imgview.folder_path[last_slash + 1] = '\0';

  /* Copy filename */
  const char *filename = path + last_slash + 1;
  i = 0;
  while (filename[i] && i < 255) {
    g_imgview.current_file[i] = filename[i];
    i++;
  }
  g_imgview.current_file[i] = '\0';

  /* Scan folder for image files - use hardcoded list for /Pictures */
  g_imgview.file_count = 0;
  g_imgview.file_index = 0;

  /* Known image files in Pictures folder */
  static const char *pictures_files[] = {
      "test.png",      "pig.jpg",    "city.jpg",     "nature.jpg",
      "wallpaper.jpg", "square.jpg", "portrait.jpg", "landscape.png"};
  int num_pictures = sizeof(pictures_files) / sizeof(pictures_files[0]);

  /* Check if we're in Pictures folder */
  int is_pictures =
      (g_imgview.folder_path[0] == '/' && g_imgview.folder_path[1] == 'P' &&
       g_imgview.folder_path[2] == 'i' && g_imgview.folder_path[3] == 'c');

  if (is_pictures) {
    for (int j = 0; j < num_pictures && g_imgview.file_count < 32; j++) {
      /* Copy filename to list */
      int k = 0;
      while (pictures_files[j][k] && k < 63) {
        g_imgview.file_list[g_imgview.file_count][k] = pictures_files[j][k];
        k++;
      }
      g_imgview.file_list[g_imgview.file_count][k] = '\0';

      /* Check if this is current file */
      int match = 1;
      for (int m = 0; filename[m] || pictures_files[j][m]; m++) {
        if (filename[m] != pictures_files[j][m]) {
          match = 0;
          break;
        }
      }
      if (match) {
        g_imgview.file_index = g_imgview.file_count;
      }
      g_imgview.file_count++;
    }
  }

  printk("Image Viewer: Loaded %s (%dx%d) - %d images in folder\n",
         g_imgview.current_file, g_imgview.image.width, g_imgview.image.height,
         g_imgview.file_count);

  /* Create modern viewer window */
  struct window *win = gui_create_window("Image Viewer", 80, 60, 800, 600);
  if (win) {
    win->on_draw = image_viewer_on_draw;
    win->on_mouse = image_viewer_on_mouse;
  }
}

static void gui_play_mp3_file(const char *path) {
  if (!path)
    return;

  uint8_t *data = NULL;
  size_t size = 0;
  if (media_load_file(path, &data, &size) != 0) {
    printk("Audio: Failed to read %s\n", path);
    return;
  }

  media_audio_t audio;
  audio.samples = NULL;
  audio.sample_count = 0;
  audio.sample_rate = 0;
  audio.channels = 0;

  if (media_decode_mp3(data, size, &audio) != 0) {
    printk("Audio: MP3 decode failed\n");
    media_free_file(data);
    return;
  }
  media_free_file(data);

  extern int intel_hda_play_pcm(const void *data, uint32_t samples,
                                uint8_t channels, uint32_t sample_rate);
  intel_hda_play_pcm(audio.samples, audio.sample_count, audio.channels,
                     audio.sample_rate);
  media_free_audio(&audio);
}

static void draw_window_internal(struct window *win) {
  // ... rest of function ...
  if (!win->visible)
    return;

  const gui_theme_palette_t *theme = gui_theme_palette();
  int x, y, w, h;
  window_get_draw_rect(win, &x, &y, &w, &h);
  struct gui_clip_state prev_clip = gui_set_clip_rect(x, y, w, h);

  if (window_uses_newwindows_chrome(win)) {
    SkinRenderer renderer = {
        .userdata = NULL,
        .fill_rect = gui_skin_fill_rect,
        .draw_rect_outline = gui_skin_draw_rect_outline,
        .draw_line = gui_skin_draw_line,
        .draw_text = gui_skin_draw_text,
    };
    SkinWindow skin_window = gui_skin_window_from_window(win);
    WindowSkin skin = gui_newwindows_skin_for_window(win);
    SkinHit hot_hit = chrome_hot_window == win ? chrome_hot_hit : SKIN_HIT_NONE;
    SkinHit pressed_hit =
        chrome_pressed_window == win ? chrome_pressed_hit : SKIN_HIT_NONE;

    if (g_blur_effects_enabled) {
      int blur_stride = gui_adjust_blur_stride_for_area(w, h, 3);
      if (blur_stride > 0)
        gui_apply_backdrop_blur(x, y, w, h, blur_stride);
    }

    skin_draw_window_chrome(&renderer, &skin_window, &skin, hot_hit, pressed_hit);
  }

  /* Draw content area */
  int content_x = x + window_content_origin_x(win);
  int content_y = y + window_content_origin_y(win);
  int inset_right = 0;
  int inset_bottom = 0;
  gui_newwindows_content_insets(win, NULL, NULL, &inset_right, &inset_bottom);
  int content_w = w - window_content_origin_x(win) - inset_right;
  int content_h = h - window_content_origin_y(win) - inset_bottom;

  if (win->layout_kind == GUI_WINDOW_LAYOUT_FRAMEBUFFER ||
      win->chrome_kind == GUI_WINDOW_CHROME_FRAMEBUFFER) {
    gui_draw_rect(content_x, content_y, content_w, content_h, 0x000000);
  } else if (win->layout_kind == GUI_WINDOW_LAYOUT_SCROLL) {
    gui_fill_visual_effect_rect(content_x, content_y, content_w, content_h,
                                0xA01A1F2D);
    gui_fill_visual_effect_rect(content_x + content_w - 12, content_y + 8, 4,
                                content_h - 16, 0x304B5563);
  } else if (win->layout_kind == GUI_WINDOW_LAYOUT_BUTTONS) {
    gui_fill_visual_effect_rect(content_x, content_y, content_w, content_h,
                                0xB01A1A1E);
    gui_fill_visual_effect_rect(content_x, content_y, content_w, 1, 0x30FFFFFF);
  } else {
    gui_fill_visual_effect_rect(content_x, content_y, content_w, content_h,
                                0x98171A26);
    gui_fill_visual_effect_rect(content_x, content_y, content_w, 1, 0x28FFFFFF);
  }

  /* Draw window-specific content based on title */
  /* Calculator - Modern Design */
  if (win->title[0] == 'C' && win->title[1] == 'a' && win->title[2] == 'l') {
    uint32_t calc_bg = g_theme_mode == GUI_THEME_LIGHT ? 0xEEF3F9 : 0x1C1C1E;
    uint32_t calc_display_bg =
        g_theme_mode == GUI_THEME_LIGHT ? 0xFFFFFF : 0x2C2C2E;
    uint32_t calc_display_top =
        g_theme_mode == GUI_THEME_LIGHT ? 0xD7E2EF : 0x3C3C3E;
    gui_draw_rect(content_x, content_y, content_w, content_h, calc_bg);

    /* Display area - gradient effect (dark to slightly lighter) */
    int disp_h = 70;
    gui_draw_rect(content_x + 12, content_y + 12, content_w - 24, disp_h,
                  calc_display_bg);
    gui_draw_rect(content_x + 12, content_y + 12, content_w - 24, 2,
                  calc_display_top);

    /* Display value - large right-aligned */
    char display[16];
    long v = calc_display;
    int is_neg = 0;
    if (v < 0) {
      is_neg = 1;
      v = -v;
    }
    int idx = 0;
    if (v == 0) {
      display[idx++] = '0';
    } else {
      char tmp[16];
      int ti = 0;
      while (v > 0 && ti < 14) {
        tmp[ti++] = '0' + (v % 10);
        v /= 10;
      }
      if (is_neg)
        display[idx++] = '-';
      while (ti > 0) {
        display[idx++] = tmp[--ti];
      }
    }
    display[idx] = '\0';
    /* Draw display value with simulated large font (double-draw) */
    int text_x = content_x + content_w - 20 - idx * 12;
    int text_y = content_y + 40;
    gui_draw_string(text_x, text_y, display, theme->settings_text,
                    calc_display_bg);
    gui_draw_string(text_x + 1, text_y, display, theme->settings_text,
                    calc_display_bg);

    /* Button grid - 4x5 layout with proper spacing */
    static const char *btns[5][4] = {{"C", "+/-", "%", "/"},
                                     {"7", "8", "9", "*"},
                                     {"4", "5", "6", "-"},
                                     {"1", "2", "3", "+"},
                                     {"0", "0", ".", "="}};

    int grid_x = content_x + 12;
    int grid_y = content_y + disp_h + 20;
    int grid_w = content_w - 24;
    int grid_h = content_h - disp_h - 32;
    int bw = (grid_w - 12) / 4; /* 3 gaps of 4px */
    int bh = (grid_h - 16) / 5; /* 4 gaps of 4px */
    int gap = 4;

    for (int row = 0; row < 5; row++) {
      for (int col = 0; col < 4; col++) {
        /* Skip duplicate "0" cell */
        if (row == 4 && col == 1)
          continue;

        int bx = grid_x + col * (bw + gap);
        int by = grid_y + row * (bh + gap);
        int btn_w = bw;

        /* "0" button spans 2 columns */
        if (row == 4 && col == 0) {
          btn_w = bw * 2 + gap;
        }

        /* Button colors */
        uint32_t bg, mg, fg;
        char btn_char = btns[row][col][0];

        if (btn_char == '/' || btn_char == '*' || btn_char == '-' ||
            btn_char == '+' || btn_char == '=') {
          /* Orange operator buttons */
          bg = 0xFF9F0A;
          mg = 0xFFAE31; /* Slightly lighter for top edge */
          fg = 0xFFFFFF;
        } else if (btn_char == 'C' || btn_char == '+' || btn_char == '%') {
          /* Light gray function buttons */
          bg = g_theme_mode == GUI_THEME_LIGHT ? 0xD6DEE8 : 0xA5A5A5;
          mg = g_theme_mode == GUI_THEME_LIGHT ? 0xE4EBF3 : 0xB5B5B5;
          fg = g_theme_mode == GUI_THEME_LIGHT ? 0x172033 : 0x000000;
        } else {
          /* Dark gray number buttons */
          bg = g_theme_mode == GUI_THEME_LIGHT ? 0xFFFFFF : 0x333333;
          mg = g_theme_mode == GUI_THEME_LIGHT ? 0xE2E8F0 : 0x444444;
          fg = g_theme_mode == GUI_THEME_LIGHT ? 0x172033 : 0xFFFFFF;
        }

        /* Draw button with rounded effect (lighter top edge) */
        gui_draw_rect(bx, by, btn_w, bh, bg);
        gui_draw_rect(bx, by, btn_w, 2, mg);

        /* Center text in button */
        const char *label = btns[row][col];
        int label_len = 0;
        while (label[label_len])
          label_len++;
        int tx = bx + (btn_w - label_len * 8) / 2;
        int ty = by + (bh - 16) / 2;
        gui_draw_string(tx, ty, label, fg, bg);
      }
    }
  }
  /* File Manager */
  else if (win->title[0] == 'F' && win->title[1] == 'i' &&
           win->title[2] == 'l') {
    const gui_theme_palette_t *theme = gui_theme_palette();
    struct fm_state *st = (struct fm_state *)win->userdata;
    const char *path = st ? st->path : "/";
    struct fm_item items[FM_MAX_ITEMS];
    int item_count = fm_collect_items(path, items, FM_MAX_ITEMS);
    int folder_count = 0;
    int selected_index = -1;

    if (item_count > 0) {
      for (int i = 0; i < item_count; i++) {
        if (items[i].type == 4)
          folder_count++;
        if (st && str_cmp(st->selected, items[i].name) == 0)
          selected_index = i;
      }
    }

    int toolbar_h = 52;
    int info_h = 54;
    int sidebar_w = 118;
    int details_w = 130;
    int sidebar_x = content_x + 8;
    int sidebar_y = content_y + toolbar_h + 8;
    int list_x = content_x + sidebar_w + 10;
    int list_y = content_y + toolbar_h + info_h + 8;
    int list_w = content_w - sidebar_w - details_w - 24;
    int row_h = 44;
    int size_col_x = list_x + list_w - 150;
    int type_col_x = list_x + list_w - 74;
    int name_text_x = list_x + 48;
    int name_max_px = size_col_x - name_text_x - 12;

    gui_draw_rect(content_x, content_y, content_w, content_h, theme->file_bg);

    gui_draw_rect(content_x, content_y, content_w, toolbar_h, theme->file_toolbar);
    gui_draw_rect(content_x + 10, content_y + 10, 64, 30, 0x334155);
    gui_draw_string(content_x + 28, content_y + 19, "Back", 0xFFFFFF, 0x334155);
    gui_draw_rect(content_x + 82, content_y + 10, 84, 30, 0x1D4ED8);
    gui_draw_string(content_x + 96, content_y + 19, "Folder", 0xFFFFFF, 0x1D4ED8);
    gui_draw_rect(content_x + 174, content_y + 10, 74, 30, 0x0F766E);
    gui_draw_string(content_x + 192, content_y + 19, "File", 0xFFFFFF, 0x0F766E);
    gui_draw_rect(content_x + 256, content_y + 10, 72, 30, 0x6D28D9);
    gui_draw_string(content_x + 270, content_y + 19, "Rename", 0xFFFFFF, 0x6D28D9);
    gui_draw_rect(content_x + 336, content_y + 10, 74, 30, 0x2563EB);
    gui_draw_string(content_x + 356, content_y + 19, "Disks", 0xFFFFFF, 0x2563EB);

    gui_draw_rect(content_x + 8, content_y + toolbar_h + 4, content_w - 16, 42,
                  theme->file_path);
    gui_draw_string(content_x + 18, content_y + toolbar_h + 10, "Location",
                    0x93C5FD, theme->file_path);
    gui_draw_string(content_x + 90, content_y + toolbar_h + 10, path,
                    theme->file_text, theme->file_path);
    gui_draw_string(content_x + content_w - 130, content_y + toolbar_h + 10,
                    "Folders", theme->file_subtext, theme->file_path);

    char count_buf[16];
    count_buf[0] = '\0';
    int count_idx = 0;
    if (folder_count >= 10)
      count_buf[count_idx++] = (char)('0' + (folder_count / 10));
    count_buf[count_idx++] = (char)('0' + (folder_count % 10));
    count_buf[count_idx] = '\0';
    gui_draw_string(content_x + content_w - 74, content_y + toolbar_h + 10,
                    count_buf, theme->file_text, theme->file_path);

    gui_draw_rect(sidebar_x, sidebar_y, sidebar_w - 16, content_h - toolbar_h - 16,
                  theme->file_sidebar);
    gui_draw_string(sidebar_x + 12, sidebar_y + 14, "Places", theme->file_text,
                    theme->file_sidebar);

    const char *places[] = {"/", "/Desktop", "/Documents", "/Pictures", "/Music",
                            "/Users", "/External", GUI_SYSTEM_APPS_FOLDER};
    const char *labels[] = {"Root", "Desktop", "Documents", "Pictures", "Music",
                            "Users", "External", "System Apps"};
    for (int i = 0; i < 8; i++) {
      int row_y = sidebar_y + 34 + i * 30;
      uint32_t row_bg = (st && str_cmp(st->path, places[i]) == 0) ? theme->accent
                                                                  : theme->file_path;
      gui_draw_rect(sidebar_x + 8, row_y, sidebar_w - 32, 24, row_bg);
      gui_draw_folder_icon(places[i], sidebar_x + 12, row_y + 2, 20);
      gui_draw_string(sidebar_x + 36, row_y + 6, labels[i], 0xFFFFFF, row_bg);
    }

    gui_draw_rect(list_x, content_y + toolbar_h + 8, list_w, content_h - toolbar_h - 16,
                  theme->file_list);
    gui_draw_rect(details_w > 0 ? content_x + content_w - details_w - 8 : content_x,
                  content_y + toolbar_h + 8, details_w, content_h - toolbar_h - 16,
                  theme->file_details);
    gui_draw_string(content_x + content_w - details_w + 8, content_y + toolbar_h + 22,
                    "Details", theme->file_text, theme->file_details);

    gui_draw_rect(list_x + 8, content_y + toolbar_h + 18, list_w - 16, 24,
                  theme->file_header);
    gui_draw_string(list_x + 18, content_y + toolbar_h + 24, "Name", 0x93C5FD,
                    theme->file_header);
    gui_draw_string(size_col_x, content_y + toolbar_h + 24, "Size",
                    0x93C5FD, theme->file_header);
    gui_draw_string(type_col_x, content_y + toolbar_h + 24, "Type",
                    0x93C5FD, theme->file_header);

    if (item_count < 0) {
      gui_draw_string(list_x + 16, list_y + 10, "Failed to open directory",
                      0xFCA5A5, theme->file_list);
    } else if (item_count == 0) {
      gui_draw_string(list_x + 16, list_y + 10, "This folder is empty.",
                      theme->file_subtext, theme->file_list);
    } else {
      // Dedup tracking
      char seen[256][256];
      int seen_count = 0;
      int displayed = 0;
      for (int i = 0; i < item_count && displayed < 10; i++) {
        // Skip . and ..
        if (!str_cmp(items[i].name, ".") || !str_cmp(items[i].name, ".."))
          continue;
        int duplicate = 0;
        for (int j = 0; j < seen_count; j++) {
          if (!str_cmp(seen[j], items[i].name)) {
            duplicate = 1;
            break;
          }
        }
        if (duplicate) continue;
        str_copy_safe(seen[seen_count++], items[i].name, 256);
        int row_y = list_y + displayed * row_h;
        int is_selected = (selected_index == i);
        uint32_t row_bg = is_selected ? theme->file_row_selected
                                      : (displayed % 2 ? theme->file_row_odd
                                                       : theme->file_row_even);
        uint32_t icon_color = 0xFFFFFF;
        const unsigned char *icon =
            fm_icon_for_item(items[i].name, items[i].type, &icon_color);
        char short_name[64];
        char size_buf[24];
        const char *type_label = fm_type_label(items[i].name, items[i].type);

        fm_truncate_label_px(items[i].name, short_name, sizeof(short_name),
                             name_max_px);
        fm_format_size(items[i].size_bytes, items[i].type, size_buf,
                       sizeof(size_buf));
        gui_draw_rect(list_x + 8, row_y, list_w - 16, row_h - 4, row_bg);
        if (items[i].type == 4) {
          char full_path[512];
          fm_join_path(st->path, items[i].name, full_path, sizeof(full_path));
          gui_draw_folder_icon(full_path, list_x + 14, row_y + 6, 24);
        } else {
          draw_icon(list_x + 14, row_y + 6, 24, icon, icon_color, row_bg);
        }
        gui_draw_string(name_text_x, row_y + 14, short_name, theme->file_text,
                        row_bg);
        gui_draw_string(size_col_x, row_y + 14, size_buf,
                        is_selected ? 0xDBEAFE : theme->file_subtext, row_bg);
        gui_draw_string(type_col_x, row_y + 14, type_label,
                        is_selected ? 0xDBEAFE : theme->file_subtext, row_bg);
        displayed++;
      }
    }

    gui_draw_string(content_x + content_w - details_w + 8, content_y + toolbar_h + 54,
                    "Items", 0x93C5FD, theme->file_details);
    char items_buf[16];
    items_buf[0] = '\0';
    int items_idx = 0;
    if (item_count > 99)
      item_count = 99;
    if (item_count >= 10)
      items_buf[items_idx++] = (char)('0' + (item_count / 10));
    items_buf[items_idx++] = (char)('0' + (item_count % 10));
    items_buf[items_idx] = '\0';
    gui_draw_string(content_x + content_w - details_w + 58, content_y + toolbar_h + 54,
                    items_buf, theme->file_text, theme->file_details);

    gui_draw_string(content_x + content_w - details_w + 8, content_y + toolbar_h + 78,
                    "Selected", 0x93C5FD, theme->file_details);
    if (selected_index >= 0) {
      char selected_short[32];
      char selected_size[24];
      fm_truncate_label_px(items[selected_index].name, selected_short,
                           sizeof(selected_short), details_w - 24);
      fm_format_size(items[selected_index].size_bytes, items[selected_index].type,
                     selected_size, sizeof(selected_size));
      gui_draw_string(content_x + content_w - details_w + 8,
                      content_y + toolbar_h + 100, selected_short,
                      theme->file_text, theme->file_details);
      gui_draw_string(content_x + content_w - details_w + 8,
                      content_y + toolbar_h + 124,
                      fm_type_label(items[selected_index].name,
                                    items[selected_index].type),
                      0xA5B4FC, theme->file_details);
      gui_draw_string(content_x + content_w - details_w + 8,
                      content_y + toolbar_h + 148, selected_size,
                      theme->file_subtext, theme->file_details);
      gui_draw_string(content_x + content_w - details_w + 8,
                      content_y + toolbar_h + 172,
                      items[selected_index].type == 4 ? "Open: click twice"
                                                     : "Open: click twice",
                      theme->file_subtext, theme->file_details);
    } else {
      gui_draw_string(content_x + content_w - details_w + 8,
                      content_y + toolbar_h + 100, "Nothing selected",
                      theme->file_subtext, theme->file_details);
    }

    gui_draw_string(content_x + content_w - details_w + 8,
                    content_y + content_h - 40, "Quick access", 0x93C5FD,
                    theme->file_details);
    gui_draw_string(content_x + content_w - details_w + 8,
                    content_y + content_h - 22, "Root Desktop Docs",
                    theme->file_subtext, theme->file_details);

    if (st && st->context_menu_visible) {
      int menu_x = win->x + st->context_menu_x;
      int menu_y = win->y + st->context_menu_y;
      int menu_w = st->context_menu_target_on_item ? 132 : 120;
      int menu_h = st->context_menu_target_on_item ? 78 : 54;
      const char *row1 = st->context_menu_target_on_item ? "Open" : "New Folder";
      const char *row2 = st->context_menu_target_on_item ? "Rename" : "New File";
      const char *row3 = st->context_menu_target_on_item ? "Delete" : NULL;

      if (menu_x + menu_w > content_x + content_w)
        menu_x = content_x + content_w - menu_w;
      if (menu_y + menu_h > content_y + content_h)
        menu_y = content_y + content_h - menu_h;
      if (menu_x < content_x + 4)
        menu_x = content_x + 4;
      if (menu_y < content_y + 4)
        menu_y = content_y + 4;

      gui_fill_rect_alpha(menu_x + 2, menu_y + 2, menu_w, menu_h, 0x24000000);
      gui_draw_glass_panel(menu_x, menu_y, menu_w, menu_h, 0xD11A2230,
                           0x28FFFFFF, 0x80465C78, 1);

      gui_draw_rect(menu_x + 6, menu_y + 6, menu_w - 12, 20,
                    mouse_x >= menu_x + 6 && mouse_x < menu_x + menu_w - 6 &&
                            mouse_y >= menu_y + 6 && mouse_y < menu_y + 26
                        ? 0x1D4ED8
                        : 0x00000000);
      gui_draw_string(menu_x + 14, menu_y + 10, row1, 0xFFFFFF, 0x00000000);

      gui_draw_rect(menu_x + 6, menu_y + 30, menu_w - 12, 20,
                    mouse_x >= menu_x + 6 && mouse_x < menu_x + menu_w - 6 &&
                            mouse_y >= menu_y + 30 && mouse_y < menu_y + 50
                        ? 0x1D4ED8
                        : 0x00000000);
      gui_draw_string(menu_x + 14, menu_y + 34, row2, 0xFFFFFF, 0x00000000);

      if (row3) {
        gui_draw_rect(menu_x + 6, menu_y + 54, menu_w - 12, 20,
                      mouse_x >= menu_x + 6 &&
                              mouse_x < menu_x + menu_w - 6 &&
                              mouse_y >= menu_y + 54 && mouse_y < menu_y + 74
                          ? 0x7F1D1D
                          : 0x00000000);
        gui_draw_string(menu_x + 14, menu_y + 58, row3, 0xFECACA, 0x00000000);
      }
    }
  }
  /* Paint */
  else if (win->title[0] == 'P' && win->title[1] == 'a' &&
           win->title[2] == 'i') {
    /* Toolbar */
    gui_draw_rect(content_x, content_y, content_w, 32, theme->surface_alt);
    gui_draw_string(content_x + 8, content_y + 8,
                    "Brush [O]  Line [/]  Color:", theme->app_fg,
                    theme->surface_alt);
    /* Color palette */
    gui_draw_rect(content_x + 200, content_y + 4, 20, 20, 0xFF0000);
    gui_draw_rect(content_x + 224, content_y + 4, 20, 20, 0x00FF00);
    gui_draw_rect(content_x + 248, content_y + 4, 20, 20, 0x0000FF);
    gui_draw_rect(content_x + 272, content_y + 4, 20, 20, 0x000000);
    /* Canvas */
    gui_draw_rect(content_x + 4, content_y + 36, content_w - 8, content_h - 44,
                  theme->card);
  }
  /* Browser */
  else if (win->title[0] == 'B' && win->title[1] == 'r' &&
           win->title[2] == 'o') {
    uint32_t browser_toolbar =
        g_theme_mode == GUI_THEME_LIGHT ? 0xE7EEF7 : 0x1F2937;
    uint32_t browser_card = g_theme_mode == GUI_THEME_LIGHT ? 0xFFFFFF : 0x0F172A;
    uint32_t browser_text =
        g_theme_mode == GUI_THEME_LIGHT ? 0x172033 : 0xE5E7EB;
    uint32_t browser_muted =
        g_theme_mode == GUI_THEME_LIGHT ? 0x627084 : 0x94A3B8;
    /* Toolbar Background */
    gui_draw_rect(content_x, content_y, content_w, 40, browser_toolbar);

    /* Address Bar */
    gui_draw_rect(content_x + 80, content_y + 8, content_w - 96, 24, browser_card);
    gui_draw_rect_outline(content_x + 80, content_y + 8, content_w - 96, 24,
                          theme->border, 1);
    gui_draw_string(content_x + 88, content_y + 12, "http://os.de",
                    browser_text, browser_card);

    /* Navigation Buttons */
    gui_draw_string(content_x + 12, content_y + 12, "<", browser_muted, browser_toolbar);
    gui_draw_string(content_x + 35, content_y + 12, ">", browser_muted, browser_toolbar);
    gui_draw_string(content_x + 58, content_y + 12, "@", browser_muted,
                    browser_toolbar); /* Refresh */

    /* Web Content Area */
    gui_draw_rect(content_x, content_y + 40, content_w, content_h - 40,
                  browser_card);

    /* Mock Page Content */
    gui_draw_string(content_x + 20, content_y + 60, "Welcome to Browser",
                    browser_text, browser_card);
    gui_draw_rect(content_x + 20, content_y + 78, 200, 2,
                  0x007AFF); /* Underline */

    gui_draw_string(content_x + 20, content_y + 90, "Status:", browser_muted,
                    browser_card);
    gui_draw_string(content_x + 80, content_y + 90, "Networking Enabled",
                    0x16A34A, browser_card);

    gui_draw_string(content_x + 20, content_y + 110, "IP Addr:", browser_muted,
                    browser_card);
    gui_draw_string(content_x + 80, content_y + 110, "10.0.2.15 (DHCP)",
                    browser_text, browser_card);

    /* Fake links */
    gui_draw_string(content_x + 20, content_y + 150, "- Latest News", 0x007AFF,
                    0xFFFFFF);
    gui_draw_string(content_x + 20, content_y + 170, "- Documentation",
                    0x007AFF, browser_card);
    gui_draw_string(content_x + 20, content_y + 190, "- Source Code", 0x007AFF,
                    browser_card);
  }
  /* App Store */
  else if (win->title[0] == 'A' && win->title[1] == 'p' &&
           win->title[2] == 'p' && win->title[3] == ' ') {
    draw_app_store(content_x, content_y, content_w, content_h);
  }
  /* Installer */
  else if (win->title[0] == 'I' && win->title[1] == 'n' &&
           win->title[2] == 's' && win->title[3] == 't') {
    draw_installer_window(content_x, content_y, content_w, content_h);
  }
  /* Partition Manager */
  else if (win->title[0] == 'P' && win->title[1] == 'a' &&
           win->title[2] == 'r' && win->title[3] == 't') {
    draw_partition_manager_window(content_x, content_y, content_w, content_h);
  }
  else if (win == startup_window || startup_flow_active() ||
           (win->title[0] == 'A' && win->title[1] == 'c' &&
            win->title[2] == 'c') ||
           (win->title[0] == 'L' && win->title[1] == 'o' &&
            win->title[2] == 'g') ||
           (win->title[0] == 'W' && win->title[1] == 'e' &&
            win->title[2] == 'l') ||
           (win->title[0] == 'P' && win->title[1] == 'r' &&
            win->title[2] == 'e')) {
    draw_startup_auth_window(win, content_x, content_y, content_w, content_h);
  }
  /* Image Viewer */
  else if (win->title[0] == 'I' && win->title[1] == 'm' &&
           win->title[2] == 'a') {
    draw_image_viewer(win, content_x, content_y, content_w, content_h);
  }
  /* Help */
  else if (win->title[0] == 'H' && win->title[1] == 'e') {
    gui_draw_rect(content_x, content_y, content_w, content_h, theme->app_bg);
    int yy = content_y + 10;
    gui_draw_string(content_x + 10, yy, "OS8 Help", 0x89B4FA, THEME_BG);
    yy += 24;
    gui_draw_string(content_x + 10, yy, "Mouse:", 0xF9E2AF, THEME_BG);
    yy += 18;
    gui_draw_string(content_x + 20, yy, "- Click dock to launch apps", 0xCDD6F4,
                    THEME_BG);
    yy += 16;
    gui_draw_string(content_x + 20, yy, "- Drag titlebars to move", 0xCDD6F4,
                    THEME_BG);
    yy += 16;
    gui_draw_string(content_x + 20, yy, "- Click red button to close", 0xCDD6F4,
                    THEME_BG);
    yy += 24;
    gui_draw_string(content_x + 10, yy, "Terminal:", 0xF9E2AF, THEME_BG);
    yy += 18;
    gui_draw_string(content_x + 20, yy, "- Type 'help' for commands", 0xCDD6F4,
                    THEME_BG);
    yy += 16;
    gui_draw_string(content_x + 20, yy, "- Type 'neofetch' for info", 0xCDD6F4,
                    THEME_BG);
  }
  /* About window */
  else if (win->title[0] == 'A' && win->title[1] == 'b' &&
           win->title[2] == 'o') {
    const gui_theme_palette_t *theme = gui_theme_palette();
    char resolution[32];
    char windows_info[32];
    char uptime_info[32];
    char phys_mem_info[48];
    char heap_mem_info[40];
    const char *activation_status;
    const char *activation_detail;
    const char *gpu_status;
    const char *blur_status;
    int hero_x = content_x + 18;
    int hero_y = content_y + 18;
    int hero_w = content_w - 36;
    int hero_h = 96;
    int left_col_x = content_x + 18;
    int right_col_x = content_x + content_w / 2 + 6;
    int col_y = hero_y + hero_h + 14;
    int col_w = (content_w - 48) / 2;
    int info_h = 192;
    int footer_y = col_y + info_h + 14;
#ifdef ARCH_X86_64
    const char *arch_info = "Architecture:  x86_64";
#elif defined(ARCH_X86)
    const char *arch_info = "Architecture:  x86";
#else
    const char *arch_info = "Architecture:  ARM64";
#endif

    gui_build_display_mode_string(resolution, sizeof(resolution));
    build_windows_string(windows_info);
    ui_format_uptime_string(uptime_info, sizeof(uptime_info));
    ui_build_memory_strings(phys_mem_info, sizeof(phys_mem_info), heap_mem_info,
                            sizeof(heap_mem_info));
    activation_status = gui_activation_status_text();
    activation_detail = gui_activation_detail_text();
    if (gui_is_gpu_rendering_enabled()) {
      gpu_status = "GPU rendering active";
    } else if (str_cmp(g_gpu_backend_name, "bochs-vbe") == 0) {
      gpu_status = "Bochs/QEMU display backend active";
    } else if (str_cmp(g_gpu_backend_name, "framebuffer") == 0) {
      gpu_status = "Framebuffer display backend active";
    } else {
      gpu_status = "Software rendering active";
    }
    blur_status = gui_blur_effects_status_label();

    gui_draw_rect(content_x, content_y, content_w, content_h, theme->about_bg);
    gui_draw_rect(hero_x, hero_y, hero_w, hero_h, theme->about_hero);
    gui_draw_rect(hero_x, hero_y, 6, hero_h, theme->accent);
    gui_draw_os_logo(hero_x + 18, hero_y + 18, 3, 0xE2E8F0, 0x93C5FD,
                     theme->about_hero);
    gui_draw_string(hero_x + 78, hero_y + 18, "OS8", 0xFFFFFF, theme->about_hero);
    gui_draw_string(hero_x + 78, hero_y + 40, "Version 8.0.0", 0xBFDBFE,
                    theme->about_hero);
    gui_draw_string(hero_x + 78, hero_y + 62,
                    "Desktop compositor build with wallpapers, settings, and apps.",
                    0xCBD5E1, theme->about_hero);
    gui_draw_rect(hero_x + hero_w - 150, hero_y + 18, 118, 26, theme->accent);
    gui_draw_string(hero_x + hero_w - 128, hero_y + 27, "System About", 0xFFFFFF,
                    theme->accent);
    gui_draw_string(hero_x + hero_w - 136, hero_y + 58, windows_info, 0xA5B4FC,
                    theme->about_hero);

    gui_draw_rect(left_col_x, col_y, col_w, info_h, theme->about_card);
    gui_draw_rect(left_col_x, col_y, col_w, 3, theme->settings_border);
    gui_draw_string(left_col_x + 14, col_y + 14, "Platform", theme->about_text,
                    theme->about_card);
    gui_draw_string(left_col_x + 14, col_y + 38, arch_info, theme->about_subtext,
                    theme->about_card);
    gui_draw_string(left_col_x + 14, col_y + 58, "Kernel: OS8 v8.0.0",
                    theme->about_subtext, theme->about_card);
    gui_draw_string(left_col_x + 14, col_y + 78, "Desktop: Window compositor active",
                    theme->about_subtext, theme->about_card);
    gui_draw_string(left_col_x + 14, col_y + 98, "Graphics:", theme->about_subtext,
                    theme->about_card);
    gui_draw_string(left_col_x + 90, col_y + 98, gpu_status, theme->accent,
                    theme->about_card);
    gui_draw_string(left_col_x + 14, col_y + 118, "Build:",
                    theme->about_subtext, theme->about_card);
    gui_draw_string(left_col_x + 90, col_y + 118, BUILD_STRING, theme->accent,
                    theme->about_card);
    gui_draw_string(left_col_x + 14, col_y + 138, "Branch:", theme->about_subtext,
                    theme->about_card);
    gui_draw_string(left_col_x + 90, col_y + 138, BUILD_BRANCH, theme->accent,
                    theme->about_card);
    gui_draw_string(left_col_x + 14, col_y + 158, "Compiled:",
                    theme->about_subtext, theme->about_card);
    gui_draw_string(left_col_x + 90, col_y + 158, BUILD_COMPILE_TIME,
                    theme->accent, theme->about_card);

    gui_draw_rect(right_col_x, col_y, col_w, info_h, theme->about_card);
    gui_draw_rect(right_col_x, col_y, col_w, 3, theme->accent);
    gui_draw_string(right_col_x + 14, col_y + 14, "Session", theme->about_text,
                    theme->about_card);
    gui_draw_string(right_col_x + 14, col_y + 38, "Display:", theme->about_subtext,
                    theme->about_card);
    gui_draw_string(right_col_x + 82, col_y + 38, resolution, theme->about_subtext,
                    theme->about_card);
    gui_draw_string(right_col_x + 14, col_y + 58, "Backend:", theme->about_subtext,
                    theme->about_card);
    gui_draw_string(right_col_x + 82, col_y + 58, g_gpu_backend_name,
                    theme->about_subtext, theme->about_card);
    gui_draw_string(right_col_x + 14, col_y + 78, "Effects:", theme->about_subtext,
                    theme->about_card);
    gui_draw_string(right_col_x + 82, col_y + 78, blur_status, theme->accent_soft,
                    theme->about_card);
    gui_draw_string(right_col_x + 14, col_y + 98, "Runtime:", theme->about_subtext,
                    theme->about_card);
    gui_draw_string(right_col_x + 82, col_y + 98, uptime_info, theme->about_subtext,
                    theme->about_card);
    gui_draw_string(right_col_x + 14, col_y + 118, "Memory:", theme->about_subtext,
                    theme->about_card);
    gui_draw_string(right_col_x + 82, col_y + 118, phys_mem_info,
                    theme->about_subtext, theme->about_card);
    gui_draw_string(right_col_x + 14, col_y + 138, "Heap:", theme->about_subtext,
                    theme->about_card);
    gui_draw_string(right_col_x + 82, col_y + 138, heap_mem_info,
                    theme->about_subtext, theme->about_card);
    gui_draw_string(right_col_x + 14, col_y + 158, "Activation:",
                    theme->about_subtext, theme->about_card);
    gui_draw_string(right_col_x + 82, col_y + 158, activation_status,
                    g_activation_state.activated ? theme->accent
                                                 : theme->about_subtext,
                    theme->about_card);

    gui_draw_rect(hero_x, footer_y, hero_w, 52, theme->about_footer);
    gui_draw_string(hero_x + 14, footer_y + 10, "Build UUID", theme->about_text,
                    theme->about_footer);
    gui_draw_string(hero_x + 14, footer_y + 28, BUILD_UUID, theme->about_subtext,
                    theme->about_footer);
    gui_draw_string(hero_x + 180, footer_y + 28, activation_detail,
                    theme->about_subtext, theme->about_footer);
  }
  /* Settings window */
  else if (win->title[0] == 'S' && win->title[1] == 'e' &&
           win->title[2] == 't') {
    const gui_theme_palette_t *theme = gui_theme_palette();
    char resolution[32];
    char windows_info[32];
    char uptime_info[32];
    char phys_mem_info[48];
    char heap_mem_info[40];
    const char *blur_status;
    const char *gpu_status;
    const char *theme_status =
        g_theme_mode == GUI_THEME_LIGHT ? "Theme: Light mode active"
                                        : "Theme: Dark mode active";
    extern int intel_hda_is_playing(void);
    int sidebar_w = 132;
    int divider_x = content_x + sidebar_w;
    int panel_x = divider_x + 3;
    int panel_y = content_y + 42;
    int panel_w = content_w - sidebar_w - 3;
    int panel_h = content_h - 42;
    int card_w = (panel_w - 30) / 2;
    int compact_settings = panel_w < 470;
    int toolbar_y = content_y + 8;
    int toolbar_h = 24;
    int toolbar_x = content_x + 154;
    int toolbar_gap = 8;
    int sidebar_track_x = divider_x - 10;
    int sidebar_track_y = content_y + 56;
    int sidebar_track_h = SETTINGS_MENU_COUNT * 28 - 10;
    int sidebar_thumb_h = 18;
    int sidebar_thumb_y = sidebar_track_y;
    int panel_scroll_x = panel_x + panel_w - 14;
    int panel_scroll_y = panel_y + 14;
    int panel_scroll_h = panel_h - 28;
    int panel_thumb_h = 42;
    int panel_thumb_y = panel_scroll_y;
    int dock_count_buf_idx = 0;
    char dock_count_buf[24];
    char installed_buf[24];
    int installed_apps = 0;

    gui_build_display_mode_string(resolution, sizeof(resolution));
    build_windows_string(windows_info);
    ui_format_uptime_string(uptime_info, sizeof(uptime_info));
    ui_build_memory_strings(phys_mem_info, sizeof(phys_mem_info), heap_mem_info,
                            sizeof(heap_mem_info));
    load_dock_config();
    for (int i = 0; i < app_catalog_count; i++) {
      if (app_is_installed(&app_catalog[i]))
        installed_apps++;
    }
    blur_status = gui_blur_effects_status_label();
    if (gui_is_gpu_rendering_enabled()) {
      gpu_status = "GPU rendering active";
    } else if (str_cmp(g_gpu_backend_name, "bochs-vbe") == 0) {
      gpu_status = "Bochs/QEMU display backend active";
    } else if (str_cmp(g_gpu_backend_name, "framebuffer") == 0) {
      gpu_status = "Framebuffer display backend active";
    } else {
      gpu_status = "Software rendering active";
    }

    dock_count_buf[0] = '\0';
    append_decimal(dock_count_buf, &dock_count_buf_idx, dock_item_count);
    notepad_append_to_buf(dock_count_buf, sizeof(dock_count_buf), " dock apps");
    installed_buf[0] = '\0';
    dock_count_buf_idx = 0;
    append_decimal(installed_buf, &dock_count_buf_idx, installed_apps);
    notepad_append_to_buf(installed_buf, sizeof(installed_buf), " installed");
    if (SETTINGS_MENU_COUNT > 1) {
      sidebar_thumb_y +=
          ((sidebar_track_h - sidebar_thumb_h) * settings_active_tab) /
          (SETTINGS_MENU_COUNT - 1);
      panel_thumb_y +=
          ((panel_scroll_h - panel_thumb_h) * settings_active_tab) /
          (SETTINGS_MENU_COUNT - 1);
    }

    gui_draw_rect(content_x, content_y, content_w, content_h, theme->settings_bg);
    gui_draw_rect(content_x, content_y, content_w, 3, theme->settings_border);
    gui_draw_rect(content_x, content_y + 39, content_w, 3, theme->settings_border);
    gui_draw_rect(divider_x, content_y + 39, 3, content_h - 39,
                  theme->settings_border);
    gui_draw_string(content_x + 18, content_y + 15, "OS SETTINGS",
                    theme->settings_text, theme->settings_bg);
    gui_draw_system_button(toolbar_x, toolbar_y, 98, toolbar_h, "Backgrounds",
                           GUI_BUTTON_PRIMARY, 1, 0);
    gui_draw_system_button(toolbar_x + 98 + toolbar_gap, toolbar_y, 88, toolbar_h,
                           "Theme Lab", GUI_BUTTON_NEUTRAL, 1,
                           settings_active_tab == 11);
    gui_draw_system_button(toolbar_x + 194 + toolbar_gap * 2, toolbar_y, 66,
                           toolbar_h, "Files", GUI_BUTTON_NEUTRAL, 1, 0);
    gui_draw_system_button(toolbar_x + 268 + toolbar_gap * 3, toolbar_y, 82,
                           toolbar_h, "Sidebar", GUI_BUTTON_NEUTRAL, 1,
                           settings_active_tab == 10);
    gui_draw_system_button(toolbar_x + 358 + toolbar_gap * 4, toolbar_y, 72,
                           toolbar_h, "About", GUI_BUTTON_NEUTRAL, 1, 0);
    gui_draw_rect(sidebar_track_x, sidebar_track_y, 4, sidebar_track_h, 0x334155);
    gui_draw_rect(sidebar_track_x, sidebar_thumb_y, 4, sidebar_thumb_h, theme->accent);

    for (int i = 0; i < SETTINGS_MENU_COUNT; i++) {
      int tab_y = content_y + 56 + i * 28;
      uint32_t tab_bg = i == settings_active_tab ? theme->settings_tab_active
                                                 : theme->settings_tab_inactive;
      uint32_t tab_fg = i == settings_active_tab ? 0xFFFFFF
                                                 : gui_contrast_title_color(tab_bg);
      gui_draw_rect(content_x + 10, tab_y, sidebar_w - 20, 18, tab_bg);
      gui_draw_rect_outline(content_x + 10, tab_y, sidebar_w - 20, 18,
                            i == settings_active_tab ? theme->accent
                                                      : theme->settings_border,
                            1);
      gui_draw_string(content_x + 16, tab_y + 5, settings_menu_labels[i], tab_fg,
                      tab_bg);
    }

    gui_draw_rect(panel_x, panel_y, panel_w - 3, panel_h - 3, theme->settings_panel);
    gui_draw_rect(panel_scroll_x, panel_scroll_y, 4, panel_scroll_h, 0x233044);
    gui_draw_rect(panel_scroll_x, panel_thumb_y, 4, panel_thumb_h, theme->accent_soft);
    gui_draw_string(panel_x + 18, panel_y + 16,
                    settings_page_heading(settings_active_tab), theme->settings_text,
                    theme->settings_panel);
    gui_draw_string(panel_x + 18, panel_y + 34, settings_status,
                    theme->settings_subtext, theme->settings_panel);

    if (settings_active_tab == 0) {
      int card_y = panel_y + 72;
      int info_card_h = compact_settings ? 88 : 72;
      int stat_card_h = compact_settings ? 88 : 74;
      int runtime_card_h = compact_settings ? 88 : 74;
      int action_y;

      gui_draw_rect(panel_x, card_y, panel_w, info_card_h, 0x252535);
      gui_draw_string(panel_x + 16, card_y + 12, "Display", 0x93C5FD, 0x252535);
      gui_draw_string(panel_x + 16, card_y + 32, resolution, 0xFFFFFF, 0x252535);
      if (compact_settings) {
        gui_draw_string(panel_x + 16, card_y + 50, windows_info, 0xCBD5E1,
                        0x252535);
        gui_draw_string(panel_x + 16, card_y + 66, dock_count_buf, 0xA5B4FC,
                        0x252535);
        gui_draw_string(panel_x + 180, card_y + 66, installed_buf, 0xA5F3FC,
                        0x252535);
      } else {
        gui_draw_string(panel_x + 180, card_y + 32, windows_info, 0xCBD5E1,
                        0x252535);
        gui_draw_string(panel_x + 16, card_y + 50, dock_count_buf, 0xA5B4FC,
                        0x252535);
        gui_draw_string(panel_x + 180, card_y + 50, installed_buf, 0xA5F3FC,
                        0x252535);
      }

      card_y += info_card_h + 12;
      if (compact_settings) {
        gui_draw_rect(panel_x, card_y, panel_w, stat_card_h, 0x252535);
        gui_draw_string(panel_x + 16, card_y + 12, "Graphics", 0x89B4FA,
                        0x252535);
        gui_draw_string(panel_x + 16, card_y + 32, gpu_status, 0xFFFFFF,
                        0x252535);
        gui_draw_string(panel_x + 16, card_y + 50, blur_status, 0xCBD5E1,
                        0x252535);

        card_y += stat_card_h + 12;
        gui_draw_rect(panel_x, card_y, panel_w, stat_card_h, 0x252535);
        gui_draw_string(panel_x + 16, card_y + 12, "Media & Network",
                        0x89B4FA, 0x252535);
        gui_draw_string(panel_x + 16, card_y + 32,
                        intel_hda_is_playing() ? "Audio is currently playing"
                                               : "Audio backend standing by",
                        intel_hda_is_playing() ? 0xA6E3A1 : 0xFFFFFF, 0x252535);
        gui_draw_string(panel_x + 16, card_y + 50,
                        wifi_has_supported_adapter()
                            ? wifi_get_status_text()
                            : "virtio-net online with user-mode NAT",
                        0xCBD5E1, 0x252535);

        card_y += stat_card_h + 12;
        gui_draw_rect(panel_x, card_y, panel_w, runtime_card_h, 0x252535);
        gui_draw_string(panel_x + 16, card_y + 12, "Runtime", 0x89B4FA,
                        0x252535);
        gui_draw_string(panel_x + 16, card_y + 32, uptime_info, 0xFFFFFF,
                        0x252535);
        gui_draw_string(panel_x + 16, card_y + 50, phys_mem_info, 0xCBD5E1,
                        0x252535);

        card_y += runtime_card_h + 12;
        gui_draw_rect(panel_x, card_y, panel_w, runtime_card_h, 0x252535);
        gui_draw_string(panel_x + 16, card_y + 12, "Memory", 0x89B4FA,
                        0x252535);
        gui_draw_string(panel_x + 16, card_y + 32, heap_mem_info, 0xFFFFFF,
                        0x252535);
        gui_draw_string(panel_x + 16, card_y + 50, "Live heap usage tracking",
                        0xCBD5E1, 0x252535);
      } else {
        gui_draw_rect(panel_x, card_y, card_w, stat_card_h, 0x252535);
        gui_draw_string(panel_x + 16, card_y + 12, "Graphics", 0x89B4FA,
                        0x252535);
        gui_draw_string(panel_x + 16, card_y + 32, gpu_status, 0xFFFFFF,
                        0x252535);
        gui_draw_string(panel_x + 16, card_y + 50, blur_status, 0xCBD5E1,
                        0x252535);

        gui_draw_rect(panel_x + card_w + 12, card_y, card_w, stat_card_h,
                      0x252535);
        gui_draw_string(panel_x + card_w + 28, card_y + 12, "Media & Network",
                        0x89B4FA, 0x252535);
        gui_draw_string(panel_x + card_w + 28, card_y + 32,
                        intel_hda_is_playing() ? "Audio is currently playing"
                                               : "Audio backend standing by",
                        intel_hda_is_playing() ? 0xA6E3A1 : 0xFFFFFF, 0x252535);
        gui_draw_string(panel_x + card_w + 28, card_y + 50,
                        wifi_has_supported_adapter()
                            ? wifi_get_status_text()
                            : "virtio-net online with user-mode NAT",
                        0xCBD5E1, 0x252535);

        card_y += stat_card_h + 14;
        gui_draw_rect(panel_x, card_y, card_w, runtime_card_h, 0x252535);
        gui_draw_string(panel_x + 16, card_y + 12, "Runtime", 0x89B4FA,
                        0x252535);
        gui_draw_string(panel_x + 16, card_y + 32, uptime_info, 0xFFFFFF,
                        0x252535);
        gui_draw_string(panel_x + 16, card_y + 50, phys_mem_info, 0xCBD5E1,
                        0x252535);

        gui_draw_rect(panel_x + card_w + 12, card_y, card_w, runtime_card_h,
                      0x252535);
        gui_draw_string(panel_x + card_w + 28, card_y + 12, "Memory", 0x89B4FA,
                        0x252535);
        gui_draw_string(panel_x + card_w + 28, card_y + 32, heap_mem_info,
                        0xFFFFFF, 0x252535);
        gui_draw_string(panel_x + card_w + 28, card_y + 50,
                        "Live heap usage tracking", 0xCBD5E1, 0x252535);
      }

      action_y = card_y + runtime_card_h + 12;
      if (compact_settings) {
        gui_draw_system_button(panel_x, action_y, 108, 28, "Backgrounds",
                               GUI_BUTTON_PRIMARY, 1, 0);
        gui_draw_system_button(panel_x + 118, action_y, 98, 28, "App Store",
                               GUI_BUTTON_PRIMARY, 1, 0);
        gui_draw_system_button(panel_x + 226, action_y, 92, 28, "Devices",
                               GUI_BUTTON_PRIMARY, 1, 0);
        gui_draw_system_button(panel_x, action_y + 36, 84, 28, "About",
                               GUI_BUTTON_NEUTRAL, 1, 0);
      } else {
        gui_draw_system_button(panel_x, action_y, 108, 30, "Backgrounds",
                               GUI_BUTTON_PRIMARY, 1, 0);
        gui_draw_system_button(panel_x + 118, action_y, 98, 30, "App Store",
                               GUI_BUTTON_PRIMARY, 1, 0);
        gui_draw_system_button(panel_x + 226, action_y, 92, 30, "Devices",
                               GUI_BUTTON_PRIMARY, 1, 0);
        gui_draw_system_button(panel_x + 328, action_y, 84, 30, "About",
                               GUI_BUTTON_NEUTRAL, 1, 0);
      }
    } else if (settings_active_tab == 1) {
      int info_y = panel_y + 72;
      int button_y = panel_y + 118;
      int password_y = panel_y + 154;
      int list_y = panel_y + 196;
      int network_count = wifi_get_network_count();
      int selected_network = wifi_get_selected_network();
      int selected_secure =
          selected_network >= 0 ? wifi_get_network_secure(selected_network) : 0;
      char masked_wifi_password[32];

      mask_secret(wifi_password_draft, masked_wifi_password,
                  sizeof(masked_wifi_password));

      gui_draw_rect(panel_x, info_y, panel_w - 18, 76, 0x252535);
      gui_draw_string(panel_x + 16, info_y + 12, "Wi-Fi Adapter", 0x89B4FA,
                      0x252535);
      gui_draw_string(panel_x + 16, info_y + 32, wifi_get_adapter_name(),
                      wifi_has_supported_adapter() ? 0xFFFFFF : 0xF38BA8,
                      0x252535);
      gui_draw_string(panel_x + 16, info_y + 50, wifi_get_driver_name(),
                      0xCBD5E1, 0x252535);

      gui_draw_system_button(panel_x, button_y, 88, 28, "Scan",
                             GUI_BUTTON_PRIMARY,
                             wifi_has_supported_adapter() &&
                                 wifi_supports_real_scanning(),
                             0);
      gui_draw_system_button(panel_x + 98, button_y, 102, 28, "Connect",
                             GUI_BUTTON_SUCCESS,
                             wifi_can_connect_selected(), 0);
      gui_draw_system_button(panel_x + 210, button_y, 118, 28, "Disconnect",
                             GUI_BUTTON_DANGER, wifi_is_connected(), 0);

      gui_draw_string(panel_x + 340, button_y + 8,
                      wifi_is_connected() ? wifi_get_connected_ssid()
                                          : "Not connected",
                      wifi_is_connected() ? 0xA6E3A1 : 0x4A4A4A, 0xF8F8F8);

      if (selected_secure) {
        uint32_t field_bg = settings_wifi_password_active ? 0x31314A : 0x232337;
        gui_draw_rect(panel_x, password_y, panel_w - 18, 28, field_bg);
        gui_draw_string(panel_x + 12, password_y + 8,
                        masked_wifi_password[0] ? masked_wifi_password
                                                : "Wi-Fi password",
                        masked_wifi_password[0] ? 0xFFFFFF : 0x6C7086, field_bg);
      }

      if (!wifi_has_supported_adapter()) {
        gui_draw_rect(panel_x, list_y, panel_w - 18, 82, 0x252535);
        gui_draw_string(panel_x + 16, list_y + 18,
                        "No supported PCI Wi-Fi adapter was detected.",
                        0xFFFFFF, 0x252535);
        gui_draw_string(panel_x + 16, list_y + 40,
                        "The wireless menu is ready, but native adapter",
                        0xCBD5E1, 0x252535);
        gui_draw_string(panel_x + 16, list_y + 56,
                        "bring-up is still deferred to keep boot safe.",
                        0xCBD5E1, 0x252535);
      } else if (!wifi_supports_real_scanning()) {
        gui_draw_rect(panel_x, list_y, panel_w - 18, 82, 0x252535);
        gui_draw_string(panel_x + 16, list_y + 18,
                        "Adapter detected, but real scanning is not ready yet.",
                        0xFFFFFF, 0x252535);
        gui_draw_string(panel_x + 16, list_y + 40,
                        "This build now waits for driver-fed scan results",
                        0xCBD5E1, 0x252535);
        gui_draw_string(panel_x + 16, list_y + 56,
                        "instead of inventing nearby Wi-Fi networks.",
                        0xCBD5E1, 0x252535);
      } else if (network_count <= 0) {
        gui_draw_rect(panel_x, list_y, panel_w - 18, 82, 0x252535);
        gui_draw_string(panel_x + 16, list_y + 18,
                        "No nearby Wi-Fi networks were reported.",
                        0xFFFFFF, 0x252535);
        gui_draw_string(panel_x + 16, list_y + 40,
                        "Run Scan to refresh the active wireless survey list.",
                        0xCBD5E1, 0x252535);
        gui_draw_string(panel_x + 16, list_y + 56,
                        "Connect unlocks when the backend returns entries.",
                        0xCBD5E1, 0x252535);
      } else {
        int i;

        for (i = 0; i < network_count; i++) {
          int row_y = list_y + i * 36;
          uint32_t row_bg =
              i == wifi_get_selected_network() ? 0x1F2937 : 0x252535;
          uint32_t row_border = i == wifi_get_selected_network() ? 0x89B4FA
                                                                 : 0x364152;
          char signal_buf[32] = "";

          gui_draw_rect(panel_x, row_y, panel_w - 18, 30, row_bg);
          gui_draw_rect(panel_x, row_y, panel_w - 18, 1, row_border);
          append_uint_to_buf(signal_buf, sizeof(signal_buf),
                             wifi_get_network_signal(i));
          notepad_append_to_buf(signal_buf, sizeof(signal_buf), "%");

          gui_draw_string(panel_x + 12, row_y + 8, wifi_get_network_ssid(i),
                          0xFFFFFF, row_bg);
          gui_draw_string(panel_x + 192, row_y + 8,
                          wifi_is_network_connected(i)
                              ? "Connected"
                              : (wifi_get_network_secure(i) ? "Secured" : "Open"),
                          wifi_is_network_connected(i)
                              ? 0x89B4FA
                              : (wifi_get_network_secure(i) ? 0xA6E3A1
                                                            : 0xF9E2AF),
                          row_bg);
          gui_draw_string(panel_x + 272, row_y + 8, signal_buf, 0xCBD5E1,
                          row_bg);
        }
      }
    } else if (settings_active_tab == 2) {
      gui_draw_string(panel_x + 18, panel_y + 72,
                      account_partition_label[0] ? account_partition_label
                                                 : "No dedicated user partition",
                      0x111111, 0xF8F8F8);
      gui_draw_string(panel_x + 18, panel_y + 92,
                      account_disk_location[0] ? account_disk_location
                                               : "Using local session storage",
                      0x4A4A4A, 0xF8F8F8);
      gui_draw_rect(panel_x + 18, panel_y + 126, 110, 30, 0x111111);
      gui_draw_string(panel_x + 40, panel_y + 135, "Devices", 0xF2F2F2, 0x111111);
      gui_draw_rect(panel_x + 138, panel_y + 126, 96, 30, 0x111111);
      gui_draw_string(panel_x + 172, panel_y + 135, "Files", 0xF2F2F2, 0x111111);
      gui_draw_rect(panel_x + 242, panel_y + 126, 112, 30, 0x111111);
      gui_draw_string(panel_x + 266, panel_y + 135, "Disk Imager", 0xF2F2F2,
                      0x111111);
    } else if (settings_active_tab == 3) {
      int preview_x = panel_x;
      int preview_y = panel_y + 72;
      int preview_w = 180;
      int preview_h = 90;
      int resolution_card_y;

      settings_sync_resolution_picker();

      gui_draw_rect(preview_x, preview_y, preview_w, preview_h, 0x252535);
      load_thumbnails();
      if (wallpapers[current_wallpaper].type == 1 &&
          thumbnail_cache[current_wallpaper].pixels) {
        media_image_t *thumb_img = &thumbnail_cache[current_wallpaper];
        gui_draw_image_scaled(preview_x + 8, preview_y + 8, preview_w - 16,
                              preview_h - 16, thumb_img);
      } else {
        for (int py = 0; py < preview_h - 16; py++) {
          uint8_t pr = wallpapers[current_wallpaper].tr +
                       ((wallpapers[current_wallpaper].br -
                         wallpapers[current_wallpaper].tr) *
                        py) /
                           (preview_h - 16);
          uint8_t pg = wallpapers[current_wallpaper].tg +
                       ((wallpapers[current_wallpaper].bg -
                         wallpapers[current_wallpaper].tg) *
                        py) /
                           (preview_h - 16);
          uint8_t pb = wallpapers[current_wallpaper].tb +
                       ((wallpapers[current_wallpaper].bb -
                         wallpapers[current_wallpaper].tb) *
                        py) /
                           (preview_h - 16);
          gui_draw_rect(preview_x + 8, preview_y + 8 + py, preview_w - 16, 1,
                        (pr << 16) | (pg << 8) | pb);
        }
      }

      gui_draw_rect(panel_x + 194, preview_y, panel_w - 194, preview_h, 0x252535);
      gui_draw_string(panel_x + 210, preview_y + 14, "Current wallpaper", 0x93C5FD,
                      0x252535);
      gui_draw_string(panel_x + 210, preview_y + 36, wallpapers[current_wallpaper].name,
                      0xFFFFFF, 0x252535);
      gui_draw_string(panel_x + 210, preview_y + 56,
                      wallpapers[current_wallpaper].type == 1 ? "Photo-based scene"
                                                              : "Gradient theme",
                      0xCBD5E1, 0x252535);
      gui_draw_string(panel_x + 210, preview_y + 72, blur_status, 0xA5B4FC, 0x252535);

      preview_y += 104;
      gui_draw_rect(panel_x, preview_y, panel_w, 104, 0x252535);
      gui_draw_string(panel_x + 16, preview_y + 12, "Visual effects", 0x89B4FA,
                      0x252535);
      gui_draw_string(panel_x + 16, preview_y + 30, gpu_status, 0xFFFFFF, 0x252535);
      gui_draw_string(panel_x + 16, preview_y + 46, g_gpu_backend_name, 0xCBD5E1,
                      0x252535);
      gui_draw_string(panel_x + 200, preview_y + 30, blur_status, 0xA5B4FC,
                      0x252535);
      gui_draw_string(panel_x + 200, preview_y + 46, theme_status, 0xF9A8D4,
                      0x252535);
      gui_draw_string(panel_x + 200, preview_y + 62,
                      dock_is_visible() ? "Dock is visible on this boot mode"
                                        : "Dock hidden in current mode",
                      0xCBD5E1, 0x252535);
      gui_draw_string(panel_x + 16, preview_y + 62,
                      gui_partial_redraw_clear_debug_enabled()
                          ? "Dirty test running"
                          : "Dirty test ready",
                      gui_partial_redraw_clear_debug_enabled() ? 0xF38BA8
                                                               : 0xCBD5E1,
                      0x252535);
      gui_draw_rect(panel_x + 16, preview_y + 72, 110, 22,
                    g_theme_mode == GUI_THEME_LIGHT ? 0x2563EB : 0x475569);
      gui_draw_string(panel_x + 44, preview_y + 79, "Light Mode", 0xFFFFFF,
                      g_theme_mode == GUI_THEME_LIGHT ? 0x2563EB : 0x475569);
      gui_draw_rect(panel_x + 136, preview_y + 72, 110, 22,
                    g_theme_mode == GUI_THEME_DARK ? 0x111827 : 0x475569);
      gui_draw_string(panel_x + 170, preview_y + 79, "Dark Mode", 0xFFFFFF,
                      g_theme_mode == GUI_THEME_DARK ? 0x111827 : 0x475569);
      gui_draw_system_button(panel_x + 260, preview_y + 72, 150, 22,
                             gui_partial_redraw_clear_debug_enabled()
                                 ? "Clear Test On"
                                 : "Test Dirty Rects",
                             gui_partial_redraw_clear_debug_enabled()
                                 ? GUI_BUTTON_DANGER
                                 : GUI_BUTTON_NEUTRAL,
                             1, gui_partial_redraw_clear_debug_enabled());

      resolution_card_y = preview_y + 116;
      gui_draw_rect(panel_x, resolution_card_y, panel_w, 96, 0x252535);
      gui_draw_string(panel_x + 16, resolution_card_y + 12, "Display resolution",
                      0x89B4FA, 0x252535);
      gui_draw_string(panel_x + 16, resolution_card_y + 28, "Current:", 0xCBD5E1,
                      0x252535);
      gui_draw_string(panel_x + 76, resolution_card_y + 28, resolution, 0xFFFFFF,
                      0x252535);
      gui_draw_string(panel_x + 210, resolution_card_y + 28, "Selected:", 0xCBD5E1,
                      0x252535);
      gui_draw_string(panel_x + 282, resolution_card_y + 28,
                      settings_resolution_options[settings_resolution_pending_idx].label,
                      0xA5B4FC, 0x252535);
      gui_draw_string(panel_x + 16, resolution_card_y + 44, "Boot:", 0xCBD5E1,
                      0x252535);
      gui_draw_string(panel_x + 64, resolution_card_y + 44,
                      settings_resolution_saved_idx >= 0
                          ? settings_resolution_options[settings_resolution_saved_idx].label
                          : "Current default",
                      0xFFFFFF, 0x252535);
      gui_draw_string(panel_x + 210, resolution_card_y + 44,
                      gui_can_apply_resolution_live()
                          ? "Apply now or save for reboot"
                          : "Live switching unavailable on this display backend",
                      0x94A3B8, 0x252535);

      for (int i = 0; i < SETTINGS_RESOLUTION_OPTION_COUNT; i++) {
        int bx, by, bw, bh;
        settings_resolution_button_bounds(panel_x, panel_y, i, &bx, &by, &bw, &bh);
        gui_draw_system_button(
            bx, by, bw, bh, settings_resolution_options[i].label,
            i == settings_resolution_pending_idx ? GUI_BUTTON_PRIMARY
                                                 : GUI_BUTTON_NEUTRAL,
            1, i == settings_resolution_current_idx);
      }

      gui_draw_system_button(panel_x + 8, resolution_card_y + 66, 90, 24,
                             "Wallpapers", GUI_BUTTON_PRIMARY, 1, 0);
      gui_draw_system_button(panel_x + 106, resolution_card_y + 66, 90, 24,
                             gui_blur_effects_button_label(),
                             g_blur_effects_mode == GUI_BLUR_EFFECTS_FORCE
                                 ? GUI_BUTTON_SUCCESS
                                 : (gui_blur_effects_requested()
                                        ? GUI_BUTTON_PRIMARY
                                        : GUI_BUTTON_NEUTRAL),
                             1, gui_blur_effects_requested());
      gui_draw_system_button(panel_x + 204, resolution_card_y + 66, 90, 24,
                             gui_is_gpu_rendering_enabled() ? "GPU On" : "GPU Off",
                             gui_is_gpu_rendering_enabled() ? GUI_BUTTON_SUCCESS
                                                            : GUI_BUTTON_NEUTRAL,
                             1, gui_is_gpu_rendering_enabled());
      gui_draw_system_button(panel_x + 302, resolution_card_y + 66, 90, 24,
                             "Apply", GUI_BUTTON_NEUTRAL,
                             gui_can_apply_resolution_live(), 0);
      gui_draw_system_button(panel_x + 400, resolution_card_y + 66, 96, 24,
                             "On Reboot", GUI_BUTTON_NEUTRAL, 1, 0);
    } else if (settings_active_tab == 4) {
      int row_y = panel_y + 72;
      gui_draw_rect(panel_x, row_y, panel_w, 82, 0x252535);
      gui_draw_string(panel_x + 16, row_y + 12, "App quick launch", 0x89B4FA,
                      0x252535);
      gui_draw_string(panel_x + 16, row_y + 32,
                      "Use these launchers to jump straight into the most useful system tools.",
                      0xFFFFFF, 0x252535);
      gui_draw_string(panel_x + 16, row_y + 52, installed_buf, 0xCBD5E1,
                      0x252535);

      row_y += 96;
      gui_draw_system_button(panel_x + 16, row_y, 110, 30, "App Store",
                             GUI_BUTTON_PRIMARY, 1, 0);
      gui_draw_system_button(panel_x + 136, row_y, 96, 30, "Files",
                             GUI_BUTTON_NEUTRAL, 1, 0);
      gui_draw_system_button(panel_x + 242, row_y, 108, 30, "Devices",
                             GUI_BUTTON_NEUTRAL, 1, 0);
      gui_draw_system_button(panel_x + 360, row_y, 118, 30, "Disk Imager",
                             GUI_BUTTON_NEUTRAL, 1, 0);

      row_y += 44;
      gui_draw_rect(panel_x, row_y, panel_w, 88, 0x252535);
      gui_draw_string(panel_x + 16, row_y + 12, "Library", 0x89B4FA, 0x252535);
      gui_draw_string(panel_x + 16, row_y + 32, "Installed apps and utilities can be launched here without leaving Settings.",
                      0xFFFFFF, 0x252535);
      gui_draw_string(panel_x + 16, row_y + 52, "Open Files to inspect packages or App Store to add more tools.",
                      0xCBD5E1, 0x252535);
    } else if (settings_active_tab == 8) {
      int block_y = panel_y + 72;
      char sidebar_layout_buf[32];
      char sidebar_width_buf[32];
      int sidebar_idx = 0;
      int sidebar_visible = desktop_sidebar_is_visible();
      int sidebar_side = desktop_sidebar_get_side();
      int sidebar_width = desktop_sidebar_get_width();

      sidebar_layout_buf[0] = '\0';
      if (sidebar_visible) {
        str_copy_safe(sidebar_layout_buf,
                      sidebar_side == DESKTOP_SIDEBAR_RIGHT ? "Visible on right"
                                                           : "Visible on left",
                      sizeof(sidebar_layout_buf));
      } else {
        str_copy_safe(sidebar_layout_buf, "Hidden", sizeof(sidebar_layout_buf));
      }
      sidebar_width_buf[0] = '\0';
      append_decimal(sidebar_width_buf, &sidebar_idx, sidebar_width);
      notepad_append_to_buf(sidebar_width_buf, sizeof(sidebar_width_buf), " px");

      gui_draw_rect(panel_x, block_y, panel_w, 76, 0x252535);
      gui_draw_string(panel_x + 16, block_y + 12, "System status", 0x89B4FA,
                      0x252535);
      gui_draw_string(panel_x + 16, block_y + 32, g_gpu_backend_name, 0xFFFFFF,
                      0x252535);
      gui_draw_string(panel_x + 150, block_y + 32,
                      intel_hda_is_playing() ? "Intel HDA streaming"
                                             : "Intel HDA ready",
                      0xCBD5E1, 0x252535);
      gui_draw_string(panel_x + 16, block_y + 52, "virtio-net / file manager / app store",
                      0xCBD5E1, 0x252535);

      block_y += 90;
      gui_draw_rect(panel_x, block_y, 110, 30, 0x3B82F6);
      gui_draw_string(panel_x + 22, block_y + 9, "Devices", 0xFFFFFF, 0x3B82F6);
      gui_draw_rect(panel_x + 120, block_y, 110, 30, 0x1D4ED8);
      gui_draw_string(panel_x + 146, block_y + 9, "Files", 0xFFFFFF, 0x1D4ED8);
      gui_draw_rect(panel_x + 240, block_y, 110, 30, 0x2563EB);
      gui_draw_string(panel_x + 264, block_y + 9, "App Store", 0xFFFFFF, 0x2563EB);
      gui_draw_rect(panel_x + 360, block_y, 110, 30, 0x6D28D9);
      gui_draw_string(panel_x + 374, block_y + 9, "Reset Dock", 0xFFFFFF, 0x6D28D9);

      block_y += 42;
      gui_draw_rect(panel_x, block_y, 110, 30, 0x4B5563);
      gui_draw_string(panel_x + 32, block_y + 9, "About", 0xFFFFFF, 0x4B5563);
      gui_draw_rect(panel_x + 120, block_y, panel_w - 120, 30, 0x1E293B);
      gui_draw_string(panel_x + 136, block_y + 9, "Use these tools to inspect and restore",
                      0xCBD5E1, 0x1E293B);

      block_y += 46;
      gui_draw_rect(panel_x, block_y, panel_w, 88, 0x252535);
      gui_draw_string(panel_x + 16, block_y + 12, "Widget launcher", 0x89B4FA,
                      0x252535);
      gui_draw_string(panel_x + 16, block_y + 32, sidebar_layout_buf, 0xFFFFFF,
                      0x252535);
      gui_draw_string(panel_x + 172, block_y + 32, sidebar_width_buf, 0xCBD5E1,
                      0x252535);
      gui_draw_system_button(panel_x + 16, block_y + 52, 82, 24,
                             sidebar_visible ? "Hide" : "Show",
                             sidebar_visible ? GUI_BUTTON_NEUTRAL
                                             : GUI_BUTTON_SUCCESS,
                             1, 0);
      gui_draw_system_button(panel_x + 108, block_y + 52, 72, 24, "Left",
                             sidebar_side == DESKTOP_SIDEBAR_LEFT
                                 ? GUI_BUTTON_PRIMARY
                                 : GUI_BUTTON_NEUTRAL,
                             1, 0);
      gui_draw_system_button(panel_x + 190, block_y + 52, 72, 24, "Right",
                             sidebar_side == DESKTOP_SIDEBAR_RIGHT
                                 ? GUI_BUTTON_PRIMARY
                                 : GUI_BUTTON_NEUTRAL,
                             1, 0);
      gui_draw_system_button(panel_x + 272, block_y + 52, 90, 24, "Narrower",
                             GUI_BUTTON_NEUTRAL, sidebar_width > 176, 0);
      gui_draw_system_button(panel_x + 372, block_y + 52, 74, 24, "Wider",
                             GUI_BUTTON_NEUTRAL, sidebar_width < 288, 0);
    } else if (settings_active_tab == 5) {
      int block_y = panel_y + 72;
      const char *username = account_username[0] ? account_username : "Guest session";
      const char *auth_state =
          desktop_session_active()
              ? "Signed in to the desktop"
              : (startup_flow_active() ? "Waiting at login screen"
                                       : "Session inactive");

      gui_draw_rect(panel_x, block_y, panel_w, 76, 0x252535);
      gui_draw_string(panel_x + 16, block_y + 12, "Current User", 0x89B4FA,
                      0x252535);
      gui_draw_string(panel_x + 16, block_y + 32, username, 0xFFFFFF, 0x252535);
      gui_draw_string(panel_x + 16, block_y + 52, account_role_label(), 0xA5B4FC,
                      0x252535);
      gui_draw_string(panel_x + 112, block_y + 52, auth_state, 0xCBD5E1,
                      0x252535);

      block_y += 88;
      gui_draw_rect(panel_x, block_y, panel_w, 90, 0x252535);
      gui_draw_string(panel_x + 16, block_y + 12, "Account Storage", 0x89B4FA,
                      0x252535);
      gui_draw_string(panel_x + 16, block_y + 34,
                      account_partition_label[0] ? account_partition_label
                                                 : "No dedicated user partition",
                      0xFFFFFF, 0x252535);
      gui_draw_string(panel_x + 16, block_y + 54,
                      account_disk_location[0] ? account_disk_location
                                               : "Using local session storage",
                      0xCBD5E1, 0x252535);
      gui_draw_string(panel_x + 16, block_y + 72,
                      session_can_logout()
                          ? "Sign out to return to the login screen."
                          : "Sign out is unavailable in this boot mode.",
                      0xA5B4FC, 0x252535);

      block_y += 102;
      gui_draw_rect(panel_x, block_y, 112, 30,
                    session_can_logout() ? 0x2563EB : 0x4B5563);
      gui_draw_string(panel_x + 30, block_y + 9, "Sign Out", 0xFFFFFF,
                      session_can_logout() ? 0x2563EB : 0x4B5563);

      {
        settings_account_list_t accounts;
        char masked_password[32];
        char selected_role[16];
        uint32_t user_field_bg =
            settings_user_active_field == 0 ? 0x31314A : 0x232337;
        uint32_t pass_field_bg =
            settings_user_active_field == 1 ? 0x31314A : 0x232337;
        int list_y = block_y + 44;
        int list_start = 0;
        int visible_rows = 0;
        int editor_x = panel_x + 224;
        int editor_w = panel_w - 224;
        int footer_y;

        settings_account_list_init(&accounts);
        settings_collect_accounts(&accounts);
        settings_sync_selected_user();
        mask_secret(settings_user_new_password, masked_password,
                    sizeof(masked_password));
        settings_user_role_for_name(settings_user_selected, selected_role,
                                    sizeof(selected_role));
        visible_rows = accounts.count < 4 ? accounts.count : 4;
        list_start = settings_account_list_visible_start(&accounts,
                                                         settings_user_selected,
                                                         visible_rows);

        gui_draw_rect(panel_x, list_y, 210, 132, 0x252535);
        gui_draw_string(panel_x + 16, list_y + 12, "Accounts", 0x89B4FA, 0x252535);
        for (int i = 0; i < visible_rows; i++) {
          int account_idx = list_start + i;
          int row_y = list_y + 34 + i * 22;
          uint32_t row_bg =
              str_cmp(settings_user_selected, accounts.names[account_idx]) == 0
                  ? 0x2563EB
                  : 0x1E293B;
          gui_draw_rect(panel_x + 12, row_y, 186, 18, row_bg);
          gui_draw_string(panel_x + 20, row_y + 5, accounts.names[account_idx],
                          0xFFFFFF, row_bg);
        }

        gui_draw_rect(editor_x, list_y, editor_w, 132, 0x252535);
        gui_draw_string(editor_x + 16, list_y + 12, "Add User", 0x89B4FA,
                        0x252535);
        gui_draw_rect(editor_x + 16, list_y + 34, editor_w - 32, 24, user_field_bg);
        gui_draw_string(editor_x + 24, list_y + 41,
                        settings_user_new_name[0] ? settings_user_new_name
                                                  : "Username",
                        settings_user_new_name[0] ? 0xFFFFFF : 0x6C7086,
                        user_field_bg);
        gui_draw_rect(editor_x + 16, list_y + 66, editor_w - 32, 24, pass_field_bg);
        gui_draw_string(editor_x + 24, list_y + 73,
                        masked_password[0] ? masked_password : "Password",
                        masked_password[0] ? 0xFFFFFF : 0x6C7086, pass_field_bg);
        gui_draw_rect(editor_x + 16, list_y + 98, 110, 26,
                      account_role_is_admin() ? 0x1E293B : 0x4B5563);
        gui_draw_string(editor_x + 24, list_y + 106, "Role", 0x89B4FA,
                        account_role_is_admin() ? 0x1E293B : 0x4B5563);
        gui_draw_string(editor_x + 60, list_y + 106,
                        settings_role_label_from_index(settings_user_new_role_idx),
                        0xFFFFFF,
                        account_role_is_admin() ? 0x1E293B : 0x4B5563);
        gui_draw_rect(editor_x + 136, list_y + 100, 92, 24,
                      account_role_is_admin() ? 0x2563EB : 0x4B5563);
        gui_draw_string(editor_x + 164, list_y + 107, "Add", 0xFFFFFF,
                        account_role_is_admin() ? 0x2563EB : 0x4B5563);
        gui_draw_rect(editor_x + 236, list_y + 100, 108, 24,
                      (account_role_is_admin() && settings_user_selected[0])
                          ? 0x7C2D12
                          : 0x4B5563);
        gui_draw_string(editor_x + 260, list_y + 107, "Remove", 0xFFFFFF,
                        (account_role_is_admin() && settings_user_selected[0])
                            ? 0x7C2D12
                            : 0x4B5563);

        footer_y = list_y + 144;
        gui_draw_rect(panel_x, footer_y, panel_w, 54, 0x1E293B);
        gui_draw_string(panel_x + 16, footer_y + 12,
                        settings_user_selected[0] ? settings_user_selected
                                                  : "No user selected",
                        0xFFFFFF, 0x1E293B);
        gui_draw_string(panel_x + 16, footer_y + 30,
                        settings_user_selected[0] ? selected_role
                                                  : "Select a user to inspect the account mode.",
                        0xCBD5E1, 0x1E293B);
        if (!account_role_is_admin()) {
          gui_draw_string(panel_x + 140, footer_y + 30,
                          "Admin mode is required to add or remove users.",
                          0xF9A8D4, 0x1E293B);
        }
        settings_account_list_free(&accounts);
      }
    } else if (settings_active_tab == 6) {
      int block_y = panel_y + 72;
      gui_draw_rect(panel_x, block_y, panel_w, 84, 0x252535);
      gui_draw_string(panel_x + 16, block_y + 12, "Personalization", 0x89B4FA,
                      0x252535);
      gui_draw_string(panel_x + 16, block_y + 32, theme_status, 0xFFFFFF, 0x252535);
      gui_draw_string(panel_x + 16, block_y + 52, wallpapers[current_wallpaper].name,
                      0xCBD5E1, 0x252535);
      gui_draw_string(panel_x + 220, block_y + 52, dock_count_buf, 0xA5B4FC,
                      0x252535);

      block_y += 98;
      gui_draw_system_button(panel_x + 16, block_y, 108, 28, "Backgrounds",
                             GUI_BUTTON_PRIMARY, 1, 0);
      gui_draw_system_button(panel_x + 134, block_y, 92, 28, "Light",
                             GUI_BUTTON_NEUTRAL, 1,
                             g_theme_mode == GUI_THEME_LIGHT);
      gui_draw_system_button(panel_x + 236, block_y, 92, 28, "Dark",
                             GUI_BUTTON_NEUTRAL, 1,
                             g_theme_mode == GUI_THEME_DARK);
      gui_draw_system_button(panel_x + 338, block_y, 110, 28, "Theme Lab",
                             GUI_BUTTON_NEUTRAL, 1, settings_active_tab == 11);

      block_y += 42;
      gui_draw_rect(panel_x, block_y, panel_w, 88, 0x252535);
      gui_draw_string(panel_x + 16, block_y + 12, "Profile", 0x89B4FA, 0x252535);
      gui_draw_string(panel_x + 16, block_y + 32,
                      "Switch palettes, update wallpapers, and keep the shell feeling consistent.",
                      0xFFFFFF, 0x252535);
      gui_draw_string(panel_x + 16, block_y + 52,
                      "Theme Lab opens the full slot editor for advanced color tuning.",
                      0xCBD5E1, 0x252535);
    } else if (settings_active_tab == 7) {
      gui_draw_string(panel_x + 18, panel_y + 72, "Updates are not available yet.",
                      0x111111, 0xF8F8F8);
      gui_draw_string(panel_x + 18, panel_y + 92,
                      "Future builds can expose package or system update actions here.",
                      0x4A4A4A, 0xF8F8F8);
    } else if (settings_active_tab == 10) {
      int block_y = panel_y + 72;
      char sidebar_layout_buf[32];
      char sidebar_width_buf[32];
      int sidebar_idx = 0;
      int sidebar_visible = desktop_sidebar_is_visible();
      int sidebar_side = desktop_sidebar_get_side();
      int sidebar_width = desktop_sidebar_get_width();

      sidebar_layout_buf[0] = '\0';
      if (sidebar_visible) {
        str_copy_safe(sidebar_layout_buf,
                      sidebar_side == DESKTOP_SIDEBAR_RIGHT ? "Visible on right"
                                                           : "Visible on left",
                      sizeof(sidebar_layout_buf));
      } else {
        str_copy_safe(sidebar_layout_buf, "Hidden", sizeof(sidebar_layout_buf));
      }
      sidebar_width_buf[0] = '\0';
      append_decimal(sidebar_width_buf, &sidebar_idx, sidebar_width);
      notepad_append_to_buf(sidebar_width_buf, sizeof(sidebar_width_buf), " px");

      gui_draw_rect(panel_x, block_y, panel_w, 82, 0x252535);
      gui_draw_string(panel_x + 16, block_y + 12, "Widget launcher", 0x89B4FA,
                      0x252535);
      gui_draw_string(panel_x + 16, block_y + 32, sidebar_layout_buf, 0xFFFFFF,
                      0x252535);
      gui_draw_string(panel_x + 16, block_y + 52, sidebar_width_buf, 0xCBD5E1,
                      0x252535);

      block_y += 96;
      gui_draw_system_button(panel_x + 16, block_y, 82, 24,
                             sidebar_visible ? "Hide" : "Show",
                             sidebar_visible ? GUI_BUTTON_NEUTRAL
                                             : GUI_BUTTON_SUCCESS,
                             1, 0);
      gui_draw_system_button(panel_x + 108, block_y, 72, 24, "Left",
                             sidebar_side == DESKTOP_SIDEBAR_LEFT
                                 ? GUI_BUTTON_PRIMARY
                                 : GUI_BUTTON_NEUTRAL,
                             1, 0);
      gui_draw_system_button(panel_x + 190, block_y, 72, 24, "Right",
                             sidebar_side == DESKTOP_SIDEBAR_RIGHT
                                 ? GUI_BUTTON_PRIMARY
                                 : GUI_BUTTON_NEUTRAL,
                             1, 0);
      gui_draw_system_button(panel_x + 272, block_y, 90, 24, "Narrower",
                             GUI_BUTTON_NEUTRAL, sidebar_width > 176, 0);
      gui_draw_system_button(panel_x + 372, block_y, 74, 24, "Wider",
                             GUI_BUTTON_NEUTRAL, sidebar_width < 288, 0);

      block_y += 40;
      gui_draw_rect(panel_x, block_y, panel_w, 88, 0x252535);
      gui_draw_string(panel_x + 16, block_y + 12, "Layout notes", 0x89B4FA,
                      0x252535);
      gui_draw_string(panel_x + 16, block_y + 32,
                      "Pin the widget launcher to either edge and resize it to match your workspace.",
                      0xFFFFFF, 0x252535);
      gui_draw_string(panel_x + 16, block_y + 52,
                      "Launcher widgets keep apps and quick places together in one rail.",
                      0xCBD5E1, 0x252535);
    } else if (settings_active_tab == 11) {
      gui_draw_string(panel_x + 18, panel_y + 72, "Developer theme lab",
                      0x89B4FA, theme->settings_panel);
      gui_draw_string(panel_x + 18, panel_y + 90,
                      "Edit slot colors, switch palettes, and save the active theme.",
                      theme->settings_subtext, theme->settings_panel);
      draw_theme_builder_window(panel_x + 12, panel_y + 112, panel_w - 28,
                                panel_h - 128);
    } else {
      gui_draw_string(panel_x + 18, panel_y + 72, BUILD_UUID, 0x111111, 0xF8F8F8);
      gui_draw_string(panel_x + 18, panel_y + 92, resolution, 0x4A4A4A, 0xF8F8F8);
      gui_draw_string(panel_x + 18, panel_y + 112, g_gpu_backend_name, 0x4A4A4A,
                      0xF8F8F8);
      gui_draw_rect(panel_x + 18, panel_y + 146, 88, 30, 0x111111);
      gui_draw_string(panel_x + 44, panel_y + 155, "About", 0xF2F2F2, 0x111111);
    }
  }
  /* Device Manager window */
  else if (win->title[0] == 'D' && win->title[1] == 'e' &&
           win->title[2] == 'v') {
    int yy = content_y + 12;
    char resolution[32];
    char windows_info[32];
    char usb_ports[32];
    char usb_names[4][48];
    char storage_overview[96];
    char storage_line0[80];
    char storage_line1[80];
    char disk_overview[80];
    int usb_count = 0;
    extern int intel_hda_is_ready(void);
    extern int intel_hda_is_playing(void);
    extern int virtio_net_is_ready(void);
    extern int intel_gfx_detected(void);
    extern int intel_gfx_is_supported_device(void);
    extern int intel_gfx_supports_gpu_rendering(void);
    extern int intel_gfx_is_using_framebuffer_fallback(void);
    extern const char *intel_gfx_get_name(void);
    extern int xhci_is_ready(void);
    extern int xhci_get_port_count(void);
    extern int xhci_get_connected_count(void);
    extern int usb_device_count(void);
    extern int usb_device_info(int idx, uint16_t *vid, uint16_t *pid, char *name,
                               int name_len);
    extern void storage_build_overview(char *buf, int max);
    extern void storage_build_disk_overview(char *buf, int max);
    extern int storage_describe_controller(int index, char *buf, int max);

    gui_build_display_mode_string(resolution, sizeof(resolution));
    build_windows_string(windows_info);
    build_device_ports_string(usb_ports, xhci_get_connected_count(),
                              xhci_get_port_count());
    for (int i = 0; i < 4; i++)
      usb_names[i][0] = '\0';
    usb_count = usb_device_count();
    for (int i = 0; i < usb_count && i < 4; i++) {
      usb_device_info(i, NULL, NULL, usb_names[i], sizeof(usb_names[i]));
    }
    if (usb_count > 4) {
      int idx = 0;
      int remaining = usb_count - 3;

      str_copy_safe(usb_names[3], "+", sizeof(usb_names[3]));
      while (usb_names[3][idx] && idx < (int)sizeof(usb_names[3]) - 1)
        idx++;
      append_decimal(usb_names[3], &idx, remaining);
      if (remaining == 1) {
        str_copy_safe(usb_names[3] + idx, " more device",
                      (int)sizeof(usb_names[3]) - idx);
      } else {
        str_copy_safe(usb_names[3] + idx, " more devices",
                      (int)sizeof(usb_names[3]) - idx);
      }
    }
    storage_build_overview(storage_overview, sizeof(storage_overview));
    storage_build_disk_overview(disk_overview, sizeof(disk_overview));
    if (storage_describe_controller(0, storage_line0, sizeof(storage_line0)) !=
        0) {
      str_copy_safe(storage_line0, "No disk controllers registered",
                    sizeof(storage_line0));
    }
    if (storage_describe_controller(1, storage_line1, sizeof(storage_line1)) !=
        0) {
      storage_line1[0] = '\0';
    }

    gui_draw_rect(content_x, content_y, content_w, content_h, theme->app_bg);
    gui_draw_string(content_x + 12, yy, "Device Manager", theme->app_fg, THEME_BG);
    yy += 28;

    gui_draw_rect(content_x + 10, yy, content_w - 20, 52, theme->card);
    gui_draw_string(content_x + 20, yy + 8, "Display Adapter", 0x89B4FA,
                    theme->card);
    if (intel_gfx_detected()) {
      gui_draw_string(content_x + 20, yy + 28, intel_gfx_get_name(),
                      theme->app_muted, theme->card);
      gui_draw_string(
          content_x + 170, yy + 28,
          intel_gfx_supports_gpu_rendering()
              ? "full system support active"
              : intel_gfx_is_using_framebuffer_fallback()
                    ? "default framebuffer fallback active"
              : intel_gfx_is_supported_device()
                    ? "framebuffer compositor mode"
                    : "compatibility framebuffer path",
          theme->app_muted, theme->card);
    } else {
      gui_draw_string(content_x + 20, yy + 28, "Framebuffer compositor active",
                      theme->app_muted, theme->card);
    }
    gui_draw_string(content_x + content_w - 150, yy + 28, resolution,
                    theme->app_muted, theme->card);
    yy += 62;

    gui_draw_rect(content_x + 10, yy, content_w - 20, 68, theme->card);
    gui_draw_string(content_x + 20, yy + 8, "Storage", 0x89B4FA, theme->card);
    gui_draw_string(content_x + 20, yy + 26, storage_overview, theme->app_muted,
                    theme->card);
    gui_draw_string(content_x + content_w - 170, yy + 26, disk_overview,
                    0xA6E3A1, theme->card);
    gui_draw_string(content_x + 20, yy + 42, storage_line0, theme->app_muted,
                    theme->card);
    if (storage_line1[0]) {
      gui_draw_string(content_x + 20, yy + 56, storage_line1, theme->app_muted,
                      theme->card);
    }
    yy += 78;

    gui_draw_rect(content_x + 10, yy, content_w - 20, 52, theme->card);
    gui_draw_string(content_x + 20, yy + 8, "Input Devices", 0x89B4FA,
                    theme->card);
    gui_draw_string(content_x + 20, yy + 28,
                    "Keyboard + pointer input subsystem online",
                    theme->app_muted, theme->card);
    gui_draw_string(content_x + content_w - 150, yy + 28, windows_info,
                    0xA6E3A1, theme->card);
    yy += 62;

    gui_draw_rect(content_x + 10, yy, content_w - 20, 52, theme->card);
    gui_draw_string(content_x + 20, yy + 8, "Audio Controller", 0x89B4FA,
                    theme->card);
    gui_draw_string(content_x + 20, yy + 28,
                    intel_hda_is_ready() ? "Intel HDA controller present"
                                         : "Intel HDA controller not detected",
                    intel_hda_is_ready() ? theme->app_muted : 0xF38BA8,
                    theme->card);
    gui_draw_string(content_x + content_w - 150, yy + 28,
                    intel_hda_is_playing() ? "Playing" : "Idle",
                    intel_hda_is_playing() ? 0xA6E3A1 : theme->app_muted,
                    theme->card);
    yy += 62;

    gui_draw_rect(content_x + 10, yy, content_w - 20, 52, theme->card);
    gui_draw_string(content_x + 20, yy + 8, "Network Adapter", 0x89B4FA,
                    theme->card);
    gui_draw_string(content_x + 20, yy + 28,
                    wifi_has_supported_adapter() ? wifi_get_status_text()
                                                 : (virtio_net_is_ready()
                                                        ? "virtio-net interface ready"
                                                        : "virtio-net interface offline"),
                    wifi_has_supported_adapter() || virtio_net_is_ready()
                        ? theme->app_muted
                        : 0xF38BA8,
                    theme->card);
    gui_draw_string(content_x + content_w - 150, yy + 28,
                    wifi_has_supported_adapter()
                        ? (wifi_is_connected() ? wifi_get_connected_ssid()
                                               : wifi_get_driver_name())
                        : (virtio_net_is_ready() ? "eth0 / NAT" : "Unavailable"),
                    wifi_has_supported_adapter() || virtio_net_is_ready()
                        ? 0xA6E3A1
                        : theme->app_muted,
                    theme->card);
    yy += 62;

    gui_draw_rect(content_x + 10, yy, content_w - 20, usb_count > 2 ? 116 : 84,
                  theme->card);
    gui_draw_string(content_x + 20, yy + 8, "USB Host Controller", 0x89B4FA,
                    theme->card);
    gui_draw_string(content_x + 20, yy + 28,
                    xhci_is_ready() ? "xHCI controller initialized"
                                    : "xHCI controller unavailable",
                    xhci_is_ready() ? theme->app_muted : 0xF38BA8, theme->card);
    gui_draw_string(content_x + content_w - 150, yy + 28, usb_ports,
                    xhci_is_ready() ? 0xA6E3A1 : theme->app_muted, theme->card);
    if (!xhci_is_ready()) {
      gui_draw_string(content_x + 20, yy + 48, "No USB enumeration available",
                      theme->app_muted, theme->card);
    } else if (usb_count <= 0) {
      gui_draw_string(content_x + 20, yy + 48, "No USB devices enumerated",
                      theme->app_muted, theme->card);
    } else {
      gui_draw_string(content_x + 20, yy + 48, usb_names[0], theme->app_muted,
                      theme->card);
      if (usb_count == 1) {
        gui_draw_string(content_x + 20, yy + 64, "1 device detected",
                        theme->app_muted, theme->card);
      } else {
        int usb_lines = usb_count > 4 ? 4 : usb_count;
        for (int i = 1; i < usb_lines; i++) {
          gui_draw_string(content_x + 20, yy + 48 + i * 16, usb_names[i],
                          theme->app_muted, theme->card);
        }
      }
    }
  }
  /* Clock window */
  else if (win->title[0] == 'C' && win->title[1] == 'l' &&
           win->title[2] == 'o') {
    draw_clock_widget(content_x, content_y, content_w, content_h, THEME_BG);
  }
  /* Game window */
  else if (win->title[0] == 'G' && win->title[1] == 'a' &&
           win->title[2] == 'm') {
    gui_draw_rect(content_x, content_y, content_w, content_h, theme->app_bg);
    gui_draw_string(content_x + 14, content_y + 12, "Games", 0x89B4FA,
                    THEME_BG);
    gui_draw_string(content_x + 14, content_y + 36,
                    "Launch Bowling from the dock or app list.",
                    theme->app_muted, THEME_BG);
  }
  /* Terminal - use term_render from terminal.c for proper output display */
  else if (win->title[0] == 'T' && win->title[1] == 'e' &&
           win->title[2] == 'r') {
    /* Use window's own terminal if available, otherwise fall back to active */
    struct terminal *term = (struct terminal *)win->userdata;
    if (!term) {
      term = term_get_active();
    }
    if (term) {
      /* Update terminal's content area to match window position */
      term_set_content_pos(term, content_x, content_y);
      term_render(term);
    } else {
      /* Fallback if no terminal */
      gui_draw_string(content_x + 10, content_y + 10,
                      "Terminal not initialized", 0xFF0000, THEME_BG);
    }
  }
  /* Notepad - Modern Design with Full Toolbar */

  else if ((win->title[0] == 'N' && win->title[1] == 'o' &&
            win->title[2] == 't') ||
           (win->title[0] == 'R' && win->title[1] == 'e' &&
            win->title[2] == 'n')) {
    uint32_t editor_toolbar =
        g_theme_mode == GUI_THEME_LIGHT ? 0xE6EDF6 : 0x2D2D30;
    uint32_t editor_bg = g_theme_mode == GUI_THEME_LIGHT ? 0xFFFFFF : 0x1E1E1E;
    uint32_t editor_gutter =
        g_theme_mode == GUI_THEME_LIGHT ? 0xEEF3F9 : 0x252526;
    uint32_t editor_border =
        g_theme_mode == GUI_THEME_LIGHT ? 0xD7E2EF : 0x3C3C3C;

    /* Modern dark toolbar */
    int toolbar_h = 62;
    gui_draw_rect(content_x, content_y, content_w, toolbar_h, editor_toolbar);

    /* Toolbar buttons with modern styling */
    int btn_y = content_y + 6;
    int btn_h = 24;
    int btn_spacing = 4;
    int bx = content_x + 8;

    /* File operations group */
    /* New button */
    gui_draw_system_button(bx, btn_y, 50, btn_h, "New", GUI_BUTTON_NEUTRAL, 1, 0);
    bx += 50 + btn_spacing;

    /* Open button */
    gui_draw_system_button(bx, btn_y, 50, btn_h, "Open", GUI_BUTTON_NEUTRAL, 1, 0);
    bx += 50 + btn_spacing;

    /* Save button - highlighted */
    gui_draw_system_button(bx, btn_y, 50, btn_h, "Save", GUI_BUTTON_PRIMARY, 1,
                           0);
    bx += 50 + btn_spacing;

    /* Save As button */
    gui_draw_system_button(bx, btn_y, 64, btn_h, "Save As", GUI_BUTTON_NEUTRAL,
                           1, 0);
    bx += 64 + btn_spacing;

    /* Separator */
    bx += 8;
    gui_draw_rect(bx, btn_y + 2, 1, btn_h - 4, editor_border);
    bx += 12;

    /* Edit operations group */
    /* Cut button */
    gui_draw_system_button(bx, btn_y, 42, btn_h, "Cut", GUI_BUTTON_NEUTRAL, 1, 0);
    bx += 42 + btn_spacing;

    /* Copy button */
    gui_draw_system_button(bx, btn_y, 50, btn_h, "Copy", GUI_BUTTON_NEUTRAL, 1,
                           0);
    bx += 50 + btn_spacing;

    /* Paste button */
    gui_draw_system_button(bx, btn_y, 55, btn_h, "Paste", GUI_BUTTON_NEUTRAL, 1,
                           0);

    if (win->title[0] == 'N') {
      gui_draw_string(content_x + 10, content_y + 40,
                      notepad_filepath[0] ? notepad_filepath : "Untitled document",
                      theme->settings_subtext, editor_toolbar);
      gui_draw_string(content_x + content_w - 170, content_y + 40,
                      notepad_dirty ? "Modified" : "Saved", 0xD7BA7D, editor_toolbar);
    }

    /* Text editing area with modern styling */
    int text_area_y = content_y + toolbar_h + 2;
    int status_h = 22;
    int text_area_h = content_h - toolbar_h - status_h - 4;

    /* Text area background with subtle border */
    gui_draw_rect(content_x + 4, text_area_y, content_w - 8, text_area_h,
                  editor_bg);
    gui_draw_rect(content_x + 4, text_area_y, content_w - 8, 1, editor_border);
    gui_draw_rect(content_x + 4, text_area_y, 1, text_area_h, editor_border);

    /* Line number gutter */
    int gutter_w = 40;
    gui_draw_rect(content_x + 5, text_area_y + 1, gutter_w - 2, text_area_h - 2,
                  editor_gutter);

    /* Draw line numbers */
    int line_num = 1;
    int max_lines = (text_area_h - 8) / 16;
    for (int i = 0; i < max_lines && i < 20; i++) {
      char num_str[4] = {0};
      int n = line_num + i;
      if (n < 10) {
        num_str[0] = '0' + n;
      } else {
        num_str[0] = '0' + (n / 10);
        num_str[1] = '0' + (n % 10);
      }
      gui_draw_string(content_x + 20, text_area_y + 4 + i * 16, num_str,
                      0x858585, 0x252526);
    }

    /* Draw text with syntax-like highlighting */
    int tx = content_x + 8 + gutter_w;
    int ty = text_area_y + 4;
    int max_x = content_x + content_w - 12;
    int max_y = text_area_y + text_area_h - 8;
    char *target_text = (win->title[0] == 'N') ? notepad_text : rename_text;
    int target_cursor = (win->title[0] == 'N') ? notepad_cursor : rename_cursor;
    int selection_start = -1;
    int selection_end = -1;
    int cursor_x = tx;
    int cursor_y = ty;
    int cursor_line = 1;
    int cursor_col = 1;
    int line_count = 1;
    int col_count = 1;

    if (win->title[0] == 'N' && notepad_has_selection()) {
      notepad_get_selection_bounds(&selection_start, &selection_end);
    }

    for (int i = 0; ty < max_y; i++) {
      char c = target_text[i];

      if (i == target_cursor) {
        cursor_x = tx;
        cursor_y = ty;
        cursor_line = line_count;
        cursor_col = col_count;
      }
      if (c == '\0')
        break;
      if (c == '\n') {
        tx = content_x + 8 + gutter_w;
        ty += 16;
        line_count++;
        col_count = 1;
      } else {
        uint32_t bg = 0x1E1E1E;
        uint32_t fg = 0xD4D4D4;
        if (selection_start >= 0 && i >= selection_start && i < selection_end) {
          gui_draw_rect(tx, ty, 8, 16, 0x264F78);
          bg = 0x264F78;
          fg = 0xFFFFFF;
        }
        gui_draw_char(tx, ty, c, fg, bg);
        tx += 8;
        col_count++;
        if (tx >= max_x) {
          tx = content_x + 8 + gutter_w;
          ty += 16;
          line_count++;
          col_count = 1;
        }
      }
    }

    if (target_text[target_cursor] == '\0') {
      cursor_x = tx;
      cursor_y = ty;
      cursor_line = line_count;
      cursor_col = col_count;
    }

    /* Cursor with blink effect */
    if (win->focused) {
      gui_draw_rect(cursor_x, cursor_y, 2, 14, 0x569CD6);
    }

    /* Status bar */
    int status_y = content_y + content_h - status_h;
    gui_draw_rect(content_x, status_y, content_w, status_h, 0x007ACC);

    /* Status text */
    char status_text[64] = "Ln ";
    int si = 3;
    if (cursor_line < 10) {
      status_text[si++] = '0' + cursor_line;
    } else {
      status_text[si++] = '0' + (cursor_line / 10);
      status_text[si++] = '0' + (cursor_line % 10);
    }
    status_text[si++] = ',';
    status_text[si++] = ' ';
    status_text[si++] = 'C';
    status_text[si++] = 'o';
    status_text[si++] = 'l';
    status_text[si++] = ' ';
    int col = cursor_col;
    if (col < 10) {
      status_text[si++] = '0' + col;
    } else {
      status_text[si++] = '0' + (col / 10);
      status_text[si++] = '0' + (col % 10);
    }
    status_text[si] = '\0';
    gui_draw_string(content_x + 12, status_y + 4, status_text, 0xFFFFFF,
                    0x007ACC);

    /* File type indicator */
    if (win->title[0] == 'N') {
      char right_status[80] = "UTF-8  ";
      if (notepad_dirty) {
        notepad_append_to_buf(right_status, sizeof(right_status), "Dirty");
      } else {
        notepad_append_to_buf(right_status, sizeof(right_status), "Saved");
      }
      gui_draw_string(content_x + content_w - 110, status_y + 4, right_status,
                      0xFFFFFF, 0x007ACC);
      gui_draw_string(content_x + 110, status_y + 4, notepad_status, 0xFFFFFF,
                      0x007ACC);
    } else {
      gui_draw_string(content_x + content_w - 60, status_y + 4, "UTF-8", 0xFFFFFF,
                      0x007ACC);
    }

    if (win->title[0] == 'N' && notepad_dialog_mode != NOTEPAD_DIALOG_NONE) {
      struct fm_item items[FM_MAX_ITEMS];
      int item_count = fm_collect_items(notepad_dialog_dir, items, FM_MAX_ITEMS);
      int panel_w = content_w - 80;
      int panel_h = content_h - 70;
      int panel_x = content_x + 40;
      int panel_y = content_y + 26;
      int list_x = panel_x + 16;
      int list_y = panel_y + 68;
      int list_w = panel_w - 32;
      int row_h = 22;
      int visible_rows = (panel_h - 156) / row_h;
      if (visible_rows < 4)
        visible_rows = 4;

      gui_draw_rect(content_x + 10, content_y + 10, content_w - 20, content_h - 20,
                    0x101012);
      gui_draw_rect(panel_x, panel_y, panel_w, panel_h, 0x252526);
      gui_draw_rect(panel_x, panel_y, panel_w, 1, 0x3C3C3C);
      gui_draw_rect(panel_x, panel_y, 1, panel_h, 0x3C3C3C);

      gui_draw_string(panel_x + 16, panel_y + 14,
                      notepad_dialog_mode == NOTEPAD_DIALOG_OPEN ? "Open File"
                                                                  : "Save File",
                      0xFFFFFF, 0x252526);
      gui_draw_rect(panel_x + 16, panel_y + 34, 44, 22, 0x3E3E42);
      gui_draw_string(panel_x + 30, panel_y + 40, "Up", 0xCCCCCC, 0x3E3E42);
      gui_draw_string(panel_x + 70, panel_y + 40, notepad_dialog_dir, 0x9CDCFE,
                      0x252526);

      gui_draw_rect(list_x, list_y, list_w, visible_rows * row_h + 4, 0x1E1E1E);
      for (int i = 0; i < item_count && i < visible_rows; i++) {
        int row_y = list_y + 2 + i * row_h;
        uint32_t row_bg =
            str_cmp(notepad_dialog_selected, items[i].name) == 0 ? 0x094771 : 0x1E1E1E;
        gui_draw_rect(list_x + 2, row_y, list_w - 4, row_h - 2, row_bg);
        if (items[i].type == 4) {
          char full_path[512];
          fm_join_path(notepad_dialog_dir, items[i].name, full_path,
                       sizeof(full_path));
          gui_draw_folder_icon(full_path, list_x + 10, row_y + 4, 18);
        } else {
          gui_draw_string(list_x + 8, row_y + 6, "[FILE]", 0xB5CEA8, row_bg);
        }
        gui_draw_string(list_x + 56, row_y + 6, items[i].name, 0xD4D4D4, row_bg);
      }

      gui_draw_string(panel_x + 16, panel_y + panel_h - 70, "Path", 0xCCCCCC,
                      0x252526);
      gui_draw_rect(panel_x + 16, panel_y + panel_h - 50, panel_w - 140, 24,
                    0x1E1E1E);
      gui_draw_string(panel_x + 24, panel_y + panel_h - 44, notepad_dialog_input,
                      0xDCDCAA, 0x1E1E1E);

      gui_draw_rect(panel_x + panel_w - 110, panel_y + panel_h - 50, 42, 24,
                    0x3E3E42);
      gui_draw_string(panel_x + panel_w - 100, panel_y + panel_h - 44, "Esc",
                      0xCCCCCC, 0x3E3E42);
      gui_draw_rect(panel_x + panel_w - 60, panel_y + panel_h - 50, 44, 24,
                    0x0E639C);
      gui_draw_string(panel_x + panel_w - 50, panel_y + panel_h - 44,
                      notepad_dialog_mode == NOTEPAD_DIALOG_OPEN ? "Open" : "Save",
                      0xFFFFFF, 0x0E639C);
    }
  }
  /* Bowling Game */
  else if ((win->title[0] == 'B' && win->title[1] == 'o' &&
            win->title[2] == 'w') ||
           (win->title[0] == 'S' && win->title[1] == 'n' &&
            win->title[2] == 'a')) {
    int lane_x = content_x + 14;
    int lane_y = content_y + 40;
    int lane_w = content_w - 160;
    int lane_h = content_h - 54;
    int board_x = lane_x + lane_w + 12;
    int board_w = content_x + content_w - board_x - 12;
    int board_y = lane_y;
    int board_h = lane_h;
    int pin_base_y = lane_y + 42;
    int standing = bowling_count_standing_pins();
    int aim_x = lane_x + 18 +
                bowling_ball_column * ((lane_w - 36) / (BOWLING_LANE_COLUMNS - 1));
    int ball_y = lane_y + lane_h - 32 -
                 (bowling_ball_progress * (lane_h - 86)) / BOWLING_LANE_PROGRESS_MAX;
    char frame_str[32] = "Frame ";
    char roll_str[32] = "Roll ";
    char score_str[32] = "Total ";
    char knock_str[32] = "Last ";
    char remain_str[32] = "Pins ";
    char framepins_str[32] = "Frame pins ";
    uint32_t lane_outer =
        g_theme_mode == GUI_THEME_LIGHT ? 0xD8C19A : 0x2C2118;
    uint32_t lane_inner =
        g_theme_mode == GUI_THEME_LIGHT ? 0xF4DFC0 : 0x6B4F30;

    append_uint_to_buf(frame_str, sizeof(frame_str), bowling_frame);
    append_uint_to_buf(roll_str, sizeof(roll_str), bowling_roll);
    append_uint_to_buf(score_str, sizeof(score_str), bowling_total_score);
    append_uint_to_buf(knock_str, sizeof(knock_str), bowling_last_knocked);
    append_uint_to_buf(remain_str, sizeof(remain_str), standing);
    append_uint_to_buf(framepins_str, sizeof(framepins_str),
                       bowling_first_roll_pins);

    gui_draw_rect(content_x, content_y, content_w, content_h, theme->app_bg);
    gui_draw_string(content_x + 12, content_y + 10, "Bowling", 0x89B4FA,
                    THEME_BG);
    gui_draw_string(content_x + 92, content_y + 10,
                    bowling_game_over ? "Game complete - press R to restart"
                                      : "A/D aim  W/S power  Q/E curve  Space bowl",
                    theme->app_muted, THEME_BG);

    gui_draw_rect(lane_x, lane_y, lane_w, lane_h, lane_outer);
    gui_draw_rect(lane_x + 6, lane_y + 6, lane_w - 12, lane_h - 12, lane_inner);
    gui_draw_rect(lane_x + 8, lane_y + 8, 8, lane_h - 16,
                  g_theme_mode == GUI_THEME_LIGHT ? 0xBF9A67 : 0x4A3520);
    gui_draw_rect(lane_x + lane_w - 16, lane_y + 8, 8, lane_h - 16,
                  g_theme_mode == GUI_THEME_LIGHT ? 0xBF9A67 : 0x4A3520);

    for (int i = 0; i < BOWLING_PIN_COUNT; i++) {
      int px;
      int py;
      if (!bowling_pin_standing[i])
        continue;
      px = lane_x + 28 +
           bowling_pin_layout_x[i] * ((lane_w - 56) / (BOWLING_LANE_COLUMNS - 1));
      py = pin_base_y + bowling_pin_layout_y[i] * 18;
      gui_draw_rect(px - 4, py - 2, 8, 13, 0xFFFFFF);
      gui_draw_rect(px - 6, py + 9, 12, 4, 0xFFFFFF);
      gui_draw_rect(px - 5, py + 2, 10, 2, 0x2563EB);
    }

    draw_filled_circle(aim_x, ball_y, 10, 0xDC2626);
    draw_filled_circle(aim_x - 3, ball_y - 2, 1, 0x7F1D1D);
    draw_filled_circle(aim_x + 1, ball_y - 4, 1, 0x7F1D1D);
    draw_filled_circle(aim_x + 3, ball_y + 1, 1, 0x7F1D1D);

    gui_draw_rect(board_x, board_y, board_w, board_h, theme->card);
    gui_draw_rect_outline(board_x, board_y, board_w, board_h, theme->border, 1);
    gui_draw_string(board_x + 12, board_y + 12, frame_str, theme->app_fg,
                    theme->card);
    gui_draw_string(board_x + 12, board_y + 32, roll_str, theme->app_muted,
                    theme->card);
    gui_draw_string(board_x + 12, board_y + 58, score_str, 0xA6E3A1,
                    theme->card);
    gui_draw_string(board_x + 12, board_y + 78, knock_str, 0xF9E2AF,
                    theme->card);
    gui_draw_string(board_x + 12, board_y + 98, remain_str, theme->app_fg,
                    theme->card);
    gui_draw_string(board_x + 12, board_y + 118, framepins_str, theme->app_muted,
                    theme->card);

    gui_draw_string(board_x + 12, board_y + 146, "Power", 0x89B4FA, theme->card);
    for (int i = 0; i < 5; i++) {
      uint32_t bar_color = i < bowling_ball_power ? theme->accent : theme->surface_alt;
      gui_draw_rect(board_x + 12 + i * 18, board_y + 164, 14, 16, bar_color);
    }

    gui_draw_string(board_x + 12, board_y + 198, "Curve", 0x89B4FA, theme->card);
    if (bowling_ball_spin < 0) {
      gui_draw_string(board_x + 12, board_y + 216, "Left hook", theme->app_fg,
                      theme->card);
    } else if (bowling_ball_spin > 0) {
      gui_draw_string(board_x + 12, board_y + 216, "Right hook", theme->app_fg,
                      theme->card);
    } else {
      gui_draw_string(board_x + 12, board_y + 216, "Straight", theme->app_fg,
                      theme->card);
    }

    gui_draw_string(board_x + 12, board_y + board_h - 48,
                    bowling_roll == 1 ? "Strike clears the frame." :
                                        "Second roll on remaining pins.",
                    theme->app_muted, theme->card);
    gui_draw_string(board_x + 12, board_y + board_h - 28,
                    bowling_ball_rolling ? "Ball is rolling..." :
                    (bowling_game_over ? "Press R to start over." :
                                         "Line it up and press Space."),
                    theme->app_fg, theme->card);
  }
  /* Clock */
  else if (win->title[0] == 'C' && win->title[1] == 'l' &&
           win->title[2] == 'o') {
    draw_clock_widget(content_x, content_y, content_w, content_h, THEME_BG);
  }

  /* Background Settings Window */
  else if (win->title[0] == 'B' && win->title[1] == 'a' &&
           win->title[2] == 'c') {
    int header_h = 42;
    int body_y = content_y + header_h;
    int body_h = content_h - header_h;
    int card_x = content_x + 12;
    int card_y = body_y + 12;
    int card_w = content_w - 24;
    int card_h = body_h - 24;
    int grid_x = card_x + 14;
    int grid_y = card_y + 40;
    int thumb_w = 60;
    int thumb_h = 40;
    int gap_x = 10;
    int gap_y = 18;
    int columns = 3;
    int sidebar_x = card_x + 218;
    int sidebar_w = card_w - (sidebar_x - card_x) - 14;
    int preview_x = sidebar_x + 10;
    int preview_y = card_y + 44;
    int preview_w = sidebar_w - 20;
    int preview_h = 78;
    const char *blur_picker_status;
    const char *wallpaper_kind =
        wallpapers[current_wallpaper].type == 1 ? "Photo-based scene"
                                                : "Gradient theme";
    const char *picker_hint = wallpapers[current_wallpaper].type == 1
                                  ? (wallpapers[current_wallpaper].fit_mode ==
                                             WALLPAPER_FIT_STRETCH
                                         ? "Stretched to fill the desktop."
                                         : "Scaled to cover the desktop surface.")
                                  : "Gradient colors render directly on the framebuffer.";

    blur_picker_status = gui_blur_effects_status_label();

    gui_draw_rect(content_x, content_y, content_w, content_h, 0x5E616A);
    gui_draw_rect(content_x, content_y, content_w, header_h, 0x27272A);
    gui_draw_string(content_x + 15, content_y + 12, "Choose Wallpaper",
                    0xFAFAFA, 0x27272A);
    gui_draw_rect(card_x, card_y, card_w, card_h, 0xF5F5F7);
    gui_draw_rect(card_x, card_y, card_w, 3, 0x1F2937);
    gui_draw_string(card_x + 14, card_y + 14, "Wallpaper Library", 0x111827,
                    0xF5F5F7);
    gui_draw_string(card_x + 14, card_y + 28, "Pick a new wallpaper for this account.",
                    0x6B7280, 0xF5F5F7);
    gui_draw_rect(sidebar_x, card_y + 12, 2, card_h - 24, 0xD4D4D8);
    load_thumbnails();

    for (int i = 0; i < NUM_WALLPAPERS; i++) {
      char label_buf[32];
      int col = i % columns;
      int row = i / columns;
      int tx = grid_x + col * (thumb_w + gap_x);
      int ty = grid_y + row * (thumb_h + gap_y + 18);

      /* Draw preview based on type */
      if (wallpapers[i].type == 1) {
        /* Use cached thumbnail */
        media_image_t *thumb_img = &thumbnail_cache[i];

        if (thumb_img->pixels && thumb_img->width > 0) {
          /* Draw scaled image from cache */
          gui_draw_image_scaled(tx, ty, thumb_w, thumb_h, thumb_img);
        } else {
          /* Fallback - gray with "?" */
          gui_draw_rect(tx, ty, thumb_w, thumb_h, 0x3A3A4A);
          gui_draw_char(tx + thumb_w / 2 - 4, ty + thumb_h / 2 - 8, '?',
                        0x888888, 0x3A3A4A);
        }
      } else {
        /* Gradient wallpaper - draw gradient preview */
        for (int py = 0; py < thumb_h; py++) {
          int progress = (py * 256) / thumb_h;
          uint8_t pr = wallpapers[i].tr +
                       ((wallpapers[i].br - wallpapers[i].tr) * progress) / 256;
          uint8_t pg = wallpapers[i].tg +
                       ((wallpapers[i].bg - wallpapers[i].tg) * progress) / 256;
          uint8_t pb = wallpapers[i].tb +
                       ((wallpapers[i].bb - wallpapers[i].tb) * progress) / 256;
          uint32_t pcolor = (pr << 16) | (pg << 8) | pb;
          gui_draw_rect(tx, ty + py, thumb_w, 1, pcolor);
        }
      }

      /* Border - highlight if selected */
      uint32_t border_color = (i == current_wallpaper) ? 0x2563EB : 0x52525B;
      gui_draw_rect(tx - 2, ty - 2, thumb_w + 4, 2, border_color);
      gui_draw_rect(tx - 2, ty + thumb_h, thumb_w + 4, 2, border_color);
      gui_draw_rect(tx - 2, ty - 2, 2, thumb_h + 4, border_color);
      gui_draw_rect(tx + thumb_w, ty - 2, 2, thumb_h + 4, border_color);
      if (i == current_wallpaper) {
        gui_draw_rect(tx - 2, ty + thumb_h + 18, thumb_w + 4, 3, 0x2563EB);
      }

      /* Label */
      fm_truncate_label_px(wallpapers[i].name, label_buf, sizeof(label_buf),
                           thumb_w);
      gui_draw_string(tx, ty + thumb_h + 4, label_buf,
                      i == current_wallpaper ? 0x1D4ED8 : 0x52525B, 0xF5F5F7);
    }

    gui_draw_rect(sidebar_x + 10, preview_y, preview_w, preview_h, 0x252535);
    if (wallpapers[current_wallpaper].type == 1 &&
        thumbnail_cache[current_wallpaper].pixels) {
      media_image_t *thumb_img = &thumbnail_cache[current_wallpaper];
      gui_draw_image_scaled(preview_x + 8, preview_y + 8, preview_w - 16,
                            preview_h - 16, thumb_img);
    } else {
      for (int py = 0; py < preview_h - 16; py++) {
        int progress = (py * 256) / (preview_h - 16);
        uint8_t pr = wallpapers[current_wallpaper].tr +
                     ((wallpapers[current_wallpaper].br -
                       wallpapers[current_wallpaper].tr) *
                      progress) /
                         256;
        uint8_t pg = wallpapers[current_wallpaper].tg +
                     ((wallpapers[current_wallpaper].bg -
                       wallpapers[current_wallpaper].tg) *
                      progress) /
                         256;
        uint8_t pb = wallpapers[current_wallpaper].tb +
                     ((wallpapers[current_wallpaper].bb -
                       wallpapers[current_wallpaper].tb) *
                      progress) /
                         256;
        gui_draw_rect(preview_x + 8, preview_y + 8 + py, preview_w - 16, 1,
                      (pr << 16) | (pg << 8) | pb);
      }
    }

    gui_draw_string(sidebar_x + 10, card_y + 18, "Current wallpaper", 0x93C5FD,
                    0xF5F5F7);
    gui_draw_string(sidebar_x + 10, preview_y + preview_h + 14,
                    wallpapers[current_wallpaper].name, 0x111827, 0xF5F5F7);
    gui_draw_string(sidebar_x + 10, preview_y + preview_h + 34, wallpaper_kind,
                    0x4B5563, 0xF5F5F7);
    gui_draw_string(sidebar_x + 10, preview_y + preview_h + 54, blur_picker_status,
                    0x6366F1, 0xF5F5F7);
    gui_draw_string(sidebar_x + 10, preview_y + preview_h + 74, picker_hint,
                    0x6B7280, 0xF5F5F7);
    gui_draw_string(sidebar_x + 10, preview_y + preview_h + 94, g_gpu_backend_name,
                    0x6B7280, 0xF5F5F7);
    gui_draw_rect(sidebar_x + 10, card_y + card_h - 42, sidebar_w - 20, 26,
                  0x1D4ED8);
    gui_draw_string(sidebar_x + 28, card_y + card_h - 33, "Click a tile to apply",
                    0xFFFFFF, 0x1D4ED8);
  }

  /* Call window's draw callback if set */
  if (win->on_draw) {
    win->on_draw(win);
  }

  gui_restore_clip_rect(prev_clip);
}

static void draw_window_surface(struct window *win, int draw_x, int draw_y,
                                int draw_w, int draw_h) {
  int clip_x0 = draw_x;
  int clip_y0 = draw_y;
  int clip_x1 = draw_x + draw_w;
  int clip_y1 = draw_y + draw_h;
  int src_x0;
  int src_y0;
  int src_x1;
  int src_y1;
  int copy_w;
  int copy_h;
  uint32_t *target;

  if (!win || !win->content_buffer || draw_w <= 0 || draw_h <= 0)
    return;

  if (g_clip.enabled) {
    if (clip_x0 < g_clip.x0)
      clip_x0 = g_clip.x0;
    if (clip_y0 < g_clip.y0)
      clip_y0 = g_clip.y0;
    if (clip_x1 > g_clip.x1)
      clip_x1 = g_clip.x1;
    if (clip_y1 > g_clip.y1)
      clip_y1 = g_clip.y1;
  }

  if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1)
    return;

  target = gui_draw_target();
  if (!target)
    return;

  src_x0 = clip_x0 - draw_x;
  src_y0 = clip_y0 - draw_y;
  src_x1 = clip_x1 - draw_x;
  src_y1 = clip_y1 - draw_y;
  if (src_x0 < 0)
    src_x0 = 0;
  if (src_y0 < 0)
    src_y0 = 0;
  if (src_x1 > draw_w)
    src_x1 = draw_w;
  if (src_y1 > draw_h)
    src_y1 = draw_h;

  copy_w = src_x1 - src_x0;
  copy_h = src_y1 - src_y0;
  if (copy_w <= 0 || copy_h <= 0)
    return;

  if (draw_w == win->surface_width && draw_h == win->surface_height) {
    int dst_x = clip_x0 - g_render_target.origin_x;
    int dst_y = clip_y0 - g_render_target.origin_y;
    int dst_pitch = g_render_target.pitch_pixels;
    int src_pitch = win->surface_width;

    for (int row = 0; row < copy_h; row++) {
      uint32_t *src =
          win->content_buffer + (src_y0 + row) * src_pitch + src_x0;
      uint32_t *dst = target + (dst_y + row) * dst_pitch + dst_x;
      fast_memcpy_line(dst, src, copy_w);
    }
    return;
  }

  {
    int local_x0 = clip_x0 - draw_x;
    int local_y0 = clip_y0 - draw_y;
    int dst_x0 = clip_x0 - g_render_target.origin_x;
    int dst_y0 = clip_y0 - g_render_target.origin_y;
    int src_x_start = (local_x0 * win->surface_width) / draw_w;
    int src_y = (local_y0 * win->surface_height) / draw_h;
    int src_x_rem = (local_x0 * win->surface_width) % draw_w;
    int src_y_rem = (local_y0 * win->surface_height) % draw_h;
    int src_x_step = win->surface_width / draw_w;
    int src_y_step = win->surface_height / draw_h;
    int src_x_step_rem = win->surface_width % draw_w;
    int src_y_step_rem = win->surface_height % draw_h;

    for (int row = 0; row < copy_h; row++) {
      int clamped_src_y = src_y;
      int src_x = src_x_start;
      int x_rem = src_x_rem;
      uint32_t *dst =
          target + (dst_y0 + row) * g_render_target.pitch_pixels + dst_x0;

      if (clamped_src_y < 0)
        clamped_src_y = 0;
      if (clamped_src_y >= win->surface_height)
        clamped_src_y = win->surface_height - 1;

      uint32_t *src_row =
          win->content_buffer + clamped_src_y * win->surface_width;

      for (int col = 0; col < copy_w; col++) {
        int clamped_src_x = src_x;
        if (clamped_src_x < 0)
          clamped_src_x = 0;
        if (clamped_src_x >= win->surface_width)
          clamped_src_x = win->surface_width - 1;

        dst[col] = src_row[clamped_src_x];
        src_x += src_x_step;
        x_rem += src_x_step_rem;
        if (x_rem >= draw_w) {
          src_x++;
          x_rem -= draw_w;
        }
      }

      src_y += src_y_step;
      src_y_rem += src_y_step_rem;
      if (src_y_rem >= draw_h) {
        src_y++;
        src_y_rem -= draw_h;
      }
    }
  }
}

static void ensure_window_surface_current(struct window *win) {
  struct gui_render_target prev_target;
  struct gui_clip_state prev_clip;
  int dirty_x;
  int dirty_y;
  int dirty_w;
  int dirty_h;
  window_animation_t saved_animation;
  int saved_anim_frame;
  int saved_anim_total_frames;

  if (!win || win->id == 0)
    return;
  if (window_ensure_surface_storage(win) != 0 || !win->content_buffer)
    return;

  if (!win->surface_valid)
    window_mark_surface_dirty_full(win);
  if (!win->surface_dirty)
    return;

  dirty_x = win->surface_dirty_x;
  dirty_y = win->surface_dirty_y;
  dirty_w = win->surface_dirty_w;
  dirty_h = win->surface_dirty_h;

  prev_target =
      gui_set_render_target(win->content_buffer, win->surface_width,
                            win->surface_height, win->surface_width, win->x,
                            win->y);
  prev_clip =
      gui_set_clip_rect(win->x + dirty_x, win->y + dirty_y, dirty_w, dirty_h);
  saved_animation = win->animation;
  saved_anim_frame = win->anim_frame;
  saved_anim_total_frames = win->anim_total_frames;
  win->animation = WINDOW_ANIM_NONE;
  win->anim_frame = 0;
  win->anim_total_frames = 0;
  draw_window_internal(win);
  win->animation = saved_animation;
  win->anim_frame = saved_anim_frame;
  win->anim_total_frames = saved_anim_total_frames;
  gui_restore_clip_rect(prev_clip);
  g_render_target = prev_target;

  win->surface_valid = 1;
  win->surface_dirty = 0;
  win->surface_dirty_x = 0;
  win->surface_dirty_y = 0;
  win->surface_dirty_w = 0;
  win->surface_dirty_h = 0;
}

static void draw_window(struct window *win) {
  int draw_x;
  int draw_y;
  int draw_w;
  int draw_h;

  if (!win || !win->visible)
    return;

  ensure_window_surface_current(win);
  if (!win->content_buffer || !win->surface_valid) {
    draw_window_internal(win);
    return;
  }

  window_get_draw_rect(win, &draw_x, &draw_y, &draw_w, &draw_h);
  draw_window_surface(win, draw_x, draw_y, draw_w, draw_h);
}

/* ===================================================================== */
/* Desktop with Menu Bar and Dock */
/* ===================================================================== */

/* Menu dropdown state */
static int menu_open = 0; /* 0=closed, 1=main menu open */
static int main_menu_power_open = 0;
static int main_menu_power_row_y_anim = -1;
static int wifi_tray_open = 0;
static int main_menu_all_programs_open = 0;
static int main_menu_program_scroll = 0;
static void main_menu_mark_dirty(void);
static void wifi_tray_mark_dirty(void);
static void draw_system_app_icon_kind(gui_app_kind_t kind, int x, int y,
                                      int size);

#define MAIN_MENU_W 364
#define MAIN_MENU_H 350
#define MAIN_MENU_HEADER_H 60
#define MAIN_MENU_LEFT_W 214
#define MAIN_MENU_ROW_H 34
#define MAIN_MENU_RIGHT_ROW_H 28
#define MAIN_MENU_SCROLL_BUTTON_H 22
#define WIFI_TRAY_PANEL_W 248
#define WIFI_TRAY_PANEL_H 208

#define MAIN_MENU_ITEM_PROGRAM_BASE 1000
#define MAIN_MENU_ITEM_PROGRAM_SCROLL_UP 2000
#define MAIN_MENU_ITEM_PROGRAM_SCROLL_DOWN 2001

enum {
  MAIN_MENU_ITEM_NONE = -1,
  MAIN_MENU_ITEM_ABOUT = 0,
  MAIN_MENU_ITEM_ALL_PROGRAMS,
  MAIN_MENU_ITEM_TERMINAL,
  MAIN_MENU_ITEM_FILES,
  MAIN_MENU_ITEM_NOTES,
  MAIN_MENU_ITEM_SETTINGS,
  MAIN_MENU_ITEM_BROWSER,
  MAIN_MENU_ITEM_APPSTORE,
  MAIN_MENU_ITEM_POWER,
  MAIN_MENU_ITEM_POWER_LOGOUT,
  MAIN_MENU_ITEM_POWER_SHUTDOWN,
  MAIN_MENU_ITEM_POWER_RESTART,
  MAIN_MENU_ITEM_COUNT
};

static void main_menu_launcher_button_rect(int *x, int *y, int *w, int *h) {
  int dock_y = (int)primary_display.height - DOCK_HEIGHT;
  int size = 50;

  if (x)
    *x = 14;
  if (y)
    *y = dock_y + (DOCK_HEIGHT - size) / 2;
  if (w)
    *w = size;
  if (h)
    *h = size;
}

static void dock_status_panel_rect(int dock_y, int dock_h, int *x, int *y, int *w,
                                   int *h) {
  int panel_w = 122;
  int panel_h = 34;
  int panel_x = (int)primary_display.width - panel_w - 16;
  int panel_y = dock_y + (dock_h - panel_h) / 2;

  if (x)
    *x = panel_x;
  if (y)
    *y = panel_y;
  if (w)
    *w = panel_w;
  if (h)
    *h = panel_h;
}

static void wifi_tray_button_rect(int dock_y, int dock_h, int *x, int *y, int *w,
                                  int *h) {
  int panel_x, panel_y, panel_w, panel_h;

  dock_status_panel_rect(dock_y, dock_h, &panel_x, &panel_y, &panel_w, &panel_h);
  if (x)
    *x = panel_x + 8;
  if (y)
    *y = panel_y + 5;
  if (w)
    *w = 36;
  if (h)
    *h = panel_h - 10;
}

static void wifi_tray_panel_rect(int dock_y, int dock_h, int *x, int *y, int *w,
                                 int *h) {
  int panel_x, panel_y, panel_w, panel_h;

  dock_status_panel_rect(dock_y, dock_h, &panel_x, &panel_y, &panel_w, &panel_h);
  if (x)
    *x = panel_x + panel_w - WIFI_TRAY_PANEL_W;
  if (y)
    *y = panel_y - WIFI_TRAY_PANEL_H - 10;
  if (w)
    *w = WIFI_TRAY_PANEL_W;
  if (h)
    *h = WIFI_TRAY_PANEL_H;
}

static int wifi_tray_contains_point(int x, int y) {
  int dock_y = (int)primary_display.height - DOCK_HEIGHT;
  int dock_h = DOCK_HEIGHT;
  int bx, by, bw, bh;
  int px, py, pw, ph;

  wifi_tray_button_rect(dock_y, dock_h, &bx, &by, &bw, &bh);
  if (x >= bx && x < bx + bw && y >= by && y < by + bh)
    return 1;

  if (!wifi_tray_open)
    return 0;

  wifi_tray_panel_rect(dock_y, dock_h, &px, &py, &pw, &ph);
  return x >= px && x < px + pw && y >= py && y < py + ph;
}

static void wifi_tray_mark_dirty(void) {
  int dock_y;
  int dock_h;
  int x;
  int y;
  int w;
  int h;

  if (!dock_is_visible())
    return;

  dock_y = (int)primary_display.height - DOCK_HEIGHT;
  dock_h = DOCK_HEIGHT;
  dock_status_panel_rect(dock_y, dock_h, &x, &y, &w, &h);
  compositor_mark_dirty(x - 8, y - 8, w + 16, h + 16);

  if (wifi_tray_open) {
    wifi_tray_panel_rect(dock_y, dock_h, &x, &y, &w, &h);
    compositor_mark_dirty(x - 12, y - 12, w + 24, h + 24);
  }
}

static int main_menu_panel_x(void) {
  int launcher_x, launcher_y, launcher_w, launcher_h;
  int x;

  main_menu_launcher_button_rect(&launcher_x, &launcher_y, &launcher_w,
                                 &launcher_h);
  x = launcher_x - 6;
  if (x < 8)
    x = 8;
  if (x + MAIN_MENU_W > (int)primary_display.width - 8)
    x = (int)primary_display.width - MAIN_MENU_W - 8;
  return x;
}

static int main_menu_panel_y(void) {
  int dock_y = (int)primary_display.height - DOCK_HEIGHT;
  int y = dock_y - MAIN_MENU_H + 10;
  if (y < MENU_BAR_HEIGHT + 8)
    y = MENU_BAR_HEIGHT + 8;
  return y;
}

static void main_menu_panel_rect(int *x, int *y, int *w, int *h) {
  if (x)
    *x = main_menu_panel_x();
  if (y)
    *y = main_menu_panel_y();
  if (w)
    *w = MAIN_MENU_W;
  if (h)
    *h = MAIN_MENU_H;
}

static int main_menu_power_row_y(void) {
  int panel_y = main_menu_panel_y();
  int extra_dropdown_h = session_can_logout() ? (MAIN_MENU_RIGHT_ROW_H + 4) : 0;
  return panel_y + MAIN_MENU_H - 108 - extra_dropdown_h;
}

static int main_menu_power_row_y_current(void) {
  if (main_menu_power_row_y_anim < 0)
    return main_menu_power_row_y();
  return main_menu_power_row_y_anim;
}

static void update_main_menu_power_animation(void) {
  int target_y = main_menu_power_row_y();
  int diff;

  if (main_menu_power_row_y_anim < 0) {
    main_menu_power_row_y_anim = target_y;
    return;
  }

  diff = target_y - main_menu_power_row_y_anim;
  if (diff == 0)
    return;
  if (diff > 0) {
    int step = diff / 4;
    if (step < 1)
      step = 1;
    main_menu_power_row_y_anim += step;
  } else {
    int step = (-diff) / 4;
    if (step < 1)
      step = 1;
    main_menu_power_row_y_anim -= step;
  }
  main_menu_mark_dirty();
}

static int main_menu_contains_point(int x, int y) {
  int panel_x, panel_y, panel_w, panel_h;
  main_menu_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
  return x >= panel_x && x < panel_x + panel_w && y >= panel_y &&
         y < panel_y + panel_h;
}

static void main_menu_mark_dirty(void) {
  int x, y, w, h;

  main_menu_panel_rect(&x, &y, &w, &h);
  compositor_mark_dirty(x - 12, y - 12, w + 24, h + 30);

  main_menu_launcher_button_rect(&x, &y, &w, &h);
  compositor_mark_dirty(x - 6, y - 6, w + 12, h + 12);
}

static int main_menu_item_bounds(int item_index, int *x, int *y, int *w,
                                 int *h) {
  int panel_x = main_menu_panel_x();
  int panel_y = main_menu_panel_y();
  int left_x = panel_x + 12;
  int left_y = panel_y + MAIN_MENU_HEADER_H + 14;
  int left_w = MAIN_MENU_LEFT_W - 24;
  int right_x = panel_x + MAIN_MENU_LEFT_W + 10;
  int right_y = panel_y + MAIN_MENU_HEADER_H + 18;
  int right_w = MAIN_MENU_W - MAIN_MENU_LEFT_W - 22;

  if (item_index < 0 || item_index >= MAIN_MENU_ITEM_COUNT)
    return 0;

  switch (item_index) {
  case MAIN_MENU_ITEM_TERMINAL:
  case MAIN_MENU_ITEM_FILES:
  case MAIN_MENU_ITEM_NOTES:
  case MAIN_MENU_ITEM_SETTINGS:
  case MAIN_MENU_ITEM_BROWSER:
  case MAIN_MENU_ITEM_APPSTORE:
    if (main_menu_all_programs_open)
      return 0;
    if (x)
      *x = left_x;
    if (y)
      *y = left_y + (item_index - MAIN_MENU_ITEM_TERMINAL) * MAIN_MENU_ROW_H;
    if (w)
      *w = left_w;
    if (h)
      *h = MAIN_MENU_ROW_H - 2;
    return 1;
  case MAIN_MENU_ITEM_ABOUT:
    if (x)
      *x = right_x;
    if (y)
      *y = right_y;
    if (w)
      *w = right_w;
    if (h)
      *h = MAIN_MENU_RIGHT_ROW_H;
    return 1;
  case MAIN_MENU_ITEM_ALL_PROGRAMS:
    if (x)
      *x = right_x;
    if (y)
      *y = right_y + MAIN_MENU_RIGHT_ROW_H + 8;
    if (w)
      *w = right_w;
    if (h)
      *h = MAIN_MENU_RIGHT_ROW_H;
    return 1;
  case MAIN_MENU_ITEM_POWER:
    if (x)
      *x = right_x;
    if (y)
      *y = main_menu_power_row_y_current();
    if (w)
      *w = right_w;
    if (h)
      *h = MAIN_MENU_RIGHT_ROW_H;
    return 1;
  default:
    return 0;
  }
}

static int main_menu_program_list_rect(int *x, int *y, int *w, int *h) {
  int panel_x = main_menu_panel_x();
  int panel_y = main_menu_panel_y();

  if (x)
    *x = panel_x + 12;
  if (y)
    *y = panel_y + MAIN_MENU_HEADER_H + 14;
  if (w)
    *w = MAIN_MENU_LEFT_W - 24;
  if (h)
    *h = MAIN_MENU_H - MAIN_MENU_HEADER_H - 28;
  return 1;
}

static int main_menu_program_content_rect(int *x, int *y, int *w, int *h) {
  int list_x, list_y, list_w, list_h;

  main_menu_program_list_rect(&list_x, &list_y, &list_w, &list_h);

  if (x)
    *x = list_x;
  if (y)
    *y = list_y + MAIN_MENU_SCROLL_BUTTON_H + 6;
  if (w)
    *w = list_w - 30;
  if (h)
    *h = list_h - MAIN_MENU_SCROLL_BUTTON_H * 2 - 12;
  return 1;
}

static int main_menu_program_scroll_max(void) {
  int visible_rows = 0;
  int list_x, list_y, list_w, list_h;

  main_menu_program_content_rect(&list_x, &list_y, &list_w, &list_h);
  visible_rows = list_h / MAIN_MENU_ROW_H;
  if (visible_rows < 1)
    visible_rows = 1;

  load_system_app_catalog();
  if (app_catalog_count <= visible_rows)
    return 0;
  return app_catalog_count - visible_rows;
}

static void main_menu_clamp_program_scroll(void) {
  int max_scroll = main_menu_program_scroll_max();
  if (main_menu_program_scroll < 0)
    main_menu_program_scroll = 0;
  if (main_menu_program_scroll > max_scroll)
    main_menu_program_scroll = max_scroll;
}

static int main_menu_program_visible_rows(void) {
  int list_x, list_y, list_w, list_h;
  int visible_rows;

  main_menu_program_content_rect(&list_x, &list_y, &list_w, &list_h);
  visible_rows = list_h / MAIN_MENU_ROW_H;
  if (visible_rows < 1)
    visible_rows = 1;
  return visible_rows;
}

static const dock_app_def_t *main_menu_program_by_visible_index(int visible_index) {
  load_system_app_catalog();
  if (visible_index < 0 || visible_index >= app_catalog_count)
    return NULL;
  return &app_catalog[visible_index];
}

static int main_menu_program_row_bounds(int visible_slot, int *x, int *y, int *w,
                                        int *h) {
  int list_x, list_y, list_w, list_h;
  int visible_rows = main_menu_program_visible_rows();

  if (visible_slot < 0 || visible_slot >= visible_rows)
    return 0;

  main_menu_program_content_rect(&list_x, &list_y, &list_w, &list_h);
  if (list_w < 48)
    list_w = 48;

  if (x)
    *x = list_x;
  if (y)
    *y = list_y + visible_slot * MAIN_MENU_ROW_H;
  if (w)
    *w = list_w;
  if (h)
    *h = MAIN_MENU_ROW_H - 2;
  return 1;
}

static int main_menu_program_scroll_button_bounds(int direction, int *x, int *y,
                                                  int *w, int *h) {
  int list_x, list_y, list_w, list_h;
  int button_x;

  main_menu_program_list_rect(&list_x, &list_y, &list_w, &list_h);
  button_x = list_x + list_w - 24;

  if (x)
    *x = button_x;
  if (y)
    *y = direction < 0 ? list_y : list_y + list_h - MAIN_MENU_SCROLL_BUTTON_H;
  if (w)
    *w = 24;
  if (h)
    *h = MAIN_MENU_SCROLL_BUTTON_H;
  return 1;
}

static int main_menu_power_item_bounds(int item_index, int *x, int *y, int *w,
                                       int *h) {
  int power_x, power_y, power_w, power_h;
  int dropdown_y;

  if (!main_menu_power_open)
    return 0;
  if (!main_menu_item_bounds(MAIN_MENU_ITEM_POWER, &power_x, &power_y, &power_w,
                             &power_h))
    return 0;
  dropdown_y = power_y + power_h + 8;

  switch (item_index) {
  case MAIN_MENU_ITEM_POWER_LOGOUT:
    if (!session_can_logout())
      return 0;
    if (x)
      *x = power_x + 8;
    if (y)
      *y = dropdown_y;
    if (w)
      *w = power_w - 8;
    if (h)
      *h = MAIN_MENU_RIGHT_ROW_H;
    return 1;
  case MAIN_MENU_ITEM_POWER_SHUTDOWN:
    if (x)
      *x = power_x + 8;
    if (y)
      *y = dropdown_y + (session_can_logout() ? MAIN_MENU_RIGHT_ROW_H + 4 : 0);
    if (w)
      *w = power_w - 8;
    if (h)
      *h = MAIN_MENU_RIGHT_ROW_H;
    return 1;
  case MAIN_MENU_ITEM_POWER_RESTART:
    if (x)
      *x = power_x + 8;
    if (y)
      *y = dropdown_y +
           (session_can_logout() ? (MAIN_MENU_RIGHT_ROW_H + 4) * 2
                                 : MAIN_MENU_RIGHT_ROW_H + 4);
    if (w)
      *w = power_w - 8;
    if (h)
      *h = MAIN_MENU_RIGHT_ROW_H;
    return 1;
  default:
    return 0;
  }
}

static int main_menu_item_at(int x, int y) {
  if (!menu_open)
    return MAIN_MENU_ITEM_NONE;
  if (!main_menu_contains_point(x, y))
    return MAIN_MENU_ITEM_NONE;

  if (main_menu_all_programs_open) {
    const int visible_items[] = {MAIN_MENU_ITEM_ABOUT, MAIN_MENU_ITEM_ALL_PROGRAMS,
                                 MAIN_MENU_ITEM_POWER};
    int btn_x, btn_y, btn_w, btn_h;
    int visible_rows = main_menu_program_visible_rows();

    for (int i = 0; i < (int)(sizeof(visible_items) / sizeof(visible_items[0]));
         i++) {
      int item_x, item_y, item_w, item_h;
      int item_index = visible_items[i];
      if (!main_menu_item_bounds(item_index, &item_x, &item_y, &item_w, &item_h))
        continue;
      if (x >= item_x && x < item_x + item_w && y >= item_y &&
          y < item_y + item_h)
        return item_index;
    }

    if (main_menu_program_scroll_button_bounds(-1, &btn_x, &btn_y, &btn_w, &btn_h) &&
        x >= btn_x && x < btn_x + btn_w && y >= btn_y && y < btn_y + btn_h)
      return MAIN_MENU_ITEM_PROGRAM_SCROLL_UP;
    if (main_menu_program_scroll_button_bounds(1, &btn_x, &btn_y, &btn_w, &btn_h) &&
        x >= btn_x && x < btn_x + btn_w && y >= btn_y && y < btn_y + btn_h)
      return MAIN_MENU_ITEM_PROGRAM_SCROLL_DOWN;

    main_menu_clamp_program_scroll();
    for (int slot = 0; slot < visible_rows; slot++) {
      int row_x, row_y, row_w, row_h;
      const dock_app_def_t *app =
          main_menu_program_by_visible_index(main_menu_program_scroll + slot);
      if (!app)
        break;
      if (!main_menu_program_row_bounds(slot, &row_x, &row_y, &row_w, &row_h))
        continue;
      if (x >= row_x && x < row_x + row_w && y >= row_y && y < row_y + row_h)
        return MAIN_MENU_ITEM_PROGRAM_BASE + main_menu_program_scroll + slot;
    }
  } else {
    for (int i = 0; i < MAIN_MENU_ITEM_COUNT; i++) {
      int item_x, item_y, item_w, item_h;
      if (!main_menu_item_bounds(i, &item_x, &item_y, &item_w, &item_h))
        continue;
      if (x >= item_x && x < item_x + item_w && y >= item_y &&
          y < item_y + item_h)
        return i;
    }
  }

  for (int i = MAIN_MENU_ITEM_POWER_LOGOUT;
       i <= MAIN_MENU_ITEM_POWER_RESTART; i++) {
    int item_x, item_y, item_w, item_h;
    if (!main_menu_power_item_bounds(i, &item_x, &item_y, &item_w, &item_h))
      continue;
    if (x >= item_x && x < item_x + item_w && y >= item_y &&
        y < item_y + item_h)
      return i;
  }
  return MAIN_MENU_ITEM_NONE;
}

static void draw_main_menu_row(int item_index, const char *label,
                               const char *subtitle, uint32_t accent,
                               int compact) {
  int row_x, row_y, row_w, row_h;
  int hovered = (main_menu_item_at(mouse_x, mouse_y) == item_index);

  if (!main_menu_item_bounds(item_index, &row_x, &row_y, &row_w, &row_h))
    return;

  if (!compact) {
    gui_fill_rect_alpha(row_x, row_y, row_w, row_h,
                        hovered ? 0x4A355177 : 0x24202A38);
    gui_draw_rect_outline(row_x, row_y, row_w, row_h,
                          hovered ? 0x8AB7DAFF : 0x304A586B, 1);
    gui_fill_rect_alpha(row_x + 6, row_y + 5, 22, 22, accent | 0x66000000);
    gui_draw_rect_outline(row_x + 6, row_y + 5, 22, 22, 0x90FFFFFF, 1);
    gui_draw_string(row_x + 36, row_y + 6, label, 0xF4F7FB, 0x00000000);
    if (subtitle)
      gui_draw_string(row_x + 36, row_y + 18, subtitle, 0xA7B4C4, 0x00000000);
  } else {
    gui_fill_rect_alpha(row_x, row_y, row_w, row_h,
                        hovered ? 0x42486178 : 0x24313D50);
    gui_draw_rect_outline(row_x, row_y, row_w, row_h,
                          hovered ? 0x8AB7DAFF : 0x304A586B, 1);
    gui_draw_string(row_x + 10, row_y + 7, label,
                    hovered ? 0xFFFFFF : 0xEAF2FF, 0x00000000);
    if (subtitle)
      gui_draw_string(row_x + row_w - 56, row_y + 7, subtitle,
                      hovered ? 0xE6F4FF : 0xBAC8D7, 0x00000000);
  }
}

static void draw_main_menu_power_dropdown(void) {
  int power_x, power_y, power_w, power_h;
  int item_x, item_y, item_w, item_h;
  int hovered_item = main_menu_item_at(mouse_x, mouse_y);
  int dropdown_h = session_can_logout() ? 94 : 62;

  if (!main_menu_power_open)
    return;
  if (!main_menu_item_bounds(MAIN_MENU_ITEM_POWER, &power_x, &power_y, &power_w,
                             &power_h))
    return;

  gui_fill_rect_alpha(power_x + 6, power_y + power_h + 6, power_w - 2, dropdown_h,
                      0x28070C14);
  gui_draw_glass_panel(power_x, power_y + power_h + 4, power_w, dropdown_h, 0x6A2C3446,
                       0x24FFFFFF, 0x8C75839A, 1);

  if (main_menu_power_item_bounds(MAIN_MENU_ITEM_POWER_LOGOUT, &item_x, &item_y,
                                  &item_w, &item_h)) {
    if (hovered_item == MAIN_MENU_ITEM_POWER_LOGOUT) {
      gui_fill_rect_alpha(item_x, item_y, item_w, item_h, 0x507256D9);
      gui_draw_rect_outline(item_x, item_y, item_w, item_h, 0xAFCDBBFF, 1);
    }
    gui_draw_string(item_x + 8, item_y + 7, "Logout", 0xFFFFFF, 0x00000000);
  }

  if (main_menu_power_item_bounds(MAIN_MENU_ITEM_POWER_SHUTDOWN, &item_x, &item_y,
                                  &item_w, &item_h)) {
    if (hovered_item == MAIN_MENU_ITEM_POWER_SHUTDOWN) {
      gui_fill_rect_alpha(item_x, item_y, item_w, item_h, 0x50C84C4C);
      gui_draw_rect_outline(item_x, item_y, item_w, item_h, 0xA5F6B0B0, 1);
    }
    gui_draw_string(item_x + 8, item_y + 7, "Shutdown", 0xFFFFFF, 0x00000000);
  }

  if (main_menu_power_item_bounds(MAIN_MENU_ITEM_POWER_RESTART, &item_x, &item_y,
                                  &item_w, &item_h)) {
    if (hovered_item == MAIN_MENU_ITEM_POWER_RESTART) {
      gui_fill_rect_alpha(item_x, item_y, item_w, item_h, 0x50D98F38);
      gui_draw_rect_outline(item_x, item_y, item_w, item_h, 0xA8FFE2B6, 1);
    }
    gui_draw_string(item_x + 8, item_y + 7, "Restart", 0xFFFFFF, 0x00000000);
  }
}

static void draw_main_menu_all_programs_list(void) {
  int list_x, list_y, list_w, list_h;
  int content_x, content_y, content_w, content_h;
  int visible_rows = main_menu_program_visible_rows();
  int hovered_item = main_menu_item_at(mouse_x, mouse_y);

  load_system_app_catalog();
  main_menu_program_list_rect(&list_x, &list_y, &list_w, &list_h);
  main_menu_program_content_rect(&content_x, &content_y, &content_w, &content_h);
  main_menu_clamp_program_scroll();

  gui_fill_rect_alpha(list_x, list_y, list_w - 30, list_h, 0x18212D3B);
  gui_fill_rect_alpha(list_x + list_w - 24, list_y, 24, list_h, 0x202B3B52);
  gui_fill_rect_alpha(content_x, content_y, content_w, content_h, 0x10212D3B);

  for (int slot = 0; slot < visible_rows; slot++) {
    int row_x, row_y, row_w, row_h;
    const dock_app_def_t *app =
        main_menu_program_by_visible_index(main_menu_program_scroll + slot);
    char label_buf[64];
    int label_max_px;
    int hovered;

    if (!app)
      break;
    if (!main_menu_program_row_bounds(slot, &row_x, &row_y, &row_w, &row_h))
      continue;

    hovered = (hovered_item == MAIN_MENU_ITEM_PROGRAM_BASE +
                                   main_menu_program_scroll + slot);
    gui_fill_rect_alpha(row_x, row_y, row_w, row_h,
                        hovered ? 0x4A355177 : 0x24202A38);
    gui_draw_rect_outline(row_x, row_y, row_w, row_h,
                          hovered ? 0x8AB7DAFF : 0x304A586B, 1);
    gui_fill_rect_alpha(row_x + 6, row_y + 5, 22, 22,
                        app->icon_color | 0x66000000);
    gui_draw_rect_outline(row_x + 6, row_y + 5, 22, 22, 0x90FFFFFF, 1);
    draw_system_app_icon_kind(app->kind, row_x + 7, row_y + 6, 20);
    label_max_px = row_w - 46;
    if (label_max_px < 8)
      label_max_px = 8;
    fm_truncate_label_px(app->label, label_buf, sizeof(label_buf), label_max_px);
    gui_draw_string(row_x + 36, row_y + 10, label_buf, 0xF4F7FB, 0x00000000);
  }

  {
    int btn_x, btn_y, btn_w, btn_h;
    char count_buf[32] = "";
    int count_idx = 0;
    int max_scroll = main_menu_program_scroll_max();

    append_decimal(count_buf, &count_idx, app_catalog_count);
    notepad_append_to_buf(count_buf, sizeof(count_buf), " apps");
    gui_draw_string(list_x + 4, list_y + 6, count_buf, 0xA7B4C4, 0x00000000);

    main_menu_program_scroll_button_bounds(-1, &btn_x, &btn_y, &btn_w, &btn_h);
    gui_fill_rect_alpha(btn_x, btn_y, btn_w, btn_h,
                        hovered_item == MAIN_MENU_ITEM_PROGRAM_SCROLL_UP
                            ? 0x42486178
                            : 0x24313D50);
    gui_draw_rect_outline(btn_x, btn_y, btn_w, btn_h, 0x304A586B, 1);
    gui_draw_string(btn_x + 8, btn_y + 6, "^", 0xEAF2FF, 0x00000000);

    main_menu_program_scroll_button_bounds(1, &btn_x, &btn_y, &btn_w, &btn_h);
    gui_fill_rect_alpha(btn_x, btn_y, btn_w, btn_h,
                        hovered_item == MAIN_MENU_ITEM_PROGRAM_SCROLL_DOWN
                            ? 0x42486178
                            : 0x24313D50);
    gui_draw_rect_outline(btn_x, btn_y, btn_w, btn_h, 0x304A586B, 1);
    gui_draw_string(btn_x + 8, btn_y + 6, "v", 0xEAF2FF, 0x00000000);

    if (max_scroll > 0 && app_catalog_count > 0) {
      int track_x = list_x + list_w - 15;
      int track_y = list_y + MAIN_MENU_SCROLL_BUTTON_H + 6;
      int track_h = list_h - MAIN_MENU_SCROLL_BUTTON_H * 2 - 12;
      int thumb_h = (visible_rows * track_h) / app_catalog_count;
      int thumb_y = track_y;

      if (thumb_h < 20)
        thumb_h = 20;
      if (track_h > thumb_h)
        thumb_y += (main_menu_program_scroll * (track_h - thumb_h)) / max_scroll;

      gui_fill_rect_alpha(track_x, track_y, 6, track_h, 0x18273548);
      gui_fill_rect_alpha(track_x, thumb_y, 6, thumb_h, 0x6A89B4FA);
    }
  }
}

static void draw_main_menu_panel(void) {
  const gui_theme_palette_t *theme = gui_theme_palette();
  int panel_x, panel_y, panel_w, panel_h;
  int launcher_x, launcher_y, launcher_w, launcher_h;
  int connector_x;

  if (!menu_open)
    return;

  main_menu_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
  main_menu_launcher_button_rect(&launcher_x, &launcher_y, &launcher_w,
                                 &launcher_h);
  connector_x = launcher_x + launcher_w / 2 - 10;
  if (connector_x < panel_x + 20)
    connector_x = panel_x + 20;
  if (connector_x > panel_x + panel_w - 20)
    connector_x = panel_x + panel_w - 20;

  gui_fill_rect_alpha(panel_x + 8, panel_y + 8, panel_w, panel_h + 2,
                      0x28050910);
  gui_fill_rect_alpha(connector_x + 4, panel_y + panel_h - 2, 20, 16,
                      0x24050910);

  gui_draw_glass_panel(panel_x, panel_y, panel_w, panel_h, theme->menu_tint,
                       theme->menu_glow, theme->menu_border, 2);
  gui_fill_visual_effect_rect(panel_x + BORDER_WIDTH, panel_y + BORDER_WIDTH,
                              panel_w - BORDER_WIDTH * 2, TITLEBAR_HEIGHT,
                              theme->menu_title);
  gui_fill_visual_effect_rect(panel_x + BORDER_WIDTH,
                              panel_y + BORDER_WIDTH + TITLEBAR_HEIGHT - 1,
                              panel_w - BORDER_WIDTH * 2, 1,
                              theme->title_line_top);

  gui_draw_string(panel_x + 21, panel_y + 8, "OS 8", 0xFFF7FBFF,
                  0x00000000);
  gui_fill_rect_alpha(panel_x + BORDER_WIDTH, panel_y + BORDER_WIDTH + TITLEBAR_HEIGHT,
                      MAIN_MENU_LEFT_W - BORDER_WIDTH, panel_h - TITLEBAR_HEIGHT - BORDER_WIDTH * 2,
                      theme->menu_left);
  gui_fill_rect_alpha(panel_x + MAIN_MENU_LEFT_W, panel_y + BORDER_WIDTH + TITLEBAR_HEIGHT,
                      panel_w - MAIN_MENU_LEFT_W - BORDER_WIDTH, panel_h - TITLEBAR_HEIGHT - BORDER_WIDTH * 2,
                      theme->menu_right);
  gui_fill_rect_alpha(panel_x + MAIN_MENU_LEFT_W - 1,
                      panel_y + BORDER_WIDTH + TITLEBAR_HEIGHT, 1,
                      panel_h - TITLEBAR_HEIGHT - BORDER_WIDTH * 2,
                      theme->menu_divider);

  gui_fill_rect_alpha(panel_x + 14, panel_y + 40, 42, 42, 0x40556F92);
  draw_filled_circle(panel_x + 35, panel_y + 61, 16, 0xFFFFFFFF);
  if (gui_draw_embedded_logo(panel_x + 19, panel_y + 45, 32, 32) != 0) {
    gui_draw_os_logo(panel_x + 24, panel_y + 50, 2, 0x3B82F6, 0x1D4ED8,
                     0x00000000);
  }
  gui_draw_string(panel_x + 68, panel_y + 45, "Username", 0xFFFFFF,
                  0x00000000);
  gui_draw_string(panel_x + MAIN_MENU_LEFT_W + 16, panel_y + 46, "System",
                  0xFFFFFF, 0x00000000);

  if (main_menu_all_programs_open) {
    draw_main_menu_all_programs_list();
  } else {
    draw_main_menu_row(MAIN_MENU_ITEM_TERMINAL, "Terminal", "Console and shell",
                       0x1F2937, 0);
    draw_main_menu_row(MAIN_MENU_ITEM_FILES, "Files", "Browse folders",
                       0x3B82F6, 0);
    draw_main_menu_row(MAIN_MENU_ITEM_NOTES, "Notepad", "Quick editing",
                       0xFACC15, 0);
    draw_main_menu_row(MAIN_MENU_ITEM_SETTINGS, "Settings", "System controls",
                       0x9CA3AF, 0);
    draw_main_menu_row(MAIN_MENU_ITEM_BROWSER, "Browser", "Open the web",
                       0x0EA5E9, 0);
    draw_main_menu_row(MAIN_MENU_ITEM_APPSTORE, "App Store", "Install apps",
                       0x7C3AED, 0);
  }

  draw_main_menu_row(MAIN_MENU_ITEM_ABOUT, "About OS", NULL, 0x89B4FA, 1);
  draw_main_menu_row(MAIN_MENU_ITEM_ALL_PROGRAMS, "All Programs",
                     main_menu_all_programs_open ? "Hide" : ">",
                     0x60A5FA, 1);
  draw_main_menu_row(MAIN_MENU_ITEM_POWER, "Power", main_menu_power_open ? "v" : ">",
                     0xDC2626, 1);
  draw_main_menu_power_dropdown();
}

static int main_menu_activate(int item_index) {
  int menu_was_visible = menu_open;

  switch (item_index) {
  case MAIN_MENU_ITEM_ABOUT:
    gui_create_window("About", 210, 140, 560, 360);
    break;
  case MAIN_MENU_ITEM_ALL_PROGRAMS:
    main_menu_mark_dirty();
    main_menu_all_programs_open = main_menu_all_programs_open ? 0 : 1;
    main_menu_program_scroll = 0;
    main_menu_mark_dirty();
    return 1;
  case MAIN_MENU_ITEM_PROGRAM_SCROLL_UP:
    if (main_menu_program_scroll > 0) {
      main_menu_mark_dirty();
      main_menu_program_scroll--;
      main_menu_mark_dirty();
    }
    return 1;
  case MAIN_MENU_ITEM_PROGRAM_SCROLL_DOWN:
    if (main_menu_program_scroll < main_menu_program_scroll_max()) {
      main_menu_mark_dirty();
      main_menu_program_scroll++;
      main_menu_mark_dirty();
    }
    return 1;
  case MAIN_MENU_ITEM_POWER:
    main_menu_mark_dirty();
    main_menu_power_open = main_menu_power_open ? 0 : 1;
    main_menu_mark_dirty();
    return 1;
  case MAIN_MENU_ITEM_POWER_LOGOUT:
    main_menu_mark_dirty();
    main_menu_power_open = 0;
    menu_open = 0;
    main_menu_mark_dirty();
    str_copy_safe(startup_input_username, account_username,
                  sizeof(startup_input_username));
    startup_begin_login_flow("Signed out.", 1);
    return 1;
  case MAIN_MENU_ITEM_POWER_SHUTDOWN: {
    extern void arch_poweroff(void);
    main_menu_power_open = 0;
    gui_flush_account_state_before_power_transition();
    arch_poweroff();
    break;
  }
  case MAIN_MENU_ITEM_POWER_RESTART: {
    extern void arch_reboot(void);
    main_menu_power_open = 0;
    gui_flush_account_state_before_power_transition();
    arch_reboot();
    break;
  }
  case MAIN_MENU_ITEM_TERMINAL:
    gui_launch_app_by_id("terminal");
    break;
  case MAIN_MENU_ITEM_FILES:
    gui_launch_app_by_id("files");
    break;
  case MAIN_MENU_ITEM_NOTES:
    gui_launch_app_by_id("notes");
    break;
  case MAIN_MENU_ITEM_SETTINGS:
    gui_launch_app_by_id("settings");
    break;
  case MAIN_MENU_ITEM_BROWSER:
    gui_launch_app_by_id("browser");
    break;
  case MAIN_MENU_ITEM_APPSTORE:
    gui_launch_app_by_id("appstore");
    break;
  default:
    if (item_index >= MAIN_MENU_ITEM_PROGRAM_BASE) {
      const dock_app_def_t *app =
          main_menu_program_by_visible_index(item_index - MAIN_MENU_ITEM_PROGRAM_BASE);
      if (!app)
        return 0;
      gui_focus_or_launch_app_by_id(app->id);
      break;
    }
    return 0;
  }

  if (menu_was_visible)
    main_menu_mark_dirty();
  menu_open = 0;
  main_menu_power_open = 0;
  main_menu_all_programs_open = 0;
  main_menu_mark_dirty();
  return 1;
}

static void draw_menu_bar(void) {
  return;
}

/* Dock icons */
#include "icons.h"

#define DOCK_ICON_SIZE 44  /* Slightly smaller for more icons */
#define DOCK_ICON_MARGIN 4 /* Padding inside dock pill */
#define DOCK_PADDING 8     /* Space between icons */

/* Draw a 32x32 bitmap icon scaled to display size */
static void draw_icon(int x, int y, int size, const unsigned char *bitmap,
                      uint32_t fg, uint32_t bg) {
  int clip_x;
  int clip_y;
  int clip_w;
  int clip_h;

  if (!bitmap || size <= 0)
    return;

  if (!gui_target_visible_rect(x, y, size, size, &clip_x, &clip_y, &clip_w,
                               &clip_h))
    return;

  uint32_t *target = gui_draw_target();
  if (!target)
    return;

  int local_x0 = clip_x - g_render_target.origin_x;
  int local_y0 = clip_y - g_render_target.origin_y;
  int start_dx = clip_x - x;
  int start_dy = clip_y - y;
  int pitch = g_render_target.pitch_pixels;

  for (int dy = 0; dy < clip_h; dy++) {
    int src_y = ((start_dy + dy) * 32) / size;
    uint32_t *dst_row = target + (local_y0 + dy) * pitch + local_x0;
    int run_start = 0;
    uint32_t run_color = bitmap[src_y * 32 + ((start_dx * 32) / size)] ? fg : bg;

    for (int dx = 0; dx <= clip_w; dx++) {
      uint32_t color = run_color;

      if (dx < clip_w) {
        int src_x = ((start_dx + dx) * 32) / size;
        color = bitmap[src_y * 32 + src_x] ? fg : bg;
      }

      if (dx == clip_w || color != run_color) {
        int span_w = dx - run_start;
        uint32_t alpha = (run_color >> 24) & 0xFF;

        if (span_w > 0 && alpha != 0) {
          if (alpha == 0xFF) {
            uint32_t solid = run_color & 0xFFFFFF;
            for (int col = 0; col < span_w; col++) {
              dst_row[run_start + col] = solid;
            }
          } else {
            for (int col = 0; col < span_w; col++) {
              int dst_index = run_start + col;
              dst_row[dst_index] =
                  gui_blend_rgb_over(dst_row[dst_index], run_color, alpha);
            }
          }
        }

        run_start = dx;
        run_color = color;
      }
    }
  }
}

/* Draw rounded rectangle helper */
static void draw_rounded_rect(int x, int y, int w, int h, int r,
                              uint32_t color) {
  if (w <= 0 || h <= 0)
    return;
  if (r < 0)
    r = 0;
  if (r * 2 > w)
    r = w / 2;
  if (r * 2 > h)
    r = h / 2;

  if (r == 0) {
    gui_draw_rect(x, y, w, h, color);
    return;
  }

  /* Main body */
  gui_draw_rect(x + r, y, w - 2 * r, h, color);
  gui_draw_rect(x, y + r, r, h - 2 * r, color);
  gui_draw_rect(x + w - r, y + r, r, h - 2 * r, color);

  /* Corners */
  for (int row = 0; row < r; row++) {
    int dy = r - 1 - row;

    for (int span = r; span >= 0; span--) {
      if (span * span + dy * dy <= r * r) {
        int span_w = span * 2 + 1;

        gui_draw_rect(x + r - span, y + row, span_w, 1, color);
        gui_draw_rect(x + w - r - 1 - span, y + row, span_w, 1, color);
        gui_draw_rect(x + r - span, y + h - 1 - row, span_w, 1, color);
        gui_draw_rect(x + w - r - 1 - span, y + h - 1 - row, span_w, 1,
                      color);
        break;
      }
    }
  }
}

/* Draw a filled circle */
static void draw_filled_circle(int cx, int cy, int r, uint32_t color) {
  if (r < 0)
    return;

  for (int y = -r; y <= r; y++) {
    for (int span = r; span >= 0; span--) {
      if (span * span + y * y <= r * r) {
        gui_draw_rect(cx - span, cy + y, span * 2 + 1, 1, color);
        break;
      }
    }
  }
}

/* Draw Terminal icon */
static void draw_icon_terminal(int x, int y, int size) {
  int pad = size / 8;
  int inner_x = x + pad;
  int inner_y = y + pad;
  int inner_w = size - pad * 2;
  int inner_h = size - pad * 2;
  gui_draw_rect(inner_x, inner_y, inner_w, inner_h, 0x10161F);
  gui_draw_rect_outline(inner_x, inner_y, inner_w, inner_h, 0xC9D6E8, 1);
  gui_draw_rect(inner_x, inner_y, inner_w, 3, 0x1F2937);
  gui_draw_rect(inner_x + 4, inner_y + 5, 2, 2, 0xEF4444);
  gui_draw_rect(inner_x + 8, inner_y + 5, 2, 2, 0xF59E0B);
  gui_draw_rect(inner_x + 12, inner_y + 5, 2, 2, 0x22C55E);
  gui_draw_line(x + size / 3, y + size / 2 - size / 8, x + size / 2 - 2,
                y + size / 2, 0x86EFAC);
  gui_draw_line(x + size / 3, y + size / 2 + size / 8, x + size / 2 - 2,
                y + size / 2, 0x86EFAC);
  gui_draw_rect(x + size / 2 + 1, y + size * 2 / 3, size / 5, 2, 0xE5E7EB);
}

/* Draw Files icon */
static void draw_icon_files(int x, int y, int size) {
  int m = size / 6;
  gui_draw_rect(x + m, y + m * 2, size - m * 2, size - m * 3, 0xF8D56A);
  gui_draw_rect(x + m + 1, y + m * 2 + 2, size - m * 2 - 2, size / 4,
                0xFFE59A);
  gui_draw_rect(x + m, y + m, size / 3, m + 3, 0xF6C84F);
  gui_draw_rect_outline(x + m, y + m * 2, size - m * 2, size - m * 3,
                        0xB9851C, 1);
}

/* Draw Calculator icon */
static void draw_icon_calc(int x, int y, int size) {
  int pad = size / 7;
  int cell = size / 6;
  gui_draw_rect(x + pad, y + pad, size - pad * 2, size - pad * 2, 0xF3F4F6);
  gui_draw_rect_outline(x + pad, y + pad, size - pad * 2, size - pad * 2,
                        0x9CA3AF, 1);
  gui_draw_rect(x + pad + 3, y + pad + 3, size - pad * 2 - 6, cell + 4,
                0x1F2937);
  gui_draw_rect(x + pad + 6, y + pad + cell + 11, cell, cell, 0xCBD5E1);
  gui_draw_rect(x + pad + 6 + cell + 3, y + pad + cell + 11, cell, cell,
                0xCBD5E1);
  gui_draw_rect(x + pad + 6, y + pad + cell * 2 + 14, cell, cell, 0xCBD5E1);
  gui_draw_rect(x + pad + 6 + cell + 3, y + pad + cell * 2 + 14, cell, cell,
                0xF59E0B);
  gui_draw_rect(x + pad + size / 2 - 4, y + size / 2 - 3, size / 3, 3,
                0x111827);
  gui_draw_rect(x + pad + size / 2 - 4, y + size / 2 + 4, size / 3, 3,
                0x111827);
}

/* Draw Notes icon */
static void draw_icon_notes(int x, int y, int size) {
  int m = size / 6;
  gui_draw_rect(x + m, y + m / 2, size - m * 2, size - m, 0xFFF8D8);
  gui_draw_rect_outline(x + m, y + m / 2, size - m * 2, size - m, 0xD1B74C, 1);
  gui_draw_rect(x + size - m * 2 - 2, y + m / 2, m + 2, m + 2, 0xFDE68A);
  gui_draw_line(x + size - m * 2 - 2, y + m / 2 + m + 2, x + size - m, y + m / 2,
                0xD1B74C);
  for (int i = 0; i < 4; i++) {
    gui_draw_rect(x + m * 2, y + m * 2 + i * (m + 1), size - m * 4, 2,
                  0x9CA3AF);
  }
}

/* Draw Settings icon */
static void draw_icon_settings(int x, int y, int size) {
  int cx = x + size / 2;
  int cy = y + size / 2;
  int r = size / 4;
  int tooth = size / 10;
  draw_filled_circle(cx, cy, r + 3, 0xE5E7EB);
  for (int i = 0; i < 8; i++) {
    int tx = cx;
    int ty = cy;
    if (i == 0) ty -= r + tooth;
    if (i == 1) { tx += r / 2 + tooth / 2; ty -= r / 2 + tooth / 2; }
    if (i == 2) tx += r + tooth;
    if (i == 3) { tx += r / 2 + tooth / 2; ty += r / 2 + tooth / 2; }
    if (i == 4) ty += r + tooth;
    if (i == 5) { tx -= r / 2 + tooth / 2; ty += r / 2 + tooth / 2; }
    if (i == 6) tx -= r + tooth;
    if (i == 7) { tx -= r / 2 + tooth / 2; ty -= r / 2 + tooth / 2; }
    draw_filled_circle(tx, ty, tooth / 2 + 1, 0xD1D5DB);
  }
  draw_filled_circle(cx, cy, r - 1, 0x9CA3AF);
  draw_filled_circle(cx, cy, r / 2, 0xF8FAFC);
}

/* Draw Clock icon */
static void draw_icon_clock(int x, int y, int size) {
  int cx = x + size / 2;
  int cy = y + size / 2;
  int r = size / 3;
  draw_filled_circle(cx, cy, r + 2, 0xE2E8F0);
  draw_filled_circle(cx, cy, r, 0xFFFFFF);
  gui_draw_line(cx, cy, cx, cy - r + 4, 0x111827);
  gui_draw_line(cx, cy, cx + r / 2, cy + r / 4, 0x475569);
  draw_filled_circle(cx, cy, 2, 0xEF4444);
}

/* Draw Bowling icon */
static void draw_icon_bowling(int x, int y, int size) {
  int ball_r = size / 5;
  int pin_w = size / 8;
  int pin_h = size / 3;
  draw_filled_circle(x + size / 3, y + size * 2 / 3, ball_r + 1, 0xDC2626);
  draw_filled_circle(x + size / 3, y + size * 2 / 3, ball_r, 0xEF4444);
  draw_filled_circle(x + size / 3 - 3, y + size * 2 / 3 - 2, 1, 0x991B1B);
  draw_filled_circle(x + size / 3 + 2, y + size * 2 / 3 - 4, 1, 0x991B1B);
  draw_filled_circle(x + size / 3 + 4, y + size * 2 / 3 + 1, 1, 0x991B1B);
  gui_draw_rect(x + size * 2 / 3 - pin_w / 2, y + size / 4, pin_w, pin_h,
                0xFFFFFF);
  gui_draw_rect(x + size * 2 / 3 - pin_w / 2 - 2, y + size / 4 + pin_h - 4,
                pin_w + 4, 4, 0xFFFFFF);
  gui_draw_rect(x + size * 2 / 3 - pin_w / 2 - 1, y + size / 4 + 5, pin_w + 2,
                3, 0x2563EB);
}

/* Draw Help icon */
static void draw_icon_help(int x, int y, int size) {
  int cx = x + size / 2;
  int cy = y + size / 2;
  int r = size / 3;
  draw_filled_circle(cx, cy, r + 2, 0xDBEAFE);
  draw_filled_circle(cx, cy, r, 0xFFFFFF);
  gui_draw_rect(cx - 2, cy - r / 2, 4, r / 2, 0x2563EB);
  gui_draw_rect(cx - 1, cy - r / 2 - 3, r / 2 + 1, 3, 0x2563EB);
  draw_filled_circle(cx, cy + r / 2 - 1, 2, 0x2563EB);
}

/* Draw Browser/App Store icon */
static void draw_icon_web(int x, int y, int size) {
  int cx = x + size / 2;
  int cy = y + size / 2;
  int r = size / 3;
  draw_filled_circle(cx, cy, r + 2, 0xDBEAFE);
  draw_filled_circle(cx, cy, r, 0xF8FAFC);
  gui_draw_line(cx - r + 4, cy, cx + r - 4, cy, 0x0EA5E9);
  gui_draw_line(cx, cy - r + 4, cx, cy + r - 4, 0x0EA5E9);
  gui_draw_line(cx - r / 2, cy - r + 5, cx + r / 2, cy + r - 5, 0x38BDF8);
}

static void draw_icon_appstore(int x, int y, int size) {
  int bag_x = x + size / 5;
  int bag_y = y + size / 4;
  int bag_w = size - size * 2 / 5;
  int bag_h = size - size / 3;
  draw_filled_circle(x + size / 2, y + size / 2, size / 3 + 2, 0xEDE9FE);
  gui_draw_rect(bag_x, bag_y, bag_w, bag_h, 0x8B5CF6);
  gui_draw_rect_outline(bag_x, bag_y, bag_w, bag_h, 0xC4B5FD, 1);
  gui_draw_line(x + size / 2 - size / 8, bag_y, x + size / 2 + size / 8, bag_y,
                0xE9D5FF);
  gui_draw_line(x + size / 2 - size / 8, bag_y, x + size / 2 - size / 10,
                bag_y - size / 10, 0xE9D5FF);
  gui_draw_line(x + size / 2 + size / 8, bag_y, x + size / 2 + size / 10,
                bag_y - size / 10, 0xE9D5FF);
  gui_draw_line(x + size / 2 - size / 10, y + size / 2, x + size / 2 + size / 10,
                y + size / 2, 0xFFFFFF);
  gui_draw_line(x + size / 2, y + size / 2 - size / 10, x + size / 2,
                y + size / 2 + size / 10, 0xFFFFFF);
}

static uint32_t gui_lerp_rgb(uint32_t top, uint32_t bottom, int step,
                             int steps) {
  uint32_t top_r = (top >> 16) & 0xFF;
  uint32_t top_g = (top >> 8) & 0xFF;
  uint32_t top_b = top & 0xFF;
  uint32_t bottom_r = (bottom >> 16) & 0xFF;
  uint32_t bottom_g = (bottom >> 8) & 0xFF;
  uint32_t bottom_b = bottom & 0xFF;

  if (steps <= 1)
    return top;

  return (((top_r + ((int)(bottom_r - top_r) * step) / (steps - 1)) & 0xFF)
          << 16) |
         (((top_g + ((int)(bottom_g - top_g) * step) / (steps - 1)) & 0xFF)
          << 8) |
         ((top_b + ((int)(bottom_b - top_b) * step) / (steps - 1)) & 0xFF);
}

static void gui_draw_vertical_gradient(int x, int y, int w, int h,
                                       uint32_t top, uint32_t bottom) {
  for (int row = 0; row < h; row++)
    gui_draw_rect(x, y + row, w, 1, gui_lerp_rgb(top, bottom, row, h));
}

static int gui_path_uses_system_folder_icon(const char *path) {
  if (!path || !path[0])
    return 0;
  return path_starts_with(path, GUI_SYSTEM_DIR) ||
         path_starts_with(path, GUI_SYSTEM_APPS_DIR) ||
         path_starts_with(path, GUI_APPS_DIR) ||
         str_cmp(path, GUI_SYSTEM_APPS_FOLDER) == 0;
}

int gui_draw_folder_icon(const char *path, int x, int y, int size) {
  int w;
  int body_h;
  int body_x;
  int body_y;
  int tab_x;
  int tab_y;
  int tab_w;
  int tab_h;
  int front_h;
  int front_y;
  int accent_pad;
  int system_variant;

  if (size <= 0)
    return -1;

  system_variant = gui_path_uses_system_folder_icon(path);
  w = size;
  body_h = (size * 34) / 48;
  if (body_h < size / 2)
    body_h = size / 2;

  body_x = x + size / 12;
  body_y = y + size / 4;
  tab_x = body_x;
  tab_y = y + size / 10;
  tab_w = size / 3;
  tab_h = size / 6;
  front_h = body_h / 3;
  front_y = body_y + body_h - front_h;
  accent_pad = size / 12;

  if (tab_w > 2 && tab_h > 2) {
    gui_draw_vertical_gradient(tab_x + 1, tab_y + 1, tab_w - 2, tab_h - 1,
                               0xBDC73D, 0x929929);
    gui_draw_rect_outline(tab_x, tab_y, tab_w, tab_h, 0x000000, 1);
  }

  if (w - size / 6 > 2 && body_h > 2) {
    gui_draw_vertical_gradient(body_x + 1, body_y + 1, w - size / 6 - 2,
                               body_h - 2, 0xD0D67F, 0xBBC263);
    gui_draw_rect_outline(body_x, body_y, w - size / 6, body_h, 0x000000, 1);
  }

  if (w - size / 6 > 2 && front_h > 1) {
    gui_draw_vertical_gradient(body_x + 1, front_y, w - size / 6 - 2,
                               front_h - 1, 0xD0D67F, 0xA4AA53);
    gui_draw_rect(body_x + 1, front_y, w - size / 6 - 2, 1, 0x000000);
  }

  if (system_variant) {
    int grid_x = body_x + accent_pad + 1;
    int grid_y = body_y + accent_pad + 1;
    int grid_w = (w - size / 6) - accent_pad * 2 - 2;
    int grid_h = body_h - front_h - accent_pad - 2;
    int half_w = grid_w / 2;
    int half_h = grid_h / 2;

    if (grid_w > 8 && grid_h > 8) {
      gui_draw_rect(grid_x, grid_y, half_w - 1, half_h - 1, 0xFFF500);
      gui_draw_rect(grid_x + half_w + 1, grid_y, grid_w - half_w - 1,
                    half_h - 1, 0xFF2C00);
      gui_draw_rect(grid_x, grid_y + half_h + 1, half_w - 1,
                    grid_h - half_h - 1, 0x00FF37);
      gui_draw_rect(grid_x + half_w + 1, grid_y + half_h + 1,
                    grid_w - half_w - 1, grid_h - half_h - 1, 0x003CFF);
      gui_draw_rect(grid_x + half_w - 1, grid_y, 2, grid_h, 0x000000);
      gui_draw_rect(grid_x, grid_y + half_h - 1, grid_w, 2, 0x000000);
    }
  } else {
    int pocket_x = body_x + size / 7;
    int pocket_y = body_y + body_h / 3;
    int pocket_w = w - size / 6 - (size / 4);
    int pocket_h = body_h / 4;

    if (pocket_w > 4 && pocket_h > 3) {
      gui_draw_vertical_gradient(pocket_x, pocket_y, pocket_w, pocket_h,
                                 0xE3E89C, 0xB7BF5C);
      gui_draw_rect_outline(pocket_x, pocket_y, pocket_w, pocket_h, 0x000000,
                            1);
    }
  }

  return 0;
}

static void draw_system_app_icon_kind(gui_app_kind_t kind, int x, int y,
                                      int size) {
  switch (kind) {
  case GUI_APP_TERMINAL:
    draw_icon_terminal(x, y, size);
    break;
  case GUI_APP_FILES:
    gui_draw_folder_icon(NULL, x, y, size);
    break;
  case GUI_APP_CALCULATOR:
    draw_icon_calc(x, y, size);
    break;
  case GUI_APP_NOTES:
    draw_icon_notes(x, y, size);
    break;
  case GUI_APP_SETTINGS:
    draw_icon_settings(x, y, size);
    break;
  case GUI_APP_CLOCK:
    draw_icon_clock(x, y, size);
    break;
  case GUI_APP_SNAKE:
    draw_icon_bowling(x, y, size);
    break;
  case GUI_APP_HELP:
    draw_icon_help(x, y, size);
    break;
  case GUI_APP_BROWSER:
    draw_icon_web(x, y, size);
    break;
  case GUI_APP_APPSTORE:
    draw_icon_appstore(x, y, size);
    break;
  case GUI_APP_DISK_IMAGER:
    draw_icon_settings(x, y, size);
    break;
  }
}

int gui_draw_system_app_icon(const char *app_id, int x, int y, int size) {
  const dock_app_def_t *app = find_catalog_app(app_id);
  if (!app)
    return -1;
  draw_system_app_icon_kind(app->kind, x, y, size);
  return 0;
}

/* Draw dock with fixed icon sizes and a top-rounded background */
static void draw_dock_status_indicators(int dock_y, int dock_h) {
  char time_str[9];
  int hours24, minutes, seconds;
  int panel_x, panel_y, panel_w, panel_h;
  int wifi_x, wifi_y, wifi_w, wifi_h;
  int wx;
  int wy;
  uint32_t wifi_color;

  dock_status_panel_rect(dock_y, dock_h, &panel_x, &panel_y, &panel_w, &panel_h);
  wifi_tray_button_rect(dock_y, dock_h, &wifi_x, &wifi_y, &wifi_w, &wifi_h);
  wx = panel_x + 26;
  wy = panel_y + 16;
  clock_get_time(&hours24, &minutes, &seconds);
  clock_format_time(time_str, hours24, minutes, seconds);
  time_str[5] = '\0';
  wifi_color = wifi_has_supported_adapter()
                   ? (wifi_is_connected() ? 0xA6E3A1 : 0xF9E2AF)
                   : 0xF38BA8;

  gui_fill_rect_alpha(panel_x, panel_y, panel_w, panel_h, 0x28405268);
  gui_draw_rect_outline(panel_x, panel_y, panel_w, panel_h, 0x50738BA3, 1);
  gui_fill_rect_alpha(panel_x, panel_y, panel_w, 1, 0x46FFFFFF);
  gui_fill_rect_alpha(wifi_x, wifi_y, wifi_w, wifi_h,
                      wifi_tray_open ? 0x406285A8 : 0x1E0F172A);

  /* WiFi status */
  gui_draw_rect(wx, wy + 6, 2, 2, wifi_color);
  gui_draw_line(wx - 3, wy + 3, wx, wy, wifi_color);
  gui_draw_line(wx, wy, wx + 3, wy + 3, wifi_color);
  gui_draw_line(wx - 6, wy, wx, wy - 3, wifi_color);
  gui_draw_line(wx, wy - 3, wx + 6, wy, wifi_color);
  draw_filled_circle(panel_x + 38, panel_y + panel_h / 2, 3, wifi_color);

  gui_draw_string(panel_x + 50, panel_y + 9, time_str, 0xFFFFFF, 0x00000000);
}

static void draw_wifi_tray_panel(int dock_y, int dock_h) {
  int panel_x, panel_y, panel_w, panel_h;
  int button_y;
  int network_count;

  if (!wifi_tray_open)
    return;

  wifi_tray_panel_rect(dock_y, dock_h, &panel_x, &panel_y, &panel_w, &panel_h);
  button_y = panel_y + 58;
  network_count = wifi_get_network_count();

  gui_fill_rect_alpha(panel_x + 8, panel_y + 8, panel_w, panel_h, 0x28050910);
  gui_draw_glass_panel(panel_x, panel_y, panel_w, panel_h, 0x74354962,
                       0x20FFFFFF, 0x8A7A8FA7, 1);
  gui_draw_string(panel_x + 14, panel_y + 12, "Wi-Fi", 0xFFFFFF, 0x00000000);
  gui_draw_string(panel_x + 14, panel_y + 30,
                  wifi_is_intel_adapter() ? "Intel wireless"
                                          : "Wireless networking",
                  0xBAC8D7, 0x00000000);
  gui_draw_string(panel_x + 14, panel_y + 44, wifi_get_adapter_name(),
                  wifi_has_supported_adapter() ? 0xEAF2FF : 0xF38BA8,
                  0x00000000);

  gui_draw_system_button(panel_x + 14, button_y, 70, 24, "Scan",
                         GUI_BUTTON_PRIMARY,
                         wifi_has_supported_adapter() &&
                             wifi_supports_real_scanning(),
                         0);
  gui_draw_system_button(panel_x + 92, button_y, 72, 24, "Connect",
                         GUI_BUTTON_SUCCESS, wifi_can_connect_selected(), 0);
  gui_draw_system_button(panel_x + 172, button_y, 62, 24, "Off",
                         GUI_BUTTON_DANGER, wifi_is_connected(), 0);

  gui_draw_string(panel_x + 14, panel_y + 84, wifi_get_status_text(),
                  0xE2E8F0, 0x00000000);

  if (wifi_get_selected_network() >= 0 &&
      wifi_get_network_secure(wifi_get_selected_network())) {
    char masked_wifi_password[32];
    uint32_t field_bg = wifi_tray_password_active ? 0x3048667D : 0x1F2937;
    mask_secret(wifi_password_draft, masked_wifi_password,
                sizeof(masked_wifi_password));
    gui_fill_rect_alpha(panel_x + 14, panel_y + 102, panel_w - 28, 24, field_bg);
    gui_draw_string(panel_x + 20, panel_y + 109,
                    masked_wifi_password[0] ? masked_wifi_password
                                            : "Wi-Fi password",
                    masked_wifi_password[0] ? 0xFFFFFF : 0x94A3B8, 0x00000000);
  }

  if (!wifi_has_supported_adapter()) {
    gui_draw_string(panel_x + 14, panel_y + 134,
                    "No supported Intel or compatibility Wi-Fi card detected.",
                    0xCBD5E1, 0x00000000);
    gui_draw_string(panel_x + 14, panel_y + 152,
                    "The tray menu is ready for supported hardware.",
                    0xCBD5E1, 0x00000000);
    return;
  }

  if (!wifi_supports_real_scanning()) {
    gui_draw_string(panel_x + 14, panel_y + 134,
                    "Adapter found, but no real scan backend is loaded yet.",
                    0xCBD5E1, 0x00000000);
    gui_draw_string(panel_x + 14, panel_y + 152,
                    "The tray no longer invents nearby Wi-Fi networks.",
                    0xCBD5E1, 0x00000000);
    return;
  }

  if (network_count <= 0) {
    gui_draw_string(panel_x + 14, panel_y + 134,
                    "No nearby networks were reported by the driver.",
                    0xCBD5E1, 0x00000000);
    gui_draw_string(panel_x + 14, panel_y + 152,
                    "Connect unlocks after the scan backend returns entries.",
                    0xCBD5E1, 0x00000000);
    return;
  }

  for (int i = 0; i < network_count && i < 3; i++) {
    int row_y = panel_y + 134 + i * 24;
    uint32_t fg = i == wifi_get_selected_network() ? 0xFFFFFF : 0xD7E3F2;
    uint32_t accent = wifi_is_network_connected(i)
                          ? 0x89B4FA
                          : (wifi_get_network_secure(i) ? 0xA6E3A1 : 0xF9E2AF);

    if (i == wifi_get_selected_network())
      gui_fill_rect_alpha(panel_x + 10, row_y - 2, panel_w - 20, 20, 0x3048667D);
    gui_draw_string(panel_x + 14, row_y, wifi_get_network_ssid(i), fg,
                    0x00000000);
    gui_draw_string(panel_x + 128, row_y,
                    wifi_is_network_connected(i)
                        ? "Connected"
                        : (wifi_get_network_secure(i) ? "Secured" : "Open"),
                    accent,
                    0x00000000);

    {
      char signal_buf[24] = "";
      append_uint_to_buf(signal_buf, sizeof(signal_buf),
                         wifi_get_network_signal(i));
      notepad_append_to_buf(signal_buf, sizeof(signal_buf), "%");
      gui_draw_string(panel_x + 196, row_y, signal_buf, 0xCBD5E1, 0x00000000);
    }
  }
}

static void draw_dock(void) {
  const gui_theme_palette_t *theme = gui_theme_palette();
  if (!dock_is_visible())
    return;
  if (!gui_is_installer_mode()) {
    load_dock_config();
  }
  if (dock_item_count <= 0)
    return;

  int icon_sizes[MAX_DOCK_ITEMS];
  int dock_h = DOCK_HEIGHT;
  int dock_x = 0;
  int dock_y = (int)primary_display.height - dock_h;
  int launcher_btn_x;
  int launcher_btn_y;
  int launcher_btn_w;
  int launcher_btn_h;
  int icon_start_x;
  int hovered_idx = -1;
  int hovered_launcher = 0;

  main_menu_launcher_button_rect(&launcher_btn_x, &launcher_btn_y,
                                 &launcher_btn_w, &launcher_btn_h);
  icon_start_x = launcher_btn_x + launcher_btn_w + 18;

  for (int i = 0; i < dock_item_count; i++) {
    int base_center_x = icon_start_x + i * (DOCK_ICON_SIZE + DOCK_PADDING) +
                        DOCK_ICON_SIZE / 2;
    int base_icon_x = base_center_x - DOCK_ICON_SIZE / 2;
    int base_icon_y = dock_y + (dock_h - DOCK_ICON_SIZE) / 2;

    icon_sizes[i] = DOCK_ICON_SIZE;
    if (mouse_x >= base_icon_x && mouse_x < base_icon_x + DOCK_ICON_SIZE &&
        mouse_y >= base_icon_y && mouse_y < base_icon_y + DOCK_ICON_SIZE) {
      hovered_idx = i;
    }
  }

  gui_fill_rect_alpha(dock_x, dock_y, primary_display.width, dock_h,
                      theme->dock_fill);
  gui_fill_rect_alpha(dock_x, dock_y, primary_display.width, 1,
                      theme->dock_top_line);
  gui_fill_rect_alpha(dock_x, dock_y + 1, primary_display.width, 1,
                      theme->dock_top_shadow);
  gui_fill_rect_alpha(dock_x, dock_y + dock_h - 1, primary_display.width, 1,
                      theme->dock_bottom_line);

  draw_main_menu_panel();

  if (mouse_x >= launcher_btn_x && mouse_x < launcher_btn_x + launcher_btn_w &&
      mouse_y >= launcher_btn_y && mouse_y < launcher_btn_y + launcher_btn_h) {
    hovered_launcher = 1;
  }

  gui_fill_rect_alpha(launcher_btn_x, launcher_btn_y, launcher_btn_w,
                      launcher_btn_h,
                      menu_open ? theme->dock_launcher_active
                                : (hovered_launcher ? theme->dock_launcher_hover
                                                    : theme->dock_launcher));
  gui_draw_rect_outline(launcher_btn_x, launcher_btn_y, launcher_btn_w,
                        launcher_btn_h,
                        menu_open ? theme->title_line_top
                                  : theme->dock_launcher_border,
                        1);
  if (gui_draw_embedded_logo(launcher_btn_x + 8, launcher_btn_y + 7, 28, 28) != 0) {
    gui_draw_os_logo(launcher_btn_x + 10, launcher_btn_y + 9, 2, 0xFFFFFF,
                     0x89B4FA, 0x00000000);
  }

  draw_dock_status_indicators(dock_y, dock_h);
  draw_wifi_tray_panel(dock_y, dock_h);

  int center_y = dock_y + dock_h / 2;
  int curr_x = icon_start_x;
  int render_centers[MAX_DOCK_ITEMS];
  int running_counts[MAX_DOCK_ITEMS];
  for (int i = 0; i < dock_item_count; i++) {
    render_centers[i] = curr_x + icon_sizes[i] / 2;
    running_counts[i] = count_windows_for_app_kind(dock_items[i]->kind);
    curr_x += icon_sizes[i] + DOCK_PADDING;
  }

  for (int i = 0; i < dock_item_count; i++) {
    int size = icon_sizes[i];
    int cx = render_centers[i];
    int cy = center_y;
    int draw_x = cx - size / 2;
    int draw_y = cy - size / 2;
    int icon_r = size / 5;
    uint32_t bg_color = dock_items[i]->icon_color;

    draw_rounded_rect(draw_x, draw_y, size, size, icon_r, bg_color);

    gui_draw_rect(draw_x + icon_r, draw_y + 2, size - 2 * icon_r, 1,
                  bg_color + 0x202020);
    gui_draw_rect(draw_x + icon_r, draw_y + 3, size - 2 * icon_r, 1,
                  bg_color + 0x202020);

    draw_system_app_icon_kind(dock_items[i]->kind, draw_x + size / 8,
                              draw_y + size / 8, size * 3 / 4);

    if (running_counts[i] > 0) {
      int dots = running_counts[i] > 3 ? 3 : running_counts[i];
      int start_x = draw_x + size / 2 - ((dots * 6) - 2) / 2;
      int dot_y = draw_y + size + 6;

      for (int dot = 0; dot < dots; dot++) {
        draw_filled_circle(start_x + dot * 6, dot_y, 2,
                           i == hovered_idx ? 0xFFFFFF : 0xC7D2FE);
      }
    }
  }

  if (hovered_idx >= 0) {
    char label_buf[80];
    const char *label = dock_items[hovered_idx]->label;
    int idx_x = render_centers[hovered_idx];
    int label_len = 0;
    int running = running_counts[hovered_idx];
    int out = 0;

    while (label[out] && out < (int)sizeof(label_buf) - 1) {
      label_buf[out] = label[out];
      out++;
    }
    if (running > 0 && out < (int)sizeof(label_buf) - 10) {
      label_buf[out++] = ' ';
      label_buf[out++] = '(';
      if (running >= 10) {
        label_buf[out++] = (char)('0' + ((running / 10) % 10));
      }
      label_buf[out++] = (char)('0' + (running % 10));
      label_buf[out++] = ')';
    }
    label_buf[out] = '\0';
    label = label_buf;

    while (label[label_len])
      label_len++;
    int label_w = label_len * 8 + 16;
    int label_h = 24;
    int label_x = idx_x - label_w / 2;
    int label_y = dock_y - 32;

    draw_rounded_rect(label_x, label_y, label_w, label_h, 6, 0x303040);
    gui_draw_rect_outline(label_x, label_y, label_w, label_h, 0x505060, 1);
    gui_draw_string(label_x + 8, label_y + 4, label, 0xFFFFFF, 0x303040);

    {
      int tri_x = label_x + label_w / 2;
      int tri_y = label_y + label_h;
      for (int i = 0; i < 4; i++) {
        gui_draw_rect(tri_x - i, tri_y + i, i * 2 + 1, 1, 0x303040);
      }
    }
  }

  if (hovered_launcher) {
    int label_w = 88;
    int label_h = 24;
    int label_x = launcher_btn_x;
    int label_y = dock_y - 32;

    draw_rounded_rect(label_x, label_y, label_w, label_h, 6, 0x303040);
    gui_draw_rect_outline(label_x, label_y, label_w, label_h, 0x505060, 1);
    gui_draw_string(label_x + 8, label_y + 4, "Main Menu", 0xFFFFFF, 0x303040);
  }
}

/* Cached wallpaper for performance - gradient is expensive to recalculate */
static uint32_t *cached_wallpaper = NULL;
static int wallpaper_cached = 0;
static int wallpaper_cached_idx = -1; /* Which wallpaper is cached */
static int cached_wallpaper_w = 0;
static int cached_wallpaper_h = 0;
static uint32_t *cached_desktop_surface = NULL;
static int cached_desktop_surface_w = 0;
static int cached_desktop_surface_h = 0;
static int cached_desktop_surface_valid = 0;

static int ensure_wallpaper_cache_size(int width, int height) {
  size_t pixel_count;
  size_t bytes;
  uint32_t *new_cache;

  if (width <= 0 || height <= 0)
    return -1;
  if (cached_wallpaper && cached_wallpaper_w == width &&
      cached_wallpaper_h == height)
    return 0;

  pixel_count = (size_t)width * (size_t)height;
  bytes = pixel_count * sizeof(uint32_t);
  new_cache = (uint32_t *)kmalloc(bytes);
  if (!new_cache)
    return -1;

  if (cached_wallpaper)
    kfree(cached_wallpaper);
  cached_wallpaper = new_cache;
  cached_wallpaper_w = width;
  cached_wallpaper_h = height;
  wallpaper_cached = 0;
  return 0;
}

static void render_wallpaper_cache(void) {
  int height = (int)primary_display.height - MENU_BAR_HEIGHT;
  int width = (int)primary_display.width;

  if (width <= 0 || height <= 0)
    return;

  if (wallpaper_cached_idx != current_wallpaper) {
    wallpaper_cached = 0;
    wallpaper_cached_idx = current_wallpaper;
    wallpaper_ensure_loaded();
  }

  if (ensure_wallpaper_cache_size(width, height) != 0 || !cached_wallpaper)
    return;
  if (wallpaper_cached)
    return;

  if (wallpapers[current_wallpaper].type == 1 && wallpaper_image.pixels) {
    int *src_x_map = NULL;
    int fit_stretch = wallpapers[current_wallpaper].fit_mode == WALLPAPER_FIT_STRETCH;
    uint32_t img_w = wallpaper_image.width;
    uint32_t img_h = wallpaper_image.height;
    uint32_t *pixels = wallpaper_image.pixels;

    if (img_w == 0 || img_h == 0)
      return;

    src_x_map = (int *)kmalloc((size_t)width * sizeof(int));
    if (src_x_map) {
      for (int x = 0; x < width; x++) {
        src_x_map[x] = fit_stretch
                           ? wallpaper_stretch_coord(x, width, (int)img_w)
                           : wallpaper_cover_coord(x, width, (int)img_w, width,
                                                   height, (int)img_w,
                                                   (int)img_h);
      }
    }

    for (int y = 0; y < height; y++) {
      uint32_t *line = cached_wallpaper + y * width;
      int src_y = fit_stretch
                      ? wallpaper_stretch_coord(y, height, (int)img_h)
                      : wallpaper_cover_coord(y, height, (int)img_h, width,
                                              height, (int)img_w, (int)img_h);
      uint32_t *src_row = pixels + src_y * img_w;

      for (int x = 0; x < width; x++) {
        int src_x = src_x_map
                        ? src_x_map[x]
                        : (fit_stretch
                               ? wallpaper_stretch_coord(x, width, (int)img_w)
                               : wallpaper_cover_coord(x, width, (int)img_w,
                                                       width, height,
                                                       (int)img_w, (int)img_h));
        line[x] = src_row[src_x];
      }
    }

    if (src_x_map)
      kfree(src_x_map);
  } else {
    for (int y = 0; y < height; y++) {
      uint32_t *line = cached_wallpaper + y * width;
      uint32_t color = wallpaper_get_pixel(0, y, height);
      fast_fill_line(line, width, color);
    }
  }

  wallpaper_cached = 1;
}

static void invalidate_desktop_surface(void) { cached_desktop_surface_valid = 0; }

static int ensure_desktop_surface_size(int width, int height) {
  uint32_t *new_surface;

  if (width <= 0 || height <= 0)
    return -1;
  if (cached_desktop_surface && cached_desktop_surface_w == width &&
      cached_desktop_surface_h == height)
    return 0;

  new_surface = kmalloc((size_t)width * (size_t)height * sizeof(uint32_t));
  if (!new_surface)
    return -ENOMEM;

  if (cached_desktop_surface)
    kfree(cached_desktop_surface);
  cached_desktop_surface = new_surface;
  cached_desktop_surface_w = width;
  cached_desktop_surface_h = height;
  cached_desktop_surface_valid = 0;
  return 0;
}

static int gui_target_visible_rect(int src_x, int src_y, int src_w, int src_h,
                                   int *clip_x, int *clip_y, int *clip_w,
                                   int *clip_h) {
  int x0 = src_x;
  int y0 = src_y;
  int x1 = src_x + src_w;
  int y1 = src_y + src_h;
  int target_x0 = g_render_target.origin_x;
  int target_y0 = g_render_target.origin_y;
  int target_x1 = g_render_target.origin_x + g_render_target.width;
  int target_y1 = g_render_target.origin_y + g_render_target.height;

  if (src_w <= 0 || src_h <= 0 || !g_render_target.pixels)
    return 0;

  if (x0 < target_x0)
    x0 = target_x0;
  if (y0 < target_y0)
    y0 = target_y0;
  if (x1 > target_x1)
    x1 = target_x1;
  if (y1 > target_y1)
    y1 = target_y1;

  if (g_clip.enabled) {
    if (x0 < g_clip.x0)
      x0 = g_clip.x0;
    if (y0 < g_clip.y0)
      y0 = g_clip.y0;
    if (x1 > g_clip.x1)
      x1 = g_clip.x1;
    if (y1 > g_clip.y1)
      y1 = g_clip.y1;
  }

  if (x0 >= x1 || y0 >= y1)
    return 0;

  if (clip_x)
    *clip_x = x0;
  if (clip_y)
    *clip_y = y0;
  if (clip_w)
    *clip_w = x1 - x0;
  if (clip_h)
    *clip_h = y1 - y0;
  return 1;
}

/* Draw wallpaper - supports both gradients and JPEG images */
static void draw_wallpaper(void) {
  int start_y = MENU_BAR_HEIGHT;
  /* Extend wallpaper all the way to bottom of screen (dock drawn on top) */
  int end_y = primary_display.height;
  int height = end_y - start_y;
  int width = primary_display.width;
  uint32_t *target = gui_draw_target();
  int clip_x;
  int clip_y;
  int clip_w;
  int clip_h;

  if (!target)
    return;
  render_wallpaper_cache();
  if (!cached_wallpaper || !wallpaper_cached || cached_wallpaper_w != width ||
      cached_wallpaper_h != height)
    return;
  if (!gui_target_visible_rect(0, start_y, width, height, &clip_x, &clip_y,
                               &clip_w, &clip_h))
    return;

  {
    int src_x = clip_x;
    int src_y0 = clip_y - start_y;
    int dst_x = clip_x - g_render_target.origin_x;
    int dst_y0 = clip_y - g_render_target.origin_y;

    for (int row = 0; row < clip_h; row++) {
      uint32_t *dst =
          target + (dst_y0 + row) * g_render_target.pitch_pixels + dst_x;
      uint32_t *src = cached_wallpaper + (src_y0 + row) * width + src_x;
      fast_memcpy_line(dst, src, clip_w);
    }
  }
}

static void draw_desktop_internal(void) {
  /* Draw beautiful gradient wallpaper */
  draw_wallpaper();

  /* Draw desktop icons */
  if (desktop_session_active()) {
    if (g_clip.enabled) {
      desktop_draw_icons_region(g_clip.x0, g_clip.y0, g_clip.x1 - g_clip.x0,
                                g_clip.y1 - g_clip.y0);
    } else {
      desktop_draw_icons();
    }
  }

  /* Draw build info in the bottom-right corner above the dock. */
  {
#ifdef ARCH_X86_64
    const char *build_info = "OS 8 x86_64 - TESTING ONLY";
#elif defined(ARCH_X86)
    const char *build_info = "OS 8 x86 - TESTING ONLY";
#else
    const char *build_info = "OS 8 ARM64 - TESTING ONLY";
#endif
    int build_len = 0;
    int uuid_len = 0;
    while (build_info[build_len]) {
      build_len++;
    }
    while (BUILD_UUID[uuid_len]) {
      uuid_len++;
    }

    int text_w = build_len > uuid_len ? build_len * 8 : uuid_len * 8;
    int text_x = (int)primary_display.width - text_w - 24;
    int text_y =
        (int)primary_display.height - dock_reserved_height() - 40;

    if (text_x < 12)
      text_x = 12;

    gui_draw_string(text_x, text_y, build_info, 0xD9E4F4, 0x00000000);
    gui_draw_string(text_x, text_y + 16, BUILD_NUMBER, 0xAEB9CB, 0x00000000);
  }
}

static void refresh_desktop_surface_if_needed(void) {
  struct gui_render_target prev_target;
  struct gui_clip_state prev_clip;
  int dirty_x;
  int dirty_y;
  int dirty_w;
  int dirty_h;

  if (ensure_desktop_surface_size((int)primary_display.width,
                                  (int)primary_display.height) != 0 ||
      !cached_desktop_surface)
    return;

  if (!cached_desktop_surface_valid) {
    desktop_mark_full_redraw();
  }

  if (!desktop_needs_redraw() && cached_desktop_surface_valid)
    return;

  if (!desktop_get_dirty_bounds(&dirty_x, &dirty_y, &dirty_w, &dirty_h)) {
    dirty_x = 0;
    dirty_y = 0;
    dirty_w = (int)primary_display.width;
    dirty_h = (int)primary_display.height;
  }

  prev_target =
      gui_set_render_target(cached_desktop_surface, cached_desktop_surface_w,
                            cached_desktop_surface_h, cached_desktop_surface_w,
                            0, 0);
  prev_clip = gui_set_clip_rect(dirty_x, dirty_y, dirty_w, dirty_h);
  draw_desktop_internal();
  gui_restore_clip_rect(prev_clip);
  g_render_target = prev_target;

  cached_desktop_surface_valid = 1;
  desktop_clear_dirty();
}

static void draw_desktop(void) {
  uint32_t *target;
  int clip_x;
  int clip_y;
  int clip_w;
  int clip_h;

  refresh_desktop_surface_if_needed();
  if (!cached_desktop_surface || !cached_desktop_surface_valid) {
    draw_desktop_internal();
    return;
  }

  target = gui_draw_target();
  if (!target)
    return;
  if (!gui_target_visible_rect(0, 0, cached_desktop_surface_w,
                               cached_desktop_surface_h, &clip_x, &clip_y,
                               &clip_w, &clip_h))
    return;

  {
    int dst_x = clip_x - g_render_target.origin_x;
    int dst_y0 = clip_y - g_render_target.origin_y;
    for (int row = 0; row < clip_h; row++) {
      uint32_t *dst =
          target + (dst_y0 + row) * g_render_target.pitch_pixels + dst_x;
      uint32_t *src =
          cached_desktop_surface + (clip_y + row) * cached_desktop_surface_w +
          clip_x;
      fast_memcpy_line(dst, src, clip_w);
    }
  }
}

/* ===================================================================== */
/* Desktop Frame Profiler                                                */
/* ===================================================================== */

#define GUI_PROFILER_PANEL_W 372
#define GUI_PROFILER_PANEL_H 240
#define GUI_PROFILER_PANEL_MARGIN 16
#define GUI_PROFILER_PANEL_ROW_H 16
#define GUI_PROFILER_NOTE_SLOTS 4

static gui_frame_profile_t g_desktop_frame_profile = {0};
static int g_desktop_frame_profile_valid = 0;
static int g_desktop_frame_profiler_enabled = 0;
static uint32_t g_desktop_frame_profile_frame_no = 0;
static int g_desktop_frame_profile_note_count = 0;

typedef struct gui_profiler_note {
  char label[32];
  uint64_t elapsed_us;
  int valid;
} gui_profiler_note_t;

static gui_profiler_note_t g_desktop_frame_notes[GUI_PROFILER_NOTE_SLOTS];

void gui_desktop_frame_profiler_clear_notes(void);

uint64_t gui_monotonic_us(void) {
  uint64_t ticks;
  uint64_t freq;

  ticks = arch_timer_get_ticks();
  freq = arch_timer_get_frequency();
  if (!freq)
    return arch_timer_get_ms() * 1000ULL;
  return (ticks * 1000000ULL) / freq;
}

static void gui_profiler_format_u64(char *buf, size_t max, uint64_t value) {
  size_t pos = 0;

  if (!buf || max == 0)
    return;

  buf[0] = '\0';
  do {
    if (pos + 1 >= max)
      return;
    buf[pos++] = (char)('0' + (value % 10ULL));
    value /= 10ULL;
  } while (value > 0);

  for (size_t i = 0; i < pos / 2; i++) {
    char tmp = buf[i];
    buf[i] = buf[pos - 1 - i];
    buf[pos - 1 - i] = tmp;
  }
  buf[pos] = '\0';
}

static void gui_profiler_copy_label(char *dst, size_t dst_size,
                                   const char *src) {
  size_t i = 0;

  if (!dst || dst_size == 0) return;
  dst[0] = '\0';
  if (!src) return;

  while (src[i] && i + 1 < dst_size) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static void gui_profiler_format_time(char *buf, size_t max, uint64_t us) {
  size_t pos = 0;
  uint64_t whole;
  uint64_t frac;

  if (!buf || max == 0)
    return;

  buf[0] = '\0';

  if (us >= 1000ULL) {
    whole = us / 1000ULL;
    frac = us % 1000ULL;
    if (whole >= 1000000000000000ULL) {
      whole = 1000000000000000ULL - 1;
      frac = 999;
    }
    gui_profiler_format_u64(buf, max, whole);
    pos = 0;
    while (buf[pos])
      pos++;
    if (pos + 5 >= max)
      return;
    buf[pos++] = '.';
    buf[pos++] = (char)('0' + (char)(frac / 100ULL));
    buf[pos++] = (char)('0' + (char)((frac / 10ULL) % 10ULL));
    buf[pos++] = (char)('0' + (char)(frac % 10ULL));
    buf[pos++] = ' ';
    buf[pos++] = 'm';
    buf[pos++] = 's';
    buf[pos] = '\0';
  } else {
    gui_profiler_format_u64(buf, max, us);
    pos = 0;
    while (buf[pos])
      pos++;
    if (pos + 3 >= max)
      return;
    buf[pos++] = ' ';
    buf[pos++] = 'u';
    buf[pos++] = 's';
    buf[pos] = '\0';
  }
}

void gui_desktop_frame_profiler_reset(void) {
  g_desktop_frame_profile.input_poll_us = 0;
  g_desktop_frame_profile.net_poll_us = 0;
  g_desktop_frame_profile.uart_key_us = 0;
  g_desktop_frame_profile.queued_keys_us = 0;
  g_desktop_frame_profile.mouse_us = 0;
  g_desktop_frame_profile.compose_us = 0;
  g_desktop_frame_profile.kernel_slice_us = 0;
  g_desktop_frame_profile.wait_next_frame_us = 0;
  g_desktop_frame_profile.total_us = 0;
  g_desktop_frame_profile_valid = 0;
  g_desktop_frame_profile_frame_no = 0;
  gui_desktop_frame_profiler_clear_notes();
}

void gui_desktop_frame_profiler_clear_notes(void) {
  int i;

  if (!g_desktop_frame_profiler_enabled)
    return;
  for (i = 0; i < GUI_PROFILER_NOTE_SLOTS; i++) {
    g_desktop_frame_notes[i].label[0] = '\0';
    g_desktop_frame_notes[i].elapsed_us = 0;
    g_desktop_frame_notes[i].valid = 0;
  }
  g_desktop_frame_profile_note_count = 0;
}

void gui_profiler_begin(gui_profiler_span_t *span, const char *label) {
  if (!span)
    return;
  if (!g_desktop_frame_profiler_enabled) {
    span->label = label;
    span->start_us = 0;
    span->elapsed_us = 0;
    span->active = 0;
    return;
  }

  span->label = label;
  span->start_us = gui_monotonic_us();
  span->elapsed_us = 0;
  span->active = 1;
}

uint64_t gui_profiler_end(gui_profiler_span_t *span) {
  uint64_t elapsed_us;

  if (!span || !span->active)
    return 0;

  elapsed_us = gui_monotonic_us() - span->start_us;
  span->elapsed_us = elapsed_us;
  span->active = 0;
  return elapsed_us;
}

void gui_desktop_frame_profiler_note(const char *label, uint64_t elapsed_us) {
  gui_profiler_note_t *note;
  int slot;

  if (!g_desktop_frame_profiler_enabled)
    return;
  if (!label || !label[0])
    return;

  slot = g_desktop_frame_profile_note_count;
  if (slot >= GUI_PROFILER_NOTE_SLOTS)
    slot = GUI_PROFILER_NOTE_SLOTS - 1;

  note = &g_desktop_frame_notes[slot];
  gui_profiler_copy_label(note->label, sizeof(note->label), label);
  note->elapsed_us = elapsed_us;
  note->valid = 1;

  if (g_desktop_frame_profile_note_count < GUI_PROFILER_NOTE_SLOTS)
    g_desktop_frame_profile_note_count++;
}

void gui_desktop_frame_profiler_submit(const gui_frame_profile_t *profile) {
  int panel_x;
  int panel_y;

  if (!g_desktop_frame_profiler_enabled || !profile)
    return;

  g_desktop_frame_profile = *profile;
  g_desktop_frame_profile_valid = 1;
  g_desktop_frame_profile_frame_no++;

  panel_x = (int)primary_display.width - GUI_PROFILER_PANEL_W -
            GUI_PROFILER_PANEL_MARGIN;
  if (panel_x < GUI_PROFILER_PANEL_MARGIN)
    panel_x = GUI_PROFILER_PANEL_MARGIN;
  panel_y = GUI_PROFILER_PANEL_MARGIN;
  gui_invalidate_rect(panel_x - 4, panel_y - 4, GUI_PROFILER_PANEL_W + 8,
                      GUI_PROFILER_PANEL_H + 8);
}

static void gui_draw_desktop_frame_profiler(void) {
  int panel_x;
  int panel_y;
  int panel_w = GUI_PROFILER_PANEL_W;
  int panel_h = GUI_PROFILER_PANEL_H;
  char time_buf[32];
  char total_buf[32];
  char frame_buf[32];
  const uint32_t panel_bg = 0xE0161B24;
  const uint32_t panel_border = 0x7A7EA7D8;
  const uint32_t label_fg = 0xE5E7EB;
  const uint32_t value_fg = 0xFFFFFF;
  const uint32_t muted_fg = 0xA7B4C4;
  int row_y;
  int note_y;
  int i;

  if (!g_desktop_frame_profiler_enabled || !g_desktop_frame_profile_valid)
    return;

  panel_x = (int)primary_display.width - panel_w - GUI_PROFILER_PANEL_MARGIN;
  if (panel_x < GUI_PROFILER_PANEL_MARGIN)
    panel_x = GUI_PROFILER_PANEL_MARGIN;
  panel_y = GUI_PROFILER_PANEL_MARGIN;

  gui_fill_rect_alpha(panel_x, panel_y, panel_w, panel_h, panel_bg);
  gui_draw_rect_outline(panel_x, panel_y, panel_w, panel_h, panel_border, 1);
  gui_fill_rect_alpha(panel_x, panel_y, panel_w, 20, 0x24FFFFFF);

  gui_draw_string(panel_x + 12, panel_y + 6, "Desktop Profiler", value_fg,
                  0x00000000);
  frame_buf[0] = '\0';
  gui_profiler_format_u64(frame_buf + 7, sizeof(frame_buf) - 7,
                          g_desktop_frame_profile_frame_no);
  frame_buf[0] = 'F';
  frame_buf[1] = 'r';
  frame_buf[2] = 'a';
  frame_buf[3] = 'm';
  frame_buf[4] = 'e';
  frame_buf[5] = ' ';
  frame_buf[6] = '#';
  gui_draw_string(panel_x + panel_w - 120, panel_y + 6, frame_buf, muted_fg,
                  0x00000000);

  row_y = panel_y + 28;

  gui_profiler_format_time(time_buf, sizeof(time_buf),
                           g_desktop_frame_profile.input_poll_us);
  gui_draw_string(panel_x + 12, row_y, "input poll", label_fg, 0x00000000);
  gui_draw_string(panel_x + 168, row_y, time_buf, value_fg, 0x00000000);

  row_y += GUI_PROFILER_PANEL_ROW_H;
  gui_profiler_format_time(time_buf, sizeof(time_buf),
                           g_desktop_frame_profile.net_poll_us);
  gui_draw_string(panel_x + 12, row_y, "network poll", label_fg,
                  0x00000000);
  gui_draw_string(panel_x + 168, row_y, time_buf, value_fg, 0x00000000);

  row_y += GUI_PROFILER_PANEL_ROW_H;
  gui_profiler_format_time(time_buf, sizeof(time_buf),
                           g_desktop_frame_profile.uart_key_us);
  gui_draw_string(panel_x + 12, row_y, "uart key", label_fg, 0x00000000);
  gui_draw_string(panel_x + 168, row_y, time_buf, value_fg, 0x00000000);

  row_y += GUI_PROFILER_PANEL_ROW_H;
  gui_profiler_format_time(time_buf, sizeof(time_buf),
                           g_desktop_frame_profile.queued_keys_us);
  gui_draw_string(panel_x + 12, row_y, "queued keys", label_fg, 0x00000000);
  gui_draw_string(panel_x + 168, row_y, time_buf, value_fg, 0x00000000);

  row_y += GUI_PROFILER_PANEL_ROW_H;
  gui_profiler_format_time(time_buf, sizeof(time_buf),
                           g_desktop_frame_profile.mouse_us);
  gui_draw_string(panel_x + 12, row_y, "mouse event", label_fg,
                  0x00000000);
  gui_draw_string(panel_x + 168, row_y, time_buf, value_fg, 0x00000000);

  row_y += GUI_PROFILER_PANEL_ROW_H;
  gui_profiler_format_time(time_buf, sizeof(time_buf),
                           g_desktop_frame_profile.compose_us);
  gui_draw_string(panel_x + 12, row_y, "compose", label_fg, 0x00000000);
  gui_draw_string(panel_x + 168, row_y, time_buf, value_fg, 0x00000000);

  row_y += GUI_PROFILER_PANEL_ROW_H;
  gui_profiler_format_time(time_buf, sizeof(time_buf),
                           g_desktop_frame_profile.kernel_slice_us);
  gui_draw_string(panel_x + 12, row_y, "kernel slice", label_fg,
                  0x00000000);
  gui_draw_string(panel_x + 168, row_y, time_buf, value_fg, 0x00000000);

  row_y += GUI_PROFILER_PANEL_ROW_H;
  gui_profiler_format_time(time_buf, sizeof(time_buf),
                           g_desktop_frame_profile.wait_next_frame_us);
  gui_draw_string(panel_x + 12, row_y, "wait next frame", label_fg,
                  0x00000000);
  gui_draw_string(panel_x + 168, row_y, time_buf, value_fg, 0x00000000);

  gui_profiler_format_time(total_buf, sizeof(total_buf),
                           g_desktop_frame_profile.total_us);
  gui_draw_string(panel_x + 12, panel_y + panel_h - 20, "total", label_fg,
                  0x00000000);
  gui_draw_string(panel_x + 168, panel_y + panel_h - 20, total_buf, value_fg,
                  0x00000000);

  note_y = panel_y + 160;
  if (g_desktop_frame_profile_note_count > 0) {
    gui_draw_string(panel_x + 12, note_y, "custom measures", muted_fg,
                    0x00000000);
    note_y += GUI_PROFILER_PANEL_ROW_H;
    for (i = 0; i < g_desktop_frame_profile_note_count &&
                i < GUI_PROFILER_NOTE_SLOTS;
         i++) {
      if (!g_desktop_frame_notes[i].valid)
        continue;
      gui_profiler_format_time(time_buf, sizeof(time_buf),
                               g_desktop_frame_notes[i].elapsed_us);
      gui_draw_string(panel_x + 12, note_y, g_desktop_frame_notes[i].label,
                      label_fg, 0x00000000);
      gui_draw_string(panel_x + 168, note_y, time_buf, value_fg,
                      0x00000000);
      note_y += GUI_PROFILER_PANEL_ROW_H;
    }
  }
}

static void draw_top_rounded_rect_alpha(int x, int y, int w, int h, int r,
                                        uint32_t color) {
  if (w <= 0 || h <= 0)
    return;
  if (r < 0)
    r = 0;
  if (r * 2 > w)
    r = w / 2;
  if (r > h)
    r = h;

  if (r == 0) {
    gui_fill_rect_alpha(x, y, w, h, color);
    return;
  }

  gui_fill_rect_alpha(x, y + r, w, h - r, color);
  gui_fill_rect_alpha(x + r, y, w - 2 * r, r, color);
  gui_fill_rect_alpha(x, y + r, r, h - r, color);
  gui_fill_rect_alpha(x + w - r, y + r, r, h - r, color);

  for (int py = y; py <= y + r; py++) {
    int rel_y = py - (y + r);

    for (int span = r; span >= 0; span--) {
      if (span * span + rel_y * rel_y <= r * r) {
        gui_fill_rect_alpha(x + r - span, py, span * 2 + 1, 1, color);
        gui_fill_rect_alpha(x + w - r - 1 - span, py, span * 2 + 1, 1, color);
        break;
      }
    }
  }
}

static int rects_intersect(int ax, int ay, int aw, int ah, int bx, int by, int bw,
                           int bh) {
  int ax2 = ax + aw;
  int ay2 = ay + ah;
  int bx2 = bx + bw;
  int by2 = by + bh;

  if (aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0)
    return 0;
  return ax < bx2 && ax2 > bx && ay < by2 && ay2 > by;
}

/* ===================================================================== */
/* Compositor - Draw everything with dirty region optimization */
/* ===================================================================== */

/* Dirty region tracking for compositor */
#define MAX_DIRTY_REGIONS 32
typedef struct {
  int x, y, w, h;
  int valid;
} compositor_dirty_rect_t;

static compositor_dirty_rect_t g_dirty_regions[MAX_DIRTY_REGIONS];
static int g_dirty_count = 0;
static int g_full_redraw = 1; /* Start with full redraw */
static int g_frame_count = 0;
#define GUI_BOOT_FULL_REDRAW_FRAMES 300
#define GUI_PARTIAL_REDRAW_CLEAR_DEBUG_FRAMES 180
static int g_gpu_rendering_enabled = 0;
static gui_blur_effects_mode_t g_blur_effects_mode = GUI_BLUR_EFFECTS_AUTO;
static gui_blur_effects_mode_t g_blur_effects_applied_mode =
    GUI_BLUR_EFFECTS_AUTO;
static int g_blur_effects_enabled = 0;
static int g_partial_redraw_clear_debug_frames = 0;
static uint32_t *g_saved_backbuffer = NULL;
static char g_gpu_backend_name[32] = "software";

static int gui_backend_prefers_coalesced_blits(void);
static int compositor_build_dirty_bounds(int *x, int *y, int *w, int *h);
void compositor_mark_full_redraw(void);
int gui_needs_redraw(void);

static int gui_boot_full_redraws_allowed(void) {
  return g_frame_count <= GUI_BOOT_FULL_REDRAW_FRAMES;
}

static void compositor_mark_screen_dirty(void) {
  if (!primary_display.width || !primary_display.height) {
    g_full_redraw = 1;
    return;
  }

  g_dirty_regions[0].x = 0;
  g_dirty_regions[0].y = 0;
  g_dirty_regions[0].w = (int)primary_display.width;
  g_dirty_regions[0].h = (int)primary_display.height;
  g_dirty_regions[0].valid = 1;
  g_dirty_count = 1;
  g_full_redraw = 0;
}

static void compositor_mark_screen_rect_dirty(void) {
  if (!primary_display.width || !primary_display.height)
    return;
  compositor_mark_dirty(0, 0, (int)primary_display.width,
                        (int)primary_display.height);
}

static int dock_handle_click(int x, int y) {
  int dock_y;
  int dock_h;
  int launcher_btn_x;
  int launcher_btn_y;
  int launcher_btn_w;
  int launcher_btn_h;
  int wifi_btn_x;
  int wifi_btn_y;
  int wifi_btn_w;
  int wifi_btn_h;
  int wifi_panel_x;
  int wifi_panel_y;
  int wifi_panel_w;
  int wifi_panel_h;

  if (!dock_is_visible())
    return 0;
  if (!gui_is_installer_mode()) {
    load_dock_config();
  }

  dock_y = primary_display.height - DOCK_HEIGHT;
  dock_h = DOCK_HEIGHT;
  main_menu_launcher_button_rect(&launcher_btn_x, &launcher_btn_y,
                                 &launcher_btn_w, &launcher_btn_h);
  wifi_tray_button_rect(dock_y, dock_h, &wifi_btn_x, &wifi_btn_y, &wifi_btn_w,
                        &wifi_btn_h);
  wifi_tray_panel_rect(dock_y, dock_h, &wifi_panel_x, &wifi_panel_y,
                       &wifi_panel_w, &wifi_panel_h);

  if (wifi_tray_open &&
      x >= wifi_panel_x && x < wifi_panel_x + wifi_panel_w && y >= wifi_panel_y &&
      y < wifi_panel_y + wifi_panel_h) {
    int button_y = wifi_panel_y + 58;
    int password_y = wifi_panel_y + 102;

    if (x >= wifi_panel_x + 14 && x < wifi_panel_x + 84 && y >= button_y &&
        y < button_y + 24) {
      wifi_tray_mark_dirty();
      if (wifi_has_supported_adapter() && wifi_supports_real_scanning())
        wifi_scan();
      wifi_tray_password_active = 0;
      wifi_tray_mark_dirty();
      return 1;
    }
    if (x >= wifi_panel_x + 92 && x < wifi_panel_x + 164 && y >= button_y &&
        y < button_y + 24) {
      wifi_tray_mark_dirty();
      if (wifi_can_connect_selected())
        wifi_connect_selected(wifi_password_draft);
      wifi_tray_password_active = 0;
      wifi_tray_mark_dirty();
      return 1;
    }
    if (x >= wifi_panel_x + 172 && x < wifi_panel_x + 234 && y >= button_y &&
        y < button_y + 24) {
      wifi_tray_mark_dirty();
      wifi_disconnect();
      wifi_tray_password_active = 0;
      wifi_tray_mark_dirty();
      return 1;
    }

    if (wifi_get_selected_network() >= 0 &&
        wifi_get_network_secure(wifi_get_selected_network()) &&
        x >= wifi_panel_x + 14 && x < wifi_panel_x + wifi_panel_w - 14 &&
        y >= password_y && y < password_y + 24) {
      wifi_tray_mark_dirty();
      wifi_tray_password_active = 1;
      wifi_tray_mark_dirty();
      return 1;
    }
    wifi_tray_mark_dirty();
    wifi_tray_password_active = 0;
    wifi_tray_mark_dirty();

    for (int i = 0; i < wifi_get_network_count() && i < 3; i++) {
      int row_y = wifi_panel_y + 134 + i * 24;
      if (x >= wifi_panel_x + 10 && x < wifi_panel_x + wifi_panel_w - 10 &&
          y >= row_y - 2 && y < row_y + 18) {
        wifi_tray_mark_dirty();
        wifi_select_network(i);
        if (!wifi_get_network_secure(i))
          wifi_password_draft[0] = '\0';
        wifi_tray_mark_dirty();
        return 1;
      }
    }

    return 1;
  }

  if (y < dock_y || y >= dock_y + dock_h)
    return 0;

  if (x >= wifi_btn_x && x < wifi_btn_x + wifi_btn_w && y >= wifi_btn_y &&
      y < wifi_btn_y + wifi_btn_h) {
    main_menu_mark_dirty();
    wifi_tray_mark_dirty();
    wifi_tray_open = wifi_tray_open ? 0 : 1;
    wifi_tray_password_active = 0;
    menu_open = 0;
    main_menu_power_open = 0;
    wifi_tray_mark_dirty();
    main_menu_mark_dirty();
    return 1;
  }

  if (x >= launcher_btn_x && x < launcher_btn_x + launcher_btn_w &&
      y >= launcher_btn_y && y < launcher_btn_y + launcher_btn_h) {
    main_menu_mark_dirty();
    wifi_tray_mark_dirty();
    menu_open = menu_open ? 0 : 1;
    wifi_tray_open = 0;
    wifi_tray_password_active = 0;
    if (!menu_open)
      main_menu_power_open = 0;
    wifi_tray_mark_dirty();
    main_menu_mark_dirty();
    return 1;
  }

  {
    int icon_x = launcher_btn_x + launcher_btn_w + 18;
    int icon_y_start = dock_y + (dock_h - DOCK_ICON_SIZE) / 2;

    for (int i = 0; i < dock_item_count; i++) {
      if (x >= icon_x && x < icon_x + DOCK_ICON_SIZE && y >= icon_y_start &&
          y < icon_y_start + DOCK_ICON_SIZE) {
        main_menu_mark_dirty();
        wifi_tray_mark_dirty();
        menu_open = 0;
        main_menu_power_open = 0;
        wifi_tray_open = 0;
        wifi_tray_password_active = 0;
        wifi_tray_mark_dirty();
        main_menu_mark_dirty();
        gui_focus_or_launch_app_by_id(dock_items[i]->id);
        return 1;
      }
      icon_x += DOCK_ICON_SIZE + DOCK_PADDING;
    }
  }

  return 1;
}

/* Mark a region as needing update */
void compositor_mark_dirty(int x, int y, int w, int h) {
  int x2;
  int y2;

  if (w <= 0 || h <= 0)
    return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x >= (int)primary_display.width || y >= (int)primary_display.height)
    return;
  if (x + w > (int)primary_display.width)
    w = (int)primary_display.width - x;
  if (y + h > (int)primary_display.height)
    h = (int)primary_display.height - y;
  if (w <= 0 || h <= 0)
    return;

  x2 = x + w;
  y2 = y + h;

  for (int i = 0; i < g_dirty_count; i++) {
    int rx1;
    int ry1;
    int rx2;
    int ry2;
    int merge_pad = gui_backend_prefers_coalesced_blits() ? 32 : 0;

    if (!g_dirty_regions[i].valid)
      continue;

    rx1 = g_dirty_regions[i].x;
    ry1 = g_dirty_regions[i].y;
    rx2 = rx1 + g_dirty_regions[i].w;
    ry2 = ry1 + g_dirty_regions[i].h;

    if (x >= rx1 && y >= ry1 && x2 <= rx2 && y2 <= ry2)
      return;

    if (x <= rx2 + merge_pad && x2 >= rx1 - merge_pad &&
        y <= ry2 + merge_pad && y2 >= ry1 - merge_pad) {
      int nx1 = x < rx1 ? x : rx1;
      int ny1 = y < ry1 ? y : ry1;
      int nx2 = x2 > rx2 ? x2 : rx2;
      int ny2 = y2 > ry2 ? y2 : ry2;

      g_dirty_regions[i].x = nx1;
      g_dirty_regions[i].y = ny1;
      g_dirty_regions[i].w = nx2 - nx1;
      g_dirty_regions[i].h = ny2 - ny1;
      return;
    }
  }

  if (g_dirty_count < MAX_DIRTY_REGIONS) {
    g_dirty_regions[g_dirty_count].x = x;
    g_dirty_regions[g_dirty_count].y = y;
    g_dirty_regions[g_dirty_count].w = w;
    g_dirty_regions[g_dirty_count].h = h;
    g_dirty_regions[g_dirty_count].valid = 1;
    g_dirty_count++;
  } else {
    if (gui_backend_prefers_coalesced_blits()) {
      int ux;
      int uy;
      int uw;
      int uh;

      if (compositor_build_dirty_bounds(&ux, &uy, &uw, &uh)) {
        int ux2 = ux + uw;
        int uy2 = uy + uh;
        if (x < ux)
          ux = x;
        if (y < uy)
          uy = y;
        if (x2 > ux2)
          ux2 = x2;
        if (y2 > uy2)
          uy2 = y2;
        g_dirty_regions[0].x = ux;
        g_dirty_regions[0].y = uy;
        g_dirty_regions[0].w = ux2 - ux;
        g_dirty_regions[0].h = uy2 - uy;
        g_dirty_regions[0].valid = 1;
        g_dirty_count = 1;
        return;
      }
    }
    if (gui_boot_full_redraws_allowed())
      compositor_mark_full_redraw();
    else
      compositor_mark_dirty(0, 0, (int)primary_display.width,
                            (int)primary_display.height);
  }
}

static void compositor_mark_visible_ui_dirty(void) {
  int screen_w = (int)primary_display.width;
  int screen_h = (int)primary_display.height;

  if (screen_w <= 0 || screen_h <= 0) {
    compositor_mark_screen_dirty();
    return;
  }

  if (MENU_BAR_HEIGHT > 0)
    compositor_mark_dirty(0, 0, screen_w, MENU_BAR_HEIGHT);

  if (dock_is_visible()) {
    int dock_h = dock_reserved_height();
    if (dock_h > 0)
      compositor_mark_dirty(0, screen_h - dock_h, screen_w, dock_h);
  }

  if (menu_open) {
    int x, y, w, h;
    main_menu_panel_rect(&x, &y, &w, &h);
    compositor_mark_dirty(x - 8, y - 8, w + 16, h + 16);
  }

  if (wifi_tray_open && dock_is_visible()) {
    int x, y, w, h;
    int dock_y = screen_h - DOCK_HEIGHT;
    wifi_tray_panel_rect(dock_y, DOCK_HEIGHT, &x, &y, &w, &h);
    compositor_mark_dirty(x - 8, y - 8, w + 16, h + 16);
  }

  if (window_switcher_frames > 0) {
    int panel_w = 360;
    int panel_h = 136;
    int panel_x = (screen_w - panel_w) / 2;
    int panel_y = MENU_BAR_HEIGHT + 36;
    compositor_mark_dirty(panel_x - 12, panel_y - 12, panel_w + 24,
                          panel_h + 24);
  }

  if (secure_attention_open) {
    int panel_w = 420;
    int panel_h = 220;
    int panel_x = (screen_w - panel_w) / 2;
    int panel_y = (screen_h - panel_h) / 2;
    compositor_mark_dirty(panel_x - 12, panel_y - 12, panel_w + 24,
                          panel_h + 24);
  }

  for (struct window *win = window_stack; win; win = win->next) {
    int x, y, w, h;
    if (!win->visible)
      continue;
    window_get_draw_rect(win, &x, &y, &w, &h);
    compositor_mark_dirty(x, y, w, h);
  }
}

void compositor_mark_full_redraw(void) {
  if (gui_boot_full_redraws_allowed()) {
    g_full_redraw = 1;
    g_dirty_count = 0;
  } else {
    compositor_mark_visible_ui_dirty();
  }
}

void gui_invalidate_rect(int x, int y, int w, int h) {
  gui_mark_windows_dirty_for_rect(x, y, w, h);
  compositor_mark_dirty(x, y, w, h);
}

void gui_invalidate_screen(void) {
  compositor_mark_full_redraw();
}

int gui_needs_redraw(void) {
  return g_full_redraw || g_dirty_count > 0 ||
         g_partial_redraw_clear_debug_frames > 0 || gui_has_active_animation();
}

void gui_start_partial_redraw_clear_debug(void) {
  g_partial_redraw_clear_debug_frames =
      GUI_PARTIAL_REDRAW_CLEAR_DEBUG_FRAMES;
}

int gui_partial_redraw_clear_debug_enabled(void) {
  return g_partial_redraw_clear_debug_frames > 0;
}

static int gui_backend_supports_blur_effects(void) {
  if (str_cmp(g_gpu_backend_name, "virtio-gpu") == 0)
    return 1;
  return 0;
}

static int gui_backend_prefers_coalesced_blits(void) {
  return str_cmp(g_gpu_backend_name, "virtio-gpu") == 0 ||
         str_cmp(g_gpu_backend_name, "intel-gfx") == 0;
}

static void gui_load_blur_effects_preference(void) {
  uint8_t *manifest_data = NULL;
  size_t manifest_size = 0;
  char manifest[128];
  char blur_mode[16];

  if (media_load_file(GUI_DISPLAY_CONFIG_PATH, &manifest_data, &manifest_size) < 0)
    return;
  if (!manifest_data || manifest_size == 0 || manifest_size >= sizeof(manifest)) {
    media_free_file(manifest_data);
    return;
  }
  for (size_t i = 0; i < manifest_size; i++)
    manifest[i] = (char)manifest_data[i];
  manifest[manifest_size] = '\0';
  media_free_file(manifest_data);

  if (manifest_get_value(manifest, "blur", blur_mode, sizeof(blur_mode)) != 0)
    return;

  if (str_cmp(blur_mode, "off") == 0) {
    g_blur_effects_mode = GUI_BLUR_EFFECTS_OFF;
  } else if (str_cmp(blur_mode, "force") == 0) {
    g_blur_effects_mode = GUI_BLUR_EFFECTS_FORCE;
  } else {
    g_blur_effects_mode = GUI_BLUR_EFFECTS_AUTO;
  }
}

static const char *gui_blur_effects_mode_name(void) {
  if (g_blur_effects_mode == GUI_BLUR_EFFECTS_FORCE)
    return "force";
  if (g_blur_effects_mode == GUI_BLUR_EFFECTS_OFF)
    return "off";
  return "auto";
}

static const char *gui_blur_effects_button_label(void) {
  if (g_blur_effects_mode == GUI_BLUR_EFFECTS_FORCE)
    return "Blur Force";
  if (g_blur_effects_mode == GUI_BLUR_EFFECTS_OFF)
    return "Blur Off";
  return "Blur Auto";
}

static const char *gui_blur_effects_status_label(void) {
  if (g_blur_effects_enabled)
    return g_blur_effects_mode == GUI_BLUR_EFFECTS_FORCE
               ? "Blur: Forced on"
               : "Blur: On";
  if (g_blur_effects_mode == GUI_BLUR_EFFECTS_OFF)
    return "Blur: Off";
  if (!g_gpu_rendering_enabled)
    return "Blur: Waiting for GPU";
  return g_blur_effects_mode == GUI_BLUR_EFFECTS_FORCE
             ? "Blur: Force unavailable"
             : "Blur: Auto-disabled on this GPU";
}

static const char *gui_blur_effects_detail_message(void) {
  if (g_blur_effects_enabled)
    return g_blur_effects_mode == GUI_BLUR_EFFECTS_FORCE
               ? "Blur and transparency forced on."
               : "Blur and transparency enabled.";
  if (g_blur_effects_mode == GUI_BLUR_EFFECTS_OFF)
    return "Blur and transparency disabled.";
  if (!g_gpu_rendering_enabled)
    return "Transparency enabled. Blur will turn on when GPU rendering is available.";
  return g_blur_effects_mode == GUI_BLUR_EFFECTS_FORCE
             ? "Transparency enabled. Forced blur is unavailable here."
             : "Transparency enabled. Blur auto-disabled on this GPU.";
}

static void gui_cycle_blur_effects_mode(void) {
  if (g_blur_effects_mode == GUI_BLUR_EFFECTS_OFF)
    g_blur_effects_mode = GUI_BLUR_EFFECTS_AUTO;
  else if (g_blur_effects_mode == GUI_BLUR_EFFECTS_AUTO)
    g_blur_effects_mode = GUI_BLUR_EFFECTS_FORCE;
  else
    g_blur_effects_mode = GUI_BLUR_EFFECTS_OFF;
}

static void gui_refresh_blur_effects_policy(void) {
  int next = g_blur_effects_mode != GUI_BLUR_EFFECTS_OFF &&
             g_gpu_rendering_enabled &&
             (gui_backend_supports_blur_effects() ||
              g_blur_effects_mode == GUI_BLUR_EFFECTS_FORCE);
  int mode_changed = g_blur_effects_applied_mode != g_blur_effects_mode;

  if (next == g_blur_effects_enabled && !mode_changed)
    return;

  g_blur_effects_enabled = next;
  g_blur_effects_applied_mode = g_blur_effects_mode;
  printk(KERN_INFO "GUI: blur effects %s (%s, mode=%s)\n",
         g_blur_effects_enabled ? "enabled" : "disabled", g_gpu_backend_name,
         gui_blur_effects_mode_name());
  compositor_mark_full_redraw();
}

void gui_set_blur_effects_enabled(int enabled) {
  g_blur_effects_mode =
      enabled ? GUI_BLUR_EFFECTS_AUTO : GUI_BLUR_EFFECTS_OFF;
  gui_refresh_blur_effects_policy();
}

int gui_blur_effects_requested(void) {
  return g_blur_effects_mode != GUI_BLUR_EFFECTS_OFF;
}

int gui_are_blur_effects_enabled(void) { return g_blur_effects_enabled; }

void gui_configure_gpu_rendering(int enabled) {
  if (enabled == g_gpu_rendering_enabled)
    return;
  if (enabled) {
    if (!primary_display.framebuffer)
      return;
    if (primary_display.backbuffer)
      g_saved_backbuffer = primary_display.backbuffer;
    g_gpu_rendering_enabled = 1;
    printk(KERN_INFO
           "GUI: %s acceleration enabled (safe compositor handoff)\n",
           g_gpu_backend_name);
  } else {
    if (!primary_display.backbuffer && g_saved_backbuffer)
      primary_display.backbuffer = g_saved_backbuffer;
    g_gpu_rendering_enabled = 0;
    printk(KERN_INFO "GUI: Software backbuffer rendering enabled\n");
  }
  gui_use_display_render_target();
  gui_refresh_blur_effects_policy();
  compositor_mark_full_redraw();
}

int gui_is_gpu_rendering_enabled(void) { return g_gpu_rendering_enabled; }

int gui_can_apply_resolution_live(void) {
  return gui_resolution_live_supported_internal();
}

int gui_is_supported_resolution(uint32_t width, uint32_t height) {
  return settings_find_resolution_index(width, height) >= 0;
}

int gui_get_resolution_option_count(void) {
  return SETTINGS_RESOLUTION_OPTION_COUNT;
}

int gui_get_resolution_option(int index, uint32_t *width, uint32_t *height,
                              const char **label) {
  if (index < 0 || index >= SETTINGS_RESOLUTION_OPTION_COUNT)
    return -1;
  if (width)
    *width = settings_resolution_options[index].width;
  if (height)
    *height = settings_resolution_options[index].height;
  if (label)
    *label = settings_resolution_options[index].label;
  return 0;
}

int gui_set_resolution(uint32_t width, uint32_t height) {
  if (!width || !height)
    return -1;
  return gui_apply_resolution(width, height);
}

int gui_run_resolution_self_test(void) {
  uint32_t *fb = NULL;
  uint32_t original_width = 0;
  uint32_t original_height = 0;
  uint32_t target_width = 0;
  uint32_t target_height = 0;
  uint32_t verify_width = 0;
  uint32_t verify_height = 0;
  int preset_count = gui_get_resolution_option_count();
  int found_target = 0;
  extern void fb_get_info(uint32_t **buffer, uint32_t *width, uint32_t *height);

  if (!gui_can_apply_resolution_live()) {
    printk(KERN_INFO "GUI: Resolution self-test unavailable on this backend\n");
    return -1;
  }

  fb_get_info(&fb, &original_width, &original_height);
  for (int i = 0; i < preset_count; i++) {
    if (gui_get_resolution_option(i, &target_width, &target_height, NULL) != 0)
      continue;
    if (target_width != original_width || target_height != original_height) {
      found_target = 1;
      break;
    }
  }

  if (!found_target) {
    printk(KERN_WARNING "GUI: Resolution self-test found no alternate preset\n");
    return -1;
  }

  printk(KERN_INFO "GUI: Resolution self-test %ux%u -> %ux%u\n",
         original_width, original_height, target_width, target_height);

  if (gui_set_resolution(target_width, target_height) != 0) {
    printk(KERN_WARNING "GUI: Resolution self-test failed to switch to %ux%u\n",
           target_width, target_height);
    return -1;
  }

  fb_get_info(&fb, &verify_width, &verify_height);
  if (verify_width != target_width || verify_height != target_height) {
    printk(KERN_WARNING
           "GUI: Resolution self-test framebuffer mismatch after switch "
           "(got %ux%u expected %ux%u)\n",
           verify_width, verify_height, target_width, target_height);
    (void)gui_set_resolution(original_width, original_height);
    return -1;
  }

  if (gui_set_resolution(original_width, original_height) != 0) {
    printk(KERN_WARNING "GUI: Resolution self-test failed to restore %ux%u\n",
           original_width, original_height);
    return -1;
  }

  fb_get_info(&fb, &verify_width, &verify_height);
  if (verify_width != original_width || verify_height != original_height) {
    printk(KERN_WARNING
           "GUI: Resolution self-test restore mismatch "
           "(got %ux%u expected %ux%u)\n",
           verify_width, verify_height, original_width, original_height);
    return -1;
  }

  printk(KERN_INFO "GUI: Resolution self-test passed at %ux%u\n",
         original_width, original_height);
  return 0;
}

void gui_refresh_hardware_acceleration_policy(void) {
  int enable = 0;
  const char *backend = "framebuffer";
  extern int intel_gfx_is_ready(void);
  extern int intel_gfx_has_framebuffer(void);
  extern int intel_gfx_supports_gpu_rendering(void);
  extern bool virtio_gpu_is_available(void);
  extern bool virtio_gpu_has_3d(void);

  if (virtio_gpu_is_available() && virtio_gpu_has_3d()) {
    enable = 1;
    backend = "virtio-gpu";
  } else if (intel_gfx_is_ready() && intel_gfx_has_framebuffer()) {
    backend = "intel-gfx";
    enable = intel_gfx_supports_gpu_rendering();
  } else if (pci_find_device(0x1234, 0x1111)) {
    backend = "bochs-vbe";
  }

  str_copy_safe(g_gpu_backend_name, backend, sizeof(g_gpu_backend_name));
  gui_load_blur_effects_preference();
  gui_configure_gpu_rendering(enable);
  gui_refresh_blur_effects_policy();
}

/* Optimized memcpy for scanlines */
static inline void fast_memcpy_line(uint32_t *dst, uint32_t *src, int width) {
#if defined(ARCH_X86_64)
  if (width > 8 && gui_backend_prefers_coalesced_blits()) {
    size_t bytes = (size_t)width * sizeof(uint32_t);
    size_t qwords = bytes / sizeof(uint64_t);
    size_t tail = bytes & (sizeof(uint64_t) - 1);
    void *dst_ptr = dst;
    const void *src_ptr = src;

    asm volatile("cld; rep movsq"
                 : "+D"(dst_ptr), "+S"(src_ptr), "+c"(qwords)
                 :
                 : "memory");

    if (tail) {
      uint8_t *d8 = (uint8_t *)dst_ptr;
      const uint8_t *s8 = (const uint8_t *)src_ptr;
      for (size_t i = 0; i < tail; i++)
        d8[i] = s8[i];
    }
    return;
  }
#endif
  /* Use 64-bit copies for better performance */
  uint64_t *d64 = (uint64_t *)dst;
  uint64_t *s64 = (uint64_t *)src;
  int count = width / 2;

  for (int i = 0; i < count; i++) {
    d64[i] = s64[i];
  }

  /* Handle odd pixel */
  if (width & 1) {
    dst[width - 1] = src[width - 1];
  }
}

static inline void fast_fill_line(uint32_t *dst, int width, uint32_t color) {
  if (width <= 0)
    return;

  dst[0] = color;
  int filled = 1;
  while (filled < width) {
    int copy = width - filled;
    if (copy > filled)
      copy = filled;
    fast_memcpy_line(dst + filled, dst, copy);
    filled += copy;
  }
}

static inline void fast_copy_framebuffer(uint32_t *dst, uint32_t *src,
                                         size_t pixels) {
#if defined(ARCH_X86_64)
  if (pixels > 128 && gui_backend_prefers_coalesced_blits()) {
    size_t qwords = (pixels * sizeof(uint32_t)) / sizeof(uint64_t);
    size_t tail = (pixels * sizeof(uint32_t)) & (sizeof(uint64_t) - 1);
    void *dst_ptr = dst;
    const void *src_ptr = src;

    asm volatile("cld; rep movsq"
                 : "+D"(dst_ptr), "+S"(src_ptr), "+c"(qwords)
                 :
                 : "memory");
    if (tail) {
      uint8_t *d8 = (uint8_t *)dst_ptr;
      const uint8_t *s8 = (const uint8_t *)src_ptr;
      for (size_t i = 0; i < tail; i++)
        d8[i] = s8[i];
    }
    return;
  }
#endif

  {
    uint64_t *src64 = (uint64_t *)src;
    uint64_t *dst64 = (uint64_t *)dst;
    size_t count64 = pixels / 2;
    size_t i = 0;
    size_t fast_count = count64 & ~7UL;

    for (; i < fast_count; i += 8) {
      dst64[i] = src64[i];
      dst64[i + 1] = src64[i + 1];
      dst64[i + 2] = src64[i + 2];
      dst64[i + 3] = src64[i + 3];
      dst64[i + 4] = src64[i + 4];
      dst64[i + 5] = src64[i + 5];
      dst64[i + 6] = src64[i + 6];
      dst64[i + 7] = src64[i + 7];
    }
    for (; i < count64; i++) {
      dst64[i] = src64[i];
    }
    if (pixels & 1) {
      dst[pixels - 1] = src[pixels - 1];
    }
  }
}

static int compositor_build_coalesced_dirty_rect(int *x, int *y, int *w, int *h) {
  int union_x1 = 0;
  int union_y1 = 0;
  int union_x2 = 0;
  int union_y2 = 0;
  int have_rect = 0;
  uint64_t sum_area = 0;
  uint64_t union_area;

  if (!gui_backend_prefers_coalesced_blits() || g_dirty_count <= 1)
    return 0;

  for (int i = 0; i < g_dirty_count; i++) {
    int rx1;
    int ry1;
    int rx2;
    int ry2;

    if (!g_dirty_regions[i].valid || g_dirty_regions[i].w <= 0 ||
        g_dirty_regions[i].h <= 0)
      continue;

    rx1 = g_dirty_regions[i].x;
    ry1 = g_dirty_regions[i].y;
    rx2 = rx1 + g_dirty_regions[i].w;
    ry2 = ry1 + g_dirty_regions[i].h;

    if (!have_rect) {
      union_x1 = rx1;
      union_y1 = ry1;
      union_x2 = rx2;
      union_y2 = ry2;
      have_rect = 1;
    } else {
      if (rx1 < union_x1)
        union_x1 = rx1;
      if (ry1 < union_y1)
        union_y1 = ry1;
      if (rx2 > union_x2)
        union_x2 = rx2;
      if (ry2 > union_y2)
        union_y2 = ry2;
    }

    sum_area += (uint64_t)g_dirty_regions[i].w * (uint64_t)g_dirty_regions[i].h;
  }

  if (!have_rect)
    return 0;

  union_area = (uint64_t)(union_x2 - union_x1) * (uint64_t)(union_y2 - union_y1);
  if (g_dirty_count < 4 && union_area > sum_area + (sum_area / 2) &&
      str_cmp(g_gpu_backend_name, "intel-gfx") != 0)
    return 0;
  if (str_cmp(g_gpu_backend_name, "intel-gfx") == 0 &&
      union_area > sum_area * 4)
    return 0;

  *x = union_x1;
  *y = union_y1;
  *w = union_x2 - union_x1;
  *h = union_y2 - union_y1;
  return 1;
}

static int compositor_build_dirty_bounds(int *x, int *y, int *w, int *h) {
  int min_x = 0;
  int min_y = 0;
  int max_x = 0;
  int max_y = 0;
  int have_rect = 0;

  if (!x || !y || !w || !h || g_dirty_count <= 0)
    return 0;

  for (int i = 0; i < g_dirty_count; i++) {
    int rx1;
    int ry1;
    int rx2;
    int ry2;

    if (!g_dirty_regions[i].valid || g_dirty_regions[i].w <= 0 ||
        g_dirty_regions[i].h <= 0)
      continue;

    rx1 = g_dirty_regions[i].x;
    ry1 = g_dirty_regions[i].y;
    rx2 = rx1 + g_dirty_regions[i].w;
    ry2 = ry1 + g_dirty_regions[i].h;

    if (!have_rect) {
      min_x = rx1;
      min_y = ry1;
      max_x = rx2;
      max_y = ry2;
      have_rect = 1;
    } else {
      if (rx1 < min_x)
        min_x = rx1;
      if (ry1 < min_y)
        min_y = ry1;
      if (rx2 > max_x)
        max_x = rx2;
      if (ry2 > max_y)
        max_y = ry2;
    }
  }

  if (!have_rect)
    return 0;

  *x = min_x;
  *y = min_y;
  *w = max_x - min_x;
  *h = max_y - min_y;
  return (*w > 0 && *h > 0);
}

/* Copy a specific region from backbuffer to framebuffer */
static void blit_region(int x, int y, int w, int h) {
  if (!primary_display.backbuffer || !primary_display.framebuffer)
    return;

  /* Clip to screen bounds */
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > (int)primary_display.width)
    w = primary_display.width - x;
  if (y + h > (int)primary_display.height)
    h = primary_display.height - y;
  if (w <= 0 || h <= 0)
    return;

  int pitch_pixels = primary_display.pitch / 4;

  if (gui_partial_redraw_clear_debug_enabled()) {
    for (int row = y; row < y + h; row++) {
      uint32_t *dst = primary_display.framebuffer + row * pitch_pixels + x;
      for (int col = 0; col < w; col++)
        dst[col] = 0x000000;
    }
    return;
  }

  for (int row = y; row < y + h; row++) {
    uint32_t *src = primary_display.backbuffer + row * pitch_pixels + x;
    uint32_t *dst = primary_display.framebuffer + row * pitch_pixels + x;
    fast_memcpy_line(dst, src, w);
  }
}

static void gui_draw_scene_layers_rect(int dirty_x, int dirty_y, int dirty_w,
                                       int dirty_h) {
  draw_desktop();

  /* Draw windows from bottom to top (reverse order) */
  struct window *draw_order[MAX_WINDOWS];
  int count = 0;
  for (struct window *win = window_stack; win && count < MAX_WINDOWS;
       win = win->next) {
    draw_order[count++] = win;
  }

  for (int i = count - 1; i >= 0; i--) {
    int win_x;
    int win_y;
    int win_w;
    int win_h;

    window_get_draw_rect(draw_order[i], &win_x, &win_y, &win_w, &win_h);
    if (rects_intersect(win_x, win_y, win_w, win_h, dirty_x, dirty_y, dirty_w,
                        dirty_h))
      draw_window(draw_order[i]);
  }

  if (rects_intersect(0, 0, (int)primary_display.width, MENU_BAR_HEIGHT, dirty_x,
                      dirty_y, dirty_w, dirty_h))
    draw_menu_bar();
  if (dock_is_visible() &&
      rects_intersect(0,
                      (int)primary_display.height - dock_reserved_height(),
                      (int)primary_display.width, dock_reserved_height(), dirty_x,
                      dirty_y, dirty_w, dirty_h))
    draw_dock();

  if (window_switcher_frames > 0)
    draw_window_switcher_overlay();
  if (secure_attention_open)
    draw_secure_attention_overlay();

  {
    int profiler_x = (int)primary_display.width - GUI_PROFILER_PANEL_W -
                     GUI_PROFILER_PANEL_MARGIN;
    int profiler_y = GUI_PROFILER_PANEL_MARGIN;
    if (profiler_x < GUI_PROFILER_PANEL_MARGIN)
      profiler_x = GUI_PROFILER_PANEL_MARGIN;
    if (g_desktop_frame_profiler_enabled && g_desktop_frame_profile_valid &&
        rects_intersect(profiler_x, profiler_y, GUI_PROFILER_PANEL_W,
                        GUI_PROFILER_PANEL_H, dirty_x, dirty_y, dirty_w, dirty_h))
      gui_draw_desktop_frame_profiler();
  }
}

static void gui_draw_scene_layers(void) {
  gui_draw_scene_layers_rect(0, 0, (int)primary_display.width,
                             (int)primary_display.height);
}

static uint32_t gui_contrast_title_color(uint32_t rgb) {
  uint32_t r = (rgb >> 16) & 0xFF;
  uint32_t g = (rgb >> 8) & 0xFF;
  uint32_t b = rgb & 0xFF;
  uint32_t luminance = r * 299 + g * 587 + b * 114;

  return luminance >= 128000 ? 0x000000 : 0xFFFFFF;
}

/* Forward declaration for cursor */
void gui_draw_cursor(void);

void gui_compose(void) {
  struct gui_clip_state prev_clip = g_clip;
  int draw_x;
  int draw_y;
  int draw_w;
  int draw_h;

  g_frame_count++;
  startup_process_first_boot_completion();
  if (!startup_setup_account_active()) {
    installer_process_autorun();
    installer_process_background_install();
  }
  update_main_menu_power_animation();

  /* Catch missed early-boot updates, then rely on explicit dirty regions. */
  if (gui_boot_full_redraws_allowed() &&
      (g_frame_count &
       (gui_backend_prefers_coalesced_blits() ? 0x1FF : 0x3F)) == 0) {
    g_full_redraw = 1;
  }

  /* Update Bowling game state (throttled) */
  static int bowling_tick = 0;
  if (++bowling_tick >= 2) {
    bowling_tick = 0;
    bowling_update();
  }

  gui_update_window_animations();

  if (window_switcher_frames > 0)
    compositor_mark_screen_rect_dirty();

  if (g_partial_redraw_clear_debug_frames > 0) {
    g_partial_redraw_clear_debug_frames--;
    if (g_partial_redraw_clear_debug_frames == 0)
      compositor_mark_visible_ui_dirty();
  }

  /* No dirty regions means no redraw work. */
  if (!g_full_redraw && g_dirty_count == 0)
    return;

  gui_use_display_render_target();
  if (g_full_redraw) {
    gui_draw_scene_layers();
  } else if (compositor_build_dirty_bounds(&draw_x, &draw_y, &draw_w,
                                           &draw_h)) {
    prev_clip = gui_set_clip_rect(draw_x, draw_y, draw_w, draw_h);
    gui_draw_scene_layers_rect(draw_x, draw_y, draw_w, draw_h);
    gui_restore_clip_rect(prev_clip);
  }

  if (window_switcher_frames > 0)
    window_switcher_frames--;

  /* Draw cursor to backbuffer BEFORE blit */
  gui_draw_cursor();

  /* Smart frame buffer update */
  if (primary_display.backbuffer && primary_display.framebuffer) {
    if (g_full_redraw) {
      size_t pixel_count =
          ((size_t)primary_display.pitch * (size_t)primary_display.height) /
          sizeof(uint32_t);
      fast_copy_framebuffer(primary_display.framebuffer,
                            primary_display.backbuffer, pixel_count);
      g_full_redraw = 0;
    } else {
      /* Partial update - only copy dirty regions */
      blit_region(draw_x, draw_y, draw_w, draw_h);
    }

    /* Memory barrier */
#ifdef ARCH_ARM64
    asm volatile("dsb sy" ::: "memory");
#elif defined(ARCH_X86_64) || defined(ARCH_X86)
    asm volatile("mfence" ::: "memory");
#endif
  }

  /* Clear dirty regions for next frame */
  g_dirty_count = 0;

}

/* ===================================================================== */
/* Mouse cursor rasterized from assets/cursor.svg and drawn to the backbuffer. */
/* ===================================================================== */

#ifdef CONFIG_EMBED_SEED_ASSETS
#define GUI_CURSOR_WIDTH ((int)bootstrap_cursor_width)
#define GUI_CURSOR_HEIGHT ((int)bootstrap_cursor_height)
#define GUI_CURSOR_PIXEL(row, col) \
  (bootstrap_cursor_rgba[(row) * bootstrap_cursor_width + (col)])
#else
#define GUI_CURSOR_WIDTH 16
#define GUI_CURSOR_HEIGHT 16
static const uint32_t gui_cursor_rgba[GUI_CURSOR_HEIGHT][GUI_CURSOR_WIDTH] = {
    {0x86F1F1F1, 0x9FFAFAFA, 0x21FFFFFF, 0x00000000, 0x00000000, 0x03FFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0xD1CECECE, 0xFF4A4A4A, 0xECA4A4A4, 0x7DFFFFFF, 0x0CFFFFFF, 0x00000000, 0x02000000, 0x02FFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0xCACCCCCC, 0xFC131313, 0xFF000000, 0xFF525252, 0xDABDBDBD, 0x54FFFFFF, 0x00000000, 0x00000000, 0x04BFBFBF, 0x01FFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0xCACDCDCD, 0xFF202020, 0xFC101010, 0xFB020202, 0xFF0E0E0E, 0xFF757575, 0xAEE4E4E4, 0x26FFFFFF, 0x00000000, 0x00000000, 0x03FFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0xCDCCCCCC, 0xFF1D1D1D, 0xFD0D0D0D, 0xFF131313, 0xFD101010, 0xFC000000, 0xFF2A2A2A, 0xF39C9C9C, 0x78FFFFFF, 0x05FFFFFF, 0x00000000, 0x04BFBFBF, 0x01FFFFFF, 0x00000000, 0x00000000, 0x00000000},
    {0xB7DDDDDD, 0xFF2C2C2C, 0xFC090909, 0xFF111111, 0xFF101010, 0xFF141414, 0xFC090909, 0xFF010101, 0xFF575757, 0xCCCDCDCD, 0x3FFFFFFF, 0x00000000, 0x00000000, 0x03FFFFFF, 0x00000000, 0x00000000},
    {0xABE4E4E4, 0xFF313131, 0xFC080808, 0xFF111111, 0xFF101010, 0xFF101010, 0xFF111111, 0xFE131313, 0xFB000000, 0xFF181818, 0xFE868686, 0x92FFFFFF, 0x0FFFFFFF, 0x00000000, 0x03AAAAAA, 0x01FFFFFF},
    {0xA2ECECEC, 0xFF393939, 0xFC060606, 0xFF111111, 0xFF101010, 0xFF101010, 0xFF101010, 0xFF101010, 0xFF131313, 0xFD0D0D0D, 0xFE000000, 0xFF484848, 0xD9C0C0C0, 0x4BFFFFFF, 0x00000000, 0x00000000},
    {0x89FFFFFF, 0xFF474747, 0xFC030303, 0xFF121212, 0xFF111111, 0xFF111111, 0xFF101010, 0xFF101010, 0xFF101010, 0xFF121212, 0xFF0A0A0A, 0xFF000000, 0xFF070707, 0xFF7F7F7F, 0xA8EEEEEE, 0x1DFFFFFF},
    {0x76FFFFFF, 0xFF565656, 0xFC020202, 0xFE161616, 0xFC080808, 0xFC0A0A0A, 0xFF131313, 0xFF101010, 0xFF121212, 0xFF090909, 0xFF494949, 0xEF959595, 0xE4AAAAAA, 0xD4A3A3A3, 0xCBE9E9E9, 0x42FFFFFF},
    {0x5AFFFFFF, 0xFE696969, 0xFA000000, 0xFF000000, 0xFF333333, 0xFF282828, 0xFD040404, 0xFF141414, 0xFF131313, 0xFC000000, 0xFF7E7E7E, 0x66FFFFFF, 0x0AFFFFFF, 0x03FFFFFF, 0x00000000, 0x00000000},
    {0x3DFFFFFF, 0xFF7D7D7D, 0xFF1B1B1B, 0xFF858585, 0xA8E9E9E9, 0xBBD2D2D2, 0xFF3E3E3E, 0xFB020202, 0xFF141414, 0xFD0E0E0E, 0xFF0E0E0E, 0xD6B5B5B5, 0x04FFFFFF, 0x02000000, 0x03AAAAAA, 0x01FFFFFF},
    {0x14FFFFFF, 0xDAD1D1D1, 0xB4D5D5D5, 0x38FFFFFF, 0x00000000, 0x01FFFFFF, 0xA7E8E8E8, 0xFF414141, 0xFB010101, 0xFF181818, 0xFC020202, 0xFF555555, 0x7FFFFFFF, 0x00000000, 0x03FFFFFF, 0x00000000},
    {0x00000000, 0x0AFFFFFF, 0x00000000, 0x00000000, 0x04BFBFBF, 0x02000000, 0x000A0A0A, 0xA8D9D9D9, 0xFF353535, 0xFB000000, 0xFC0F0F0F, 0xFA000000, 0xFD8B8B8B, 0x33FFFFFF, 0x00000000, 0x02FFFFFF},
    {0x01FFFFFF, 0x00000000, 0x02000000, 0x02FFFFFF, 0x00000000, 0x02FFFFFF, 0x02000000, 0x02FFFFFF, 0xBAD3D3D3, 0xFF393939, 0xFF282828, 0xFF595959, 0xEEAEAEAE, 0x63FFFFFF, 0x00000000, 0x03FFFFFF},
    {0x00000000, 0x01FFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x02FFFFFF, 0x01000000, 0x0CFFFFFF, 0xBBE5E5E5, 0xB6DBDBDB, 0x65FFFFFF, 0x1BFFFFFF, 0x00000000, 0x01FFFFFF, 0x00000000},
};
#define GUI_CURSOR_PIXEL(row, col) (gui_cursor_rgba[(row)][(col)])
#endif

static uint32_t gui_blend_cursor_pixel(uint32_t dst, uint32_t src) {
  uint32_t alpha = (src >> 24) & 0xFF;
  if (alpha == 0)
    return dst;
  if (alpha == 0xFF)
    return src;

  uint32_t inv_alpha = 255 - alpha;
  uint32_t src_r = (src >> 16) & 0xFF;
  uint32_t src_g = (src >> 8) & 0xFF;
  uint32_t src_b = src & 0xFF;
  uint32_t dst_r = (dst >> 16) & 0xFF;
  uint32_t dst_g = (dst >> 8) & 0xFF;
  uint32_t dst_b = dst & 0xFF;

  uint32_t out_r = (src_r * alpha + dst_r * inv_alpha) / 255;
  uint32_t out_g = (src_g * alpha + dst_g * inv_alpha) / 255;
  uint32_t out_b = (src_b * alpha + dst_b * inv_alpha) / 255;
  return 0xFF000000 | (out_r << 16) | (out_g << 8) | out_b;
}

static void gui_mark_cursor_dirty_at(int x, int y) {
  gui_invalidate_rect(x - 1, y - 1, GUI_CURSOR_WIDTH + 2,
                      GUI_CURSOR_HEIGHT + 2);
}

/* Draw cursor directly to the active render target. */
void gui_draw_cursor(void) {
  extern void mouse_get_position(int *x, int *y);
  int cx, cy;
  mouse_get_position(&cx, &cy);

  /* Update global mouse position for event handling */
  mouse_x = cx;
  mouse_y = cy;

  uint32_t *target =
      primary_display.backbuffer ? primary_display.backbuffer
                                 : primary_display.framebuffer;
  if (!target)
    return;

  int pitch = primary_display.pitch / 4;

  for (int row = 0; row < GUI_CURSOR_HEIGHT; row++) {
    for (int col = 0; col < GUI_CURSOR_WIDTH; col++) {
      uint32_t pixel = GUI_CURSOR_PIXEL(row, col);
      if ((pixel >> 24) == 0)
        continue; /* Transparent */

      int px = cx + col;
      int py = cy + row;
      if (px >= 0 && px < (int)primary_display.width && py >= 0 &&
          py < (int)primary_display.height) {
        target[py * pitch + px] =
            gui_blend_cursor_pixel(target[py * pitch + px], pixel);
      }
    }
  }
}

void gui_move_mouse(int dx, int dy) {
  int old_x = mouse_x;
  int old_y = mouse_y;

  mouse_x += dx;
  mouse_y += dy;

  if (mouse_x < 0)
    mouse_x = 0;
  if (mouse_y < 0)
    mouse_y = 0;
  if (mouse_x >= (int)primary_display.width)
    mouse_x = primary_display.width - 1;
  if (mouse_y >= (int)primary_display.height)
    mouse_y = primary_display.height - 1;

  if (mouse_x != old_x || mouse_y != old_y) {
    gui_mark_cursor_dirty_at(old_x, old_y);
    gui_mark_cursor_dirty_at(mouse_x, mouse_y);
    if (menu_open)
      main_menu_mark_dirty();
    if (wifi_tray_open || wifi_tray_contains_point(old_x, old_y) ||
        wifi_tray_contains_point(mouse_x, mouse_y))
      wifi_tray_mark_dirty();
  }
}

void gui_set_mouse_buttons(int buttons) { mouse_buttons = buttons; }

void gui_handle_key_event(int key) {
  if (key < 0 || key > 0x1FF) {
    static int warned_bad_key = 0;
    if (!warned_bad_key) {
      printk(KERN_WARNING "GUI: Ignoring invalid keycode %d\n", key);
      warned_bad_key = 1;
    }
    return;
  }

  if (startup_flow_active()) {
    startup_handle_key(key);
    return;
  }

  if (key == KEY_CTRL_ALT_DEL) {
    open_secure_attention();
    return;
  }

  if (key == KEY_LEFT_SUPER || key == KEY_RIGHT_SUPER) {
    main_menu_mark_dirty();
    menu_open = menu_open ? 0 : 1;
    main_menu_power_open = 0;
    main_menu_mark_dirty();
    return;
  }

  if (secure_attention_open) {
    if (key == 27) {
      execute_secure_attention_action(SECURE_ACTION_CANCEL);
      return;
    }
    if (key == '\t' || key == KEY_RIGHT) {
      secure_attention_selection = (secure_attention_selection + 1) % 3;
      compositor_mark_screen_rect_dirty();
      return;
    }
    if (key == KEY_LEFT) {
      secure_attention_selection = (secure_attention_selection + 2) % 3;
      compositor_mark_screen_rect_dirty();
      return;
    }
    if (key == '\n' || key == '\r' || key == ' ') {
      execute_secure_attention_action(secure_attention_selection);
      return;
    }
    return;
  }

  if (key == KEY_WINDOW_SWITCHER) {
    activate_window_switcher();
    return;
  }

  /* Check if desktop is doing inline rename - takes priority */
  extern int desktop_is_renaming(void);
  extern int desktop_handle_key(int key);
  if (desktop_is_renaming()) {
    if (desktop_handle_key(key))
      return; /* Desktop consumed the key */
  }

  if (wifi_tray_open && wifi_tray_password_active) {
    if (key == '\t' || key == 27) {
      wifi_tray_password_active = 0;
      wifi_tray_mark_dirty();
      return;
    }
    if (key == '\r' || key == '\n') {
      if (wifi_can_connect_selected())
        wifi_connect_selected(wifi_password_draft);
      wifi_tray_mark_dirty();
      return;
    }
    append_input_char(wifi_password_draft, (int)sizeof(wifi_password_draft), key);
    wifi_tray_mark_dirty();
    return;
  }

  if (focused_window &&
      (!focused_window->visible || !gui_window_in_stack(focused_window))) {
    gui_clear_focus();
  }

  /* Route key to focused window */
  if (focused_window && focused_window->visible) {
    int focused_draw_x;
    int focused_draw_y;
    int focused_draw_w;
    int focused_draw_h;

    window_get_draw_rect(focused_window, &focused_draw_x, &focused_draw_y,
                         &focused_draw_w, &focused_draw_h);
    compositor_mark_dirty(focused_draw_x, focused_draw_y, focused_draw_w,
                          focused_draw_h);

    if (focused_window->title[0] == 'S' && focused_window->title[1] == 'e' &&
        focused_window->title[2] == 't' && settings_user_editor_is_visible()) {
      if (!account_role_is_admin())
        return;
      char *target = settings_user_active_field == 0 ? settings_user_new_name
                                                     : settings_user_new_password;
      if (key == '\t') {
        settings_user_active_field = 1 - settings_user_active_field;
        return;
      }
      if (key == '\r' || key == '\n') {
        settings_add_user_account();
        return;
      }
      append_input_char(target, 32, key);
      return;
    }
    if (focused_window->title[0] == 'S' && focused_window->title[1] == 'e' &&
        focused_window->title[2] == 't' && settings_active_tab == 1 &&
        settings_wifi_password_active) {
      if (key == '\t' || key == 27) {
        settings_wifi_password_active = 0;
        return;
      }
      if (key == '\r' || key == '\n') {
        if (wifi_can_connect_selected())
          wifi_connect_selected(wifi_password_draft);
        str_copy_safe(settings_status, wifi_get_status_text(),
                      sizeof(settings_status));
        return;
      }
      append_input_char(wifi_password_draft, (int)sizeof(wifi_password_draft),
                        key);
      return;
    }
    /* Check if it's a Terminal window */
    if (focused_window->title[0] == 'T' && focused_window->title[1] == 'e' &&
        focused_window->title[2] == 'r') {
      struct terminal *term = (struct terminal *)focused_window->userdata;
      if (!term) {
        term = term_get_active();
      }
      if (term) {
        term_set_active(term);
        term_handle_key(term, key);
      }
    }
    /* Check if it's a Notepad window */
    else if (focused_window->title[0] == 'N' &&
             focused_window->title[1] == 'o' &&
             focused_window->title[2] == 't') {
      notepad_key(key);
    }
    /* Check if it's a Rename window */
    else if (focused_window->title[0] == 'R' &&
             focused_window->title[1] == 'e' &&
             focused_window->title[2] == 'n') {
      rename_key(key);
    }
    /* Check if it's a Bowling window */
    else if ((focused_window->title[0] == 'B' &&
              focused_window->title[1] == 'o' &&
              focused_window->title[2] == 'w') ||
             (focused_window->title[0] == 'S' &&
              focused_window->title[1] == 'n' &&
              focused_window->title[2] == 'a')) {
      bowling_key(key);
    }
    /* Check if it's an Image Viewer window */
    else if (focused_window->title[0] == 'I' &&
             focused_window->title[1] == 'm' &&
             focused_window->title[2] == 'a') {
      /* ESC key (27) - exit fullscreen */
      if (key == 27 && g_imgview.fullscreen) {
        g_imgview.fullscreen = 0;
        g_imgview.zoom_pct = 0;
        g_imgview.offset_x = 0;
        g_imgview.offset_y = 0;
      }
      /* F key - toggle fullscreen */
      else if (key == 'f' || key == 'F') {
        g_imgview.fullscreen = !g_imgview.fullscreen;
        g_imgview.zoom_pct = 0;
        g_imgview.offset_x = 0;
        g_imgview.offset_y = 0;
      }
      /* R key - rotate right */
      else if (key == 'r' || key == 'R') {
        g_imgview.rotation = (g_imgview.rotation + 90) % 360;
      }
      /* L key - rotate left */
      else if (key == 'l' || key == 'L') {
        g_imgview.rotation = (g_imgview.rotation + 270) % 360;
      }
    }
    /* Call window's key handler if set */
    if (focused_window->on_key) {
      focused_window->on_key(focused_window, key);
    }
    if (focused_window && focused_window->visible &&
        gui_window_in_stack(focused_window)) {
      window_get_draw_rect(focused_window, &focused_draw_x, &focused_draw_y,
                           &focused_draw_w, &focused_draw_h);
      compositor_mark_dirty(focused_draw_x, focused_draw_y, focused_draw_w,
                            focused_draw_h);
    }
    return;
  }

  if ((key >= 32 && key < 127) || key == '\b' || key == 127)
    return;
}

/* ===================================================================== */
/* Event Handling with Window Dragging and Resizing */
/* ===================================================================== */

/* Dragging state */
static struct window *dragging_window = 0;
static int drag_offset_x = 0, drag_offset_y = 0;
static int prev_buttons = 0;

#define SNAP_EDGE_THRESHOLD 28
#define WINDOW_BOTTOM_CLEARANCE 12

static void snap_window_to_zone(struct window *win, int mouse_x_pos,
                                int mouse_y_pos) {
  int screen_w;
  int screen_h;
  int work_y;
  int work_h;
  int old_x;
  int old_y;
  int old_w;
  int old_h;
  int snapped = 0;

  if (!win)
    return;

  window_get_draw_rect(win, &old_x, &old_y, &old_w, &old_h);

  screen_w = (int)primary_display.width;
  screen_h = (int)primary_display.height;
  work_y = MENU_BAR_HEIGHT;
  work_h =
      screen_h - MENU_BAR_HEIGHT - dock_reserved_height() - WINDOW_BOTTOM_CLEARANCE;

  if (mouse_y_pos <= work_y + SNAP_EDGE_THRESHOLD) {
    if (win->state != WINDOW_MAXIMIZED) {
      win->saved_x = win->x;
      win->saved_y = win->y;
      win->saved_width = win->width;
      win->saved_height = win->height;
    }
    win->x = 0;
    win->y = work_y;
    win->width = screen_w;
    win->height = work_h;
    win->state = WINDOW_MAXIMIZED;
    snapped = 1;
  }

  else if (mouse_x_pos <= SNAP_EDGE_THRESHOLD &&
      mouse_y_pos <= work_y + work_h / 2) {
    win->x = 0;
    win->y = work_y;
    win->width = screen_w / 2;
    win->height = work_h / 2;
    win->state = WINDOW_NORMAL;
    snapped = 1;
  }

  else if (mouse_x_pos >= screen_w - SNAP_EDGE_THRESHOLD &&
      mouse_y_pos <= work_y + work_h / 2) {
    win->x = screen_w / 2;
    win->y = work_y;
    win->width = screen_w - win->x;
    win->height = work_h / 2;
    win->state = WINDOW_NORMAL;
    snapped = 1;
  }

  else if (mouse_x_pos <= SNAP_EDGE_THRESHOLD &&
      mouse_y_pos >= work_y + work_h - SNAP_EDGE_THRESHOLD) {
    win->x = 0;
    win->y = work_y + work_h / 2;
    win->width = screen_w / 2;
    win->height = work_h - work_h / 2;
    win->state = WINDOW_NORMAL;
    snapped = 1;
  }

  else if (mouse_x_pos >= screen_w - SNAP_EDGE_THRESHOLD &&
      mouse_y_pos >= work_y + work_h - SNAP_EDGE_THRESHOLD) {
    win->x = screen_w / 2;
    win->y = work_y + work_h / 2;
    win->width = screen_w - win->x;
    win->height = work_h - work_h / 2;
    win->state = WINDOW_NORMAL;
    snapped = 1;
  }

  else if (mouse_x_pos <= SNAP_EDGE_THRESHOLD) {
    win->x = 0;
    win->y = work_y;
    win->width = screen_w / 2;
    win->height = work_h;
    win->state = WINDOW_NORMAL;
    snapped = 1;
  }

  else if (mouse_x_pos >= screen_w - SNAP_EDGE_THRESHOLD) {
    win->x = screen_w / 2;
    win->y = work_y;
    win->width = screen_w - win->x;
    win->height = work_h;
    win->state = WINDOW_NORMAL;
    snapped = 1;
  }

  if (snapped) {
    int new_x;
    int new_y;
    int new_w;
    int new_h;
    compositor_mark_dirty(old_x, old_y, old_w, old_h);
    window_get_draw_rect(win, &new_x, &new_y, &new_w, &new_h);
    compositor_mark_dirty(new_x, new_y, new_w, new_h);
  }
}

/* Resizing state */
static struct window *resizing_window = 0;
#define RESIZE_NONE 0
#define RESIZE_RIGHT 1
#define RESIZE_BOTTOM 2
#define RESIZE_BOTTOM_RIGHT 3
#define RESIZE_LEFT 4
#define RESIZE_TOP 5
#define RESIZE_TOP_LEFT 6
#define RESIZE_TOP_RIGHT 7
#define RESIZE_BOTTOM_LEFT 8
static int resize_edge = RESIZE_NONE;
static int resize_start_x = 0, resize_start_y = 0;
static int resize_start_w = 0, resize_start_h = 0;
static int resize_start_win_x = 0, resize_start_win_y = 0;

static void window_begin_resize(struct window *win, int edge, int x, int y) {
  if (!win || edge == RESIZE_NONE)
    return;
  resizing_window = win;
  resize_edge = edge;
  resize_start_x = x;
  resize_start_y = y;
  resize_start_w = win->width;
  resize_start_h = win->height;
  resize_start_win_x = win->x;
  resize_start_win_y = win->y;
}

static int gui_resize_edge_from_skin_hit(SkinHit hit) {
  switch (hit) {
  case SKIN_HIT_RESIZE_RIGHT:
    return RESIZE_RIGHT;
  case SKIN_HIT_RESIZE_BOTTOM:
    return RESIZE_BOTTOM;
  case SKIN_HIT_RESIZE_BOTTOM_RIGHT:
    return RESIZE_BOTTOM_RIGHT;
  case SKIN_HIT_RESIZE_LEFT:
    return RESIZE_LEFT;
  case SKIN_HIT_RESIZE_TOP:
    return RESIZE_TOP;
  case SKIN_HIT_RESIZE_TOP_LEFT:
    return RESIZE_TOP_LEFT;
  case SKIN_HIT_RESIZE_TOP_RIGHT:
    return RESIZE_TOP_RIGHT;
  case SKIN_HIT_RESIZE_BOTTOM_LEFT:
    return RESIZE_BOTTOM_LEFT;
  default:
    return RESIZE_NONE;
  }
}

static void gui_set_newwindows_pointer_state(struct window *hot_window,
                                             SkinHit hot_hit,
                                             struct window *pressed_window,
                                             SkinHit pressed_hit) {
  struct window *old_hot_window = chrome_hot_window;
  struct window *old_pressed_window = chrome_pressed_window;
  SkinHit old_hot_hit = chrome_hot_hit;
  SkinHit old_pressed_hit = chrome_pressed_hit;

  chrome_hot_window = hot_window;
  chrome_hot_hit = hot_hit;
  chrome_pressed_window = pressed_window;
  chrome_pressed_hit = pressed_hit;

  if (old_hot_window && (old_hot_window != chrome_hot_window ||
                         old_hot_hit != chrome_hot_hit))
    gui_invalidate_window(old_hot_window);
  if (chrome_hot_window && (old_hot_window != chrome_hot_window ||
                            old_hot_hit != chrome_hot_hit))
    gui_invalidate_window(chrome_hot_window);
  if (old_pressed_window &&
      (old_pressed_window != chrome_pressed_window ||
       old_pressed_hit != chrome_pressed_hit))
    gui_invalidate_window(old_pressed_window);
  if (chrome_pressed_window &&
      (old_pressed_window != chrome_pressed_window ||
       old_pressed_hit != chrome_pressed_hit))
    gui_invalidate_window(chrome_pressed_window);
}

static void gui_update_newwindows_hover_state(int x, int y) {
  struct window *hot_window = NULL;
  SkinHit hot_hit = SKIN_HIT_NONE;

  if (chrome_pressed_window) {
    hot_window = chrome_pressed_window;
    hot_hit = gui_newwindows_hit_test(chrome_pressed_window, x, y);
    if (hot_hit != chrome_pressed_hit)
      hot_hit = SKIN_HIT_NONE;
  } else {
    for (struct window *win = window_stack; win; win = win->next) {
      int draw_x, draw_y, draw_w, draw_h;
      if (!win->visible || !window_uses_newwindows_chrome(win))
        continue;
      window_get_draw_rect(win, &draw_x, &draw_y, &draw_w, &draw_h);
      if (x < draw_x || x >= draw_x + draw_w || y < draw_y || y >= draw_y + draw_h)
        continue;
      hot_hit = gui_newwindows_hit_test(win, x, y);
      hot_window = win;
      break;
    }
  }

  gui_set_newwindows_pointer_state(hot_window, hot_hit, chrome_pressed_window,
                                   chrome_pressed_hit);
}

static void window_restore_saved_bounds(struct window *win) {
  if (!win)
    return;
  win->x = win->saved_x;
  win->y = win->saved_y;
  win->width = win->saved_width;
  win->height = win->saved_height;
  win->state = WINDOW_NORMAL;
  window_mark_surface_dirty_full(win);
}

static void window_apply_maximized_bounds(struct window *win) {
  if (!win)
    return;
  win->x = 0;
  win->y = MENU_BAR_HEIGHT;
  win->width = primary_display.width;
  win->height = primary_display.height - MENU_BAR_HEIGHT -
                dock_reserved_height() - WINDOW_BOTTOM_CLEARANCE;
  win->state = WINDOW_MAXIMIZED;
  window_mark_surface_dirty_full(win);
}

static void window_toggle_maximize(struct window *win) {
  int draw_x, draw_y, draw_w, draw_h;

  if (!win)
    return;

  window_get_draw_rect(win, &draw_x, &draw_y, &draw_w, &draw_h);
  compositor_mark_dirty(draw_x, draw_y, draw_w, draw_h);

  if (win->state == WINDOW_MAXIMIZED) {
    window_restore_saved_bounds(win);
  } else {
    win->saved_x = win->x;
    win->saved_y = win->y;
    win->saved_width = win->width;
    win->saved_height = win->height;
    window_apply_maximized_bounds(win);
  }

  window_get_draw_rect(win, &draw_x, &draw_y, &draw_w, &draw_h);
  compositor_mark_dirty(draw_x, draw_y, draw_w, draw_h);
}

static void window_minimize(struct window *win) {
  int draw_x, draw_y, draw_w, draw_h;

  if (!win)
    return;

  window_get_draw_rect(win, &draw_x, &draw_y, &draw_w, &draw_h);
  compositor_mark_dirty(draw_x, draw_y, draw_w, draw_h);
  gui_clear_chrome_visual_state_for_window(win);
  win->visible = false;
  win->state = WINDOW_MINIMIZED;
}

static void window_activate_chrome_hit(struct window *win, SkinHit hit) {
  if (!win)
    return;

  if (hit == SKIN_HIT_CLOSE) {
    if (!window_close_disabled(win))
      gui_destroy_window(win);
    return;
  }

  if (hit == SKIN_HIT_MINIMIZE) {
    if (!window_minimize_disabled(win))
      window_minimize(win);
    return;
  }

  if (hit == SKIN_HIT_MAXIMIZE) {
    window_toggle_maximize(win);
  }
}

static SkinRect gui_window_desktop_bounds(void) {
  SkinRect bounds;
  bounds.x = 0;
  bounds.y = MENU_BAR_HEIGHT;
  bounds.width = (int)primary_display.width;
  bounds.height = (int)primary_display.height - MENU_BAR_HEIGHT -
                  dock_reserved_height() - WINDOW_BOTTOM_CLEARANCE;
  if (bounds.height < 1)
    bounds.height = 1;
  return bounds;
}

static void window_begin_drag(struct window *win, int x, int y, SkinHit hit) {
  if (!win)
    return;

  dragging_window = win;
  drag_offset_x = x - win->x;
  drag_offset_y = y - win->y;
  chrome_drag_state.active = false;

  if (window_uses_newwindows_chrome(win)) {
    SkinWindow skin_window = gui_skin_window_from_window(win);
    skin_begin_drag(&chrome_drag_state, &skin_window, hit, x, y);
  }
}

static void window_update_drag(struct window *win, int x, int y) {
  int old_x, old_y, old_w, old_h;

  if (!win)
    return;

  window_get_draw_rect(win, &old_x, &old_y, &old_w, &old_h);

  if (win->state == WINDOW_MAXIMIZED) {
    window_restore_saved_bounds(win);
    drag_offset_x = win->width / 2;
    if (drag_offset_x < 40)
      drag_offset_x = 40;
    {
      int drag_limit = window_titlebar_drag_limit(win);
      if (drag_offset_y > drag_limit)
        drag_offset_y = drag_limit;
    }
    win->x = x - drag_offset_x;
    win->y = y - drag_offset_y;
    chrome_drag_state.active = false;
  } else if (window_uses_newwindows_chrome(win) && chrome_drag_state.active) {
    SkinWindow skin_window = gui_skin_window_from_window(win);
    SkinRect desktop_bounds = gui_window_desktop_bounds();
    skin_update_drag(&chrome_drag_state, &skin_window, x, y, desktop_bounds);
    win->x = skin_window.x;
    win->y = skin_window.y;
  } else {
    win->x = x - drag_offset_x;
    win->y = y - drag_offset_y;
  }

  if (win->y < MENU_BAR_HEIGHT)
    win->y = MENU_BAR_HEIGHT;
  {
    int drag_limit = window_titlebar_drag_limit(win);
    if (win->y > (int)primary_display.height - dock_reserved_height() -
                     drag_limit - WINDOW_BOTTOM_CLEARANCE)
      win->y = primary_display.height - dock_reserved_height() - drag_limit -
               WINDOW_BOTTOM_CLEARANCE;
  }
  if (win->x > (int)primary_display.width - 100)
    win->x = primary_display.width - 100;
  if (win->x < 0)
    win->x = 0;

  compositor_mark_dirty(old_x, old_y, old_w, old_h);
  window_get_draw_rect(win, &old_x, &old_y, &old_w, &old_h);
  compositor_mark_dirty(old_x, old_y, old_w, old_h);
}

void gui_handle_mouse_event(int x, int y, int buttons) {
  int old_mouse_x = mouse_x;
  int old_mouse_y = mouse_y;

  if (buttons < 0)
    buttons = 0;
  buttons &= 0x1F;

  int old_buttons = prev_buttons;
  mouse_x = x;
  mouse_y = y;
  prev_buttons = buttons;

  if (mouse_x != old_mouse_x || mouse_y != old_mouse_y) {
    gui_mark_cursor_dirty_at(old_mouse_x, old_mouse_y);
    gui_mark_cursor_dirty_at(mouse_x, mouse_y);
    if (menu_open) {
      main_menu_mark_dirty();
    } else if (dock_is_visible()) {
      int lx, ly, lw, lh;
      int was_on_launcher;
      int is_on_launcher;

      main_menu_launcher_button_rect(&lx, &ly, &lw, &lh);
      was_on_launcher =
          old_mouse_x >= lx && old_mouse_x < lx + lw && old_mouse_y >= ly &&
          old_mouse_y < ly + lh;
      is_on_launcher = mouse_x >= lx && mouse_x < lx + lw && mouse_y >= ly &&
                       mouse_y < ly + lh;
      if (was_on_launcher || is_on_launcher)
        main_menu_mark_dirty();
    }
    if (wifi_tray_open || wifi_tray_contains_point(old_mouse_x, old_mouse_y) ||
        wifi_tray_contains_point(mouse_x, mouse_y))
      wifi_tray_mark_dirty();
  }

  gui_update_newwindows_hover_state(x, y);

  int left_click = (buttons & 1) && !(old_buttons & 1); /* Just pressed */
  int left_held = (buttons & 1);
  int left_release = !(buttons & 1) && (old_buttons & 1);
  int right_click = (buttons & 2) && !(old_buttons & 2); /* Right button */

  if (startup_flow_active()) {
    if (desktop_is_context_menu_visible())
      desktop_hide_context_menu();

    if (left_click && startup_window) {
      int content_x = 0, content_y = 0, content_w = 0, content_h = 0;
      gui_get_window_content_rect(startup_window, &content_x, &content_y,
                                  &content_w, &content_h);

      gui_focus_window(startup_window);
      if (startup_setup_account_active()) {
        int button_x = 0, button_y = 0, button_w = 0, button_h = 0;
        int field_x = 0, field_y = 0, field_w = 0, field_h = 0;

        startup_get_setup_button_rect(content_x, content_y, content_w, content_h,
                                      &button_x, &button_y, &button_w,
                                      &button_h);
        if (startup_setup_account_form_active()) {
          startup_get_setup_field_rect(content_x, content_y, content_w,
                                       content_h, 0, &field_x, &field_y,
                                       &field_w, &field_h);
          if (x >= field_x && x < field_x + field_w && y >= field_y &&
              y < field_y + field_h) {
            startup_active_field = 0;
            return;
          }
          startup_get_setup_field_rect(content_x, content_y, content_w,
                                       content_h, 1, &field_x, &field_y,
                                       &field_w, &field_h);
          if (x >= field_x && x < field_x + field_w && y >= field_y &&
              y < field_y + field_h) {
            startup_active_field = 1;
            return;
          }
        }
        if (x >= button_x && x < button_x + button_w && y >= button_y &&
            y < button_y + button_h) {
          submit_startup_flow();
          return;
        }
        return;
      }
      if (startup_flow == STARTUP_FLOW_LOGIN && startup_login_user_dropdown_open) {
        settings_account_list_t accounts;
        int dropdown_rows = 0;
        int dropdown_start = 0;
        settings_account_list_init(&accounts);
        settings_collect_accounts(&accounts);
        if (accounts.count > 0 && x >= content_x + 20 &&
            x < content_x + content_w - 20 && y >= content_y + 118) {
          dropdown_rows = accounts.count < 4 ? accounts.count : 4;
          dropdown_start = settings_account_list_visible_start(
              &accounts, startup_input_username, dropdown_rows);
          for (int i = 0; i < dropdown_rows; i++) {
            int account_idx = dropdown_start + i;
            int row_y = content_y + 122 + i * 22;
            if (x >= content_x + 24 && x < content_x + content_w - 24 &&
                y >= row_y && y < row_y + 18) {
              str_copy_safe(startup_input_username, accounts.names[account_idx],
                            sizeof(startup_input_username));
              startup_login_user_dropdown_open = 0;
              startup_active_field = 1;
              settings_account_list_free(&accounts);
              return;
            }
          }
        }
        settings_account_list_free(&accounts);
      }
      if (x >= content_x + 20 && x < content_x + content_w - 20 &&
          y >= content_y + 94 && y < content_y + 128) {
        if (startup_flow == STARTUP_FLOW_LOGIN) {
          startup_login_user_dropdown_open = !startup_login_user_dropdown_open;
        }
        startup_active_field = 0;
        return;
      }
      {
        settings_account_list_t accounts;
        int startup_dropdown_rows = 0;
        int startup_login_extra_y = 0;
        settings_account_list_init(&accounts);
        if (startup_flow == STARTUP_FLOW_LOGIN && startup_login_user_dropdown_open) {
          settings_collect_accounts(&accounts);
          startup_dropdown_rows = accounts.count < 4 ? accounts.count : 4;
          if (startup_dropdown_rows > 0)
            startup_login_extra_y = 8 + startup_dropdown_rows * 22;
        }
        if (x >= content_x + 20 && x < content_x + content_w - 20 &&
            y >= content_y + 162 + startup_login_extra_y &&
            y < content_y + 196 + startup_login_extra_y) {
          startup_active_field = 1;
          startup_login_user_dropdown_open = 0;
          settings_account_list_free(&accounts);
          return;
        }
        if (x >= content_x + 20 && x < content_x + 190 &&
            y >= content_y + 214 + startup_login_extra_y &&
            y < content_y + 248 + startup_login_extra_y) {
          settings_account_list_free(&accounts);
          submit_startup_flow();
          return;
        }
        if (startup_flow == STARTUP_FLOW_LOGIN) {
          if (x >= content_x + 200 && x < content_x + 296 &&
              y >= content_y + 204 + startup_login_extra_y &&
              y < content_y + 238 + startup_login_extra_y) {
            startup_login_user_dropdown_open = 0;
            settings_account_list_free(&accounts);
            execute_secure_attention_action(SECURE_ACTION_RESTART);
            return;
          }
          if (x >= content_x + 306 && x < content_x + 414 &&
              y >= content_y + 204 + startup_login_extra_y &&
              y < content_y + 238 + startup_login_extra_y) {
            startup_login_user_dropdown_open = 0;
            settings_account_list_free(&accounts);
            execute_secure_attention_action(SECURE_ACTION_SHUTDOWN);
            return;
          }
        }
        settings_account_list_free(&accounts);
      }
      if (x >= content_x + 20 && x < content_x + content_w - 20 &&
          y >= content_y + 162 && y < content_y + 196) {
        startup_active_field = 1;
        return;
      }
      if (x >= content_x + 20 && x < content_x + 190 && y >= content_y + 214 &&
          y < content_y + 248) {
        submit_startup_flow();
        return;
      }
      startup_login_user_dropdown_open = 0;
    }
    return;
  }

  /* Handle context menu hover - ALWAYS call when menu visible */
  int menu_vis = desktop_is_context_menu_visible();
  if (desktop_session_active())
    desktop_handle_pointer_motion(x, y);
  if (menu_vis) {
    printk(KERN_INFO "MOUSE: Menu visible, calling hover at %d,%d\n", x, y);
    desktop_context_menu_hover(x, y);
  }

  /* Track for double-click detection */
  static int last_click_x = 0, last_click_y = 0;
  static int click_count = 0;

  if (secure_attention_open) {
    if (left_click) {
      int hit = secure_attention_button_hit(x, y);
      if (hit >= 0) {
        secure_attention_selection = hit;
        execute_secure_attention_action(hit);
      } else {
        execute_secure_attention_action(SECURE_ACTION_CANCEL);
      }
    }
    return;
  }

  /* Handle window dragging */
  if (dragging_window && left_held) {
    window_update_drag(dragging_window, x, y);
  }

  /* Handle window resizing */
  if (resizing_window && left_held) {
    int old_x, old_y, old_w, old_h;
    int dx = x - resize_start_x;
    int dy = y - resize_start_y;
    int new_w = resize_start_w;
    int new_h = resize_start_h;
    int new_x = resize_start_win_x;
    int new_y = resize_start_win_y;

    window_get_draw_rect(resizing_window, &old_x, &old_y, &old_w, &old_h);

    /* Calculate new dimensions based on which edge is being dragged */
    if (resize_edge == RESIZE_RIGHT || resize_edge == RESIZE_BOTTOM_RIGHT ||
        resize_edge == RESIZE_TOP_RIGHT) {
      new_w = resize_start_w + dx;
    }
    if (resize_edge == RESIZE_LEFT || resize_edge == RESIZE_BOTTOM_LEFT ||
        resize_edge == RESIZE_TOP_LEFT) {
      new_w = resize_start_w - dx;
      new_x = resize_start_win_x + dx;
    }
    if (resize_edge == RESIZE_BOTTOM || resize_edge == RESIZE_BOTTOM_RIGHT ||
        resize_edge == RESIZE_BOTTOM_LEFT) {
      new_h = resize_start_h + dy;
    }
    if (resize_edge == RESIZE_TOP || resize_edge == RESIZE_TOP_LEFT ||
        resize_edge == RESIZE_TOP_RIGHT) {
      new_h = resize_start_h - dy;
      new_y = resize_start_win_y + dy;
    }

    /* Clamp to screen */
    if (new_y < MENU_BAR_HEIGHT)
      new_y = MENU_BAR_HEIGHT;
    if (new_x < 0)
      new_x = 0;

    resizing_window->x = new_x;
    resizing_window->y = new_y;
    resizing_window->width = new_w;
    resizing_window->height = new_h;
    compositor_mark_dirty(old_x, old_y, old_w, old_h);
    window_get_draw_rect(resizing_window, &old_x, &old_y, &old_w, &old_h);
    compositor_mark_dirty(old_x, old_y, old_w, old_h);
  }

  if (left_release) {
    if (dragging_window) {
      snap_window_to_zone(dragging_window, x, y);
    }
    if (chrome_pressed_window) {
      struct window *released_window = chrome_pressed_window;
      SkinHit released_hit = chrome_pressed_hit;
      SkinHit current_hit = gui_newwindows_hit_test(released_window, x, y);
      gui_set_newwindows_pointer_state(
          current_hit == released_hit ? released_window : NULL,
          current_hit == released_hit ? current_hit : SKIN_HIT_NONE, NULL,
          SKIN_HIT_NONE);
      if (current_hit == released_hit)
        window_activate_chrome_hit(released_window, released_hit);
    }
    dragging_window = 0;
    chrome_drag_state.active = false;
    resizing_window = 0;
    resize_edge = RESIZE_NONE;
    if (desktop_session_active())
      desktop_release_drag(x, y);
  }

  if (desktop_session_active() && left_held && !dragging_window &&
      !resizing_window)
    desktop_update_drag(x, y, left_held);

  /* Handle desktop right-click (context menu) - check BEFORE left_click gate */
  if (desktop_session_active() && right_click) {
    printk(KERN_INFO "RIGHT-CLICK at %d,%d buttons=%d\n", x, y, buttons);
    /* Check if right-click is on desktop area (not on window, menu bar, or
     * dock) */
    int on_window = 0;
    for (struct window *win = window_stack; win; win = win->next) {
      if (!win->visible)
        continue;
      if (x >= win->x && x < win->x + win->width && y >= win->y &&
          y < win->y + win->height) {
        on_window = 1;
        break;
      }
    }

    if (!on_window && y > MENU_BAR_HEIGHT &&
        y < (int)primary_display.height - dock_reserved_height()) {
      printk(KERN_INFO
             "RIGHT-CLICK on desktop, calling desktop_handle_click\n");
      /* Right-click on desktop - handle in desktop manager */
      desktop_handle_click(x, y, 2, 0); /* button 2 = right */
      return;
    }
  }

  /* Handle desktop left-click for icon selection - check BEFORE window checks
   */
  if (desktop_session_active() && left_click) {
    /* Check context menu first */
    if (desktop_is_context_menu_visible()) {
      if (desktop_context_menu_click(x, y)) {
        return;
      }
    }

    /* Check menu bar dropdown BEFORE desktop icons (dropdown overlaps desktop
     * area) */
    if (menu_open == 1 && main_menu_contains_point(x, y)) {
      if (main_menu_activate(main_menu_item_at(x, y))) {
        return;
      }
      main_menu_mark_dirty();
      menu_open = 0;
      main_menu_power_open = 0;
      main_menu_mark_dirty();
      return;
    }

    if (wifi_tray_open && !wifi_tray_contains_point(x, y)) {
      wifi_tray_mark_dirty();
      wifi_tray_open = 0;
      wifi_tray_password_active = 0;
      wifi_tray_mark_dirty();
    }

    if (y < MENU_BAR_HEIGHT) {
      main_menu_mark_dirty();
      menu_open = 0;
      main_menu_power_open = 0;
      main_menu_mark_dirty();
      return;
    }

    if (dock_handle_click(x, y)) {
      return;
    }

    /* Check if click is on desktop area (not on window) */
    int on_window = 0;
    for (struct window *win = window_stack; win; win = win->next) {
      if (!win->visible)
        continue;
      if (x >= win->x && x < win->x + win->width && y >= win->y &&
          y < win->y + win->height) {
        on_window = 1;
        break;
      }
    }

    if (!on_window && y > MENU_BAR_HEIGHT &&
        y < (int)primary_display.height - dock_reserved_height()) {
      gui_clear_focus();

      /* Track double-click */
      int dx = x - last_click_x;
      int dy = y - last_click_y;
      if (dx < 0)
        dx = -dx;
      if (dy < 0)
        dy = -dy;

      if (dx < 10 && dy < 10) {
        click_count++;
        if (click_count >= 2) {
          /* Double click - open item */
          desktop_handle_double_click(x, y);
          click_count = 0;
          return;
        }
      } else {
        click_count = 1;
      }
      last_click_x = x;
      last_click_y = y;

      /* Single click - select icon */
      int shift_held = 0;
      if (desktop_handle_click(x, y, 1, shift_held)) {
        return; /* Click was on desktop icon */
      }
    }
  }

  /* Check if clicking on a window */
  if (!left_click)
    return;

  /* Check menu bar and dropdown clicks */
  if (y < MENU_BAR_HEIGHT || (menu_open && main_menu_contains_point(x, y))) {

    printk("MENU DEBUG: x=%d y=%d menu_open=%d MBH=%d\\n", x, y, menu_open,
           MENU_BAR_HEIGHT);

    /* If dropdown is open, check dropdown item clicks */
    if (menu_open == 1 && main_menu_contains_point(x, y)) {
      if (main_menu_activate(main_menu_item_at(x, y))) {
        return;
      }
      main_menu_mark_dirty();
      menu_open = 0;
      main_menu_power_open = 0;
      main_menu_mark_dirty();
      return;
    }

    /* Menu bar clicks */
    if (y < MENU_BAR_HEIGHT) {
      main_menu_mark_dirty();
      menu_open = 0;
      main_menu_power_open = 0;
      main_menu_mark_dirty();
      return;
    }
  }

  /* Close menu if clicking elsewhere */
  if (menu_open) {
    main_menu_mark_dirty();
    menu_open = 0;
    main_menu_power_open = 0;
    main_menu_mark_dirty();
  }

  for (struct window *win = window_stack; win; win = win->next) {
    int draw_x, draw_y, draw_w, draw_h;
    if (!win->visible)
      continue;
    window_get_draw_rect(win, &draw_x, &draw_y, &draw_w, &draw_h);

    if (x >= draw_x && x < draw_x + draw_w && y >= draw_y &&
        y < draw_y + draw_h) {
      compositor_mark_dirty(draw_x, draw_y, draw_w, draw_h);

      gui_focus_window(win);
      window_get_draw_rect(win, &draw_x, &draw_y, &draw_w, &draw_h);
      compositor_mark_dirty(draw_x, draw_y, draw_w, draw_h);

      if (win->animation != WINDOW_ANIM_NONE)
        return;

      if (window_uses_newwindows_chrome(win)) {
        SkinHit hit = gui_newwindows_hit_test(win, x, y);
        int edge = gui_resize_edge_from_skin_hit(hit);

        if (edge != RESIZE_NONE) {
          window_begin_resize(win, edge, x, y);
          return;
        }

        if (hit == SKIN_HIT_CLOSE) {
          gui_set_newwindows_pointer_state(win, hit, win, hit);
          return;
        }

        if (hit == SKIN_HIT_MINIMIZE) {
          gui_set_newwindows_pointer_state(win, hit, win, hit);
          return;
        }

        if (hit == SKIN_HIT_MAXIMIZE) {
          gui_set_newwindows_pointer_state(win, hit, win, hit);
          return;
        }

        if (hit == SKIN_HIT_TITLEBAR) {
          gui_set_newwindows_pointer_state(win, hit, NULL, SKIN_HIT_NONE);
          window_begin_drag(win, x, y, hit);
          return;
        }
      }

      /* Handle clicks inside Calculator window */
      if (win->title[0] == 'C' && win->title[1] == 'a' &&
          win->title[2] == 'l') {
        /* Calculate content area */
        int content_x = 0;
        int content_y = 0;
        int content_w = 0;
        int content_h = 0;
        gui_get_window_content_rect(win, &content_x, &content_y, &content_w,
                                    &content_h);

        /* Button layout - 5x4 grid matching render */
        static const char btns[5][4] = {
            {'C', 'N', '%', '/'}, /* N = +/- (negate) */
            {'7', '8', '9', '*'},
            {'4', '5', '6', '-'},
            {'1', '2', '3', '+'},
            {'0', '0', '.', '='}};

        int disp_h = 70;
        int grid_x = content_x + 12;
        int grid_y = content_y + disp_h + 20;
        int grid_w = content_w - 24;
        int grid_h = content_h - disp_h - 32;
        int bw = (grid_w - 12) / 4;
        int bh = (grid_h - 16) / 5;
        int gap = 4;

        /* Check if click is in button area */
        if (x >= grid_x && y >= grid_y) {
          int col = (x - grid_x) / (bw + gap);
          int row = (y - grid_y) / (bh + gap);
          if (row >= 0 && row < 5 && col >= 0 && col < 4) {
            char btn = btns[row][col];
            /* Handle special buttons */
            if (btn == 'N') {
              /* +/- button - negate display */
              calc_display = -calc_display;
            } else if (btn == '%') {
              /* Percent - divide by 100 */
              calc_display = calc_display / 100;
            } else if (btn == '.') {
              /* Decimal point - ignore for integers */
            } else {
              calc_button_click(btn);
            }
          }
        }
        break;
      }

      /* Handle clicks inside Background Settings window */
      if (win->title[0] == 'B' && win->title[1] == 'a' &&
          win->title[2] == 'c') {
        int content_x = 0;
        int content_y = 0;
        gui_get_window_content_rect(win, &content_x, &content_y, NULL, NULL);

        /* Wallpaper grid layout (matching render) */
        int header_h = 42;
        int card_x = content_x + 12;
        int card_y = content_y + header_h + 12;
        int grid_x = card_x + 14;
        int grid_y = card_y + 40;
        int thumb_w = 60;
        int thumb_h = 40;
        int gap_x = 10;
        int gap_y = 18;
        int columns = 3;

        for (int i = 0; i < NUM_WALLPAPERS; i++) {
          int col = i % columns;
          int row = i / columns;
          int tx = grid_x + col * (thumb_w + gap_x);
          int ty = grid_y + row * (thumb_h + gap_y + 18);

          if (x >= tx - 2 && x < tx + thumb_w + 2 && y >= ty - 2 &&
              y < ty + thumb_h + 20) {
            apply_account_wallpaper(i);
            account_wallpaper = current_wallpaper;
            if (account_username[0] && account_password[0])
              save_account_state();
            break;
          }
        }
        break;
      }

      /* Handle clicks inside Settings window */
      if (win->title[0] == 'S' && win->title[1] == 'e' &&
          win->title[2] == 't') {
        int content_x = 0;
        int content_y = 0;
        int content_w = 0;
        gui_get_window_content_rect(win, &content_x, &content_y, &content_w,
                                    NULL);
        int sidebar_w = 132;
        int panel_w = content_w - sidebar_w - 3;
        int panel_x = content_x + sidebar_w + 3;
        int panel_y = content_y + 42;
        int toolbar_y = content_y + 8;
        int toolbar_h = 24;
        int toolbar_x = content_x + 154;
        int toolbar_gap = 8;

        if (x >= toolbar_x && x < toolbar_x + 98 && y >= toolbar_y &&
            y < toolbar_y + toolbar_h) {
          gui_create_window("Background Settings", win->x + 18, win->y + 18,
                            460, 360);
          str_copy_safe(settings_status, "Opened background settings.",
                        sizeof(settings_status));
          break;
        }
        if (x >= toolbar_x + 98 + toolbar_gap &&
            x < toolbar_x + 98 + toolbar_gap + 88 && y >= toolbar_y &&
            y < toolbar_y + toolbar_h) {
          settings_active_tab = 11;
          str_copy_safe(settings_status, settings_default_status_message(11),
                        sizeof(settings_status));
          break;
        }
        if (x >= toolbar_x + 194 + toolbar_gap * 2 &&
            x < toolbar_x + 194 + toolbar_gap * 2 + 66 && y >= toolbar_y &&
            y < toolbar_y + toolbar_h) {
          gui_create_file_manager_path(win->x + 26, win->y + 26, "/");
          str_copy_safe(settings_status, "Opened file manager.",
                        sizeof(settings_status));
          break;
        }
        if (x >= toolbar_x + 268 + toolbar_gap * 3 &&
            x < toolbar_x + 268 + toolbar_gap * 3 + 82 && y >= toolbar_y &&
            y < toolbar_y + toolbar_h) {
          settings_active_tab = 10;
          str_copy_safe(settings_status, settings_default_status_message(10),
                        sizeof(settings_status));
          break;
        }
        if (x >= toolbar_x + 358 + toolbar_gap * 4 &&
            x < toolbar_x + 358 + toolbar_gap * 4 + 72 && y >= toolbar_y &&
            y < toolbar_y + toolbar_h) {
          gui_create_window("About", 210, 140, 560, 360);
          str_copy_safe(settings_status, "Opened about window.",
                        sizeof(settings_status));
          break;
        }

        for (int i = 0; i < SETTINGS_MENU_COUNT; i++) {
          int tab_y = content_y + 56 + i * 28;
          if (x >= content_x + 10 && x < content_x + sidebar_w - 10 &&
              y >= tab_y && y < tab_y + 18) {
            settings_active_tab = i;
            settings_wifi_password_active = 0;
            str_copy_safe(settings_status, settings_default_status_message(i),
                          sizeof(settings_status));
            break;
          }
        }

        if (settings_active_tab == 0) {
          int row_y = panel_y + 72 + 84 + 88;
          if (x >= panel_x && x < panel_x + 108 && y >= row_y && y < row_y + 30) {
            gui_create_window("Background Settings", win->x + 18, win->y + 18, 460,
                              360);
            str_copy_safe(settings_status, "Opened background settings.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 118 && x < panel_x + 216 && y >= row_y &&
              y < row_y + 30) {
            gui_create_window("App Store", win->x + 28, win->y + 28, 540, 420);
            str_copy_safe(settings_status, "Opened the app store.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 226 && x < panel_x + 318 && y >= row_y &&
              y < row_y + 30) {
            gui_create_window("Device Manager", win->x + 40, win->y + 40, 460,
                              360);
            str_copy_safe(settings_status, "Opened device manager.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 328 && x < panel_x + 412 && y >= row_y &&
              y < row_y + 30) {
            gui_create_window("About", 210, 140, 560, 360);
            str_copy_safe(settings_status, "Opened about window.",
                          sizeof(settings_status));
            break;
          }
        } else if (settings_active_tab == 1) {
          int button_y = panel_y + 118;
          int password_y = panel_y + 154;
          int list_y = panel_y + 196;
          int network_count = wifi_get_network_count();
          int selected_network = wifi_get_selected_network();
          int selected_secure =
              selected_network >= 0 ? wifi_get_network_secure(selected_network) : 0;

          if (x >= panel_x && x < panel_x + 88 && y >= button_y &&
              y < button_y + 28) {
            if (wifi_has_supported_adapter() && wifi_supports_real_scanning())
              wifi_scan();
            settings_wifi_password_active = 0;
            str_copy_safe(settings_status, wifi_get_status_text(),
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 98 && x < panel_x + 200 && y >= button_y &&
              y < button_y + 28) {
            if (wifi_can_connect_selected())
              wifi_connect_selected(wifi_password_draft);
            settings_wifi_password_active = 0;
            str_copy_safe(settings_status, wifi_get_status_text(),
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 210 && x < panel_x + 328 && y >= button_y &&
              y < button_y + 28) {
            wifi_disconnect();
            settings_wifi_password_active = 0;
            str_copy_safe(settings_status, wifi_get_status_text(),
                          sizeof(settings_status));
            break;
          }

          if (selected_secure && x >= panel_x && x < panel_x + panel_w - 18 &&
              y >= password_y && y < password_y + 28) {
            settings_wifi_password_active = 1;
            break;
          }
          settings_wifi_password_active = 0;

          for (int i = 0; i < network_count; i++) {
            int row_y = list_y + i * 36;
            if (x >= panel_x && x < panel_x + panel_w - 18 && y >= row_y &&
                y < row_y + 30) {
              wifi_select_network(i);
              if (!wifi_get_network_secure(i))
                wifi_password_draft[0] = '\0';
              str_copy_safe(settings_status, wifi_get_status_text(),
                            sizeof(settings_status));
              break;
            }
          }
        } else if (settings_active_tab == 2) {
          int row_y = panel_y + 126;
          if (x >= panel_x + 18 && x < panel_x + 128 && y >= row_y &&
              y < row_y + 30) {
            gui_create_window("Device Manager", win->x + 40, win->y + 40, 460,
                              360);
            str_copy_safe(settings_status, "Opened device manager.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 138 && x < panel_x + 234 && y >= row_y &&
              y < row_y + 30) {
            gui_create_file_manager_path(win->x + 26, win->y + 26, "/");
            str_copy_safe(settings_status, "Opened file manager.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 242 && x < panel_x + 354 && y >= row_y &&
              y < row_y + 30) {
            gui_create_window("Disk Imager", win->x + 34, win->y + 34, 620, 440);
            str_copy_safe(settings_status, "Opened disk imager.",
                          sizeof(settings_status));
            break;
          }
        } else if (settings_active_tab == 3) {
          int theme_button_y = panel_y + 72 + 104 + 72;
          int resolution_card_y = panel_y + 72 + 104 + 116;
          int button_y = resolution_card_y + 66;
          int picked_resolution = 0;

          settings_sync_resolution_picker();
          for (int i = 0; i < SETTINGS_RESOLUTION_OPTION_COUNT; i++) {
            int bx, by, bw, bh;
            settings_resolution_button_bounds(panel_x, panel_y, i, &bx, &by, &bw,
                                              &bh);
            if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
              settings_resolution_pending_idx = i;
              str_copy_safe(settings_status, "Resolution preset selected.",
                            sizeof(settings_status));
              picked_resolution = 1;
              break;
            }
          }
          if (picked_resolution)
            break;

          if (x >= panel_x + 16 && x < panel_x + 126 && y >= theme_button_y &&
              y < theme_button_y + 22) {
            gui_set_theme_mode(GUI_THEME_LIGHT);
            gui_save_theme_preference();
            str_copy_safe(settings_status, "Light mode applied.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 136 && x < panel_x + 246 && y >= theme_button_y &&
              y < theme_button_y + 22) {
            gui_set_theme_mode(GUI_THEME_DARK);
            gui_save_theme_preference();
            str_copy_safe(settings_status, "Dark mode applied.",
                          sizeof(settings_status));
            break;
          }

          if (x >= panel_x + 8 && x < panel_x + 98 && y >= button_y &&
              y < button_y + 24) {
            gui_create_window("Background Settings", win->x + 18, win->y + 18, 460,
                              360);
            str_copy_safe(settings_status, "Pick a new wallpaper.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 106 && x < panel_x + 196 && y >= button_y &&
              y < button_y + 24) {
            int save_idx = settings_resolution_saved_idx;
            if (save_idx < 0 || save_idx >= SETTINGS_RESOLUTION_OPTION_COUNT) {
              save_idx = settings_resolution_current_idx;
              if (save_idx < 0 || save_idx >= SETTINGS_RESOLUTION_OPTION_COUNT)
                save_idx = settings_resolution_pending_idx;
            }
            gui_cycle_blur_effects_mode();
            gui_refresh_blur_effects_policy();
            if (save_idx >= 0 && save_idx < SETTINGS_RESOLUTION_OPTION_COUNT) {
              gui_save_resolution_preference(
                  settings_resolution_options[save_idx].width,
                  settings_resolution_options[save_idx].height);
            }
            str_copy_safe(settings_status, gui_blur_effects_detail_message(),
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 204 && x < panel_x + 294 && y >= button_y &&
              y < button_y + 24) {
            gui_configure_gpu_rendering(!gui_is_gpu_rendering_enabled());
            str_copy_safe(settings_status,
                          gui_is_gpu_rendering_enabled() ? "GPU rendering enabled."
                                                         : "Software renderer enabled.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 302 && x < panel_x + 392 && y >= button_y &&
              y < button_y + 24) {
            const settings_resolution_option_t *opt =
                &settings_resolution_options[settings_resolution_pending_idx];
            gui_save_resolution_preference(opt->width, opt->height);
            settings_resolution_saved_idx = settings_resolution_pending_idx;
            if (settings_resolution_pending_idx == settings_resolution_current_idx) {
              str_copy_safe(settings_status,
                            "Resolution saved as the boot default.",
                            sizeof(settings_status));
            } else if (!gui_can_apply_resolution_live()) {
              str_copy_safe(settings_status,
                            "Live resolution switching is unavailable here. Saved for reboot.",
                            sizeof(settings_status));
            } else if (gui_set_resolution(opt->width, opt->height) == 0) {
              settings_sync_resolution_picker();
              str_copy_safe(settings_status,
                            "Resolution changed now and saved for reboot.",
                            sizeof(settings_status));
            } else {
              str_copy_safe(settings_status,
                            "Resolution saved. Restart OS8 to apply it.",
                            sizeof(settings_status));
            }
            break;
          }
          if (x >= panel_x + 400 && x < panel_x + 496 && y >= button_y &&
              y < button_y + 24) {
            const settings_resolution_option_t *opt =
                &settings_resolution_options[settings_resolution_pending_idx];
            gui_save_resolution_preference(opt->width, opt->height);
            settings_resolution_saved_idx = settings_resolution_pending_idx;
            str_copy_safe(settings_status,
                          "Resolution saved for the next reboot.",
                          sizeof(settings_status));
            break;
          }
        } else if (settings_active_tab == 4) {
          int row_y = panel_y + 72 + 96;
          if (x >= panel_x + 16 && x < panel_x + 126 && y >= row_y &&
              y < row_y + 30) {
            gui_create_window("App Store", win->x + 28, win->y + 28, 540, 420);
            str_copy_safe(settings_status, "Opened the app store.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 136 && x < panel_x + 232 && y >= row_y &&
              y < row_y + 30) {
            gui_create_file_manager_path(win->x + 26, win->y + 26, "/");
            str_copy_safe(settings_status, "Opened file manager.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 242 && x < panel_x + 350 && y >= row_y &&
              y < row_y + 30) {
            gui_create_window("Device Manager", win->x + 40, win->y + 40, 460,
                              360);
            str_copy_safe(settings_status, "Opened device manager.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 360 && x < panel_x + 478 && y >= row_y &&
              y < row_y + 30) {
            gui_create_window("Disk Imager", win->x + 34, win->y + 34, 620,
                              440);
            str_copy_safe(settings_status, "Opened disk imager.",
                          sizeof(settings_status));
            break;
          }
        } else if (settings_active_tab == 8) {
          int row1_y = panel_y + 72 + 90;
          int row2_y = row1_y + 42;
          int sidebar_y = row2_y + 46;
          if (x >= panel_x && x < panel_x + 110 && y >= row1_y && y < row1_y + 30) {
            gui_create_window("Device Manager", win->x + 40, win->y + 40, 460,
                              360);
            str_copy_safe(settings_status, "Opened device manager.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 120 && x < panel_x + 230 && y >= row1_y &&
              y < row1_y + 30) {
            gui_create_file_manager_path(win->x + 26, win->y + 26, "/");
            str_copy_safe(settings_status, "Opened file manager.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 240 && x < panel_x + 350 && y >= row1_y &&
              y < row1_y + 30) {
            gui_create_window("App Store", win->x + 28, win->y + 28, 540, 420);
            str_copy_safe(settings_status, "Opened the app store.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 360 && x < panel_x + 470 && y >= row1_y &&
              y < row1_y + 30) {
            dock_item_count = 0;
            dock_add_all_system_apps();
            save_dock_config();
            str_copy_safe(settings_status, "Dock reset to system defaults.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x && x < panel_x + 110 && y >= row2_y &&
              y < row2_y + 30) {
            gui_create_window("About", 210, 140, 560, 360);
            str_copy_safe(settings_status, "Opened about window.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 16 && x < panel_x + 98 && y >= sidebar_y + 52 &&
              y < sidebar_y + 76) {
            desktop_sidebar_set_visible(!desktop_sidebar_is_visible());
            str_copy_safe(settings_status,
                          desktop_sidebar_is_visible() ? "Sidebar shown."
                                                       : "Sidebar hidden.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 108 && x < panel_x + 180 && y >= sidebar_y + 52 &&
              y < sidebar_y + 76) {
            desktop_sidebar_set_side(DESKTOP_SIDEBAR_LEFT);
            str_copy_safe(settings_status, "Sidebar pinned to the left.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 190 && x < panel_x + 262 && y >= sidebar_y + 52 &&
              y < sidebar_y + 76) {
            desktop_sidebar_set_side(DESKTOP_SIDEBAR_RIGHT);
            str_copy_safe(settings_status, "Sidebar pinned to the right.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 272 && x < panel_x + 362 && y >= sidebar_y + 52 &&
              y < sidebar_y + 76) {
            desktop_sidebar_set_width(desktop_sidebar_get_width() - 16);
            str_copy_safe(settings_status, "Sidebar width reduced.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 372 && x < panel_x + 446 && y >= sidebar_y + 52 &&
              y < sidebar_y + 76) {
            desktop_sidebar_set_width(desktop_sidebar_get_width() + 16);
            str_copy_safe(settings_status, "Sidebar width increased.",
                          sizeof(settings_status));
            break;
          }
        } else if (settings_active_tab == 5) {
          settings_account_list_t accounts;
          int signout_y = panel_y + 72 + 88 + 102;
          int list_y = signout_y + 44;
          int list_start = 0;
          int visible_rows = 0;
          int editor_x = panel_x + 224;
          int editor_w = panel_w - 224;
          int admin_mode = account_role_is_admin();
          int handled_selection = 0;

          settings_account_list_init(&accounts);
          settings_collect_accounts(&accounts);
          settings_sync_selected_user();
          visible_rows = accounts.count < 4 ? accounts.count : 4;
          list_start = settings_account_list_visible_start(&accounts,
                                                           settings_user_selected,
                                                           visible_rows);
          if (session_can_logout() && x >= panel_x && x < panel_x + 112 &&
              y >= signout_y && y < signout_y + 30) {
            str_copy_safe(startup_input_username, account_username,
                          sizeof(startup_input_username));
            startup_begin_login_flow("Signed out.", 1);
            str_copy_safe(settings_status, "Returned to the login screen.",
                          sizeof(settings_status));
            settings_account_list_free(&accounts);
            break;
          }
          for (int i = 0; i < visible_rows; i++) {
            int account_idx = list_start + i;
            int row_y = list_y + 34 + i * 22;
            if (x >= panel_x + 12 && x < panel_x + 198 && y >= row_y &&
                y < row_y + 18) {
              str_copy_safe(settings_user_selected, accounts.names[account_idx],
                            sizeof(settings_user_selected));
              str_copy_safe(settings_status, "Selected user account.",
                            sizeof(settings_status));
              handled_selection = 1;
              break;
            }
          }
          if (handled_selection) {
            settings_account_list_free(&accounts);
            break;
          }
          if (x >= editor_x + 16 && x < editor_x + editor_w - 16 &&
              y >= list_y + 34 && y < list_y + 58) {
            if (admin_mode)
              settings_user_active_field = 0;
            else
              str_copy_safe(settings_status,
                            "Admin mode is required to edit account details.",
                            sizeof(settings_status));
            settings_account_list_free(&accounts);
            break;
          }
          if (x >= editor_x + 16 && x < editor_x + editor_w - 16 &&
              y >= list_y + 66 && y < list_y + 90) {
            if (admin_mode)
              settings_user_active_field = 1;
            else
              str_copy_safe(settings_status,
                            "Admin mode is required to edit account details.",
                            sizeof(settings_status));
            settings_account_list_free(&accounts);
            break;
          }
          if (x >= editor_x + 16 && x < editor_x + 126 &&
              y >= list_y + 98 && y < list_y + 124) {
            if (admin_mode) {
              settings_user_new_role_idx =
                  (settings_user_new_role_idx + 1) % SETTINGS_USER_ROLE_COUNT;
              str_copy_safe(settings_status, "User mode updated.",
                            sizeof(settings_status));
            } else {
              str_copy_safe(settings_status,
                            "Admin mode is required to change user modes.",
                            sizeof(settings_status));
            }
            settings_account_list_free(&accounts);
            break;
          }
          if (x >= editor_x + 136 && x < editor_x + 228 && y >= list_y + 100 &&
              y < list_y + 124) {
            if (admin_mode)
              settings_add_user_account();
            else
              str_copy_safe(settings_status,
                            "Admin mode is required to add users.",
                            sizeof(settings_status));
            settings_account_list_free(&accounts);
            break;
          }
          if (x >= editor_x + 236 && x < editor_x + 344 &&
              y >= list_y + 100 && y < list_y + 124) {
            if (admin_mode)
              settings_remove_selected_user_account();
            else
              str_copy_safe(settings_status,
                            "Admin mode is required to remove users.",
                            sizeof(settings_status));
            settings_account_list_free(&accounts);
            break;
          }
          settings_account_list_free(&accounts);
        } else if (settings_active_tab == 6) {
          int block_y = panel_y + 72 + 98;
          if (x >= panel_x + 16 && x < panel_x + 124 && y >= block_y &&
              y < block_y + 28) {
            gui_create_window("Background Settings", win->x + 18, win->y + 18,
                              460, 360);
            str_copy_safe(settings_status, "Opened background settings.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 134 && x < panel_x + 226 && y >= block_y &&
              y < block_y + 28) {
            gui_set_theme_mode(GUI_THEME_LIGHT);
            gui_save_theme_preference();
            str_copy_safe(settings_status, "Light mode applied.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 236 && x < panel_x + 328 && y >= block_y &&
              y < block_y + 28) {
            gui_set_theme_mode(GUI_THEME_DARK);
            gui_save_theme_preference();
            str_copy_safe(settings_status, "Dark mode applied.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 338 && x < panel_x + 448 && y >= block_y &&
              y < block_y + 28) {
            settings_active_tab = 11;
            str_copy_safe(settings_status, settings_default_status_message(11),
                          sizeof(settings_status));
            break;
          }
        } else if (settings_active_tab == 9) {
          if (x >= panel_x + 18 && x < panel_x + 106 && y >= panel_y + 146 &&
              y < panel_y + 176) {
            gui_create_window("About", 210, 140, 560, 360);
            str_copy_safe(settings_status, "Opened about window.",
                          sizeof(settings_status));
            break;
          }
        } else if (settings_active_tab == 10) {
          int sidebar_y = panel_y + 72 + 96;
          if (x >= panel_x + 16 && x < panel_x + 98 && y >= sidebar_y &&
              y < sidebar_y + 24) {
            desktop_sidebar_set_visible(!desktop_sidebar_is_visible());
            str_copy_safe(settings_status,
                          desktop_sidebar_is_visible() ? "Sidebar shown."
                                                       : "Sidebar hidden.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 108 && x < panel_x + 180 && y >= sidebar_y &&
              y < sidebar_y + 24) {
            desktop_sidebar_set_side(DESKTOP_SIDEBAR_LEFT);
            str_copy_safe(settings_status, "Sidebar pinned to the left.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 190 && x < panel_x + 262 && y >= sidebar_y &&
              y < sidebar_y + 24) {
            desktop_sidebar_set_side(DESKTOP_SIDEBAR_RIGHT);
            str_copy_safe(settings_status, "Sidebar pinned to the right.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 272 && x < panel_x + 362 && y >= sidebar_y &&
              y < sidebar_y + 24) {
            desktop_sidebar_set_width(desktop_sidebar_get_width() - 16);
            str_copy_safe(settings_status, "Sidebar width reduced.",
                          sizeof(settings_status));
            break;
          }
          if (x >= panel_x + 372 && x < panel_x + 446 && y >= sidebar_y &&
              y < sidebar_y + 24) {
            desktop_sidebar_set_width(desktop_sidebar_get_width() + 16);
            str_copy_safe(settings_status, "Sidebar width increased.",
                          sizeof(settings_status));
            break;
          }
        } else if (settings_active_tab == 11) {
          int builder_x = panel_x + 12;
          int builder_y = panel_y + 112;
          int builder_w = panel_w - 28;
          int preview_x = builder_x + 14;
          int preview_y = builder_y + 14;
          int preview_w = builder_w - 28;
          int preview_h = 102;
          int slots_x = builder_x + 14;
          int slots_y = preview_y + preview_h + 14;
          int slot_gap = 8;
          int slot_w = (builder_w - 28 - slot_gap * 3) / 4;
          int slot_h = 40;
          int chips_x = builder_x + 14;
          int chips_y = slots_y + ((slot_h + slot_gap) * 3) + 14;
          int chip_gap = 8;
          int chip_w = 32;
          int chip_h = 26;
          int chip_cols = 6;

          if (x >= preview_x + preview_w - 196 &&
              x < preview_x + preview_w - 144 && y >= preview_y + 12 &&
              y < preview_y + 38) {
            gui_set_theme_mode(GUI_THEME_LIGHT);
            str_copy_safe(settings_status, "Theme lab switched to light mode.",
                          sizeof(settings_status));
            break;
          }
          if (x >= preview_x + preview_w - 136 &&
              x < preview_x + preview_w - 84 && y >= preview_y + 12 &&
              y < preview_y + 38) {
            gui_set_theme_mode(GUI_THEME_DARK);
            str_copy_safe(settings_status, "Theme lab switched to dark mode.",
                          sizeof(settings_status));
            break;
          }
          if (x >= preview_x + preview_w - 74 &&
              x < preview_x + preview_w - 14 && y >= preview_y + 12 &&
              y < preview_y + 38) {
            theme_builder_save_current_theme();
            break;
          }
          for (int i = 0; i < SETTINGS_THEME_SLOT_COUNT; i++) {
            int col = i % 4;
            int row = i / 4;
            int sx = slots_x + col * (slot_w + slot_gap);
            int sy = slots_y + row * (slot_h + slot_gap);
            if (x >= sx && x < sx + slot_w && y >= sy && y < sy + slot_h) {
              settings_theme_active_slot = i;
              str_copy_safe(settings_status, "Theme slot selected.",
                            sizeof(settings_status));
              break;
            }
          }
          for (int i = 0; i < SETTINGS_THEME_CHIP_COUNT; i++) {
            int col = i % chip_cols;
            int row = i / chip_cols;
            int cx = chips_x + col * (chip_w + chip_gap);
            int cy = chips_y + 28 + row * (chip_h + chip_gap);
            if (x >= cx && x < cx + chip_w && y >= cy && y < cy + chip_h) {
              theme_builder_apply_slot_color(settings_theme_active_slot,
                                             settings_theme_chips[i]);
              str_copy_safe(settings_status, "Theme slot color updated.",
                            sizeof(settings_status));
              break;
            }
          }
        }
      }

      /* Handle clicks inside App Store window */
      if (win->title[0] == 'A' && win->title[1] == 'p' &&
          win->title[2] == 'p' && win->title[3] == ' ') {
        int content_x = 0;
        int content_y = 0;
        int content_w = 0;
        gui_get_window_content_rect(win, &content_x, &content_y, &content_w,
                                    NULL);
        int y_row = content_y + 12 + 18 + 24;

        for (int i = 0; i < app_catalog_count; i++) {
          const dock_app_def_t *app = &app_catalog[i];
          if (!app->visible_in_store)
            continue;

          int installed = app_is_installed(app);
          int button_w = installed ? 72 : 88;
          int button_x = content_x + content_w - button_w - 18;
          int button_y = y_row + 13;

          if (x >= button_x && x < button_x + button_w && y >= button_y &&
              y < button_y + 28) {
            if (!installed) {
              install_app(app, 1);
            }
            gui_launch_app_by_id(app->id);
            return;
          }

          y_row += APP_STORE_CARD_HEIGHT + 8;
        }
      }

      if (win->title[0] == 'I' && win->title[1] == 'n' &&
          win->title[2] == 's' && win->title[3] == 't') {
        int content_x = 0;
        int content_y = 0;
        int content_w = 0;
        int content_h = 0;
        gui_get_window_content_rect(win, &content_x, &content_y, &content_w,
                                    &content_h);
        int panel_x = content_x + 20;
        int panel_y = content_y + 18;
        int panel_w = content_w - 40;
        int panel_h = content_h - 36;
        int rail_w = 180;
        int body_x = panel_x + rail_w + 18;
        int body_y = panel_y + 18;
        int body_w = panel_w - rail_w - 36;
        int footer_y = panel_y + panel_h - 54;
        int primary_x = body_x;
        int secondary_x = body_x + 154;
        int utility_x = body_x + body_w - 166;
        int button_y = footer_y + 12;
        int button_w = 140;
        int button_h = 34;

        if (!installer_active && !installer_show_restart_screen)
          installer_refresh_disk_inventory();

        if (installer_show_restart_screen)
          installer_page = INSTALLER_PAGE_COMPLETE;
        else if (installer_active)
          installer_page = INSTALLER_PAGE_PROGRESS;

        if (installer_page == INSTALLER_PAGE_COMPLETE) {
          if (x >= primary_x && x < primary_x + button_w && y >= button_y &&
              y < button_y + button_h) {
            installer_reboot_deadline_ms = 0;
            {
              extern void arch_reboot(void);
              gui_flush_account_state_before_power_transition();
              arch_reboot();
            }
            return;
          }
          return;
        }

        if (installer_page == INSTALLER_PAGE_TARGET) {
          for (int i = 0; i < installer_disk_count && i < 5; i++) {
            int row_y = body_y + 96 + i * 34;
            if (x >= body_x && x < body_x + body_w - 18 && y >= row_y &&
                y < row_y + 28) {
              installer_selected_disk = i;
              installer_set_status("Installer target disk updated.");
              return;
            }
          }
        }

        if (installer_page > INSTALLER_PAGE_WELCOME &&
            installer_page < INSTALLER_PAGE_PROGRESS &&
            x >= primary_x && x < primary_x + button_w && y >= button_y &&
            y < button_y + button_h) {
          installer_page--;
          return;
        }

        if (installer_page == INSTALLER_PAGE_WELCOME &&
            x >= secondary_x && x < secondary_x + button_w && y >= button_y &&
            y < button_y + button_h) {
          installer_page = INSTALLER_PAGE_TARGET;
          return;
        }

        if (installer_page == INSTALLER_PAGE_TARGET &&
            x >= secondary_x && x < secondary_x + button_w && y >= button_y &&
            y < button_y + button_h) {
          installer_page = INSTALLER_PAGE_REVIEW;
          installer_set_status("Review the install plan before continuing.");
          return;
        }

        if (installer_page == INSTALLER_PAGE_REVIEW &&
            x >= secondary_x && x < secondary_x + button_w && y >= button_y &&
            y < button_y + button_h) {
          if (!installer_has_run && !installer_active)
            installer_start_background_install();
          return;
        }

        if ((installer_page == INSTALLER_PAGE_TARGET ||
             installer_page == INSTALLER_PAGE_REVIEW) &&
            x >= utility_x && x < utility_x + 150 && y >= button_y &&
            y < button_y + button_h) {
          open_partition_manager_window(win->x + 36, win->y + 30);
          return;
        }
      }

      if (win->title[0] == 'P' && win->title[1] == 'a' &&
          win->title[2] == 'r' && win->title[3] == 't') {
        int content_x = 0;
        int content_y = 0;
        gui_get_window_content_rect(win, &content_x, &content_y, NULL, NULL);
        int selected_disk_index = installer_selected_disk_index();
        extern int storage_create_partition(int disk_index,
                                            storage_partition_kind_t kind,
                                            uint32_t size_mib);
        extern int storage_update_partition(int disk_index, int partition_index,
                                            storage_partition_kind_t kind,
                                            uint32_t size_mib);
        extern int storage_delete_partition(int disk_index, int partition_index);
        extern int storage_format_partition(int disk_index, int partition_index,
                                            storage_filesystem_kind_t fs_kind);
        extern int storage_ensure_install_partitions(int disk_index);

        installer_refresh_disk_inventory();
        partition_manager_refresh_partitions();
        for (int i = 0; i < installer_disk_count && i < 6; i++) {
          int row_y = content_y + 96 + i * 28;
          if (x >= content_x + 24 && x < content_x + win->width - 24 &&
              y >= row_y && y < row_y + 24) {
            installer_selected_disk = i;
            str_copy_safe(partition_manager_status, "Selected disk updated.",
                          sizeof(partition_manager_status));
            return;
          }
        }

        for (int i = 0; i < partition_manager_partition_count && i < 4; i++) {
          int row_y = content_y + 196 + i * 22;
          if (x >= content_x + 24 && x < content_x + win->width - 24 &&
              y >= row_y && y < row_y + 18) {
            partition_manager_selected_partition = i;
            str_copy_safe(partition_manager_status, "Selected partition updated.",
                          sizeof(partition_manager_status));
            return;
          }
        }

        if (x >= content_x + 24 && x < content_x + 144 && y >= content_y + 298 &&
            y < content_y + 328) {
          if (installer_write_target_config() == 0) {
            str_copy_safe(partition_manager_status,
                          "Real disk set as installer target.",
                          sizeof(partition_manager_status));
            installer_set_status("Installer target disk updated.");
          } else {
            str_copy_safe(partition_manager_status,
                          "Failed to write installer target config.",
                          sizeof(partition_manager_status));
          }
          return;
        }

        if (x >= content_x + 152 && x < content_x + 230 &&
            y >= content_y + 298 && y < content_y + 328) {
          if (selected_disk_index >= 0 &&
              storage_create_partition(selected_disk_index,
                                       STORAGE_PARTITION_EFI, 256) == 0) {
            partition_manager_refresh_partitions();
            str_copy_safe(partition_manager_status, "EFI partition created.",
                          sizeof(partition_manager_status));
          } else {
            str_copy_safe(partition_manager_status, "EFI partition creation failed.",
                          sizeof(partition_manager_status));
          }
          return;
        }

        if (x >= content_x + 238 && x < content_x + 332 &&
            y >= content_y + 298 && y < content_y + 328) {
          if (selected_disk_index >= 0 &&
              storage_create_partition(selected_disk_index,
                                       STORAGE_PARTITION_SYSTEM, 8192) == 0) {
            partition_manager_refresh_partitions();
            str_copy_safe(partition_manager_status, "System partition created.",
                          sizeof(partition_manager_status));
          } else {
            str_copy_safe(partition_manager_status,
                          "System partition creation failed.",
                          sizeof(partition_manager_status));
          }
          return;
        }

        if (x >= content_x + 340 && x < content_x + 424 &&
            y >= content_y + 298 && y < content_y + 328) {
          if (selected_disk_index >= 0 &&
              storage_create_partition(selected_disk_index,
                                       STORAGE_PARTITION_DATA, 4096) == 0) {
            partition_manager_refresh_partitions();
            str_copy_safe(partition_manager_status, "Data partition created.",
                          sizeof(partition_manager_status));
          } else {
            str_copy_safe(partition_manager_status, "Data partition creation failed.",
                          sizeof(partition_manager_status));
          }
          return;
        }

        if (x >= content_x + 432 && x < content_x + 528 &&
            y >= content_y + 298 && y < content_y + 328) {
          if (partition_manager_partition_count > 0 &&
              selected_disk_index >= 0 &&
              storage_delete_partition(selected_disk_index,
                                       partition_manager_selected_partition) ==
                  0) {
            partition_manager_refresh_partitions();
            str_copy_safe(partition_manager_status, "Partition deleted.",
                          sizeof(partition_manager_status));
          } else {
            str_copy_safe(partition_manager_status, "Partition delete failed.",
                          sizeof(partition_manager_status));
          }
          return;
        }

        if (x >= content_x + 24 && x < content_x + 124 &&
            y >= content_y + 332 && y < content_y + 362) {
          if (partition_manager_partition_count > 0 &&
              selected_disk_index >= 0 &&
              storage_format_partition(selected_disk_index,
                                       partition_manager_selected_partition,
                                       STORAGE_FILESYSTEM_EXT4) == 0) {
            partition_manager_refresh_partitions();
            str_copy_safe(partition_manager_status,
                          "Selected partition formatted as ext4.",
                          sizeof(partition_manager_status));
          } else {
            str_copy_safe(partition_manager_status, "ext4 format failed.",
                          sizeof(partition_manager_status));
          }
          return;
        }

        if (x >= content_x + 132 && x < content_x + 242 &&
            y >= content_y + 332 && y < content_y + 362) {
          if (partition_manager_partition_count > 0 &&
              selected_disk_index >= 0 &&
              storage_format_partition(selected_disk_index,
                                       partition_manager_selected_partition,
                                       STORAGE_FILESYSTEM_FAT32) == 0) {
            partition_manager_refresh_partitions();
            str_copy_safe(partition_manager_status,
                          "Selected partition formatted as FAT32.",
                          sizeof(partition_manager_status));
          } else {
            str_copy_safe(partition_manager_status, "FAT32 format failed.",
                          sizeof(partition_manager_status));
          }
          return;
        }

        if (x >= content_x + 250 && x < content_x + 340 &&
            y >= content_y + 332 && y < content_y + 362) {
          if (partition_manager_partition_count > 0 &&
              selected_disk_index >= 0 &&
              storage_format_partition(selected_disk_index,
                                       partition_manager_selected_partition,
                                       STORAGE_FILESYSTEM_SWAP) == 0) {
            partition_manager_refresh_partitions();
            str_copy_safe(partition_manager_status,
                          "Selected partition formatted as swap.",
                          sizeof(partition_manager_status));
          } else {
            str_copy_safe(partition_manager_status, "Swap format failed.",
                          sizeof(partition_manager_status));
          }
          return;
        }

        if (x >= content_x + 348 && x < content_x + 458 &&
            y >= content_y + 332 && y < content_y + 362) {
          if (selected_disk_index >= 0 &&
              storage_ensure_install_partitions(selected_disk_index) >= 0) {
            partition_manager_refresh_partitions();
            str_copy_safe(partition_manager_status,
                          "Auto layout ensured EFI and System partitions.",
                          sizeof(partition_manager_status));
          } else {
            str_copy_safe(partition_manager_status, "Auto layout failed.",
                          sizeof(partition_manager_status));
          }
          return;
        }

        if (x >= content_x + 466 && x < content_x + 528 &&
            y >= content_y + 332 && y < content_y + 362) {
          installer_refresh_disk_inventory();
          partition_manager_refresh_partitions();
          str_copy_safe(partition_manager_status, "Disk list refreshed.",
                        sizeof(partition_manager_status));
          return;
        }
      }

      if (win->title[0] == 'D' && win->title[1] == 'i' &&
          win->title[2] == 's' && win->title[3] == 'k') {
        int content_x = 0;
        int content_y = 0;
        int content_w = 0;
        int content_h = 0;
        gui_get_window_content_rect(win, &content_x, &content_y, &content_w,
                                    &content_h);
        int list_x = content_x + 18;
        int list_y = content_y + 78;
        int list_w = (content_w - 54) / 2;
        int right_x = list_x + list_w + 18;
        int disk_count = storage_get_disk_count();
        int partition_count;

        disk_imager_refresh_state();
        partition_count = disk_imager_selected_disk >= 0
                              ? storage_get_partition_count(
                                    disk_imager_selected_disk)
                              : 0;

        for (int i = 0; i < disk_count && i < 6; i++) {
          int row_y = list_y + 36 + i * 28;
          if (x >= list_x + 10 && x < list_x + list_w - 10 && y >= row_y &&
              y < row_y + 24) {
            disk_imager_selected_disk = i;
            disk_imager_selected_partition = 0;
            disk_imager_set_status("Disk selected.");
            return;
          }
        }

        partition_count = disk_imager_selected_disk >= 0
                              ? storage_get_partition_count(
                                    disk_imager_selected_disk)
                              : 0;
        for (int i = 0; i < partition_count && i < 6; i++) {
          int row_y = list_y + 36 + i * 28;
          if (x >= right_x + 10 && x < right_x + list_w - 10 &&
              y >= row_y && y < row_y + 24) {
            disk_imager_selected_partition = i;
            disk_imager_set_status("Partition selected.");
            return;
          }
        }

        if (x >= content_x + 24 && x < content_x + 128 &&
            y >= content_y + content_h - 44 && y < content_y + content_h - 16) {
          if (disk_imager_backup_disk() == 0) {
            disk_imager_set_status("Disk image created successfully.");
          } else {
            disk_imager_set_status("Disk backup failed.");
          }
          return;
        }

        if (x >= content_x + 136 && x < content_x + 240 &&
            y >= content_y + content_h - 44 && y < content_y + content_h - 16) {
          if (disk_imager_restore_disk() == 0) {
            disk_imager_set_status("Disk image restored successfully.");
          } else {
            disk_imager_set_status("Disk restore failed.");
          }
          return;
        }

        if (x >= content_x + 248 && x < content_x + 364 &&
            y >= content_y + content_h - 44 && y < content_y + content_h - 16) {
          if (disk_imager_backup_partition() == 0) {
            disk_imager_set_status("Partition image created successfully.");
          } else {
            disk_imager_set_status("Partition backup failed.");
          }
          return;
        }

        if (x >= content_x + 372 && x < content_x + 488 &&
            y >= content_y + content_h - 44 && y < content_y + content_h - 16) {
          if (disk_imager_restore_partition() == 0) {
            disk_imager_set_status("Partition image restored successfully.");
          } else {
            disk_imager_set_status("Partition restore failed.");
          }
          return;
        }

        if (x >= content_x + 496 && x < content_x + 580 &&
            y >= content_y + content_h - 44 && y < content_y + content_h - 16) {
          disk_imager_refresh_state();
          disk_imager_set_status("Disk inventory refreshed.");
          return;
        }
      }

      if (win->on_mouse) {
        win->on_mouse(win, x - win->x, y - win->y, buttons);
      }
      break;
    }
  }

}

/* ===================================================================== */
/* Initialization */
/* ===================================================================== */

int gui_init(uint32_t *framebuffer, uint32_t width, uint32_t height,
             uint32_t pitch) {
  printk(KERN_INFO "GUI: Initializing windowing system\n");

  if (gui_is_installer_mode()) {
    installer_has_run = 0;
    installer_active = 0;
    installer_autorun_attempted = 0;
    installer_show_restart_screen = 0;
    installer_page = INSTALLER_PAGE_WELCOME;
    installer_install_journal_ready = 0;
    installer_install_journal_disk_index = -1;
    installer_install_journal_next_lba = 0;
    installer_install_journal_last_lba = 0;
    installer_set_status("Ready to install the system image.");
  }

  gui_apply_saved_boot_resolution(&framebuffer, &width, &height, &pitch);
  gui_load_saved_theme();

  primary_display.framebuffer = framebuffer;
  primary_display.width = width;
  primary_display.height = height;
  primary_display.pitch = pitch;
  primary_display.bpp = 32;
  input_set_mouse_bounds((int)width, (int)height);
  mouse_x = (int)width / 2;
  mouse_y = (int)height / 2;

  /* Register input callbacks */
  extern void input_set_gui_key_callback(void (*callback)(int key));
  extern void gui_handle_key_event(int key);
  input_set_gui_key_callback(gui_handle_key_event);

  /* Allocate backbuffer for double-buffering */
  if (height && pitch > ((uint32_t)-1) / height) {
    primary_display.backbuffer = NULL;
  } else {
    primary_display.backbuffer = kmalloc((size_t)pitch * (size_t)height);
  }
  if (!primary_display.backbuffer) {
    printk(KERN_WARNING
           "GUI: Backbuffer allocation failed, rendering directly to framebuffer\n");
  } else {
    g_saved_backbuffer = primary_display.backbuffer;
  }
  gui_use_display_render_target();
  gui_desktop_frame_profiler_reset();

  /* Clear windows */
  for (int i = 0; i < MAX_WINDOWS; i++) {
    windows[i].id = 0;
  }

  /* Initialize desktop manager only after the filesystem stack is ready. */
#ifndef ARCH_X86_64
  desktop_manager_init();
#endif

#if !CONFIG_INSTALLER_APP
  if (gui_is_installer_mode())
    gui_open_installer_window();
#endif

  printk(KERN_INFO "GUI: Display %ux%u initialized\n", width, height);

  return 0;
}

void gui_notify_storage_ready(void) {
  int startup_active_before;

  if (gui_is_installer_mode())
    return;

  account_partition_storage_ready = 1;
  if (account_state_persist_pending && account_username[0] && account_password[0])
    save_account_state();
  startup_active_before = startup_flow_active();
  runtime_sync_boot_storage_to_live();
  ensure_startup_flow();
  if ((startup_active_before || !startup_flow_active()) && !startup_flow_active())
    load_dock_config();
}

int gui_installer_mode(void) { return gui_is_installer_mode(); }

int gui_installer_disk_label(int slot, char *buf, size_t size) {
  if (!buf || size == 0)
    return -1;

  buf[0] = '\0';
  if (!gui_is_installer_mode())
    return -1;

  installer_refresh_disk_inventory();
  if (slot < 0 || slot >= installer_disk_count)
    return -1;

  str_copy_safe(buf, installer_disk_labels[slot], (int)size);
  return 0;
}

int gui_installer_select_disk(int slot) {
  if (!gui_is_installer_mode())
    return -1;

  installer_refresh_disk_inventory();
  if (slot < 0 || slot >= installer_disk_count)
    return -1;

  installer_selected_disk = slot;
  if (installer_write_target_config() != 0)
    return -1;

  installer_set_status("Installer target disk updated.");
  return 0;
}

int gui_installer_select_disk_index(int disk_index) {
  if (!gui_is_installer_mode())
    return -1;

  installer_refresh_disk_inventory();
  for (int i = 0; i < installer_disk_count; i++) {
    if (installer_disk_indices[i] != disk_index)
      continue;

    installer_selected_disk = i;
    if (installer_write_target_config() != 0)
      return -1;

    installer_set_status("Installer target disk updated.");
    return 0;
  }

  return -1;
}

int gui_installer_reboot_now(void) {
  if (!gui_is_installer_mode())
    return -1;

  gui_flush_account_state_before_power_transition();
  arch_reboot();
  return 0;
}

int gui_installer_target_root(char *buf, size_t size) {
  if (!buf || size == 0 || !gui_is_installer_mode())
    return -1;

  installer_refresh_disk_inventory();
  if (installer_selected_disk < 0 || installer_selected_disk >= installer_disk_count)
    return -1;

  installer_target_root_path(buf, (int)size);
  return buf[0] ? 0 : -1;
}

int gui_installer_target_physical_root(char *buf, size_t size) {
  if (!buf || size == 0 || !gui_is_installer_mode())
    return -1;

  return installer_get_persistent_root(buf, (int)size);
}

int gui_installer_system_image_root(char *buf, size_t size) {
  const char *path;

  if (!buf || size == 0 || !gui_is_installer_mode())
    return -1;
  if (installer_validate_system_image_payload() != 0)
    return -1;

  path = installer_system_image_root_path();
  if (!path || !path[0])
    return -1;
  str_copy_safe(buf, path, (int)size);
  return buf[0] ? 0 : -1;
}

int gui_installer_boot_payload_root(char *buf, size_t size) {
  const char *path;

  if (!buf || size == 0 || !gui_is_installer_mode())
    return -1;
  if (installer_validate_boot_payload() != 0)
    return -1;

  path = installer_boot_payload_root_path();
  if (!path || !path[0])
    return -1;
  str_copy_safe(buf, path, (int)size);
  return buf[0] ? 0 : -1;
}

int gui_bootscource_root(char *buf, size_t size) {
  if (!buf || size == 0)
    return -1;

  if (!bootscource[0] &&
      boot_storage_root_path(bootscource, (int)sizeof(bootscource)) != 0) {
    return -1;
  }

  str_copy_safe(buf, bootscource, (int)size);
  return buf[0] ? 0 : -1;
}

int gui_installer_payload_is_archive(const char *path) {
  if (!path || !gui_is_installer_mode())
    return -1;
  return installer_payload_is_packaged(path);
}

int gui_installer_has_raw_disk_image(void) {
  if (!gui_is_installer_mode())
    return -1;
  return installer_system_disk_image_path() ? 1 : 0;
}

int gui_installer_apply_system_payload(void) {
  if (!gui_is_installer_mode())
    return -1;

  installer_refresh_disk_inventory();
  if (installer_selected_disk < 0 || installer_selected_disk >= installer_disk_count)
    return -1;

  installer_target_root_path(installer_target_root, sizeof(installer_target_root));
  installer_partition_root_path(installer_efi_root, sizeof(installer_efi_root),
                                "EFI");
  installer_partition_root_path(installer_update_root,
                                sizeof(installer_update_root), "boot");
  return installer_apply_system_image_payload(installer_target_root);
}

int gui_installer_apply_raw_disk_image(void) {
  int selected_disk_index;

  if (!gui_is_installer_mode())
    return -1;
  if (installer_validate_raw_system_disk_image_payload() != 0)
    return -1;

  installer_refresh_disk_inventory();
  selected_disk_index = installer_selected_disk_index();
  if (selected_disk_index < 0)
    return -1;

  return installer_apply_system_disk_image(selected_disk_index);
}

struct display *gui_get_display(void) { return &primary_display; }

uint32_t gui_get_screen_width(void) { return primary_display.width; }

uint32_t gui_get_screen_height(void) { return primary_display.height; }

struct window *gui_create_file_manager(int x, int y) {
  struct window *win = gui_create_window("File Manager", x, y, 450, 350);
  if (win) {
    struct fm_state *st = kmalloc(sizeof(struct fm_state));
    if (st) {
      st->path[0] = '/';
      st->path[1] = '\0';
      st->selected[0] = '\0';
      st->scroll_y = 0;
      st->context_menu_visible = 0;
      st->context_menu_x = 0;
      st->context_menu_y = 0;
      st->context_menu_target_type = 0;
      st->context_menu_target_on_item = 0;
      st->context_menu_target[0] = '\0';
      win->userdata = st;
      win->on_mouse = fm_on_mouse;
      fm_set_window_title(win, st->path);
    }
  }
  return win;
}

/* Create file manager at specific path */
struct window *gui_create_file_manager_path(int x, int y, const char *path) {
  /* Build title with path */
  char title[128] = "File Manager - ";
  int ti = 15;
  if (path) {
    for (int i = 0; path[i] && ti < 126; i++) {
      title[ti++] = path[i];
    }
  }
  title[ti] = '\0';

  struct window *win = gui_create_window(title, x, y, 450, 350);
  if (win) {
    struct fm_state *st = kmalloc(sizeof(struct fm_state));
    if (st) {
      /* Copy the provided path */
      if (path) {
        int i = 0;
        while (path[i] && i < 255) {
          st->path[i] = path[i];
          i++;
        }
        st->path[i] = '\0';
      } else {
        st->path[0] = '/';
        st->path[1] = '\0';
      }
      st->selected[0] = '\0';
      st->scroll_y = 0;
      st->context_menu_visible = 0;
      st->context_menu_x = 0;
      st->context_menu_y = 0;
      st->context_menu_target_type = 0;
      st->context_menu_target_on_item = 0;
      st->context_menu_target[0] = '\0';
      win->userdata = st;
      win->on_mouse = fm_on_mouse;
      fm_set_window_title(win, st->path);
    }
  }
  return win;
}

static void notepad_on_mouse(struct window *win, int x, int y, int buttons) {
  int content_x = 0;
  int content_y = 0;
  int content_w = 0;
  int content_h = 0;
  int toolbar_h = 30;
  int status_h = 22;
  gui_get_window_content_rect_local(win, &content_x, &content_y, &content_w,
                                    &content_h);
  int text_area_y = content_y + toolbar_h + 2;
  int text_area_h = content_h - toolbar_h - status_h - 4;
  int gutter_w = 40;
  int text_x = content_x + 8 + gutter_w;
  int max_x = content_x + content_w - 12;
  int max_y = text_area_y + text_area_h - 8;

  if (notepad_dialog_mode != NOTEPAD_DIALOG_NONE) {
    struct fm_item items[FM_MAX_ITEMS];
    int item_count = fm_collect_items(notepad_dialog_dir, items, FM_MAX_ITEMS);
    int panel_w = content_w - 80;
    int panel_h = content_h - 70;
    int panel_x = content_x + 40;
    int panel_y = content_y + 26;
    int list_x = panel_x + 16;
    int list_y = panel_y + 68;
    int list_w = panel_w - 32;
    int row_h = 22;
    int visible_rows = (panel_h - 156) / row_h;
    if (visible_rows < 4)
      visible_rows = 4;

    if (x >= panel_x + 16 && x < panel_x + 60 && y >= panel_y + 34 &&
        y < panel_y + 56) {
      char parent[256];
      notepad_extract_parent_dir(notepad_dialog_dir, parent, sizeof(parent));
      str_copy_safe(notepad_dialog_dir, parent, sizeof(notepad_dialog_dir));
      notepad_set_status("Moved to parent folder");
      return;
    }

    for (int i = 0; i < item_count && i < visible_rows; i++) {
      int row_y = list_y + 2 + i * row_h;
      if (x >= list_x + 2 && x < list_x + list_w - 2 && y >= row_y &&
          y < row_y + row_h - 2) {
        char full_path[512];
        str_copy_safe(notepad_dialog_selected, items[i].name,
                      sizeof(notepad_dialog_selected));
        fm_join_path(notepad_dialog_dir, items[i].name, full_path, sizeof(full_path));
        if (items[i].type == 4) {
          str_copy_safe(notepad_dialog_dir, full_path, sizeof(notepad_dialog_dir));
          str_copy_safe(notepad_dialog_input, full_path, sizeof(notepad_dialog_input));
          notepad_dialog_selected[0] = '\0';
          notepad_set_status("Opened folder");
        } else {
          str_copy_safe(notepad_dialog_input, full_path, sizeof(notepad_dialog_input));
          notepad_set_status("Selected file");
        }
        return;
      }
    }

    if (x >= panel_x + panel_w - 110 && x < panel_x + panel_w - 68 &&
        y >= panel_y + panel_h - 50 && y < panel_y + panel_h - 26) {
      notepad_close_dialog();
      notepad_set_status("Dialog closed");
      return;
    }
    if (x >= panel_x + panel_w - 60 && x < panel_x + panel_w - 16 &&
        y >= panel_y + panel_h - 50 && y < panel_y + panel_h - 26) {
      notepad_confirm_dialog();
      return;
    }
    return;
  }

  if (y >= content_y && y < content_y + 30) {
    if (x >= content_x + 8 && x < content_x + 58) {
      notepad_reset_document();
      return;
    }
    if (x >= content_x + 62 && x < content_x + 112) {
      notepad_begin_dialog(NOTEPAD_DIALOG_OPEN);
      return;
    }
    if (x >= content_x + 116 && x < content_x + 166) {
      if (notepad_filepath[0])
        notepad_save_to_path(notepad_filepath);
      else
        notepad_begin_dialog(NOTEPAD_DIALOG_SAVE);
      return;
    }
    if (x >= content_x + 170 && x < content_x + 234) {
      notepad_begin_dialog(NOTEPAD_DIALOG_SAVE);
      return;
    }
    if (x >= content_x + 259 && x < content_x + 301) {
      if (notepad_copy_selection_to_clipboard() && notepad_delete_selection())
        notepad_mark_dirty("Cut selection to clipboard");
      return;
    }
    if (x >= content_x + 305 && x < content_x + 355) {
      if (notepad_copy_selection_to_clipboard())
        notepad_set_status("Copied selection");
      return;
    }
    if (x >= content_x + 359 && x < content_x + 414) {
      if (clipboard_len > 0 &&
          notepad_insert_bytes(clipboard_buffer, clipboard_len) > 0)
        notepad_mark_dirty("Pasted from clipboard");
      return;
    }
  }

  if (x >= content_x + 5 && x < content_x + content_w - 4 &&
      y >= text_area_y && y < text_area_y + text_area_h) {
    int old_cursor = notepad_cursor;
    int old_selection_anchor = notepad_selection_anchor;
    int old_selection_cursor = notepad_selection_cursor;
    int old_selecting = notepad_selecting_with_mouse;
    int cursor = notepad_cursor_from_point(x, y, text_x, text_area_y + 4, max_x, max_y);

    if (buttons & 1) {
      if (!notepad_selecting_with_mouse)
        notepad_selection_anchor = cursor;
      notepad_selection_cursor = cursor;
      notepad_selecting_with_mouse = 1;
      notepad_cursor = cursor;
    } else {
      notepad_move_cursor(cursor, 0);
      notepad_selecting_with_mouse = 0;
    }
    if (old_cursor != notepad_cursor ||
        old_selection_anchor != notepad_selection_anchor ||
        old_selection_cursor != notepad_selection_cursor ||
        old_selecting != notepad_selecting_with_mouse)
      gui_invalidate_window(win);
    return;
  }

  if (!(buttons & 1))
    notepad_selecting_with_mouse = 0;
}

void gui_open_notepad(const char *path) {
  notepad_reset_document();
  if (path)
    notepad_load_file(path);

  struct window *win = gui_create_window("Notepad", 150, 80, 450, 350);
  if (win) {
    win->on_mouse = notepad_on_mouse;
    notepad_update_window_title();
  }
}

static void rename_on_mouse(struct window *win, int x, int y, int buttons) {
  (void)buttons;
  /* Check Save Button */
  int content_x = 0;
  int content_y = 0;
  gui_get_window_content_rect_local(win, &content_x, &content_y, NULL, NULL);
  if (y >= content_y && y < content_y + 30) {
    if (x >= content_x + 10 && x < content_x + 70) {
      /* Save (Rename) clicked */
      if (rename_path[0] && rename_text[0]) {
        /* Construct new full path */
        char new_full_path[512];

        /* Extract parent dir from rename_path */
        int i = 0;
        int last_slash = -1;
        while (rename_path[i]) {
          new_full_path[i] = rename_path[i];
          if (rename_path[i] == '/')
            last_slash = i;
          i++;
        }

        /* Append new name after last slash */
        int idx = last_slash + 1;
        int t_idx = 0;
        while (rename_text[t_idx]) {
          new_full_path[idx++] = rename_text[t_idx++];
        }
        new_full_path[idx] = '\0';

        int ret = user_storage_rename(rename_path, new_full_path);

        if (ret == 0) {
          printk("Rename successful: %s -> %s\n", rename_path, new_full_path);
          gui_invalidate_window(win);
          win->visible = 0; /* Close window */
        } else {
          printk("Rename failed: %d\n", ret);
        }
      }
    }
  }
}

void gui_open_rename(const char *path) {
  /* Clear state */
  rename_text[0] = '\0';
  rename_cursor = 0;

  /* Copy path */
  int i = 0;
  while (path[i] && i < 511) {
    rename_path[i] = path[i];
    i++;
  }
  rename_path[i] = '\0';

  /* Pre-fill text with filename only */
  int last_slash = -1;
  i = 0;
  while (rename_path[i]) {
    if (rename_path[i] == '/')
      last_slash = i;
    i++;
  }

  const char *filename = rename_path + last_slash + 1;
  i = 0;
  while (filename[i] && i < 255) {
    rename_text[i] = filename[i];
    i++;
  }
  rename_text[i] = '\0';
  rename_cursor = i;

  struct window *win = gui_create_window("Rename", 200, 150, 300, 150);
  if (win) {
    win->on_mouse = rename_on_mouse;
  }
}

/* ===================================================================== */
/* Image Viewer                                                          */
/* ===================================================================== */

/* g_imgview is already defined as extern earlier in the file */

#define NUM_BOOTSTRAP_IMAGES 7

static const char *get_bootstrap_image_path(int index) {
  static const char *paths[] = {
      "/assets/wallpapers/landscape.png",
      "/assets/wallpapers/nature.jpg",
      "/assets/wallpapers/city.jpg",
      "/assets/wallpapers/portrait.jpg",
      "/assets/wallpapers/square.jpg",
      "/assets/wallpapers/ducks.png",
      "/assets/wallpapers/default.jpg",
  };
  if (index >= 0 && index < NUM_BOOTSTRAP_IMAGES)
    return paths[index];
  return NULL;
}

static const char *get_bootstrap_image_name(int index) {
  static const char *names[] = {"Landscape", "Nature", "City",
                                "Portrait",  "Square", "Ducks", "Default"};
  if (index >= 0 && index < NUM_BOOTSTRAP_IMAGES)
    return names[index];
  return "Unknown";
}

static void image_viewer_load_bootstrap(int index) {
  if (index < 0 || index >= NUM_BOOTSTRAP_IMAGES)
    return;

  /* Free previous image */
  if (g_imgview.loaded) {
    media_free_image(&g_imgview.image);
  }

  /* Decode image - detect format by magic bytes */
  uint8_t *external_data = NULL;
  size_t external_size = 0;
  const unsigned char *data = NULL;
  unsigned int len = 0;
  const char *path = get_bootstrap_image_path(index);
  int ret = -1;
  int used_external_file = 0;

  if (path && media_load_file(path, &external_data, &external_size) == 0) {
    data = external_data;
    len = (unsigned int)external_size;
    used_external_file = 1;
  } else {
    printk(KERN_ERR "Image Viewer: Missing bootstrap asset %s\n",
           path ? path : "(null)");
    g_imgview.loaded = 0;
    return;
  }

  ret = decode_image_file_for_path(path, data, len, &g_imgview.image, NULL, 0,
                                   NULL);
  if (used_external_file)
    media_free_file(external_data);

  if (ret == 0) {
    g_imgview.loaded = 1;
    g_imgview.zoom_pct = 100;
    g_imgview.offset_x = 0;
    g_imgview.offset_y = 0;
    g_imgview.current_image_index = index;

    int i = 0;
    const char *name = get_bootstrap_image_name(index);
    while (name[i] && i < 255) {
      g_imgview.current_file[i] = name[i];
      i++;
    }
    g_imgview.current_file[i] = '\0';

    printk(KERN_INFO "Image Viewer: Loaded %s (%dx%d)\n",
           get_bootstrap_image_name(index), g_imgview.image.width,
           g_imgview.image.height);
  } else {
    printk(KERN_ERR "Image Viewer: Failed to load image\n");
    g_imgview.loaded = 0;
  }
}

/* Load image from folder file list */
static void image_viewer_load_from_folder(int index) {
  if (index < 0 || index >= g_imgview.file_count)
    return;

  /* Build full path */
  char full_path[512];
  int pi = 0;
  for (int i = 0; g_imgview.folder_path[i] && pi < 500; i++) {
    full_path[pi++] = g_imgview.folder_path[i];
  }
  for (int i = 0; g_imgview.file_list[index][i] && pi < 511; i++) {
    full_path[pi++] = g_imgview.file_list[index][i];
  }
  full_path[pi] = '\0';

  /* Load image file */
  uint8_t *data = NULL;
  size_t size = 0;
  if (media_load_file(full_path, &data, &size) != 0) {
    printk(KERN_ERR "Image Viewer: Failed to read %s\n", full_path);
    return;
  }

  /* Free previous image */
  if (g_imgview.loaded) {
    media_free_image(&g_imgview.image);
  }

  /* Decode image */
  int ret =
      decode_image_file_for_path(full_path, data, size, &g_imgview.image, NULL, 0,
                                 NULL);
  media_free_file(data);

  if (ret == 0) {
    g_imgview.loaded = 1;
    g_imgview.zoom_pct = 100;
    g_imgview.offset_x = 0;
    g_imgview.offset_y = 0;
    g_imgview.file_index = index;
    g_imgview.current_image_index = -1; /* Mark as folder-loaded */

    /* Copy filename */
    int i = 0;
    while (g_imgview.file_list[index][i] && i < 255) {
      g_imgview.current_file[i] = g_imgview.file_list[index][i];
      i++;
    }
    g_imgview.current_file[i] = '\0';

    printk(KERN_INFO "Image Viewer: Loaded %s (%dx%d) [%d/%d]\n",
           g_imgview.current_file, g_imgview.image.width,
           g_imgview.image.height, index + 1, g_imgview.file_count);
  } else {
    printk(KERN_ERR "Image Viewer: Failed to decode %s\n", full_path);
    g_imgview.loaded = 0;
  }
}

static void image_viewer_on_draw(struct window *win) {
  int screen_w = primary_display.width;
  int screen_h = primary_display.height;

  /* Content area (below titlebar, inside borders) */
  int content_x = 0;
  int content_y = 0;
  int content_w = 0;
  int content_h = 0;
  gui_get_window_content_rect(win, &content_x, &content_y, &content_w,
                              &content_h);

  /* In fullscreen mode, use entire screen */
  int draw_x = g_imgview.fullscreen ? 0 : content_x;
  int draw_y = g_imgview.fullscreen ? 0 : content_y;
  int draw_w = g_imgview.fullscreen ? screen_w : content_w;
  int draw_h = g_imgview.fullscreen ? screen_h : content_h;

  /* Dark cinematic background */
  uint32_t bg_color = 0x0D0D0D;
  gui_draw_rect(draw_x, draw_y, draw_w, draw_h, bg_color);

  if (!g_imgview.loaded) {
    /* Elegant "No image" message */
    const char *msg = "Drop an image or click Next";
    int msg_len = 27;
    int text_x = draw_x + (draw_w - msg_len * 8) / 2;
    int text_y = draw_y + draw_h / 2;
    gui_draw_string(text_x, text_y, msg, 0x6B7280, bg_color);
    return;
  }

  /* Calculate display dimensions with rotation */
  int img_w = (int)g_imgview.image.width;
  int img_h = (int)g_imgview.image.height;
  int rot = g_imgview.rotation;

  /* Swap dimensions for 90/270 rotation */
  if (rot == 90 || rot == 270) {
    int tmp = img_w;
    img_w = img_h;
    img_h = tmp;
  }

  /* Auto-fit image to screen */
  int toolbar_h = 56;
  int avail_h = draw_h - toolbar_h - 20;
  int avail_w = draw_w - 20;

  int zoom = g_imgview.zoom_pct;
  if (zoom == 0) {
    /* Auto-fit mode */
    int zoom_w = (avail_w * 100) / img_w;
    int zoom_h = (avail_h * 100) / img_h;
    zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;
    if (zoom > 100)
      zoom = 100; /* Don't upscale */
  }

  int scaled_w = img_w * zoom / 100;
  int scaled_h = img_h * zoom / 100;

  /* Center image */
  int img_x = draw_x + (draw_w - scaled_w) / 2 + g_imgview.offset_x;
  int img_y = draw_y + (avail_h - scaled_h) / 2 + 10 + g_imgview.offset_y;

  /* Draw image with rotation */
  int orig_w = (int)g_imgview.image.width;
  int orig_h = (int)g_imgview.image.height;

  int clip_x;
  int clip_y;
  int clip_w;
  int clip_h;

  if (gui_target_visible_rect(img_x, img_y, scaled_w, scaled_h, &clip_x, &clip_y,
                              &clip_w, &clip_h)) {
    int viewport_x1 = draw_x + draw_w;
    int viewport_y1 = draw_y + avail_h + 10;

    if (clip_x < draw_x) {
      int delta = draw_x - clip_x;
      clip_x = draw_x;
      clip_w -= delta;
    }
    if (clip_y < draw_y) {
      int delta = draw_y - clip_y;
      clip_y = draw_y;
      clip_h -= delta;
    }
    if (clip_x + clip_w > viewport_x1)
      clip_w = viewport_x1 - clip_x;
    if (clip_y + clip_h > viewport_y1)
      clip_h = viewport_y1 - clip_y;

    if (clip_w > 0 && clip_h > 0) {
      uint32_t *target = gui_draw_target();
      int local_x0 = clip_x - g_render_target.origin_x;
      int local_y0 = clip_y - g_render_target.origin_y;
      int start_dx = clip_x - img_x;
      int start_dy = clip_y - img_y;
      int pitch = g_render_target.pitch_pixels;

      for (int dy = 0; dy < clip_h; dy++) {
        uint32_t *dst_row = target + (local_y0 + dy) * pitch + local_x0;

        for (int dx = 0; dx < clip_w; dx++) {
          int ix = (start_dx + dx) * 100 / zoom;
          int iy = (start_dy + dy) * 100 / zoom;
          int src_x;
          int src_y;

          switch (rot) {
          case 90:
            src_x = iy;
            src_y = orig_h - 1 - ix;
            break;
          case 180:
            src_x = orig_w - 1 - ix;
            src_y = orig_h - 1 - iy;
            break;
          case 270:
            src_x = orig_w - 1 - iy;
            src_y = ix;
            break;
          default:
            src_x = ix;
            src_y = iy;
            break;
          }

          if (src_x >= 0 && src_x < orig_w && src_y >= 0 && src_y < orig_h) {
            uint32_t pixel = g_imgview.image.pixels[src_y * orig_w + src_x];
            uint32_t alpha = pixel >> 24;

            if (alpha == 0)
              continue;
            if (alpha == 0xFF) {
              dst_row[dx] = pixel & 0xFFFFFF;
            } else {
              dst_row[dx] = gui_blend_rgb_over(dst_row[dx], pixel, alpha);
            }
          }
        }
      }
    }
  }

  /* ============================================= */
  /* MODERN FLOATING TOOLBAR                      */
  /* ============================================= */

  int tb_w = 520;
  int tb_h = 48;
  int tb_x = draw_x + (draw_w - tb_w) / 2;
  int tb_y = draw_y + draw_h - tb_h - 16;
  int corner_r = 12;

  /* Glassmorphism toolbar background */
  for (int row = 0; row < tb_h; row++) {
    int dist_y = (row < tb_h / 2) ? row : (tb_h - 1 - row);
    int alpha = 200 + (dist_y * 30 / (tb_h / 2));
    int fill_x = tb_x;
    int fill_w = tb_w;

    if (alpha > 230)
      alpha = 230;

    if (row < corner_r) {
      int dy = corner_r - 1 - row;

      for (int span = corner_r; span >= 0; span--) {
        if (span * span + dy * dy <= corner_r * corner_r) {
          fill_x = tb_x + corner_r - span;
          fill_w = tb_w - 2 * (corner_r - span);
          break;
        }
      }
    }

    gui_draw_rect(fill_x, tb_y + row, fill_w, 1,
                  ((alpha / 10) << 16) | ((alpha / 10) << 8) | (alpha / 8));
  }

  /* Toolbar buttons */
  int btn_size = 36;
  int btn_spacing = 8;
  int btn_y = tb_y + (tb_h - btn_size) / 2;
  int btn_x = tb_x + 16;

  uint32_t btn_bg = 0x374151;
  uint32_t btn_hover = 0x4B5563;
  uint32_t icon_color = 0xE5E7EB;

  for (int i = 0; i < 8; i++) {
    /* Check hover */
    int is_hover = (mouse_x >= btn_x && mouse_x < btn_x + btn_size &&
                    mouse_y >= btn_y && mouse_y < btn_y + btn_size);
    uint32_t bg = is_hover ? btn_hover : btn_bg;

    draw_rounded_rect(btn_x, btn_y, btn_size, btn_size, 8, bg);
    /* Draw pre-rendered RGBA icon from toolbar_icons.h */
    const uint32_t *icon_data = toolbar_icons[i];
    int icon_x = btn_x + (btn_size - TOOLBAR_ICON_SIZE) / 2;
    int icon_y = btn_y + (btn_size - TOOLBAR_ICON_SIZE) / 2;

    for (int iy = 0; iy < TOOLBAR_ICON_SIZE; iy++) {
      int run_start = -1;

      for (int ix = 0; ix <= TOOLBAR_ICON_SIZE; ix++) {
        int visible = 0;
        if (ix < TOOLBAR_ICON_SIZE) {
          uint32_t pixel = icon_data[iy * TOOLBAR_ICON_SIZE + ix];
          visible = ((pixel >> 24) & 0xFF) > 128;
        }

        if (visible) {
          if (run_start < 0)
            run_start = ix;
        } else if (run_start >= 0) {
          gui_draw_rect(icon_x + run_start, icon_y + iy, ix - run_start, 1,
                        icon_color);
          run_start = -1;
        }
      }
    }
    btn_x += btn_size + btn_spacing;
  }

  /* Image info text */
  char info[64];
  int idx = 0;
  /* Dimensions */
  int w = (int)g_imgview.image.width;
  int h = (int)g_imgview.image.height;
  if (w >= 1000) {
    info[idx++] = '0' + (w / 1000) % 10;
  }
  if (w >= 100) {
    info[idx++] = '0' + (w / 100) % 10;
  }
  if (w >= 10) {
    info[idx++] = '0' + (w / 10) % 10;
  }
  info[idx++] = '0' + w % 10;
  info[idx++] = 'x';
  if (h >= 1000) {
    info[idx++] = '0' + (h / 1000) % 10;
  }
  if (h >= 100) {
    info[idx++] = '0' + (h / 100) % 10;
  }
  if (h >= 10) {
    info[idx++] = '0' + (h / 10) % 10;
  }
  info[idx++] = '0' + h % 10;
  info[idx++] = ' ';
  /* Rotation */
  if (rot > 0) {
    if (rot >= 100)
      info[idx++] = '0' + (rot / 100) % 10;
    if (rot >= 10)
      info[idx++] = '0' + (rot / 10) % 10;
    info[idx++] = '0' + rot % 10;
    info[idx++] = (char)176; /* degree symbol approximation */
  }
  info[idx] = '\0';

  gui_draw_string(tb_x + tb_w - 120, btn_y + 12, info, 0x9CA3AF, 0x1F2937);
}

static void image_viewer_on_mouse(struct window *win, int x, int y,
                                  int buttons) {
  /* x,y are already window-relative (0,0 = window top-left) */

  /* Content area within window (relative coords) */
  int content_x = 0;
  int content_y = 0;
  int content_w = 0;
  int content_h = 0;
  gui_get_window_content_rect_local(win, &content_x, &content_y, &content_w,
                                    &content_h);

  /* Toolbar position within content area */
  int tb_w = 520;
  int tb_h = 48;
  int tb_x = content_x + (content_w - tb_w) / 2;
  int tb_y = content_y + content_h - tb_h - 16;

  int btn_size = 36;
  int btn_spacing = 8;
  int btn_y_pos = tb_y + (tb_h - btn_size) / 2;
  int btn_x = tb_x + 16;

  /* Check each of the 8 toolbar buttons */
  for (int i = 0; i < 8; i++) {
    if (x >= btn_x && x < btn_x + btn_size && y >= btn_y_pos &&
        y < btn_y_pos + btn_size) {

      switch (i) {
      case 0: /* Previous */
        if (g_imgview.file_count > 0) {
          /* Folder-based navigation */
          int new_index = g_imgview.file_index - 1;
          if (new_index < 0)
            new_index = g_imgview.file_count - 1;
          image_viewer_load_from_folder(new_index);
        } else {
          /* Fallback to bootstrap images */
          if (g_imgview.current_image_index <= 0) {
            image_viewer_load_bootstrap(NUM_BOOTSTRAP_IMAGES - 1);
          } else {
            image_viewer_load_bootstrap(g_imgview.current_image_index - 1);
          }
        }
        break;

      case 1: /* Next */
        if (g_imgview.file_count > 0) {
          /* Folder-based navigation */
          int new_index = g_imgview.file_index + 1;
          if (new_index >= g_imgview.file_count)
            new_index = 0;
          image_viewer_load_from_folder(new_index);
        } else {
          /* Fallback to bootstrap images */
          if (g_imgview.current_image_index < 0 ||
              g_imgview.current_image_index >= NUM_BOOTSTRAP_IMAGES - 1) {
            image_viewer_load_bootstrap(0);
          } else {
            image_viewer_load_bootstrap(g_imgview.current_image_index + 1);
          }
        }
        break;

      case 2: /* Rotate Right (CW) */
        g_imgview.rotation = (g_imgview.rotation + 90) % 360;
        break;

      case 3: /* Rotate Left (CCW) */
        g_imgview.rotation = (g_imgview.rotation + 270) % 360;
        break;

      case 4: /* Zoom In */
        if (g_imgview.zoom_pct == 0) {
          g_imgview.zoom_pct = 100;
        }
        g_imgview.zoom_pct = g_imgview.zoom_pct * 125 / 100;
        if (g_imgview.zoom_pct > 400) {
          g_imgview.zoom_pct = 400;
        }
        break;

      case 5: /* Zoom Out */
        if (g_imgview.zoom_pct == 0) {
          g_imgview.zoom_pct = 100;
        }
        g_imgview.zoom_pct = g_imgview.zoom_pct * 80 / 100;
        if (g_imgview.zoom_pct < 10) {
          g_imgview.zoom_pct = 10;
        }
        break;

      case 6:                   /* Fit */
        g_imgview.zoom_pct = 0; /* Auto-fit mode */
        g_imgview.offset_x = 0;
        g_imgview.offset_y = 0;
        break;

      case 7: /* Fullscreen Toggle */
        g_imgview.fullscreen = !g_imgview.fullscreen;
        g_imgview.zoom_pct = 0; /* Reset to auto-fit */
        g_imgview.offset_x = 0;
        g_imgview.offset_y = 0;
        break;
      }
      gui_invalidate_window(win);
      return;
    }
    btn_x += btn_size + btn_spacing;
  }

  /* Pan disabled - image stays fixed */
  (void)buttons; /* Unused */
}

void gui_open_image_gallery(void) {
  struct window *win = gui_create_window("Image Gallery", 100, 80, 700, 550);
  if (win) {
    win->on_draw = image_viewer_on_draw;
    win->on_mouse = image_viewer_on_mouse;

    /* Load first image */
    if (!g_imgview.loaded) {
      image_viewer_load_bootstrap(0);
    }
  }
}
