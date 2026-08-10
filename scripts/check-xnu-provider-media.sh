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
boot_plan_manifest="$tmp_dir/metadata/xnu-boot-plan.manifest"
boot_contract="$tmp_dir/docs/XNU_BOOT_CONTRACT.md"
handoff_abi="$tmp_dir/boot/xnu/xnu_boot_handoff.h"
handoff_builder="$tmp_dir/boot/xnu/xnu_boot_handoff_builder.h"
macho_loader="$tmp_dir/boot/xnu/xnu_macho_loader.h"
uefi_handoff="$tmp_dir/boot/xnu/xnu_uefi_handoff.h"
x86_64_boot_args="$tmp_dir/boot/xnu/xnu_x86_64_boot_args.h"
x86_64_entry_handoff="$tmp_dir/boot/custom/startup-handoff.S"
x86_64_startup_loader="$tmp_dir/boot/custom/startup.c"
abi_check_script="scripts/check-xnu-boot-abi.sh"
kernel_artifact_check_script="scripts/check-xnu-kernel-artifact.sh"
abi_manifest="$tmp_dir/metadata/xnu-boot-abi.generated"

for required in "$provider_manifest" "$media_manifest" "$handoff_manifest" "$boot_plan_manifest" "$boot_contract" "$handoff_abi" "$handoff_builder" "$macho_loader" "$uefi_handoff" "$x86_64_boot_args" "$x86_64_entry_handoff" "$x86_64_startup_loader"; do
  if [ ! -s "$required" ]; then
    echo "error: XNU provider archive is missing required file: ${required#$tmp_dir/}" >&2
    exit 1
  fi
done

if [ ! -x "$abi_check_script" ]; then
  echo "error: XNU boot ABI checker is missing or not executable: $abi_check_script" >&2
  exit 1
fi
if [ ! -f "$kernel_artifact_check_script" ]; then
  echo "error: XNU kernel artifact verifier is missing: $kernel_artifact_check_script" >&2
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
handoff_builder_path="$(manifest_value handoff_builder "$media_manifest")"
macho_loader_path="$(manifest_value macho_loader "$media_manifest")"
uefi_handoff_path="$(manifest_value uefi_handoff "$media_manifest")"
x86_64_boot_args_path="$(manifest_value x86_64_boot_args "$media_manifest")"
x86_64_entry_handoff_path="$(manifest_value x86_64_entry_handoff "$media_manifest")"
x86_64_startup_loader_path="$(manifest_value x86_64_startup_loader "$media_manifest")"
handoff_path="$(manifest_value boot_handoff "$media_manifest")"
boot_plan_path="$(manifest_value boot_plan "$media_manifest")"

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

if [ "$handoff_builder_path" != "boot/xnu/xnu_boot_handoff_builder.h" ]; then
  echo "error: provider media points at unexpected boot handoff builder: $handoff_builder_path" >&2
  exit 1
fi

if [ "$macho_loader_path" != "boot/xnu/xnu_macho_loader.h" ]; then
  echo "error: provider media points at unexpected Mach-O loader contract: $macho_loader_path" >&2
  exit 1
fi

if [ "$uefi_handoff_path" != "boot/xnu/xnu_uefi_handoff.h" ]; then
  echo "error: provider media points at unexpected UEFI handoff helper: $uefi_handoff_path" >&2
  exit 1
fi

if [ "$x86_64_boot_args_path" != "boot/xnu/xnu_x86_64_boot_args.h" ]; then
  echo "error: provider media points at unexpected x86_64 boot args builder: $x86_64_boot_args_path" >&2
  exit 1
fi

if [ "$x86_64_entry_handoff_path" != "boot/custom/startup-handoff.S" ]; then
  echo "error: provider media points at unexpected x86_64 entry handoff shim: $x86_64_entry_handoff_path" >&2
  exit 1
fi

if [ "$x86_64_startup_loader_path" != "boot/custom/startup.c" ]; then
  echo "error: provider media points at unexpected x86_64 startup loader: $x86_64_startup_loader_path" >&2
  exit 1
fi

if [ "$handoff_path" != "metadata/xnu-boot-handoff.manifest" ]; then
  echo "error: provider media points at unexpected boot handoff: $handoff_path" >&2
  exit 1
fi

