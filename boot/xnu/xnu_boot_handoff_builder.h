#ifndef OS8_XNU_BOOT_HANDOFF_BUILDER_H
#define OS8_XNU_BOOT_HANDOFF_BUILDER_H

#include "xnu_boot_handoff.h"

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
