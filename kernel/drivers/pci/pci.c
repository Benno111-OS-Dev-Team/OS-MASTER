/*
 * OS8 - PCI Driver Implementation
 *
 * Scans ECAM to find devices. Supports virtio-gpu and Intel HDA.
 */

#include "drivers/pci.h"
#include "drivers/intel_gfx.h"
#include "drivers/storage.h"
#include "drivers/intel_hda.h"
#include "drivers/usb/usb.h"
#include "bootmanager.h"
#include "arch/arch.h"
#include "printk.h"
#include "types.h"

/* Device list */
static pci_device_t device_pool[64];
static int device_count = 0;
static pci_device_t *device_list = NULL;
static int pci_generic_drivers_only = 0;

/* MMIO allocation for unassigned BARs */
static uint64_t next_mmio_base = 0x10000000;

static int pci_is_xhci_controller(const pci_device_t *pci_dev) {
  if (!pci_dev)
    return 0;
  return pci_dev->class_code == 0x0C && pci_dev->subclass == 0x03 &&
         pci_dev->prog_if == 0x30;
}

static void pci_try_init_xhci_controller(pci_device_t *pci_dev,
                                         int allow_on_x86) {
  if (!pci_is_xhci_controller(pci_dev))
    return;

  if (pci_generic_drivers_only) {
    printk("PCI: generic driver mode skipping xHCI controller at %02x:%02x.%x\n",
           pci_dev->bus, pci_dev->slot, pci_dev->func);
    return;
  }

  printk("PCI: Found xHCI USB controller at %02x:%02x.%x\n", pci_dev->bus,
         pci_dev->slot, pci_dev->func);

#if defined(ARCH_X86_64) || defined(ARCH_X86)
  if (!allow_on_x86) {
    printk("PCI: xHCI deferred on x86 bring-up path\n");
    return;
  }
#endif

  pci_enable_device(pci_dev);

  if (pci_dev->bar0) {
    printk("PCI: xHCI BAR0=0x%llx, IRQ=%d\n", pci_dev->bar0, pci_dev->irq);
    if (xhci_init(pci_dev->bar0) != 0) {
      printk("PCI: xHCI initialization failed for %02x:%02x.%x\n",
             pci_dev->bus, pci_dev->slot, pci_dev->func);
    }
  } else {
    printk("PCI: xHCI controller missing MMIO BAR0, skipping\n");
  }
}

/* Helper to calculate ECAM address */
/* Bus 8 bits, Device 5 bits, Function 3 bits, Offset 12 bits */
static volatile uint32_t *pci_ecam_addr(uint8_t bus, uint8_t slot, uint8_t func,
                                        uint8_t offset) {
  uint64_t addr = PCI_ECAM_BASE | ((uint64_t)bus << 20) |
                  ((uint64_t)slot << 15) | ((uint64_t)func << 12) |
                  (offset & 0xFFF);
  return (volatile uint32_t *)addr;
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
#if defined(ARCH_X86_64) || defined(ARCH_X86)
  uint32_t address = 0x80000000U | ((uint32_t)bus << 16) |
                     ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                     (offset & 0xFC);
  outl(0xCF8, address);
  return inl(0xCFC);
#else
  volatile uint32_t *addr = pci_ecam_addr(bus, slot, func, offset);
  return *addr;
#endif
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                 uint32_t value) {
#if defined(ARCH_X86_64) || defined(ARCH_X86)
  uint32_t address = 0x80000000U | ((uint32_t)bus << 16) |
                     ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                     (offset & 0xFC);
  outl(0xCF8, address);
  outl(0xCFC, value);
#else
  volatile uint32_t *addr = pci_ecam_addr(bus, slot, func, offset);
  *addr = value;
#endif
}

/* Find a device by vendor/device ID */
pci_device_t *pci_find_device(uint16_t vendor, uint16_t device) {
  pci_device_t *dev = device_list;
  while (dev) {
    if (dev->vendor_id == vendor && dev->device_id == device) {
      return dev;
    }
    dev = dev->next;
  }
  return NULL;
}

