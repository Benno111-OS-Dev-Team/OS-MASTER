#ifndef OS8_XNU_BOOT_HANDOFF_H
#define OS8_XNU_BOOT_HANDOFF_H

#include <stdint.h>

#define OS8_XNU_BOOT_HANDOFF_MAGIC 0x584E55424F4F5431ULL
#define OS8_XNU_BOOT_HANDOFF_VERSION 1ULL

#define OS8_XNU_BOOT_FLAG_GRAPHICS 0x00000001ULL
#define OS8_XNU_BOOT_FLAG_ACPI 0x00000002ULL
#define OS8_XNU_BOOT_FLAG_DEVICE_TREE 0x00000004ULL

#define OS8_XNU_ARCH_X86_64 1U
#define OS8_XNU_ARCH_ARM64 2U

#define OS8_XNU_PLATFORM_ACPI 1U
#define OS8_XNU_PLATFORM_DEVICE_TREE 2U

typedef struct os8_xnu_range {
  uint64_t base;
  uint64_t size;
  uint32_t type;
  uint32_t attributes;
} os8_xnu_range_t;

typedef struct os8_xnu_framebuffer {
  uint64_t base;
  uint64_t size;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint32_t pixel_format;
} os8_xnu_framebuffer_t;

typedef struct os8_xnu_boot_handoff {
  uint64_t magic;
  uint64_t version;
  uint64_t flags;
  uint32_t arch;
  uint32_t platform_kind;
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
} os8_xnu_boot_handoff_t;

#endif
