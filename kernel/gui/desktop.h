/*
 * OS8 - Desktop Manager Header
 */

#ifndef _DESKTOP_H
#define _DESKTOP_H

#include "types.h"

/* Initialize desktop manager */
void desktop_manager_init(void);

/* Refresh desktop from filesystem */
void desktop_refresh(void);

/* Sort and arrange icons */
void desktop_sort_icons(void);
void desktop_arrange_icons(void);

/* Event handling */
int desktop_handle_click(int x, int y, int button, int shift_held);
int desktop_handle_double_click(int x, int y);
void desktop_handle_pointer_motion(int x, int y);
void desktop_update_drag(int x, int y, int left_held);
void desktop_release_drag(int x, int y);
int desktop_handle_key(int key);  /* Returns 1 if consumed */
int desktop_context_menu_hover(int mx, int my);
int desktop_context_menu_click(int mx, int my);
int desktop_is_renaming(void);

/* Drawing */
void desktop_draw_icons(void);
void desktop_draw_icons_region(int x, int y, int w, int h);

/* Dirty region tracking */
void desktop_mark_dirty(int x, int y, int w, int h);
void desktop_mark_full_redraw(void);
int desktop_needs_redraw(void);
int desktop_get_dirty_bounds(int *x, int *y, int *w, int *h);
void desktop_clear_dirty(void);

/* State queries */
int desktop_get_icon_count(void);
int desktop_is_context_menu_visible(void);
int desktop_sidebar_is_visible(void);
int desktop_sidebar_get_side(void);
int desktop_sidebar_get_width(void);
void desktop_sidebar_set_visible(int visible);
void desktop_sidebar_set_side(int side);
void desktop_sidebar_set_width(int width);

/* Context menu */
void desktop_show_context_menu(int x, int y, int on_icon);
void desktop_hide_context_menu(void);

#define DESKTOP_SIDEBAR_LEFT 0
#define DESKTOP_SIDEBAR_RIGHT 1

#endif /* _DESKTOP_H */
