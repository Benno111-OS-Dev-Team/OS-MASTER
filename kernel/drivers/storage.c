#include "drivers/storage.h"
#include "arch/arch.h"
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "printk.h"

#define STORAGE_MAX_CONTROLLERS 16
#define STORAGE_MAX_DISKS 16
#define STORAGE_MAX_PARTITIONS 8

typedef struct {
  storage_kind_t kind;
  uint16_t vendor_id;
  uint16_t device_id;
  uint8_t bus;
  uint8_t slot;
  uint8_t func;
  char name[48];
  char bus_name[16];
} storage_controller_t;

typedef struct {
  storage_kind_t kind;
  uint8_t controller_index;
  uint8_t disk_index;
  uint32_t capacity_mib;
  storage_disk_read_fn_t read_fn;
  storage_disk_write_fn_t write_fn;
  void *backend_ctx;
  char name[48];
  char location[24];
} storage_disk_t;

typedef struct {
  int present;
  storage_partition_kind_t kind;
  uint32_t size_mib;
  uint32_t start_lba;
  uint32_t sector_count;
  char label[32];
} storage_partition_t;

static storage_controller_t storage_controllers[STORAGE_MAX_CONTROLLERS];
static storage_disk_t storage_disks[STORAGE_MAX_DISKS];
static storage_partition_t storage_partitions[STORAGE_MAX_DISKS]
                                            [STORAGE_MAX_PARTITIONS];
static int storage_controller_count = 0;
static int storage_disk_count = 0;
static int storage_kind_counts[STORAGE_KIND_APPLE_ANS + 1];
static int storage_initialized = 0;

static void storage_load_mbr_partitions(int disk_index);

#define STORAGE_IO_TIMEOUT_MS 1000
#define AHCI_SECTOR_SIZE 512
#define NVME_ADMIN_Q_DEPTH 16
#define NVME_IO_Q_DEPTH 16
#define NVME_PAGE_SIZE 4096

#define STORAGE_SECTOR_SIZE 512
#define STORAGE_MBR_PARTITION_OFFSET 446
#define STORAGE_MBR_SIGNATURE_OFFSET 510

typedef struct {
  volatile uint32_t *abar;
  volatile uint8_t *port_mmio;
  uint32_t sector_count;
  int port_no;
  int active;
  uint8_t command_list[1024] __attribute__((aligned(1024)));
  uint8_t rfis[256] __attribute__((aligned(256)));
  uint8_t command_table[256] __attribute__((aligned(128)));
  uint16_t identify_data[256] __attribute__((aligned(2)));
} storage_ahci_port_ctx_t;

typedef struct {
  volatile uint32_t *regs;
  uint32_t nsid;
  uint32_t sector_size;
  uint64_t sector_count;
  uint16_t admin_sq_tail;
  uint16_t admin_cq_head;
  uint8_t admin_phase;
  uint16_t io_sq_tail;
  uint16_t io_cq_head;
  uint8_t io_phase;
  int active;
  uint8_t admin_sq[NVME_PAGE_SIZE] __attribute__((aligned(NVME_PAGE_SIZE)));
  uint8_t admin_cq[NVME_PAGE_SIZE] __attribute__((aligned(NVME_PAGE_SIZE)));
  uint8_t io_sq[NVME_PAGE_SIZE] __attribute__((aligned(NVME_PAGE_SIZE)));
  uint8_t io_cq[NVME_PAGE_SIZE] __attribute__((aligned(NVME_PAGE_SIZE)));
  uint8_t identify_data[NVME_PAGE_SIZE] __attribute__((aligned(NVME_PAGE_SIZE)));
  uint8_t io_buffer[NVME_PAGE_SIZE] __attribute__((aligned(NVME_PAGE_SIZE)));
} storage_nvme_ctx_t;

typedef struct {
  uint16_t io_base;
  uint8_t drive_select;
  int active;
} storage_ide_atapi_ctx_t;

static storage_ahci_port_ctx_t storage_ahci_ports[STORAGE_MAX_DISKS];
static storage_nvme_ctx_t storage_nvme_contexts[STORAGE_MAX_DISKS];
static storage_ide_atapi_ctx_t storage_ide_atapi_contexts[4];

#if defined(ARCH_X86_64)
#define STORAGE_X86_64_KERNEL_VIRT_BASE 0xFFFFFFFF80000000ULL
#define STORAGE_X86_64_KERNEL_PHYS_BASE 0x100000ULL
extern uint64_t limine_get_hhdm_offset(void);
#endif

static phys_addr_t storage_dma_addr(const void *ptr) {
  uintptr_t addr;
  phys_addr_t paddr;

  if (!ptr)
    return 0;

  addr = (uintptr_t)ptr;

#if defined(ARCH_X86_64)
  if (addr >= STORAGE_X86_64_KERNEL_VIRT_BASE)
    return STORAGE_X86_64_KERNEL_PHYS_BASE +
           (phys_addr_t)(addr - STORAGE_X86_64_KERNEL_VIRT_BASE);

  {
    uint64_t hhdm = limine_get_hhdm_offset();
    if (hhdm && addr >= hhdm)
      return (phys_addr_t)(addr - hhdm);
  }
#endif

  paddr = vmm_virt_to_phys((virt_addr_t)(uintptr_t)ptr);
  if (paddr)
    return paddr;

  if ((uintptr_t)ptr >= PHYS_OFFSET)
    return virt_to_phys((void *)ptr);

  return (phys_addr_t)(uintptr_t)ptr;
}

static void storage_copy_string(char *dst, const char *src, int max) {
  int i = 0;
  if (!dst || max <= 0)
    return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  while (src[i] && i < max - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static void storage_append_string(char *dst, int max, const char *src) {
  int idx = 0;
  if (!dst || !src || max <= 0)
    return;
  while (idx < max - 1 && dst[idx])
    idx++;
  for (int i = 0; src[i] && idx < max - 1; i++)
    dst[idx++] = src[i];
  dst[idx] = '\0';
}

static void storage_append_decimal(char *dst, int max, int value) {
  char digits[16];
  int count = 0;
  int idx = 0;

  if (!dst || max <= 0)
    return;
  while (idx < max - 1 && dst[idx])
    idx++;

  if (value == 0) {
    if (idx < max - 1) {
      dst[idx++] = '0';
      dst[idx] = '\0';
    }
    return;
  }

  if (value < 0 && idx < max - 1) {
    dst[idx++] = '-';
    value = -value;
  }

  while (value > 0 && count < (int)sizeof(digits)) {
    digits[count++] = (char)('0' + (value % 10));
    value /= 10;
  }

  while (count > 0 && idx < max - 1) {
    dst[idx++] = digits[--count];
  }
  dst[idx] = '\0';
}

static void storage_append_location(char *buf, int max, const char *prefix,
                                    int value) {
  storage_append_string(buf, max, prefix);
  storage_append_decimal(buf, max, value);
}

static uint32_t storage_default_capacity_mib(storage_kind_t kind) {
  switch (kind) {
  case STORAGE_KIND_IDE:
    return 32768;
  case STORAGE_KIND_AHCI:
  case STORAGE_KIND_SATA:
    return 131072;
  case STORAGE_KIND_NVME:
  case STORAGE_KIND_APPLE_ANS:
    return 262144;
  case STORAGE_KIND_CDROM:
    return 700;
  case STORAGE_KIND_USB_MASS_STORAGE:
    return 8192;
  default:
    return 16384;
  }
}

const char *storage_partition_kind_name(storage_partition_kind_t kind) {
  switch (kind) {
  case STORAGE_PARTITION_EFI:
    return "EFI System";
  case STORAGE_PARTITION_SYSTEM:
    return "Update";
  case STORAGE_PARTITION_DATA:
    return "Data";
  case STORAGE_PARTITION_SWAP:
    return "Swap";
  default:
    return "Unknown";
  }
}

static uint32_t storage_partition_used_mib(int disk_index) {
  uint32_t total = 0;
  if (disk_index < 0 || disk_index >= storage_disk_count)
    return 0;
  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    if (storage_partitions[disk_index][i].present)
      total += storage_partitions[disk_index][i].size_mib;
  }
  return total;
}

static int storage_find_free_partition_slot(int disk_index) {
  if (disk_index < 0 || disk_index >= storage_disk_count)
    return -1;
  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    if (!storage_partitions[disk_index][i].present)
      return i;
  }
  return -1;
}

static void storage_default_partition_label(char *buf, int max,
                                            storage_partition_kind_t kind,
                                            int index) {
  if (!buf || max <= 0)
    return;
  buf[0] = '\0';
  switch (kind) {
  case STORAGE_PARTITION_EFI:
    storage_append_string(buf, max, "EFI");
    break;
  case STORAGE_PARTITION_SYSTEM:
    storage_append_string(buf, max, "Update");
    break;
  case STORAGE_PARTITION_DATA:
    storage_append_string(buf, max, "Data");
    break;
  case STORAGE_PARTITION_SWAP:
    storage_append_string(buf, max, "Swap");
    break;
  default:
    storage_append_string(buf, max, "Partition");
    break;
  }
  if (index > 0) {
    storage_append_string(buf, max, " ");
    storage_append_decimal(buf, max, index + 1);
  }
}

static uint32_t storage_mib_to_sectors(uint32_t size_mib) {
  return size_mib * 2048U;
}

static uint8_t storage_partition_mbr_type(storage_partition_kind_t kind) {
  switch (kind) {
  case STORAGE_PARTITION_EFI:
    return 0xEF;
  case STORAGE_PARTITION_SYSTEM:
    return 0x83;
  case STORAGE_PARTITION_DATA:
    return 0x83;
  case STORAGE_PARTITION_SWAP:
    return 0x82;
  default:
    return 0x83;
  }
}

static void storage_write_le32(uint8_t *dst, uint32_t value) {
  dst[0] = (uint8_t)(value & 0xFF);
  dst[1] = (uint8_t)((value >> 8) & 0xFF);
  dst[2] = (uint8_t)((value >> 16) & 0xFF);
  dst[3] = (uint8_t)((value >> 24) & 0xFF);
}

