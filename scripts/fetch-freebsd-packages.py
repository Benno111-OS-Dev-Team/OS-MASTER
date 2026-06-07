#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import shutil
import tarfile
import urllib.request
from pathlib import Path


DEFAULT_PACKAGES = [
    "xorg",
    "xinit",
    "xdm",
    "dbus",
    "openbox",
    "tint2",
    "pcmanfm",
    "xterm",
    "feh",
    "firefox",
    "gtk3",
    "adwaita-icon-theme",
    "hicolor-icon-theme",
    "desktop-file-utils",
    "shared-mime-info",
    "gvfs",
    "dejavu",
    "liberation-fonts-ttf",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Fetch a FreeBSD offline package repo for OS-MASTER desktop packages."
    )
    parser.add_argument("--repo-url", required=True, help="Base URL of the FreeBSD pkg repo")
    parser.add_argument("--output-dir", required=True, help="Directory to write the offline repo")
    parser.add_argument(
        "--package",
        action="append",
        dest="packages",
        help="Package name to include; may be repeated",
    )
    return parser.parse_args()


def download_if_missing(url: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_file():
        return

    with urllib.request.urlopen(url) as response, destination.open("wb") as handle:
        shutil.copyfileobj(response, handle)


def load_packagesite(packagesite_path: Path) -> dict[str, dict[str, object]]:
    with tarfile.open(packagesite_path, "r:*") as archive:
        member = archive.extractfile("packagesite.yaml")
        if member is None:
            raise SystemExit(f"[ERROR] packagesite.yaml missing from {packagesite_path}")
        packages: dict[str, dict[str, object]] = {}
        for raw_line in member:
            line = raw_line.decode("utf-8").strip()
            if not line:
                continue
            entry = json.loads(line)
            packages[str(entry["name"])] = entry
        return packages


def resolve_package_closure(
    package_index: dict[str, dict[str, object]],
    package_names: list[str],
) -> dict[str, dict[str, object]]:
    resolved: dict[str, dict[str, object]] = {}
    pending = list(package_names)

    while pending:
        name = pending.pop()
        if name in resolved:
            continue
        if name not in package_index:
            raise SystemExit(f"[ERROR] Package not found in repo metadata: {name}")

        entry = package_index[name]
        resolved[name] = entry
        deps = entry.get("deps", {})
        if isinstance(deps, dict):
            pending.extend(str(dep_name) for dep_name in deps.keys())

    return resolved


def write_repo_manifest(output_dir: Path, repo_url: str, package_entries: dict[str, dict[str, object]]) -> None:
    manifest_path = output_dir / "os-master-offline-packages.txt"
    lines = [
        "OS-MASTER Offline Desktop Package Repo",
        "",
        f"Source repo: {repo_url}",
        f"Package count: {len(package_entries)}",
        "",
        "Resolved packages:",
    ]
    for package_name in sorted(package_entries):
        entry = package_entries[package_name]
        lines.append(f"  - {package_name} {entry['version']}")
    manifest_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    repo_url = args.repo_url.rstrip("/")
    output_dir = Path(args.output_dir).resolve()
    requested_packages = args.packages if args.packages else list(DEFAULT_PACKAGES)

    output_dir.mkdir(parents=True, exist_ok=True)

    meta_conf_path = output_dir / "meta.conf"
    packagesite_path = output_dir / "packagesite.pkg"

    download_if_missing(f"{repo_url}/meta.conf", meta_conf_path)
    download_if_missing(f"{repo_url}/packagesite.pkg", packagesite_path)

    package_index = load_packagesite(packagesite_path)
    resolved = resolve_package_closure(package_index, requested_packages)

    for entry in resolved.values():
        package_path = Path(str(entry["path"]))
        destination = output_dir / package_path
        download_if_missing(f"{repo_url}/{package_path.as_posix()}", destination)

    write_repo_manifest(output_dir, repo_url, resolved)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
