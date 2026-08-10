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
builder_header="$(dirname "$header")/xnu_boot_handoff_builder.h"
macho_loader_header="$(dirname "$header")/xnu_macho_loader.h"
uefi_handoff_header="$(dirname "$header")/xnu_uefi_handoff.h"
local_builder="$work_dir/xnu_boot_handoff_builder.h"
local_macho_loader="$work_dir/xnu_macho_loader.h"
local_uefi_handoff="$work_dir/xnu_uefi_handoff.h"

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
if [ -f "$builder_header" ]; then
  cp "$builder_header" "$local_builder"
fi
if [ -f "$macho_loader_header" ]; then
  cp "$macho_loader_header" "$local_macho_loader"
fi
if [ -f "$uefi_handoff_header" ]; then
  cp "$uefi_handoff_header" "$local_uefi_handoff"
fi
cat > "$source_file" <<'C'
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include "xnu_boot_handoff.h"
#ifdef __has_include
#if __has_include("xnu_boot_handoff_builder.h")
#include "xnu_boot_handoff_builder.h"
#define OS8_XNU_HAVE_BUILDER 1
#endif
#endif
#ifdef __has_include
#if __has_include("xnu_macho_loader.h")
#include "xnu_macho_loader.h"
#define OS8_XNU_HAVE_MACHO_LOADER 1
#endif
#endif
#ifdef __has_include
#if __has_include("xnu_uefi_handoff.h")
#include "xnu_uefi_handoff.h"
#define OS8_XNU_HAVE_UEFI_HANDOFF 1
#endif
#endif

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
#ifdef OS8_XNU_HAVE_BUILDER
  os8_xnu_boot_handoff_t handoff;
  os8_xnu_boot_handoff_input_t input = {
      .arch = OS8_XNU_ARCH_X86_64,
      .kernel_base = 0x100000,
      .kernel_size = 0x200000,
      .kernel_entry = 0x101000,
  };
  if (os8_xnu_boot_handoff_apply_boot_args(&input, 0x300000, 0x1000) != 0)
    return 1;
  if (os8_xnu_boot_handoff_apply_memory_map(
          &input, 0x400000, 1, sizeof(os8_xnu_range_t)) != 0)
    return 1;
  if (os8_xnu_boot_handoff_apply_platform_data(&input, 0x500000, 0x1000) != 0)
    return 1;
  if (os8_xnu_boot_handoff_apply_timer(&input, 1000000000ULL) != 0)
    return 1;
  if (os8_xnu_boot_handoff_apply_cpu_topology(&input, 0x600000, 0x1000) != 0)
    return 1;
  if (os8_xnu_boot_handoff_apply_framebuffer(
          &input, 0x700000, 0x100000, 1024, 768, 4096, 1) != 0)
    return 1;
  if (os8_xnu_boot_handoff_apply_memory_map(&input, 0x400000, 1, 16) == 0)
    return 1;
  if (os8_xnu_boot_handoff_build(&handoff, &input) != 0) return 1;
  if (handoff.magic != OS8_XNU_BOOT_HANDOFF_MAGIC ||
      handoff.version != OS8_XNU_BOOT_HANDOFF_VERSION ||
      handoff.flags != (OS8_XNU_BOOT_FLAG_GRAPHICS | OS8_XNU_BOOT_FLAG_ACPI) ||
      handoff.platform_kind != OS8_XNU_PLATFORM_ACPI ||
      handoff.memory_map_entry_size != sizeof(os8_xnu_range_t)) {
    return 1;
  }
