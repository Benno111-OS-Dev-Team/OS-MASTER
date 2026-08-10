#ifndef OS8_XNU_MACHO_LOADER_H
#define OS8_XNU_MACHO_LOADER_H

#include <stdint.h>

#include "xnu_boot_handoff.h"

#define OS8_XNU_MH_MAGIC_64 0xFEEDFACFULL
#define OS8_XNU_CPU_TYPE_X86_64 0x01000007U
#define OS8_XNU_CPU_TYPE_ARM64 0x0100000CU
#define OS8_XNU_LC_SEGMENT_64 0x19U
#define OS8_XNU_LC_MAIN 0x80000028U

typedef struct os8_xnu_macho64_header {
  uint32_t magic;
  uint32_t cputype;
  uint32_t cpusubtype;
  uint32_t filetype;
  uint32_t ncmds;
  uint32_t sizeofcmds;
  uint32_t flags;
  uint32_t reserved;
} os8_xnu_macho64_header_t;

typedef struct os8_xnu_macho64_load_command {
  uint32_t cmd;
  uint32_t cmdsize;
} os8_xnu_macho64_load_command_t;

typedef struct os8_xnu_macho64_segment_command {
  uint32_t cmd;
  uint32_t cmdsize;
  char segname[16];
  uint64_t vmaddr;
  uint64_t vmsize;
  uint64_t fileoff;
  uint64_t filesize;
  uint32_t maxprot;
  uint32_t initprot;
  uint32_t nsects;
  uint32_t flags;
} os8_xnu_macho64_segment_command_t;

typedef struct os8_xnu_macho64_entry_point_command {
  uint32_t cmd;
  uint32_t cmdsize;
  uint64_t entryoff;
  uint64_t stacksize;
} os8_xnu_macho64_entry_point_command_t;

typedef struct os8_xnu_macho64_image {
  uint32_t cputype;
  uint32_t segment_count;
  uint64_t lowest_vmaddr;
  uint64_t highest_vmaddr;
  uint64_t entry_fileoff;
  uint64_t entry_vmaddr;
} os8_xnu_macho64_image_t;

typedef struct os8_xnu_macho64_segment {
  uint64_t vmaddr;
  uint64_t vmsize;
  uint64_t fileoff;
  uint64_t filesize;
  uint32_t maxprot;
  uint32_t initprot;
} os8_xnu_macho64_segment_t;

static inline int os8_xnu_u64_add_overflow(uint64_t a, uint64_t b,
                                           uint64_t *out) {
  *out = a + b;
  return *out < a;
}

static inline int os8_xnu_range_exceeds(uint64_t offset, uint64_t length,
                                        uint64_t size) {
  uint64_t end;
  if (os8_xnu_u64_add_overflow(offset, length, &end)) return 1;
  return end > size;
}

static inline int os8_xnu_macho64_expected_cpu(uint32_t arch,
                                               uint32_t *cpu_out) {
  if (arch == OS8_XNU_ARCH_X86_64) {
    *cpu_out = OS8_XNU_CPU_TYPE_X86_64;
    return 0;
  }
  if (arch == OS8_XNU_ARCH_ARM64) {
    *cpu_out = OS8_XNU_CPU_TYPE_ARM64;
    return 0;
  }
  return -1;
}

