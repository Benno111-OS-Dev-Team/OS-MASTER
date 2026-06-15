/*
 * OS8 - Intel Integrated Graphics Driver
 *
 * Conservative Intel graphics support:
 * - Detect Intel display-class PCI devices
 * - Reuse the bootloader-provided framebuffer for display output
 * - Keep the driver fully initialized in framebuffer mode
 * - Avoid unimplemented native modesetting paths that can crash boot
 */

#include "drivers/intel_gfx.h"
#include "mm/vmm.h"
#include "printk.h"

#define INTEL_GFX_MMIO_MAP_SIZE 0x200000

typedef struct {
  bool detected;
  bool initialized;
  bool probe_attempted;
  bool init_failed;
  bool has_framebuffer;
  bool supported_device;
  bool supports_gpu_rendering;
  bool framebuffer_fallback_active;
  bool mmio_mapped;
  uint16_t device_id;
  uint64_t mmio_base;
  uint64_t aperture_base;
  uint32_t irq;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  volatile uint8_t *mmio;
} intel_gfx_state_t;

static intel_gfx_state_t intel_gfx_state = {0};

static int intel_gfx_is_display_device(const pci_device_t *pci_dev) {
  if (!pci_dev)
    return 0;
  if (pci_dev->vendor_id != INTEL_GPU_VENDOR_ID)
    return 0;
  if (pci_dev->class_code != 0x03)
    return 0;
  return 1;
}

static int intel_gfx_device_id_in_list(uint16_t device_id,
                                       const uint16_t *device_ids) {
  if (!device_ids)
    return 0;
  while (*device_ids) {
    if (*device_ids == device_id)
      return 1;
    device_ids++;
  }
  return 0;
}

static const uint16_t intel_gfx_ivybridge_ids[] = {
    0x0152, 0x0156, 0x015A, 0x0162, 0x0166, 0x016A, 0};

static const uint16_t intel_gfx_sandy_bridge_ids[] = {
    0x0102, 0x0106, 0x010A, 0x0112, 0x0116, 0x0122, 0x0126, 0};

static const uint16_t intel_gfx_haswell_ids[] = {
    0x0402, 0x0406, 0x040A, 0x040B, 0x040E, 0x0412, 0x0416, 0x041A, 0x041E,
    0x0A02, 0x0A06, 0x0A0A, 0x0A0B, 0x0A0E, 0x0A16, 0x0A1E, 0x0C02, 0x0C06,
    0x0C0A, 0x0C0B, 0x0C0E, 0x0D02, 0x0D06, 0x0D0A, 0x0D0B, 0x0D0E, 0};

static const uint16_t intel_gfx_broadwell_ids[] = {
    0x1602, 0x1606, 0x160A, 0x160B, 0x160D, 0x160E, 0x1612, 0x1616, 0x161A,
    0x161B, 0x161D, 0x161E, 0x1622, 0x1626, 0x162A, 0x162B, 0x162D, 0x162E,
    0x1632, 0x1636, 0x163A, 0x163B, 0x163D, 0x163E, 0};

static const uint16_t intel_gfx_bay_trail_ids[] = {
    0x0155, 0x0157, 0x0F30, 0x0F31, 0x0F32, 0x0F33, 0};

static const uint16_t intel_gfx_cherryview_ids[] = {
    0x22B0, 0x22B1, 0x22B2, 0x22B3, 0};

static const uint16_t intel_gfx_skylake_ids[] = {
    0x1902, 0x1906, 0x190A, 0x190B, 0x190E, 0x1912, 0x1913, 0x1915, 0x1916,
    0x1917, 0x191A, 0x191B, 0x191D, 0x191E, 0x1921, 0x1923, 0x1926, 0x1927,
    0x192A, 0x192B, 0x192D, 0x1932, 0x193A, 0x193B, 0x193D, 0};

static const uint16_t intel_gfx_kaby_lake_ids[] = {
    0x5902, 0x5906, 0x5908, 0x590A, 0x590B, 0x590E, 0x5912, 0x5913, 0x5915,
    0x5916, 0x5917, 0x591A, 0x591B, 0x591C, 0x591D, 0x591E, 0x5921, 0x5923,
    0x5926, 0x5927, 0x593B, 0x87C0, 0};

