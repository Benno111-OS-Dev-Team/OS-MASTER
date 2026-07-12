# OS8 Project Status

Last updated: 2026-07-11

## Current Snapshot

- Window manager: active and now wired to use the `newwindows` chrome and hit-test layer for `GUI_WINDOW_CHROME_SYSTEM` windows.
- Desktop shell: present with dock, menus, settings, browser, notes, calculator, media, installer, and startup/login flows.
- Rendering paths: framebuffer path is active, with optional GPU and blur features varying by backend.
- Build targets: multi-arch build flow exists for `arm64`, `x86_64`, and `x86`.

## Recently Completed

- Integrated `newwindows` into the main window manager draw path for standard system windows.
- Replaced duplicated system-window button, titlebar, and resize hit logic with `newwindows` hit testing.
- Added kernel build wiring so the shared `newwindows` source is compiled as part of OS builds.

## In Progress

- Validate the new chrome integration on desktop runs and tune metrics and colors across themes.
- Confirm maximize, minimize, resize, and drag behavior across special windows like installer, startup, and custom app surfaces.
- Decide whether `GUI_WINDOW_CHROME_MINIMAL` should remain custom or migrate onto the same adapter.

## Known Gaps

- `newwindows` integration currently targets `GUI_WINDOW_CHROME_SYSTEM`; minimal and framebuffer chrome paths still use legacy rendering.
- Hover and pressed visual states from `newwindows` are not yet threaded through compositor pointer state.
- Shadow and blur data from `newwindows` is only partially mapped; compositor-level blur integration still needs deeper work.

## Next Suggested Work

1. Add hover and pressed tracking so chrome buttons animate the way `newwindows` expects.
2. Move minimize, maximize, and restore flows into shared helpers to reduce repeated window-state code.
3. Evaluate a second skin profile for minimal chrome.
4. Add a focused GUI smoke-test checklist for window interactions on `x86_64` desktop builds.
//embed the cursor.svg file and render the cursor 