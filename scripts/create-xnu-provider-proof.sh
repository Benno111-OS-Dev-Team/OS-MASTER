#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 4 ] || [ "$#" -gt 6 ]; then
  echo "usage: $0 <arch> <provider-manifest> <provider-archive> <kernel-artifact> [uefi-image] [smoke-log]" >&2
  exit 2
fi

arch="$1"
provider_manifest="$2"
provider_archive="$3"
kernel_artifact="$4"
uefi_image="${5:-}"
smoke_log="${6:-}"

case "$arch" in
  x86_64|arm64) ;;
  *)
    echo "error: XNU provider proof supports ARCH=x86_64 or ARCH=arm64, got: $arch" >&2
    exit 1
    ;;
esac

manifest_value() {
  awk -F= -v key="$1" '$1 == key { print substr($0, length(key) + 2); found=1; exit } END { if (!found) exit 1 }' "$2"
}

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{ print $1 }'
  else
    shasum -a 256 "$1" | awk '{ print $1 }'
  fi
}

for required in "$provider_manifest" "$provider_archive" "$kernel_artifact"; do
  if [ ! -s "$required" ]; then
    echo "error: XNU provider proof input is missing or empty: $required" >&2
    exit 1
  fi
done

provider="$(manifest_value provider "$provider_manifest")"
provider_arch="$(manifest_value arch "$provider_manifest")"
source_origin="$(manifest_value source_origin "$provider_manifest")"
source_commit="$(manifest_value source_commit "$provider_manifest")"
source_state="$(manifest_value source_state "$provider_manifest")"
provider_mode="$(manifest_value mode "$provider_manifest")"

if [ "$provider" != "xnu" ] || [ "$provider_arch" != "$arch" ]; then
  echo "error: XNU provider proof manifest does not match provider/architecture" >&2
  exit 1
fi
if [ "$source_origin" != "https://github.com/apple-oss-distributions/xnu.git" ] ||
   [ "$source_state" != "clean" ] ||
   [ "$provider_mode" != "compiled" ]; then
  echo "error: XNU provider proof requires compiled media from a clean official XNU checkout" >&2
  exit 1
fi
if ! printf '%s\n' "$source_commit" | grep -Eq '^[0-9a-f]{40}$'; then
  echo "error: XNU provider proof source_commit is not a 40-character SHA-1: $source_commit" >&2
  exit 1
fi

if [ "$arch" = "x86_64" ]; then
  if [ ! -s "$uefi_image" ]; then
    echo "error: x86_64 XNU provider proof requires a compiled UEFI image" >&2
    exit 1
  fi
  if [ ! -s "$smoke_log" ]; then
    echo "error: x86_64 XNU provider proof requires a compiled smoke log" >&2
    exit 1
  fi
  grep -q 'OS8 Startup Executable' "$smoke_log"
  grep -q 'Kernel verified and loaded. Exiting boot services' "$smoke_log"
  grep -q 'Entered ExitBootServices; jumping to XNU kernel' "$smoke_log"
fi

proof_dir="$(dirname "$provider_manifest")/../xnu-proof"
proof_manifest="$proof_dir/xnu-provider-proof.manifest"
mkdir -p "$proof_dir"

{
  printf 'proof_version=1\n'
  printf 'provider=xnu\n'
  printf 'arch=%s\n' "$arch"
  printf 'mode=compiled\n'
  printf 'source_origin=%s\n' "$source_origin"
  printf 'source_commit=%s\n' "$source_commit"
  printf 'source_state=%s\n' "$source_state"
  printf 'provider_manifest=%s\n' "$provider_manifest"
  printf 'provider_archive=%s\n' "$provider_archive"
  printf 'provider_archive_sha256=%s\n' "$(sha256_file "$provider_archive")"
  printf 'kernel_artifact=%s\n' "$kernel_artifact"
  printf 'kernel_artifact_sha256=%s\n' "$(sha256_file "$kernel_artifact")"
  if [ "$arch" = "x86_64" ]; then
    printf 'compiled_uefi_image=%s\n' "$uefi_image"
    printf 'compiled_uefi_image_sha256=%s\n' "$(sha256_file "$uefi_image")"
    printf 'compiled_smoke_log=%s\n' "$smoke_log"
    printf 'compiled_smoke_log_sha256=%s\n' "$(sha256_file "$smoke_log")"
    printf 'compiled_smoke_marker=startup-and-post-exit-handoff\n'
  fi
} > "$proof_manifest"

echo "[XNU] Provider proof manifest created: $proof_manifest"
