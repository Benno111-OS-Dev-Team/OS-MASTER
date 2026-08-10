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
abi_check_script="scripts/check-xnu-boot-abi.sh"
abi_manifest="$tmp_dir/metadata/xnu-boot-abi.generated"

for required in "$provider_manifest" "$media_manifest" "$handoff_manifest" "$boot_contract" "$handoff_abi"; do
  if [ ! -s "$required" ]; then
    echo "error: XNU provider archive is missing required file: ${required#$tmp_dir/}" >&2
    exit 1
  fi
done

if [ ! -x "$abi_check_script" ]; then
  echo "error: XNU boot ABI checker is missing or not executable: $abi_check_script" >&2
  exit 1
fi

bash "$abi_check_script" "$handoff_abi" --emit-manifest > "$abi_manifest"

manifest_value() {
  awk -F= -v key="$1" '$1 == key { print substr($0, length(key) + 2); found=1; exit } END { if (!found) exit 1 }' "$2"
}

manifest_matches_abi() {
  key="$1"
  handoff_value="$(manifest_value "$key" "$handoff_manifest")"
  abi_value="$(manifest_value "$key" "$abi_manifest")"
  if [ "$handoff_value" != "$abi_value" ]; then
    echo "error: boot handoff metadata does not match packaged ABI: $key" >&2
    exit 1
  fi
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
handoff_struct="$(manifest_value handoff_struct "$handoff_manifest")"
handoff_struct_size="$(manifest_value handoff_struct_size "$handoff_manifest")"
handoff_framebuffer_offset="$(manifest_value handoff_framebuffer_offset "$handoff_manifest")"
handoff_arch_id="$(manifest_value handoff_arch_id "$handoff_manifest")"
handoff_platform_kind="$(manifest_value handoff_platform_kind "$handoff_manifest")"
handoff_required_flags="$(manifest_value handoff_required_flags "$handoff_manifest")"

for required_key in contract_version boot_args memory_map firmware_state cpu_topology timer platform_data framebuffer early_userspace handoff_abi handoff_magic handoff_version handoff_struct handoff_struct_size handoff_framebuffer_offset handoff_arch_id handoff_platform_kind handoff_required_flags; do
  if ! manifest_value "$required_key" "$handoff_manifest" >/dev/null; then
    echo "error: boot handoff manifest is missing required key: $required_key" >&2
    exit 1
  fi
done

expected_fields='
handoff_field_magic=0:uint64
handoff_field_version=8:uint64
handoff_field_flags=16:uint64
handoff_field_arch=24:uint32
handoff_field_platform_kind=28:uint32
handoff_field_kernel_base=32:uint64
handoff_field_kernel_size=40:uint64
handoff_field_kernel_entry=48:uint64
handoff_field_boot_args_base=56:uint64
handoff_field_boot_args_size=64:uint64
handoff_field_memory_map_base=72:uint64
handoff_field_memory_map_entry_count=80:uint64
handoff_field_memory_map_entry_size=88:uint64
handoff_field_platform_data_base=96:uint64
handoff_field_platform_data_size=104:uint64
handoff_field_timer_frequency_hz=112:uint64
handoff_field_cpu_topology_base=120:uint64
handoff_field_cpu_topology_size=128:uint64
handoff_field_framebuffer=136:os8_xnu_framebuffer_t
'

printf '%s\n' "$expected_fields" | while IFS= read -r expected_field; do
  if [ -z "$expected_field" ]; then
    continue
  fi
  expected_key="${expected_field%%=*}"
  expected_value="${expected_field#*=}"
  actual_value="$(manifest_value "$expected_key" "$handoff_manifest")"
  if [ "$actual_value" != "$expected_value" ]; then
    echo "error: boot handoff field metadata mismatch: $expected_key=$actual_value" >&2
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
   [ "$handoff_version" != "1" ] ||
   [ "$handoff_struct" != "os8_xnu_boot_handoff_t" ] ||
   [ "$handoff_struct_size" != "168" ] ||
   [ "$handoff_framebuffer_offset" != "136" ]; then
  echo "error: boot handoff ABI metadata is inconsistent" >&2
  exit 1
fi

for abi_key in handoff_magic handoff_version handoff_struct handoff_struct_size handoff_framebuffer_offset; do
  manifest_matches_abi "$abi_key"
done

printf '%s\n' "$expected_fields" | while IFS= read -r expected_field; do
  if [ -z "$expected_field" ]; then
    continue
  fi
  manifest_matches_abi "${expected_field%%=*}"
done

case "$arch" in
  x86_64)
    expected_arch_id="1"
    expected_platform_kind="1"
    expected_flags="0x00000003"
    ;;
  arm64)
    expected_arch_id="2"
    expected_platform_kind="2"
    expected_flags="0x00000005"
    ;;
esac

if [ "$handoff_arch_id" != "$expected_arch_id" ] ||
   [ "$handoff_platform_kind" != "$expected_platform_kind" ] ||
   [ "$handoff_required_flags" != "$expected_flags" ]; then
  echo "error: boot handoff target metadata is inconsistent for $arch" >&2
  exit 1
fi

grep -q 'OS8_XNU_BOOT_HANDOFF_MAGIC 0x584E55424F4F5431ULL' "$handoff_abi"
grep -q 'OS8_XNU_BOOT_HANDOFF_VERSION 1ULL' "$handoff_abi"
grep -q 'OS8_XNU_ARCH_X86_64 1U' "$handoff_abi"
grep -q 'OS8_XNU_ARCH_ARM64 2U' "$handoff_abi"
grep -q 'OS8_XNU_PLATFORM_ACPI 1U' "$handoff_abi"
grep -q 'OS8_XNU_PLATFORM_DEVICE_TREE 2U' "$handoff_abi"
grep -q 'os8_xnu_boot_handoff_t' "$handoff_abi"

echo "[XNU] Provider media verified: $archive"