static uint32_t storage_read_le32(const uint8_t *src) {
  return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
         ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

const char *storage_kind_name(storage_kind_t kind) {
  switch (kind) {
  case STORAGE_KIND_IDE:
    return "IDE/PATA";
  case STORAGE_KIND_AHCI:
    return "AHCI SATA";
  case STORAGE_KIND_SATA:
    return "SATA";
  case STORAGE_KIND_RAID:
    return "RAID";
  case STORAGE_KIND_NVME:
    return "NVMe";
  case STORAGE_KIND_CDROM:
    return "CD-ROM";
  case STORAGE_KIND_USB_MASS_STORAGE:
    return "USB Mass Storage";
  case STORAGE_KIND_APPLE_ANS:
    return "Apple ANS NVMe";
  default:
    return "Unknown Storage";
  }
}

static storage_kind_t storage_classify_pci(const pci_device_t *dev) {
  if (!dev)
    return STORAGE_KIND_UNKNOWN;

  if (dev->class_code != 0x01)
    return STORAGE_KIND_UNKNOWN;

  if (dev->subclass == 0x01)
    return STORAGE_KIND_IDE;
  if (dev->subclass == 0x04)
    return STORAGE_KIND_RAID;
  if (dev->subclass == 0x06) {
    if (dev->prog_if == 0x01)
      return STORAGE_KIND_AHCI;
    return STORAGE_KIND_SATA;
  }
  if (dev->subclass == 0x08)
    return STORAGE_KIND_NVME;

  return STORAGE_KIND_UNKNOWN;
}

static int storage_find_controller_index(storage_kind_t kind, uint8_t bus,
                                         uint8_t slot, uint8_t func,
                                         const char *name) {
  for (int i = 0; i < storage_controller_count; i++) {
    storage_controller_t *ctrl = &storage_controllers[i];
    if (name && ctrl->bus_name[0] && ctrl->bus == 0xFF && ctrl->slot == 0xFF &&
        ctrl->func == 0xFF) {
      int j = 0;
      while (name[j] && ctrl->name[j] && name[j] == ctrl->name[j])
        j++;
      if (name[j] == '\0' && ctrl->name[j] == '\0')
        return i;
    }
    if (ctrl->kind == kind && ctrl->bus == bus && ctrl->slot == slot &&
        ctrl->func == func) {
      return i;
    }
  }
  return -1;
}

static int storage_record_controller(storage_kind_t kind, uint16_t vendor,
                                     uint16_t device, uint8_t bus,
                                     uint8_t slot, uint8_t func,
                                     const char *name, const char *bus_name) {
  storage_controller_t *ctrl;
  int existing;

  if (kind <= STORAGE_KIND_UNKNOWN || kind > STORAGE_KIND_APPLE_ANS)
    return -1;
  existing = storage_find_controller_index(kind, bus, slot, func, name);
  if (existing >= 0)
    return existing;
  if (storage_controller_count >= STORAGE_MAX_CONTROLLERS)
    return -1;

  ctrl = &storage_controllers[storage_controller_count++];
  ctrl->kind = kind;
  ctrl->vendor_id = vendor;
  ctrl->device_id = device;
  ctrl->bus = bus;
  ctrl->slot = slot;
  ctrl->func = func;
  storage_copy_string(ctrl->name, name ? name : storage_kind_name(kind),
                      sizeof(ctrl->name));
  storage_copy_string(ctrl->bus_name, bus_name ? bus_name : "pci",
                      sizeof(ctrl->bus_name));
  storage_kind_counts[kind]++;

  printk(KERN_INFO "STORAGE: Registered %s controller via %s\n", ctrl->name,
         ctrl->bus_name);
  return storage_controller_count - 1;
}

static int storage_disk_exists(const char *name, const char *location) {
  for (int i = 0; i < storage_disk_count; i++) {
    int j = 0;
    while (name[j] && storage_disks[i].name[j] &&
           name[j] == storage_disks[i].name[j]) {
      j++;
    }
    if (name[j] != '\0' || storage_disks[i].name[j] != '\0')
      continue;

    j = 0;
    while (location[j] && storage_disks[i].location[j] &&
           location[j] == storage_disks[i].location[j]) {
      j++;
    }
    if (location[j] == '\0' && storage_disks[i].location[j] == '\0')
      return 1;
  }
  return 0;
}

static void storage_record_disk(storage_kind_t kind, int controller_index,
                                int disk_index, const char *name,
                                const char *location) {
  storage_disk_t *disk;

  if (!name || !location)
    return;
  if (storage_disk_count >= STORAGE_MAX_DISKS)
    return;
  if (storage_disk_exists(name, location))
    return;

  disk = &storage_disks[storage_disk_count++];
  disk->kind = kind;
  disk->controller_index = (uint8_t)controller_index;
  disk->disk_index = (uint8_t)disk_index;
  disk->capacity_mib = storage_default_capacity_mib(kind);
  disk->read_fn = NULL;
  disk->write_fn = NULL;
  disk->backend_ctx = NULL;
  storage_copy_string(disk->name, name, sizeof(disk->name));
  storage_copy_string(disk->location, location, sizeof(disk->location));

  printk(KERN_INFO "STORAGE: Registered disk %s at %s\n", disk->name,
         disk->location);
  if (kind == STORAGE_KIND_IDE)
    storage_load_mbr_partitions(storage_disk_count - 1);
}

static int storage_find_disk_by_location(const char *location) {
  int i;

  if (!location)
    return -1;

  for (i = 0; i < storage_disk_count; i++) {
    int j = 0;
    while (location[j] && storage_disks[i].location[j] &&
           location[j] == storage_disks[i].location[j]) {
      j++;
    }
    if (location[j] == '\0' && storage_disks[i].location[j] == '\0')
      return i;
  }

  return -1;
}

#if defined(ARCH_X86_64) || defined(ARCH_X86)
static int storage_ide_wait(uint16_t io_base, uint8_t mask, uint8_t value,
                            int timeout) {
  while (timeout-- > 0) {
    uint8_t status = inb(io_base + 7);
    if ((status & mask) == value)
      return status;
    io_wait();
  }
  return -1;
}

static int storage_ide_disk_geometry(const storage_disk_t *disk,
                                     uint16_t *io_base,
                                     uint8_t *drive_select) {
  if (!disk || !io_base || !drive_select || disk->kind != STORAGE_KIND_IDE)
    return -1;

  switch (disk->disk_index) {
  case 0:
    *io_base = 0x1F0;
    *drive_select = 0x00;
    return 0;
  case 1:
    *io_base = 0x1F0;
    *drive_select = 0x10;
    return 0;
  case 2:
    *io_base = 0x170;
    *drive_select = 0x00;
    return 0;
  case 3:
    *io_base = 0x170;
    *drive_select = 0x10;
    return 0;
  default:
    return -1;
  }
}

static int storage_ide_read_sector(const storage_disk_t *disk, uint32_t lba,
                                   void *buffer) {
  uint16_t io_base;
  uint8_t drive_select;
  int status;
  uint16_t *words = (uint16_t *)buffer;

  if (!buffer || storage_ide_disk_geometry(disk, &io_base, &drive_select) != 0)
    return -1;
  if (lba > 0x0FFFFFFF)
    return -1;

  outb(io_base + 6,
       (uint8_t)(0xE0 | drive_select | ((lba >> 24) & 0x0F)));
  io_wait();
  outb(io_base + 1, 0);
  outb(io_base + 2, 1);
  outb(io_base + 3, (uint8_t)(lba & 0xFF));
  outb(io_base + 4, (uint8_t)((lba >> 8) & 0xFF));
  outb(io_base + 5, (uint8_t)((lba >> 16) & 0xFF));
  outb(io_base + 7, 0x20);

  status = storage_ide_wait(io_base, 0x88, 0x08, 100000);
  if (status < 0 || (status & 0x01))
    return -1;

  for (int i = 0; i < 256; i++)
    words[i] = inw(io_base);
  return 0;
}

static int storage_ide_read_atapi_packet(uint16_t io_base, uint8_t drive_select,
                                         uint32_t lba, void *buffer) {
  int status;
  uint16_t *words = (uint16_t *)buffer;
  uint16_t byte_count;
  uint16_t words_reported;
  uint16_t words_to_copy;
  uint16_t words_to_discard;
  uint8_t packet[12] = {0};

  if (!buffer)
    return -1;

  outb(io_base + 6, (uint8_t)(0xA0 | drive_select));
  io_wait();
  outb(io_base + 1, 0);
  outb(io_base + 4, 0x00);
  outb(io_base + 5, 0x08);
  outb(io_base + 7, 0xA0);

  status = storage_ide_wait(io_base, 0x88, 0x08, 100000);
  if (status < 0 || (status & 0x01))
    return -1;

  packet[0] = 0xA8;
  packet[2] = (uint8_t)((lba >> 24) & 0xFF);
  packet[3] = (uint8_t)((lba >> 16) & 0xFF);
  packet[4] = (uint8_t)((lba >> 8) & 0xFF);
  packet[5] = (uint8_t)(lba & 0xFF);
  packet[9] = 1;

  for (int i = 0; i < 6; i++) {
    uint16_t word = (uint16_t)packet[i * 2] |
                    ((uint16_t)packet[i * 2 + 1] << 8);
    outw(io_base, word);
  }

  status = storage_ide_wait(io_base, 0x88, 0x08, 100000);
  if (status < 0 || (status & 0x01))
    return -1;

  byte_count = (uint16_t)inb(io_base + 4) |
               ((uint16_t)inb(io_base + 5) << 8);
  if (byte_count == 0)
    byte_count = 2048;

  words_reported = (uint16_t)((byte_count + 1U) / 2U);
  words_to_copy = words_reported > 1024 ? 1024 : words_reported;
  words_to_discard = words_reported > 1024 ? (uint16_t)(words_reported - 1024) : 0;

  for (uint16_t i = 0; i < words_to_copy; i++)
    words[i] = inw(io_base);
  for (uint16_t i = words_to_copy; i < 1024; i++)
    words[i] = 0;
  for (uint16_t i = 0; i < words_to_discard; i++)
    (void)inw(io_base);

  return 0;
}

static int storage_ide_read_atapi_block(const storage_disk_t *disk, uint32_t lba,
                                        void *buffer) {
  uint16_t io_base;
  uint8_t drive_select;

  if (!buffer || storage_ide_disk_geometry(disk, &io_base, &drive_select) != 0)
    return -1;
  return storage_ide_read_atapi_packet(io_base, drive_select, lba, buffer);
}

static int storage_ide_atapi_read(uint64_t lba, uint32_t count, void *buffer,
                                  void *ctx) {
  storage_ide_atapi_ctx_t *ide_ctx = (storage_ide_atapi_ctx_t *)ctx;
  uint8_t *dst = (uint8_t *)buffer;

  if (!ide_ctx || !ide_ctx->active || !buffer || count == 0)
    return -1;

  for (uint32_t i = 0; i < count; i++) {
    if (storage_ide_read_atapi_packet(ide_ctx->io_base, ide_ctx->drive_select,
                                      (uint32_t)(lba + i), dst + i * 2048) != 0)
      return -1;
  }

  return 0;
}

static int storage_ide_write_sector(const storage_disk_t *disk, uint32_t lba,
                                    const void *buffer) {
  uint16_t io_base;
  uint8_t drive_select;
  int status;
  const uint16_t *words = (const uint16_t *)buffer;

  if (!buffer || storage_ide_disk_geometry(disk, &io_base, &drive_select) != 0)
    return -1;
  if (lba > 0x0FFFFFFF)
    return -1;

  outb(io_base + 6,
       (uint8_t)(0xE0 | drive_select | ((lba >> 24) & 0x0F)));
  io_wait();
  outb(io_base + 1, 0);
  outb(io_base + 2, 1);
  outb(io_base + 3, (uint8_t)(lba & 0xFF));
  outb(io_base + 4, (uint8_t)((lba >> 8) & 0xFF));
  outb(io_base + 5, (uint8_t)((lba >> 16) & 0xFF));
  outb(io_base + 7, 0x30);

  status = storage_ide_wait(io_base, 0x88, 0x08, 100000);
  if (status < 0 || (status & 0x01))
    return -1;

  for (int i = 0; i < 256; i++)
    outw(io_base, words[i]);
  outb(io_base + 7, 0xE7);
  status = storage_ide_wait(io_base, 0x80, 0x00, 100000);
  if (status < 0 || (status & 0x01))
    return -1;
  return 0;
}

static void storage_probe_ide_channel(int controller_index, uint16_t io_base,
                                      uint8_t drive_select, uint8_t ide_index) {
  uint16_t identify[256];
  char location[24];
  int status;
  uint8_t mid;
  uint8_t hi;
  uint32_t total_sectors;
  int disk_slot;
  storage_ide_atapi_ctx_t *ide_ctx;

  outb(io_base + 6, (uint8_t)(0xA0 | drive_select));
  io_wait();
  outb(io_base + 2, 0);
  outb(io_base + 3, 0);
  outb(io_base + 4, 0);
  outb(io_base + 5, 0);
  outb(io_base + 7, 0xEC);
  io_wait();

  status = inb(io_base + 7);
  if (status == 0)
    return;

  status = storage_ide_wait(io_base, 0x80, 0x00, 100000);
  if (status < 0)
    return;

  mid = inb(io_base + 4);
  hi = inb(io_base + 5);
  if (mid != 0 || hi != 0) {
    if (mid == 0x14 && hi == 0xEB) {
      outb(io_base + 7, 0xA1);
      status = storage_ide_wait(io_base, 0x89, 0x08, 100000);
      if (status < 0 || !(status & 0x08))
        return;

      for (int i = 0; i < 256; i++)
        identify[i] = inw(io_base);

      location[0] = '\0';
      storage_append_location(location, sizeof(location), "cd", storage_disk_count);
      storage_record_disk(STORAGE_KIND_CDROM, controller_index, ide_index,
                          "ATAPI CD-ROM", location);
      disk_slot = storage_find_disk_by_location(location);
      if (disk_slot >= 0 && ide_index >= 0 &&
          ide_index < (int)(sizeof(storage_ide_atapi_contexts) /
                            sizeof(storage_ide_atapi_contexts[0]))) {
        ide_ctx = &storage_ide_atapi_contexts[ide_index];
        ide_ctx->io_base = io_base;
        ide_ctx->drive_select = drive_select;
        ide_ctx->active = 1;
        storage_register_disk_backend(location, storage_ide_atapi_read, NULL,
                                      ide_ctx);
      }
    }
    return;
  }

  status = storage_ide_wait(io_base, 0x09, 0x08, 100000);
  if (status < 0 || !(status & 0x08))
    return;

  for (int i = 0; i < 256; i++)
    identify[i] = inw(io_base);

  location[0] = '\0';
  storage_append_location(location, sizeof(location), "hd", storage_disk_count);
  storage_record_disk(STORAGE_KIND_IDE, controller_index, ide_index,
                      "IDE Hard Disk", location);
  total_sectors = (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);
  disk_slot = storage_find_disk_by_location(location);
  if (disk_slot >= 0 && total_sectors > 0) {
    storage_disks[disk_slot].capacity_mib =
        total_sectors / 2048U ? total_sectors / 2048U : 1;
  }
}
#endif

static void storage_probe_ide_controller(int controller_index) {
#if defined(ARCH_X86_64) || defined(ARCH_X86)
  storage_probe_ide_channel(controller_index, 0x1F0, 0x00, 0);
  storage_probe_ide_channel(controller_index, 0x1F0, 0x10, 1);
  storage_probe_ide_channel(controller_index, 0x170, 0x00, 2);
  storage_probe_ide_channel(controller_index, 0x170, 0x10, 3);
#else
  (void)controller_index;
#endif
}

static uint64_t storage_get_deadline_ms(uint32_t timeout_ms) {
  return arch_timer_get_ms() + timeout_ms;
}

static int storage_wait_for_bit32(volatile uint32_t *reg, uint32_t mask,
                                  uint32_t value, uint32_t timeout_ms) {
  uint64_t deadline = storage_get_deadline_ms(timeout_ms);
  while (arch_timer_get_ms() <= deadline) {
    if ((*reg & mask) == value)
      return 0;
    io_wait();
  }
  return -1;
}

typedef struct {
  uint16_t flags;
  uint16_t prdtl;
  uint32_t prdbc;
  uint32_t ctba;
  uint32_t ctbau;
  uint32_t reserved[4];
} __attribute__((packed)) ahci_cmd_header_t;

typedef struct {
  uint8_t fis_type;
  uint8_t pmport_c;
  uint8_t command;
  uint8_t featurel;
  uint8_t lba0;
  uint8_t lba1;
  uint8_t lba2;
  uint8_t device;
  uint8_t lba3;
  uint8_t lba4;
  uint8_t lba5;
  uint8_t featureh;
  uint8_t countl;
  uint8_t counth;
  uint8_t icc;
  uint8_t control;
  uint8_t reserved[4];
} __attribute__((packed)) ahci_fis_reg_h2d_t;

typedef struct {
  uint32_t dba;
  uint32_t dbau;
  uint32_t reserved0;
  uint32_t dbc_i;
} __attribute__((packed)) ahci_prdt_entry_t;

typedef struct {
  uint8_t cfis[64];
  uint8_t acmd[16];
  uint8_t reserved[48];
  ahci_prdt_entry_t prdt[1];
} __attribute__((packed)) ahci_cmd_table_t;

static void storage_set_prdt_addr(ahci_prdt_entry_t *prdt, const void *buffer) {
  phys_addr_t paddr = storage_dma_addr(buffer);

  prdt->dba = (uint32_t)(paddr & 0xFFFFFFFFULL);
  prdt->dbau = (uint32_t)(paddr >> 32);
}

static int storage_ahci_port_wait_ready(storage_ahci_port_ctx_t *ctx) {
  volatile uint32_t *port;
  uint64_t deadline;

  if (!ctx || !ctx->port_mmio)
    return -1;
  port = (volatile uint32_t *)ctx->port_mmio;
  deadline = storage_get_deadline_ms(STORAGE_IO_TIMEOUT_MS);
  while (arch_timer_get_ms() <= deadline) {
    if ((port[0x20 / 4] & (0x80 | 0x08)) == 0)
      return 0;
    io_wait();
  }
  return -1;
}

static void storage_ahci_port_stop(storage_ahci_port_ctx_t *ctx) {
  volatile uint32_t *port;
  uint32_t cmd;

  if (!ctx || !ctx->port_mmio)
    return;
  port = (volatile uint32_t *)ctx->port_mmio;
  cmd = port[0x18 / 4];
  cmd &= ~((uint32_t)(1U << 0) | (uint32_t)(1U << 4));
  port[0x18 / 4] = cmd;
  (void)storage_wait_for_bit32(&port[0x18 / 4], (1U << 15) | (1U << 14), 0,
                               STORAGE_IO_TIMEOUT_MS);
}

static void storage_ahci_port_start(storage_ahci_port_ctx_t *ctx) {
  volatile uint32_t *port;
  uint32_t cmd;

  if (!ctx || !ctx->port_mmio)
    return;
  port = (volatile uint32_t *)ctx->port_mmio;
  cmd = port[0x18 / 4];
  cmd |= (1U << 4);
  port[0x18 / 4] = cmd;
  cmd |= (1U << 0);
  port[0x18 / 4] = cmd;
}

static void storage_ahci_setup_port(storage_ahci_port_ctx_t *ctx) {
  volatile uint32_t *port;
  ahci_cmd_header_t *header;

  if (!ctx || !ctx->port_mmio)
    return;
  port = (volatile uint32_t *)ctx->port_mmio;
  storage_ahci_port_stop(ctx);
  {
    phys_addr_t command_list = storage_dma_addr(ctx->command_list);
    phys_addr_t rfis = storage_dma_addr(ctx->rfis);
    port[0x00 / 4] = (uint32_t)(command_list & 0xFFFFFFFFULL);
    port[0x04 / 4] = (uint32_t)(command_list >> 32);
    port[0x08 / 4] = (uint32_t)(rfis & 0xFFFFFFFFULL);
    port[0x0C / 4] = (uint32_t)(rfis >> 32);
  }
  port[0x10 / 4] = 0xFFFFFFFF;
  for (int i = 0; i < (int)sizeof(ctx->command_list); i++)
    ctx->command_list[i] = 0;
  for (int i = 0; i < (int)sizeof(ctx->rfis); i++)
    ctx->rfis[i] = 0;
  for (int i = 0; i < (int)sizeof(ctx->command_table); i++)
    ctx->command_table[i] = 0;
  header = (ahci_cmd_header_t *)ctx->command_list;
  header[0].flags = 5;
  header[0].prdtl = 1;
  {
    phys_addr_t command_table = storage_dma_addr(ctx->command_table);
    header[0].ctba = (uint32_t)(command_table & 0xFFFFFFFFULL);
    header[0].ctbau = (uint32_t)(command_table >> 32);
  }
  storage_ahci_port_start(ctx);
}

static int storage_ahci_issue(storage_ahci_port_ctx_t *ctx, uint8_t command,
                              uint64_t lba, uint16_t count, void *buffer,
                              int write) {
  volatile uint32_t *port;
  ahci_cmd_header_t *header;
  ahci_cmd_table_t *table;
  ahci_fis_reg_h2d_t *fis;
  uint64_t deadline;

  if (!ctx || !ctx->port_mmio || !buffer || count == 0)
    return -1;
  if (storage_ahci_port_wait_ready(ctx) != 0)
    return -1;
  port = (volatile uint32_t *)ctx->port_mmio;
  header = (ahci_cmd_header_t *)ctx->command_list;
  table = (ahci_cmd_table_t *)ctx->command_table;
  for (int i = 0; i < (int)sizeof(ctx->command_table); i++)
    ctx->command_table[i] = 0;
  header[0].flags = (uint16_t)(5 | (write ? (1U << 6) : 0));
  header[0].prdtl = 1;
  header[0].prdbc = 0;
  fis = (ahci_fis_reg_h2d_t *)table->cfis;
  fis->fis_type = 0x27;
  fis->pmport_c = 1U << 7;
  fis->command = command;
  fis->device = 1U << 6;
  fis->lba0 = (uint8_t)(lba & 0xFF);
  fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
  fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
  fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
  fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
  fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
  fis->countl = (uint8_t)(count & 0xFF);
  fis->counth = (uint8_t)((count >> 8) & 0xFF);
  storage_set_prdt_addr(&table->prdt[0], buffer);
  table->prdt[0].dbc_i =
      (uint32_t)(count * AHCI_SECTOR_SIZE - 1) | (1U << 31);
  port[0x10 / 4] = 0xFFFFFFFF;
  port[0x38 / 4] = 1;
  deadline = storage_get_deadline_ms(STORAGE_IO_TIMEOUT_MS);
  while (arch_timer_get_ms() <= deadline) {
    if ((port[0x38 / 4] & 1U) == 0)
      break;
    if (port[0x10 / 4] & (1U << 30))
      return -1;
  }
  if (port[0x38 / 4] & 1U)
    return -1;
  return (port[0x20 / 4] & 0x01) ? -1 : 0;
}

static int storage_ahci_issue_atapi(storage_ahci_port_ctx_t *ctx, uint32_t lba,
                                    uint16_t blocks, void *buffer) {
  volatile uint32_t *port;
  ahci_cmd_header_t *header;
  ahci_cmd_table_t *table;
  ahci_fis_reg_h2d_t *fis;
  uint64_t deadline;

  if (!ctx || !ctx->port_mmio || !buffer || blocks == 0)
    return -1;
  if (storage_ahci_port_wait_ready(ctx) != 0)
    return -1;

  port = (volatile uint32_t *)ctx->port_mmio;
  header = (ahci_cmd_header_t *)ctx->command_list;
  table = (ahci_cmd_table_t *)ctx->command_table;
  for (int i = 0; i < (int)sizeof(ctx->command_table); i++)
    ctx->command_table[i] = 0;

  header[0].flags = (uint16_t)(5 | (1U << 5));
  header[0].prdtl = 1;
  header[0].prdbc = 0;

  fis = (ahci_fis_reg_h2d_t *)table->cfis;
  fis->fis_type = 0x27;
  fis->pmport_c = 1U << 7;
  fis->command = 0xA0;
  /* ATA PACKET byte count is cylinder low/high. Request one 2048-byte CD sector. */
  fis->lba1 = 0x00;
  fis->lba2 = 0x08;

  table->acmd[0] = 0xA8;
  table->acmd[2] = (uint8_t)((lba >> 24) & 0xFF);
  table->acmd[3] = (uint8_t)((lba >> 16) & 0xFF);
  table->acmd[4] = (uint8_t)((lba >> 8) & 0xFF);
  table->acmd[5] = (uint8_t)(lba & 0xFF);
  table->acmd[7] = (uint8_t)((blocks >> 8) & 0xFF);
  table->acmd[8] = (uint8_t)(blocks & 0xFF);

  storage_set_prdt_addr(&table->prdt[0], buffer);
  table->prdt[0].dbc_i = (uint32_t)(blocks * 2048U - 1U) | (1U << 31);

  port[0x10 / 4] = 0xFFFFFFFF;
  port[0x38 / 4] = 1;
  deadline = storage_get_deadline_ms(STORAGE_IO_TIMEOUT_MS);
  while (arch_timer_get_ms() <= deadline) {
    if ((port[0x38 / 4] & 1U) == 0)
      break;
    if (port[0x10 / 4] & (1U << 30))
      return -1;
  }
  if (port[0x38 / 4] & 1U)
    return -1;
  return (port[0x20 / 4] & 0x01) ? -1 : 0;
}

static int storage_ahci_read(uint64_t lba, uint32_t count, void *buffer,
                             void *ctx_ptr) {
  storage_ahci_port_ctx_t *ctx = (storage_ahci_port_ctx_t *)ctx_ptr;
  uint8_t *dst = (uint8_t *)buffer;
  for (uint32_t i = 0; i < count; i++) {
    if (storage_ahci_issue(ctx, 0x25, lba + i, 1, dst + i * AHCI_SECTOR_SIZE,
                           0) != 0)
      return -1;
  }
  return 0;
}

static int storage_ahci_write(uint64_t lba, uint32_t count, const void *buffer,
                              void *ctx_ptr) {
  storage_ahci_port_ctx_t *ctx = (storage_ahci_port_ctx_t *)ctx_ptr;
  const uint8_t *src = (const uint8_t *)buffer;
  for (uint32_t i = 0; i < count; i++) {
    if (storage_ahci_issue(ctx, 0x35, lba + i, 1,
                           (void *)(uintptr_t)(src + i * AHCI_SECTOR_SIZE),
                           1) != 0)
      return -1;
  }
  return 0;
}

static int storage_ahci_atapi_read(uint64_t lba, uint32_t count, void *buffer,
                                   void *ctx_ptr) {
  storage_ahci_port_ctx_t *ctx = (storage_ahci_port_ctx_t *)ctx_ptr;
  uint8_t *dst = (uint8_t *)buffer;

  for (uint32_t i = 0; i < count; i++) {
    if (storage_ahci_issue_atapi(ctx, (uint32_t)(lba + i), 1,
                                 dst + i * 2048U) != 0)
      return -1;
  }
  return 0;
}

static void storage_ahci_extract_model(const uint16_t *identify, char *buf,
                                       int max) {
  int out = 0;
  if (!identify || !buf || max <= 0)
    return;
  for (int word = 27; word <= 46 && out < max - 1; word++) {
    char hi = (char)((identify[word] >> 8) & 0xFF);
    char lo = (char)(identify[word] & 0xFF);
    if (hi && out < max - 1)
      buf[out++] = hi;
    if (lo && out < max - 1)
      buf[out++] = lo;
  }
  while (out > 0 && buf[out - 1] == ' ')
    out--;
  buf[out] = '\0';
}

typedef struct {
  uint8_t opcode;
  uint8_t flags;
  uint16_t cid;
  uint32_t nsid;
  uint64_t rsvd2;
  uint64_t mptr;
  uint64_t prp1;
  uint64_t prp2;
  uint32_t cdw10;
  uint32_t cdw11;
  uint32_t cdw12;
  uint32_t cdw13;
  uint32_t cdw14;
  uint32_t cdw15;
} __attribute__((packed)) nvme_sqe_t;

typedef struct {
  uint32_t cdw0;
  uint32_t rsvd;
  uint16_t sq_head;
  uint16_t sq_id;
  uint16_t cid;
  uint16_t status;
} __attribute__((packed)) nvme_cqe_t;

static uint32_t storage_nvme_db_stride(volatile uint32_t *regs) {
  return 4U << ((regs[0] >> 20) & 0xF);
}

static volatile uint32_t *storage_nvme_db_reg(volatile uint32_t *regs,
                                              uint16_t qid, int is_cq) {
  uintptr_t base = (uintptr_t)regs + 0x1000;
  uintptr_t stride = storage_nvme_db_stride(regs);
  return (volatile uint32_t *)(base + ((qid * 2U + (is_cq ? 1U : 0U)) * stride));
}

static int storage_nvme_wait_ready(storage_nvme_ctx_t *ctx, int ready) {
  uint64_t deadline;
  if (!ctx || !ctx->regs)
    return -1;
  deadline = storage_get_deadline_ms(STORAGE_IO_TIMEOUT_MS);
  while (arch_timer_get_ms() <= deadline) {
    if ((((ctx->regs[0x1C / 4] & 1U) != 0) ? 1 : 0) == (ready ? 1 : 0))
      return 0;
  }
  return -1;
}

static int storage_nvme_submit_admin(storage_nvme_ctx_t *ctx, nvme_sqe_t *cmd) {
  nvme_sqe_t *sq;
  nvme_cqe_t *cq;
  uint16_t cid;
  uint64_t deadline;

  if (!ctx || !ctx->regs || !cmd)
    return -1;
  sq = (nvme_sqe_t *)ctx->admin_sq;
  cq = (nvme_cqe_t *)ctx->admin_cq;
  cid = ctx->admin_sq_tail;
  cmd->cid = cid;
  sq[ctx->admin_sq_tail] = *cmd;
  ctx->admin_sq_tail = (uint16_t)((ctx->admin_sq_tail + 1) % NVME_ADMIN_Q_DEPTH);
  *storage_nvme_db_reg(ctx->regs, 0, 0) = ctx->admin_sq_tail;
  deadline = storage_get_deadline_ms(STORAGE_IO_TIMEOUT_MS);
  while (arch_timer_get_ms() <= deadline) {
    nvme_cqe_t *entry = &cq[ctx->admin_cq_head];
    if (((entry->status >> 15) & 1U) == ctx->admin_phase) {
      if (entry->cid != cid || ((entry->status >> 1) & 0x7FF) != 0)
        return -1;
      ctx->admin_cq_head =
          (uint16_t)((ctx->admin_cq_head + 1) % NVME_ADMIN_Q_DEPTH);
      if (ctx->admin_cq_head == 0)
        ctx->admin_phase ^= 1U;
      *storage_nvme_db_reg(ctx->regs, 0, 1) = ctx->admin_cq_head;
      return 0;
    }
  }
  return -1;
}

static int storage_nvme_submit_io(storage_nvme_ctx_t *ctx, nvme_sqe_t *cmd) {
  nvme_sqe_t *sq;
  nvme_cqe_t *cq;
  uint16_t cid;
  uint64_t deadline;

  if (!ctx || !ctx->regs || !cmd)
    return -1;
  sq = (nvme_sqe_t *)ctx->io_sq;
  cq = (nvme_cqe_t *)ctx->io_cq;
  cid = ctx->io_sq_tail;
  cmd->cid = cid;
  sq[ctx->io_sq_tail] = *cmd;
  ctx->io_sq_tail = (uint16_t)((ctx->io_sq_tail + 1) % NVME_IO_Q_DEPTH);
  *storage_nvme_db_reg(ctx->regs, 1, 0) = ctx->io_sq_tail;
  deadline = storage_get_deadline_ms(STORAGE_IO_TIMEOUT_MS);
  while (arch_timer_get_ms() <= deadline) {
    nvme_cqe_t *entry = &cq[ctx->io_cq_head];
    if (((entry->status >> 15) & 1U) == ctx->io_phase) {
      if (entry->cid != cid || ((entry->status >> 1) & 0x7FF) != 0)
        return -1;
      ctx->io_cq_head = (uint16_t)((ctx->io_cq_head + 1) % NVME_IO_Q_DEPTH);
      if (ctx->io_cq_head == 0)
        ctx->io_phase ^= 1U;
      *storage_nvme_db_reg(ctx->regs, 1, 1) = ctx->io_cq_head;
      return 0;
    }
  }
  return -1;
}

static int storage_nvme_rw(storage_nvme_ctx_t *ctx, uint64_t lba,
                           uint32_t count, void *buffer, int write) {
  nvme_sqe_t cmd;
  uint32_t bytes;

  if (!ctx || !ctx->active || !buffer || count == 0)
    return -1;
  bytes = count * ctx->sector_size;
  if (bytes > sizeof(ctx->io_buffer))
    return -1;
  if (write) {
    const uint8_t *src = (const uint8_t *)buffer;
    for (uint32_t i = 0; i < bytes; i++)
      ctx->io_buffer[i] = src[i];
  }
  for (int i = 0; i < (int)sizeof(cmd); i++)
    ((uint8_t *)&cmd)[i] = 0;
  cmd.opcode = write ? 0x01 : 0x02;
  cmd.nsid = ctx->nsid;
  cmd.prp1 = (uint64_t)(uintptr_t)ctx->io_buffer;
  cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFFULL);
  cmd.cdw11 = (uint32_t)(lba >> 32);
  cmd.cdw12 = count - 1;
  if (storage_nvme_submit_io(ctx, &cmd) != 0)
    return -1;
  if (!write) {
    uint8_t *dst = (uint8_t *)buffer;
    for (uint32_t i = 0; i < bytes; i++)
      dst[i] = ctx->io_buffer[i];
  }
  return 0;
}

