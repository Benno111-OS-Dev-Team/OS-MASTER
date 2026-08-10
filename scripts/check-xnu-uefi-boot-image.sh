#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work_dir="${TMPDIR:-/tmp}/os8-xnu-uefi-image-check.$$"
build_dir="$work_dir/build/x86_64"
image_dir="$work_dir/image"
kernel="$build_dir/kernel/xnu-x86_64.kernel"
image="$image_dir/xnu-x86_64-uefi.img"

cleanup() {
  rm -rf "$work_dir"
}
trap cleanup EXIT

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: required command is missing: $1" >&2
    exit 1
  fi
}

require_cmd clang
require_cmd lld-link
require_cmd mformat
require_cmd mmd
require_cmd mcopy
require_cmd mdir
require_cmd mtype

mkdir -p "$build_dir/kernel" "$image_dir"
printf 'synthetic-xnu-kernel-for-uefi-image-check\n' > "$kernel"

KERNEL_PATH="$kernel" KERNEL_FORMAT=xnu \
  bash "$root/scripts/build-custom-uefi.sh" "$build_dir" >/dev/null
bash "$root/scripts/create-xnu-uefi-boot-image.sh" \
  "$build_dir" "$image_dir" "$kernel" >/dev/null

test -s "$image"
mdir -i "$image" ::/EFI/BOOT/BOOTX64.EFI >/dev/null
mdir -i "$image" ::/EFI/OS8/STARTUPX64.EFI >/dev/null
mdir -i "$image" ::/EFI/OS8/os8boot.cfg >/dev/null
mdir -i "$image" ::/boot/main.sys >/dev/null
mtype -i "$image" ::/EFI/OS8/os8boot.cfg | grep -q '^kernel_format=xnu$'
mtype -i "$image" ::/EFI/OS8/os8boot.cfg | grep -q '^kernel_path=\\boot\\main.sys$'
mtype -i "$image" ::/boot/main.sys | grep -q '^synthetic-xnu-kernel-for-uefi-image-check$'

echo "[XNU] x86_64 UEFI boot image verified"