static const uint16_t intel_gfx_apollo_lake_ids[] = {
    0x0A84, 0x1A84, 0x1A85, 0x5A84, 0x5A85, 0};

static const uint16_t intel_gfx_gemini_lake_ids[] = {0x3184, 0x3185, 0};

static const uint16_t intel_gfx_ice_lake_ids[] = {
    0x8A50, 0x8A51, 0x8A52, 0x8A53, 0x8A54, 0x8A56, 0x8A57, 0x8A58, 0x8A59,
    0x8A5A, 0x8A5B, 0x8A5C, 0x8A5D, 0x8A70, 0x8A71, 0};

static const uint16_t intel_gfx_tiger_lake_ids[] = {
    0x9A40, 0x9A49, 0x9A59, 0x9A60, 0x9A68, 0x9A70, 0x9A78, 0};

static const uint16_t intel_gfx_rocket_lake_ids[] = {
    0x4C8A, 0x4C8B, 0x4C90, 0x4C9A, 0};

static const uint16_t intel_gfx_jasper_lake_ids[] = {
    0x4E51, 0x4E55, 0x4E57, 0x4E61, 0x4E71, 0};

static const uint16_t intel_gfx_elkhart_lake_ids[] = {
    0x4541, 0x4551, 0x4555, 0x4557, 0x4571, 0};

static const uint16_t intel_gfx_alder_lake_ids[] = {
    0x4626, 0x4628, 0x462A, 0x4680, 0x4682, 0x4688, 0x468A, 0x468B, 0x4690,
    0x4692, 0x4693, 0x46A0, 0x46A1, 0x46A2, 0x46A3, 0x46A6, 0x46A8, 0x46AA,
    0x46B0, 0x46B1, 0x46B2, 0x46B3, 0x46C0, 0x46C1, 0x46C2, 0x46C3, 0x46D0,
    0x46D1, 0x46D2, 0x46D3, 0x46D4, 0xA721, 0};

static const uint16_t intel_gfx_coffee_lake_ids[] = {
    0x3E90, 0x3E91, 0x3E92, 0x3E93, 0x3E94, 0x3E96, 0x3E98, 0x3E99, 0x3E9A,
    0x3E9B, 0x3E9C, 0x3EA0, 0x3EA1, 0x3EA2, 0x3EA3, 0x3EA4, 0x3EA5, 0x3EA6,
    0x3EA7, 0x3EA8, 0x3EA9, 0x87CA, 0x9B21, 0x9BA0, 0x9BA2, 0x9BA4, 0x9BA5,
    0x9BA8, 0x9BAA, 0x9BAB, 0x9BAC, 0x9BC0, 0x9BC2, 0x9BC4, 0x9BC5, 0x9BC6,
    0x9BCA, 0x9BCB, 0x9BCC, 0x9BE6, 0x9BF6, 0};

static const uint16_t intel_gfx_comet_lake_ids[] = {0x9BC5, 0x9BC8, 0};

static int intel_gfx_is_ivybridge_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_ivybridge_ids);
}

static int intel_gfx_is_sandy_bridge_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_sandy_bridge_ids);
}

static int intel_gfx_is_haswell_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_haswell_ids);
}

static int intel_gfx_is_broadwell_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_broadwell_ids);
}

static int intel_gfx_is_bay_trail_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_bay_trail_ids);
}

static int intel_gfx_is_cherryview_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_cherryview_ids);
}

static int intel_gfx_is_skylake_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_skylake_ids);
}

static int intel_gfx_is_kaby_lake_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_kaby_lake_ids);
}

static int intel_gfx_is_apollo_lake_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_apollo_lake_ids);
}

static int intel_gfx_is_gemini_lake_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_gemini_lake_ids);
}

static int intel_gfx_is_ice_lake_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_ice_lake_ids);
}

static int intel_gfx_is_tiger_lake_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_tiger_lake_ids);
}