static int storage_nvme_read(uint64_t lba, uint32_t count, void *buffer,
                             void *ctx_ptr) {
  return storage_nvme_rw((storage_nvme_ctx_t *)ctx_ptr, lba, count, buffer, 0);
}

static int storage_nvme_write(uint64_t lba, uint32_t count, const void *buffer,
                              void *ctx_ptr) {
  return storage_nvme_rw((storage_nvme_ctx_t *)ctx_ptr, lba, count,
                         (void *)(uintptr_t)buffer, 1);
}

static void storage_probe_ahci_controller(int controller_index,
                                          const pci_device_t *dev) {
  volatile uint32_t *abar;
  uint32_t ports_implemented;
  char location[24];
  char model[48];

  if (!dev || !dev->bar0)
    return;

  vmm_map_range(dev->bar0, dev->bar0, 0x2000, VM_DEVICE);
  abar = (volatile uint32_t *)(uintptr_t)dev->bar0;
  ports_implemented = abar[0x0C / 4];

  for (int port = 0; port < 32; port++) {
    volatile uint32_t *port_regs;
    uint32_t ssts;
    uint32_t sig;
    uint32_t det;
    uint32_t ipm;
    int disk_slot;
    storage_ahci_port_ctx_t *ctx;
    uint64_t sectors;

    if (!(ports_implemented & (1U << port)))
      continue;

    port_regs = (volatile uint32_t *)((uintptr_t)abar + 0x100 + port * 0x80);
    sig = port_regs[0x24 / 4];
    ssts = port_regs[0x28 / 4];
    det = ssts & 0x0F;
    ipm = (ssts >> 8) & 0x0F;

    if (det != 3 || ipm != 1)
      continue;
    if (sig == 0xEB140101) {
      storage_ahci_port_ctx_t *cd_ctx;

      location[0] = '\0';
      storage_append_location(location, sizeof(location), "cd", storage_disk_count);
      storage_record_disk(STORAGE_KIND_CDROM, controller_index, port,
                          "SATA ATAPI CD-ROM", location);
      disk_slot = storage_find_disk_by_location(location);
      if (disk_slot < 0)
        continue;

      cd_ctx = &storage_ahci_ports[disk_slot];
      cd_ctx->abar = abar;
      cd_ctx->port_mmio = (volatile uint8_t *)port_regs;
      cd_ctx->port_no = port;
      cd_ctx->active = 1;
      storage_ahci_setup_port(cd_ctx);
      storage_register_disk_backend(location, storage_ahci_atapi_read, NULL,
                                    cd_ctx);
      continue;
    }
    if (sig == 0x96690101)
      continue;

    location[0] = '\0';
    storage_append_location(location, sizeof(location), "sd", storage_disk_count);
    storage_record_disk(STORAGE_KIND_AHCI, controller_index, port,
                        "SATA Hard Disk", location);
    disk_slot = storage_find_disk_by_location(location);
    if (disk_slot < 0)
      continue;

    ctx = &storage_ahci_ports[disk_slot];
    ctx->abar = abar;
    ctx->port_mmio = (volatile uint8_t *)port_regs;
    ctx->port_no = port;
    ctx->active = 1;
    storage_ahci_setup_port(ctx);

    if (storage_ahci_issue(ctx, 0xEC, 0, 1, ctx->identify_data, 0) == 0) {
      sectors = ((uint64_t)ctx->identify_data[100]) |
                ((uint64_t)ctx->identify_data[101] << 16) |
                ((uint64_t)ctx->identify_data[102] << 32) |
                ((uint64_t)ctx->identify_data[103] << 48);
      if (sectors == 0) {
        sectors = (uint32_t)ctx->identify_data[60] |
                  ((uint32_t)ctx->identify_data[61] << 16);
      }
      if (sectors > 0) {
        ctx->sector_count = (uint32_t)(sectors > 0xFFFFFFFFULL ? 0xFFFFFFFFULL
                                                               : sectors);
        storage_disks[disk_slot].capacity_mib =
            (uint32_t)(sectors / 2048ULL ? sectors / 2048ULL : 1ULL);
      }
      storage_ahci_extract_model(ctx->identify_data, model, sizeof(model));
      if (model[0])
        storage_copy_string(storage_disks[disk_slot].name, model,
                            sizeof(storage_disks[disk_slot].name));
    }
    storage_register_disk_backend(location, storage_ahci_read, storage_ahci_write,
                                  ctx);
  }
}

