# C Window Skin Sample

A plain C11 window chrome/skinning sample intended to be adapted into an existing window manager, framebuffer desktop, compositor or custom OS GUI.

## Features

- C11-only core
- No heap allocation required
- Abstract rendering callback interface
- Per-skin metrics and colors
- Swappable minimize/maximize button order
- Textured buttons
  - brushed
  - glass
  - noise
  - grid
  - diagonal stripes
- Border transparency parameter
- Border blur parameter exposed to the compositor
- Border reflection parameter
- Hard and soft shadow examples
- Chrome layout generation
- Resize hit testing
- Titlebar and client hit testing
- Full-window dragging
- Desktop-bound drag clamping
- Built-in Aurora sample skin

## Build

### Make

```sh
make
make run
```

### CMake

```sh
cmake -S . -B build
cmake --build build
./build/window_skin_demo
```

## Adapting it to your windowing system

The main integration point is `SkinRenderer` in:

```text
include/window_skin.h
```

Replace the demo callbacks with your own graphics functions:

```c
SkinRenderer renderer = {
    .userdata = framebuffer,
    .fill_rect = my_fill_rect,
    .draw_rect_outline = my_draw_rect_outline,
    .draw_line = my_draw_line,
    .draw_text = my_draw_text
};
```

Then call:

```c
SkinHit hot = skin_hit_test(
    &window,
    &skin,
    mouse_x,
    mouse_y
);

skin_draw_window_chrome(
    &renderer,
    &window,
    &skin,
    hot,
    pressed_hit
);
```

## Full-window drag

The sample deliberately allows a move to start from either the titlebar or client area:

```c
if (hit != SKIN_HIT_TITLEBAR &&
    hit != SKIN_HIT_CLIENT) {
    return false;
}
```

Change that to only `SKIN_HIT_TITLEBAR` for conventional window dragging.

## Blur

`border_blur` is stored in the skin, but the sample software renderer does not perform a real Gaussian blur.

A real compositor should use it when sampling the pixels behind the window chrome:

```text
desktop backbuffer
        |
        v
sample behind window border
        |
        v
blur radius = skin.border_blur
        |
        v
blend frame using skin.border_transparency
        |
        v
draw reflection and border
```

This keeps expensive blur work out of the chrome layout code.

## Recommended integration order

1. Replace `SkinRenderer` callbacks.
2. Map your own `Window` structure to `SkinWindow`.
3. Use `skin_calculate_layout()` for both drawing and mouse hit testing.
4. Use `skin_hit_test()` before handling clicks.
5. Feed `border_blur` to your compositor.
6. Replace the built-in sample with JSON-loaded or compiled skins.
