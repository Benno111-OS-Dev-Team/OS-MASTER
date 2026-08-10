#include "uefi.h"
#include "xnu_macho_loader.h"
#include "xnu_x86_64_boot_args.h"

#define HHDM_OFFSET 0xffff800000000000ULL
#define KERNEL_VIRT_BASE 0xffffffff80000000ULL
#define PAGE_PRESENT 0x001ULL
#define PAGE_WRITE 0x002ULL
#define PAGE_HUGE 0x080ULL
#define OS8_BOOT_HANDOFF_MAGIC 0x4F5338424F4F5448ULL
#define OS8_BOOT_HANDOFF_VERSION 1
#define OS8_MAX_FRAMEBUFFER_DIMENSION 16384ULL
#define OS8_XNU_BOOT_ARGS_MAX_ADDRESS 0xffffffffULL

typedef struct {
  uint8_t ident[16];
  uint16_t type;
  uint16_t machine;
  uint32_t version;
  uint64_t entry;
  uint64_t phoff;
  uint64_t shoff;
  uint32_t flags;
  uint16_t ehsize;
  uint16_t phentsize;
  uint16_t phnum;
  uint16_t shentsize;
  uint16_t shnum;
  uint16_t shstrndx;
} Elf64_Ehdr;

typedef struct {
  uint32_t type;
  uint32_t flags;
  uint64_t offset;
  uint64_t vaddr;
  uint64_t paddr;
  uint64_t filesz;
  uint64_t memsz;
  uint64_t align;
} Elf64_Phdr;

typedef struct {
  uint32_t name;
  uint32_t type;
  uint64_t flags;
  uint64_t addr;
  uint64_t offset;
  uint64_t size;
  uint32_t link;
  uint32_t info;
  uint64_t addralign;
  uint64_t entsize;
} Elf64_Shdr;

typedef struct {
  uint32_t name;
  uint8_t info;
  uint8_t other;
  uint16_t shndx;
  uint64_t value;
  uint64_t size;
} Elf64_Sym;

typedef struct {
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
} OS8_BOOT_HANDOFF;

struct limine_framebuffer {
  void *address;
  uint64_t width;
  uint64_t height;
  uint64_t pitch;
  uint16_t bpp;
  uint8_t memory_model;
  uint8_t red_mask_size;
  uint8_t red_mask_shift;
  uint8_t green_mask_size;
  uint8_t green_mask_shift;
  uint8_t blue_mask_size;
  uint8_t blue_mask_shift;
  uint8_t unused[7];
  uint64_t edid_size;
  void *edid;
  uint64_t mode_count;
  void *modes;
};

struct limine_framebuffer_response {
  uint64_t revision;
  uint64_t framebuffer_count;
  struct limine_framebuffer **framebuffers;
};

struct limine_hhdm_response {
  uint64_t revision;
  uint64_t offset;
};

struct limine_kernel_address_response {
  uint64_t revision;
  uint64_t physical_base;
  uint64_t virtual_base;
};

struct limine_rsdp_response {
  uint64_t revision;
  void *address;
};

static EFI_SYSTEM_TABLE *g_st;
static EFI_HANDLE g_image;

typedef struct {
  uint64_t virt;
  uint64_t phys;
  uint64_t size;
} LoadedSegment;

static LoadedSegment g_loaded_segments[16];
static uint16_t g_loaded_segment_count;
static uint64_t g_kernel_physical_base;

static int u64_add_overflow(uint64_t a, uint64_t b, uint64_t *out) {
  if (a > UINT64_MAX - b) return 1;
  *out = a + b;
  return 0;
}

static int u64_mul_overflow(uint64_t a, uint64_t b, uint64_t *out) {
  if (a != 0 && b > UINT64_MAX / a) return 1;
  *out = a * b;
  return 0;
}

static int u64_range_exceeds(uint64_t offset, uint64_t length, uint64_t size) {
  uint64_t end = 0;
  return u64_add_overflow(offset, length, &end) || end > size;
}

static int gop_framebuffer_is_sane(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop) {
  uint64_t width;
  uint64_t height;
  uint64_t pixels_per_scanline;
  uint64_t min_pitch;
  uint64_t pitch;
  uint64_t framebuffer_bytes;

  if (!gop || !gop->Mode || !gop->Mode->Info)
    return 0;
  if (!gop->Mode->FrameBufferBase || !gop->Mode->FrameBufferSize)
    return 0;

  width = gop->Mode->Info->HorizontalResolution;
  height = gop->Mode->Info->VerticalResolution;
  pixels_per_scanline = gop->Mode->Info->PixelsPerScanLine;
  if (!width || !height || !pixels_per_scanline)
    return 0;
  if (width > OS8_MAX_FRAMEBUFFER_DIMENSION ||
      height > OS8_MAX_FRAMEBUFFER_DIMENSION)
    return 0;
  if (pixels_per_scanline < width)
    return 0;
  if (u64_mul_overflow(width, 4ULL, &min_pitch) ||
      u64_mul_overflow(pixels_per_scanline, 4ULL, &pitch))
    return 0;
  if (pitch > min_pitch * 8ULL)
    return 0;
  if (u64_mul_overflow(pitch, height, &framebuffer_bytes))
    return 0;
  if (gop->Mode->FrameBufferSize < framebuffer_bytes)
    return 0;

  return 1;
}