static void storage_probe_nvme_controller(int controller_index,
                                          const pci_device_t *dev) {
  volatile uint32_t *regs;
  uint32_t cap_lo;
  uint32_t cap_hi;
  uint32_t version;
  char location[24];
  int disk_slot;
  storage_nvme_ctx_t *ctx;
  nvme_sqe_t cmd;
  uint32_t cc;
  uint64_t nsze;
  uint8_t flbas;
  uint8_t lbaf_index;
  uint8_t lbads;

  if (!dev || !dev->bar0)
    return;

  vmm_map_range(dev->bar0, dev->bar0, 0x1000, VM_DEVICE);
  regs = (volatile uint32_t *)(uintptr_t)dev->bar0;
  cap_lo = regs[0x00 / 4];
  cap_hi = regs[0x04 / 4];
  version = regs[0x08 / 4];

  if ((cap_lo == 0 && cap_hi == 0) || (cap_lo == 0xFFFFFFFF && cap_hi == 0xFFFFFFFF))
    return;
  if (version == 0 || version == 0xFFFFFFFF)
    return;

  location[0] = '\0';
  storage_append_location(location, sizeof(location), "nvme", storage_disk_count);
  storage_record_disk(STORAGE_KIND_NVME, controller_index, 0, "NVMe Disk",
                      location);
  disk_slot = storage_find_disk_by_location(location);
  if (disk_slot < 0)
    return;

  ctx = &storage_nvme_contexts[disk_slot];
  ctx->regs = regs;
  ctx->nsid = 1;
  ctx->sector_size = 512;
  ctx->sector_count = 0;
  ctx->admin_sq_tail = 0;
  ctx->admin_cq_head = 0;
  ctx->admin_phase = 1;
  ctx->io_sq_tail = 0;
  ctx->io_cq_head = 0;
  ctx->io_phase = 1;

  cc = regs[0x14 / 4];
  if (cc & 1U) {
    regs[0x14 / 4] = cc & ~1U;
    if (storage_nvme_wait_ready(ctx, 0) != 0)
      return;
  }

  regs[0x24 / 4] = ((NVME_ADMIN_Q_DEPTH - 1) << 16) | (NVME_ADMIN_Q_DEPTH - 1);
  regs[0x28 / 4] = (uint32_t)(uintptr_t)ctx->admin_sq;
  regs[0x2C / 4] = (uint32_t)(((uint64_t)(uintptr_t)ctx->admin_sq) >> 32);
  regs[0x30 / 4] = (uint32_t)(uintptr_t)ctx->admin_cq;
  regs[0x34 / 4] = (uint32_t)(((uint64_t)(uintptr_t)ctx->admin_cq) >> 32);
  regs[0x14 / 4] = (6U << 20) | (4U << 16) | 1U;
  if (storage_nvme_wait_ready(ctx, 1) != 0)
    return;

  for (int i = 0; i < (int)sizeof(cmd); i++)
    ((uint8_t *)&cmd)[i] = 0;
  cmd.opcode = 0x05;
  cmd.prp1 = (uint64_t)(uintptr_t)ctx->io_cq;
  cmd.cdw10 = ((NVME_IO_Q_DEPTH - 1) << 16) | 1U;
  cmd.cdw11 = 1U;
  if (storage_nvme_submit_admin(ctx, &cmd) != 0)
    return;

  for (int i = 0; i < (int)sizeof(cmd); i++)
    ((uint8_t *)&cmd)[i] = 0;
  cmd.opcode = 0x01;
  cmd.prp1 = (uint64_t)(uintptr_t)ctx->io_sq;
  cmd.cdw10 = ((NVME_IO_Q_DEPTH - 1) << 16) | 1U;
  cmd.cdw11 = 1U | (1U << 16);
  if (storage_nvme_submit_admin(ctx, &cmd) != 0)
    return;

  for (int i = 0; i < (int)sizeof(cmd); i++)
    ((uint8_t *)&cmd)[i] = 0;
  for (int i = 0; i < (int)sizeof(ctx->identify_data); i++)
    ctx->identify_data[i] = 0;
  cmd.opcode = 0x06;
  cmd.nsid = 1;
  cmd.prp1 = (uint64_t)(uintptr_t)ctx->identify_data;
  cmd.cdw10 = 0;
  if (storage_nvme_submit_admin(ctx, &cmd) != 0)
    return;

  nsze = (uint64_t)storage_read_le32(&ctx->identify_data[0]) |
         ((uint64_t)storage_read_le32(&ctx->identify_data[4]) << 32);
  flbas = ctx->identify_data[26];
  lbaf_index = flbas & 0x0F;
  lbads = ctx->identify_data[128 + lbaf_index * 4 + 2];
  if (lbads >= 9 && lbads < 17)
    ctx->sector_size = 1U << lbads;
  if (ctx->sector_size != 512)
    return;
  ctx->sector_count = nsze;
  ctx->active = 1;
  if (nsze > 0) {
    storage_disks[disk_slot].capacity_mib =
        (uint32_t)(((nsze * (uint64_t)ctx->sector_size) >> 20) ? 
                   ((nsze * (uint64_t)ctx->sector_size) >> 20) : 1ULL);
  }
  storage_register_disk_backend(location, storage_nvme_read, storage_nvme_write,
                                ctx);
}

