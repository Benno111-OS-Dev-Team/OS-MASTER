#ifndef OS8_X86_64_BOOT_HANDOFF_H
#define OS8_X86_64_BOOT_HANDOFF_H

#include "types.h"

#define OS8_BOOT_HANDOFF_MAGIC 0x4F5338424F4F5448ULL
#define OS8_BOOT_HANDOFF_VERSION 1

typedef struct os8_boot_handoff {
  uint64_t magic;
  uint64_t version;
  uint64_t framebuffer_addr;
  uint64_t framebuffer_width;
  uint64_t framebuffer_height;
  uint64_t framebuffer_pitch;
  uint64_t rsdp_addr;
  uint64_t hhdm_offset;
  uint64_t bootstrap_file_addr;
  uint64_t bootstrap_file_size;
  uint64_t kernel_file_addr;
  uint64_t kernel_file_size;
  uint64_t cmdline_addr;
} os8_boot_handoff_t;

#endif
