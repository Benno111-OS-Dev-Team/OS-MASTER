/*
 * OS8 - USB Host Controller Driver (XHCI)
 *
 * USB 3.0/3.1/3.2 Host Controller Interface.
 */

#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "arch/arch.h"
#include "printk.h"
#include "types.h"

/* ===================================================================== */
/* XHCI Constants */
/* ===================================================================== */

#define XHCI_MAX_SLOTS 256
#define XHCI_MAX_PORTS 127
#define XHCI_MAX_INTERRUPTERS 1024

/* Capability Register offsets */
#define XHCI_CAPLENGTH 0x00
#define XHCI_HCIVERSION 0x02
#define XHCI_HCSPARAMS1 0x04
#define XHCI_HCSPARAMS2 0x08
#define XHCI_HCSPARAMS3 0x0C
#define XHCI_HCCPARAMS1 0x10
#define XHCI_DBOFF 0x14
#define XHCI_RTSOFF 0x18
#define XHCI_HCCPARAMS2 0x1C

/* Operational Register offsets (from OpBase) */
#define XHCI_USBCMD 0x00
#define XHCI_USBSTS 0x04
#define XHCI_PAGESIZE 0x08
#define XHCI_DNCTRL 0x14
#define XHCI_CRCR 0x18
#define XHCI_DCBAAP 0x30
#define XHCI_CONFIG 0x38

/* Port Register offsets */
#define XHCI_PORTSC 0x00
#define XHCI_PORTPMSC 0x04
#define XHCI_PORTLI 0x08
#define XHCI_PORTHLPMC 0x0C

/* USBCMD bits */
#define XHCI_CMD_RUN (1 << 0)
#define XHCI_CMD_HCRST (1 << 1)
#define XHCI_CMD_INTE (1 << 2)
#define XHCI_CMD_HSEE (1 << 3)

/* USBSTS bits */
#define XHCI_STS_HCH (1 << 0)
#define XHCI_STS_HSE (1 << 2)
#define XHCI_STS_EINT (1 << 3)
#define XHCI_STS_PCD (1 << 4)
#define XHCI_STS_CNR (1 << 11)

/* PORTSC bits */
#define XHCI_PORT_CCS (1 << 0)
#define XHCI_PORT_PED (1 << 1)
#define XHCI_PORT_OCA (1 << 3)
#define XHCI_PORT_PR (1 << 4)
#define XHCI_PORT_PP (1 << 9)
#define XHCI_PORT_CSC (1 << 17)
#define XHCI_PORT_PEC (1 << 18)
#define XHCI_PORT_WRC (1 << 19)
#define XHCI_PORT_OCC (1 << 20)
#define XHCI_PORT_PRC (1 << 21)

/* ===================================================================== */
/* TRB (Transfer Request Block) Types */
/* ===================================================================== */

#define TRB_TYPE_NORMAL 1
#define TRB_TYPE_SETUP 2
#define TRB_TYPE_DATA 3
#define TRB_TYPE_STATUS 4
#define TRB_TYPE_LINK 6
#define TRB_TYPE_EVENT_DATA 7
#define TRB_TYPE_NOOP 8
#define TRB_TYPE_ENABLE_SLOT 9
#define TRB_TYPE_DISABLE_SLOT 10
#define TRB_TYPE_ADDRESS_DEV 11
#define TRB_TYPE_CONFIG_EP 12
#define TRB_TYPE_EVAL_CTX 13
#define TRB_TYPE_RESET_EP 14
#define TRB_TYPE_STOP_EP 15
#define TRB_TYPE_SET_TR_DEQ 16
#define TRB_TYPE_RESET_DEV 17
#define TRB_TYPE_TRANSFER 32
#define TRB_TYPE_CMD_COMPLETE 33
#define TRB_TYPE_PORT_STATUS 34

struct xhci_trb {
  uint64_t param;
  uint32_t status;
  uint32_t control;
} __attribute__((packed));

/* ===================================================================== */
/* Device Context */
/* ===================================================================== */

struct xhci_slot_ctx {
  uint32_t field1;
  uint32_t field2;
  uint32_t tt_info;
  uint32_t state;
  uint32_t reserved[4];
} __attribute__((packed));

