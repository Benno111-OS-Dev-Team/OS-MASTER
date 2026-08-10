#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <arch> <build-dir> <xnu-source-dir>" >&2
  exit 2
fi

arch="$1"
build_dir="$2"
xnu_dir="$3"

case "$arch" in
  x86_64) xnu_arch="X86_64" ;;
  arm64) xnu_arch="ARM64" ;;
  *)
    echo "error: XNU provider supports ARCH=x86_64 or ARCH=arm64, got: $arch" >&2
    exit 1
    ;;
esac

kernel_dir="$build_dir/kernel"
xnu_build_root="$build_dir/xnu"
provider_artifact="$kernel_dir/xnu-$arch.kernel"
manifest="$kernel_dir/xnu-provider.manifest"
mkdir -p "$kernel_dir" "$xnu_build_root/obj" "$xnu_build_root/sym" "$xnu_build_root/dst"

xnu_abs="$(cd "$xnu_dir" && pwd -P)"
source_origin="unversioned"
source_commit="unavailable"
source_state="unversioned"
git_top="$(git -C "$xnu_dir" rev-parse --show-toplevel 2>/dev/null || true)"
if [ -n "$git_top" ]; then
  git_top_abs="$(cd "$git_top" && pwd -P)"
  if [ "$git_top_abs" = "$xnu_abs" ]; then
    source_origin="$(git -C "$xnu_dir" remote get-url origin 2>/dev/null || printf 'none')"
    source_commit="$(git -C "$xnu_dir" rev-parse HEAD)"
    if git -C "$xnu_dir" diff --quiet && git -C "$xnu_dir" diff --cached --quiet; then
      source_state="clean"
    else
      source_state="modified"
    fi
  else
    source_origin="parent-repository"
    source_commit="unavailable"
    source_state="not-standalone-git-tree"
  fi
fi

host="$(uname -s)"
if [ "$host" != "Darwin" ]; then
  if [ "${XNU_SOURCE_VALIDATION_ONLY:-0}" = "1" ]; then
    {
      printf 'provider=xnu\n'
      printf 'arch=%s\n' "$arch"
      printf 'source=%s\n' "$xnu_dir"
      printf 'source_origin=%s\n' "$source_origin"
      printf 'source_commit=%s\n' "$source_commit"
      printf 'source_state=%s\n' "$source_state"
      printf 'artifact=%s\n' "$provider_artifact"
      printf 'mode=source-validation\n'
      printf 'reason=XNU kernel compilation requires macOS, Xcode, and matching Apple kernel dependencies\n'
    } > "$manifest"
    echo "[XNU] Source provider validation recorded: $manifest"
    exit 0
  fi

  echo "error: building XNU requires macOS with Xcode and matching Apple kernel dependencies" >&2
  echo "hint: CI source validation can set XNU_SOURCE_VALIDATION_ONLY=1" >&2
  exit 1
fi

sdkroot="${SDKROOT:-macosx}"
kernel_configs="${KERNEL_CONFIGS:-RELEASE}"

echo "[XNU] Building external XNU provider without writing to $xnu_dir"
make -C "$xnu_dir" \
  SDKROOT="$sdkroot" \
  ARCH_CONFIGS="$xnu_arch" \
  KERNEL_CONFIGS="$kernel_configs" \
  OBJROOT="$xnu_build_root/obj" \
  SYMROOT="$xnu_build_root/sym" \
  DSTROOT="$xnu_build_root/dst"

xnu_kernel="$(find "$xnu_build_root/obj" "$xnu_build_root/sym" "$xnu_build_root/dst" \
  -type f \( -name 'kernel' -o -name 'kernel.*' \) | head -n1)"

if [ -z "$xnu_kernel" ]; then
  echo "error: XNU build finished but no kernel artifact was found under $xnu_build_root" >&2
  exit 1
fi

cp "$xnu_kernel" "$provider_artifact"
{
  printf 'provider=xnu\n'
  printf 'arch=%s\n' "$arch"
  printf 'source=%s\n' "$xnu_dir"
  printf 'source_origin=%s\n' "$source_origin"
  printf 'source_commit=%s\n' "$source_commit"
  printf 'source_state=%s\n' "$source_state"
  printf 'artifact=%s\n' "$provider_artifact"
  printf 'mode=compiled\n'
} > "$manifest"

echo "[XNU] Kernel provider artifact staged: $provider_artifact"