if [ "$boot_plan_path" != "metadata/xnu-boot-plan.manifest" ]; then
  echo "error: provider media points at unexpected boot plan: $boot_plan_path" >&2
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
  bash "$kernel_artifact_check_script" "$arch" "$tmp_dir/kernel/$kernel_name" >/dev/null
  if [ "$provider_mode" != "compiled" ]; then
    echo "error: compiled provider media has provider mode $provider_mode" >&2
    exit 1
  fi
  if [ "$arch" = "x86_64" ]; then
    x86_64_uefi_boot="$(manifest_value x86_64_uefi_boot "$media_manifest")"
    x86_64_uefi_config="$(manifest_value x86_64_uefi_config "$media_manifest")"
    x86_64_uefi_startup="$(manifest_value x86_64_uefi_startup "$media_manifest")"
    x86_64_uefi_image="$(manifest_value x86_64_uefi_image "$media_manifest")"
    if [ "$x86_64_uefi_boot" != "boot/custom-uefi" ] ||
       [ "$x86_64_uefi_config" != "boot/custom-uefi/os8boot.cfg" ] ||
       [ "$x86_64_uefi_startup" != "boot/custom-uefi/STARTUPX64.EFI" ] ||
       [ "$x86_64_uefi_image" != "image/xnu-x86_64-uefi.img" ]; then
      echo "error: compiled x86_64 provider media has inconsistent UEFI boot metadata" >&2
      exit 1
    fi
    for required in "$tmp_dir/boot/custom-uefi/BOOTX64.EFI" "$tmp_dir/boot/custom-uefi/STARTUPX64.EFI" "$tmp_dir/boot/custom-uefi/os8boot.cfg" "$tmp_dir/image/xnu-x86_64-uefi.img"; do
      if [ ! -s "$required" ]; then
        echo "error: compiled x86_64 provider media is missing UEFI boot artifact: ${required#$tmp_dir/}" >&2
        exit 1
      fi
    done
    grep -q '^kernel_format=xnu$' "$tmp_dir/boot/custom-uefi/os8boot.cfg"
    grep -q '^kernel_path=\\boot\\main.sys$' "$tmp_dir/boot/custom-uefi/os8boot.cfg"
    grep -Eq '^kernel_sha256=[0-9a-f]{64}$' "$tmp_dir/boot/custom-uefi/os8boot.cfg"
    if command -v mdir >/dev/null 2>&1 && command -v mtype >/dev/null 2>&1; then
      mdir -i "$tmp_dir/image/xnu-x86_64-uefi.img" ::/EFI/BOOT/BOOTX64.EFI >/dev/null
      mdir -i "$tmp_dir/image/xnu-x86_64-uefi.img" ::/EFI/OS8/STARTUPX64.EFI >/dev/null
      mdir -i "$tmp_dir/image/xnu-x86_64-uefi.img" ::/EFI/OS8/os8boot.cfg >/dev/null
      mdir -i "$tmp_dir/image/xnu-x86_64-uefi.img" ::/boot/main.sys >/dev/null
      mtype -i "$tmp_dir/image/xnu-x86_64-uefi.img" ::/EFI/OS8/os8boot.cfg | grep -q '^kernel_format=xnu$'
    else
      echo "[XNU] mtools not available; skipping FAT image content inspection" >&2
    fi
  fi
else
  if [ "$provider_mode" != "source-validation" ]; then
    echo "error: source-validation provider media has provider mode $provider_mode" >&2
    exit 1
  fi
  if find "$tmp_dir/kernel" -type f | grep -q .; then
    echo "error: source-validation provider media must not include a kernel payload" >&2
    exit 1
  fi
  if [ -e "$tmp_dir/boot/custom-uefi" ] || [ -e "$tmp_dir/image/xnu-x86_64-uefi.img" ]; then
    echo "error: source-validation provider media must not include bootable XNU UEFI artifacts" >&2
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
handoff_framebuffer="$(manifest_value framebuffer "$handoff_manifest")"

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

if [ "$handoff_framebuffer" != "required" ]; then
  echo "error: boot handoff manifest must require framebuffer input" >&2
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

