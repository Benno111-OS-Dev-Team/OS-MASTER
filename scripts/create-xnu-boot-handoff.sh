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
mkdir -p "$handoff_dir"

case "$arch" in
  x86_64)
    platform_data="acpi"
    firmware_state="uefi-exit-boot-services"
    ;;
  arm64)
    platform_data="device-tree"
    firmware_state="uefi-exit-boot-services"
    ;;
esac

{
  printf 'contract_version=1\n'
  printf 'provider=xnu\n'
  printf 'arch=%s\n' "$arch"
  printf 'kernel_artifact=%s\n' "$kernel_artifact"
  printf 'payload_mode=%s\n' "$payload_mode"
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