static inline int os8_xnu_macho64_inspect(const void *image, uint64_t size,
                                          uint32_t arch,
                                          os8_xnu_macho64_image_t *out) {
  const uint8_t *bytes = (const uint8_t *)image;
  const os8_xnu_macho64_header_t *header;
  uint32_t expected_cpu;
  uint64_t commands_offset = sizeof(os8_xnu_macho64_header_t);
  uint64_t commands_end;
  uint64_t cursor;
  uint32_t segment_count = 0;
  uint64_t lowest_vmaddr = UINT64_MAX;
  uint64_t highest_vmaddr = 0;
  uint64_t entry_fileoff = 0;
  uint64_t entry_vmaddr = 0;

  if (!image || !out) return -1;
  if (size < sizeof(os8_xnu_macho64_header_t)) return -1;
  if (os8_xnu_macho64_expected_cpu(arch, &expected_cpu) != 0) return -1;

  header = (const os8_xnu_macho64_header_t *)image;
  if (header->magic != OS8_XNU_MH_MAGIC_64) return -1;
  if (header->cputype != expected_cpu) return -1;
  if (os8_xnu_range_exceeds(commands_offset, header->sizeofcmds, size))
    return -1;
  if (os8_xnu_u64_add_overflow(commands_offset, header->sizeofcmds,
                               &commands_end))
    return -1;

  cursor = commands_offset;
  for (uint32_t i = 0; i < header->ncmds; i++) {
    const os8_xnu_macho64_load_command_t *lc;
    if (os8_xnu_range_exceeds(cursor, sizeof(*lc), size)) return -1;
    lc = (const os8_xnu_macho64_load_command_t *)(bytes + cursor);
    if (lc->cmdsize < sizeof(*lc)) return -1;
    if (os8_xnu_range_exceeds(cursor, lc->cmdsize, size)) return -1;
    if (cursor + lc->cmdsize > commands_end) return -1;

    if (lc->cmd == OS8_XNU_LC_SEGMENT_64) {
      const os8_xnu_macho64_segment_command_t *seg;
      uint64_t vm_end;
      if (lc->cmdsize < sizeof(*seg)) return -1;
      seg = (const os8_xnu_macho64_segment_command_t *)(bytes + cursor);
      if (seg->vmsize != 0) {
        if (os8_xnu_range_exceeds(seg->fileoff, seg->filesize, size))
          return -1;
        if (seg->filesize > seg->vmsize) return -1;
        if (os8_xnu_u64_add_overflow(seg->vmaddr, seg->vmsize, &vm_end))
          return -1;
        if (seg->vmaddr < lowest_vmaddr) lowest_vmaddr = seg->vmaddr;
        if (vm_end > highest_vmaddr) highest_vmaddr = vm_end;
        segment_count++;
      }
    } else if (lc->cmd == OS8_XNU_LC_MAIN) {
      const os8_xnu_macho64_entry_point_command_t *entry;
      if (lc->cmdsize < sizeof(*entry)) return -1;
      entry = (const os8_xnu_macho64_entry_point_command_t *)(bytes + cursor);
      if (entry->entryoff >= size) return -1;
      entry_fileoff = entry->entryoff;
    }

    cursor += lc->cmdsize;
  }

  if (cursor != commands_end) return -1;
  if (segment_count == 0) return -1;
  if (entry_fileoff != 0) {
    cursor = commands_offset;
    for (uint32_t i = 0; i < header->ncmds; i++) {
      const os8_xnu_macho64_load_command_t *lc =
          (const os8_xnu_macho64_load_command_t *)(bytes + cursor);
      if (lc->cmd == OS8_XNU_LC_SEGMENT_64) {
        const os8_xnu_macho64_segment_command_t *seg =
            (const os8_xnu_macho64_segment_command_t *)(bytes + cursor);
        uint64_t file_end;
        if (os8_xnu_u64_add_overflow(seg->fileoff, seg->filesize, &file_end))
          return -1;
        if (entry_fileoff >= seg->fileoff && entry_fileoff < file_end) {
          entry_vmaddr = seg->vmaddr + (entry_fileoff - seg->fileoff);
          break;
        }
      }
      cursor += lc->cmdsize;
    }
    if (entry_vmaddr == 0) return -1;
  }

  out->cputype = header->cputype;
  out->segment_count = segment_count;
  out->lowest_vmaddr = lowest_vmaddr;
  out->highest_vmaddr = highest_vmaddr;
  out->entry_fileoff = entry_fileoff;
  out->entry_vmaddr = entry_vmaddr;
  return 0;
}

static inline int os8_xnu_macho64_segment_at(
    const void *image, uint64_t size, uint32_t arch, uint32_t segment_index,
    os8_xnu_macho64_segment_t *out) {
  const uint8_t *bytes = (const uint8_t *)image;
  const os8_xnu_macho64_header_t *header;
  uint32_t expected_cpu;
  uint64_t commands_offset = sizeof(os8_xnu_macho64_header_t);
  uint64_t commands_end;
  uint64_t cursor;
  uint32_t seen = 0;

  if (!image || !out) return -1;
  if (size < sizeof(os8_xnu_macho64_header_t)) return -1;
  if (os8_xnu_macho64_expected_cpu(arch, &expected_cpu) != 0) return -1;

  header = (const os8_xnu_macho64_header_t *)image;
  if (header->magic != OS8_XNU_MH_MAGIC_64) return -1;
  if (header->cputype != expected_cpu) return -1;
  if (os8_xnu_range_exceeds(commands_offset, header->sizeofcmds, size))
    return -1;
  if (os8_xnu_u64_add_overflow(commands_offset, header->sizeofcmds,
                               &commands_end))
    return -1;

  cursor = commands_offset;
  for (uint32_t i = 0; i < header->ncmds; i++) {
    const os8_xnu_macho64_load_command_t *lc;
    if (os8_xnu_range_exceeds(cursor, sizeof(*lc), size)) return -1;
    lc = (const os8_xnu_macho64_load_command_t *)(bytes + cursor);
    if (lc->cmdsize < sizeof(*lc)) return -1;
    if (os8_xnu_range_exceeds(cursor, lc->cmdsize, size)) return -1;
    if (cursor + lc->cmdsize > commands_end) return -1;

    if (lc->cmd == OS8_XNU_LC_SEGMENT_64) {
      const os8_xnu_macho64_segment_command_t *seg;
      if (lc->cmdsize < sizeof(*seg)) return -1;
      seg = (const os8_xnu_macho64_segment_command_t *)(bytes + cursor);
      if (seg->vmsize != 0) {
        uint64_t vm_end;
        if (os8_xnu_range_exceeds(seg->fileoff, seg->filesize, size))
          return -1;
        if (seg->filesize > seg->vmsize) return -1;
        if (os8_xnu_u64_add_overflow(seg->vmaddr, seg->vmsize, &vm_end))
          return -1;
        if (seen == segment_index) {
          out->vmaddr = seg->vmaddr;
          out->vmsize = seg->vmsize;
          out->fileoff = seg->fileoff;
          out->filesize = seg->filesize;
          out->maxprot = seg->maxprot;
          out->initprot = seg->initprot;
          return 0;
        }
        seen++;
      }
    }

    cursor += lc->cmdsize;
  }

  if (cursor != commands_end) return -1;
  return -1;
}

#endif
