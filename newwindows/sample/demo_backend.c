#include "demo_backend.h"

#include <stdio.h>

static void demo_fill_rect(
    void *userdata,
    SkinRect rect,
    SkinColor color
)
{
    (void)userdata;

    printf(
        "fill_rect x=%d y=%d w=%d h=%d rgba=(%u,%u,%u,%u)\n",
        rect.x,
        rect.y,
        rect.width,
        rect.height,
        color.r,
        color.g,
        color.b,
        color.a
    );
}

static void demo_draw_rect_outline(
    void *userdata,
    SkinRect rect,
    SkinColor color
)
{
    (void)userdata;

    printf(
        "outline x=%d y=%d w=%d h=%d rgba=(%u,%u,%u,%u)\n",
        rect.x,
        rect.y,
        rect.width,
        rect.height,
        color.r,
        color.g,
        color.b,
        color.a
    );
}

static void demo_draw_line(
    void *userdata,
    int x1,
    int y1,
    int x2,
    int y2,
    SkinColor color
)
{
    (void)userdata;

    printf(
        "line (%d,%d)->(%d,%d) rgba=(%u,%u,%u,%u)\n",
        x1,
        y1,
        x2,
        y2,
        color.r,
        color.g,
        color.b,
        color.a
    );
}

static void demo_draw_text(
    void *userdata,
    int x,
    int y,
    const char *text,
    SkinColor color
)
{
    (void)userdata;

    printf(
        "text x=%d y=%d \"%s\" rgba=(%u,%u,%u,%u)\n",
        x,
        y,
        text != NULL ? text : "",
        color.r,
        color.g,
        color.b,
        color.a
    );
}

SkinRenderer demo_create_renderer(void)
{
    SkinRenderer renderer = {
        .userdata = NULL,
        .fill_rect = demo_fill_rect,
        .draw_rect_outline = demo_draw_rect_outline,
        .draw_line = demo_draw_line,
        .draw_text = demo_draw_text
    };

    return renderer;
}
