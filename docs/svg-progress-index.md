# SVG Progress Index

This file tracks the current state of the in-kernel SVG renderer while the remaining spec-alignment work is still in progress.

## Current focus

- Keep the tree compiling while the renderer expansion continues.
- Preserve the newer SVG features already landed in `kernel/media/media.c`.
- Record major implemented areas so follow-up work has a single starting point.

## Implemented renderer areas

- Basic structure: `svg`, `g`, nested `svg`, `symbol`, `use`, `switch`, and `a`.
- Shapes and geometry: `path`, `polyline`, `polygon`, `line`, `rect`, `circle`, and `ellipse`.
- Path commands: move, line, horizontal, vertical, cubic, smooth cubic, quadratic, smooth quadratic, arc, and close-path.
- Number-list parsing: shared SVG number parsing now accepts comma and whitespace separators, improving compatibility across path data, point lists, transforms, and viewBox-style attribute lists.
- Paint and stroke styling: fill, stroke, `currentColor`, opacity variants, fill/stroke rules, stroke caps, joins, miter limit, dash array, and dash offset, including comma/whitespace-separated dash lists.
- Style parsing: presentation attributes, inline `style=""`, and embedded stylesheet rule collection.
- CSS selector support: element, `*`, `.class`, `#id`, `tag.class`, `tag#id`, and chained simple class/id suffixes.
- CSS robustness: comment skipping, CDATA/XML comment wrappers, basic `@media screen` and `@media all`, unsupported at-rule skipping, and `!important` trimming.
- Viewport handling: `viewBox`, `preserveAspectRatio`, root width/height, root transform, and root `x`/`y`.
- Gradients: linear and radial gradients with inheritance, transform support, spread modes, stop interpolation, and stop-level `currentColor` resolution through gradient/stop style context.
- Images: embedded PNG/JPEG data URIs, file-backed PNG/JPEG/SVG `href` loading, aspect-ratio handling, opacity-aware blitting, and full affine transform rendering for `<image>`.
- Text: `text`, nested `tspan`, inherited font size, anchor alignment, spacing controls, text length adjustment, and basic rotate handling.

## Known status

- The broader SVG-spec expansion goal is not complete yet.
- The immediate repository priority for this pass is restoring successful compilation and keeping progress documented.

## Build validation

- Focused verification currently passes with:
  `clang -fsyntax-only -Ikernel -Ikernel/include -Ishared-api -Inewwindows/include -I. kernel/media/media.c`
- A full repo kernel build was not completed from this Windows shell because `make` is not available in the current host environment.
