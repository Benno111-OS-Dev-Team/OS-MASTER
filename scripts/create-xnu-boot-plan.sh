#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 4 ]; then
  echo "usage: $0 <arch> <build-dir> <kernel-artifact> <payload-mode>" >&2
  exit 2
fi

arch="$1"
build_dir="$2"
kernel_artifact="$3"
payload_mode="$4"

case "$arch" in
  x86_64)
    entry_protocol="x86_64-efi-boot-args"
    firmware_state="uefi-exit-boot-services"
    platform_data="acpi"
    platform_kind="1"
    handoff_arch_id="1"
    handoff_required_flags="0x00000003"
    ;;
  arm64)
    entry_protocol="arm64-device-tree-boot-args"
    firmware_state="uefi-exit-boot-services"
    platform_data="device-tree"
    platform_kind="2"
    handoff_arch_id="2"
    handoff_required_flags="0x00000005"
    ;;
  *)
    echo "error: XNU boot plan supports ARCH=x86_64 or ARCH=arm64, got: $arch" >&2
    exit 1
    ;;
esac

if [ "$payload_mode" != "compiled" ] && [ "$payload_mode" != "source-validation" ]; then
  echo "error: XNU boot plan got unknown payload mode: $payload_mode" >&2
  exit 1
fi

plan_dir="$build_dir/xnu-boot"
plan_manifest="$plan_dir/xnu-boot-plan.manifest"
mkdir -p "$plan_dir"

{
  printf 'plan_version=1\n'
  printf 'provider=xnu\n'
  printf 'arch=%s\n' "$arch"
  printf 'kernel_artifact=%s\n' "$kernel_artifact"
  printf 'payload_mode=%s\n' "$payload_mode"
  printf 'entry_protocol=%s\n' "$entry_protocol"
  printf 'firmware_state=%s\n' "$firmware_state"
  printf 'platform_data=%s\n' "$platform_data"
  printf 'platform_kind=%s\n' "$platform_kind"
  printf 'handoff_arch_id=%s\n' "$handoff_arch_id"
  printf 'handoff_required_flags=%s\n' "$handoff_required_flags"
  printf 'handoff_abi=boot/xnu/xnu_boot_handoff.h\n'
  printf 'handoff_builder=boot/xnu/xnu_boot_handoff_builder.h\n'
  printf 'macho_loader=boot/xnu/xnu_macho_loader.h\n'
  printf 'boot_handoff=metadata/xnu-boot-handoff.manifest\n'
  printf 'loader_step_1=inspect-mach-o-and-load-segments\n'
  printf 'loader_step_2=prepare-architecture-boot-args\n'
  printf 'loader_step_3=prepare-memory-map\n'
  printf 'loader_step_4=prepare-platform-data\n'
  printf 'loader_step_5=prepare-timer-and-cpu-topology\n'
  printf 'loader_step_6=prepare-framebuffer\n'
  printf 'loader_step_7=derive-handoff-kernel-fields-from-mach-o\n'
  printf 'loader_step_8=build-os8-xnu-handoff\n'
  printf 'loader_step_9=enter-xnu-kernel\n'
  printf 'required_loader_inputs=kernel,boot_args,memory_map,platform_data,timer,cpu_topology,framebuffer\n'
  printf 'external_source_policy=read-only\n'
} > "$plan_manifest"

echo "[XNU] Boot plan manifest created: $plan_manifest"
