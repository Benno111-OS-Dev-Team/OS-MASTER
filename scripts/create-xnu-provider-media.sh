#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 4 ]; then
  echo "usage: $0 <arch> <build-dir> <image-dir> <kernel-artifact>" >&2
  exit 2
fi

arch="$1"
build_dir="$2"
image_dir="$3"
kernel_artifact="$4"

manifest="$build_dir/kernel/xnu-provider.manifest"
media_root="$build_dir/xnu-provider-media"
archive="$image_dir/xnu-$arch-provider.tar.gz"
boot_contract="docs/XNU_BOOT_CONTRACT.md"
handoff_abi="boot/xnu/xnu_boot_handoff.h"
handoff_builder="boot/xnu/xnu_boot_handoff_builder.h"
macho_loader="boot/xnu/xnu_macho_loader.h"
uefi_handoff="boot/xnu/xnu_uefi_handoff.h"
x86_64_boot_args="boot/xnu/xnu_x86_64_boot_args.h"
x86_64_entry_handoff="boot/custom/startup-handoff.S"
x86_64_startup_loader="boot/custom/startup.c"
handoff_script="scripts/create-xnu-boot-handoff.sh"
boot_plan_script="scripts/create-xnu-boot-plan.sh"
handoff_manifest="$build_dir/xnu-boot/xnu-boot-handoff.manifest"
boot_plan_manifest="$build_dir/xnu-boot/xnu-boot-plan.manifest"

if [ ! -f "$manifest" ]; then
  echo "error: XNU provider manifest is missing: $manifest" >&2
  exit 1
fi
if [ ! -f "$boot_contract" ]; then
  echo "error: XNU boot contract is missing: $boot_contract" >&2
  exit 1
fi
if [ ! -f "$handoff_abi" ]; then
  echo "error: XNU boot handoff ABI is missing: $handoff_abi" >&2
  exit 1
fi
if [ ! -f "$handoff_builder" ]; then
  echo "error: XNU boot handoff builder is missing: $handoff_builder" >&2
  exit 1
fi
if [ ! -f "$macho_loader" ]; then
  echo "error: XNU Mach-O loader contract is missing: $macho_loader" >&2
  exit 1
fi
if [ ! -f "$uefi_handoff" ]; then
  echo "error: XNU UEFI handoff helper is missing: $uefi_handoff" >&2
  exit 1
fi
if [ ! -f "$x86_64_boot_args" ]; then
  echo "error: XNU x86_64 boot args builder is missing: $x86_64_boot_args" >&2
  exit 1
fi
if [ ! -f "$x86_64_entry_handoff" ]; then
  echo "error: XNU x86_64 entry handoff shim is missing: $x86_64_entry_handoff" >&2
  exit 1
fi
if [ ! -f "$x86_64_startup_loader" ]; then
  echo "error: XNU x86_64 startup loader is missing: $x86_64_startup_loader" >&2
  exit 1
fi
if [ ! -x "$handoff_script" ]; then
  echo "error: XNU boot handoff generator is missing or not executable: $handoff_script" >&2
  exit 1
fi
if [ ! -x "$boot_plan_script" ]; then
  echo "error: XNU boot plan generator is missing or not executable: $boot_plan_script" >&2
  exit 1
fi

rm -rf "$media_root"
mkdir -p "$media_root/kernel" "$media_root/metadata" "$media_root/docs" "$media_root/boot/xnu" "$media_root/boot/custom" "$image_dir"

cp "$manifest" "$media_root/metadata/xnu-provider.manifest"
cp "$boot_contract" "$media_root/docs/XNU_BOOT_CONTRACT.md"
cp "$handoff_abi" "$media_root/boot/xnu/xnu_boot_handoff.h"
cp "$handoff_builder" "$media_root/boot/xnu/xnu_boot_handoff_builder.h"
cp "$macho_loader" "$media_root/boot/xnu/xnu_macho_loader.h"
cp "$uefi_handoff" "$media_root/boot/xnu/xnu_uefi_handoff.h"
cp "$x86_64_boot_args" "$media_root/boot/xnu/xnu_x86_64_boot_args.h"
cp "$x86_64_entry_handoff" "$media_root/boot/custom/startup-handoff.S"
cp "$x86_64_startup_loader" "$media_root/boot/custom/startup.c"
if [ -f "$kernel_artifact" ]; then
  cp "$kernel_artifact" "$media_root/kernel/$(basename "$kernel_artifact")"
  payload_mode="compiled"
else
  payload_mode="source-validation"
fi

bash "$handoff_script" "$arch" "$build_dir" "$kernel_artifact" "$payload_mode"
bash "$boot_plan_script" "$arch" "$build_dir" "$kernel_artifact" "$payload_mode"
cp "$handoff_manifest" "$media_root/metadata/xnu-boot-handoff.manifest"
cp "$boot_plan_manifest" "$media_root/metadata/xnu-boot-plan.manifest"

{
  printf 'provider=xnu\n'
  printf 'arch=%s\n' "$arch"
  printf 'kernel_artifact=%s\n' "$kernel_artifact"
  printf 'payload_mode=%s\n' "$payload_mode"
  printf 'external_source_policy=read-only\n'
  printf 'boot_contract=docs/XNU_BOOT_CONTRACT.md\n'
  printf 'handoff_abi=boot/xnu/xnu_boot_handoff.h\n'
  printf 'handoff_builder=boot/xnu/xnu_boot_handoff_builder.h\n'
  printf 'macho_loader=boot/xnu/xnu_macho_loader.h\n'
  printf 'uefi_handoff=boot/xnu/xnu_uefi_handoff.h\n'
  printf 'x86_64_boot_args=boot/xnu/xnu_x86_64_boot_args.h\n'
  printf 'x86_64_entry_handoff=boot/custom/startup-handoff.S\n'
  printf 'x86_64_startup_loader=boot/custom/startup.c\n'
  printf 'boot_handoff=metadata/xnu-boot-handoff.manifest\n'
  printf 'boot_plan=metadata/xnu-boot-plan.manifest\n'
} > "$media_root/metadata/media.manifest"

cat > "$media_root/README.txt" <<EOF
XNU provider media for $arch

This archive is produced from the selected XNU kernel provider.
The XNU source tree is external read-only input; generated files are staged
under build/ and image/ only.
EOF

rm -f "$archive"
tar -C "$media_root" -czf "$archive" .

echo "[XNU] Provider media archive created: $archive"