static EFI_STATUS error(const char *code, const char *message, EFI_STATUS status) {
  efi_print(g_st, "\nOS8 startup error ");
  efi_print(g_st, code);
  efi_print(g_st, ": ");
  efi_print(g_st, message);
  efi_print(g_st, " status=");
  efi_print_hex(g_st, status);
  efi_print(g_st, "\n");
  return status ? status : EFI_LOAD_ERROR;
}

static int verify_buffer(const void *buffer, uint64_t size, const char *expected_hex) {
  uint8_t actual[32];
  uint8_t expected[32];
  if (!hex_to_bytes(expected_hex, expected, sizeof(expected))) return 0;
  sha256((const uint8_t *)buffer, size, actual);
  return efi_memcmp(actual, expected, sizeof(actual)) == 0;
}

static EFI_STATUS alloc_zero_pages(uint64_t pages, uint64_t *phys_out) {
  EFI_PHYSICAL_ADDRESS phys = 0;
  EFI_STATUS status = g_st->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &phys);
  if (EFI_ERROR(status)) return status;
  efi_memset((void *)(uintptr_t)phys, 0, pages * 4096);
  *phys_out = phys;
  return EFI_SUCCESS;
}

static EFI_STATUS alloc_zero_pages_below(uint64_t pages, uint64_t max_address,
                                         uint64_t *phys_out) {
  EFI_PHYSICAL_ADDRESS phys = max_address;
  EFI_STATUS status = g_st->BootServices->AllocatePages(
      AllocateMaxAddress, EfiLoaderData, pages, &phys);
  if (EFI_ERROR(status)) return status;
  efi_memset((void *)(uintptr_t)phys, 0, pages * 4096);
  *phys_out = phys;
  return EFI_SUCCESS;
}

static EFI_STATUS alloc_zero_bytes_below(uint64_t bytes, uint64_t max_address,
                                         uint64_t *phys_out,
                                         uint64_t *size_out) {
  uint64_t pages;
  uint64_t rounded;

  if (!bytes) return EFI_INVALID_PARAMETER;
  if (u64_add_overflow(bytes, 0xfffULL, &rounded)) return EFI_OUT_OF_RESOURCES;
  pages = rounded >> 12;
  if (!pages) return EFI_OUT_OF_RESOURCES;
  *size_out = pages << 12;
  return alloc_zero_pages_below(pages, max_address, phys_out);
}

static uint64_t *new_table(void) {
  uint64_t phys = 0;
  if (EFI_ERROR(alloc_zero_pages(1, &phys))) return NULL;
  return (uint64_t *)(uintptr_t)phys;
}

static EFI_STATUS map_2m(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
  uint64_t pml4_i = (virt >> 39) & 0x1ff;
  uint64_t pdpt_i = (virt >> 30) & 0x1ff;
  uint64_t pd_i = (virt >> 21) & 0x1ff;

  if (!(pml4[pml4_i] & PAGE_PRESENT)) {
    uint64_t *pdpt = new_table();
    if (!pdpt) return EFI_OUT_OF_RESOURCES;
    pml4[pml4_i] = (uint64_t)(uintptr_t)pdpt | PAGE_PRESENT | PAGE_WRITE;
  }
  uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[pml4_i] & ~0xfffULL);
  if (!(pdpt[pdpt_i] & PAGE_PRESENT)) {
    uint64_t *pd = new_table();
    if (!pd) return EFI_OUT_OF_RESOURCES;
    pdpt[pdpt_i] = (uint64_t)(uintptr_t)pd | PAGE_PRESENT | PAGE_WRITE;
  }
  uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_i] & ~0xfffULL);
  pd[pd_i] = (phys & ~0x1fffffULL) | flags | PAGE_PRESENT | PAGE_HUGE;
  return EFI_SUCCESS;
}

static EFI_STATUS map_4k(uint64_t *pml4, uint64_t virt, uint64_t phys,
                         uint64_t flags) {
  uint64_t pml4_i = (virt >> 39) & 0x1ff;
  uint64_t pdpt_i = (virt >> 30) & 0x1ff;
  uint64_t pd_i = (virt >> 21) & 0x1ff;
  uint64_t pt_i = (virt >> 12) & 0x1ff;

  if (!(pml4[pml4_i] & PAGE_PRESENT)) {
    uint64_t *pdpt = new_table();
    if (!pdpt) return EFI_OUT_OF_RESOURCES;
    pml4[pml4_i] = (uint64_t)(uintptr_t)pdpt | PAGE_PRESENT | PAGE_WRITE;
  }
  uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[pml4_i] & ~0xfffULL);
  if (!(pdpt[pdpt_i] & PAGE_PRESENT)) {
    uint64_t *pd = new_table();
    if (!pd) return EFI_OUT_OF_RESOURCES;
    pdpt[pdpt_i] = (uint64_t)(uintptr_t)pd | PAGE_PRESENT | PAGE_WRITE;
  }
  uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_i] & ~0xfffULL);
  if (!(pd[pd_i] & PAGE_PRESENT) || (pd[pd_i] & PAGE_HUGE)) {
    uint64_t *pt = new_table();
    if (!pt) return EFI_OUT_OF_RESOURCES;
    pd[pd_i] = (uint64_t)(uintptr_t)pt | PAGE_PRESENT | PAGE_WRITE;
  }
  uint64_t *pt = (uint64_t *)(uintptr_t)(pd[pd_i] & ~0xfffULL);
  pt[pt_i] = (phys & ~0xfffULL) | flags | PAGE_PRESENT;
  return EFI_SUCCESS;
}

