/*
 * OS8 - Simple VGA/Bochs Display Driver
 * 
 * Uses QEMU's bochs-display or std-vga device for graphics output.
 * This is a simpler approach than virtio-gpu.
 */

#include "types.h"
#include "printk.h"
#include "arch/arch.h"
#include "drivers/pci.h"
#include "mm/vmm.h"

/* ===================================================================== */
/* Bochs VBE (VGA BIOS Extensions) Registers */
/* ===================================================================== */

/* PCI BAR0 for bochs-display: framebuffer (usually at 0x10000000) */
/* PCI BAR2 for bochs-display: MMIO registers */

#define VBE_DISPI_MMIO_BASE     0x10001000UL  /* Bochs VBE MMIO registers */
#define VBE_FRAMEBUFFER_BASE    0x10000000UL  /* Default framebuffer location */
#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01D0

/* VBE register offsets (16-bit registers at 0x500 + index*2) */
#define VBE_DISPI_INDEX_ID          0
#define VBE_DISPI_INDEX_XRES        1
#define VBE_DISPI_INDEX_YRES        2
#define VBE_DISPI_INDEX_BPP         3
#define VBE_DISPI_INDEX_ENABLE      4
#define VBE_DISPI_INDEX_BANK        5
#define VBE_DISPI_INDEX_VIRT_WIDTH  6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7
#define VBE_DISPI_INDEX_X_OFFSET    8
#define VBE_DISPI_INDEX_Y_OFFSET    9
#define VBE_DISPI_INDEX_VIDEO_MEM   10

/* VBE enable flags */
#define VBE_DISPI_DISABLED      0x00
#define VBE_DISPI_ENABLED       0x01
#define VBE_DISPI_LFB_ENABLED   0x40
#define VBE_DISPI_NOCLEARMEM    0x80

/* VBE ID values */
#define VBE_DISPI_ID0           0xB0C0
#define VBE_DISPI_ID1           0xB0C1
#define VBE_DISPI_ID2           0xB0C2
#define VBE_DISPI_ID3           0xB0C3
#define VBE_DISPI_ID4           0xB0C4
#define VBE_DISPI_ID5           0xB0C5

#define BOCHS_MAX_DIMENSION     16384U

/* ===================================================================== */
/* Global State */
/* ===================================================================== */

static struct {
    volatile uint16_t *vbe_regs;
    volatile uint32_t *framebuffer;
    uint64_t mmio_phys;
    uint64_t framebuffer_phys;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    bool initialized;
} bochs_display = {0};

#if defined(ARCH_X86_64) || defined(ARCH_X86)
extern uint64_t limine_get_hhdm_offset(void);
#endif

static void *bochs_phys_to_virt(uint64_t paddr) {
#if defined(ARCH_X86_64) || defined(ARCH_X86)
    uint64_t hhdm = limine_get_hhdm_offset();
    if (hhdm)
        return (void *)(uintptr_t)(paddr + hhdm);
#endif
    return phys_to_virt((phys_addr_t)paddr);
}

static void bochs_resolve_mappings(void) {
    pci_device_t *dev = pci_find_device(0x1234, 0x1111);
    uint64_t mmio_phys = VBE_DISPI_MMIO_BASE;
    uint64_t framebuffer_phys = VBE_FRAMEBUFFER_BASE;

    if (dev) {
        if (dev->bar0)
            framebuffer_phys = dev->bar0;
        if (dev->bar2)
            mmio_phys = dev->bar2;
    }

    bochs_display.mmio_phys = mmio_phys;
    bochs_display.framebuffer_phys = framebuffer_phys;
    bochs_display.vbe_regs = (volatile uint16_t *)bochs_phys_to_virt(mmio_phys);
    bochs_display.framebuffer =
        (volatile uint32_t *)bochs_phys_to_virt(framebuffer_phys);
}

static bool bochs_mode_is_sane(uint32_t width, uint32_t height)
{
    if (!width || !height)
        return false;
    if (width > BOCHS_MAX_DIMENSION || height > BOCHS_MAX_DIMENSION)
        return false;
    return true;
}

/* ===================================================================== */
/* Register Access */
/* ===================================================================== */