#endif
#ifdef OS8_XNU_HAVE_MACHO_LOADER
  struct synthetic_macho {
    os8_xnu_macho64_header_t header;
    os8_xnu_macho64_segment_command_t segment;
    os8_xnu_macho64_entry_point_command_t entry;
    unsigned char payload[16];
  } macho = {
      .header = {
          .magic = OS8_XNU_MH_MAGIC_64,
          .cputype = OS8_XNU_CPU_TYPE_X86_64,
          .ncmds = 2,
          .sizeofcmds = sizeof(os8_xnu_macho64_segment_command_t) +
                        sizeof(os8_xnu_macho64_entry_point_command_t),
      },
      .segment = {
          .cmd = OS8_XNU_LC_SEGMENT_64,
          .cmdsize = sizeof(os8_xnu_macho64_segment_command_t),
          .vmaddr = 0x100000,
          .vmsize = sizeof(((struct synthetic_macho *)0)->payload),
          .fileoff = offsetof(struct synthetic_macho, payload),
          .filesize = sizeof(((struct synthetic_macho *)0)->payload),
      },
      .entry = {
          .cmd = OS8_XNU_LC_MAIN,
          .cmdsize = sizeof(os8_xnu_macho64_entry_point_command_t),
          .entryoff = offsetof(struct synthetic_macho, payload),
      },
  };
  os8_xnu_macho64_image_t macho_info;
  os8_xnu_macho64_segment_t macho_segment;
  if (os8_xnu_macho64_inspect(&macho, sizeof(macho), OS8_XNU_ARCH_X86_64,
                              &macho_info) != 0) {
    return 1;
  }
  if (os8_xnu_macho64_segment_at(&macho, sizeof(macho), OS8_XNU_ARCH_X86_64,
                                 0, &macho_segment) != 0) {
    return 1;
  }
  if (macho_info.cputype != OS8_XNU_CPU_TYPE_X86_64 ||
      macho_info.segment_count != 1 ||
      macho_info.lowest_vmaddr != 0x100000 ||
      macho_info.entry_vmaddr != 0x100000) {
    return 1;
  }
  if (macho_segment.vmaddr != 0x100000 ||
      macho_segment.fileoff != offsetof(struct synthetic_macho, payload) ||
      macho_segment.filesize != sizeof(macho.payload) ||
      os8_xnu_macho64_segment_at(&macho, sizeof(macho), OS8_XNU_ARCH_X86_64,
                                 1, &macho_segment) == 0) {
    return 1;
  }
  input.kernel_base = 0;
  input.kernel_size = 0;
  input.kernel_entry = 0;
  if (os8_xnu_boot_handoff_apply_macho(&input, &macho_info, 0x800000) != 0) {
    return 1;
  }
  if (input.kernel_base != 0x800000 ||
      input.kernel_size != sizeof(macho.payload) ||
      input.kernel_entry != 0x800000) {
    return 1;
  }
#endif
#ifdef OS8_XNU_HAVE_UEFI_HANDOFF
  os8_xnu_efi_memory_descriptor_t efi_map[] = {
      {
          .type = OS8_XNU_EFI_CONVENTIONAL_MEMORY,
          .physical_start = 0x100000,
          .number_of_pages = 2,
      },
      {
          .type = OS8_XNU_EFI_ACPI_RECLAIM_MEMORY,
          .physical_start = 0x300000,
          .number_of_pages = 1,
          .attribute = 0x8000000000000000ULL,
      },
  };
  os8_xnu_range_t ranges[2];
  uint64_t range_count = 0;
  if (os8_xnu_uefi_memory_map_convert(
          efi_map, sizeof(efi_map), sizeof(efi_map[0]), ranges, 2,
          &range_count) != 0) {
    return 1;
  }
  if (range_count != 2 ||
      ranges[0].base != 0x100000 ||
      ranges[0].size != 0x2000 ||
      ranges[0].type != OS8_XNU_RANGE_USABLE ||
      ranges[1].type != OS8_XNU_RANGE_ACPI_RECLAIM ||
      ranges[1].attributes != 0) {
    return 1;
  }
  if (os8_xnu_boot_handoff_apply_uefi_memory_map(&input, ranges, range_count) != 0)
    return 1;
  if (os8_xnu_boot_handoff_apply_uefi_framebuffer(
          &input, 0x900000, 0x100000, 800, 600, 800, 4, 1) != 0) {
    return 1;
  }
  if (input.memory_map_entry_count != 2 ||
      input.memory_map_entry_size != sizeof(os8_xnu_range_t) ||
      input.framebuffer.pitch != 3200) {
    return 1;
  }
#endif
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
