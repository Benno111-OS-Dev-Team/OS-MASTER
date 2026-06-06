#!/usr/bin/env python3

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
OVERLAY_ROOT = ROOT / "freebsd-overlay"

OVERLAY_FILES = [
    (
        OVERLAY_ROOT / "root" / "os-master-x11-common.sh",
        "/root/os-master-x11-common.sh",
        0o755,
    ),
    (
        OVERLAY_ROOT / "root" / "dot.xinitrc",
        "/root/.xinitrc",
        0o644,
    ),
    (
        OVERLAY_ROOT / "root" / "OS-MASTER-X11.txt",
        "/root/OS-MASTER-X11.txt",
        0o644,
    ),
    (
        OVERLAY_ROOT / "usr" / "libexec" / "bsdinstall" / "local.pre-everything",
        "/usr/libexec/bsdinstall/local.pre-everything",
        0o755,
    ),
    (
        OVERLAY_ROOT / "usr" / "libexec" / "bsdinstall" / "local.post-configure",
        "/usr/libexec/bsdinstall/local.post-configure",
        0o755,
    ),
    (
        OVERLAY_ROOT / "usr" / "share" / "skel" / "dot.xinitrc",
        "/usr/share/skel/dot.xinitrc",
        0o644,
    ),
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Overlay OS-MASTER installer customizations onto a FreeBSD ISO."
    )
    parser.add_argument("--input", required=True, help="Path to the verified upstream ISO")
    parser.add_argument("--output", required=True, help="Path to write the customized ISO")
    return parser.parse_args()


def require_tool(name: str) -> None:
    if shutil.which(name) is None:
        raise SystemExit(f"[ERROR] Required tool not found: {name}")


def stage_overlay_files(tmp_dir: Path) -> list[tuple[Path, str]]:
    staged: list[tuple[Path, str]] = []
    for source, iso_path, file_mode in OVERLAY_FILES:
        destination = tmp_dir / source.name
        shutil.copy2(source, destination)
        destination.chmod(file_mode)
        staged.append((destination, iso_path))
    return staged


def build_xorriso_command(input_iso: Path, output_iso: Path, staged_files: list[tuple[Path, str]]) -> list[str]:
    command = [
        "xorriso",
        "-indev",
        str(input_iso),
        "-outdev",
        str(output_iso),
        "-overwrite",
        "on",
    ]
    for source, iso_path in staged_files:
        command.extend(["-map", str(source), iso_path])
    command.extend(["-boot_image", "any", "replay"])
    command.append("-end")
    return command


def main() -> int:
    args = parse_args()
    require_tool("xorriso")

    input_iso = Path(args.input).resolve()
    output_iso = Path(args.output).resolve()
    output_iso.parent.mkdir(parents=True, exist_ok=True)
    if output_iso.exists():
        output_iso.unlink()

    temp_root = output_iso.parent / ".overlay-staging"
    if temp_root.exists():
        shutil.rmtree(temp_root)
    temp_root.mkdir(parents=True)

    try:
        staged_files = stage_overlay_files(temp_root)
        command = build_xorriso_command(input_iso, output_iso, staged_files)
        subprocess.run(command, check=True)
    finally:
        shutil.rmtree(temp_root, ignore_errors=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
