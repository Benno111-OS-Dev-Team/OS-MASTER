#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import shlex
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
OVERLAY_ROOT = ROOT / "freebsd-overlay"

EXECUTABLE_RELATIVE_PATHS = {
    "root/os-master-x11-common.sh",
    "usr/libexec/bsdinstall/local.pre-everything",
    "usr/libexec/bsdinstall/local.post-configure",
    "usr/local/bin/os-master-session",
    "usr/local/bin/os-master-launch",
}

ADDITIONAL_FILES: list[tuple[Path, str, int]] = []
DOTFILE_PATH_OVERRIDES = {
    "root/dot.xinitrc": "/root/.xinitrc",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Overlay OS-MASTER installer customizations onto a FreeBSD ISO."
    )
    parser.add_argument("--input", required=True, help="Path to the verified upstream ISO")
    parser.add_argument("--output", required=True, help="Path to write the customized ISO")
    parser.add_argument(
        "--cache-root",
        help="Optional directory for persistent extracted-tree cache and working files",
    )
    return parser.parse_args()


def require_tool(name: str) -> None:
    if shutil.which(name) is None:
        raise SystemExit(f"[ERROR] Required tool not found: {name}")


def iso_path_from_relative_path(relative_path: str) -> str:
    override = DOTFILE_PATH_OVERRIDES.get(relative_path)
    if override:
        return override
    return f"/{relative_path}"


def get_overlay_files() -> list[tuple[Path, str, int]]:
    overlay_files: list[tuple[Path, str, int]] = []

    for source in sorted(OVERLAY_ROOT.rglob("*")):
        if not source.is_file():
            continue
        relative_path = source.relative_to(OVERLAY_ROOT).as_posix()
        file_mode = 0o755 if relative_path in EXECUTABLE_RELATIVE_PATHS else 0o644
        overlay_files.append(
            (source, iso_path_from_relative_path(relative_path), file_mode)
        )

    overlay_files.extend(ADDITIONAL_FILES)
    return overlay_files


def stage_overlay_files(tmp_dir: Path) -> list[tuple[Path, str, int]]:
    staged: list[tuple[Path, str, int]] = []
    for source, iso_path, file_mode in get_overlay_files():
        destination = tmp_dir / iso_path.lstrip("/")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        destination.chmod(file_mode)
        staged.append((destination, iso_path, file_mode))
    return staged


def get_boot_replay_options(input_iso: Path) -> list[str]:
    result = subprocess.run(
        [
            "xorriso",
            "-indev",
            str(input_iso),
            "-report_el_torito",
            "as_mkisofs",
            "-report_system_area",
            "as_mkisofs",
        ],
        check=False,
        capture_output=True,
        text=True,
    )

    options: list[str] = []
    combined_output = "\n".join([result.stdout, result.stderr])

    for line in combined_output.splitlines():
        stripped = line.strip()
        if stripped.startswith("-"):
            options.extend(shlex.split(stripped))

    if result.returncode not in (0, 32):
        raise SystemExit(
            "[ERROR] xorriso could not report boot options from source ISO\n"
            f"{combined_output}"
        )

    if not options:
        raise SystemExit(
            "[ERROR] Could not determine boot options from source ISO\n"
            f"{combined_output}"
        )

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


def hardlink_or_copy(source: str, destination: str) -> str:
    try:
        os.link(source, destination)
    except OSError:
        shutil.copy2(source, destination)
    return destination


def clone_tree(source_root: Path, destination_root: Path) -> None:
    shutil.copytree(
        source_root,
        destination_root,
        symlinks=True,
        copy_function=hardlink_or_copy,
    )


def apply_overlay(extract_root: Path, staged_files: list[tuple[Path, str, int]]) -> None:
    for source, iso_path, file_mode in staged_files:
        relative_path = iso_path.lstrip("/")
        destination = extract_root / relative_path
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists() or destination.is_symlink():
            destination.unlink()
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


def source_cache_identity(input_iso: Path) -> str:
    stat_result = input_iso.stat()
    return f"{input_iso.name}|{stat_result.st_size}|{stat_result.st_mtime_ns}"


def ensure_cached_extract(input_iso: Path, cache_root: Path) -> Path:
    cache_extract_root = cache_root / "source-extract"
    cache_identity_path = cache_root / "source-extract.id"
    current_identity = source_cache_identity(input_iso)
    cached_identity = ""

    if cache_identity_path.exists():
        cached_identity = cache_identity_path.read_text(encoding="utf-8").strip()

    if cache_extract_root.exists() and cached_identity == current_identity:
        return cache_extract_root

    refresh_root = cache_root / "source-extract.refresh"
    shutil.rmtree(refresh_root, ignore_errors=True)
    refresh_root.mkdir(parents=True, exist_ok=True)

    try:
        extract_iso_tree(input_iso, refresh_root)
        cache_identity_path.write_text(current_identity, encoding="utf-8")
        shutil.rmtree(cache_extract_root, ignore_errors=True)
        refresh_root.replace(cache_extract_root)
    finally:
        shutil.rmtree(refresh_root, ignore_errors=True)

    return cache_extract_root


def main() -> int:
    args = parse_args()
    require_tool("xorriso")

    input_iso = Path(args.input).resolve()
    output_iso = Path(args.output).resolve()
    output_iso.parent.mkdir(parents=True, exist_ok=True)
    if output_iso.exists():
        output_iso.unlink()

    cache_root = Path(args.cache_root).resolve() if args.cache_root else None
    if cache_root is not None:
        cache_root.mkdir(parents=True, exist_ok=True)
        temp_root = cache_root / "overlay-staging"
        extract_root = cache_root / "work-extract"
    else:
        temp_root = output_iso.parent / ".overlay-staging"
        extract_root = output_iso.parent / ".iso-extract"

    if temp_root.exists():
        shutil.rmtree(temp_root)
    if extract_root.exists():
        shutil.rmtree(extract_root)
    temp_root.mkdir(parents=True)

    try:
        staged_files = stage_overlay_files(temp_root)
        if cache_root is not None:
            source_extract_root = ensure_cached_extract(input_iso, cache_root)
            clone_tree(source_extract_root, extract_root)
        else:
            extract_root.mkdir(parents=True)
            extract_iso_tree(input_iso, extract_root)
        apply_overlay(extract_root, staged_files)
        build_repacked_iso(input_iso, extract_root, output_iso)
    finally:
        shutil.rmtree(temp_root, ignore_errors=True)
        shutil.rmtree(extract_root, ignore_errors=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