static int storage_disk_read_sector(int disk_index, uint32_t lba, void *buffer) {
  if (disk_index < 0 || disk_index >= storage_disk_count || !buffer)
    return -1;

  /* CD-ROM backends expose 2048-byte logical blocks.  Do not service a
   * 512-byte sector request through their generic read callback because that
   * would copy a full optical block into a 512-byte caller buffer. */
  if (storage_disks[disk_index].kind == STORAGE_KIND_CDROM)
    return -1;

  if (storage_disks[disk_index].read_fn) {
    return storage_disks[disk_index].read_fn(lba, 1, buffer,
                                             storage_disks[disk_index].backend_ctx);
  }

  switch (storage_disks[disk_index].kind) {
#if defined(ARCH_X86_64) || defined(ARCH_X86)
  case STORAGE_KIND_IDE:
    return storage_ide_read_sector(&storage_disks[disk_index], lba, buffer);
#endif
  default:
    return -1;
  }
}

static int storage_disk_write_sector(int disk_index, uint32_t lba,
                                     const void *buffer) {
  if (disk_index < 0 || disk_index >= storage_disk_count || !buffer)
    return -1;

  if (storage_disks[disk_index].write_fn) {
    return storage_disks[disk_index].write_fn(
        lba, 1, buffer, storage_disks[disk_index].backend_ctx);
  }

  switch (storage_disks[disk_index].kind) {
#if defined(ARCH_X86_64) || defined(ARCH_X86)
  case STORAGE_KIND_IDE:
    return storage_ide_write_sector(&storage_disks[disk_index], lba, buffer);
#endif
  default:
    return -1;
  }
}

static int storage_fixup_bios_boot_disk(int disk_index) {
  uint8_t sector[STORAGE_SECTOR_SIZE];
  uint64_t disk_sectors;
  int first_present = -1;
  int active_entry = -1;
  int present_count = 0;
  uint32_t first_start_lba = 0;

  if (disk_index < 0 || disk_index >= storage_disk_count)
    return -1;

  if (storage_disk_read_sector(disk_index, 0, sector) != 0)
    return -1;
  if (sector[STORAGE_MBR_SIGNATURE_OFFSET] != 0x55 ||
      sector[STORAGE_MBR_SIGNATURE_OFFSET + 1] != 0xAA) {
    return 0;
  }

  disk_sectors = (uint64_t)storage_disks[disk_index].capacity_mib * 2048ULL;

  for (int entry = 0; entry < 4; entry++) {
    uint8_t *mbr_entry = &sector[STORAGE_MBR_PARTITION_OFFSET + entry * 16];
    uint8_t status = mbr_entry[0];
    uint8_t type = mbr_entry[4];
    uint32_t start_lba = storage_read_le32(&mbr_entry[8]);
    uint32_t sector_count = storage_read_le32(&mbr_entry[12]);

    if (type == 0 || sector_count == 0)
      continue;

    if (first_present < 0) {
      first_present = entry;
      first_start_lba = start_lba;
    }
    if (status == 0x80)
      active_entry = entry;
    present_count++;
  }

  if (first_present < 0)
    return 0;
  if (active_entry < 0)
    active_entry = first_present;

  if (present_count == 1 && first_start_lba > 0 && disk_sectors > first_start_lba) {
    uint8_t *mbr_entry =
        &sector[STORAGE_MBR_PARTITION_OFFSET + first_present * 16];
    uint32_t sector_count = storage_read_le32(&mbr_entry[12]);
    uint64_t max_partition_sectors = disk_sectors - (uint64_t)first_start_lba;
    if (max_partition_sectors > 0xFFFFFFFFULL)
      max_partition_sectors = 0xFFFFFFFFULL;
    if (max_partition_sectors > sector_count) {
      storage_write_le32(&mbr_entry[12], (uint32_t)max_partition_sectors);
    }
  }

  for (int entry = 0; entry < 4; entry++) {
    uint8_t *mbr_entry = &sector[STORAGE_MBR_PARTITION_OFFSET + entry * 16];
    uint8_t type = mbr_entry[4];
    uint32_t sector_count = storage_read_le32(&mbr_entry[12]);

    if (type == 0 || sector_count == 0) {
      mbr_entry[0] = 0x00;
      continue;
    }
    mbr_entry[0] = (entry == active_entry) ? 0x80 : 0x00;
  }

  if (storage_disk_write_sector(disk_index, 0, sector) != 0)
    return -1;

  storage_load_mbr_partitions(disk_index);
  return 0;
}

