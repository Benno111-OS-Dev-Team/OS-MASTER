#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter


ROOT = Path(__file__).resolve().parent.parent
BOOTSCREEN_PATH = ROOT / "assets" / "boot-assets" / "bootscreen.png"
LOGO_PATH = ROOT / "assets" / "logo.png"
SPLASH_OUTPUT_PATH = ROOT / "freebsd-overlay" / "boot" / "os-master-splash.bmp"
WALLPAPER_OUTPUT_PATH = (
    ROOT
    / "freebsd-overlay"
    / "usr"
    / "local"
    / "share"
    / "os-master-desktop"
    / "installer"
    / "wallpaper.png"
)


def cover_resize(image: Image.Image, size: tuple[int, int]) -> Image.Image:
    width, height = size
    scale = max(width / image.width, height / image.height)
    resized = image.resize(
        (int(image.width * scale), int(image.height * scale)),
        Image.LANCZOS,
    )
    left = max((resized.width - width) // 2, 0)
    top = max((resized.height - height) // 2, 0)
    return resized.crop((left, top, left + width, top + height))


def generate_boot_splash() -> None:
    image = Image.open(BOOTSCREEN_PATH).convert("RGB")
    splash = cover_resize(image, (320, 200)).quantize(colors=256, method=Image.MEDIANCUT)
    SPLASH_OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    splash.save(SPLASH_OUTPUT_PATH, format="BMP")


def generate_installer_wallpaper() -> None:
    canvas_size = (1600, 900)
    canvas = Image.new("RGBA", canvas_size, (7, 10, 18, 255))

    if BOOTSCREEN_PATH.exists():
      background = Image.open(BOOTSCREEN_PATH).convert("RGBA")
      background = cover_resize(background, canvas_size)
      background = background.filter(ImageFilter.GaussianBlur(radius=20))
      background.putalpha(110)
      canvas.alpha_composite(background)

    overlay = Image.new("RGBA", canvas_size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    draw.rectangle((0, 0, canvas_size[0], canvas_size[1]), fill=(7, 10, 18, 140))
    draw.ellipse((1150, -180, 1750, 420), fill=(37, 99, 235, 70))
    draw.ellipse((-220, 520, 440, 1160), fill=(219, 39, 119, 65))
    draw.rectangle((0, 720, canvas_size[0], 900), fill=(15, 23, 42, 210))
    canvas.alpha_composite(overlay)

    logo = Image.open(LOGO_PATH).convert("RGBA")
    target_logo_width = 280
    scale = target_logo_width / logo.width
    logo = logo.resize((target_logo_width, int(logo.height * scale)), Image.LANCZOS)
    logo_pos = ((canvas_size[0] - logo.width) // 2, 225)
    canvas.alpha_composite(logo, logo_pos)

    draw = ImageDraw.Draw(canvas)
    draw.rounded_rectangle((520, 500, 1080, 544), radius=22, fill=(20, 25, 35, 190))
    draw.rounded_rectangle((532, 510, 700, 534), radius=12, fill=(245, 245, 247, 255))
    draw.text((555, 620), "OS-MASTER Graphical Installer", fill=(248, 250, 252, 255))
    draw.text((470, 666), "Launch bsdinstall from the desktop to begin setup.", fill=(203, 213, 225, 255))

    WALLPAPER_OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    canvas.convert("RGB").save(WALLPAPER_OUTPUT_PATH)


def main() -> int:
    generate_boot_splash()
    generate_installer_wallpaper()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