static uint64_t pci_read_bar(uint8_t bus, uint8_t slot, uint8_t func,
                             uint8_t bar_offset) {
  uint32_t bar_raw = pci_read32(bus, slot, func, bar_offset);
  uint64_t addr;

  if (bar_raw == 0 || bar_raw == 0xFFFFFFFF)
    return 0;
  if (bar_raw & 0x1)
    return 0; /* Ignore I/O BARs in this MMIO-only path */

  addr = (uint64_t)(bar_raw & 0xFFFFFFF0U);
  if ((bar_raw & 0x6) == 0x4) {
    uint32_t high = pci_read32(bus, slot, func, (uint8_t)(bar_offset + 4));
    if (high == 0xFFFFFFFF)
      return 0;
    addr |= ((uint64_t)high << 32);
  }

  return addr;
}

/* Allocate BAR if unassigned */
static uint64_t pci_alloc_bar(uint8_t bus, uint8_t slot, uint8_t func,
                              uint8_t bar_offset) {
#if defined(ARCH_X86_64) || defined(ARCH_X86)
  /* Real hardware is sensitive to BAR sizing/probing during bring-up. */
  return pci_read_bar(bus, slot, func, bar_offset);
#else
  uint32_t bar_raw = pci_read32(bus, slot, func, bar_offset);
  uint32_t flags = bar_raw & 0xF;

  /* Check if BAR is already assigned with valid address */
  if ((bar_raw & 0xFFFFFFF0) != 0 && bar_raw != 0xFFFFFFFF) {
    return pci_read_bar(bus, slot, func, bar_offset);
  }

  /* BAR is unassigned - try to size and allocate */
  pci_write32(bus, slot, func, bar_offset, 0xFFFFFFFF);
  uint32_t size_val = pci_read32(bus, slot, func, bar_offset);
  pci_write32(bus, slot, func, bar_offset, bar_raw); /* Restore */

  /* Check if BAR responds to sizing */
  if (size_val == 0 || size_val == 0xFFFFFFFF) {
    return 0; /* BAR not implemented */
  }

  /* Calculate size from response */
  uint32_t size_mask = size_val & 0xFFFFFFF0;
  uint32_t size = (~size_mask) + 1;
  if (size == 0 || size > 0x10000000)
    size = 0x4000; /* Default to 16KB if invalid */

  /* Check if 64-bit BAR */
  bool is_64bit = (flags & 0x4);

  /* Align allocation */
  next_mmio_base = (next_mmio_base + size - 1) & ~(size - 1);
  uint64_t addr = next_mmio_base;

  /* Write new address */
  pci_write32(bus, slot, func, bar_offset, (uint32_t)addr | (flags & 0xF));

  /* Handle 64-bit BAR */
  if (is_64bit) {
    pci_write32(bus, slot, func, bar_offset + 4, (uint32_t)(addr >> 32));
  }

  next_mmio_base += size;

  printk("PCI:   [%02x:%02x.%x] BAR@0x%02x allocated at 0x%llx (size 0x%x)\n",
         bus, slot, func, bar_offset, addr, size);
  return addr;
#endif
}

