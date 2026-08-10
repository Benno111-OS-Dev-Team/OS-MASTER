/*
 * UnixOS Kernel - Physical Memory Manager Implementation
 * 
 * Buddy allocator for physical page allocation.
 */

#include "mm/pmm.h"
#include "printk.h"

/* ===================================================================== */
/* Constants */
/* ===================================================================== */

#define MAX_ORDER           11      /* Maximum order (2^11 = 2048 pages = 8MB) */
#define BUDDY_MAX_PAGES     (1UL << MAX_ORDER)

/* Initial memory layout - will be updated from DTB/UEFI */
#define MEMORY_BASE         0x40000000  /* 1GB - typical for ARM64 */
#define MEMORY_SIZE         (256UL * 1024 * 1024)  /* 256MB - matches QEMU default */

/* ===================================================================== */
/* Static data */
/* ===================================================================== */

/* Free lists for each order */
static struct page *free_lists[MAX_ORDER + 1];

/* Page array - describes all physical pages */
static struct page *page_array;
static size_t total_pages;

/* Memory statistics */
static size_t free_pages_count;
static size_t total_memory;
static phys_addr_t memory_start;
static phys_addr_t memory_end;

/* Bitmap for early page tracking before page_array is set up */
/* Track 64K pages = 256MB - enough for initial boot */
#define EARLY_BITMAP_SIZE   (64 * 1024 / 8)  /* 8KB bitmap */
#define MAX_TRACKED_PAGES   (EARLY_BITMAP_SIZE * 8)
static uint8_t early_bitmap[EARLY_BITMAP_SIZE];
static bool early_mode = true;
static struct page page_storage[MAX_TRACKED_PAGES];

extern uint64_t limine_get_usable_memory_base(void) __attribute__((weak));
extern uint64_t limine_get_usable_memory_size(void) __attribute__((weak));
extern uint64_t limine_get_total_usable_memory(void) __attribute__((weak));

/* ===================================================================== */
/* Helper functions */
/* ===================================================================== */

static inline size_t order_to_pages(unsigned int order)
{
    return 1UL << order;
}

static inline size_t order_to_size(unsigned int order)
{
    return order_to_pages(order) * PAGE_SIZE;
}

static inline unsigned int size_to_order(size_t size)
{
    unsigned int order = 0;
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    while ((1UL << order) < pages && order < MAX_ORDER) {
        order++;
    }
    
    return order;
}

static uint64_t boot_usable_memory_base(void)
{
    if (limine_get_usable_memory_base) {
        return limine_get_usable_memory_base();
    }
    return 0;
}

static uint64_t boot_usable_memory_size(void)
{
    if (limine_get_usable_memory_size) {
        return limine_get_usable_memory_size();
    }
    return 0;
}

static uint64_t boot_total_usable_memory(void)
{
    if (limine_get_total_usable_memory) {
        return limine_get_total_usable_memory();
    }
    return 0;
}

static void pmm_configure_memory_range(void)
{
    uint64_t boot_base = boot_usable_memory_base();
    uint64_t boot_size = boot_usable_memory_size();
    uint64_t boot_total = boot_total_usable_memory();

    memory_start = MEMORY_BASE;
    total_memory = MEMORY_SIZE;

    if (boot_size >= PAGE_SIZE && boot_base + boot_size > boot_base) {
        phys_addr_t aligned_start = PAGE_ALIGN(boot_base);
        phys_addr_t aligned_end = PAGE_ALIGN_DOWN(boot_base + boot_size);

        if (aligned_end > aligned_start) {
            memory_start = aligned_start;
            total_memory = (size_t)(aligned_end - aligned_start);
        }
    }

    total_pages = total_memory / PAGE_SIZE;
    if (total_pages > MAX_TRACKED_PAGES) {
        total_pages = MAX_TRACKED_PAGES;
        total_memory = total_pages * PAGE_SIZE;
    }

    memory_end = memory_start + total_memory;

    if (boot_total > total_memory) {
        printk("PMM: Limine reports more usable RAM than this allocator can track\n");
    }
}

/* ===================================================================== */
/* Early boot allocator (bitmap-based) */
/* ===================================================================== */

static void early_mark_used(phys_addr_t addr)
{
    size_t pfn = PHYS_TO_PFN(addr - memory_start);
    if (pfn < EARLY_BITMAP_SIZE * 8) {
        early_bitmap[pfn / 8] |= (1 << (pfn % 8));
    }
}

static void early_mark_free(phys_addr_t addr)
{
    size_t pfn = PHYS_TO_PFN(addr - memory_start);
    if (pfn < EARLY_BITMAP_SIZE * 8) {
        early_bitmap[pfn / 8] &= ~(1 << (pfn % 8));
    }
}