static int intel_gfx_is_rocket_lake_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_rocket_lake_ids);
}

static int intel_gfx_is_jasper_lake_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_jasper_lake_ids);
}

static int intel_gfx_is_elkhart_lake_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_elkhart_lake_ids);
}

static int intel_gfx_is_alder_lake_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_alder_lake_ids);
}

static int intel_gfx_is_coffee_lake_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_coffee_lake_ids);
}

static int intel_gfx_is_comet_lake_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_comet_lake_ids);
}

static int intel_gfx_is_modern_xe_device(uint16_t device_id) {
  return intel_gfx_device_id_in_list(device_id, intel_gfx_alder_lake_ids);
}

static int intel_gfx_is_supported_device_id(uint16_t device_id) {
  return intel_gfx_is_sandy_bridge_device(device_id) ||
         intel_gfx_is_ivybridge_device(device_id) ||
         intel_gfx_is_haswell_device(device_id) ||
         intel_gfx_is_broadwell_device(device_id) ||
         intel_gfx_is_bay_trail_device(device_id) ||
         intel_gfx_is_cherryview_device(device_id) ||
         intel_gfx_is_skylake_device(device_id) ||
         intel_gfx_is_kaby_lake_device(device_id) ||
         intel_gfx_is_apollo_lake_device(device_id) ||
         intel_gfx_is_gemini_lake_device(device_id) ||
         intel_gfx_is_ice_lake_device(device_id) ||
         intel_gfx_is_tiger_lake_device(device_id) ||
         intel_gfx_is_rocket_lake_device(device_id) ||
         intel_gfx_is_jasper_lake_device(device_id) ||
         intel_gfx_is_elkhart_lake_device(device_id) ||
         intel_gfx_is_alder_lake_device(device_id) ||
         intel_gfx_is_coffee_lake_device(device_id) ||
         intel_gfx_is_comet_lake_device(device_id) ||
         intel_gfx_is_modern_xe_device(device_id);
}

static int intel_gfx_supports_gpu_rendering_device(uint16_t device_id) {
  /*
   * The current Intel path is framebuffer-only. We keep the hook so the
   * compositor can distinguish future hardware acceleration support, but the
   * driver does not advertise GPU rendering yet.
   */
  (void)device_id;
  return 0;
}

static int intel_gfx_should_use_native_driver(uint16_t device_id) {
  (void)device_id;
  return 0;
}