struct xhci_ep_ctx {
  uint32_t field1;
  uint32_t field2;
  uint64_t tr_dequeue;
  uint32_t tx_info;
  uint32_t reserved[3];
} __attribute__((packed));

/* ===================================================================== */
/* Driver State */
/* ===================================================================== */

struct xhci_device {
  volatile uint8_t *base;
  volatile uint8_t *op_base;
  volatile uint8_t *run_base;
  volatile uint8_t *db_base;

  uint32_t max_slots;
  uint32_t max_ports;
  uint32_t max_interrupters;
  uint32_t page_size;

  /* Device Context Base Address Array */
  uint64_t *dcbaa;
  phys_addr_t dcbaa_phys;

  /* Command Ring */
  struct xhci_trb *cmd_ring;
  phys_addr_t cmd_ring_phys;
  int cmd_ring_enqueue;
  bool cmd_ring_cycle;

  /* Event Ring */
  struct xhci_trb *event_ring;
  phys_addr_t event_ring_phys;
  int event_ring_dequeue;
  bool event_ring_cycle;

  /* Port info */
  struct {
    bool connected;
    uint8_t speed;
    uint8_t slot_id;
  } ports[XHCI_MAX_PORTS];
};

static struct xhci_device xhci = {0};
static int xhci_ready = 0;
static int xhci_probe_attempted = 0;
static int xhci_init_failed = 0;

#if defined(ARCH_X86_64) || defined(ARCH_X86)
extern uint64_t limine_get_hhdm_offset(void);
#endif

static void *xhci_phys_to_cpu_virt(phys_addr_t paddr) {
#if defined(ARCH_X86_64) || defined(ARCH_X86)
  uint64_t hhdm = limine_get_hhdm_offset();
  if (!paddr)
    return NULL;
  if (hhdm)
    return (void *)(uintptr_t)(paddr + hhdm);
  return phys_to_virt(paddr);
#else
  return phys_to_virt(paddr);
#endif
}

static int xhci_mmio_base_is_sane(phys_addr_t mmio_base) {
  if (!mmio_base)
    return 0;
  if (mmio_base == (phys_addr_t)0xFFFFFFFFULL ||
      mmio_base == (phys_addr_t)0xFFFFFFFFFFFFFFFFULL)
    return 0;
  if ((mmio_base & 0xFFF) != 0)
    return 0;
  return 1;
}

static const char *xhci_speed_name(uint8_t speed) {
  switch (speed) {
  case 1:
    return "full-speed";
  case 2:
    return "low-speed";
  case 3:
    return "high-speed";
  case 4:
    return "super-speed";
  case 5:
    return "super-speed+";
  default:
    return "unknown-speed";
  }
}

static void xhci_append_text(char *dst, int max, const char *src) {
  int idx = 0;

  if (!dst || max <= 0 || !src)
    return;
  while (dst[idx] && idx < max - 1)
    idx++;
  for (int i = 0; src[i] && idx < max - 1; i++)
    dst[idx++] = src[i];
  dst[idx] = '\0';
}

static void xhci_append_decimal(char *dst, int max, int value) {
  char tmp[12];
  int len = 0;

  if (value <= 0) {
    xhci_append_text(dst, max, "0");
    return;
  }

  while (value > 0 && len < (int)sizeof(tmp)) {
    tmp[len++] = (char)('0' + (value % 10));
    value /= 10;
  }
  for (int i = len - 1; i >= 0; i--) {
    char one[2] = {tmp[i], '\0'};
    xhci_append_text(dst, max, one);
  }
}

/* ===================================================================== */
/* MMIO Access */
/* ===================================================================== */

static inline uint32_t xhci_cap_read32(uint32_t offset) {
  return *(volatile uint32_t *)(xhci.base + offset);
}

static inline uint32_t xhci_op_read32(uint32_t offset) {
  return *(volatile uint32_t *)(xhci.op_base + offset);
}

static inline void xhci_op_write32(uint32_t offset, uint32_t val) {
  *(volatile uint32_t *)(xhci.op_base + offset) = val;
}

static inline void xhci_op_write64(uint32_t offset, uint64_t val) {
  *(volatile uint64_t *)(xhci.op_base + offset) = val;
}