boot_plan_provider="$(manifest_value provider "$boot_plan_manifest")"
boot_plan_arch="$(manifest_value arch "$boot_plan_manifest")"
boot_plan_kernel="$(manifest_value kernel_artifact "$boot_plan_manifest")"
boot_plan_payload="$(manifest_value payload_mode "$boot_plan_manifest")"
boot_plan_policy="$(manifest_value external_source_policy "$boot_plan_manifest")"
boot_plan_abi="$(manifest_value handoff_abi "$boot_plan_manifest")"
boot_plan_builder="$(manifest_value handoff_builder "$boot_plan_manifest")"
boot_plan_macho_loader="$(manifest_value macho_loader "$boot_plan_manifest")"
boot_plan_uefi_handoff="$(manifest_value uefi_handoff "$boot_plan_manifest")"
boot_plan_x86_64_boot_args="$(manifest_value x86_64_boot_args "$boot_plan_manifest")"
boot_plan_x86_64_entry_handoff="$(manifest_value x86_64_entry_handoff "$boot_plan_manifest")"
boot_plan_x86_64_startup_loader="$(manifest_value x86_64_startup_loader "$boot_plan_manifest")"
boot_plan_x86_64_uefi_boot="$(manifest_value x86_64_uefi_boot "$boot_plan_manifest")"
boot_plan_handoff="$(manifest_value boot_handoff "$boot_plan_manifest")"
boot_plan_platform_kind="$(manifest_value platform_kind "$boot_plan_manifest")"
boot_plan_arch_id="$(manifest_value handoff_arch_id "$boot_plan_manifest")"
boot_plan_flags="$(manifest_value handoff_required_flags "$boot_plan_manifest")"
boot_plan_inputs="$(manifest_value required_loader_inputs "$boot_plan_manifest")"

for required_key in plan_version entry_protocol firmware_state platform_data loader_step_1 loader_step_2 loader_step_3 loader_step_4 loader_step_5 loader_step_6 loader_step_7 loader_step_8 loader_step_9 loader_step_10; do
  if ! manifest_value "$required_key" "$boot_plan_manifest" >/dev/null; then
    echo "error: boot plan manifest is missing required key: $required_key" >&2
    exit 1
  fi
done

if [ "$(manifest_value loader_step_1 "$boot_plan_manifest")" != "inspect-mach-o-and-load-segments" ]; then
  echo "error: boot plan does not require Mach-O segment loading" >&2
  exit 1
fi

case "$arch" in
  x86_64)
    expected_loader_step_2="build-x86_64-efi-boot-args"
    ;;
  arm64)
    expected_loader_step_2="prepare-architecture-boot-args"
    ;;
esac

if [ "$(manifest_value loader_step_2 "$boot_plan_manifest")" != "$expected_loader_step_2" ]; then
  echo "error: boot plan has unexpected architecture boot args step" >&2
  exit 1
fi

if [ "$(manifest_value loader_step_3 "$boot_plan_manifest")" != "convert-uefi-memory-map" ]; then
  echo "error: boot plan does not require UEFI memory map conversion" >&2
  exit 1
fi

if [ "$(manifest_value loader_step_6 "$boot_plan_manifest")" != "convert-uefi-framebuffer" ]; then
  echo "error: boot plan does not require UEFI framebuffer conversion" >&2
  exit 1
fi

if [ "$(manifest_value loader_step_7 "$boot_plan_manifest")" != "derive-handoff-kernel-fields-from-mach-o" ]; then
  echo "error: boot plan does not derive handoff kernel fields from Mach-O metadata" >&2
  exit 1
fi

if [ "$(manifest_value loader_step_8 "$boot_plan_manifest")" != "validate-and-apply-handoff-inputs" ]; then
  echo "error: boot plan does not require validated handoff input helpers" >&2
  exit 1
fi

if [ "$boot_plan_provider" != "xnu" ] ||
   [ "$boot_plan_arch" != "$arch" ] ||
   [ "$boot_plan_kernel" != "$kernel_artifact" ] ||
   [ "$boot_plan_payload" != "$payload_mode" ] ||
   [ "$boot_plan_policy" != "read-only" ] ||
   [ "$boot_plan_abi" != "boot/xnu/xnu_boot_handoff.h" ] ||
   [ "$boot_plan_builder" != "boot/xnu/xnu_boot_handoff_builder.h" ] ||
   [ "$boot_plan_macho_loader" != "boot/xnu/xnu_macho_loader.h" ] ||
   [ "$boot_plan_uefi_handoff" != "boot/xnu/xnu_uefi_handoff.h" ] ||
   [ "$boot_plan_x86_64_boot_args" != "boot/xnu/xnu_x86_64_boot_args.h" ] ||
   [ "$boot_plan_x86_64_entry_handoff" != "boot/custom/startup-handoff.S" ] ||
   [ "$boot_plan_x86_64_startup_loader" != "boot/custom/startup.c" ] ||
   [ "$boot_plan_x86_64_uefi_boot" != "boot/custom-uefi" ] ||
   [ "$boot_plan_handoff" != "metadata/xnu-boot-handoff.manifest" ]; then
  echo "error: boot plan manifest does not match provider media metadata" >&2
  exit 1