static const char *intel_gfx_detect_name(uint16_t device_id) {
  switch (device_id) {
  case 0x0102:
  case 0x0106:
  case 0x010A:
    return "Intel HD Graphics 2000";
  case 0x0112:
  case 0x0116:
  case 0x0122:
  case 0x0126:
    return "Intel HD Graphics 3000";
  case 0x0152:
  case 0x0156:
    return "Intel HD Graphics 2500";
  case 0x015A:
    return "Intel HD Graphics";
  case 0x0162:
  case 0x0166:
    return "Intel HD Graphics 4000";
  case 0x016A:
    return "Intel HD Graphics P4000";
  case 0x1602:
  case 0x1606:
  case 0x160A:
  case 0x160B:
  case 0x160D:
  case 0x160E:
  case 0x161B:
  case 0x161D:
  case 0x162D:
  case 0x162E:
  case 0x1632:
  case 0x1636:
  case 0x163A:
  case 0x163B:
  case 0x163D:
  case 0x163E:
    return "Intel HD Graphics";
  case 0x1612:
    return "Intel HD Graphics 5600";
  case 0x1616:
    return "Intel HD Graphics 5500";
  case 0x161A:
    return "Intel HD Graphics P5700";
  case 0x161E:
    return "Intel HD Graphics 5300";
  case 0x1622:
    return "Intel Iris Pro Graphics 6200";
  case 0x1626:
    return "Intel HD Graphics 6000";
  case 0x162A:
    return "Intel Iris Pro Graphics P6300";
  case 0x162B:
    return "Intel Iris Graphics 6100";
  case 0x0155:
  case 0x0157:
  case 0x0F30:
  case 0x0F31:
  case 0x0F32:
  case 0x0F33:
    return "Intel HD Graphics";
  case 0x22B0:
  case 0x22B2:
  case 0x22B3:
    return "Intel HD Graphics";
  case 0x22B1:
    return "Intel HD Graphics";
  case 0x0402:
  case 0x0406:
  case 0x040A:
  case 0x040B:
  case 0x040E:
  case 0x0412:
  case 0x0416:
  case 0x041A:
  case 0x0A02:
  case 0x0A06:
  case 0x0A0A:
  case 0x0A0B:
  case 0x0A0E:
  case 0x0C02:
  case 0x0C06:
  case 0x0C0A:
  case 0x0C0B:
  case 0x0C0E:
  case 0x0D02:
  case 0x0D06:
  case 0x0D0A:
  case 0x0D0B:
  case 0x0D0E:
    return "Intel HD Graphics 4600";
  case 0x041E:
    return "Intel HD Graphics 4400";
  case 0x0A16:
    return "Intel HD Graphics 4400";
  case 0x0A1E:
    return "Intel HD Graphics 4200";
  case 0x0A26:
    return "Intel HD Graphics 5000";
  case 0x0A2E:
    return "Intel Iris Graphics 5100";
  case 0x0A12:
  case 0x0A1A:
    return "Intel HD Graphics 4600";
  case 0x5927:
    return "Intel Iris Plus Graphics 650";
  case 0x5926:
    return "Intel Iris Plus Graphics 640";
  case 0x5923:
    return "Intel HD Graphics 635";
  case 0x5917:
    return "Intel UHD Graphics 620";
  case 0x5916:
  case 0x5921:
    return "Intel HD Graphics 620";
  case 0x1916:
  case 0x1921:
    return "Intel HD Graphics 520";
  case 0x591A:
  case 0x591D:
    return "Intel HD Graphics P630";
  case 0x591E:
    return "Intel HD Graphics 615";
  case 0x591C:
    return "Intel UHD Graphics 615";
  case 0x5912:
  case 0x591B:
    return "Intel HD Graphics 630";
  case 0x593B:
    return "Intel HD Graphics";
  case 0x5902:
  case 0x5906:
  case 0x590B:
    return "Intel HD Graphics 610";
  case 0x5908:
  case 0x590A:
  case 0x590E:
    return "Intel HD Graphics";
  case 0x3184:
    return "Intel UHD Graphics 605";
  case 0x3185:
    return "Intel UHD Graphics 600";
  case 0x8A50:
  case 0x8A57:
  case 0x8A59:
  case 0x8A5B:
  case 0x8A5D:
  case 0x8A70:
  case 0x8A71:
    return "Intel HD Graphics";
  case 0x8A56:
  case 0x8A58:
    return "Intel UHD Graphics";
  case 0x8A51:
  case 0x8A52:
  case 0x8A53:
  case 0x8A54:
  case 0x8A5A:
  case 0x8A5C:
    return "Intel Iris Plus Graphics";
  case 0x9A40:
  case 0x9A49:
    return "Intel Iris Xe Graphics";
  case 0x9A59:
  case 0x9A60:
  case 0x9A68:
  case 0x9A70:
  case 0x9A78:
    return "Intel UHD Graphics";
  case 0x4C8A:
    return "Intel UHD Graphics 750";
  case 0x4C8B:
    return "Intel UHD Graphics 730";
  case 0x4C90:
  case 0x4C9A:
    return "Intel UHD Graphics P750";
  case 0x4E51:
  case 0x4E55:
  case 0x4E57:
  case 0x4E61:
  case 0x4E71:
    return "Intel UHD Graphics";
  case 0x4541:
  case 0x4551:
  case 0x4555:
  case 0x4557:
  case 0x4571:
    return "Intel UHD Graphics";
  case 0x4626:
  case 0x4628:
  case 0x462A:
  case 0x4680:
  case 0x4682:
  case 0x4688:
  case 0x468A:
  case 0x468B:
  case 0x4690:
  case 0x4692:
  case 0x4693:
  case 0x46A0:
  case 0x46A1:
  case 0x46A2:
  case 0x46A3:
  case 0x46B0:
  case 0x46B1:
  case 0x46B2:
  case 0x46B3:
  case 0x46C0:
  case 0x46C1:
  case 0x46C2:
  case 0x46C3:
  case 0x46D0:
  case 0x46D2:
  case 0x46D3:
  case 0x46D4:
  case 0xA721:
    return "Intel UHD Graphics";
  case 0x3EA5:
  case 0x3EA8:
    return "Intel Iris Plus Graphics 655";
  case 0x3EA6:
    return "Intel Iris Plus Graphics 645";
  case 0x3EA7:
    return "Intel HD Graphics";
  case 0x3EA2:
    return "Intel UHD Graphics";
  case 0x3E90:
  case 0x3E93:
  case 0x3E99:
  case 0x3E9C:
  case 0x3EA1:
  case 0x9BA5:
  case 0x9BA8:
    return "Intel UHD Graphics 610";
  case 0x3EA4:
  case 0x9B21:
  case 0x9BA0:
  case 0x9BA2:
  case 0x9BA4:
  case 0x9BAA:
  case 0x9BAB:
  case 0x9BAC:
    return "Intel UHD Graphics";
  case 0x87CA:
  case 0x3EA3:
  case 0x9B41:
  case 0x9BC0:
  case 0x9BC2:
  case 0x9BC4:
  case 0x9BCA:
  case 0x9BCB:
  case 0x9BCC:
    return "Intel UHD Graphics";
  case 0x3E91:
  case 0x3E92:
  case 0x3E98:
  case 0x3E9B:
  case 0x9BC5:
  case 0x9BC8:
    return "Intel UHD Graphics 630";
  case 0x3E96:
  case 0x3E9A:
  case 0x3E94:
  case 0x9BC6:
  case 0x9BE6:
  case 0x9BF6:
    return "Intel UHD Graphics P630";
  case 0x3EA9:
  case 0x3EA0:
    return "Intel UHD Graphics 620";
  case 0x46A6:
  case 0x46A8:
  case 0x46AA:
  case 0x46D1:
    return "Intel Iris Xe Graphics";
  default:
    return "Intel Integrated Graphics";
  }
}