static EFI_STATUS map_range_4k(uint64_t *pml4, uint64_t virt, uint64_t phys,
                               uint64_t size, uint64_t flags) {
  uint64_t virt_start = virt & ~0xfffULL;
  uint64_t phys_start = phys & ~0xfffULL;
  uint64_t delta = virt - virt_start;
  uint64_t total = 0;
  uint64_t rounded = 0;

  if (u64_add_overflow(size, delta, &total) ||
      u64_add_overflow(total, 0xfffULL, &rounded))
    return EFI_LOAD_ERROR;
  total = rounded & ~0xfffULL;

  for (uint64_t off = 0; off < total; off += 0x1000) {
    EFI_STATUS status = map_4k(pml4, virt_start + off, phys_start + off, flags);
    if (EFI_ERROR(status)) return status;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS map_kernel_segments(uint64_t *pml4) {
  for (uint16_t i = 0; i < g_loaded_segment_count; i++) {
    EFI_STATUS status = map_range_4k(pml4, g_loaded_segments[i].virt,
                                     g_loaded_segments[i].phys,
                                     g_loaded_segments[i].size, PAGE_WRITE);
    if (EFI_ERROR(status)) return status;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS build_page_tables(const void *kernel,
                                    uint64_t framebuffer_base,
                                    uint64_t framebuffer_size,
                                    uint64_t boot_context_base,
                                    uint64_t boot_context_size,
                                    uint64_t handoff_base,
                                    uint64_t handoff_size,
                                    uint64_t *pml4_phys_out) {
  uint64_t pml4_phys = 0;
  EFI_STATUS status = alloc_zero_pages(1, &pml4_phys);
  if (EFI_ERROR(status)) return status;
  uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;

  for (uint64_t phys = 0; phys < 0x400000000ULL; phys += 0x200000) {
    status = map_2m(pml4, phys, phys, PAGE_WRITE);
    if (EFI_ERROR(status)) return status;
    status = map_2m(pml4, HHDM_OFFSET + phys, phys, PAGE_WRITE);
    if (EFI_ERROR(status)) return status;
  }
  (void)kernel;
  status = map_kernel_segments(pml4);
  if (EFI_ERROR(status)) return status;
  status = map_range_4k(pml4, framebuffer_base, framebuffer_base,
                        framebuffer_size, PAGE_WRITE);
  if (EFI_ERROR(status)) return status;
  status = map_range_4k(pml4, HHDM_OFFSET + framebuffer_base, framebuffer_base,
                        framebuffer_size, PAGE_WRITE);
  if (EFI_ERROR(status)) return status;
  status = map_range_4k(pml4, boot_context_base, boot_context_base,
                        boot_context_size, PAGE_WRITE);
  if (EFI_ERROR(status)) return status;
  status = map_range_4k(pml4, handoff_base, handoff_base, handoff_size,
                        PAGE_WRITE);
  if (EFI_ERROR(status)) return status;

  *pml4_phys_out = pml4_phys;
  return EFI_SUCCESS;
}

static int valid_elf64_kernel(const void *kernel, uint64_t size) {
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)kernel;
  uint64_t ph_size = 0;

  if (!kernel || size < sizeof(*eh)) return 0;
  if (eh->ident[0] != 0x7f || eh->ident[1] != 'E' ||
      eh->ident[2] != 'L' || eh->ident[3] != 'F' || eh->ident[4] != 2 ||
      eh->ident[5] != 1 || eh->machine != 0x3e ||
      eh->phentsize != sizeof(Elf64_Phdr) || eh->phnum == 0)
    return 0;

  if (u64_mul_overflow((uint64_t)eh->phnum, sizeof(Elf64_Phdr), &ph_size))
    return 0;
  return !u64_range_exceeds(eh->phoff, ph_size, size);
}

static int find_elf_symbol(const void *kernel, uint64_t size, const char *name,
                           uint64_t *value_out) {
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)kernel;
  uint64_t sh_size = 0;

  if (!kernel || !name || !value_out || eh->shentsize != sizeof(Elf64_Shdr))
    return 0;
  if (eh->shoff == 0 ||
      u64_mul_overflow((uint64_t)eh->shnum, sizeof(Elf64_Shdr), &sh_size) ||
      u64_range_exceeds(eh->shoff, sh_size, size))
    return 0;

  const Elf64_Shdr *sh =
      (const Elf64_Shdr *)((const uint8_t *)kernel + eh->shoff);
  for (uint16_t i = 0; i < eh->shnum; i++) {
    if (sh[i].type != 2 && sh[i].type != 11) continue;
    if (sh[i].entsize != sizeof(Elf64_Sym) || sh[i].link >= eh->shnum)
      continue;
    if (u64_range_exceeds(sh[i].offset, sh[i].size, size) ||
        u64_range_exceeds(sh[sh[i].link].offset, sh[sh[i].link].size, size))
      continue;

    const Elf64_Sym *sym =
        (const Elf64_Sym *)((const uint8_t *)kernel + sh[i].offset);
    const char *strings = (const char *)kernel + sh[sh[i].link].offset;
    uint64_t count = sh[i].size / sizeof(Elf64_Sym);
    for (uint64_t n = 0; n < count; n++) {
      if (sym[n].name >= sh[sh[i].link].size) continue;
      if (efi_streq(strings + sym[n].name, name)) {
        *value_out = sym[n].value;
        return 1;
      }
    }
  }
  return 0;
}

static int loaded_segments_contain_addr(uint64_t addr) {
  for (uint16_t i = 0; i < g_loaded_segment_count; i++) {
    uint64_t end = 0;

    if (u64_add_overflow(g_loaded_segments[i].virt, g_loaded_segments[i].size,
                         &end))
      continue;
    if (addr >= g_loaded_segments[i].virt && addr < end)
      return 1;
  }

  return 0;
}

static EFI_STATUS load_elf_segments(const void *kernel, uint64_t size, uint64_t *entry_out) {
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)kernel;
  const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)kernel + eh->phoff);
  uint64_t lowest_virt = UINT64_MAX;
  g_loaded_segment_count = 0;
  g_kernel_physical_base = 0;

  for (uint16_t i = 0; i < eh->phnum; i++) {
    if (ph[i].type != 1 || ph[i].memsz == 0) continue;
    if (u64_range_exceeds(ph[i].offset, ph[i].filesz, size) ||
        ph[i].filesz > ph[i].memsz) {
      return EFI_LOAD_ERROR;
    }
    if (g_loaded_segment_count >= sizeof(g_loaded_segments) / sizeof(g_loaded_segments[0]))
      return EFI_OUT_OF_RESOURCES;

    uint64_t virt_start = ph[i].vaddr & ~0xfffULL;
    uint64_t delta = ph[i].vaddr - virt_start;
    uint64_t total = 0;
    uint64_t rounded = 0;
    uint64_t phys_start = 0;

    if (u64_add_overflow(ph[i].memsz, delta, &total) ||
        u64_add_overflow(total, 0xfffULL, &rounded))
      return EFI_LOAD_ERROR;
    total = rounded & ~0xfffULL;

    EFI_STATUS status = alloc_zero_pages(total / 4096, &phys_start);
    if (EFI_ERROR(status)) return status;

    efi_memset((void *)(uintptr_t)(phys_start + delta), 0, ph[i].memsz);
    efi_memcpy((void *)(uintptr_t)(phys_start + delta),
               (const uint8_t *)kernel + ph[i].offset, ph[i].filesz);

    g_loaded_segments[g_loaded_segment_count].virt = virt_start;
    g_loaded_segments[g_loaded_segment_count].phys = phys_start;
    g_loaded_segments[g_loaded_segment_count].size = total;
    g_loaded_segment_count++;

    if (virt_start < lowest_virt) {
      lowest_virt = virt_start;
      g_kernel_physical_base = phys_start;
    }
  }
  if (g_loaded_segment_count == 0 || g_kernel_physical_base == 0)
    return EFI_LOAD_ERROR;
  if (*entry_out == 0)
    *entry_out = eh->entry;
  if (!loaded_segments_contain_addr(*entry_out))
    return EFI_LOAD_ERROR;
  return EFI_SUCCESS;
}

