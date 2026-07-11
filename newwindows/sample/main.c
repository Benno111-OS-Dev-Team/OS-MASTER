#include "demo_backend.h"
#include "window_skin.h"

#include <stdio.h>

static const char *hit_name(SkinHit hit)
{
    switch (hit) {
        case SKIN_HIT_NONE: return "none";
        case SKIN_HIT_CLIENT: return "client";
        case SKIN_HIT_TITLEBAR: return "titlebar";
        case SKIN_HIT_MINIMIZE: return "minimize";
        case SKIN_HIT_MAXIMIZE: return "maximize";
        case SKIN_HIT_CLOSE: return "close";
        case SKIN_HIT_RESIZE_LEFT: return "resize-left";
        case SKIN_HIT_RESIZE_RIGHT: return "resize-right";
        case SKIN_HIT_RESIZE_TOP: return "resize-top";
        case SKIN_HIT_RESIZE_BOTTOM: return "resize-bottom";
        case SKIN_HIT_RESIZE_TOP_LEFT: return "resize-top-left";
        case SKIN_HIT_RESIZE_TOP_RIGHT: return "resize-top-right";
        case SKIN_HIT_RESIZE_BOTTOM_LEFT: return "resize-bottom-left";
        case SKIN_HIT_RESIZE_BOTTOM_RIGHT: return "resize-bottom-right";
        default: return "unknown";
    }
}

int main(void)
{
    WindowSkin skin = skin_make_aurora_sample();

    SkinWindow window = {
        .x = 80,
        .y = 60,
        .width = 640,
        .height = 420,
        .focused = true,
        .resizable = true,
        .movable = true,
        .title = "Editson Skin Demo"
    };

    SkinRenderer renderer = demo_create_renderer();

    puts("=== original button order ===");
    skin_draw_window_chrome(
        &renderer,
        &window,
        &skin,
        SKIN_HIT_MAXIMIZE,
        SKIN_HIT_NONE
    );

    skin_swap_minimize_maximize(&skin);

    puts("\n=== minimize/maximize swapped ===");
    skin_draw_window_chrome(
        &renderer,
        &window,
        &skin,
        SKIN_HIT_CLOSE,
        SKIN_HIT_CLOSE
    );

    SkinHit hit = skin_hit_test(
        &window,
        &skin,
        200,
        200
    );

    printf("\nhit: %s\n", hit_name(hit));

    SkinDragState drag = {0};

    if (skin_begin_drag(
            &drag,
            &window,
            hit,
            200,
            200)) {
        skin_update_drag(
            &drag,
            &window,
            360,
            280,
            (SkinRect){0, 0, 1280, 720}
        );

        skin_end_drag(&drag);
    }

    printf(
        "window after drag: x=%d y=%d\n",
        window.x,
        window.y
    );

    return 0;
}