fi

if [ "$boot_plan_platform_kind" != "$handoff_platform_kind" ] ||
   [ "$boot_plan_arch_id" != "$handoff_arch_id" ] ||
   [ "$boot_plan_flags" != "$handoff_required_flags" ]; then
  echo "error: boot plan target metadata does not match boot handoff manifest" >&2
  exit 1
fi

if [ "$boot_plan_inputs" != "kernel,boot_args,memory_map,platform_data,timer,cpu_topology,framebuffer" ]; then
  echo "error: boot plan loader input set is incomplete: $boot_plan_inputs" >&2
  exit 1
fi

grep -q 'OS8_XNU_BOOT_HANDOFF_MAGIC 0x584E55424F4F5431ULL' "$handoff_abi"
grep -q 'OS8_XNU_BOOT_HANDOFF_VERSION 1ULL' "$handoff_abi"
grep -q 'OS8_XNU_ARCH_X86_64 1U' "$handoff_abi"
grep -q 'OS8_XNU_ARCH_ARM64 2U' "$handoff_abi"
grep -q 'OS8_XNU_PLATFORM_ACPI 1U' "$handoff_abi"
grep -q 'OS8_XNU_PLATFORM_DEVICE_TREE 2U' "$handoff_abi"
grep -q 'os8_xnu_boot_handoff_t' "$handoff_abi"
grep -q 'OS8_XNU_MH_MAGIC_64 0xFEEDFACFULL' "$macho_loader"
grep -q 'os8_xnu_macho64_inspect' "$macho_loader"
grep -q 'os8_xnu_macho64_segment_at' "$macho_loader"
grep -q 'os8_xnu_boot_handoff_apply_macho' "$handoff_builder"
grep -q 'os8_xnu_boot_handoff_apply_boot_args' "$handoff_builder"
grep -q 'os8_xnu_boot_handoff_apply_memory_map' "$handoff_builder"
grep -q 'os8_xnu_boot_handoff_apply_platform_data' "$handoff_builder"
grep -q 'os8_xnu_boot_handoff_apply_framebuffer' "$handoff_builder"
grep -q 'os8_xnu_uefi_memory_map_convert' "$uefi_handoff"
grep -q 'os8_xnu_boot_handoff_apply_uefi_framebuffer' "$uefi_handoff"
grep -q 'OS8_XNU_X86_64_BOOT_ARGS_SIZE 4096U' "$x86_64_boot_args"
grep -q 'os8_xnu_x86_64_boot_args_t' "$x86_64_boot_args"
grep -q 'os8_xnu_x86_64_boot_args_build' "$x86_64_boot_args"
grep -q 'os8_xnu_boot_handoff_apply_x86_64_boot_args' "$x86_64_boot_args"
grep -q 'startup_enter_xnu_kernel' "$x86_64_entry_handoff"
grep -Eq 'mov[[:space:]]+%r8d,[[:space:]]*%edi' "$x86_64_entry_handoff"
grep -q 'kernel_format' "$x86_64_startup_loader"
grep -q 'valid_xnu_macho64_kernel' "$x86_64_startup_loader"
grep -q 'load_xnu_macho64_segments' "$x86_64_startup_loader"
grep -q 'alloc_zero_pages_below(total / 4096' "$x86_64_startup_loader"
grep -q 'build_xnu_boot_args_pre_exit' "$x86_64_startup_loader"
grep -q 'Entering XNU kernel after ExitBootServices' "$x86_64_startup_loader"
grep -q 'Entered ExitBootServices; jumping to XNU kernel' "$x86_64_startup_loader"
grep -q 'startup_enter_xnu_kernel' "$x86_64_startup_loader"
if grep -q 'startup_enter_xnu_kernel(pml4_phys, entry, 0)' "$x86_64_startup_loader"; then
  echo "error: packaged XNU startup loader can enter without boot_args" >&2
  exit 1
fi

echo "[XNU] Provider media verified: $archive"
