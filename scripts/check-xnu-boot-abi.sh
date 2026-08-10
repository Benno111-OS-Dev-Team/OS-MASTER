#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
header="${1:-$root/boot/xnu/xnu_boot_handoff.h}"
emit_manifest="${2:-}"
work_dir="${TMPDIR:-/tmp}/os8-xnu-abi-check.$$"
source_file="$work_dir/check.c"
object_file="$work_dir/check.o"
binary_file="$work_dir/check"
local_header="$work_dir/xnu_boot_handoff.h"

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
cp "$header" "$local_header"
cat > "$source_file" <<'C'
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include "xnu_boot_handoff.h"

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
  printf("handoff_magic=0x%llX\n", (unsigned long long)OS8_XNU_BOOT_HANDOFF_MAGIC);
  printf("handoff_version=%llu\n", (unsigned long long)OS8_XNU_BOOT_HANDOFF_VERSION);
  printf("handoff_struct=os8_xnu_boot_handoff_t\n");
  printf("handoff_struct_size=%zu\n", sizeof(os8_xnu_boot_handoff_t));
  printf("handoff_framebuffer_offset=%zu\n", offsetof(os8_xnu_boot_handoff_t, framebuffer));
  printf("handoff_field_magic=%zu:uint64\n", offsetof(os8_xnu_boot_handoff_t, magic));
  printf("handoff_field_version=%zu:uint64\n", offsetof(os8_xnu_boot_handoff_t, version));
  printf("handoff_field_flags=%zu:uint64\n", offsetof(os8_xnu_boot_handoff_t, flags));
  printf("handoff_field_arch=%zu:uint32\n", offsetof(os8_xnu_boot_handoff_t, arch));
  printf("handoff_field_platform_kind=%zu:uint32\n", offsetof(os8_xnu_boot_handoff_t, platform_kind));
  printf("handoff_field_kernel_base=%zu:uint64\n", offsetof(os8_xnu_boot_handoff_t, kernel_base));
  printf("handoff_field_kernel_size=%zu:uint64\n", offsetof(os8_xnu_boot_handoff_t, kernel_size));
  printf("handoff_field_kernel_entry=%zu:uint64\n", offsetof(os8_xnu_boot_handoff_t, kernel_entry));
  printf("handoff_field_boot_args_base=%zu:uint64\n", offsetof(os8_xnu_boot_handoff_t, boot_args_base));
  printf("handoff_field_boot_args_size=%zu:uint64\n", offsetof(os8_xnu_boot_handoff_t, boot_args_size));
  printf("handoff_field_memory_map_base=%zu:uint64\n", offsetof(os8_xnu_boot_handoff_t, memory_map_base));
  printf("handoff_field_memory_map_entry_count=%zu:uint64\n", offsetof(os8_xnu_boot_handoff_t, memory_map_entry_count));
  printf("handoff_field_memory_map_entry_size=%zu:uint64\n", offsetof(os8_xnu_boot_handoff_t, memory_map_entry_size));
  printf("handoff_field_platform_data_base=%zu:uint64\n", offsetof(os8_xnu_boot_handoff_t, platform_data_base));
  printf("handoff_field_platform_data_size=%zu:uint64\n", offsetof(os8_xnu_boot_handoff_t, platform_data_size));
  printf("handoff_field_timer_frequency_hz=%zu:uint64\n", offsetof(os8_xnu_boot_handoff_t, timer_frequency_hz));
  printf("handoff_field_cpu_topology_base=%zu:uint64\n", offsetof(os8_xnu_boot_handoff_t, cpu_topology_base));
  printf("handoff_field_cpu_topology_size=%zu:uint64\n", offsetof(os8_xnu_boot_handoff_t, cpu_topology_size));
  printf("handoff_field_framebuffer=%zu:os8_xnu_framebuffer_t\n", offsetof(os8_xnu_boot_handoff_t, framebuffer));
  return 0;
}
C

if [ "$emit_manifest" = "--emit-manifest" ]; then
  "$cc_bin" -std=c11 -Wall -Wextra -I"$work_dir" "$source_file" -o "$binary_file"
  "$binary_file"
  exit 0
fi

"$cc_bin" -std=c11 -Wall -Wextra -I"$work_dir" -c "$source_file" -o "$object_file"
echo "[XNU] Boot handoff ABI verified"