int storage_write_disk_image(int disk_index, const uint8_t *data, size_t size) {
  uint8_t sector[STORAGE_SECTOR_SIZE];
  uint64_t disk_sectors;
  uint64_t image_sectors;

  if (disk_index < 0 || disk_index >= storage_disk_count || !data || size == 0)
    return -1;
  if (storage_disks[disk_index].kind == STORAGE_KIND_CDROM)
    return -1;

  disk_sectors = (uint64_t)storage_disks[disk_index].capacity_mib * 2048ULL;
  image_sectors = ((uint64_t)size + (STORAGE_SECTOR_SIZE - 1)) /
                  STORAGE_SECTOR_SIZE;
  if (image_sectors > disk_sectors)
    return -1;

  for (uint64_t sector_index = 0; sector_index < image_sectors; sector_index++) {
    size_t src_offset = (size_t)(sector_index * STORAGE_SECTOR_SIZE);
    size_t remaining = size - src_offset;
    size_t chunk = remaining > STORAGE_SECTOR_SIZE ? STORAGE_SECTOR_SIZE : remaining;

    for (size_t i = 0; i < STORAGE_SECTOR_SIZE; i++)
      sector[i] = 0;
    for (size_t i = 0; i < chunk; i++)
      sector[i] = data[src_offset + i];

    if (storage_disk_write_sector(disk_index, (uint32_t)sector_index, sector) != 0)
      return -1;
  }

  if (storage_fixup_bios_boot_disk(disk_index) != 0)
    return -1;

  return 0;
}

int storage_write_disk_image_file(int disk_index, const char *path) {
  struct file *file;
  uint8_t sector[STORAGE_SECTOR_SIZE];
  uint64_t disk_sectors;
  uint64_t image_sectors;
  loff_t file_size;

  if (disk_index < 0 || disk_index >= storage_disk_count || !path || !path[0])
    return -1;
  if (storage_disks[disk_index].kind == STORAGE_KIND_CDROM)
    return -1;

  file = vfs_open(path, O_RDONLY, 0);
  if (!file)
    return -1;

  file_size = vfs_lseek(file, 0, SEEK_END);
  if (file_size <= 0) {
    vfs_close(file);
    return -1;
  }
  if (vfs_lseek(file, 0, SEEK_SET) < 0) {
    vfs_close(file);
    return -1;
  }

  disk_sectors = (uint64_t)storage_disks[disk_index].capacity_mib * 2048ULL;
  image_sectors = ((uint64_t)file_size + (STORAGE_SECTOR_SIZE - 1)) /
                  STORAGE_SECTOR_SIZE;
  if (image_sectors > disk_sectors) {
    vfs_close(file);
    return -1;
  }

  for (uint64_t sector_index = 0; sector_index < image_sectors; sector_index++) {
    ssize_t bytes_read = vfs_read(file, (char *)sector, STORAGE_SECTOR_SIZE);
    if (bytes_read < 0) {
      vfs_close(file);
      return -1;
    }
    if (bytes_read == 0) {
      vfs_close(file);
      return -1;
    }
    for (size_t i = (size_t)bytes_read; i < STORAGE_SECTOR_SIZE; i++)
      sector[i] = 0;
    if (storage_disk_write_sector(disk_index, (uint32_t)sector_index, sector) != 0) {
      vfs_close(file);
      return -1;
    }
  }

  vfs_close(file);
  if (storage_fixup_bios_boot_disk(disk_index) != 0)
    return -1;
  return 0;
}

static void storage_recompute_partition_layout(int disk_index) {
  uint32_t next_lba = 2048;

  if (disk_index < 0 || disk_index >= storage_disk_count)
    return;

  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    if (!storage_partitions[disk_index][i].present) {
      storage_partitions[disk_index][i].start_lba = 0;
      storage_partitions[disk_index][i].sector_count = 0;
      continue;
    }
    storage_partitions[disk_index][i].start_lba = next_lba;
    storage_partitions[disk_index][i].sector_count =
        storage_mib_to_sectors(storage_partitions[disk_index][i].size_mib);
    next_lba += storage_partitions[disk_index][i].sector_count;
  }
}

static int storage_commit_mbr_partitions(int disk_index) {
  uint8_t sector[STORAGE_SECTOR_SIZE];
  int present_count = 0;
  int active_slot = -1;

  if (disk_index < 0 || disk_index >= storage_disk_count)
    return -1;

  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    if (storage_partitions[disk_index][i].present)
      present_count++;
  }
  if (present_count > 4)
    return -1;

  storage_recompute_partition_layout(disk_index);
  if (storage_disk_read_sector(disk_index, 0, sector) != 0) {
    for (int i = 0; i < STORAGE_SECTOR_SIZE; i++)
      sector[i] = 0;
  }

  /* Keep the existing MBR bootstrap code intact and only rewrite the
   * partition table entries plus the signature. This prevents post-install
   * partition edits from erasing the already-installed BIOS boot sector. */
  for (int i = STORAGE_MBR_PARTITION_OFFSET;
       i < STORAGE_MBR_SIGNATURE_OFFSET; i++)
    sector[i] = 0;

  /* Legacy BIOS/MBR boot should point at the system/update partition when
   * available. UEFI uses the EFI system partition type and copied BOOTX64.EFI
   * payload, so it does not need the active flag. */
  for (int i = 0, entry = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    if (!storage_partitions[disk_index][i].present)
      continue;
    if (active_slot < 0 &&
        storage_partitions[disk_index][i].kind == STORAGE_PARTITION_SYSTEM) {
      active_slot = entry;
      break;
    }
    entry++;
  }
  if (active_slot < 0) {
    for (int i = 0, entry = 0; i < STORAGE_MAX_PARTITIONS; i++) {
      if (!storage_partitions[disk_index][i].present)
        continue;
      if (storage_partitions[disk_index][i].kind == STORAGE_PARTITION_EFI) {
        active_slot = entry;
        break;
      }
      entry++;
    }
  }

  for (int i = 0, entry = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    uint8_t *mbr_entry;
    storage_partition_t *part;

    if (!storage_partitions[disk_index][i].present)
      continue;
    part = &storage_partitions[disk_index][i];
    mbr_entry = &sector[STORAGE_MBR_PARTITION_OFFSET + entry * 16];
    mbr_entry[0] = (entry == active_slot) ? 0x80 : 0x00;
    mbr_entry[1] = 0xFE;
    mbr_entry[2] = 0xFF;
    mbr_entry[3] = 0xFF;
    mbr_entry[4] = storage_partition_mbr_type(part->kind);
    mbr_entry[5] = 0xFE;
    mbr_entry[6] = 0xFF;
    mbr_entry[7] = 0xFF;
    storage_write_le32(&mbr_entry[8], part->start_lba);
    storage_write_le32(&mbr_entry[12], part->sector_count);
    entry++;
  }

  sector[STORAGE_MBR_SIGNATURE_OFFSET] = 0x55;
  sector[STORAGE_MBR_SIGNATURE_OFFSET + 1] = 0xAA;
  return storage_disk_write_sector(disk_index, 0, sector);
}

static void storage_load_mbr_partitions(int disk_index) {
  uint8_t sector[STORAGE_SECTOR_SIZE];
  int part_slot = 0;

  if (storage_disk_read_sector(disk_index, 0, sector) != 0)
    return;
  if (sector[STORAGE_MBR_SIGNATURE_OFFSET] != 0x55 ||
      sector[STORAGE_MBR_SIGNATURE_OFFSET + 1] != 0xAA)
    return;

  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    storage_partitions[disk_index][i].present = 0;
    storage_partitions[disk_index][i].kind = STORAGE_PARTITION_UNKNOWN;
    storage_partitions[disk_index][i].size_mib = 0;
    storage_partitions[disk_index][i].start_lba = 0;
    storage_partitions[disk_index][i].sector_count = 0;
    storage_partitions[disk_index][i].label[0] = '\0';
  }

  for (int entry = 0; entry < 4 && part_slot < STORAGE_MAX_PARTITIONS; entry++) {
    const uint8_t *mbr_entry = &sector[STORAGE_MBR_PARTITION_OFFSET + entry * 16];
    uint8_t type = mbr_entry[4];
    uint32_t start_lba = storage_read_le32(&mbr_entry[8]);
    uint32_t sector_count = storage_read_le32(&mbr_entry[12]);
    storage_partition_kind_t kind = STORAGE_PARTITION_UNKNOWN;
    uint32_t size_mib;

    if (type == 0 || sector_count == 0)
      continue;
    if (type == 0xEF)
      kind = STORAGE_PARTITION_EFI;
    else if (type == 0x0B || type == 0x0C || type == 0x0E)
      kind = STORAGE_PARTITION_SYSTEM;
    else if (type == 0x82)
      kind = STORAGE_PARTITION_SWAP;
    else if (type == 0x83)
      kind = (part_slot == 0 && start_lba <= 4096) ? STORAGE_PARTITION_SYSTEM
                                                    : STORAGE_PARTITION_DATA;
    else
      kind = STORAGE_PARTITION_DATA;

    size_mib = sector_count / 2048U;
    if (size_mib == 0)
      size_mib = 1;

    storage_partitions[disk_index][part_slot].present = 1;
    storage_partitions[disk_index][part_slot].kind = kind;
    storage_partitions[disk_index][part_slot].size_mib = size_mib;
    storage_partitions[disk_index][part_slot].start_lba = start_lba;
    storage_partitions[disk_index][part_slot].sector_count = sector_count;
    storage_default_partition_label(storage_partitions[disk_index][part_slot].label,
                                    sizeof(storage_partitions[disk_index][part_slot].label),
                                    kind, part_slot);
    part_slot++;
  }
}

static void storage_load_pci_driver(storage_kind_t kind, int controller_index,
                                    pci_device_t *dev) {
  switch (kind) {
  case STORAGE_KIND_IDE:
    printk(KERN_INFO "STORAGE: Loading IDE driver for %02x:%02x.%x\n", dev->bus,
           dev->slot, dev->func);
    storage_probe_ide_controller(controller_index);
    break;
  case STORAGE_KIND_AHCI:
  case STORAGE_KIND_SATA:
    printk(KERN_INFO "STORAGE: Loading AHCI driver for %02x:%02x.%x\n",
           dev->bus, dev->slot, dev->func);
    pci_enable_device(dev);
    storage_probe_ahci_controller(controller_index, dev);
    break;
  case STORAGE_KIND_NVME:
    printk(KERN_INFO "STORAGE: Loading NVMe driver for %02x:%02x.%x\n",
           dev->bus, dev->slot, dev->func);
    pci_enable_device(dev);
    storage_probe_nvme_controller(controller_index, dev);
    break;
  default:
    break;
  }
}