static bool early_is_free(phys_addr_t addr)
{
    if (addr < memory_start || addr >= memory_end) {
        return false;
    }
    size_t pfn = PHYS_TO_PFN(addr - memory_start);
    if (pfn >= EARLY_BITMAP_SIZE * 8) {
        return false;
    }
    return !(early_bitmap[pfn / 8] & (1 << (pfn % 8)));
}

static bool pmm_range_valid(phys_addr_t addr, size_t count)
{
    if (count == 0 || addr < memory_start || addr >= memory_end) {
        return false;
    }
    if ((addr & (PAGE_SIZE - 1)) != 0) {
        return false;
    }
    if (count > (memory_end - addr) / PAGE_SIZE) {
        return false;
    }
    return true;
}

static phys_addr_t early_alloc_page(void)
{
    for (size_t i = 0; i < EARLY_BITMAP_SIZE; i++) {
        if (early_bitmap[i] != 0xFF) {
            for (int j = 0; j < 8; j++) {
                if (!(early_bitmap[i] & (1 << j))) {
                    early_bitmap[i] |= (1 << j);
                    phys_addr_t addr = memory_start + (i * 8 + j) * PAGE_SIZE;
                    return addr;
                }
            }
        }
    }
    return 0;
}

/* ===================================================================== */
/* Buddy allocator */
/* ===================================================================== */

static inline phys_addr_t buddy_address(phys_addr_t addr, unsigned int order)
{
    return addr ^ (PAGE_SIZE << order);
}

static void buddy_add_to_list(phys_addr_t addr, unsigned int order)
{
    struct page *page = pmm_phys_to_page(addr);
    if (!page) {
        return;
    }
    page->order = order;
    page->flags = PAGE_FLAG_FREE;
    page->next = free_lists[order];
    free_lists[order] = page;
}

static phys_addr_t buddy_remove_from_list(unsigned int order)
{
    if (!free_lists[order]) {
        return 0;
    }
    
    struct page *page = free_lists[order];
    free_lists[order] = page->next;
    page->next = NULL;
    page->flags = PAGE_FLAG_USED;
    
    return pmm_page_to_phys(page);
}

static int buddy_remove_specific(phys_addr_t addr, unsigned int order)
{
    struct page **link;

    if (order > MAX_ORDER) {
        return 0;
    }

    link = &free_lists[order];
    while (*link) {
        struct page *page = *link;
        if (pmm_page_to_phys(page) == addr) {
            *link = page->next;
            page->next = NULL;
            page->flags = PAGE_FLAG_USED;
            return 1;
        }
        link = &page->next;
    }
    return 0;
}

static void buddy_add_free_range(phys_addr_t start, phys_addr_t end)
{
    phys_addr_t addr = PAGE_ALIGN(start);

    end = PAGE_ALIGN_DOWN(end);
    while (addr < end) {
        unsigned int order = MAX_ORDER;
        size_t remaining_pages = (size_t)((end - addr) / PAGE_SIZE);

        while (order > 0) {
            size_t block_size = order_to_size(order);
            if (((addr - memory_start) & (block_size - 1)) == 0 &&
                order_to_pages(order) <= remaining_pages) {
                break;
            }
            order--;
        }

        for (size_t i = 0; i < order_to_pages(order); i++) {
            struct page *page = pmm_phys_to_page(addr + i * PAGE_SIZE);
            if (page) {
                page->flags = PAGE_FLAG_FREE;
                page->order = order;
                page->next = NULL;
            }
        }
        buddy_add_to_list(addr, order);
        free_pages_count += order_to_pages(order);
        addr += order_to_size(order);
    }
}

static void buddy_init_from_early_bitmap(void)
{
    page_array = page_storage;

    for (size_t i = 0; i < total_pages; i++) {
        page_array[i].flags = PAGE_FLAG_USED;
        page_array[i].order = 0;
        page_array[i].next = NULL;
        page_array[i].slab = NULL;
        atomic_set(&page_array[i].refcount, 0);
    }

    free_pages_count = 0;
    phys_addr_t range_start = 0;
    for (size_t i = 0; i < total_pages; i++) {
        phys_addr_t addr = memory_start + i * PAGE_SIZE;
        if (early_is_free(addr)) {
            if (!range_start) {
                range_start = addr;
            }
            continue;
        }

        if (range_start) {
            buddy_add_free_range(range_start, addr);
            range_start = 0;
        }
    }

    if (range_start) {
        buddy_add_free_range(range_start, memory_end);
    }

    early_mode = false;
}

/* ===================================================================== */
/* Public functions */
/* ===================================================================== */

