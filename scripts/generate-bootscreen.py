#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter


ROOT = Path(__file__).resolve().parent.parent
LOGO_PATH = ROOT / "assets" / "logo.png"
OUTPUT_PATH = ROOT / "assets" / "boot-assets" / "bootscreen.png"

CANVAS_SIZE = (1920, 1080)
BACKGROUND = (0, 0, 0, 255)
TRACK_COLOR = (28, 28, 30, 255)
FILL_COLOR = (245, 245, 247, 255)
SHADOW_COLOR = (255, 255, 255, 30)


def draw_round_bar(draw: ImageDraw.ImageDraw, box: tuple[int, int, int, int], color: tuple[int, int, int, int]) -> None:
    radius = (box[3] - box[1]) // 2
    draw.rounded_rectangle(box, radius=radius, fill=color)


def main() -> int:
    canvas = Image.new("RGBA", CANVAS_SIZE, BACKGROUND)
    logo = Image.open(LOGO_PATH).convert("RGBA")

    target_logo_width = 250
    scale = target_logo_width / logo.width
    target_logo_size = (target_logo_width, int(logo.height * scale))
    logo = logo.resize(target_logo_size, Image.LANCZOS)

    center_x = CANVAS_SIZE[0] // 2
    center_y = CANVAS_SIZE[1] // 2 - 70

    logo_pos = (
        center_x - logo.width // 2,
        center_y - logo.height // 2,
    )

    glow = Image.new("RGBA", CANVAS_SIZE, (0, 0, 0, 0))
    glow_draw = ImageDraw.Draw(glow)

    track_width = 360
    track_height = 18
    track_top = logo_pos[1] + logo.height + 42
    track_left = center_x - track_width // 2
    track_box = (track_left, track_top, track_left + track_width, track_top + track_height)

    fill_width = 54
    fill_inset = 2
    fill_box = (
        track_left + fill_inset,
        track_top + fill_inset,
        track_left + fill_inset + fill_width,
        track_top + track_height - fill_inset,
    )

    shadow_box = (
        fill_box[0] - 4,
        fill_box[1] - 2,
        fill_box[2] + 4,
        fill_box[3] + 2,
    )
    draw_round_bar(glow_draw, shadow_box, SHADOW_COLOR)
    glow = glow.filter(ImageFilter.GaussianBlur(radius=8))

    canvas.alpha_composite(glow)
    canvas.alpha_composite(logo, logo_pos)

    draw = ImageDraw.Draw(canvas)
    draw_round_bar(draw, track_box, TRACK_COLOR)
    draw_round_bar(draw, fill_box, FILL_COLOR)

    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(OUTPUT_PATH)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
