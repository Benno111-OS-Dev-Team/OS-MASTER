#include "uefi.h"

#define HHDM_OFFSET 0xffff800000000000ULL
#define KERNEL_VIRT_BASE 0xffffffff80000000ULL
#define PAGE_PRESENT 0x001ULL
#define PAGE_WRITE 0x002ULL
#define PAGE_HUGE 0x080ULL
#define OS8_BOOT_HANDOFF_MAGIC 0x4F5338424F4F5448ULL
#define OS8_BOOT_HANDOFF_VERSION 1

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
  uint64_t total = (size + delta + 0xfffULL) & ~0xfffULL;

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
  return size >= sizeof(*eh) && eh->ident[0] == 0x7f && eh->ident[1] == 'E' &&
         eh->ident[2] == 'L' && eh->ident[3] == 'F' && eh->ident[4] == 2 &&
         eh->ident[5] == 1 && eh->machine == 0x3e && eh->phentsize == sizeof(Elf64_Phdr) &&
         eh->phoff + ((uint64_t)eh->phnum * sizeof(Elf64_Phdr)) <= size;
}

static int find_elf_symbol(const void *kernel, uint64_t size, const char *name,
                           uint64_t *value_out) {
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)kernel;

  if (!kernel || !name || !value_out || eh->shentsize != sizeof(Elf64_Shdr))
    return 0;
  if (eh->shoff == 0 ||
      eh->shoff + (uint64_t)eh->shnum * sizeof(Elf64_Shdr) > size)
    return 0;

  const Elf64_Shdr *sh =
      (const Elf64_Shdr *)((const uint8_t *)kernel + eh->shoff);
  for (uint16_t i = 0; i < eh->shnum; i++) {
    if (sh[i].type != 2 && sh[i].type != 11) continue;
    if (sh[i].entsize != sizeof(Elf64_Sym) || sh[i].link >= eh->shnum)
      continue;
    if (sh[i].offset + sh[i].size > size ||
        sh[sh[i].link].offset + sh[sh[i].link].size > size)
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

static EFI_STATUS load_elf_segments(const void *kernel, uint64_t size, uint64_t *entry_out) {
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)kernel;
  const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)kernel + eh->phoff);
  uint64_t lowest_virt = UINT64_MAX;
  g_loaded_segment_count = 0;
  g_kernel_physical_base = 0;

  for (uint16_t i = 0; i < eh->phnum; i++) {
    if (ph[i].type != 1 || ph[i].memsz == 0) continue;
    if (ph[i].offset + ph[i].filesz > size || ph[i].filesz > ph[i].memsz) {
      return EFI_LOAD_ERROR;
    }
    if (g_loaded_segment_count >= sizeof(g_loaded_segments) / sizeof(g_loaded_segments[0]))
      return EFI_OUT_OF_RESOURCES;

    uint64_t virt_start = ph[i].vaddr & ~0xfffULL;
    uint64_t delta = ph[i].vaddr - virt_start;
    uint64_t total = (ph[i].memsz + delta + 0xfffULL) & ~0xfffULL;
    uint64_t phys_start = 0;
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
  EFI_STATUS status = g_st->BootServices->AllocatePool(EfiLoaderData, pool_size, &pool);
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

static EFI_STATUS exit_boot_services_with_map(void) {
  void *map = NULL;
  uint64_t map_capacity = 0;
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
      map_capacity = map_size + desc_size * 32;
      status = g_st->BootServices->AllocatePool(EfiLoaderData, map_capacity,
                                                &map);
      if (EFI_ERROR(status)) return status;
    }

    map_size = map_capacity;
    status = g_st->BootServices->GetMemoryMap(&map_size, map, &map_key,
                                              &desc_size, &desc_version);
    if (EFI_STATUS_CODE(status) == EFI_BUFFER_TOO_SMALL) {
      map_capacity = map_size + desc_size * 32;
      map = NULL;
      continue;
    }
    if (EFI_ERROR(status)) return status;

    status = g_st->BootServices->ExitBootServices(g_image, map_key);
    if (!EFI_ERROR(status)) return EFI_SUCCESS;
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
  static CHAR16 kernel_path[128] = L"\\boot\\main.sys";
  EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
  void *fb_resp = NULL, *hhdm_resp = NULL, *addr_resp = NULL, *rsdp_resp = NULL;
  void *boot_context = NULL;
  uint64_t boot_context_size = 0;
  uint64_t entry = 0;
  uint64_t pml4_phys = 0;
  OS8_BOOT_HANDOFF *handoff = NULL;
  void *rsdp = NULL;

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

  status = efi_read_file(image, st, kernel_path, &kernel, &kernel_size);
  if (EFI_ERROR(status)) return error("KERNEL-0001", "The kernel could not be loaded.", status);

  if (!cfg_get((const char *)cfg_data, "kernel_sha256", kernel_hash, sizeof(kernel_hash)) ||
      !verify_buffer(kernel, kernel_size, kernel_hash)) {
    return error("KERNEL-0002", "The kernel is not trusted.", EFI_SECURITY_VIOLATION);
  }
  if (!valid_elf64_kernel(kernel, kernel_size)) {
    return error("KERNEL-0003", "The verified kernel is not a supported x86_64 ELF image.", EFI_LOAD_ERROR);
  }

  status = st->BootServices->LocateProtocol(&gop_guid, NULL, (void **)&gop);
  if (EFI_ERROR(status)) return error("KERNEL-0004", "Graphics output protocol is unavailable.", status);

  if (!find_elf_symbol(kernel, kernel_size, "_start_from_loader", &entry)) {
    return error("KERNEL-0005", "The custom kernel entry point is missing.", EFI_LOAD_ERROR);
  }

  status = load_elf_segments(kernel, kernel_size, &entry);
  if (EFI_ERROR(status)) return error("KERNEL-0005", "The verified kernel could not be mapped.", status);

  rsdp = find_acpi_rsdp();
  status = allocate_limine_responses(gop, rsdp,
                                     g_kernel_physical_base, &fb_resp,
                                     &hhdm_resp, &addr_resp, &rsdp_resp,
                                     &boot_context, &boot_context_size);
  if (EFI_ERROR(status)) return error("KERNEL-0006", "Boot context allocation failed.", status);
  patch_limine_requests(fb_resp, hhdm_resp, addr_resp, rsdp_resp);
  status = allocate_os8_handoff(gop, rsdp, kernel, kernel_size, &handoff);
  if (EFI_ERROR(status)) return error("KERNEL-0006", "OS8 handoff allocation failed.", status);

  status = build_page_tables(kernel, gop->Mode->FrameBufferBase,
                             gop->Mode->FrameBufferSize,
                             (uint64_t)(uintptr_t)boot_context,
                             boot_context_size, (uint64_t)(uintptr_t)handoff,
                             sizeof(*handoff), &pml4_phys);
  if (EFI_ERROR(status)) return error("KERNEL-0007", "Kernel page table creation failed.", status);

  efi_print(st, "Kernel verified and loaded. Exiting boot services...\n");
  status = exit_boot_services_with_map();
  if (EFI_ERROR(status)) return error("KERNEL-0008", "ExitBootServices failed.", status);

  startup_enter_kernel(pml4_phys, entry, handoff);
  return EFI_SUCCESS;
}
