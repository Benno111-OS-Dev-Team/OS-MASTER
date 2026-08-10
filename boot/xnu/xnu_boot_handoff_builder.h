#ifndef OS8_XNU_BOOT_HANDOFF_BUILDER_H
#define OS8_XNU_BOOT_HANDOFF_BUILDER_H

#include "xnu_boot_handoff.h"
#include "xnu_macho_loader.h"

typedef struct os8_xnu_boot_handoff_input {
  uint32_t arch;
  uint64_t kernel_base;
  uint64_t kernel_size;
  uint64_t kernel_entry;
  uint64_t boot_args_base;
  uint64_t boot_args_size;
  uint64_t memory_map_base;
  uint64_t memory_map_entry_count;
  uint64_t memory_map_entry_size;
  uint64_t platform_data_base;
  uint64_t platform_data_size;
  uint64_t timer_frequency_hz;
  uint64_t cpu_topology_base;
  uint64_t cpu_topology_size;
  os8_xnu_framebuffer_t framebuffer;
} os8_xnu_boot_handoff_input_t;

static inline uint32_t os8_xnu_boot_platform_kind(uint32_t arch) {
  if (arch == OS8_XNU_ARCH_X86_64) return OS8_XNU_PLATFORM_ACPI;
  if (arch == OS8_XNU_ARCH_ARM64) return OS8_XNU_PLATFORM_DEVICE_TREE;
  return 0;
}

static inline uint64_t os8_xnu_boot_required_flags(uint32_t arch) {
  if (arch == OS8_XNU_ARCH_X86_64)
    return OS8_XNU_BOOT_FLAG_GRAPHICS | OS8_XNU_BOOT_FLAG_ACPI;
  if (arch == OS8_XNU_ARCH_ARM64)
    return OS8_XNU_BOOT_FLAG_GRAPHICS | OS8_XNU_BOOT_FLAG_DEVICE_TREE;
  return 0;
}

static inline int os8_xnu_boot_handoff_apply_macho(
    os8_xnu_boot_handoff_input_t *in,
    const os8_xnu_macho64_image_t *macho,
    uint64_t loaded_kernel_base) {
  uint64_t kernel_size;
  uint64_t entry_delta;

  if (!in || !macho) return -1;
  if (!loaded_kernel_base) return -1;
  if (!macho->lowest_vmaddr || macho->highest_vmaddr <= macho->lowest_vmaddr)
    return -1;
  if (!macho->entry_vmaddr || macho->entry_vmaddr < macho->lowest_vmaddr ||
      macho->entry_vmaddr >= macho->highest_vmaddr) {
    return -1;
  }

  kernel_size = macho->highest_vmaddr - macho->lowest_vmaddr;
  entry_delta = macho->entry_vmaddr - macho->lowest_vmaddr;
  in->kernel_base = loaded_kernel_base;
  in->kernel_size = kernel_size;
  in->kernel_entry = loaded_kernel_base + entry_delta;
  if (in->kernel_entry < loaded_kernel_base) return -1;
  return 0;
}

static inline int os8_xnu_boot_handoff_apply_boot_args(
    os8_xnu_boot_handoff_input_t *in, uint64_t base, uint64_t size) {
  if (!in || !base || !size) return -1;
  in->boot_args_base = base;
  in->boot_args_size = size;
  return 0;
}

static inline int os8_xnu_boot_handoff_apply_memory_map(
    os8_xnu_boot_handoff_input_t *in, uint64_t base, uint64_t entry_count,
    uint64_t entry_size) {
  if (!in || !base || !entry_count) return -1;
  if (entry_size != sizeof(os8_xnu_range_t)) return -1;
  in->memory_map_base = base;
  in->memory_map_entry_count = entry_count;
  in->memory_map_entry_size = entry_size;
  return 0;
}

static inline int os8_xnu_boot_handoff_apply_platform_data(
    os8_xnu_boot_handoff_input_t *in, uint64_t base, uint64_t size) {
  if (!in || !base || !size) return -1;
  in->platform_data_base = base;
  in->platform_data_size = size;
  return 0;
}

static inline int os8_xnu_boot_handoff_apply_timer(
    os8_xnu_boot_handoff_input_t *in, uint64_t frequency_hz) {
  if (!in || !frequency_hz) return -1;
  in->timer_frequency_hz = frequency_hz;
  return 0;
}

static inline int os8_xnu_boot_handoff_apply_cpu_topology(
    os8_xnu_boot_handoff_input_t *in, uint64_t base, uint64_t size) {
  if (!in || !base || !size) return -1;
  in->cpu_topology_base = base;
  in->cpu_topology_size = size;
  return 0;
}

static inline int os8_xnu_boot_handoff_apply_framebuffer(
    os8_xnu_boot_handoff_input_t *in, uint64_t base, uint64_t size,
    uint32_t width, uint32_t height, uint32_t pitch, uint32_t pixel_format) {
  if (!in || !base || !size || !width || !height || !pitch) return -1;
  in->framebuffer.base = base;
  in->framebuffer.size = size;
  in->framebuffer.width = width;
  in->framebuffer.height = height;
  in->framebuffer.pitch = pitch;
  in->framebuffer.pixel_format = pixel_format;
  return 0;
}

static inline int os8_xnu_boot_handoff_build(
    os8_xnu_boot_handoff_t *out,
    const os8_xnu_boot_handoff_input_t *in) {
  uint32_t platform_kind;
  uint64_t flags;

  if (!out || !in) return -1;
  platform_kind = os8_xnu_boot_platform_kind(in->arch);
  flags = os8_xnu_boot_required_flags(in->arch);
  if (!platform_kind || !flags) return -1;
  if (!in->kernel_base || !in->kernel_size || !in->kernel_entry) return -1;
  if (!in->boot_args_base || !in->boot_args_size) return -1;
  if (!in->memory_map_base || !in->memory_map_entry_count) return -1;
  if (in->memory_map_entry_size != sizeof(os8_xnu_range_t)) return -1;
  if (!in->platform_data_base || !in->platform_data_size) return -1;
  if (!in->timer_frequency_hz) return -1;
  if (!in->cpu_topology_base || !in->cpu_topology_size) return -1;
  if (!in->framebuffer.base || !in->framebuffer.size ||
      !in->framebuffer.width || !in->framebuffer.height ||
      !in->framebuffer.pitch) {
    return -1;
  }

  out->magic = OS8_XNU_BOOT_HANDOFF_MAGIC;
  out->version = OS8_XNU_BOOT_HANDOFF_VERSION;
  out->flags = flags;
  out->arch = in->arch;
  out->platform_kind = platform_kind;
  out->kernel_base = in->kernel_base;
  out->kernel_size = in->kernel_size;
  out->kernel_entry = in->kernel_entry;
  out->boot_args_base = in->boot_args_base;
  out->boot_args_size = in->boot_args_size;
  out->memory_map_base = in->memory_map_base;
  out->memory_map_entry_count = in->memory_map_entry_count;
  out->memory_map_entry_size = in->memory_map_entry_size;
  out->platform_data_base = in->platform_data_base;
  out->platform_data_size = in->platform_data_size;
  out->timer_frequency_hz = in->timer_frequency_hz;
  out->cpu_topology_base = in->cpu_topology_base;
  out->cpu_topology_size = in->cpu_topology_size;
  out->framebuffer = in->framebuffer;
  return 0;
}

#endif
