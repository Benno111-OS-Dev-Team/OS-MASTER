#include "window_skin.h"

#include <string.h>

static int skin_clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static float skin_clamp_float(float value, float minimum, float maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static bool rect_contains(SkinRect rect, int x, int y)
{
    return x >= rect.x &&
           y >= rect.y &&
           x < rect.x + rect.width &&
           y < rect.y + rect.height;
}

SkinColor skin_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    SkinColor color = {r, g, b, a};
    return color;
}

SkinColor skin_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return skin_rgba(r, g, b, 255);
}

SkinColor skin_blend(SkinColor fg, SkinColor bg)
{
    const unsigned int alpha = fg.a;
    const unsigned int inverse = 255U - alpha;

    return skin_rgb(
        (uint8_t)(((unsigned int)fg.r * alpha + (unsigned int)bg.r * inverse) / 255U),
        (uint8_t)(((unsigned int)fg.g * alpha + (unsigned int)bg.g * inverse) / 255U),
        (uint8_t)(((unsigned int)fg.b * alpha + (unsigned int)bg.b * inverse) / 255U)
    );
}

SkinColor skin_lerp(SkinColor a, SkinColor b, float t)
{
    t = skin_clamp_float(t, 0.0f, 1.0f);

    return skin_rgba(
        (uint8_t)(a.r + (b.r - a.r) * t),
        (uint8_t)(a.g + (b.g - a.g) * t),
        (uint8_t)(a.b + (b.b - a.b) * t),
        (uint8_t)(a.a + (b.a - a.a) * t)
    );
}

WindowSkin skin_make_aurora_sample(void)
{
    WindowSkin skin;
    memset(&skin, 0, sizeof(skin));

    skin.name = "Editson Aurora Glass Reflective";
    skin.author = "Benno111";

    skin.metrics.titlebar_height = 36;
    skin.metrics.border_left = 2;
    skin.metrics.border_right = 2;
    skin.metrics.border_top = 2;
    skin.metrics.border_bottom = 2;
    skin.metrics.corner_radius = 12;
    skin.metrics.button_width = 46;
    skin.metrics.button_height = 36;
    skin.metrics.button_spacing = 0;
    skin.metrics.title_padding = 10;
    skin.metrics.icon_size = 16;
    skin.metrics.shadow_size = 18;
    skin.metrics.resize_grip_size = 7;

    skin.colors.frame = skin_rgba(39, 48, 68, 204);
    skin.colors.frame_inactive = skin_rgba(26, 29, 39, 184);
    skin.colors.titlebar = skin_rgba(46, 59, 84, 217);
    skin.colors.titlebar_inactive = skin_rgba(32, 37, 50, 192);
    skin.colors.title_text = skin_rgb(247, 250, 255);
    skin.colors.title_text_inactive = skin_rgb(154, 165, 184);
    skin.colors.client_background = skin_rgb(16, 18, 24);
    skin.colors.border_light = skin_rgba(165, 213, 255, 136);
    skin.colors.border_dark = skin_rgb(10, 12, 18);
    skin.colors.button = skin_rgba(49, 65, 91, 217);
    skin.colors.button_hover = skin_rgba(88, 116, 155, 234);
    skin.colors.button_pressed = skin_rgb(111, 141, 181);
    skin.colors.close_button = skin_rgba(49, 65, 91, 217);
    skin.colors.close_button_hover = skin_rgb(227, 74, 95);
    skin.colors.button_icon = skin_rgb(244, 247, 255);
    skin.colors.shadow = skin_rgba(0, 0, 0, 153);
    skin.colors.accent = skin_rgb(104, 185, 255);

    skin.border_style = SKIN_BORDER_FLAT;
    skin.shadow_style = SKIN_SHADOW_SOFT;
    skin.button_texture = SKIN_TEXTURE_BRUSHED;

    skin.button_order[0] = SKIN_BUTTON_MINIMIZE;
    skin.button_order[1] = SKIN_BUTTON_MAXIMIZE;
    skin.button_order[2] = SKIN_BUTTON_CLOSE;

    skin.button_texture_strength = 0.34f;
    skin.border_blur = 14.0f;
    skin.border_transparency = 0.72f;
    skin.border_reflection = 0.62f;

    skin.buttons_on_left = false;
    skin.animate_buttons = true;
    skin.show_icon = true;
    skin.show_title = true;
    skin.translucent_titlebar = true;

    return skin;
}

