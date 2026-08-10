#ifndef OS8_XNU_X86_64_BOOT_ARGS_H
#define OS8_XNU_X86_64_BOOT_ARGS_H

#include <stdint.h>

#include "xnu_boot_handoff_builder.h"

#define OS8_XNU_X86_64_BOOT_LINE_LENGTH 1024U
#define OS8_XNU_X86_64_BOOT_ARGS_REVISION 0U
#define OS8_XNU_X86_64_BOOT_ARGS_VERSION 2U
#define OS8_XNU_X86_64_BOOT_ARGS_EFI_MODE_64 64U
#define OS8_XNU_X86_64_BOOT_ARGS_SIZE 4096U
#define OS8_XNU_X86_64_BOOT_ARGS_GRAPHICS_MODE 1U

typedef struct os8_xnu_x86_64_boot_video_v1 {
  uint32_t v_baseAddr;
  uint32_t v_display;
  uint32_t v_rowBytes;
  uint32_t v_width;
  uint32_t v_height;
  uint32_t v_depth;
} os8_xnu_x86_64_boot_video_v1_t;

typedef struct os8_xnu_x86_64_boot_video {
  uint32_t v_display;
  uint32_t v_rowBytes;
  uint32_t v_width;
  uint32_t v_height;
  uint32_t v_depth;
  uint8_t v_rotate;
  uint8_t v_resv_byte[3];
  uint32_t v_resv[6];
  uint64_t v_baseAddr;
} os8_xnu_x86_64_boot_video_t;

typedef struct os8_xnu_x86_64_boot_args {
  uint16_t Revision;
  uint16_t Version;
  uint8_t efiMode;
  uint8_t debugMode;
  uint16_t flags;
  char CommandLine[OS8_XNU_X86_64_BOOT_LINE_LENGTH];
  uint32_t MemoryMap;
  uint32_t MemoryMapSize;
  uint32_t MemoryMapDescriptorSize;
  uint32_t MemoryMapDescriptorVersion;
  os8_xnu_x86_64_boot_video_v1_t VideoV1;
  uint32_t deviceTreeP;
  uint32_t deviceTreeLength;
  uint32_t kaddr;
  uint32_t ksize;
  uint32_t efiRuntimeServicesPageStart;
  uint32_t efiRuntimeServicesPageCount;
  uint64_t efiRuntimeServicesVirtualPageStart;
  uint32_t efiSystemTable;
  uint32_t kslide;
  uint32_t performanceDataStart;
  uint32_t performanceDataSize;
  uint32_t keyStoreDataStart;
  uint32_t keyStoreDataSize;
  uint64_t bootMemStart;
  uint64_t bootMemSize;
  uint64_t PhysicalMemorySize;
  uint64_t FSBFrequency;
  uint64_t pciConfigSpaceBaseAddress;
  uint32_t pciConfigSpaceStartBusNumber;
  uint32_t pciConfigSpaceEndBusNumber;
  uint32_t csrActiveConfig;
  uint32_t csrCapabilities;
  uint32_t boot_SMC_plimit;
  uint16_t bootProgressMeterStart;
  uint16_t bootProgressMeterEnd;
  os8_xnu_x86_64_boot_video_t Video;
  uint32_t apfsDataStart;
  uint32_t apfsDataSize;
  uint64_t KC_hdrs_vaddr;
  uint64_t arvRootHashStart;
  uint64_t arvRootHashSize;
  uint64_t arvManifestStart;
  uint64_t arvManifestSize;
  uint64_t bsARVRootHashStart;
  uint64_t bsARVRootHashSize;
  uint64_t bsARVManifestStart;
  uint64_t bsARVManifestSize;
  uint32_t __reserved4[692];
} os8_xnu_x86_64_boot_args_t;

typedef struct os8_xnu_x86_64_boot_args_input {
  uint64_t boot_args_base;
  uint64_t memory_map_base;
  uint64_t memory_map_size;
  uint64_t memory_map_descriptor_size;
  uint64_t memory_map_descriptor_version;
  uint64_t kernel_base;
  uint64_t kernel_size;
  uint64_t efi_system_table;
  uint64_t physical_memory_size;
  const char *command_line;
  uint64_t command_line_size;
  os8_xnu_framebuffer_t framebuffer;
} os8_xnu_x86_64_boot_args_input_t;

typedef char os8_xnu_x86_64_boot_args_must_be_4096[
    sizeof(os8_xnu_x86_64_boot_args_t) == OS8_XNU_X86_64_BOOT_ARGS_SIZE ? 1
                                                                        : -1];

