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
LOGIN_BACKGROUND_OUTPUT_PATH = (
    ROOT
    / "freebsd-overlay"
    / "usr"
    / "local"
    / "share"
    / "os-master-desktop"
    / "login"
    / "background.png"
)
BOOTSPLASH_INSTALLER_DIR = (
    ROOT
    / "freebsd-overlay"
    / "usr"
    / "local"
    / "share"
    / "os-master-desktop"
    / "bootsplash"
    / "installer"
)
BOOTSPLASH_DESKTOP_DIR = (
    ROOT
    / "freebsd-overlay"
    / "usr"
    / "local"
    / "share"
    / "os-master-desktop"
    / "bootsplash"
    / "desktop"
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


def generate_login_background() -> None:
    canvas_size = (1600, 900)
    canvas = Image.new("RGBA", canvas_size, (4, 8, 16, 255))

    if BOOTSCREEN_PATH.exists():
        background = Image.open(BOOTSCREEN_PATH).convert("RGBA")
        background = cover_resize(background, canvas_size)
        background = background.filter(ImageFilter.GaussianBlur(radius=16))
        background.putalpha(135)
        canvas.alpha_composite(background)

    overlay = Image.new("RGBA", canvas_size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    draw.rectangle((0, 0, canvas_size[0], canvas_size[1]), fill=(4, 8, 16, 162))
    draw.ellipse((1020, -120, 1680, 500), fill=(56, 189, 248, 70))
    draw.ellipse((-240, 420, 520, 1160), fill=(244, 63, 94, 58))
    draw.rounded_rectangle((470, 180, 1130, 720), radius=38, fill=(10, 15, 27, 178))
    canvas.alpha_composite(overlay)

    logo = Image.open(LOGO_PATH).convert("RGBA")
    target_logo_width = 240
    scale = target_logo_width / logo.width
    logo = logo.resize((target_logo_width, int(logo.height * scale)), Image.LANCZOS)
    logo_pos = ((canvas_size[0] - logo.width) // 2, 240)
    canvas.alpha_composite(logo, logo_pos)

    draw = ImageDraw.Draw(canvas)
    draw.text((630, 430), "OS-MASTER", fill=(248, 250, 252, 255))
    draw.text((545, 475), "Sign in to launch the restored desktop.", fill=(203, 213, 225, 255))
    draw.rounded_rectangle((560, 560, 1040, 570), radius=5, fill=(248, 250, 252, 65))
    draw.rounded_rectangle((560, 560, 760, 570), radius=5, fill=(248, 250, 252, 210))

    LOGIN_BACKGROUND_OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    canvas.convert("RGB").save(LOGIN_BACKGROUND_OUTPUT_PATH)


def render_dynamic_splash_frame(
    output_dir: Path,
    title: str,
    subtitle: str,
    accent: tuple[int, int, int, int],
) -> None:
    canvas_size = (1600, 900)
    frame_count = 9
    output_dir.mkdir(parents=True, exist_ok=True)

    logo = Image.open(LOGO_PATH).convert("RGBA")
    target_logo_width = 260
    scale = target_logo_width / logo.width
    logo = logo.resize((target_logo_width, int(logo.height * scale)), Image.LANCZOS)

    for index in range(frame_count):
        progress = index / (frame_count - 1)
        canvas = Image.new("RGBA", canvas_size, (3, 7, 16, 255))

        if BOOTSCREEN_PATH.exists():
            background = Image.open(BOOTSCREEN_PATH).convert("RGBA")
            background = cover_resize(background, canvas_size)
            background = background.filter(ImageFilter.GaussianBlur(radius=18))
            background.putalpha(118)
            canvas.alpha_composite(background)

        overlay = Image.new("RGBA", canvas_size, (0, 0, 0, 0))
        draw = ImageDraw.Draw(overlay)
        draw.rectangle((0, 0, canvas_size[0], canvas_size[1]), fill=(4, 9, 18, 168))
        draw.ellipse((1060, -140, 1740, 520), fill=(accent[0], accent[1], accent[2], 72))
        draw.ellipse((-260, 460, 500, 1180), fill=(239, 68, 68, 48))
        draw.rounded_rectangle((430, 200, 1170, 700), radius=42, fill=(10, 15, 27, 196))
        canvas.alpha_composite(overlay)

        logo_pos = ((canvas_size[0] - logo.width) // 2, 250)
        canvas.alpha_composite(logo, logo_pos)

        draw = ImageDraw.Draw(canvas)
        draw.text((595, 430), title, fill=(248, 250, 252, 255))
        draw.text((470, 478), subtitle, fill=(203, 213, 225, 255))

        bar_left = 540
        bar_top = 575
        bar_right = 1060
        bar_bottom = 597
        fill_right = int(bar_left + (bar_right - bar_left) * progress)

        draw.rounded_rectangle((bar_left, bar_top, bar_right, bar_bottom), radius=11, fill=(26, 32, 44, 220))
        draw.rounded_rectangle((bar_left, bar_top, fill_right, bar_bottom), radius=11, fill=accent)
        draw.text((688, 625), f"{int(progress * 100):d}% ready", fill=(226, 232, 240, 255))

        frame_path = output_dir / f"frame-{index:02d}.png"
        canvas.convert("RGB").save(frame_path)


def generate_dynamic_bootsplash_frames() -> None:
    render_dynamic_splash_frame(
        BOOTSPLASH_INSTALLER_DIR,
        "OS-MASTER Installer",
        "Preparing the graphical installer environment.",
        (245, 245, 247, 255),
    )
    render_dynamic_splash_frame(
        BOOTSPLASH_DESKTOP_DIR,
        "OS-MASTER Desktop",
        "Preparing the restored desktop session.",
        (56, 189, 248, 255),
    )


def main() -> int:
    generate_boot_splash()
    generate_installer_wallpaper()
    generate_login_background()
    generate_dynamic_bootsplash_frames()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