int pmm_init(void)
{
    printk("PMM: Starting init\n");

    pmm_configure_memory_range();
    
    printk("PMM: Memory configured\n");
    
    /* Initialize free lists */
    for (int i = 0; i <= MAX_ORDER; i++) {
        free_lists[i] = NULL;
    }
    
    printk("PMM: Free lists cleared\n");
    
    /* Skip bitmap clearing - BSS should already be zero */
    printk("PMM: Skipping bitmap clear (BSS pre-zeroed)\n");
    
    /* Reserve kernel memory */
    extern char __kernel_start[];
    extern char __kernel_end[];
    
    phys_addr_t kernel_start = (phys_addr_t)__kernel_start;
    phys_addr_t kernel_end = (phys_addr_t)__kernel_end;
    
    printk("PMM: Got kernel addresses\n");
    
    /* Mark kernel pages as used */
    for (phys_addr_t addr = PAGE_ALIGN_DOWN(kernel_start);
         addr < PAGE_ALIGN(kernel_end);
         addr += PAGE_SIZE) {
        early_mark_used(addr);
    }
    
    printk("PMM: Kernel pages marked\n");
    
    /* Count free pages */
    free_pages_count = 0;
    for (size_t i = 0; i < total_pages && i < EARLY_BITMAP_SIZE * 8; i++) {
        if (!(early_bitmap[i / 8] & (1 << (i % 8)))) {
            free_pages_count++;
        }
    }
    
    buddy_init_from_early_bitmap();

    printk("PMM: Buddy allocator initialized\n");
    
    return 0;
}

phys_addr_t pmm_alloc_page(void)
{
    return pmm_alloc_pages(0);
}

phys_addr_t pmm_alloc_pages(unsigned int order)
{
    if (order > MAX_ORDER) {
        return 0;
    }
    
    if (early_mode) {
        /* Allocate contiguous pages in early mode */
        size_t count = order_to_pages(order);
        phys_addr_t start = 0;
        size_t found = 0;
        
        for (phys_addr_t addr = memory_start;
             addr < memory_end;
             addr += PAGE_SIZE) {
            if (early_is_free(addr)) {
                if (found == 0) {
                    start = addr;
                }
                found++;
                if (found == count) {
                    /* Mark all as used */
                    for (size_t i = 0; i < count; i++) {
                        early_mark_used(start + i * PAGE_SIZE);
                    }
                    free_pages_count -= count;
                    return start;
                }
            } else {
                found = 0;
            }
        }
        return 0;
    }
    
    /* Buddy allocator */
    for (unsigned int o = order; o <= MAX_ORDER; o++) {
        phys_addr_t addr = buddy_remove_from_list(o);
        if (addr) {
            /* Split larger blocks if needed */
            while (o > order) {
                o--;
                phys_addr_t buddy = buddy_address(addr, o);
                buddy_add_to_list(buddy, o);
            }
            free_pages_count -= order_to_pages(order);
            return addr;
        }
    }
    
    return 0;
}

void pmm_free_page(phys_addr_t addr)
{
    pmm_free_pages(addr, 0);
}

void pmm_free_pages(phys_addr_t addr, unsigned int order)
{
    if (!addr || order > MAX_ORDER) {
        return;
    }
    size_t count = order_to_pages(order);
    if (!pmm_range_valid(addr, count)) {
        return;
    }
    
    if (early_mode) {
        for (size_t i = 0; i < count; i++) {
            if (early_is_free(addr + i * PAGE_SIZE)) {
                return;
            }
        }
        for (size_t i = 0; i < count; i++) {
            early_mark_free(addr + i * PAGE_SIZE);
        }
        free_pages_count += count;
        return;
    }
    
    /* Buddy allocator - coalesce with buddy if possible */
    while (order < MAX_ORDER) {
        phys_addr_t buddy = buddy_address(addr, order);
        struct page *buddy_page = pmm_phys_to_page(buddy);
        
        /* Check if buddy is free and same order */
        if (buddy_page && buddy_page->flags == PAGE_FLAG_FREE &&
            buddy_page->order == order &&
            buddy_remove_specific(buddy, order)) {
            /* Merge with buddy */
            if (buddy < addr) {
                addr = buddy;
            }
            order++;
        } else {
            break;
        }
    }
    
    buddy_add_to_list(addr, order);
    free_pages_count += count;
}

size_t pmm_get_free_memory(void)
{
    return free_pages_count * PAGE_SIZE;
}

size_t pmm_get_total_memory(void)
{
    return total_memory;
}

phys_addr_t pmm_page_to_phys(struct page *page)
{
    if (!page_array || !page) {
        return 0;
    }
    size_t index = page - page_array;
    return memory_start + index * PAGE_SIZE;
}

struct page *pmm_phys_to_page(phys_addr_t addr)
{
    if (!page_array || addr < memory_start || addr >= memory_end ||
        (addr & (PAGE_SIZE - 1)) != 0) {
        return NULL;
    }
    size_t index = PHYS_TO_PFN(addr - memory_start);
    return &page_array[index];
}