void skin_swap_minimize_maximize(WindowSkin *skin)
{
    size_t minimize_index = SKIN_BUTTON_COUNT;
    size_t maximize_index = SKIN_BUTTON_COUNT;

    if (skin == NULL) {
        return;
    }

    for (size_t i = 0; i < SKIN_BUTTON_COUNT; ++i) {
        if (skin->button_order[i] == SKIN_BUTTON_MINIMIZE) {
            minimize_index = i;
        } else if (skin->button_order[i] == SKIN_BUTTON_MAXIMIZE) {
            maximize_index = i;
        }
    }

    if (minimize_index < SKIN_BUTTON_COUNT &&
        maximize_index < SKIN_BUTTON_COUNT) {
        SkinButtonKind temporary = skin->button_order[minimize_index];
        skin->button_order[minimize_index] = skin->button_order[maximize_index];
        skin->button_order[maximize_index] = temporary;
    }
}

SkinChromeLayout skin_calculate_layout(
    const SkinWindow *window,
    const WindowSkin *skin
)
{
    SkinChromeLayout layout;
    memset(&layout, 0, sizeof(layout));

    if (window == NULL || skin == NULL) {
        return layout;
    }

    layout.outer = (SkinRect){
        window->x,
        window->y,
        window->width,
        window->height
    };

    layout.titlebar = (SkinRect){
        window->x + skin->metrics.border_left,
        window->y + skin->metrics.border_top,
        window->width -
            skin->metrics.border_left -
            skin->metrics.border_right,
        skin->metrics.titlebar_height
    };

    layout.client = (SkinRect){
        window->x + skin->metrics.border_left,
        layout.titlebar.y + layout.titlebar.height,
        window->width -
            skin->metrics.border_left -
            skin->metrics.border_right,
        window->height -
            skin->metrics.border_top -
            skin->metrics.titlebar_height -
            skin->metrics.border_bottom
    };

    int cursor;

    if (skin->buttons_on_left) {
        cursor = layout.titlebar.x;

        for (size_t i = 0; i < SKIN_BUTTON_COUNT; ++i) {
            SkinButtonKind kind = skin->button_order[i];

            layout.buttons[kind] = (SkinRect){
                cursor,
                layout.titlebar.y,
                skin->metrics.button_width,
                skin->metrics.button_height
            };

            layout.button_visible[kind] = true;
            cursor += skin->metrics.button_width +
                      skin->metrics.button_spacing;
        }
    } else {
        cursor = layout.titlebar.x + layout.titlebar.width;

        for (size_t i = SKIN_BUTTON_COUNT; i > 0; --i) {
            SkinButtonKind kind = skin->button_order[i - 1];

            cursor -= skin->metrics.button_width;

            layout.buttons[kind] = (SkinRect){
                cursor,
                layout.titlebar.y,
                skin->metrics.button_width,
                skin->metrics.button_height
            };

            layout.button_visible[kind] = true;
            cursor -= skin->metrics.button_spacing;
        }
    }

    return layout;
}

