#ifndef OS8_XNU_UEFI_HANDOFF_H
#define OS8_XNU_UEFI_HANDOFF_H

#include <stdint.h>

#include "xnu_boot_handoff.h"
#include "xnu_boot_handoff_builder.h"

#define OS8_XNU_RANGE_USABLE 1U
#define OS8_XNU_RANGE_RESERVED 2U
#define OS8_XNU_RANGE_ACPI_RECLAIM 3U
#define OS8_XNU_RANGE_ACPI_NVS 4U
#define OS8_XNU_RANGE_MMIO 5U

#define OS8_XNU_EFI_LOADER_CODE 1U
#define OS8_XNU_EFI_LOADER_DATA 2U
#define OS8_XNU_EFI_BOOT_SERVICES_CODE 3U
#define OS8_XNU_EFI_BOOT_SERVICES_DATA 4U
#define OS8_XNU_EFI_RUNTIME_SERVICES_CODE 5U
#define OS8_XNU_EFI_RUNTIME_SERVICES_DATA 6U
#define OS8_XNU_EFI_CONVENTIONAL_MEMORY 7U
#define OS8_XNU_EFI_UNUSABLE_MEMORY 8U
#define OS8_XNU_EFI_ACPI_RECLAIM_MEMORY 9U
#define OS8_XNU_EFI_ACPI_MEMORY_NVS 10U
#define OS8_XNU_EFI_MEMORY_MAPPED_IO 11U
#define OS8_XNU_EFI_MEMORY_MAPPED_IO_PORT_SPACE 12U
#define OS8_XNU_EFI_PAL_CODE 13U
#define OS8_XNU_EFI_PERSISTENT_MEMORY 14U

typedef struct os8_xnu_efi_memory_descriptor {
  uint32_t type;
  uint32_t pad;
  uint64_t physical_start;
  uint64_t virtual_start;
  uint64_t number_of_pages;
  uint64_t attribute;
} os8_xnu_efi_memory_descriptor_t;

static inline uint32_t os8_xnu_range_type_from_efi(uint32_t efi_type) {
  if (efi_type == OS8_XNU_EFI_CONVENTIONAL_MEMORY ||
      efi_type == OS8_XNU_EFI_LOADER_CODE ||
      efi_type == OS8_XNU_EFI_LOADER_DATA ||
      efi_type == OS8_XNU_EFI_BOOT_SERVICES_CODE ||
      efi_type == OS8_XNU_EFI_BOOT_SERVICES_DATA) {
    return OS8_XNU_RANGE_USABLE;
  }
  if (efi_type == OS8_XNU_EFI_ACPI_RECLAIM_MEMORY)
    return OS8_XNU_RANGE_ACPI_RECLAIM;
  if (efi_type == OS8_XNU_EFI_ACPI_MEMORY_NVS) return OS8_XNU_RANGE_ACPI_NVS;
  if (efi_type == OS8_XNU_EFI_MEMORY_MAPPED_IO ||
      efi_type == OS8_XNU_EFI_MEMORY_MAPPED_IO_PORT_SPACE) {
    return OS8_XNU_RANGE_MMIO;
  }
  return OS8_XNU_RANGE_RESERVED;
}

static inline int os8_xnu_uefi_memory_map_convert(
    const void *efi_memory_map, uint64_t efi_memory_map_size,
    uint64_t efi_descriptor_size, os8_xnu_range_t *ranges,
    uint64_t range_capacity, uint64_t *range_count_out) {
  const uint8_t *cursor = (const uint8_t *)efi_memory_map;
  uint64_t count;

  if (!efi_memory_map || !ranges || !range_count_out) return -1;
  if (efi_descriptor_size < sizeof(os8_xnu_efi_memory_descriptor_t))
    return -1;
  if (efi_memory_map_size == 0 ||
      (efi_memory_map_size % efi_descriptor_size) != 0) {
    return -1;
  }

  count = efi_memory_map_size / efi_descriptor_size;
  if (count > range_capacity) return -1;

  for (uint64_t i = 0; i < count; i++) {
    const os8_xnu_efi_memory_descriptor_t *desc =
        (const os8_xnu_efi_memory_descriptor_t *)(cursor + i * efi_descriptor_size);
    uint64_t size = desc->number_of_pages << 12;
    if (!desc->physical_start || !desc->number_of_pages || (size >> 12) != desc->number_of_pages)
      return -1;
    ranges[i].base = desc->physical_start;
    ranges[i].size = size;
    ranges[i].type = os8_xnu_range_type_from_efi(desc->type);
    ranges[i].attributes = (uint32_t)(desc->attribute & 0xffffffffU);
  }

  *range_count_out = count;
  return 0;
}

static inline int os8_xnu_boot_handoff_apply_uefi_memory_map(
    os8_xnu_boot_handoff_input_t *in, os8_xnu_range_t *ranges,
    uint64_t range_count) {
  if (!in || !ranges || !range_count) return -1;
  return os8_xnu_boot_handoff_apply_memory_map(
      in, (uint64_t)(uintptr_t)ranges, range_count, sizeof(os8_xnu_range_t));
}

static inline int os8_xnu_boot_handoff_apply_uefi_framebuffer(
    os8_xnu_boot_handoff_input_t *in, uint64_t base, uint64_t size,
    uint32_t width, uint32_t height, uint32_t pixels_per_scan_line,
    uint32_t bytes_per_pixel, uint32_t pixel_format) {
  uint64_t pitch;
  if (!pixels_per_scan_line || !bytes_per_pixel) return -1;
  pitch = (uint64_t)pixels_per_scan_line * bytes_per_pixel;
  if (pitch / bytes_per_pixel != pixels_per_scan_line) return -1;
  if (pitch > UINT32_MAX) return -1;
  return os8_xnu_boot_handoff_apply_framebuffer(
      in, base, size, width, height, (uint32_t)pitch, pixel_format);
}

#endif