static int valid_xnu_macho64_kernel(const void *kernel, uint64_t size,
                                    os8_xnu_macho64_image_t *macho_out) {
  return os8_xnu_macho64_inspect(kernel, size, OS8_XNU_ARCH_X86_64,
                                 macho_out) == 0;
}

static EFI_STATUS load_xnu_macho64_segments(
    const void *kernel, uint64_t size, const os8_xnu_macho64_image_t *macho,
    uint64_t *entry_out) {
  g_loaded_segment_count = 0;
  g_kernel_physical_base = 0;

  if (!macho || !entry_out || !macho->entry_vmaddr) return EFI_LOAD_ERROR;
  for (uint32_t i = 0; i < macho->segment_count; i++) {
    os8_xnu_macho64_segment_t segment;
    uint64_t virt_start;
    uint64_t delta;
    uint64_t total;
    uint64_t rounded;
    uint64_t phys_start;
    EFI_STATUS status;

    if (g_loaded_segment_count >=
        sizeof(g_loaded_segments) / sizeof(g_loaded_segments[0])) {
      return EFI_OUT_OF_RESOURCES;
    }
    if (os8_xnu_macho64_segment_at(kernel, size, OS8_XNU_ARCH_X86_64, i,
                                   &segment) != 0) {
      return EFI_LOAD_ERROR;
    }
    virt_start = segment.vmaddr & ~0xfffULL;
    delta = segment.vmaddr - virt_start;
    if (u64_add_overflow(segment.vmsize, delta, &total) ||
        u64_add_overflow(total, 0xfffULL, &rounded)) {
      return EFI_LOAD_ERROR;
    }
    total = rounded & ~0xfffULL;
    status = alloc_zero_pages_below(total / 4096,
                                    OS8_XNU_BOOT_ARGS_MAX_ADDRESS,
                                    &phys_start);
    if (EFI_ERROR(status)) return status;

    efi_memset((void *)(uintptr_t)(phys_start + delta), 0, segment.vmsize);
    efi_memcpy((void *)(uintptr_t)(phys_start + delta),
               (const uint8_t *)kernel + segment.fileoff, segment.filesize);
    g_loaded_segments[g_loaded_segment_count].virt = virt_start;
    g_loaded_segments[g_loaded_segment_count].phys = phys_start;
    g_loaded_segments[g_loaded_segment_count].size = total;
    g_loaded_segment_count++;
    if (segment.vmaddr == macho->lowest_vmaddr)
      g_kernel_physical_base = phys_start + delta;
  }

  if (g_loaded_segment_count == 0 || g_kernel_physical_base == 0)
    return EFI_LOAD_ERROR;
  *entry_out = macho->entry_vmaddr;
  if (!loaded_segments_contain_addr(*entry_out)) return EFI_LOAD_ERROR;
  return EFI_SUCCESS;
}

