/*
 * OS8 - GUI System Header
 */

#ifndef _GUI_H
#define _GUI_H

#include "types.h"

/* ===================================================================== */
/* Window System */
/* ===================================================================== */

struct window;
struct display;

typedef struct gui_frame_profile {
    uint64_t input_poll_us;
    uint64_t net_poll_us;
    uint64_t uart_key_us;
    uint64_t queued_keys_us;
    uint64_t mouse_us;
    uint64_t compose_us;
    uint64_t kernel_slice_us;
    uint64_t wait_next_frame_us;
    uint64_t total_us;
} gui_frame_profile_t;

typedef struct gui_profiler_span {
    const char *label;
    uint64_t start_us;
    uint64_t elapsed_us;
    int active;
} gui_profiler_span_t;

/* Display */
int gui_init(uint32_t *framebuffer, uint32_t width, uint32_t height, uint32_t pitch);
struct display *gui_get_display(void);
void gui_compose(void);
void gui_invalidate_rect(int x, int y, int w, int h);
void gui_invalidate_screen(void);
int gui_needs_redraw(void);
void gui_configure_gpu_rendering(int enabled);
int gui_is_gpu_rendering_enabled(void);
void gui_refresh_hardware_acceleration_policy(void);
uint64_t gui_monotonic_us(void);
void gui_profiler_begin(gui_profiler_span_t *span, const char *label);
uint64_t gui_profiler_end(gui_profiler_span_t *span);
void gui_desktop_frame_profiler_note(const char *label, uint64_t elapsed_us);
void gui_desktop_frame_profiler_submit(const gui_frame_profile_t *profile);
void gui_desktop_frame_profiler_clear_notes(void);
void gui_desktop_frame_profiler_reset(void);

/* Window management */
struct window *gui_create_window(const char *title, int x, int y, int w, int h);
void gui_destroy_window(struct window *win);
void gui_focus_window(struct window *win);

/* Drawing primitives */
void gui_draw_rect(int x, int y, int w, int h, uint32_t color);
void gui_draw_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);
void gui_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void gui_draw_circle(int cx, int cy, int r, uint32_t color, bool filled);
void gui_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void gui_draw_string(int x, int y, const char *str, uint32_t fg, uint32_t bg);
void fb_show_boot_log(void);

/* Input */
void gui_handle_mouse_event(int x, int y, int buttons);
void gui_handle_key_event(int keycode);
void gui_move_mouse(int dx, int dy);
void gui_draw_cursor(void);
void gui_open_image_viewer(const char *path);
int gui_launch_app_by_id(const char *app_id);
int gui_draw_system_app_icon(const char *app_id, int x, int y, int size);

/* ===================================================================== */
/* Terminal */
/* ===================================================================== */

struct terminal;

struct terminal *term_create(int x, int y, int cols, int rows);
void term_destroy(struct terminal *term);
void term_putc(struct terminal *term, char c);
void term_puts(struct terminal *term, const char *str);
void term_render(struct terminal *term);
void term_handle_key(struct terminal *term, int key);
struct terminal *term_get_active(void);
void term_set_active(struct terminal *term);

/* ===================================================================== */
/* Application Framework */
/* ===================================================================== */

typedef enum {
    APP_TYPE_TERMINAL,
    APP_TYPE_FILE_MANAGER,
    APP_TYPE_TEXT_EDITOR,
    APP_TYPE_IMAGE_VIEWER,
    APP_TYPE_BROWSER,
    APP_TYPE_SETTINGS,
    APP_TYPE_CUSTOM
} app_type_t;

struct application;

struct application *app_launch(const char *name, app_type_t type);
void app_close(struct application *app);
void app_update_all(void);
void app_draw_all(void);

/* Desktop */
void desktop_init(void);
void launcher_add_item(const char *name, const char *icon, app_type_t type);
void launcher_draw(void);
void launcher_handle_click(int x, int y);

#endif /* _GUI_H */
