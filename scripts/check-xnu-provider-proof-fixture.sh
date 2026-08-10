#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work_dir="${TMPDIR:-/tmp}/os8-xnu-provider-proof-fixture.$$"
build_dir="$work_dir/build/x86_64"
image_dir="$work_dir/image"
provider_manifest="$build_dir/kernel/xnu-provider.manifest"
kernel_artifact="$build_dir/kernel/xnu-x86_64.kernel"
provider_archive="$image_dir/xnu-x86_64-provider.tar.gz"
uefi_image="$image_dir/xnu-x86_64-uefi.img"
smoke_log="$build_dir/xnu-smoke/qemu-serial.log"
proof_manifest="$build_dir/xnu-proof/xnu-provider-proof.manifest"

cleanup() {
  rm -rf "$work_dir"
}
trap cleanup EXIT

mkdir -p "$build_dir/kernel" "$build_dir/xnu-smoke" "$image_dir"

printf 'synthetic compiled xnu payload for proof fixture\n' > "$kernel_artifact"
printf 'synthetic compiled xnu uefi image for proof fixture\n' > "$uefi_image"
{
  printf 'OS8 Startup Executable\n'
  printf 'Kernel verified and loaded. Exiting boot services\n'
  printf 'Entered ExitBootServices; jumping to XNU kernel\n'
} > "$smoke_log"

tar -czf "$provider_archive" -C "$work_dir" build image

{
  printf 'provider=xnu\n'
  printf 'arch=x86_64\n'
  printf 'source=%s\n' "$work_dir/External/xnu"
  printf 'source_origin=https://github.com/apple-oss-distributions/xnu.git\n'
  printf 'source_commit=0123456789abcdef0123456789abcdef01234567\n'
  printf 'source_state=clean\n'
  printf 'artifact=%s\n' "$kernel_artifact"
  printf 'mode=compiled\n'
} > "$provider_manifest"

bash "$root/scripts/create-xnu-provider-proof.sh" \
  x86_64 \
  "$provider_manifest" \
  "$provider_archive" \
  "$kernel_artifact" \
  "$uefi_image" \
  "$smoke_log" >/dev/null

bash "$root/scripts/check-xnu-provider-proof.sh" \
  x86_64 \
  "$proof_manifest" \
  "$provider_manifest" \
  "$provider_archive" \
  "$uefi_image" \
  "$smoke_log" >/dev/null

printf 'tampered\n' >> "$smoke_log"
if bash "$root/scripts/check-xnu-provider-proof.sh" \
  x86_64 \
  "$proof_manifest" \
  "$provider_manifest" \
  "$provider_archive" \
  "$uefi_image" \
  "$smoke_log" >/dev/null 2>&1; then
  echo "error: XNU provider proof verifier accepted a tampered smoke log" >&2
  exit 1
fi

echo "[XNU] Provider proof fixture verified"
