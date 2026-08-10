#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

xnu_dir="${XNU_SOURCE_DIR:-$root/External/xnu}"

required_paths=(
  APPLE_LICENSE
  Makefile
  README.md
  bsd
  config
  iokit
  libkern
  libsa
  osfmk
  pexpert
)

if [ ! -d "$xnu_dir" ]; then
  echo "error: XNU source tree is missing at $xnu_dir" >&2
  echo "hint: fetch it with: git clone https://github.com/apple-oss-distributions/xnu.git External/xnu" >&2
  exit 1
fi

for rel in "${required_paths[@]}"; do
  if [ ! -e "$xnu_dir/$rel" ]; then
    echo "error: XNU source tree is missing required path: $rel" >&2
    exit 1
  fi
done

if git ls-files --error-unmatch External/xnu >/dev/null 2>&1; then
  echo "error: External/xnu contains tracked files; keep upstream XNU read-only and out of this repository" >&2
  exit 1
fi

git_top="$(git -C "$xnu_dir" rev-parse --show-toplevel 2>/dev/null || true)"
if [ -n "$git_top" ]; then
  xnu_abs="$(cd "$xnu_dir" && pwd -P)"
  git_top_abs="$(cd "$git_top" && pwd -P)"
  if [ "$git_top_abs" != "$xnu_abs" ]; then
    echo "error: XNU source must be a standalone checkout outside this repository's tracked tree" >&2
    exit 1
  fi
  origin="$(git -C "$xnu_dir" remote get-url origin 2>/dev/null || true)"
  if [ "$origin" != "https://github.com/apple-oss-distributions/xnu.git" ]; then
    echo "error: unexpected XNU source origin: ${origin:-none}" >&2
    exit 1
  fi
  source_status="$(git -C "$xnu_dir" status --porcelain=v1 --untracked-files=normal)"
  if [ -n "$source_status" ]; then
    printf '%s\n' "$source_status" >&2
    echo "error: XNU source checkout contains local changes or generated files" >&2
    exit 1
  fi
else
  echo "error: XNU source must be a standalone git checkout" >&2
  exit 1
fi

fs_type="$(findmnt -T "$xnu_dir" -no FSTYPE 2>/dev/null | head -n1 || true)"
if [ "$fs_type" = "9p" ] || [ "$fs_type" = "drvfs" ]; then
  echo "[XNU] Skipping permission-bit enforcement on Windows-mounted filesystem: $fs_type"
elif [ -w "$xnu_dir" ]; then
  echo "error: XNU source tree is writable; CI and local release builds must mark it read-only" >&2
  exit 1
fi

echo "[XNU] External source tree verified at $xnu_dir"