static void pci_register_device(uint8_t bus, uint8_t slot, uint8_t func) {
  uint32_t vendor_dev = pci_read32(bus, slot, func, PCI_VENDOR_ID);
  uint16_t vendor = vendor_dev & 0xFFFF;
  uint16_t device = (vendor_dev >> 16) & 0xFFFF;

  if (vendor == 0xFFFF || vendor == 0x0000)
    return;

  printk("PCI: Found %04x:%04x at %02x:%02x.%x\n", vendor, device, bus, slot,
         func);

  if (device_count >= (int)(sizeof(device_pool) / sizeof(device_pool[0]))) {
    printk("PCI: Device pool full!\n");
    return;
  }

  pci_device_t *pci_dev = &device_pool[device_count++];
  pci_dev->bus = bus;
  pci_dev->slot = slot;
  pci_dev->func = func;
  pci_dev->vendor_id = vendor;
  pci_dev->device_id = device;

  uint32_t class_rev = pci_read32(bus, slot, func, PCI_CLASS_REV);
  pci_dev->class_code = (class_rev >> 24) & 0xFF;
  pci_dev->subclass = (class_rev >> 16) & 0xFF;
  pci_dev->prog_if = (class_rev >> 8) & 0xFF;

  pci_dev->bar0 = pci_alloc_bar(bus, slot, func, PCI_BAR0);
  pci_dev->bar1 = pci_alloc_bar(bus, slot, func, PCI_BAR1);
  pci_dev->bar2 = pci_alloc_bar(bus, slot, func, PCI_BAR2);

  uint32_t irq_line = pci_read32(bus, slot, func, PCI_INTERRUPT);
  pci_dev->irq = irq_line & 0xFF;

  pci_dev->next = device_list;
  device_list = pci_dev;

  storage_register_pci_controller(pci_dev);

  if (pci_generic_drivers_only && intel_hda_is_device_supported(pci_dev)) {
    printk("PCI: generic driver mode skipping Intel HDA at %02x:%02x.%x\n",
           pci_dev->bus, pci_dev->slot, pci_dev->func);
  } else if (intel_hda_is_device_supported(pci_dev)) {
    printk("PCI: Found Intel HDA Audio Controller!\n");
    printk("PCI: HDA BAR0=0x%llx, IRQ=%d\n", pci_dev->bar0, pci_dev->irq);
    intel_hda_init(pci_dev);
  }

  if (pci_generic_drivers_only && vendor == INTEL_GPU_VENDOR_ID &&
      pci_dev->class_code == 0x03) {
    printk("PCI: generic driver mode skipping Intel integrated graphics at %02x:%02x.%x\n",
           pci_dev->bus, pci_dev->slot, pci_dev->func);
  } else if (vendor == INTEL_GPU_VENDOR_ID && pci_dev->class_code == 0x03) {
    printk("PCI: Found Intel integrated graphics controller\n");
    printk("PCI: Intel GPU BAR0=0x%llx BAR2=0x%llx IRQ=%d\n", pci_dev->bar0,
           pci_dev->bar2, pci_dev->irq);
    intel_gfx_init(pci_dev);
  }

  if (vendor == PCI_VENDOR_VIRTIO && device == PCI_DEVICE_VIRTIO_GPU) {
    if (pci_generic_drivers_only) {
      printk("PCI: generic driver mode skipping virtio-gpu at %02x:%02x.%x\n",
             pci_dev->bus, pci_dev->slot, pci_dev->func);
    } else {
      printk("PCI: Found virtio-gpu device!\n");
      printk("PCI: virtio-gpu BAR0=0x%llx\n", pci_dev->bar0);
    }
  }

  pci_try_init_xhci_controller(pci_dev, 0);
}

void pci_init(void) {
#if defined(ARCH_X86_64) || defined(ARCH_X86)
  int allow_xhci_after_scan = 0;
#endif
#if defined(ARCH_X86_64) || defined(ARCH_X86)
  printk("PCI: Initializing x86 config-space scan...\n");
#else
  printk("PCI: Initializing High ECAM scan at 0x%llx...\n", PCI_ECAM_BASE);
#endif

  device_count = 0;
  device_list = NULL;
  pci_generic_drivers_only = boot_use_generic_drivers_only();
  if (pci_generic_drivers_only) {
    printk("PCI: generic driver mode enabled; vendor-specific PCI drivers will be skipped\n");
  }

  for (int bus = 0; bus < 256; bus++) {
    for (int slot = 0; slot < 32; slot++) {
      uint32_t vendor_dev = pci_read32((uint8_t)bus, (uint8_t)slot, 0, PCI_VENDOR_ID);
      uint16_t vendor = vendor_dev & 0xFFFF;
      uint8_t functions = 1;

      if (vendor == 0xFFFF || vendor == 0x0000)
        continue;

      if (pci_read8((uint8_t)bus, (uint8_t)slot, 0, PCI_HEADER_TYPE) & 0x80)
        functions = 8;

      for (uint8_t func = 0; func < functions; func++) {
        pci_register_device((uint8_t)bus, (uint8_t)slot, func);
      }
    }
  }

#if defined(ARCH_X86_64) || defined(ARCH_X86)
  allow_xhci_after_scan = boot_allow_xhci();
  if (allow_xhci_after_scan) {
    printk("PCI: Enabling xHCI bring-up after full PCI scan (%s)\n",
           "cmdline override");
    for (pci_device_t *dev = device_list; dev; dev = dev->next) {
      pci_try_init_xhci_controller(dev, 1);
    }
  } else {
    printk("PCI: xHCI remains disabled on x86 bring-up path "
           "(set cmdline: xhci=on to enable)\n");
  }
#endif

  printk("PCI: Scan complete (%d devices found).\n", device_count);
}