void storage_init(void) {
  if (storage_initialized)
    return;

  storage_initialized = 1;
  storage_controller_count = 0;
  storage_disk_count = 0;
  for (int i = 0; i <= STORAGE_KIND_APPLE_ANS; i++)
    storage_kind_counts[i] = 0;

#ifdef ARCH_ARM64
  extern int ans_nvme_init(void);
  if (ans_nvme_init() == 0) {
    storage_register_platform_controller("Apple ANS NVMe",
                                         STORAGE_KIND_APPLE_ANS, "platform");
  }
#endif
}

void storage_register_pci_controller(pci_device_t *dev) {
  storage_kind_t kind;
  const char *name;
  int existing_index;
  int planned_index;
  int controller_index;

  if (!storage_initialized)
    storage_init();
  if (!dev)
    return;

  kind = storage_classify_pci(dev);
  if (kind == STORAGE_KIND_UNKNOWN)
    return;

  name = storage_kind_name(kind);
  existing_index =
      storage_find_controller_index(kind, dev->bus, dev->slot, dev->func, name);
  if (existing_index >= 0) {
    controller_index = existing_index;
  } else {
    if (storage_controller_count >= STORAGE_MAX_CONTROLLERS)
      return;
    planned_index = storage_controller_count;
    storage_load_pci_driver(kind, planned_index, dev);
    controller_index = storage_record_controller(kind, dev->vendor_id,
                                                 dev->device_id, dev->bus,
                                                 dev->slot, dev->func, name,
                                                 "pci");
    return;
  }
  if (controller_index < 0)
    return;
  storage_load_pci_driver(kind, controller_index, dev);
}

void storage_register_platform_controller(const char *name, storage_kind_t kind,
                                          const char *bus_name) {
  if (!storage_initialized)
    storage_init();

  (void)storage_record_controller(kind, 0, 0, 0xFF, 0xFF, 0xFF, name,
                                  bus_name);
}

void storage_register_disk_device(const char *name, storage_kind_t kind,
                                  const char *location) {
  if (!storage_initialized)
    storage_init();

  storage_record_disk(kind, 0xFF, storage_disk_count, name, location);
}

int storage_register_disk_backend(const char *location,
                                  storage_disk_read_fn_t read_fn,
                                  storage_disk_write_fn_t write_fn,
                                  void *ctx) {
  int disk_index;

  if (!storage_initialized)
    storage_init();

  disk_index = storage_find_disk_by_location(location);
  if (disk_index < 0)
    return -1;

  storage_disks[disk_index].read_fn = read_fn;
  storage_disks[disk_index].write_fn = write_fn;
  storage_disks[disk_index].backend_ctx = ctx;
  if (read_fn && storage_disks[disk_index].kind != STORAGE_KIND_CDROM)
    storage_load_mbr_partitions(disk_index);
  return 0;
}

int storage_disk_supports_partition_writes(int disk_index) {
  if (disk_index < 0 || disk_index >= storage_disk_count)
    return 0;
  if (storage_disks[disk_index].kind == STORAGE_KIND_CDROM)
    return 0;
  if (storage_disks[disk_index].write_fn)
    return 1;
  return storage_disks[disk_index].kind == STORAGE_KIND_IDE;
}

int storage_get_controller_count(void) { return storage_controller_count; }

int storage_get_disk_count(void) { return storage_disk_count; }

int storage_get_disk_kind(int index) {
  if (index < 0 || index >= storage_disk_count)
    return STORAGE_KIND_UNKNOWN;
  return storage_disks[index].kind;
}

int storage_disk_is_removable(int index) {
  if (index < 0 || index >= storage_disk_count)
    return 0;
  return storage_disks[index].kind == STORAGE_KIND_USB_MASS_STORAGE ||
         storage_disks[index].kind == STORAGE_KIND_CDROM;
}

uint32_t storage_get_disk_capacity_mib(int index) {
  if (index < 0 || index >= storage_disk_count)
    return 0;
  return storage_disks[index].capacity_mib;
}

int storage_get_disk_location(int index, char *buf, int max) {
  if (!buf || max <= 0 || index < 0 || index >= storage_disk_count)
    return -1;
  storage_copy_string(buf, storage_disks[index].location, max);
  return 0;
}

int storage_get_disk_index_by_location(const char *location) {
  if (!location)
    return -1;
  return storage_find_disk_by_location(location);
}

int storage_read_block(int disk_index, uint32_t lba, void *buffer,
                       uint32_t block_size) {
  if (disk_index < 0 || disk_index >= storage_disk_count || !buffer)
    return -1;

  if (block_size == 512)
    return storage_disk_read_sector(disk_index, lba, buffer);

  if (block_size == 2048 && storage_disks[disk_index].kind == STORAGE_KIND_CDROM) {
    if (storage_disks[disk_index].read_fn) {
      return storage_disks[disk_index].read_fn(lba, 1, buffer,
                                               storage_disks[disk_index].backend_ctx);
    }
#if defined(ARCH_X86_64) || defined(ARCH_X86)
    return storage_ide_read_atapi_block(&storage_disks[disk_index], lba, buffer);
#else
    return -1;
#endif
  }

  return -1;
}

int storage_write_block(int disk_index, uint32_t lba, const void *buffer,
                        uint32_t block_size) {
  if (disk_index < 0 || disk_index >= storage_disk_count || !buffer)
    return -1;

  if (block_size == 512)
    return storage_disk_write_sector(disk_index, lba, (void *)buffer);

  return -1;
}

int storage_get_kind_count(storage_kind_t kind) {
  if (kind <= STORAGE_KIND_UNKNOWN || kind > STORAGE_KIND_APPLE_ANS)
    return 0;
  return storage_kind_counts[kind];
}

int storage_describe_controller(int index, char *buf, int max) {
  storage_controller_t *ctrl;

  if (!buf || max <= 0 || index < 0 || index >= storage_controller_count)
    return -1;

  ctrl = &storage_controllers[index];
  buf[0] = '\0';
  storage_append_string(buf, max, ctrl->name);
  storage_append_string(buf, max, " [");
  storage_append_string(buf, max, ctrl->bus_name);
  if (ctrl->bus != 0xFF) {
    storage_append_string(buf, max, " ");
    storage_append_decimal(buf, max, ctrl->bus);
    storage_append_string(buf, max, ":");
    storage_append_decimal(buf, max, ctrl->slot);
    storage_append_string(buf, max, ".");
    storage_append_decimal(buf, max, ctrl->func);
  }
  storage_append_string(buf, max, "]");
  return 0;
}

void storage_build_overview(char *buf, int max) {
  int total;

  if (!buf || max <= 0)
    return;

  buf[0] = '\0';
  total = storage_controller_count;
  if (total == 0) {
    storage_append_string(buf, max, "No storage controllers detected");
    return;
  }

  storage_append_decimal(buf, max, total);
  storage_append_string(buf, max, " controller");
  if (total != 1)
    storage_append_string(buf, max, "s");

  if (storage_get_kind_count(STORAGE_KIND_NVME) > 0 ||
      storage_get_kind_count(STORAGE_KIND_APPLE_ANS) > 0) {
    storage_append_string(buf, max, "  NVMe:");
    storage_append_decimal(buf, max,
                           storage_get_kind_count(STORAGE_KIND_NVME) +
                               storage_get_kind_count(STORAGE_KIND_APPLE_ANS));
  }
  if (storage_get_kind_count(STORAGE_KIND_AHCI) > 0 ||
      storage_get_kind_count(STORAGE_KIND_SATA) > 0) {
    storage_append_string(buf, max, "  SATA:");
    storage_append_decimal(buf, max,
                           storage_get_kind_count(STORAGE_KIND_AHCI) +
                               storage_get_kind_count(STORAGE_KIND_SATA));
  }
  if (storage_get_kind_count(STORAGE_KIND_IDE) > 0) {
    storage_append_string(buf, max, "  IDE:");
    storage_append_decimal(buf, max, storage_get_kind_count(STORAGE_KIND_IDE));
  }
  if (storage_get_kind_count(STORAGE_KIND_CDROM) > 0) {
    storage_append_string(buf, max, "  CD:");
    storage_append_decimal(buf, max, storage_get_kind_count(STORAGE_KIND_CDROM));
  }
  if (storage_get_kind_count(STORAGE_KIND_USB_MASS_STORAGE) > 0) {
    storage_append_string(buf, max, "  USB:");
    storage_append_decimal(buf, max,
                           storage_get_kind_count(STORAGE_KIND_USB_MASS_STORAGE));
  }
  if (storage_get_kind_count(STORAGE_KIND_RAID) > 0) {
    storage_append_string(buf, max, "  RAID:");
    storage_append_decimal(buf, max, storage_get_kind_count(STORAGE_KIND_RAID));
  }
}

int storage_describe_disk(int index, char *buf, int max) {
  storage_disk_t *disk;
  uint32_t free_mib;

  if (!buf || max <= 0 || index < 0 || index >= storage_disk_count)
    return -1;

  disk = &storage_disks[index];
  if (disk->kind == STORAGE_KIND_CDROM) {
    buf[0] = '\0';
    storage_append_string(buf, max, disk->name);
    storage_append_string(buf, max, " [");
    storage_append_string(buf, max, disk->location);
    storage_append_string(buf, max, "] ");
    storage_append_decimal(buf, max, (int)disk->capacity_mib);
    storage_append_string(buf, max, " MiB optical media, read-only");
    return 0;
  }
  if (disk->kind == STORAGE_KIND_USB_MASS_STORAGE) {
    free_mib = disk->capacity_mib > storage_partition_used_mib(index)
                   ? disk->capacity_mib - storage_partition_used_mib(index)
                   : 0;
    buf[0] = '\0';
    storage_append_string(buf, max, disk->name);
    storage_append_string(buf, max, " [");
    storage_append_string(buf, max, disk->location);
    storage_append_string(buf, max, "] ");
    storage_append_decimal(buf, max, (int)disk->capacity_mib);
    storage_append_string(buf, max, " MiB USB flash drive");
    if (disk->read_fn)
      storage_append_string(buf, max, ", readable");
    else
      storage_append_string(buf, max, ", backend pending");
    if (free_mib > 0) {
      storage_append_string(buf, max, ", free ");
      storage_append_decimal(buf, max, (int)free_mib);
      storage_append_string(buf, max, " MiB");
    }
    return 0;
  }
  free_mib = disk->capacity_mib > storage_partition_used_mib(index)
                 ? disk->capacity_mib - storage_partition_used_mib(index)
                 : 0;
  buf[0] = '\0';
  storage_append_string(buf, max, disk->name);
  storage_append_string(buf, max, " [");
  storage_append_string(buf, max, disk->location);
  storage_append_string(buf, max, "] ");
  storage_append_decimal(buf, max, (int)disk->capacity_mib);
  storage_append_string(buf, max, " MiB total, ");
  storage_append_decimal(buf, max, (int)free_mib);
  storage_append_string(buf, max, " MiB free");
  return 0;
}

