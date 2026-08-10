#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo "usage: $0 <arch> <archive> [expected-payload-mode]" >&2
  exit 2
fi

arch="$1"
archive="$2"
expected_mode="${3:-}"

case "$arch" in
  x86_64|arm64) ;;
  *)
    echo "error: XNU provider media supports ARCH=x86_64 or ARCH=arm64, got: $arch" >&2
    exit 1
    ;;
esac

if [ ! -s "$archive" ]; then
  echo "error: XNU provider archive is missing or empty: $archive" >&2
  exit 1
fi

tmp_dir="$(mktemp -d)"
cleanup() {
  rm -rf "$tmp_dir"
}
trap cleanup EXIT

tar -xzf "$archive" -C "$tmp_dir"

provider_manifest="$tmp_dir/metadata/xnu-provider.manifest"
media_manifest="$tmp_dir/metadata/media.manifest"
handoff_manifest="$tmp_dir/metadata/xnu-boot-handoff.manifest"
boot_contract="$tmp_dir/docs/XNU_BOOT_CONTRACT.md"
handoff_abi="$tmp_dir/boot/xnu/xnu_boot_handoff.h"

for required in "$provider_manifest" "$media_manifest" "$handoff_manifest" "$boot_contract" "$handoff_abi"; do
  if [ ! -s "$required" ]; then
    echo "error: XNU provider archive is missing required file: ${required#$tmp_dir/}" >&2
    exit 1
  fi
done

manifest_value() {
  awk -F= -v key="$1" '$1 == key { print substr($0, length(key) + 2); found=1; exit } END { if (!found) exit 1 }' "$2"
}

provider="$(manifest_value provider "$provider_manifest")"
provider_arch="$(manifest_value arch "$provider_manifest")"
source_origin="$(manifest_value source_origin "$provider_manifest")"
source_commit="$(manifest_value source_commit "$provider_manifest")"
source_state="$(manifest_value source_state "$provider_manifest")"
provider_mode="$(manifest_value mode "$provider_manifest")"

media_provider="$(manifest_value provider "$media_manifest")"
media_arch="$(manifest_value arch "$media_manifest")"
kernel_artifact="$(manifest_value kernel_artifact "$media_manifest")"
payload_mode="$(manifest_value payload_mode "$media_manifest")"
source_policy="$(manifest_value external_source_policy "$media_manifest")"
contract_path="$(manifest_value boot_contract "$media_manifest")"
handoff_abi_path="$(manifest_value handoff_abi "$media_manifest")"
handoff_path="$(manifest_value boot_handoff "$media_manifest")"

if [ "$provider" != "xnu" ] || [ "$media_provider" != "xnu" ]; then
  echo "error: provider media does not identify the XNU provider" >&2
  exit 1
fi

if [ "$provider_arch" != "$arch" ] || [ "$media_arch" != "$arch" ]; then
  echo "error: provider media architecture mismatch for $arch" >&2
  exit 1
fi

if [ "$source_policy" != "read-only" ]; then
  echo "error: provider media does not declare read-only external source policy" >&2
  exit 1
fi

if [ "$contract_path" != "docs/XNU_BOOT_CONTRACT.md" ]; then
  echo "error: provider media points at unexpected boot contract: $contract_path" >&2
  exit 1
fi

if [ "$handoff_abi_path" != "boot/xnu/xnu_boot_handoff.h" ]; then
  echo "error: provider media points at unexpected boot handoff ABI: $handoff_abi_path" >&2
  exit 1
fi

if [ "$handoff_path" != "metadata/xnu-boot-handoff.manifest" ]; then
  echo "error: provider media points at unexpected boot handoff: $handoff_path" >&2
  exit 1
fi

if [ -n "$expected_mode" ] && [ "$payload_mode" != "$expected_mode" ]; then
  echo "error: provider media payload mode is $payload_mode, expected $expected_mode" >&2
  exit 1
fi

if [ "$payload_mode" != "compiled" ] && [ "$payload_mode" != "source-validation" ]; then
  echo "error: unknown provider media payload mode: $payload_mode" >&2
  exit 1
fi

if [ "$payload_mode" = "compiled" ]; then
  kernel_name="$(basename "$kernel_artifact")"
  if [ ! -s "$tmp_dir/kernel/$kernel_name" ]; then
    echo "error: compiled provider media is missing kernel payload: kernel/$kernel_name" >&2
    exit 1
  fi
  if [ "$provider_mode" != "compiled" ]; then
    echo "error: compiled provider media has provider mode $provider_mode" >&2
    exit 1
  fi
else
  if [ "$provider_mode" != "source-validation" ]; then
    echo "error: source-validation provider media has provider mode $provider_mode" >&2
    exit 1
  fi
fi

if [ -z "$source_origin" ] || [ -z "$source_commit" ] || [ -z "$source_state" ]; then
  echo "error: provider manifest is missing source identity fields" >&2
  exit 1
fi

handoff_provider="$(manifest_value provider "$handoff_manifest")"
handoff_arch="$(manifest_value arch "$handoff_manifest")"
handoff_kernel="$(manifest_value kernel_artifact "$handoff_manifest")"
handoff_payload="$(manifest_value payload_mode "$handoff_manifest")"
handoff_policy="$(manifest_value external_source_policy "$handoff_manifest")"
handoff_abi_ref="$(manifest_value handoff_abi "$handoff_manifest")"
handoff_magic="$(manifest_value handoff_magic "$handoff_manifest")"
handoff_version="$(manifest_value handoff_version "$handoff_manifest")"

for required_key in contract_version boot_args memory_map firmware_state cpu_topology timer platform_data framebuffer early_userspace handoff_abi handoff_magic handoff_version; do
  if ! manifest_value "$required_key" "$handoff_manifest" >/dev/null; then
    echo "error: boot handoff manifest is missing required key: $required_key" >&2
    exit 1
  fi
done

if [ "$handoff_provider" != "xnu" ] || [ "$handoff_arch" != "$arch" ]; then
  echo "error: boot handoff manifest does not match XNU provider architecture" >&2
  exit 1
fi

if [ "$handoff_kernel" != "$kernel_artifact" ] || [ "$handoff_payload" != "$payload_mode" ]; then
  echo "error: boot handoff manifest does not match media payload metadata" >&2
  exit 1
fi

if [ "$handoff_policy" != "read-only" ]; then
  echo "error: boot handoff manifest does not preserve read-only source policy" >&2
  exit 1
fi

if [ "$handoff_abi_ref" != "boot/xnu/xnu_boot_handoff.h" ] ||
   [ "$handoff_magic" != "0x584E55424F4F5431" ] ||
   [ "$handoff_version" != "1" ]; then
  echo "error: boot handoff ABI metadata is inconsistent" >&2
  exit 1
fi

grep -q 'OS8_XNU_BOOT_HANDOFF_MAGIC 0x584E55424F4F5431ULL' "$handoff_abi"
grep -q 'OS8_XNU_BOOT_HANDOFF_VERSION 1ULL' "$handoff_abi"
grep -q 'os8_xnu_boot_handoff_t' "$handoff_abi"

echo "[XNU] Provider media verified: $archive"
