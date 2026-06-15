/*
 * OS8 - Minimal ACPI support for x86 platforms
 */

#ifndef _ACPI_H
#define _ACPI_H

#include "types.h"

typedef struct {
  char signature[8];
  uint8_t checksum;
  char oem_id[6];
  uint8_t revision;
  uint32_t rsdt_address;
  uint32_t length;
  uint64_t xsdt_address;
  uint8_t ext_checksum;
  uint8_t reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

typedef struct {
  char signature[4];
  uint32_t length;
  uint8_t revision;
  uint8_t checksum;
  char oem_id[6];
  char oem_table_id[8];
  uint32_t oem_revision;
  uint32_t creator_id;
  uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

typedef struct {
  uint8_t address_space;
  uint8_t bit_width;
  uint8_t bit_offset;
  uint8_t access_size;
  uint64_t address;
} __attribute__((packed)) acpi_gas_t;

typedef struct {
  acpi_sdt_header_t header;
  uint32_t firmware_ctrl;
  uint32_t dsdt;
  uint8_t reserved0;
  uint8_t preferred_pm_profile;
  uint16_t sci_int;
  uint32_t smi_cmd;
  uint8_t acpi_enable;
  uint8_t acpi_disable;
  uint8_t s4bios_req;
  uint8_t pstate_cnt;
  uint32_t pm1a_evt_blk;
  uint32_t pm1b_evt_blk;
  uint32_t pm1a_cnt_blk;
  uint32_t pm1b_cnt_blk;
  uint32_t pm2_cnt_blk;
  uint32_t pm_tmr_blk;
  uint32_t gpe0_blk;
  uint32_t gpe1_blk;
  uint8_t pm1_evt_len;
  uint8_t pm1_cnt_len;
  uint8_t pm2_cnt_len;
  uint8_t pm_tmr_len;
  uint8_t gpe0_blk_len;
  uint8_t gpe1_blk_len;
  uint8_t gpe1_base;
  uint8_t cst_cnt;
  uint16_t p_lvl2_lat;
  uint16_t p_lvl3_lat;
  uint16_t flush_size;
  uint16_t flush_stride;
  uint8_t duty_offset;
  uint8_t duty_width;
  uint8_t day_alrm;
  uint8_t mon_alrm;
  uint8_t century;
  uint16_t iapc_boot_arch;
  uint8_t reserved1;
  uint32_t flags;
  acpi_gas_t reset_reg;
  uint8_t reset_value;
  uint8_t reserved2[3];
  uint64_t x_firmware_ctrl;
  uint64_t x_dsdt;
  acpi_gas_t x_pm1a_evt_blk;
  acpi_gas_t x_pm1b_evt_blk;
  acpi_gas_t x_pm1a_cnt_blk;
  acpi_gas_t x_pm1b_cnt_blk;
  acpi_gas_t x_pm2_cnt_blk;
  acpi_gas_t x_pm_tmr_blk;
  acpi_gas_t x_gpe0_blk;
  acpi_gas_t x_gpe1_blk;
  acpi_gas_t sleep_control_reg;
  acpi_gas_t sleep_status_reg;
} __attribute__((packed)) acpi_fadt_t;

typedef struct {
  acpi_sdt_header_t header;
  uint32_t lapic_addr;
  uint32_t flags;
} __attribute__((packed)) acpi_madt_t;

typedef struct {
  uint8_t type;
  uint8_t length;
} __attribute__((packed)) acpi_madt_entry_header_t;

typedef struct {
  acpi_madt_entry_header_t header;
  uint8_t acpi_processor_id;
  uint8_t apic_id;
  uint32_t flags;
} __attribute__((packed)) acpi_madt_lapic_t;

typedef struct {
  acpi_madt_entry_header_t header;
  uint8_t ioapic_id;
  uint8_t reserved;
  uint32_t ioapic_addr;
  uint32_t gsi_base;
} __attribute__((packed)) acpi_madt_ioapic_t;

typedef struct {
  acpi_madt_entry_header_t header;
  uint8_t reserved[2];
  uint64_t lapic_addr;
} __attribute__((packed)) acpi_madt_lapic_addr_override_t;

typedef struct {
  acpi_madt_entry_header_t header;
  uint16_t reserved;
  uint32_t x2apic_id;
  uint32_t flags;
  uint32_t acpi_uid;
} __attribute__((packed)) acpi_madt_x2apic_t;

#define ACPI_MADT_ENTRY_LAPIC 0
#define ACPI_MADT_ENTRY_IOAPIC 1
#define ACPI_MADT_ENTRY_LAPIC_ADDR_OVERRIDE 5
#define ACPI_MADT_ENTRY_X2APIC 9

#define ACPI_MADT_LAPIC_ENABLED (1u << 0)

typedef struct {
  uint16_t pm1a_cnt_port;
  uint16_t pm1b_cnt_port;
  uint8_t pm1_cnt_len;
  uint16_t slp_typa;
  uint16_t slp_typb;
  uint8_t reset_supported;
  acpi_gas_t reset_reg;
  uint8_t reset_value;
} acpi_power_info_t;

void acpi_init(void *rsdp_ptr);
const acpi_madt_t *acpi_get_madt(void);
const acpi_fadt_t *acpi_get_fadt(void);
uint64_t acpi_madt_get_lapic_base(void);
uint64_t acpi_madt_get_ioapic_base(uint32_t *gsi_base_out);
uint32_t acpi_madt_get_cpu_count(void);
int acpi_power_available(void);
int acpi_reboot(void);
int acpi_poweroff(void);

#endif