static inline uint32_t xhci_port_read32(int port, uint32_t offset) {
  return *(volatile uint32_t *)(xhci.op_base + 0x400 + port * 0x10 + offset);
}

static inline void xhci_port_write32(int port, uint32_t offset, uint32_t val) {
  *(volatile uint32_t *)(xhci.op_base + 0x400 + port * 0x10 + offset) = val;
}

static inline void xhci_cpu_relax(void) {
#if defined(ARCH_X86_64) || defined(ARCH_X86)
  __asm__ volatile("pause");
#else
  __asm__ volatile("yield");
#endif
}

static int xhci_wait_for_mask(uint32_t offset, uint32_t mask, uint32_t expect,
                              uint32_t timeout_ms) {
  uint64_t deadline = arch_timer_get_ms() + timeout_ms;
  while ((xhci_op_read32(offset) & mask) != expect) {
    if (arch_timer_get_ms() >= deadline) {
      return -1;
    }
    xhci_cpu_relax();
  }
  return 0;
}

static int xhci_wait_for_port_mask(int port, uint32_t mask, uint32_t expect,
                                   uint32_t timeout_ms) {
  uint64_t deadline = arch_timer_get_ms() + timeout_ms;
  while ((xhci_port_read32(port, XHCI_PORTSC) & mask) != expect) {
    if (arch_timer_get_ms() >= deadline) {
      return -1;
    }
    xhci_cpu_relax();
  }
  return 0;
}

static void xhci_delay_ms(uint32_t delay_ms) {
  uint64_t deadline = arch_timer_get_ms() + delay_ms;
  while (arch_timer_get_ms() < deadline) {
    xhci_cpu_relax();
  }
}

/* ===================================================================== */
/* Ring Operations */
/* ===================================================================== */

static void xhci_ring_cmd(uint64_t param, uint32_t status, uint32_t control) {
  struct xhci_trb *trb = &xhci.cmd_ring[xhci.cmd_ring_enqueue];

  trb->param = param;
  trb->status = status;
  trb->control = control | (xhci.cmd_ring_cycle ? 1 : 0);

  xhci.cmd_ring_enqueue++;

  /* Handle ring wrap */
  if (xhci.cmd_ring_enqueue >= 63) {
    /* Insert link TRB */
    struct xhci_trb *link = &xhci.cmd_ring[63];
    link->param = xhci.cmd_ring_phys;
    link->status = 0;
    link->control =
        (TRB_TYPE_LINK << 10) | (xhci.cmd_ring_cycle ? 1 : 0) | (1 << 1);

    xhci.cmd_ring_enqueue = 0;
    xhci.cmd_ring_cycle = !xhci.cmd_ring_cycle;
  }

  /* Ring doorbell */
  *(volatile uint32_t *)(xhci.db_base) = 0;
}

/* ===================================================================== */
/* Controller Operations */
/* ===================================================================== */

static int xhci_reset(void) {
  printk(KERN_INFO "XHCI: Resetting controller\n");

  /* Stop controller */
  uint32_t cmd = xhci_op_read32(XHCI_USBCMD);
  xhci_op_write32(XHCI_USBCMD, cmd & ~XHCI_CMD_RUN);

  /* Wait for halt */
  if (xhci_wait_for_mask(XHCI_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, 100) < 0) {
    printk(KERN_ERR "XHCI: Failed to halt\n");
    return -1;
  }

  /* Reset */
  xhci_op_write32(XHCI_USBCMD, XHCI_CMD_HCRST);

  if (xhci_wait_for_mask(XHCI_USBCMD, XHCI_CMD_HCRST, 0, 250) < 0) {
    printk(KERN_ERR "XHCI: Reset timeout\n");
    return -1;
  }

  /* Wait for CNR to clear */
  if (xhci_wait_for_mask(XHCI_USBSTS, XHCI_STS_CNR, 0, 250) < 0) {
    printk(KERN_ERR "XHCI: Controller not ready after reset\n");
    return -1;
  }

  printk(KERN_INFO "XHCI: Reset complete\n");
  return 0;
}