static int intel_gfx_is_supported_subclass(const pci_device_t *pci_dev) {
  if (!pci_dev)
    return 0;
  return pci_dev->subclass == 0x00 || pci_dev->subclass == 0x80;
}

static int intel_gfx_mmio_bar_is_sane(uint64_t addr) {
  if (!addr)
    return 0;
  if (addr == 0xFFFFFFFFULL || addr == 0xFFFFFFFFFFFFFFFFULL)
    return 0;
  if ((addr & 0xFFFULL) != 0)
    return 0;
  return 1;
}

static int intel_gfx_framebuffer_is_sane(uint32_t *fb, uint32_t width,
                                         uint32_t height, uint32_t pitch) {
  if (!fb || !width || !height)
    return 0;
  if (width > 16384 || height > 16384)
    return 0;
  if (pitch < width * sizeof(uint32_t))
    return 0;
  if (pitch > width * sizeof(uint32_t) * 8)
    return 0;
  return 1;
}

static uint64_t intel_gfx_read_bar(const pci_device_t *pci_dev,
                                   uint8_t bar_offset) {
  uint32_t low;
  uint64_t addr;

  if (!pci_dev)
    return 0;

  low = pci_read32(pci_dev->bus, pci_dev->slot, pci_dev->func, bar_offset);
  if (low == 0 || low == 0xFFFFFFFF)
    return 0;
  if (low & 0x1)
    return 0;

  addr = (uint64_t)(low & 0xFFFFFFF0U);
  if ((low & 0x6) == 0x4) {
    uint32_t high =
        pci_read32(pci_dev->bus, pci_dev->slot, pci_dev->func, bar_offset + 4);
    addr |= ((uint64_t)high << 32);
  }

  return addr;
}

