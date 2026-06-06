#!/usr/bin/env python3

from __future__ import annotations

import argparse
import shutil
import subprocess
import shlex
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


def stage_overlay_files(tmp_dir: Path) -> list[tuple[Path, str, int]]:
    staged: list[tuple[Path, str, int]] = []
    for source, iso_path, file_mode in OVERLAY_FILES:
        destination = tmp_dir / source.name
        shutil.copy2(source, destination)
        destination.chmod(file_mode)
        staged.append((destination, iso_path, file_mode))
    return staged


def get_boot_replay_options(input_iso: Path) -> list[str]:
    result = subprocess.run(
        ["xorriso", "-indev", str(input_iso), "-report_el_torito", "as_mkisofs"],
        check=True,
        capture_output=True,
        text=True,
    )

    options: list[str] = []
    for line in result.stdout.splitlines():
        stripped = line.strip()
        if stripped.startswith("-"):
            options.extend(shlex.split(stripped))

    if not options:
        raise SystemExit("[ERROR] Could not determine boot options from source ISO")

    return options


def extract_iso_tree(input_iso: Path, extract_root: Path) -> None:
    subprocess.run(
        [
            "xorriso",
            "-osirrox",
            "on",
            "-indev",
            str(input_iso),
            "-extract",
            "/",
            str(extract_root),
            "-end",
        ],
        check=True,
    )


def apply_overlay(extract_root: Path, staged_files: list[tuple[Path, str, int]]) -> None:
    for source, iso_path, file_mode in staged_files:
        relative_path = iso_path.lstrip("/")
        destination = extract_root / relative_path
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        destination.chmod(file_mode)


def build_repacked_iso(input_iso: Path, extract_root: Path, output_iso: Path) -> None:
    options = get_boot_replay_options(input_iso)
    command = [
        "xorriso",
        "-as",
        "mkisofs",
        "-r",
        "-J",
        "-joliet-long",
        "-o",
        str(output_iso),
        *options,
        str(extract_root),
    ]
    subprocess.run(command, check=True)


def main() -> int:
    args = parse_args()
    require_tool("xorriso")

    input_iso = Path(args.input).resolve()
    output_iso = Path(args.output).resolve()
    output_iso.parent.mkdir(parents=True, exist_ok=True)
    if output_iso.exists():
        output_iso.unlink()

    temp_root = output_iso.parent / ".overlay-staging"
    extract_root = output_iso.parent / ".iso-extract"
    if temp_root.exists():
        shutil.rmtree(temp_root)
    if extract_root.exists():
        shutil.rmtree(extract_root)
    temp_root.mkdir(parents=True)
    extract_root.mkdir(parents=True)

    try:
        staged_files = stage_overlay_files(temp_root)
        extract_iso_tree(input_iso, extract_root)
        apply_overlay(extract_root, staged_files)
        build_repacked_iso(input_iso, extract_root, output_iso)
    finally:
        shutil.rmtree(temp_root, ignore_errors=True)
        shutil.rmtree(extract_root, ignore_errors=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