static inline int os8_xnu_x86_64_u32_from_u64(uint64_t value,
                                              uint32_t *out) {
  if (!out || value > 0xFFFFFFFFULL) return -1;
  *out = (uint32_t)value;
  return 0;
}

static inline void os8_xnu_x86_64_zero_boot_args(
    os8_xnu_x86_64_boot_args_t *out) {
  uint8_t *bytes = (uint8_t *)out;
  uint64_t i;

  for (i = 0; i < (uint64_t)sizeof(*out); ++i) bytes[i] = 0;
}

static inline int os8_xnu_x86_64_copy_command_line(
    char out[OS8_XNU_X86_64_BOOT_LINE_LENGTH], const char *in,
    uint64_t in_size) {
  uint64_t i;

  if (!out) return -1;
  if (!in) return 0;
  if (!in_size) return 0;
  if (in_size >= OS8_XNU_X86_64_BOOT_LINE_LENGTH) return -1;
  for (i = 0; i < in_size; ++i) {
    out[i] = in[i];
    if (in[i] == '\0') return 0;
  }
  out[in_size] = '\0';
  return 0;
}

static inline int os8_xnu_x86_64_boot_args_build(
    os8_xnu_x86_64_boot_args_t *out,
    const os8_xnu_x86_64_boot_args_input_t *in) {
  uint32_t memory_map;
  uint32_t memory_map_size;
  uint32_t memory_map_descriptor_size;
  uint32_t memory_map_descriptor_version;
  uint32_t kernel_base;
  uint32_t kernel_size;
  uint32_t efi_system_table;

  if (!out || !in) return -1;
  if (!in->boot_args_base || !in->memory_map_base || !in->memory_map_size)
    return -1;
  if (!in->memory_map_descriptor_size) return -1;
  if (!in->kernel_base || !in->kernel_size) return -1;
  if (!in->efi_system_table) return -1;
  if (!in->framebuffer.base || !in->framebuffer.width ||
      !in->framebuffer.height || !in->framebuffer.pitch) {
    return -1;
  }
  if (os8_xnu_x86_64_u32_from_u64(in->memory_map_base, &memory_map) != 0)
    return -1;
  if (os8_xnu_x86_64_u32_from_u64(in->memory_map_size, &memory_map_size) != 0)
    return -1;
  if (os8_xnu_x86_64_u32_from_u64(in->memory_map_descriptor_size,
                                  &memory_map_descriptor_size) != 0) {
    return -1;
  }
  if (os8_xnu_x86_64_u32_from_u64(in->memory_map_descriptor_version,
                                  &memory_map_descriptor_version) != 0) {
    return -1;
  }
  if (os8_xnu_x86_64_u32_from_u64(in->kernel_base, &kernel_base) != 0)
    return -1;
  if (os8_xnu_x86_64_u32_from_u64(in->kernel_size, &kernel_size) != 0)
    return -1;
  if (os8_xnu_x86_64_u32_from_u64(in->efi_system_table, &efi_system_table) !=
      0) {
    return -1;
  }

  os8_xnu_x86_64_zero_boot_args(out);
  out->Revision = OS8_XNU_X86_64_BOOT_ARGS_REVISION;
  out->Version = OS8_XNU_X86_64_BOOT_ARGS_VERSION;
  out->efiMode = OS8_XNU_X86_64_BOOT_ARGS_EFI_MODE_64;
  out->MemoryMap = memory_map;
  out->MemoryMapSize = memory_map_size;
  out->MemoryMapDescriptorSize = memory_map_descriptor_size;
  out->MemoryMapDescriptorVersion = memory_map_descriptor_version;
  out->kaddr = kernel_base;
  out->ksize = kernel_size;
  out->efiSystemTable = efi_system_table;
  out->PhysicalMemorySize = in->physical_memory_size;
  out->Video.v_display = OS8_XNU_X86_64_BOOT_ARGS_GRAPHICS_MODE;
  out->Video.v_rowBytes = in->framebuffer.pitch;
  out->Video.v_width = in->framebuffer.width;
  out->Video.v_height = in->framebuffer.height;
  out->Video.v_depth = 32;
  out->Video.v_baseAddr = in->framebuffer.base;
  return os8_xnu_x86_64_copy_command_line(
      out->CommandLine, in->command_line, in->command_line_size);
}

static inline int os8_xnu_boot_handoff_apply_x86_64_boot_args(
    os8_xnu_boot_handoff_input_t *handoff,
    const os8_xnu_x86_64_boot_args_input_t *boot_args) {
  if (!boot_args) return -1;
  return os8_xnu_boot_handoff_apply_boot_args(
      handoff, boot_args->boot_args_base, OS8_XNU_X86_64_BOOT_ARGS_SIZE);
}

#endif