SkinHit skin_hit_test(
    const SkinWindow *window,
    const WindowSkin *skin,
    int pointer_x,
    int pointer_y
)
{
    if (window == NULL || skin == NULL) {
        return SKIN_HIT_NONE;
    }

    SkinChromeLayout layout = skin_calculate_layout(window, skin);

    if (!rect_contains(layout.outer, pointer_x, pointer_y)) {
        return SKIN_HIT_NONE;
    }

    if (window->resizable) {
        const int grip = skin->metrics.resize_grip_size;

        const bool left =
            pointer_x < layout.outer.x + grip;
        const bool right =
            pointer_x >= layout.outer.x +
                         layout.outer.width - grip;
        const bool top =
            pointer_y < layout.outer.y + grip;
        const bool bottom =
            pointer_y >= layout.outer.y +
                         layout.outer.height - grip;

        if (left && top) {
            return SKIN_HIT_RESIZE_TOP_LEFT;
        }
        if (right && top) {
            return SKIN_HIT_RESIZE_TOP_RIGHT;
        }
        if (left && bottom) {
            return SKIN_HIT_RESIZE_BOTTOM_LEFT;
        }
        if (right && bottom) {
            return SKIN_HIT_RESIZE_BOTTOM_RIGHT;
        }
        if (left) {
            return SKIN_HIT_RESIZE_LEFT;
        }
        if (right) {
            return SKIN_HIT_RESIZE_RIGHT;
        }
        if (top) {
            return SKIN_HIT_RESIZE_TOP;
        }
        if (bottom) {
            return SKIN_HIT_RESIZE_BOTTOM;
        }
    }

    for (size_t i = 0; i < SKIN_BUTTON_COUNT; ++i) {
        if (!layout.button_visible[i]) {
            continue;
        }

        if (rect_contains(layout.buttons[i], pointer_x, pointer_y)) {
            switch ((SkinButtonKind)i) {
                case SKIN_BUTTON_MINIMIZE:
                    return SKIN_HIT_MINIMIZE;
                case SKIN_BUTTON_MAXIMIZE:
                    return SKIN_HIT_MAXIMIZE;
                case SKIN_BUTTON_CLOSE:
                    return SKIN_HIT_CLOSE;
                default:
                    break;
            }
        }
    }

    if (rect_contains(layout.titlebar, pointer_x, pointer_y)) {
        return SKIN_HIT_TITLEBAR;
    }

    if (rect_contains(layout.client, pointer_x, pointer_y)) {
        return SKIN_HIT_CLIENT;
    }

    return SKIN_HIT_NONE;
}

static void draw_texture(
    const SkinRenderer *renderer,
    SkinRect rect,
    SkinTexture texture,
    SkinColor base,
    float strength
)
{
    if (renderer == NULL ||
        renderer->draw_line == NULL ||
        texture == SKIN_TEXTURE_NONE ||
        strength <= 0.0f) {
        return;
    }

    strength = skin_clamp_float(strength, 0.0f, 1.0f);

    SkinColor light = skin_lerp(
        base,
        skin_rgb(255, 255, 255),
        0.35f * strength
    );

    SkinColor dark = skin_lerp(
        base,
        skin_rgb(0, 0, 0),
        0.25f * strength
    );

    switch (texture) {
        case SKIN_TEXTURE_BRUSHED:
            for (int y = rect.y + 1; y < rect.y + rect.height; y += 3) {
                renderer->draw_line(
                    renderer->userdata,
                    rect.x,
                    y,
                    rect.x + rect.width - 1,
                    y,
                    light
                );
            }
            break;

        case SKIN_TEXTURE_GLASS:
            renderer->fill_rect(
                renderer->userdata,
                (SkinRect){
                    rect.x,
                    rect.y,
                    rect.width,
                    rect.height / 2
                },
                light
            );
            break;

        case SKIN_TEXTURE_GRID:
            for (int x = rect.x; x < rect.x + rect.width; x += 4) {
                renderer->draw_line(
                    renderer->userdata,
                    x,
                    rect.y,
                    x,
                    rect.y + rect.height - 1,
                    light
                );
            }
            for (int y = rect.y; y < rect.y + rect.height; y += 4) {
                renderer->draw_line(
                    renderer->userdata,
                    rect.x,
                    y,
                    rect.x + rect.width - 1,
                    y,
                    dark
                );
            }
            break;

        case SKIN_TEXTURE_STRIPES:
            for (int offset = -rect.height; offset < rect.width; offset += 6) {
                renderer->draw_line(
                    renderer->userdata,
                    rect.x + offset,
                    rect.y + rect.height - 1,
                    rect.x + offset + rect.height,
                    rect.y,
                    light
                );
            }
            break;

        case SKIN_TEXTURE_NOISE:
            for (int y = rect.y; y < rect.y + rect.height; y += 5) {
                for (int x = rect.x; x < rect.x + rect.width; x += 7) {
                    if (((x * 13 + y * 7) & 3) == 0) {
                        renderer->fill_rect(
                            renderer->userdata,
                            (SkinRect){x, y, 1, 1},
                            light
                        );
                    }
                }
            }
            break;

        case SKIN_TEXTURE_NONE:
        default:
            break;
    }
}

