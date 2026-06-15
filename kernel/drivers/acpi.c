/*
 * OS8 - Minimal ACPI and x86 power management
 */

#include "acpi.h"
#include "printk.h"

#if defined(ARCH_X86_64) || defined(ARCH_X86)

#include "arch/arch.h"

static const acpi_madt_t *g_madt = NULL;
static const acpi_fadt_t *g_fadt = NULL;
static acpi_power_info_t g_power_info = {0};
static int g_acpi_ready = 0;

extern uint64_t limine_get_hhdm_offset(void);

static int checksum_ok(const uint8_t *ptr, uint32_t len) {
  uint8_t sum = 0;
  for (uint32_t i = 0; i < len; i++) {
    sum = (uint8_t)(sum + ptr[i]);
  }
  return sum == 0;
}

static int sig_eq(const char *a, const char *b) {
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static const uint8_t *madt_entries_begin(const acpi_madt_t *madt) {
  if (!madt || madt->header.length <= sizeof(*madt)) {
    return NULL;
  }
  return (const uint8_t *)madt + sizeof(*madt);
}

static const uint8_t *madt_entries_end(const acpi_madt_t *madt) {
  if (!madt || madt->header.length <= sizeof(*madt)) {
    return NULL;
  }
  return (const uint8_t *)madt + madt->header.length;
}

static void *map_phys(uint64_t phys) {
  uint64_t hhdm = limine_get_hhdm_offset();
  if (!phys || !hhdm) {
    return NULL;
  }
  return (void *)(uintptr_t)(phys + hhdm);
}

static const acpi_sdt_header_t *find_sdt(const acpi_rsdp_t *rsdp,
                                         const char *sig) {
  if (!rsdp) {
    return NULL;
  }

  if (rsdp->revision >= 2 && rsdp->xsdt_address) {
    const acpi_sdt_header_t *xsdt =
        (const acpi_sdt_header_t *)map_phys(rsdp->xsdt_address);
    if (!xsdt || !checksum_ok((const uint8_t *)xsdt, xsdt->length)) {
      return NULL;
    }

    uint32_t count = (xsdt->length - sizeof(acpi_sdt_header_t)) / 8;
    const uint64_t *entries =
        (const uint64_t *)((const uint8_t *)xsdt + sizeof(acpi_sdt_header_t));

    for (uint32_t i = 0; i < count; i++) {
      const acpi_sdt_header_t *hdr =
          (const acpi_sdt_header_t *)map_phys(entries[i]);
      if (hdr && sig_eq(hdr->signature, sig) &&
          checksum_ok((const uint8_t *)hdr, hdr->length)) {
        return hdr;
      }
    }
  }

  if (rsdp->rsdt_address) {
    const acpi_sdt_header_t *rsdt =
        (const acpi_sdt_header_t *)map_phys(rsdp->rsdt_address);
    if (!rsdt || !checksum_ok((const uint8_t *)rsdt, rsdt->length)) {
      return NULL;
    }

    uint32_t count = (rsdt->length - sizeof(acpi_sdt_header_t)) / 4;
    const uint32_t *entries =
        (const uint32_t *)((const uint8_t *)rsdt + sizeof(acpi_sdt_header_t));

    for (uint32_t i = 0; i < count; i++) {
      const acpi_sdt_header_t *hdr =
          (const acpi_sdt_header_t *)map_phys(entries[i]);
      if (hdr && sig_eq(hdr->signature, sig) &&
          checksum_ok((const uint8_t *)hdr, hdr->length)) {
        return hdr;
      }
    }
  }

  return NULL;
}

static uint16_t gas_port(const acpi_gas_t *gas, uint32_t fallback) {
  if (gas && gas->address_space == 1 && gas->address) {
    return (uint16_t)gas->address;
  }
  return (uint16_t)fallback;
}

static uint8_t *find_s5_package(const acpi_fadt_t *fadt) {
  uint64_t dsdt_phys = 0;
  uint32_t dsdt_legacy = 0;
  const acpi_sdt_header_t *dsdt;
  const uint8_t *aml;
  uint32_t len;

  if (!fadt) {
    return NULL;
  }

  dsdt_phys = fadt->x_dsdt ? fadt->x_dsdt : fadt->dsdt;
  dsdt_legacy = fadt->dsdt;
  if (!dsdt_phys && dsdt_legacy) {
    dsdt_phys = dsdt_legacy;
  }

  dsdt = (const acpi_sdt_header_t *)map_phys(dsdt_phys);
  if (!dsdt || !checksum_ok((const uint8_t *)dsdt, dsdt->length) ||
      dsdt->length <= sizeof(acpi_sdt_header_t)) {
    return NULL;
  }

  aml = (const uint8_t *)dsdt + sizeof(acpi_sdt_header_t);
  len = dsdt->length - sizeof(acpi_sdt_header_t);

  for (uint32_t i = 0; i + 7 < len; i++) {
    if (aml[i] == '_' && aml[i + 1] == 'S' && aml[i + 2] == '5' &&
        aml[i + 3] == '_') {
      uint32_t j = i + 4;
      if (aml[j] == 0x12) {
        j++;
      }
      if (j >= len) {
        return NULL;
      }
      if ((aml[j] & 0xC0) == 0x40) {
        j++;
      } else {
        uint8_t pkg_len_bytes = (aml[j] >> 6) & 0x3;
        j += 1 + pkg_len_bytes;
      }
      if (j < len && aml[j] == 0x0A) {
        j++;
      }
      return (uint8_t *)(uintptr_t)(aml + j);
    }
  }

  return NULL;
}

static void populate_power_info(const acpi_fadt_t *fadt) {
  uint8_t *s5;

  g_power_info.pm1a_cnt_port = gas_port(&fadt->x_pm1a_cnt_blk, fadt->pm1a_cnt_blk);
  g_power_info.pm1b_cnt_port = gas_port(&fadt->x_pm1b_cnt_blk, fadt->pm1b_cnt_blk);
  g_power_info.pm1_cnt_len = fadt->pm1_cnt_len;
  g_power_info.reset_reg = fadt->reset_reg;
  g_power_info.reset_value = fadt->reset_value;
  g_power_info.reset_supported =
      (fadt->flags & (1u << 10)) && fadt->reset_reg.address_space == 1 &&
      fadt->reset_reg.address != 0;

  s5 = find_s5_package(fadt);
  if (s5) {
    uint8_t val_a = s5[0];
    uint8_t idx = 1;
    if (val_a == 0x0A) {
      val_a = s5[idx++];
    }
    if (s5[idx] == 0x0A) {
      idx++;
    }
    g_power_info.slp_typa = ((uint16_t)val_a) << 10;
    g_power_info.slp_typb = ((uint16_t)s5[idx]) << 10;
  }
}

void acpi_init(void *rsdp_ptr) {
  const acpi_rsdp_t *rsdp = (const acpi_rsdp_t *)rsdp_ptr;

  g_madt = NULL;
  g_fadt = NULL;
  g_power_info = (acpi_power_info_t){0};
  g_acpi_ready = 0;

  if (!rsdp_ptr) {
    printk(KERN_WARNING "ACPI: no RSDP provided by bootloader\n");
    return;
  }

  if (rsdp->signature[0] != 'R' || rsdp->signature[1] != 'S' ||
      rsdp->signature[2] != 'D' || rsdp->signature[3] != ' ' ||
      rsdp->signature[4] != 'P' || rsdp->signature[5] != 'T' ||
      rsdp->signature[6] != 'R' || rsdp->signature[7] != ' ') {
    printk(KERN_WARNING "ACPI: invalid RSDP signature\n");
    return;
  }

  if (!checksum_ok((const uint8_t *)rsdp,
                   rsdp->revision >= 2 ? rsdp->length : 20)) {
    printk(KERN_WARNING "ACPI: RSDP checksum failed\n");
    return;
  }

  g_madt = (const acpi_madt_t *)find_sdt(rsdp, "APIC");
  g_fadt = (const acpi_fadt_t *)find_sdt(rsdp, "FACP");
  if (g_fadt) {
    populate_power_info(g_fadt);
  }

  g_acpi_ready = 1;
  printk(KERN_INFO "ACPI: initialized (MADT=%p, FADT=%p)\n", g_madt, g_fadt);
}

const acpi_madt_t *acpi_get_madt(void) { return g_madt; }

const acpi_fadt_t *acpi_get_fadt(void) { return g_fadt; }

uint64_t acpi_madt_get_lapic_base(void) {
  const uint8_t *ptr;
  const uint8_t *end;

  if (!g_madt) {
    return 0;
  }

  ptr = madt_entries_begin(g_madt);
  end = madt_entries_end(g_madt);
  while (ptr && end && ptr + sizeof(acpi_madt_entry_header_t) <= end) {
    const acpi_madt_entry_header_t *entry =
        (const acpi_madt_entry_header_t *)ptr;
    if (entry->length < sizeof(*entry) || ptr + entry->length > end) {
      break;
    }
    if (entry->type == ACPI_MADT_ENTRY_LAPIC_ADDR_OVERRIDE &&
        entry->length >= sizeof(acpi_madt_lapic_addr_override_t)) {
      const acpi_madt_lapic_addr_override_t *override =
          (const acpi_madt_lapic_addr_override_t *)ptr;
      if (override->lapic_addr) {
        return override->lapic_addr;
      }
    }
    ptr += entry->length;
  }

  return g_madt->lapic_addr;
}

uint64_t acpi_madt_get_ioapic_base(uint32_t *gsi_base_out) {
  const uint8_t *ptr;
  const uint8_t *end;

  if (gsi_base_out) {
    *gsi_base_out = 0;
  }
  if (!g_madt) {
    return 0;
  }

  ptr = madt_entries_begin(g_madt);
  end = madt_entries_end(g_madt);
  while (ptr && end && ptr + sizeof(acpi_madt_entry_header_t) <= end) {
    const acpi_madt_entry_header_t *entry =
        (const acpi_madt_entry_header_t *)ptr;
    if (entry->length < sizeof(*entry) || ptr + entry->length > end) {
      break;
    }
    if (entry->type == ACPI_MADT_ENTRY_IOAPIC &&
        entry->length >= sizeof(acpi_madt_ioapic_t)) {
      const acpi_madt_ioapic_t *ioapic = (const acpi_madt_ioapic_t *)ptr;
      if (gsi_base_out) {
        *gsi_base_out = ioapic->gsi_base;
      }
      return ioapic->ioapic_addr;
    }
    ptr += entry->length;
  }

  return 0;
}

uint32_t acpi_madt_get_cpu_count(void) {
  const uint8_t *ptr;
  const uint8_t *end;
  uint32_t count = 0;

  if (!g_madt) {
    return 0;
  }

  ptr = madt_entries_begin(g_madt);
  end = madt_entries_end(g_madt);
  while (ptr && end && ptr + sizeof(acpi_madt_entry_header_t) <= end) {
    const acpi_madt_entry_header_t *entry =
        (const acpi_madt_entry_header_t *)ptr;
    if (entry->length < sizeof(*entry) || ptr + entry->length > end) {
      break;
    }

    if (entry->type == ACPI_MADT_ENTRY_LAPIC &&
        entry->length >= sizeof(acpi_madt_lapic_t)) {
      const acpi_madt_lapic_t *lapic = (const acpi_madt_lapic_t *)ptr;
      if (lapic->flags & ACPI_MADT_LAPIC_ENABLED) {
        count++;
      }
    } else if (entry->type == ACPI_MADT_ENTRY_X2APIC &&
               entry->length >= sizeof(acpi_madt_x2apic_t)) {
      const acpi_madt_x2apic_t *x2apic = (const acpi_madt_x2apic_t *)ptr;
      if (x2apic->flags & ACPI_MADT_LAPIC_ENABLED) {
        count++;
      }
    }

    ptr += entry->length;
  }

  return count;
}

int acpi_power_available(void) {
  return g_acpi_ready && g_fadt != NULL;
}

int acpi_reboot(void) {
  if (!acpi_power_available()) {
    return -1;
  }

  if (g_power_info.reset_supported) {
    outb((uint16_t)g_power_info.reset_reg.address, g_power_info.reset_value);
    io_wait();
    return 0;
  }

  return -1;
}

int acpi_poweroff(void) {
  uint16_t sleep_enable = (1u << 13);

  if (!acpi_power_available()) {
    return -1;
  }

  if (g_power_info.pm1a_cnt_port && g_power_info.slp_typa) {
    outw(g_power_info.pm1a_cnt_port,
         (uint16_t)(g_power_info.slp_typa | sleep_enable));
  }
  if (g_power_info.pm1b_cnt_port && g_power_info.slp_typb) {
    outw(g_power_info.pm1b_cnt_port,
         (uint16_t)(g_power_info.slp_typb | sleep_enable));
  }

  return (g_power_info.pm1a_cnt_port || g_power_info.pm1b_cnt_port) ? 0 : -1;
}

#else

void acpi_init(void *rsdp_ptr) {
  (void)rsdp_ptr;
}

const acpi_madt_t *acpi_get_madt(void) { return NULL; }

const acpi_fadt_t *acpi_get_fadt(void) { return NULL; }

uint64_t acpi_madt_get_lapic_base(void) { return 0; }

uint64_t acpi_madt_get_ioapic_base(uint32_t *gsi_base_out) {
  if (gsi_base_out) {
    *gsi_base_out = 0;
  }
  return 0;
}

uint32_t acpi_madt_get_cpu_count(void) { return 0; }

int acpi_power_available(void) { return 0; }

int acpi_reboot(void) { return -1; }

int acpi_poweroff(void) { return -1; }

#endif
