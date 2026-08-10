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

if [ ! -f "$manifest" ]; then
  echo "error: XNU provider manifest is missing: $manifest" >&2
  exit 1
fi
if [ ! -f "$boot_contract" ]; then
  echo "error: XNU boot contract is missing: $boot_contract" >&2
  exit 1
fi

rm -rf "$media_root"
mkdir -p "$media_root/kernel" "$media_root/metadata" "$media_root/docs" "$image_dir"

cp "$manifest" "$media_root/metadata/xnu-provider.manifest"
cp "$boot_contract" "$media_root/docs/XNU_BOOT_CONTRACT.md"
if [ -f "$kernel_artifact" ]; then
  cp "$kernel_artifact" "$media_root/kernel/$(basename "$kernel_artifact")"
  payload_mode="compiled"
else
  payload_mode="source-validation"
fi

{
  printf 'provider=xnu\n'
  printf 'arch=%s\n' "$arch"
  printf 'kernel_artifact=%s\n' "$kernel_artifact"
  printf 'payload_mode=%s\n' "$payload_mode"
  printf 'external_source_policy=read-only\n'
  printf 'boot_contract=docs/XNU_BOOT_CONTRACT.md\n'
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
