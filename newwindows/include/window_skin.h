#ifndef WINDOW_SKIN_H
#define WINDOW_SKIN_H

#ifdef WINDOW_SKIN_USE_KERNEL_TYPES
#include "types.h"
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} SkinColor;

typedef struct {
    int x;
    int y;
    int width;
    int height;
} SkinRect;

typedef enum {
    SKIN_BORDER_NONE = 0,
    SKIN_BORDER_FLAT,
    SKIN_BORDER_BEVEL,
    SKIN_BORDER_DOUBLE
} SkinBorderStyle;

typedef enum {
    SKIN_SHADOW_NONE = 0,
    SKIN_SHADOW_HARD,
    SKIN_SHADOW_SOFT
} SkinShadowStyle;

typedef enum {
    SKIN_BUTTON_MINIMIZE = 0,
    SKIN_BUTTON_MAXIMIZE,
    SKIN_BUTTON_CLOSE,
    SKIN_BUTTON_COUNT
} SkinButtonKind;

typedef enum {
    SKIN_TEXTURE_NONE = 0,
    SKIN_TEXTURE_BRUSHED,
    SKIN_TEXTURE_GLASS,
    SKIN_TEXTURE_NOISE,
    SKIN_TEXTURE_GRID,
    SKIN_TEXTURE_STRIPES
} SkinTexture;

typedef enum {
    SKIN_HIT_NONE = 0,
    SKIN_HIT_CLIENT,
    SKIN_HIT_TITLEBAR,
    SKIN_HIT_MINIMIZE,
    SKIN_HIT_MAXIMIZE,
    SKIN_HIT_CLOSE,
    SKIN_HIT_RESIZE_LEFT,
    SKIN_HIT_RESIZE_RIGHT,
    SKIN_HIT_RESIZE_TOP,
    SKIN_HIT_RESIZE_BOTTOM,
    SKIN_HIT_RESIZE_TOP_LEFT,
    SKIN_HIT_RESIZE_TOP_RIGHT,
    SKIN_HIT_RESIZE_BOTTOM_LEFT,
    SKIN_HIT_RESIZE_BOTTOM_RIGHT
} SkinHit;

typedef struct {
    int titlebar_height;
    int border_left;
    int border_right;
    int border_top;
    int border_bottom;
    int corner_radius;
    int button_width;
    int button_height;
    int button_spacing;
    int title_padding;
    int icon_size;
    int shadow_size;
    int resize_grip_size;
} SkinMetrics;

typedef struct {
    SkinColor frame;
    SkinColor frame_inactive;
    SkinColor titlebar;
    SkinColor titlebar_inactive;
    SkinColor title_text;
    SkinColor title_text_inactive;
    SkinColor client_background;
    SkinColor border_light;
    SkinColor border_dark;
    SkinColor button;
    SkinColor button_hover;
    SkinColor button_pressed;
    SkinColor close_button;
    SkinColor close_button_hover;
    SkinColor button_icon;
    SkinColor shadow;
    SkinColor accent;
} SkinColors;

typedef struct {
    const char *name;
    const char *author;

    SkinMetrics metrics;
    SkinColors colors;

    SkinBorderStyle border_style;
    SkinShadowStyle shadow_style;
    SkinTexture button_texture;

    SkinButtonKind button_order[SKIN_BUTTON_COUNT];

    float button_texture_strength;
    float border_blur;
    float border_transparency;
    float border_reflection;

    bool buttons_on_left;
    bool animate_buttons;
    bool show_icon;
    bool show_title;
    bool translucent_titlebar;
} WindowSkin;

typedef struct {
    SkinRect outer;
    SkinRect titlebar;
    SkinRect client;
    SkinRect buttons[SKIN_BUTTON_COUNT];
    bool button_visible[SKIN_BUTTON_COUNT];
} SkinChromeLayout;

typedef struct {
    int x;
    int y;
    int width;
    int height;

    bool focused;
    bool resizable;
    bool movable;

    const char *title;
} SkinWindow;

typedef struct {
    bool active;
    int pointer_x;
    int pointer_y;
    int window_start_x;
    int window_start_y;
} SkinDragState;

/*
 * Abstract renderer.
 *
 * Replace these callbacks with framebuffer, GPU or compositor functions.
 */
typedef struct {
    void *userdata;

    void (*fill_rect)(
        void *userdata,
        SkinRect rect,
        SkinColor color
    );

    void (*draw_rect_outline)(
        void *userdata,
        SkinRect rect,
        SkinColor color
    );

    void (*draw_line)(
        void *userdata,
        int x1,
        int y1,
        int x2,
        int y2,
        SkinColor color
    );

    void (*draw_text)(
        void *userdata,
        int x,
        int y,
        const char *text,
        SkinColor color
    );
} SkinRenderer;

SkinColor skin_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
SkinColor skin_rgb(uint8_t r, uint8_t g, uint8_t b);
SkinColor skin_blend(SkinColor fg, SkinColor bg);
SkinColor skin_lerp(SkinColor a, SkinColor b, float t);

WindowSkin skin_make_aurora_sample(void);

void skin_swap_minimize_maximize(WindowSkin *skin);

SkinChromeLayout skin_calculate_layout(
    const SkinWindow *window,
    const WindowSkin *skin
);

SkinHit skin_hit_test(
    const SkinWindow *window,
    const WindowSkin *skin,
    int pointer_x,
    int pointer_y
);

void skin_draw_window_chrome(
    const SkinRenderer *renderer,
    const SkinWindow *window,
    const WindowSkin *skin,
    SkinHit hot,
    SkinHit pressed
);

bool skin_begin_drag(
    SkinDragState *drag,
    const SkinWindow *window,
    SkinHit hit,
    int pointer_x,
    int pointer_y
);

void skin_update_drag(
    SkinDragState *drag,
    SkinWindow *window,
    int pointer_x,
    int pointer_y,
    SkinRect desktop_bounds
);

void skin_end_drag(SkinDragState *drag);

#ifdef __cplusplus
}
#endif

#endif