static void draw_button_icon(
    const SkinRenderer *renderer,
    SkinRect rect,
    SkinButtonKind kind,
    SkinColor color
)
{
    if (renderer == NULL || renderer->draw_line == NULL) {
        return;
    }

    const int cx = rect.x + rect.width / 2;
    const int cy = rect.y + rect.height / 2;

    switch (kind) {
        case SKIN_BUTTON_MINIMIZE:
            renderer->draw_line(
                renderer->userdata,
                cx - 5,
                cy + 3,
                cx + 5,
                cy + 3,
                color
            );
            break;

        case SKIN_BUTTON_MAXIMIZE:
            renderer->draw_rect_outline(
                renderer->userdata,
                (SkinRect){cx - 5, cy - 5, 10, 10},
                color
            );
            break;

        case SKIN_BUTTON_CLOSE:
            renderer->draw_line(
                renderer->userdata,
                cx - 5,
                cy - 5,
                cx + 5,
                cy + 5,
                color
            );
            renderer->draw_line(
                renderer->userdata,
                cx + 5,
                cy - 5,
                cx - 5,
                cy + 5,
                color
            );
            break;

        default:
            break;
    }
}

static SkinHit hit_for_button(SkinButtonKind kind)
{
    switch (kind) {
        case SKIN_BUTTON_MINIMIZE:
            return SKIN_HIT_MINIMIZE;
        case SKIN_BUTTON_MAXIMIZE:
            return SKIN_HIT_MAXIMIZE;
        case SKIN_BUTTON_CLOSE:
            return SKIN_HIT_CLOSE;
        default:
            return SKIN_HIT_NONE;
    }
}

void skin_draw_window_chrome(
    const SkinRenderer *renderer,
    const SkinWindow *window,
    const WindowSkin *skin,
    SkinHit hot,
    SkinHit pressed
)
{
    if (renderer == NULL ||
        window == NULL ||
        skin == NULL ||
        renderer->fill_rect == NULL ||
        renderer->draw_rect_outline == NULL) {
        return;
    }

    SkinChromeLayout layout = skin_calculate_layout(window, skin);

    if (skin->shadow_style != SKIN_SHADOW_NONE) {
        if (skin->shadow_style == SKIN_SHADOW_HARD) {
            SkinRect shadow = layout.outer;
            shadow.x += skin->metrics.shadow_size;
            shadow.y += skin->metrics.shadow_size;
            renderer->fill_rect(
                renderer->userdata,
                shadow,
                skin->colors.shadow
            );
        } else {
            for (int layer = skin->metrics.shadow_size; layer > 0; --layer) {
                float t =
                    1.0f -
                    ((float)layer /
                     (float)(skin->metrics.shadow_size + 1));

                SkinColor color = skin->colors.shadow;
                color.a = (uint8_t)(color.a * t * t);

                renderer->draw_rect_outline(
                    renderer->userdata,
                    (SkinRect){
                        layout.outer.x - layer,
                        layout.outer.y - layer,
                        layout.outer.width + layer * 2,
                        layout.outer.height + layer * 2
                    },
                    color
                );
            }
        }
    }

    SkinColor frame = window->focused
        ? skin->colors.frame
        : skin->colors.frame_inactive;

    frame.a = (uint8_t)(
        255.0f *
        skin_clamp_float(
            skin->border_transparency,
            0.0f,
            1.0f
        )
    );

    renderer->fill_rect(
        renderer->userdata,
        layout.outer,
        frame
    );

    if (skin->border_style == SKIN_BORDER_FLAT) {
        renderer->draw_rect_outline(
            renderer->userdata,
            layout.outer,
            skin->colors.border_light
        );
    } else if (skin->border_style == SKIN_BORDER_BEVEL) {
        renderer->draw_rect_outline(
            renderer->userdata,
            layout.outer,
            skin->colors.border_light
        );

        renderer->draw_rect_outline(
            renderer->userdata,
            (SkinRect){
                layout.outer.x + 1,
                layout.outer.y + 1,
                layout.outer.width - 2,
                layout.outer.height - 2
            },
            skin->colors.border_dark
        );
    } else if (skin->border_style == SKIN_BORDER_DOUBLE) {
        renderer->draw_rect_outline(
            renderer->userdata,
            layout.outer,
            skin->colors.border_light
        );

        renderer->draw_rect_outline(
            renderer->userdata,
            (SkinRect){
                layout.outer.x + 2,
                layout.outer.y + 2,
                layout.outer.width - 4,
                layout.outer.height - 4
            },
            skin->colors.border_light
        );
    }

    SkinColor titlebar = window->focused
        ? skin->colors.titlebar
        : skin->colors.titlebar_inactive;

    renderer->fill_rect(
        renderer->userdata,
        layout.titlebar,
        titlebar
    );

    if (skin->border_reflection > 0.0f) {
        SkinColor reflection = skin_rgb(255, 255, 255);
        reflection.a = (uint8_t)(
            100.0f *
            skin_clamp_float(
                skin->border_reflection,
                0.0f,
                1.0f
            )
        );

        renderer->draw_line(
            renderer->userdata,
            layout.titlebar.x + 2,
            layout.titlebar.y + 1,
            layout.titlebar.x +
                layout.titlebar.width - 3,
            layout.titlebar.y + 1,
            reflection
        );
    }

    for (size_t i = 0; i < SKIN_BUTTON_COUNT; ++i) {
        SkinButtonKind kind = (SkinButtonKind)i;
        SkinHit button_hit = hit_for_button(kind);

        SkinColor normal = kind == SKIN_BUTTON_CLOSE
            ? skin->colors.close_button
            : skin->colors.button;

        SkinColor hover = kind == SKIN_BUTTON_CLOSE
            ? skin->colors.close_button_hover
            : skin->colors.button_hover;

        SkinColor color = normal;

        if (pressed == button_hit) {
            color = skin->colors.button_pressed;
        } else if (hot == button_hit) {
            color = hover;
        }

        renderer->fill_rect(
            renderer->userdata,
            layout.buttons[kind],
            color
        );

        draw_texture(
            renderer,
            layout.buttons[kind],
            skin->button_texture,
            color,
            skin->button_texture_strength
        );

        draw_button_icon(
            renderer,
            layout.buttons[kind],
            kind,
            skin->colors.button_icon
        );
    }

    if (skin->show_title &&
        renderer->draw_text != NULL &&
        window->title != NULL) {
        SkinColor title_color = window->focused
            ? skin->colors.title_text
            : skin->colors.title_text_inactive;

        int title_x =
            layout.titlebar.x +
            skin->metrics.title_padding;

        if (skin->buttons_on_left) {
            title_x +=
                SKIN_BUTTON_COUNT *
                (skin->metrics.button_width +
                 skin->metrics.button_spacing);
        }

        renderer->draw_text(
            renderer->userdata,
            title_x,
            layout.titlebar.y +
                layout.titlebar.height / 2,
            window->title,
            title_color
        );
    }
}