static void vbe_write(uint16_t index, uint16_t value)
{
#if defined(ARCH_X86_64) || defined(ARCH_X86)
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
#else
    if (bochs_display.vbe_regs) {
        /* Bochs MMIO: index at offset 0x500, data at offset 0x502. */
        volatile uint16_t *idx = (volatile uint16_t *)((uintptr_t)bochs_display.vbe_regs + 0x500);
        volatile uint16_t *data = (volatile uint16_t *)((uintptr_t)bochs_display.vbe_regs + 0x502);
        *idx = index;
        *data = value;
    }
#endif
}

static uint16_t vbe_read(uint16_t index)
{
#if defined(ARCH_X86_64) || defined(ARCH_X86)
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
#else
    if (bochs_display.vbe_regs) {
        volatile uint16_t *idx = (volatile uint16_t *)((uintptr_t)bochs_display.vbe_regs + 0x500);
        volatile uint16_t *data = (volatile uint16_t *)((uintptr_t)bochs_display.vbe_regs + 0x502);
        *idx = index;
        return *data;
    }
    return 0;
#endif
}

/* ===================================================================== */
/* Framebuffer Operations */
/* ===================================================================== */

void bochs_clear(uint32_t color)
{
    if (!bochs_display.initialized) return;
    if (!bochs_display.framebuffer) return;
    
    uint32_t *fb = (uint32_t *)bochs_display.framebuffer;
    uint32_t pixels = bochs_display.width * bochs_display.height;
    
    for (uint32_t i = 0; i < pixels; i++) {
        fb[i] = color;
    }
}

void bochs_put_pixel(int x, int y, uint32_t color)
{
    if (!bochs_display.initialized) return;
    if (!bochs_display.framebuffer) return;
    if (x < 0 || x >= (int)bochs_display.width) return;
    if (y < 0 || y >= (int)bochs_display.height) return;
    
    bochs_display.framebuffer[y * bochs_display.width + x] = color;
}

/* ===================================================================== */
/* Initialization */
/* ===================================================================== */

int bochs_init(uint32_t width, uint32_t height)
{
    printk(KERN_INFO "BOCHS: Initializing display %ux%u\n", width, height);

    if (!bochs_mode_is_sane(width, height)) {
        printk(KERN_ERR "BOCHS: Invalid display mode %ux%u\n", width, height);
        return -1;
    }
    
    /* Set up register and framebuffer pointers */
    bochs_resolve_mappings();
    printk(KERN_INFO "BOCHS: MMIO phys=0x%llx virt=%p FB phys=0x%llx virt=%p\n",
           (unsigned long long)bochs_display.mmio_phys,
           (const void *)bochs_display.vbe_regs,
           (unsigned long long)bochs_display.framebuffer_phys,
           (const void *)bochs_display.framebuffer);

    if (!bochs_display.framebuffer) {
        printk(KERN_ERR "BOCHS: Framebuffer mapping unavailable\n");
        return -1;
    }
    
    /* Check for Bochs VBE */
    uint16_t vbe_id = vbe_read(VBE_DISPI_INDEX_ID);
    printk(KERN_INFO "BOCHS: VBE ID = 0x%04x\n", vbe_id);
    
    if (vbe_id < VBE_DISPI_ID0 || vbe_id > VBE_DISPI_ID5) {
        printk(KERN_ERR "BOCHS: VBE not detected (ID=0x%04x)\n", vbe_id);
        return -1;
    }
    
    /* Disable display during mode set */
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    
    /* Set resolution */
    vbe_write(VBE_DISPI_INDEX_XRES, width);
    vbe_write(VBE_DISPI_INDEX_YRES, height);
    vbe_write(VBE_DISPI_INDEX_BPP, 32);
    vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, width);
    vbe_write(VBE_DISPI_INDEX_VIRT_HEIGHT, height);
    vbe_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    
    /* Enable display with LFB */
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
    
    bochs_display.width = width;
    bochs_display.height = height;
    bochs_display.bpp = 32;
    bochs_display.pitch = width * 4;
    bochs_display.initialized = true;
    
    printk(KERN_INFO "BOCHS: Display initialized, FB at 0x%lx\n", 
           (unsigned long)bochs_display.framebuffer_phys);
    
    /* Clear screen to dark blue */
    bochs_clear(0x1E1E2E);
    
    return 0;
}

/* Get framebuffer info */
void bochs_get_info(uint32_t **buffer, uint32_t *width, uint32_t *height)
{
    if (buffer) *buffer = (uint32_t *)bochs_display.framebuffer;
    if (width) *width = bochs_display.width;
    if (height) *height = bochs_display.height;
}