static int xhci_setup_rings(void) {
  /* Allocate DCBAA */
  xhci.dcbaa_phys = pmm_alloc_page();
  if (!xhci.dcbaa_phys)
    return -1;
  xhci.dcbaa = (uint64_t *)xhci_phys_to_cpu_virt(xhci.dcbaa_phys);
  if (!xhci.dcbaa)
    return -1;
  for (uint32_t i = 0; i <= xhci.max_slots; i++) {
    xhci.dcbaa[i] = 0;
  }

  /* Set DCBAAP */
  xhci_op_write64(XHCI_DCBAAP, xhci.dcbaa_phys);

  /* Allocate Command Ring */
  xhci.cmd_ring_phys = pmm_alloc_page();
  if (!xhci.cmd_ring_phys)
    return -1;
  xhci.cmd_ring = (struct xhci_trb *)xhci_phys_to_cpu_virt(xhci.cmd_ring_phys);
  if (!xhci.cmd_ring)
    return -1;
  xhci.cmd_ring_enqueue = 0;
  xhci.cmd_ring_cycle = true;

  /* Set CRCR */
  xhci_op_write64(XHCI_CRCR, xhci.cmd_ring_phys | 1);

  /* Allocate Event Ring */
  xhci.event_ring_phys = pmm_alloc_page();
  if (!xhci.event_ring_phys)
    return -1;
  xhci.event_ring =
      (struct xhci_trb *)xhci_phys_to_cpu_virt(xhci.event_ring_phys);
  if (!xhci.event_ring)
    return -1;
  xhci.event_ring_dequeue = 0;
  xhci.event_ring_cycle = true;

  printk(KERN_INFO "XHCI: Rings initialized\n");
  return 0;
}

/* ===================================================================== */
/* Port Management */
/* ===================================================================== */

#include "drivers/usb/usb.h"

static void xhci_enumerate_device(int port) {
  printk(KERN_INFO "XHCI: Enumerating device on port %d\n", port + 1);

  /* TODO:
   * 1. Enable Slot
   * 2. Address Device
   * 3. Read Device Descriptor
   * 4. Read Configuration Descriptor
   */

  /* access to drivers */
  struct usb_device *dev = kzalloc(sizeof(struct usb_device), GFP_KERNEL);
  if (!dev)
    return;

  dev->controller = &xhci;
  dev->bus_id = 0;   // Bus 0
  dev->dev_addr = 0; // Pending

  /* Mock Dispatch for demonstration until descriptors are read */
  /* Checks would be based on bDeviceClass/bInterfaceClass */

  /* Attempt to load drivers */
  // In a real implementation we would match against descriptors
  // usb_hid_init(dev);
  // usb_msd_init(dev);
}

static void xhci_ack_port_changes(int port, uint32_t portsc) {
  uint32_t ack = portsc &
                 (XHCI_PORT_CSC | XHCI_PORT_PEC | XHCI_PORT_WRC |
                  XHCI_PORT_OCC | XHCI_PORT_PRC);
  if (ack) {
    xhci_port_write32(port, XHCI_PORTSC, XHCI_PORT_PP | ack);
  }
}

static void xhci_power_port(int port) {
  uint32_t portsc = xhci_port_read32(port, XHCI_PORTSC);
  if ((portsc & XHCI_PORT_PP) == 0) {
    xhci_port_write32(port, XHCI_PORTSC, portsc | XHCI_PORT_PP);
  }
}

static void xhci_reset_port_if_needed(int port, uint32_t portsc) {
  uint32_t ack;

  if ((portsc & XHCI_PORT_CCS) == 0 || (portsc & XHCI_PORT_PED) != 0) {
    return;
  }

  ack = portsc &
        (XHCI_PORT_CSC | XHCI_PORT_PEC | XHCI_PORT_WRC | XHCI_PORT_OCC |
         XHCI_PORT_PRC);
  xhci_port_write32(port, XHCI_PORTSC, XHCI_PORT_PP | XHCI_PORT_PR | ack);

  if (xhci_wait_for_port_mask(port, XHCI_PORT_PR, 0, 100) < 0) {
    printk(KERN_WARNING "XHCI: Port %d reset timed out\n", port + 1);
  }
}