int intel_gfx_init(pci_device_t *pci_dev) {
  uint32_t *fb = NULL;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pitch = 0;
  int safe_framebuffer = 0;

  if (!intel_gfx_is_display_device(pci_dev))
    return -1;
  if (intel_gfx_state.initialized)
    return 0;
  if (!intel_gfx_is_supported_subclass(pci_dev)) {
    printk(KERN_WARNING "IGFX: Unsupported display subclass 0x%02x, skipping\n",
           pci_dev->subclass);
    return -1;
  }
  intel_gfx_state = (intel_gfx_state_t){0};
  intel_gfx_state.detected = true;
  intel_gfx_state.device_id = pci_dev->device_id;
  intel_gfx_state.irq = pci_dev->irq;
  intel_gfx_state.supported_device =
      intel_gfx_is_supported_device_id(pci_dev->device_id);
  extern void fb_get_info(uint32_t **buffer, uint32_t *width, uint32_t *height);
  extern uint32_t fb_get_pitch(void);

  fb_get_info(&fb, &width, &height);
  pitch = fb_get_pitch();
  safe_framebuffer = intel_gfx_framebuffer_is_sane(fb, width, height, pitch);

  if (!safe_framebuffer) {
    printk(KERN_WARNING
           "IGFX: Ignoring invalid framebuffer handoff fb=%p %ux%u pitch=%u\n",
           fb, width, height, pitch);
    intel_gfx_state.init_failed = true;
    return -1;
  }

  intel_gfx_state.initialized = true;
  intel_gfx_state.has_framebuffer = true;
  intel_gfx_state.framebuffer_fallback_active = true;
  intel_gfx_state.supports_gpu_rendering = false;
  intel_gfx_state.width = width;
  intel_gfx_state.height = height;
  intel_gfx_state.pitch = pitch;
  intel_gfx_state.mmio_base = 0;
  intel_gfx_state.aperture_base = 0;
  intel_gfx_state.mmio = NULL;
  intel_gfx_state.mmio_mapped = false;
  intel_gfx_state.probe_attempted = true;

  printk(KERN_INFO "IGFX: %s detected (%04x:%04x) at %02x:%02x.%x\n",
         intel_gfx_detect_name(pci_dev->device_id), pci_dev->vendor_id,
         pci_dev->device_id, pci_dev->bus, pci_dev->slot, pci_dev->func);
  printk(KERN_INFO "IGFX: Framebuffer video mode active %ux%u pitch=%u\n",
         intel_gfx_state.width, intel_gfx_state.height, intel_gfx_state.pitch);
  if (intel_gfx_state.supported_device) {
    printk(KERN_INFO
           "IGFX: Intel GPU generation recognized; native modesetting remains unimplemented\n");
  } else {
    printk(KERN_INFO
           "IGFX: Intel GPU compatible only through framebuffer handoff\n");
  }

  return 0;
}

int intel_gfx_is_ready(void) { return intel_gfx_state.initialized ? 1 : 0; }

int intel_gfx_detected(void) { return intel_gfx_state.detected ? 1 : 0; }

int intel_gfx_is_supported_device(void) {
  return intel_gfx_state.detected && intel_gfx_state.supported_device;
}

int intel_gfx_has_framebuffer(void) {
  return intel_gfx_state.initialized && intel_gfx_state.has_framebuffer;
}

int intel_gfx_supports_gpu_rendering(void) {
  return intel_gfx_state.initialized && intel_gfx_state.has_framebuffer &&
         intel_gfx_state.supports_gpu_rendering;
}

int intel_gfx_is_using_framebuffer_fallback(void) {
  return intel_gfx_state.detected && intel_gfx_state.has_framebuffer &&
         intel_gfx_state.framebuffer_fallback_active &&
         !intel_gfx_supports_gpu_rendering();
}

void intel_gfx_get_display_info(uint32_t *width, uint32_t *height,
                                uint32_t *pitch) {
  if (width)
    *width = intel_gfx_state.width;
  if (height)
    *height = intel_gfx_state.height;
  if (pitch)
    *pitch = intel_gfx_state.pitch;
}

const char *intel_gfx_get_name(void) {
  return intel_gfx_detect_name(intel_gfx_state.device_id);
}