static void *find_acpi_rsdp(void) {
  EFI_GUID acpi2 = ACPI_20_TABLE_GUID;
  EFI_GUID acpi1 = ACPI_TABLE_GUID;
  EFI_CONFIGURATION_TABLE *tables = (EFI_CONFIGURATION_TABLE *)g_st->ConfigurationTable;
  void *fallback = NULL;
  for (uint64_t i = 0; i < g_st->NumberOfTableEntries; i++) {
    if (efi_guid_eq(&tables[i].VendorGuid, &acpi2)) return tables[i].VendorTable;
    if (efi_guid_eq(&tables[i].VendorGuid, &acpi1)) fallback = tables[i].VendorTable;
  }
  return fallback;
}

static EFI_STATUS allocate_limine_responses(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                            void *rsdp,
                                            uint64_t kernel_phys_base,
                                            void **fb_resp_out,
                                            void **hhdm_resp_out,
                                            void **addr_resp_out,
                                            void **rsdp_resp_out,
                                            void **boot_context_out,
                                            uint64_t *boot_context_size_out) {
  void *pool = NULL;
  const uint64_t pool_size = 1024;
  const uint64_t required_size =
      sizeof(struct limine_framebuffer) +
      sizeof(struct limine_framebuffer *) +
      sizeof(struct limine_framebuffer_response) +
      sizeof(struct limine_hhdm_response) +
      sizeof(struct limine_kernel_address_response) +
      sizeof(struct limine_rsdp_response);
  EFI_STATUS status;

  if (required_size > pool_size) return EFI_OUT_OF_RESOURCES;
  status = g_st->BootServices->AllocatePool(EfiLoaderData, pool_size, &pool);
  if (EFI_ERROR(status)) return status;
  efi_memset(pool, 0, pool_size);

  uint8_t *p = (uint8_t *)pool;
  struct limine_framebuffer *fb = (struct limine_framebuffer *)p;
  p += sizeof(*fb);
  struct limine_framebuffer **fb_array = (struct limine_framebuffer **)p;
  p += sizeof(*fb_array);
  struct limine_framebuffer_response *fb_resp = (struct limine_framebuffer_response *)p;
  p += sizeof(*fb_resp);
  struct limine_hhdm_response *hhdm_resp = (struct limine_hhdm_response *)p;
  p += sizeof(*hhdm_resp);
  struct limine_kernel_address_response *addr_resp = (struct limine_kernel_address_response *)p;
  p += sizeof(*addr_resp);
  struct limine_rsdp_response *rsdp_resp = (struct limine_rsdp_response *)p;

  fb->address = (void *)(uintptr_t)gop->Mode->FrameBufferBase;
  fb->width = gop->Mode->Info->HorizontalResolution;
  fb->height = gop->Mode->Info->VerticalResolution;
  fb->pitch = (uint64_t)gop->Mode->Info->PixelsPerScanLine * 4;
  fb->bpp = 32;
  fb->memory_model = 1;
  if (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
    fb->red_mask_shift = 16;
    fb->green_mask_shift = 8;
    fb->blue_mask_shift = 0;
  } else {
    fb->red_mask_shift = 0;
    fb->green_mask_shift = 8;
    fb->blue_mask_shift = 16;
  }
  fb->red_mask_size = 8;
  fb->green_mask_size = 8;
  fb->blue_mask_size = 8;
  *fb_array = fb;
  fb_resp->framebuffer_count = 1;
  fb_resp->framebuffers = fb_array;

  hhdm_resp->offset = HHDM_OFFSET;
  addr_resp->physical_base = kernel_phys_base;
  addr_resp->virtual_base = KERNEL_VIRT_BASE;
  rsdp_resp->address = rsdp;

  *fb_resp_out = fb_resp;
  *hhdm_resp_out = hhdm_resp;
  *addr_resp_out = addr_resp;
  *rsdp_resp_out = rsdp_resp;
  *boot_context_out = pool;
  *boot_context_size_out = pool_size;
  return EFI_SUCCESS;
}

static EFI_STATUS allocate_os8_handoff(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                       void *rsdp, const void *kernel_file,
                                       uint64_t kernel_file_size,
                                       OS8_BOOT_HANDOFF **handoff_out) {
  static const char cmdline[] = "boot=uefi loader=os8-custom";
  OS8_BOOT_HANDOFF *handoff = NULL;
  EFI_STATUS status = g_st->BootServices->AllocatePool(
      EfiLoaderData, sizeof(*handoff), (void **)&handoff);
  if (EFI_ERROR(status)) return status;

  efi_memset(handoff, 0, sizeof(*handoff));
  handoff->magic = OS8_BOOT_HANDOFF_MAGIC;
  handoff->version = OS8_BOOT_HANDOFF_VERSION;
  handoff->framebuffer_addr = gop->Mode->FrameBufferBase;
  handoff->framebuffer_width = gop->Mode->Info->HorizontalResolution;
  handoff->framebuffer_height = gop->Mode->Info->VerticalResolution;
  handoff->framebuffer_pitch = (uint64_t)gop->Mode->Info->PixelsPerScanLine * 4;
  handoff->rsdp_addr = (uint64_t)(uintptr_t)rsdp;
  handoff->hhdm_offset = HHDM_OFFSET;
  handoff->bootstrap_file_addr = (uint64_t)(uintptr_t)kernel_file;
  handoff->bootstrap_file_size = kernel_file_size;
  handoff->kernel_file_addr = (uint64_t)(uintptr_t)kernel_file;
  handoff->kernel_file_size = kernel_file_size;
  handoff->cmdline_addr = (uint64_t)(uintptr_t)cmdline;
  *handoff_out = handoff;
  return EFI_SUCCESS;
}