static void xhci_check_port(int port) {
  uint32_t portsc;
  bool connected;
  int speed;

  xhci_power_port(port);

  portsc = xhci_port_read32(port, XHCI_PORTSC);
  xhci_ack_port_changes(port, portsc);
  xhci_reset_port_if_needed(port, portsc);

  portsc = xhci_port_read32(port, XHCI_PORTSC);
  connected = (portsc & XHCI_PORT_CCS) != 0;
  speed = (portsc >> 10) & 0xF;

  xhci.ports[port].connected = connected;
  xhci.ports[port].speed = speed;

  if (connected) {
    const char *speed_str = "Unknown";
    switch (speed) {
    case 1:
      speed_str = "Full Speed (12 Mbps)";
      break;
    case 2:
      speed_str = "Low Speed (1.5 Mbps)";
      break;
    case 3:
      speed_str = "High Speed (480 Mbps)";
      break;
    case 4:
      speed_str = "Super Speed (5 Gbps)";
      break;
    case 5:
      speed_str = "Super Speed+ (10 Gbps)";
      break;
    }
    printk(KERN_INFO "XHCI: Port %d: Device connected - %s\n", port + 1,
           speed_str);

    /* Trigger enumeration */
    xhci_enumerate_device(port);
  }
}

/* ===================================================================== */
/* Initialization */
/* ===================================================================== */

