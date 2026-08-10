#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 4 ] || [ "$#" -gt 6 ]; then
  echo "usage: $0 <arch> <proof-manifest> <provider-manifest> <provider-archive> [uefi-image] [smoke-log]" >&2
  exit 2
fi

arch="$1"
proof_manifest="$2"
provider_manifest="$3"
provider_archive="$4"
uefi_image="${5:-}"
smoke_log="${6:-}"

case "$arch" in
  x86_64|arm64) ;;
  *)
    echo "error: XNU provider proof verifier supports ARCH=x86_64 or ARCH=arm64, got: $arch" >&2
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

canonical_path() {
  dir="$(dirname "$1")"
  base="$(basename "$1")"
  if [ ! -e "$dir/$base" ]; then
    printf '%s\n' "$1"
    return 0
  fi
  dir_abs="$(cd "$dir" && pwd -P)"
  printf '%s/%s\n' "$dir_abs" "$base"
}

for required in "$proof_manifest" "$provider_manifest" "$provider_archive"; do
  if [ ! -s "$required" ]; then
    echo "error: XNU provider proof verification input is missing or empty: $required" >&2
    exit 1
  fi
done

proof_version="$(manifest_value proof_version "$proof_manifest")"
proof_provider="$(manifest_value provider "$proof_manifest")"
proof_arch="$(manifest_value arch "$proof_manifest")"
proof_mode="$(manifest_value mode "$proof_manifest")"
proof_origin="$(manifest_value source_origin "$proof_manifest")"
proof_commit="$(manifest_value source_commit "$proof_manifest")"
proof_state="$(manifest_value source_state "$proof_manifest")"
proof_provider_manifest="$(manifest_value provider_manifest "$proof_manifest")"
proof_provider_archive="$(manifest_value provider_archive "$proof_manifest")"
proof_archive_hash="$(manifest_value provider_archive_sha256 "$proof_manifest")"
proof_kernel_artifact="$(manifest_value kernel_artifact "$proof_manifest")"
proof_kernel_hash="$(manifest_value kernel_artifact_sha256 "$proof_manifest")"

provider="$(manifest_value provider "$provider_manifest")"
provider_arch="$(manifest_value arch "$provider_manifest")"
provider_origin="$(manifest_value source_origin "$provider_manifest")"
provider_commit="$(manifest_value source_commit "$provider_manifest")"
provider_state="$(manifest_value source_state "$provider_manifest")"
provider_mode="$(manifest_value mode "$provider_manifest")"
provider_kernel="$(manifest_value artifact "$provider_manifest")"

if [ "$proof_version" != "1" ] ||
   [ "$proof_provider" != "xnu" ] ||
   [ "$proof_arch" != "$arch" ] ||
   [ "$proof_mode" != "compiled" ]; then
  echo "error: XNU provider proof identity fields are inconsistent" >&2
  exit 1
fi

if [ "$provider" != "xnu" ] ||
   [ "$provider_arch" != "$arch" ] ||
   [ "$provider_mode" != "compiled" ] ||
   [ "$provider_origin" != "https://github.com/apple-oss-distributions/xnu.git" ] ||
   [ "$provider_state" != "clean" ]; then
  echo "error: XNU provider proof must reference compiled media from a clean official XNU checkout" >&2
  exit 1
fi

if [ "$proof_origin" != "$provider_origin" ] ||
   [ "$proof_commit" != "$provider_commit" ] ||
   [ "$proof_state" != "$provider_state" ]; then
  echo "error: XNU provider proof source identity does not match provider manifest" >&2
  exit 1
fi

if [ "$(canonical_path "$proof_provider_manifest")" != "$(canonical_path "$provider_manifest")" ] ||
   [ "$(canonical_path "$proof_provider_archive")" != "$(canonical_path "$provider_archive")" ] ||
   [ "$(canonical_path "$proof_kernel_artifact")" != "$(canonical_path "$provider_kernel")" ]; then
  echo "error: XNU provider proof paths do not match provider inputs" >&2
  exit 1
fi

if [ "$proof_archive_hash" != "$(sha256_file "$provider_archive")" ]; then
  echo "error: XNU provider proof archive hash does not match provider archive" >&2
  exit 1
fi
if [ "$proof_kernel_hash" != "$(sha256_file "$provider_kernel")" ]; then
  echo "error: XNU provider proof kernel hash does not match kernel artifact" >&2
  exit 1
fi

if [ "$arch" = "x86_64" ]; then
  if [ ! -s "$uefi_image" ] || [ ! -s "$smoke_log" ]; then
    echo "error: x86_64 XNU provider proof verification requires UEFI image and smoke log inputs" >&2
    exit 1
  fi
  proof_uefi_image="$(manifest_value compiled_uefi_image "$proof_manifest")"
  proof_uefi_hash="$(manifest_value compiled_uefi_image_sha256 "$proof_manifest")"
  proof_smoke_log="$(manifest_value compiled_smoke_log "$proof_manifest")"
  proof_smoke_hash="$(manifest_value compiled_smoke_log_sha256 "$proof_manifest")"
  proof_smoke_marker="$(manifest_value compiled_smoke_marker "$proof_manifest")"
  if [ "$(canonical_path "$proof_uefi_image")" != "$(canonical_path "$uefi_image")" ] ||
     [ "$(canonical_path "$proof_smoke_log")" != "$(canonical_path "$smoke_log")" ] ||
     [ "$proof_smoke_marker" != "startup-and-post-exit-handoff" ]; then
    echo "error: x86_64 XNU provider proof handoff metadata is inconsistent" >&2
    exit 1
  fi
  if [ "$proof_uefi_hash" != "$(sha256_file "$uefi_image")" ]; then
    echo "error: x86_64 XNU provider proof UEFI image hash does not match" >&2
    exit 1
  fi
  if [ "$proof_smoke_hash" != "$(sha256_file "$smoke_log")" ]; then
    echo "error: x86_64 XNU provider proof smoke log hash does not match" >&2
    exit 1
  fi
  grep -q 'OS8 Startup Executable' "$smoke_log"
  grep -q 'Kernel verified and loaded. Exiting boot services' "$smoke_log"
  grep -q 'Entered ExitBootServices; jumping to XNU kernel' "$smoke_log"
fi

echo "[XNU] Provider proof verified: $proof_manifest"