static void patch_limine_requests_in_range(uint64_t scan_start, uint64_t scan_size,
                                  void *fb_resp, void *hhdm_resp,
                                  void *addr_resp, void *rsdp_resp) {
  const uint64_t common0 = 0xc7b1dd30df4c8b88ULL;
  const uint64_t common1 = 0x0a82e883a194f07bULL;
  const uint64_t base0 = 0xf9562b2d5c95a6c8ULL;
  const uint64_t base1 = 0x6a7b384944536bdcULL;
  for (uint64_t off = 0; off + 48 <= scan_size; off += 8) {
    uint64_t *q = (uint64_t *)(uintptr_t)(scan_start + off);
    if (q[0] == base0 && q[1] == base1) q[2] = 0;
    if (q[0] != common0 || q[1] != common1) continue;
    if (q[2] == 0x9d5827dcd881dd75ULL && q[3] == 0xa3148604f6fab11bULL) q[5] = (uint64_t)(uintptr_t)fb_resp;
    if (q[2] == 0x48dcf1cb8ad2b852ULL && q[3] == 0x63984e959a98244bULL) q[5] = (uint64_t)(uintptr_t)hhdm_resp;
    if (q[2] == 0x71ba76863cc55f63ULL && q[3] == 0xb2644a48c516a487ULL) q[5] = (uint64_t)(uintptr_t)addr_resp;
    if (q[2] == 0xc5e77b6b397e7b43ULL && q[3] == 0x27637845accdcf3cULL) q[5] = (uint64_t)(uintptr_t)rsdp_resp;
  }
}

static void patch_limine_requests(void *fb_resp, void *hhdm_resp,
                                  void *addr_resp, void *rsdp_resp) {
  for (uint16_t i = 0; i < g_loaded_segment_count; i++) {
    patch_limine_requests_in_range(g_loaded_segments[i].phys,
                                   g_loaded_segments[i].size, fb_resp,
                                   hhdm_resp, addr_resp, rsdp_resp);
  }
}

typedef EFI_STATUS (*pre_exit_boot_services_fn)(void *context,
                                                uint64_t memory_map_base,
                                                uint64_t memory_map_size,
                                                uint64_t desc_size,
                                                uint32_t desc_version);

typedef struct {
  os8_xnu_x86_64_boot_args_t *boot_args;
  uint64_t boot_args_phys;
  uint64_t kernel_base;
  uint64_t kernel_size;
  uint64_t efi_system_table;
  const char *command_line;
  uint64_t command_line_size;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
} XNU_PRE_EXIT_BOOT_ARGS;

static EFI_STATUS build_xnu_boot_args_pre_exit(void *context,
                                               uint64_t memory_map_base,
                                               uint64_t memory_map_size,
                                               uint64_t desc_size,
                                               uint32_t desc_version) {
  XNU_PRE_EXIT_BOOT_ARGS *xnu = (XNU_PRE_EXIT_BOOT_ARGS *)context;
  os8_xnu_x86_64_boot_args_input_t input;

  if (!xnu || !xnu->boot_args || !xnu->gop || !xnu->gop->Mode ||
      !xnu->gop->Mode->Info) {
    return EFI_INVALID_PARAMETER;
  }

  efi_memset(&input, 0, sizeof(input));
  input.boot_args_base = xnu->boot_args_phys;
  input.memory_map_base = memory_map_base;
  input.memory_map_size = memory_map_size;
  input.memory_map_descriptor_size = desc_size;
  input.memory_map_descriptor_version = desc_version;
  input.kernel_base = xnu->kernel_base;
  input.kernel_size = xnu->kernel_size;
  input.efi_system_table = xnu->efi_system_table;
  input.command_line = xnu->command_line;
  input.command_line_size = xnu->command_line_size;
  input.framebuffer.base = xnu->gop->Mode->FrameBufferBase;
  input.framebuffer.size = xnu->gop->Mode->FrameBufferSize;
  input.framebuffer.width = xnu->gop->Mode->Info->HorizontalResolution;
  input.framebuffer.height = xnu->gop->Mode->Info->VerticalResolution;
  input.framebuffer.pitch =
      (uint64_t)xnu->gop->Mode->Info->PixelsPerScanLine * 4;
  input.framebuffer.pixel_format =
      (uint32_t)xnu->gop->Mode->Info->PixelFormat;

  if (os8_xnu_x86_64_boot_args_build(xnu->boot_args, &input) != 0) {
    return EFI_LOAD_ERROR;
  }

  return EFI_SUCCESS;
}