int xhci_init(phys_addr_t mmio_base) {
  printk(KERN_INFO "XHCI: Initializing USB 3.x Host Controller\n");

  if (xhci_ready)
    return 0;
  if (xhci_probe_attempted && xhci_init_failed)
    return -1;

  xhci_probe_attempted = 1;

  if (!xhci_mmio_base_is_sane(mmio_base)) {
    printk(KERN_WARNING "XHCI: Refusing unsafe MMIO base 0x%llx\n",
           (unsigned long long)mmio_base);
    xhci_init_failed = 1;
    return -1;
  }

  /* Resolve MMIO virtual mapping */
  xhci.base = (volatile uint8_t *)xhci_phys_to_cpu_virt(mmio_base);
  if (!xhci.base) {
    printk(KERN_ERR "XHCI: Failed to resolve MMIO CPU mapping for 0x%llx\n",
           (unsigned long long)mmio_base);
    xhci_init_failed = 1;
    return -1;
  }

#if defined(ARCH_X86_64) || defined(ARCH_X86)
  /*
   * On x86 bring-up we keep Limine paging active, so VMM remaps are not
   * reliable yet. Use the existing HHDM mapping for PCI MMIO instead.
   */
  if (!limine_get_hhdm_offset()) {
    printk(KERN_ERR "XHCI: Missing HHDM mapping for MMIO base 0x%llx\n",
           (unsigned long long)mmio_base);
    xhci_init_failed = 1;
    return -1;
  }
#else
  if (vmm_map_range(mmio_base, mmio_base, 0x10000, VM_DEVICE) != 0) {
    printk(KERN_ERR "XHCI: Failed to map controller MMIO at 0x%llx\n",
           (unsigned long long)mmio_base);
    xhci_init_failed = 1;
    return -1;
  }
#endif

  /* Read capability registers */
  uint8_t cap_length = xhci.base[XHCI_CAPLENGTH];
  uint16_t hci_version = (uint16_t)(xhci_cap_read32(XHCI_HCSPARAMS1 - 0x4) >> 16);
  uint32_t hcsparams1 = xhci_cap_read32(XHCI_HCSPARAMS1);

  if (cap_length < 0x20 || cap_length > 0x80) {
    printk(KERN_ERR "XHCI: Invalid CAPLENGTH=0x%x\n", cap_length);
    xhci_init_failed = 1;
    return -1;
  }

  xhci.max_slots = hcsparams1 & 0xFF;
  xhci.max_ports = (hcsparams1 >> 24) & 0xFF;
  xhci.max_interrupters = (hcsparams1 >> 8) & 0x7FF;

  if (xhci.max_ports > XHCI_MAX_PORTS) {
    printk(KERN_WARNING
           "XHCI: Controller reports %d ports, clamping to %d\n",
           xhci.max_ports, XHCI_MAX_PORTS);
    xhci.max_ports = XHCI_MAX_PORTS;
  }

  printk(KERN_INFO "XHCI: Version %x.%x, %d slots, %d ports\n",
         hci_version >> 8, hci_version & 0xFF, xhci.max_slots, xhci.max_ports);

  /* Calculate register bases */
  xhci.op_base = xhci.base + cap_length;

  uint32_t rtsoff = xhci_cap_read32(XHCI_RTSOFF) & ~0x1F;
  xhci.run_base = xhci.base + rtsoff;

  uint32_t dboff = xhci_cap_read32(XHCI_DBOFF) & ~0x3;
  xhci.db_base = xhci.base + dboff;

  if (rtsoff >= 0x10000 || dboff >= 0x10000 || (rtsoff + 0x1000) > 0x10000 ||
      (dboff + 0x1000) > 0x10000) {
    printk(KERN_ERR "XHCI: Invalid register offsets RTSOFF=0x%x DBOFF=0x%x\n",
           rtsoff, dboff);
    xhci_init_failed = 1;
    return -1;
  }

  /* Get page size */
  xhci.page_size = xhci_op_read32(XHCI_PAGESIZE) << 12;

  /* Reset controller */
  if (xhci_reset() < 0) {
    return -1;
  }

  /* Set max slots */
  xhci_op_write32(XHCI_CONFIG, xhci.max_slots);

  /* Setup rings */
  if (xhci_setup_rings() < 0) {
    return -1;
  }

  /*
   * Start controller in polling-safe mode.
   *
   * We currently do not program interrupter state (ERST/ERDP/IMAN/IMOD), so
   * enabling host interrupts here can trigger faults as soon as USB input
   * events arrive. Keep INTE cleared until full interrupt plumbing lands.
   */
  uint32_t cmd = xhci_op_read32(XHCI_USBCMD);
  xhci_op_write32(XHCI_USBCMD, (cmd | XHCI_CMD_RUN) & ~XHCI_CMD_INTE);
  if (xhci_wait_for_mask(XHCI_USBSTS, XHCI_STS_HCH, 0, 100) < 0) {
    printk(KERN_ERR "XHCI: Controller failed to start\n");
    return -1;
  }

  /* Give laptop root hubs a short settle window before probing ports. */
  xhci_delay_ms(20);

  /* Sweep all root hub ports so every physical port is powered and probed. */
  for (int i = 0; i < (int)xhci.max_ports; i++) {
    xhci_check_port(i);
  }

  /* A second short pass helps controllers that report connect status late. */
  xhci_delay_ms(20);
  for (int i = 0; i < (int)xhci.max_ports; i++) {
    if (!xhci.ports[i].connected) {
      xhci_check_port(i);
    }
  }

  printk(KERN_INFO "XHCI: Controller started\n");
  xhci_ready = 1;
  xhci_init_failed = 0;
  return 0;
}

int xhci_is_ready(void) { return xhci_ready; }

int xhci_get_port_count(void) { return (int)xhci.max_ports; }

int xhci_get_connected_count(void) {
  int count = 0;
  for (int i = 0; i < (int)xhci.max_ports; i++) {
    if (xhci.ports[i].connected) {
      count++;
    }
  }
  return count;
}

int usb_device_count(void) { return xhci_get_connected_count(); }

int usb_device_info(int idx, uint16_t *vid, uint16_t *pid, char *name,
                    int name_len) {
  int current = 0;

  if (idx < 0)
    return 0;

  for (int port = 0; port < (int)xhci.max_ports; port++) {
    if (!xhci.ports[port].connected)
      continue;
    if (current != idx) {
      current++;
      continue;
    }

    if (vid)
      *vid = 0;
    if (pid)
      *pid = 0;
    if (name && name_len > 0) {
      name[0] = '\0';
      xhci_append_text(name, name_len, "USB Device P");
      xhci_append_decimal(name, name_len, port + 1);
      xhci_append_text(name, name_len, " ");
      xhci_append_text(name, name_len, xhci_speed_name(xhci.ports[port].speed));
    }
    return 1;
  }

  if (name && name_len > 0)
    name[0] = '\0';
  return 0;
}
