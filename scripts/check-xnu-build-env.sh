#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
  echo "usage: $0 <arch> <xnu-source-dir>" >&2
  exit 2
fi

arch="$1"
xnu_dir="$2"
sdkroot="${SDKROOT:-macosx}"
kdkroot="${KDKROOT:-}"

case "$arch" in
  x86_64|arm64) ;;
  *)
    echo "error: XNU build environment supports ARCH=x86_64 or ARCH=arm64, got: $arch" >&2
    exit 1
    ;;
esac

if [ "$(uname -s)" != "Darwin" ]; then
  echo "error: real XNU compilation requires macOS; use XNU_SOURCE_VALIDATION_ONLY=1 for source validation on this host" >&2
  exit 1
fi

for tool in git make xcodebuild xcrun tar; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: required XNU build tool is missing: $tool" >&2
    exit 1
  fi
done

sdk_path="$(xcrun --sdk "$sdkroot" --show-sdk-path)"
if [ ! -d "$sdk_path" ]; then
  echo "error: SDKROOT=$sdkroot resolved to a missing SDK path: $sdk_path" >&2
  exit 1
fi

xnu_abs="$(cd "$xnu_dir" && pwd -P)"
git_top="$(git -C "$xnu_dir" rev-parse --show-toplevel 2>/dev/null || true)"
if [ -z "$git_top" ]; then
  echo "error: XNU source must be a standalone git checkout for real compilation: $xnu_dir" >&2
  exit 1
fi

git_top_abs="$(cd "$git_top" && pwd -P)"
if [ "$git_top_abs" != "$xnu_abs" ]; then
  echo "error: XNU source is not a standalone checkout: $xnu_dir" >&2
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
  echo "error: XNU source checkout must be clean before compilation" >&2
  exit 1
fi

if [ "$arch" = "arm64" ]; then
  if [ -z "$kdkroot" ]; then
    echo "error: ARCH=arm64 real XNU builds require KDKROOT to point at a matching Kernel Debug Kit" >&2
    exit 1
  fi
  if [ ! -d "$kdkroot" ]; then
    echo "error: KDKROOT does not exist: $kdkroot" >&2
    exit 1
  fi
fi

echo "[XNU] Build environment ready"
echo "[XNU] SDKROOT=$sdkroot"
echo "[XNU] SDK path=$sdk_path"
echo "[XNU] Source origin=$origin"
echo "[XNU] Source commit=$(git -C "$xnu_dir" rev-parse HEAD)"
if [ -n "$kdkroot" ]; then
  echo "[XNU] KDKROOT=$kdkroot"
fi
