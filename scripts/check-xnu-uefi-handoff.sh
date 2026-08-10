#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work_dir="${TMPDIR:-/tmp}/os8-xnu-uefi-check.$$"
source_file="$work_dir/check-xnu-uefi.c"
object_file="$work_dir/check-xnu-uefi.obj"

cleanup() {
  rm -rf "$work_dir"
}
trap cleanup EXIT

cc_bin="${CC:-}"
if [ -z "$cc_bin" ]; then
  cc_bin="$(command -v clang 2>/dev/null || true)"
fi
if [ -z "$cc_bin" ]; then
  echo "error: clang is required to compile-check XNU UEFI handoff helpers" >&2
  exit 1
fi

mkdir -p "$work_dir"
cat > "$source_file" <<'C'
#include <stddef.h>
#include <stdint.h>

#include "uefi.h"
#include "xnu_uefi_handoff.h"
#include "xnu_x86_64_boot_args.h"

#define CHECK(cond, name) typedef char check_##name[(cond) ? 1 : -1]

CHECK(sizeof(EFI_MEMORY_DESCRIPTOR) == sizeof(os8_xnu_efi_memory_descriptor_t),
      efi_descriptor_size);
CHECK(offsetof(EFI_MEMORY_DESCRIPTOR, Type) ==
          offsetof(os8_xnu_efi_memory_descriptor_t, type),
      efi_descriptor_type);
CHECK(offsetof(EFI_MEMORY_DESCRIPTOR, PhysicalStart) ==
          offsetof(os8_xnu_efi_memory_descriptor_t, physical_start),
      efi_descriptor_physical_start);
CHECK(offsetof(EFI_MEMORY_DESCRIPTOR, NumberOfPages) ==
          offsetof(os8_xnu_efi_memory_descriptor_t, number_of_pages),
      efi_descriptor_pages);
CHECK(EfiConventionalMemory == OS8_XNU_EFI_CONVENTIONAL_MEMORY,
      conventional_memory);
CHECK(EfiACPIReclaimMemory == OS8_XNU_EFI_ACPI_RECLAIM_MEMORY,
      acpi_reclaim_memory);
CHECK(EfiMemoryMappedIO == OS8_XNU_EFI_MEMORY_MAPPED_IO, mmio_memory);

int xnu_uefi_handoff_compile_check(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                   EFI_MEMORY_DESCRIPTOR *memory_map,
                                   uint64_t memory_map_size,
                                   uint64_t descriptor_size) {
  os8_xnu_boot_handoff_input_t input = {0};
  os8_xnu_x86_64_boot_args_t boot_args;
  os8_xnu_x86_64_boot_args_input_t boot_args_input = {0};
  os8_xnu_range_t ranges[8];
  uint64_t range_count = 0;

  if (!gop || !memory_map) return -1;
  input.arch = OS8_XNU_ARCH_X86_64;
  if (os8_xnu_uefi_memory_map_convert(memory_map, memory_map_size,
                                      descriptor_size, ranges, 8,
                                      &range_count) != 0) {
    return -1;
  }
  if (os8_xnu_boot_handoff_apply_uefi_memory_map(&input, ranges,
                                                 range_count) != 0) {
    return -1;
  }
  if (os8_xnu_boot_handoff_apply_uefi_framebuffer(
          &input, gop->Mode->FrameBufferBase, gop->Mode->FrameBufferSize,
          gop->Mode->Info->HorizontalResolution,
          gop->Mode->Info->VerticalResolution,
          gop->Mode->Info->PixelsPerScanLine, 4,
          (uint32_t)gop->Mode->Info->PixelFormat) != 0) {
    return -1;
  }
  boot_args_input.boot_args_base = 0x300000;
  boot_args_input.memory_map_base = (uint64_t)(uintptr_t)memory_map;
  boot_args_input.memory_map_size = memory_map_size;
  boot_args_input.memory_map_descriptor_size = descriptor_size;
  boot_args_input.memory_map_descriptor_version = 1;
  boot_args_input.kernel_base = 0x100000;
  boot_args_input.kernel_size = 0x200000;
  boot_args_input.efi_system_table = 0x700000;
  boot_args_input.framebuffer = input.framebuffer;
  if (os8_xnu_x86_64_boot_args_build(&boot_args, &boot_args_input) != 0) {
    return -1;
  }
  if (os8_xnu_boot_handoff_apply_x86_64_boot_args(&input,
                                                  &boot_args_input) != 0) {
    return -1;
  }
  return 0;
}
C

"$cc_bin" \
  -target x86_64-unknown-windows \
  -ffreestanding \
  -fshort-wchar \
  -mno-red-zone \
  -fno-stack-protector \
  -fno-builtin \
  -nostdlib \
  -Wall \
  -Wextra \
  -I"$root/boot/custom" \
  -I"$root/boot/xnu" \
  -c "$source_file" \
  -o "$object_file"

echo "[XNU] UEFI handoff helpers compile in bootloader mode"
