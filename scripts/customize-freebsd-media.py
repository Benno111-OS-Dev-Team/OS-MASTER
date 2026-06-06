#!/usr/bin/env python3

from __future__ import annotations

import argparse
import shutil
from pathlib import Path

import pycdlib


OVERLAY_ROOT = Path(__file__).resolve().parent.parent / "freebsd-overlay"

OVERLAY_FILES = [
    (
        OVERLAY_ROOT / "root" / "os-master-x11-common.sh",
        "/ROOT/OSMX11.SH;1",
        "os-master-x11-common.sh",
        0o755,
    ),
    (
        OVERLAY_ROOT / "root" / "dot.xinitrc",
        "/ROOT/DOTXINIT.;1",
        ".xinitrc",
        0o644,
    ),
    (
        OVERLAY_ROOT / "root" / "OS-MASTER-X11.txt",
        "/ROOT/OSMX11.TXT;1",
        "OS-MASTER-X11.txt",
        0o644,
    ),
    (
        OVERLAY_ROOT / "usr" / "libexec" / "bsdinstall" / "local.pre-everything",
        "/USR/LIBEXEC/BSDINSTALL/LOCALPRE.;1",
        "local.pre-everything",
        0o755,
    ),
    (
        OVERLAY_ROOT / "usr" / "libexec" / "bsdinstall" / "local.post-configure",
        "/USR/LIBEXEC/BSDINSTALL/LOCALPOS.;1",
        "local.post-configure",
        0o755,
    ),
    (
        OVERLAY_ROOT / "usr" / "share" / "skel" / "dot.xinitrc",
        "/USR/SHARE/SKEL/DOTXINIT.;1",
        "dot.xinitrc",
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


def stage_overlay_files(tmp_dir: Path) -> list[tuple[Path, str, str, int]]:
    staged = []
    for source, iso_path, rr_name, file_mode in OVERLAY_FILES:
        destination = tmp_dir / source.name
        shutil.copy2(source, destination)
        staged.append((destination, iso_path, rr_name, file_mode))
    return staged


def main() -> int:
    args = parse_args()

    input_iso = Path(args.input).resolve()
    output_iso = Path(args.output).resolve()
    output_iso.parent.mkdir(parents=True, exist_ok=True)

    temp_root = output_iso.parent / ".overlay-staging"
    if temp_root.exists():
        shutil.rmtree(temp_root)
    temp_root.mkdir(parents=True)

    staged_files = stage_overlay_files(temp_root)

    iso = pycdlib.PyCdlib()
    iso.open(str(input_iso))
    for source, iso_path, rr_name, file_mode in staged_files:
        iso.add_file(
            str(source),
            iso_path=iso_path,
            rr_name=rr_name,
            file_mode=file_mode,
        )
    iso.write(str(output_iso))
    iso.close()

    shutil.rmtree(temp_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
