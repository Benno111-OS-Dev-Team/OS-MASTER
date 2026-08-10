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
  x86_64|arm64) ;;
  *)
    echo "error: XNU boot handoff supports ARCH=x86_64 or ARCH=arm64, got: $arch" >&2
    exit 1
    ;;
esac

if [ "$payload_mode" != "compiled" ] && [ "$payload_mode" != "source-validation" ]; then
  echo "error: XNU boot handoff got unknown payload mode: $payload_mode" >&2
  exit 1
fi

handoff_dir="$build_dir/xnu-boot"
handoff_manifest="$handoff_dir/xnu-boot-handoff.manifest"
handoff_abi="boot/xnu/xnu_boot_handoff.h"
mkdir -p "$handoff_dir"

case "$arch" in
  x86_64)
    arch_id="1"
    platform_data="acpi"
    platform_kind="1"
    required_flags="0x00000003"
    firmware_state="uefi-exit-boot-services"
    ;;
  arm64)
    arch_id="2"
    platform_data="device-tree"
    platform_kind="2"
    required_flags="0x00000005"
    firmware_state="uefi-exit-boot-services"
    ;;
esac

{
  printf 'contract_version=1\n'
  printf 'provider=xnu\n'
  printf 'arch=%s\n' "$arch"
  printf 'kernel_artifact=%s\n' "$kernel_artifact"
  printf 'payload_mode=%s\n' "$payload_mode"
  printf 'handoff_abi=%s\n' "$handoff_abi"
  printf 'handoff_magic=0x584E55424F4F5431\n'
  printf 'handoff_version=1\n'
  printf 'handoff_struct=os8_xnu_boot_handoff_t\n'
  printf 'handoff_struct_size=168\n'
  printf 'handoff_framebuffer_offset=136\n'
  printf 'handoff_arch_id=%s\n' "$arch_id"
  printf 'handoff_platform_kind=%s\n' "$platform_kind"
  printf 'handoff_required_flags=%s\n' "$required_flags"
  printf 'boot_args=required\n'
  printf 'memory_map=required\n'
  printf 'firmware_state=%s\n' "$firmware_state"
  printf 'cpu_topology=required\n'
  printf 'timer=required\n'
  printf 'platform_data=%s\n' "$platform_data"
  printf 'framebuffer=optional\n'
  printf 'early_userspace=deferred\n'
  printf 'external_source_policy=read-only\n'
} > "$handoff_manifest"

echo "[XNU] Boot handoff manifest created: $handoff_manifest"