static EFI_STATUS exit_boot_services_with_map(uint64_t *map_base_out,
                                              uint64_t *map_size_out,
                                              uint64_t *desc_size_out,
                                              uint32_t *desc_version_out,
                                              pre_exit_boot_services_fn pre_exit,
                                              void *pre_exit_context) {
  void *map = NULL;
  uint64_t map_capacity = 0;
  uint64_t map_base = 0;
  uint64_t map_allocation_size = 0;
  uint64_t desc_size = 0;
  uint32_t desc_version = 0;

  for (int attempt = 0; attempt < 8; attempt++) {
    uint64_t map_size = map_capacity;
    uint64_t map_key = 0;
    EFI_STATUS status;

    if (!map) {
      map_size = 0;
      status = g_st->BootServices->GetMemoryMap(&map_size, NULL, &map_key,
                                                &desc_size, &desc_version);
      if (EFI_STATUS_CODE(status) != EFI_BUFFER_TOO_SMALL) return status;
      if (desc_size == 0) return EFI_LOAD_ERROR;
      if (u64_mul_overflow(desc_size, 32, &map_capacity) ||
          u64_add_overflow(map_size, map_capacity, &map_capacity))
        return EFI_OUT_OF_RESOURCES;
      status = alloc_zero_bytes_below(map_capacity, OS8_XNU_BOOT_ARGS_MAX_ADDRESS,
                                      &map_base, &map_allocation_size);
      if (EFI_ERROR(status)) return status;
      map = (void *)(uintptr_t)map_base;
      map_capacity = map_allocation_size;
    }

    map_size = map_capacity;
    status = g_st->BootServices->GetMemoryMap(&map_size, map, &map_key,
                                              &desc_size, &desc_version);
    if (EFI_STATUS_CODE(status) == EFI_BUFFER_TOO_SMALL) {
      uint64_t extra = 0;
      if (desc_size == 0) return EFI_LOAD_ERROR;
      if (u64_mul_overflow(desc_size, 32, &extra) ||
          u64_add_overflow(map_size, extra, &map_capacity))
        return EFI_OUT_OF_RESOURCES;
      g_st->BootServices->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)map,
                                    map_allocation_size >> 12);
      map = NULL;
      map_base = 0;
      map_allocation_size = 0;
      continue;
    }
    if (EFI_ERROR(status)) return status;

    if (pre_exit) {
      status = pre_exit(pre_exit_context, (uint64_t)(uintptr_t)map, map_size,
                        desc_size, desc_version);
      if (EFI_ERROR(status)) return status;
    }

    status = g_st->BootServices->ExitBootServices(g_image, map_key);
    if (!EFI_ERROR(status)) {
      if (map_base_out) *map_base_out = (uint64_t)(uintptr_t)map;
      if (map_size_out) *map_size_out = map_size;
      if (desc_size_out) *desc_size_out = desc_size;
      if (desc_version_out) *desc_version_out = desc_version;
      return EFI_SUCCESS;
    }
  }

  return EFI_LOAD_ERROR;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
  EFI_STATUS status;
  void *cfg_data = NULL;
  uint64_t cfg_size = 0;
  void *kernel = NULL;
  uint64_t kernel_size = 0;
  char kernel_hash[80];
  char kernel_path_ascii[128];
  char kernel_format[16];
  static CHAR16 kernel_path[128] = L"\\boot\\main.sys";
  EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
  void *fb_resp = NULL, *hhdm_resp = NULL, *addr_resp = NULL, *rsdp_resp = NULL;
  void *boot_context = NULL;
  uint64_t boot_context_size = 0;
  uint64_t entry = 0;
  uint64_t pml4_phys = 0;
  OS8_BOOT_HANDOFF *handoff = NULL;
  os8_xnu_x86_64_boot_args_t *xnu_boot_args = NULL;
  uint64_t xnu_boot_args_phys = 0;
  uint64_t xnu_kernel_size = 0;
  void *rsdp = NULL;
  int xnu_mode = 0;

  g_st = st;
  g_image = image;
  efi_print(st, "OS8 Startup Executable\n");

  status = efi_read_file(image, st, L"\\EFI\\OS8\\os8boot.cfg", &cfg_data, &cfg_size);
  if (EFI_ERROR(status)) return error("CONFIG-0002", "No valid boot configuration is available.", status);
  (void)cfg_size;

  if (cfg_get((const char *)cfg_data, "kernel_path", kernel_path_ascii, sizeof(kernel_path_ascii))) {
    uint64_t i = 0;
    while (kernel_path_ascii[i] && i + 1 < sizeof(kernel_path) / sizeof(kernel_path[0])) {
      kernel_path[i] = (CHAR16)kernel_path_ascii[i];
      i++;
    }
    kernel_path[i] = 0;
  }
  xnu_mode = cfg_get((const char *)cfg_data, "kernel_format", kernel_format,
                     sizeof(kernel_format)) &&
             efi_streq(kernel_format, "xnu");

  status = efi_read_file(image, st, kernel_path, &kernel, &kernel_size);
  if (EFI_ERROR(status)) return error("KERNEL-0001", "The kernel could not be loaded.", status);

  if (!cfg_get((const char *)cfg_data, "kernel_sha256", kernel_hash, sizeof(kernel_hash)) ||
      !verify_buffer(kernel, kernel_size, kernel_hash)) {
    return error("KERNEL-0002", "The kernel is not trusted.", EFI_SECURITY_VIOLATION);
  }
  status = st->BootServices->LocateProtocol(&gop_guid, NULL, (void **)&gop);
  if (EFI_ERROR(status)) return error("KERNEL-0004", "Graphics output protocol is unavailable.", status);
  if (!gop_framebuffer_is_sane(gop))
    return error("KERNEL-0004", "Graphics output mode is invalid.", EFI_UNSUPPORTED);

  rsdp = find_acpi_rsdp();

  if (xnu_mode) {
    os8_xnu_macho64_image_t macho;
    uint64_t boot_args_allocation_size = 0;
    if (!valid_xnu_macho64_kernel(kernel, kernel_size, &macho)) {
      return error("KERNEL-0003", "The verified XNU kernel is not a supported x86_64 Mach-O image.", EFI_LOAD_ERROR);
    }
    xnu_kernel_size = macho.highest_vmaddr - macho.lowest_vmaddr;
    status = load_xnu_macho64_segments(kernel, kernel_size, &macho, &entry);
    if (EFI_ERROR(status)) return error("KERNEL-0005", "The verified XNU kernel could not be mapped.", status);
    if (xnu_kernel_size > 0xffffffffULL ||
        g_kernel_physical_base > 0xffffffffULL ||
        (uint64_t)(uintptr_t)st > 0xffffffffULL ||
        gop->Mode->FrameBufferBase > 0xffffffffULL) {
      return error("KERNEL-0005", "The XNU handoff requires boot inputs below 4G.", EFI_LOAD_ERROR);
    }
    status = alloc_zero_bytes_below(sizeof(*xnu_boot_args),
                                    OS8_XNU_BOOT_ARGS_MAX_ADDRESS,
                                    &xnu_boot_args_phys,
                                    &boot_args_allocation_size);
    if (EFI_ERROR(status)) return error("KERNEL-0006", "XNU boot args allocation failed.", status);
    (void)boot_args_allocation_size;
    xnu_boot_args = (os8_xnu_x86_64_boot_args_t *)(uintptr_t)xnu_boot_args_phys;
  } else {
    if (!valid_elf64_kernel(kernel, kernel_size)) {
      return error("KERNEL-0003", "The verified kernel is not a supported x86_64 ELF image.", EFI_LOAD_ERROR);
    }
    if (!find_elf_symbol(kernel, kernel_size, "_start_from_loader", &entry)) {
      return error("KERNEL-0005", "The custom kernel entry point is missing.", EFI_LOAD_ERROR);
    }
    status = load_elf_segments(kernel, kernel_size, &entry);
    if (EFI_ERROR(status)) return error("KERNEL-0005", "The verified kernel could not be mapped.", status);

    status = allocate_limine_responses(gop, rsdp,
                                       g_kernel_physical_base, &fb_resp,
                                       &hhdm_resp, &addr_resp, &rsdp_resp,
                                       &boot_context, &boot_context_size);
    if (EFI_ERROR(status)) return error("KERNEL-0006", "Boot context allocation failed.", status);
    patch_limine_requests(fb_resp, hhdm_resp, addr_resp, rsdp_resp);
    status = allocate_os8_handoff(gop, rsdp, kernel, kernel_size, &handoff);
    if (EFI_ERROR(status)) return error("KERNEL-0006", "OS8 handoff allocation failed.", status);
  }

  status = build_page_tables(kernel, gop->Mode->FrameBufferBase,
                             gop->Mode->FrameBufferSize,
                             (uint64_t)(uintptr_t)boot_context,
                             boot_context_size,
                             xnu_mode ? xnu_boot_args_phys
                                      : (uint64_t)(uintptr_t)handoff,
                             xnu_mode ? sizeof(*xnu_boot_args)
                                      : sizeof(*handoff),
                             &pml4_phys);
  if (EFI_ERROR(status)) return error("KERNEL-0007", "Kernel page table creation failed.", status);

  efi_print(st, "Kernel verified and loaded. Exiting boot services...\n");
  if (xnu_mode) {
    uint64_t memory_map_base = 0;
    uint64_t memory_map_size = 0;
    uint64_t descriptor_size = 0;
    uint32_t descriptor_version = 0;
    static const char xnu_command_line[] = "debug=0x144 keepsyms=1";
    XNU_PRE_EXIT_BOOT_ARGS xnu_pre_exit;

    efi_memset(&xnu_pre_exit, 0, sizeof(xnu_pre_exit));
    xnu_pre_exit.boot_args = xnu_boot_args;
    xnu_pre_exit.boot_args_phys = xnu_boot_args_phys;
    xnu_pre_exit.kernel_base = g_kernel_physical_base;
    xnu_pre_exit.kernel_size = xnu_kernel_size;
    xnu_pre_exit.efi_system_table = (uint64_t)(uintptr_t)st;
    xnu_pre_exit.command_line = xnu_command_line;
    xnu_pre_exit.command_line_size = sizeof(xnu_command_line) - 1;
    xnu_pre_exit.gop = gop;

    status = exit_boot_services_with_map(&memory_map_base, &memory_map_size,
                                         &descriptor_size,
                                         &descriptor_version,
                                         build_xnu_boot_args_pre_exit,
                                         &xnu_pre_exit);
    if (EFI_ERROR(status)) return error("KERNEL-0008", "XNU boot args or ExitBootServices failed.", status);
    (void)memory_map_base;
    (void)memory_map_size;
    (void)descriptor_size;
    (void)descriptor_version;
    startup_enter_xnu_kernel(pml4_phys, entry, xnu_boot_args_phys);
    return EFI_SUCCESS;
  } else {
    status = exit_boot_services_with_map(NULL, NULL, NULL, NULL, NULL, NULL);
  }
  if (EFI_ERROR(status)) return error("KERNEL-0008", "ExitBootServices failed.", status);

  startup_enter_kernel(pml4_phys, entry, handoff);
  return EFI_SUCCESS;
}
