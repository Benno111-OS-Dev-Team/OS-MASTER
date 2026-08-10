#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work_dir="${TMPDIR:-/tmp}/os8-xnu-uefi-image-check.$$"
build_dir="$work_dir/build/x86_64"
image_dir="$work_dir/image"
kernel="$build_dir/kernel/xnu-x86_64.kernel"
image="$image_dir/xnu-x86_64-uefi.img"
startup_source="$root/boot/custom/startup.c"

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

grep -q 'load_xnu_macho64_segments' "$startup_source"
grep -q 'alloc_zero_pages_below(total / 4096' "$startup_source"
grep -q 'OS8_XNU_BOOT_ARGS_MAX_ADDRESS' "$startup_source"
grep -q 'build_xnu_boot_args_pre_exit' "$startup_source"
if grep -q 'startup_enter_xnu_kernel(pml4_phys, entry, 0)' "$startup_source"; then
  echo "error: XNU startup loader can enter without boot_args" >&2
  exit 1
fi

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
