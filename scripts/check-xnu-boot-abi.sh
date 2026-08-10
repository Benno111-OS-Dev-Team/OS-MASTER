#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
header="$root/boot/xnu/xnu_boot_handoff.h"
work_dir="${TMPDIR:-/tmp}/os8-xnu-abi-check.$$"
source_file="$work_dir/check.c"
object_file="$work_dir/check.o"

cleanup() {
  rm -rf "$work_dir"
}
trap cleanup EXIT

if [ ! -f "$header" ]; then
  echo "error: XNU boot handoff ABI header is missing: $header" >&2
  exit 1
fi

cc_bin="${CC:-}"
if [ -z "$cc_bin" ]; then
  cc_bin="$(command -v clang 2>/dev/null || command -v cc 2>/dev/null || true)"
fi
if [ -z "$cc_bin" ]; then
  echo "error: clang or cc is required to validate the XNU boot handoff ABI" >&2
  exit 1
fi

mkdir -p "$work_dir"
cat > "$source_file" <<'C'
#include <stddef.h>
#include <stdint.h>
#include "boot/xnu/xnu_boot_handoff.h"

#define CHECK(cond, name) typedef char check_##name[(cond) ? 1 : -1]

CHECK(OS8_XNU_BOOT_HANDOFF_MAGIC == 0x584E55424F4F5431ULL, magic);
CHECK(OS8_XNU_BOOT_HANDOFF_VERSION == 1ULL, version);
CHECK(OS8_XNU_ARCH_X86_64 == 1U, arch_x86_64);
CHECK(OS8_XNU_ARCH_ARM64 == 2U, arch_arm64);
CHECK(OS8_XNU_PLATFORM_ACPI == 1U, platform_acpi);
CHECK(OS8_XNU_PLATFORM_DEVICE_TREE == 2U, platform_device_tree);
CHECK(sizeof(os8_xnu_range_t) == 24, range_size);
CHECK(sizeof(os8_xnu_framebuffer_t) == 32, framebuffer_size);
CHECK(offsetof(os8_xnu_boot_handoff_t, magic) == 0, magic_offset);
CHECK(offsetof(os8_xnu_boot_handoff_t, version) == 8, version_offset);
CHECK(offsetof(os8_xnu_boot_handoff_t, flags) == 16, flags_offset);
CHECK(offsetof(os8_xnu_boot_handoff_t, arch) == 24, arch_offset);
CHECK(offsetof(os8_xnu_boot_handoff_t, platform_kind) == 28, platform_offset);
CHECK(offsetof(os8_xnu_boot_handoff_t, kernel_base) == 32, kernel_base_offset);
CHECK(offsetof(os8_xnu_boot_handoff_t, boot_args_base) == 56, boot_args_offset);
CHECK(offsetof(os8_xnu_boot_handoff_t, memory_map_base) == 72, memory_map_offset);
CHECK(offsetof(os8_xnu_boot_handoff_t, platform_data_base) == 96, platform_data_offset);
CHECK(offsetof(os8_xnu_boot_handoff_t, timer_frequency_hz) == 112, timer_offset);
CHECK(offsetof(os8_xnu_boot_handoff_t, cpu_topology_base) == 120, cpu_topology_offset);
CHECK(offsetof(os8_xnu_boot_handoff_t, framebuffer) == 136, framebuffer_offset);
CHECK(sizeof(os8_xnu_boot_handoff_t) == 168, handoff_size);

int main(void) {
  return 0;
}
C

"$cc_bin" -std=c11 -Wall -Wextra -I"$root" -c "$source_file" -o "$object_file"
echo "[XNU] Boot handoff ABI verified"
