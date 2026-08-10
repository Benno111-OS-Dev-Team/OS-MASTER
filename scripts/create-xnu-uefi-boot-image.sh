#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <build-dir> <image-dir> <kernel-artifact>" >&2
  exit 2
fi

build_dir="$1"
image_dir="$2"
kernel_artifact="$3"
uefi_dir="$build_dir/boot/custom-uefi"
image_path="$image_dir/xnu-x86_64-uefi.img"
staging_dir="$build_dir/xnu-uefi-root"

require_file() {
  if [ ! -s "$1" ]; then
    echo "error: required XNU UEFI boot input is missing: $1" >&2
    exit 1
  fi
}

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: required command is missing: $1" >&2
    exit 1
  fi
}

require_file "$kernel_artifact"
require_file "$uefi_dir/BOOTX64.EFI"
require_file "$uefi_dir/STARTUPX64.EFI"
require_file "$uefi_dir/os8boot.cfg"
require_cmd mformat
require_cmd mmd
require_cmd mcopy

if ! grep -q '^kernel_format=xnu$' "$uefi_dir/os8boot.cfg"; then
  echo "error: XNU UEFI config must declare kernel_format=xnu" >&2
  exit 1
fi

mkdir -p "$image_dir"
rm -rf "$staging_dir"
mkdir -p "$staging_dir/EFI/BOOT" "$staging_dir/EFI/OS8" "$staging_dir/boot"

cp "$uefi_dir/BOOTX64.EFI" "$staging_dir/EFI/BOOT/BOOTX64.EFI"
cp "$uefi_dir/STARTUPX64.EFI" "$staging_dir/EFI/OS8/STARTUPX64.EFI"
cp "$uefi_dir/os8boot.cfg" "$staging_dir/EFI/OS8/os8boot.cfg"
cp "$kernel_artifact" "$staging_dir/boot/main.sys"

cat > "$staging_dir/XNU-UEFI.txt" <<EOF
XNU x86_64 UEFI boot image

The XNU kernel payload is staged at /boot/main.sys.
The startup config at /EFI/OS8/os8boot.cfg declares kernel_format=xnu.
The external XNU source tree remains read-only input.
EOF

kernel_kib="$(du -sk "$kernel_artifact" | awk '{ print $1 }')"
image_mib=$(((kernel_kib / 1024) + 32))
if [ "$image_mib" -lt 32 ]; then
  image_mib=32
fi

rm -f "$image_path"
dd if=/dev/zero of="$image_path" bs=1M count="$image_mib" status=none
mformat -i "$image_path" -v XNUUEFI ::
mmd -i "$image_path" ::/EFI
mmd -i "$image_path" ::/EFI/BOOT
mmd -i "$image_path" ::/EFI/OS8
mmd -i "$image_path" ::/boot
mcopy -i "$image_path" "$staging_dir/EFI/BOOT/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$image_path" "$staging_dir/EFI/OS8/STARTUPX64.EFI" ::/EFI/OS8/STARTUPX64.EFI
mcopy -i "$image_path" "$staging_dir/EFI/OS8/os8boot.cfg" ::/EFI/OS8/os8boot.cfg
mcopy -i "$image_path" "$staging_dir/boot/main.sys" ::/boot/main.sys
mcopy -i "$image_path" "$staging_dir/XNU-UEFI.txt" ::/XNU-UEFI.txt

echo "[XNU] UEFI boot image created: $image_path"