void storage_build_disk_overview(char *buf, int max) {
  if (!buf || max <= 0)
    return;

  buf[0] = '\0';
  if (storage_disk_count == 0) {
    storage_append_string(buf, max, "No storage media registered");
    return;
  }

  storage_append_decimal(buf, max, storage_disk_count);
  storage_append_string(buf, max, " storage device");
  if (storage_disk_count != 1)
    storage_append_string(buf, max, "s");
  storage_append_string(buf, max, " ready");
}

int storage_get_partition_count(int disk_index) {
  int count = 0;
  if (disk_index < 0 || disk_index >= storage_disk_count)
    return 0;
  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    if (storage_partitions[disk_index][i].present)
      count++;
  }
  return count;
}

int storage_count_partitions_of_kind(int disk_index,
                                     storage_partition_kind_t kind) {
  int count = 0;

  if (disk_index < 0 || disk_index >= storage_disk_count)
    return 0;

  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    if (storage_partitions[disk_index][i].present &&
        storage_partitions[disk_index][i].kind == kind) {
      count++;
    }
  }

  return count;
}

int storage_describe_partition(int disk_index, int partition_index, char *buf,
                               int max) {
  int seen = 0;
  storage_partition_t *part = NULL;

  if (!buf || max <= 0 || disk_index < 0 || disk_index >= storage_disk_count)
    return -1;

  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    if (!storage_partitions[disk_index][i].present)
      continue;
    if (seen == partition_index) {
      part = &storage_partitions[disk_index][i];
      break;
    }
    seen++;
  }

  if (!part)
    return -1;

  buf[0] = '\0';
  storage_append_string(buf, max, part->label);
  storage_append_string(buf, max, " (");
  storage_append_string(buf, max, storage_partition_kind_name(part->kind));
  storage_append_string(buf, max, ", ");
  storage_append_decimal(buf, max, (int)part->size_mib);
  storage_append_string(buf, max, " MiB)");
  return 0;
}

int storage_get_partition_info(int disk_index, int partition_index,
                               storage_partition_kind_t *kind, char *label,
                               int label_max, uint32_t *start_lba,
                               uint32_t *sector_count) {
  storage_partition_t *part = NULL;
  int visible_index = 0;

  if (disk_index < 0 || disk_index >= storage_disk_count)
    return -1;

  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    if (!storage_partitions[disk_index][i].present)
      continue;
    if (visible_index == partition_index) {
      part = &storage_partitions[disk_index][i];
      break;
    }
    visible_index++;
  }

  if (!part)
    return -1;

  if (kind)
    *kind = part->kind;
  if (label && label_max > 0)
    storage_copy_string(label, part->label, label_max);
  if (start_lba)
    *start_lba = part->start_lba;
  if (sector_count)
    *sector_count = part->sector_count;
  return 0;
}

int storage_create_partition(int disk_index, storage_partition_kind_t kind,
                             uint32_t size_mib) {
  int slot;
  int ordinal = 0;
  char label[32];
  storage_partition_t old_parts[STORAGE_MAX_PARTITIONS];

  if (disk_index < 0 || disk_index >= storage_disk_count)
    return -1;
  if (size_mib == 0)
    return -1;
  if (storage_partition_used_mib(disk_index) + size_mib >
      storage_disks[disk_index].capacity_mib)
    return -1;

  slot = storage_find_free_partition_slot(disk_index);
  if (slot < 0)
    return -1;

  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++)
    old_parts[i] = storage_partitions[disk_index][i];

  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    if (storage_partitions[disk_index][i].present &&
        storage_partitions[disk_index][i].kind == kind) {
      ordinal++;
    }
  }

  storage_default_partition_label(label, sizeof(label), kind, ordinal);
  storage_partitions[disk_index][slot].present = 1;
  storage_partitions[disk_index][slot].kind = kind;
  storage_partitions[disk_index][slot].size_mib = size_mib;
  storage_copy_string(storage_partitions[disk_index][slot].label, label,
                      sizeof(storage_partitions[disk_index][slot].label));
  if (storage_commit_mbr_partitions(disk_index) != 0) {
    for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++)
      storage_partitions[disk_index][i] = old_parts[i];
    return -1;
  }
  printk(KERN_INFO "STORAGE: Created %s partition on disk %s (%u MiB)\n",
         storage_partition_kind_name(kind), storage_disks[disk_index].location,
         size_mib);
  return 0;
}

int storage_update_partition(int disk_index, int partition_index,
                             storage_partition_kind_t kind,
                             uint32_t size_mib) {
  int seen = 0;
  storage_partition_t *part = NULL;
  uint32_t used_without_part;
  storage_partition_t old_parts[STORAGE_MAX_PARTITIONS];

  if (disk_index < 0 || disk_index >= storage_disk_count || size_mib == 0)
    return -1;

  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++)
    old_parts[i] = storage_partitions[disk_index][i];

  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    if (!storage_partitions[disk_index][i].present)
      continue;
    if (seen == partition_index) {
      part = &storage_partitions[disk_index][i];
      break;
    }
    seen++;
  }
  if (!part)
    return -1;

  used_without_part = storage_partition_used_mib(disk_index) - part->size_mib;
  if (used_without_part + size_mib > storage_disks[disk_index].capacity_mib)
    return -1;

  part->kind = kind;
  part->size_mib = size_mib;
  storage_default_partition_label(part->label, sizeof(part->label), kind,
                                  partition_index);
  if (storage_commit_mbr_partitions(disk_index) != 0) {
    for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++)
      storage_partitions[disk_index][i] = old_parts[i];
    return -1;
  }
  printk(KERN_INFO "STORAGE: Updated partition %d on disk %s\n", partition_index,
         storage_disks[disk_index].location);
  return 0;
}

int storage_delete_partition(int disk_index, int partition_index) {
  int seen = 0;
  storage_partition_t old_parts[STORAGE_MAX_PARTITIONS];

  if (disk_index < 0 || disk_index >= storage_disk_count)
    return -1;

  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++)
    old_parts[i] = storage_partitions[disk_index][i];

  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    if (!storage_partitions[disk_index][i].present)
      continue;
    if (seen == partition_index) {
      storage_partitions[disk_index][i].present = 0;
      storage_partitions[disk_index][i].kind = STORAGE_PARTITION_UNKNOWN;
      storage_partitions[disk_index][i].size_mib = 0;
      storage_partitions[disk_index][i].start_lba = 0;
      storage_partitions[disk_index][i].sector_count = 0;
      storage_partitions[disk_index][i].label[0] = '\0';
      if (storage_commit_mbr_partitions(disk_index) != 0) {
        for (int j = 0; j < STORAGE_MAX_PARTITIONS; j++)
          storage_partitions[disk_index][j] = old_parts[j];
        return -1;
      }
      printk(KERN_INFO "STORAGE: Deleted partition %d on disk %s\n",
             partition_index, storage_disks[disk_index].location);
      return 0;
    }
    seen++;
  }
  return -1;
}

int storage_has_efi_partition(int disk_index) {
  if (disk_index < 0 || disk_index >= storage_disk_count)
    return 0;
  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    if (storage_partitions[disk_index][i].present &&
        storage_partitions[disk_index][i].kind == STORAGE_PARTITION_EFI)
      return 1;
  }
  return 0;
}

int storage_ensure_install_partitions(int disk_index) {
  int changed = 0;
  int has_system = 0;
  int has_data = 0;
  uint32_t free_mib;

  if (disk_index < 0 || disk_index >= storage_disk_count)
    return -1;

  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    if (storage_partitions[disk_index][i].present &&
        storage_partitions[disk_index][i].kind == STORAGE_PARTITION_SYSTEM) {
      has_system = 1;
    }
    if (storage_partitions[disk_index][i].present &&
        storage_partitions[disk_index][i].kind == STORAGE_PARTITION_DATA) {
      has_data = 1;
    }
  }

  if (!storage_has_efi_partition(disk_index)) {
    if (storage_create_partition(disk_index, STORAGE_PARTITION_EFI, 256) == 0)
      changed++;
  }

  if (!has_system) {
    uint32_t system_size = storage_disks[disk_index].capacity_mib / 2;
    if (system_size < 8192)
      system_size = 8192;
    if (system_size > 65536)
      system_size = 65536;
    if (storage_create_partition(disk_index, STORAGE_PARTITION_SYSTEM,
                                 system_size) == 0)
      changed++;
  }

  free_mib = storage_disks[disk_index].capacity_mib >
                     storage_partition_used_mib(disk_index)
                 ? storage_disks[disk_index].capacity_mib -
                       storage_partition_used_mib(disk_index)
                 : 0;
  if (!has_data && free_mib >= 4096) {
    uint32_t data_size = free_mib;
    if (data_size > 65536)
      data_size = 65536;
    if (storage_create_partition(disk_index, STORAGE_PARTITION_DATA,
                                 data_size) == 0)
      changed++;
  }

  return changed;
}

int storage_prepare_user_partition(int disk_index) {
  int has_data = 0;
  int visible_index = 0;
  int system_partition_index = -1;
  int system_partition_slot = -1;
  int present_count = 0;
  uint32_t free_mib;

  if (disk_index < 0 || disk_index >= storage_disk_count)
    return -1;

  for (int i = 0; i < STORAGE_MAX_PARTITIONS; i++) {
    if (!storage_partitions[disk_index][i].present)
      continue;
    present_count++;
    if (storage_partitions[disk_index][i].kind == STORAGE_PARTITION_DATA)
      has_data = 1;
    if (storage_partitions[disk_index][i].kind == STORAGE_PARTITION_SYSTEM &&
        system_partition_index < 0) {
      system_partition_index = visible_index;
      system_partition_slot = i;
    }
    visible_index++;
  }

  if (has_data)
    return 0;

  free_mib = storage_disks[disk_index].capacity_mib >
                     storage_partition_used_mib(disk_index)
                 ? storage_disks[disk_index].capacity_mib -
                       storage_partition_used_mib(disk_index)
                 : 0;
  if (free_mib >= 4096) {
    uint32_t data_size = free_mib;
    if (data_size > 32768)
      data_size = 32768;
    if (storage_create_partition(disk_index, STORAGE_PARTITION_DATA,
                                 data_size) != 0)
      return -1;
    printk(KERN_INFO
           "STORAGE: Added user data partition on disk %s (%u MiB free space)\n",
           storage_disks[disk_index].location, data_size);
    return 1;
  }

  if (present_count == 1 && system_partition_index >= 0) {
    uint32_t original_system_size =
        storage_partitions[disk_index][system_partition_slot].size_mib;
    uint32_t data_size = original_system_size / 4;
    uint32_t new_system_size;

    if (data_size < 4096)
      data_size = 4096;
    if (data_size > 16384)
      data_size = 16384;
    if (original_system_size <= data_size + 8192)
      return 0;

    new_system_size = original_system_size - data_size;
    if (new_system_size < 8192)
      return 0;

    if (storage_update_partition(disk_index, system_partition_index,
                                 STORAGE_PARTITION_SYSTEM,
                                 new_system_size) != 0)
      return -1;
    if (storage_create_partition(disk_index, STORAGE_PARTITION_DATA,
                                 data_size) != 0) {
      storage_update_partition(disk_index, system_partition_index,
                               STORAGE_PARTITION_SYSTEM,
                               original_system_size);
      return -1;
    }
    printk(KERN_INFO
           "STORAGE: Split disk %s into system (%u MiB) and user data (%u MiB)\n",
           storage_disks[disk_index].location, new_system_size, data_size);
    return 1;
  }

  return 0;
}
