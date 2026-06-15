/*
 * OS8 - Intel Integrated Graphics Driver
 */

#ifndef DRIVERS_INTEL_GFX_H
#define DRIVERS_INTEL_GFX_H

#include "drivers/pci.h"
#include "types.h"

#define INTEL_GPU_VENDOR_ID 0x8086

int intel_gfx_init(pci_device_t *pci_dev);
int intel_gfx_detected(void);
int intel_gfx_is_ready(void);
int intel_gfx_is_supported_device(void);
int intel_gfx_has_framebuffer(void);
int intel_gfx_supports_gpu_rendering(void);
int intel_gfx_is_using_framebuffer_fallback(void);
void intel_gfx_get_display_info(uint32_t *width, uint32_t *height,
                                uint32_t *pitch);
const char *intel_gfx_get_name(void);

#endif