bool skin_begin_drag(
    SkinDragState *drag,
    const SkinWindow *window,
    SkinHit hit,
    int pointer_x,
    int pointer_y
)
{
    if (drag == NULL ||
        window == NULL ||
        !window->movable) {
        return false;
    }

    /*
     * Full-window drag:
     * allow dragging from the titlebar or client area.
     * Buttons and resize regions do not start a move.
     */
    if (hit != SKIN_HIT_TITLEBAR &&
        hit != SKIN_HIT_CLIENT) {
        return false;
    }

    drag->active = true;
    drag->pointer_x = pointer_x;
    drag->pointer_y = pointer_y;
    drag->window_start_x = window->x;
    drag->window_start_y = window->y;

    return true;
}

void skin_update_drag(
    SkinDragState *drag,
    SkinWindow *window,
    int pointer_x,
    int pointer_y,
    SkinRect desktop_bounds
)
{
    if (drag == NULL ||
        window == NULL ||
        !drag->active) {
        return;
    }

    const int delta_x = pointer_x - drag->pointer_x;
    const int delta_y = pointer_y - drag->pointer_y;

    const int max_x =
        desktop_bounds.x +
        desktop_bounds.width -
        window->width;

    const int max_y =
        desktop_bounds.y +
        desktop_bounds.height -
        window->height;

    window->x = skin_clamp_int(
        drag->window_start_x + delta_x,
        desktop_bounds.x,
        max_x
    );

    window->y = skin_clamp_int(
        drag->window_start_y + delta_y,
        desktop_bounds.y,
        max_y
    );
}

void skin_end_drag(SkinDragState *drag)
{
    if (drag != NULL) {
        drag->active = false;
    }
}
