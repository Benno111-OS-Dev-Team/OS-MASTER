/*
 * UnixOS Kernel - Kernel Heap Allocator Implementation
 *
 * A simple bucket-based allocator for kernel memory.
 * Fixed to use direct memory region like VibeOS for reliability.
 */

#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "sync/spinlock.h"
#include "printk.h"

/* ===================================================================== */
/* Configuration */
/* ===================================================================== */

#define HEAP_SIZE                                                              \
  (128 * 1024 * 1024) /* 128MB kernel heap - 4K wallpapers need space */
#define MIN_ALLOC 32  /* Minimum allocation size */
#define MAX_ALLOC                                                              \
  (32 * 1024 * 1024) /* Maximum single allocation (32MB for large images) */

/* Fixed heap location - after kernel at 0x42000000 */
/* Kernel loads at 0x40200000, so 0x42000000 gives 30MB for kernel code/data */
#define HEAP_BASE 0x42000000

/* Block header */
struct block_header {
  size_t size;               /* Size of this block (including header) */
  uint32_t magic;            /* Magic number for validation */
  uint32_t flags;            /* Block flags */
  int32_t owner;             /* GC owner; 0 means manually managed kernel block */
  struct block_header *next; /* Next free block (if free) */
  struct block_header *prev; /* Previous block */
};

#define BLOCK_MAGIC_FREE 0xDEADBEEF
#define BLOCK_MAGIC_USED 0xCAFEBABE

#define BLOCK_FLAG_FREE 0x01

/* ===================================================================== */
/* Static data */
/* ===================================================================== */

static uint8_t *heap_start;
static uint8_t *heap_end;
static struct block_header *free_list;
static size_t heap_total;
static size_t heap_used;
static bool heap_initialized = false;
#ifdef ARCH_X86_64
static uint8_t x86_64_heap_storage[HEAP_SIZE] __attribute__((aligned(4096)));
#endif

/* IRQ-safe spinlock for heap operations */
static spinlock_t heap_lock = SPINLOCK_INIT;

static inline uint64_t lock_heap(void) {
  return spin_lock_irqsave(&heap_lock);
}

static inline void unlock_heap(uint64_t flags) {
  spin_unlock_irqrestore(&heap_lock, flags);
}

/* ===================================================================== */
/* Helper functions */
/* ===================================================================== */

static inline size_t align_up(size_t val, size_t align) {
  return (val + align - 1) & ~(align - 1);
}

static inline void *block_data(struct block_header *block) {
  return (void *)((uint8_t *)block + sizeof(struct block_header));
}

static inline struct block_header *data_to_block(void *ptr) {
  return (struct block_header *)((uint8_t *)ptr - sizeof(struct block_header));
}

static void kfree_block_locked(struct block_header *block) {
  heap_used -= block->size;

  /* Mark as free */
  block->magic = BLOCK_MAGIC_FREE;
  block->flags = BLOCK_FLAG_FREE;
  block->owner = 0;

  /* Add to front of free list */
  block->next = free_list;
  block->prev = NULL;
  if (free_list) {
    free_list->prev = block;
  }
  free_list = block;

  /* Coalesce with next physical block if it's free */
  struct block_header *next_physical =
      (struct block_header *)((uint8_t *)block + block->size);
  if ((uint8_t *)next_physical < heap_end &&
      next_physical->magic == BLOCK_MAGIC_FREE) {
    /* Remove next_physical from free list */
    if (next_physical->prev) {
      next_physical->prev->next = next_physical->next;
    } else {
      free_list = next_physical->next;
    }
    if (next_physical->next) {
      next_physical->next->prev = next_physical->prev;
    }
    /* Merge sizes */
    block->size += next_physical->size;
    /* Invalidate merged block's magic to prevent double-free */
    next_physical->magic = 0;
    next_physical->owner = 0;
  }
}

/* ===================================================================== */
/* Initialization */
/* ===================================================================== */

void kmalloc_init(void) {
  /* Use fixed memory region - no PMM dependency */
  /* This is like how VibeOS does it - simple and reliable */
#ifdef ARCH_X86_64
  heap_start = x86_64_heap_storage;
#else
  heap_start = (uint8_t *)HEAP_BASE;
#endif
  heap_end = heap_start + HEAP_SIZE;
  heap_total = HEAP_SIZE;
  heap_used = 0;

  /* Initialize single free block covering entire heap */
  free_list = (struct block_header *)heap_start;
  free_list->size = HEAP_SIZE;
  free_list->magic = BLOCK_MAGIC_FREE;
  free_list->flags = BLOCK_FLAG_FREE;
  free_list->owner = 0;
  free_list->next = NULL;
  free_list->prev = NULL;

  heap_initialized = true;

  printk(KERN_INFO "KMALLOC: Heap at 0x%lx - 0x%lx (%lu KB)\n",
         (unsigned long)heap_start, (unsigned long)heap_end,
         (unsigned long)(HEAP_SIZE / 1024));
}

/* ===================================================================== */
/* Allocation */
/* ===================================================================== */

void *_kmalloc(size_t size, uint32_t flags) {
  if (!heap_initialized) {
    kmalloc_init();
    if (!heap_initialized) {
      return NULL;
    }
  }

  if (size == 0 || size > MAX_ALLOC) {
    return NULL;
  }

  /* Align size and add header */
  size_t total_size = align_up(size + sizeof(struct block_header), MIN_ALLOC);

  uint64_t heap_flags = lock_heap();

  /* Find first fit */
  struct block_header *block = free_list;
  struct block_header *prev_free = NULL;

  while (block) {
    if (block->magic != BLOCK_MAGIC_FREE) {
      printk(KERN_ERR "KMALLOC: Corrupted free list!\n");
      unlock_heap(heap_flags);
      return NULL;
    }

    if (block->size >= total_size) {
      /* Found a suitable block */
      break;
    }

    prev_free = block;
    block = block->next;
  }

  if (!block) {
    /* No suitable block found */
    unlock_heap(heap_flags);
    return NULL;
  }

  /* Split block if it's much larger than needed */
  if (block->size >= total_size + sizeof(struct block_header) + MIN_ALLOC) {
    /* Create new free block from remainder */
    struct block_header *new_block =
        (struct block_header *)((uint8_t *)block + total_size);
    new_block->size = block->size - total_size;
    new_block->magic = BLOCK_MAGIC_FREE;
    new_block->flags = BLOCK_FLAG_FREE;
    new_block->owner = 0;
    new_block->next = block->next;
    new_block->prev = block;

    if (block->next) {
      block->next->prev = new_block;
    }

    block->size = total_size;
    block->next = new_block;
  }

  /* Remove block from free list */
  if (prev_free) {
    prev_free->next = block->next;
  } else {
    free_list = block->next;
  }

  if (block->next) {
    block->next->prev = prev_free;
  }

  /* Mark as used */
  block->magic = BLOCK_MAGIC_USED;
  block->flags = 0;
  block->owner = 0;
  block->next = NULL;

  heap_used += block->size;

  unlock_heap(heap_flags);

  void *ptr = block_data(block);

  /* Zero if requested */
  if (flags & GFP_ZERO) {
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < size; i++) {
      p[i] = 0;
    }
  }

  return ptr;
}

void *kzalloc(size_t size, uint32_t flags) {
  return kmalloc(size, flags | GFP_ZERO);
}

/* ===================================================================== */
/* Deallocation */
/* ===================================================================== */

void kfree(void *ptr) {
  if (!ptr) {
    return;
  }

  uint64_t heap_flags = lock_heap();
  struct block_header *block = data_to_block(ptr);

  /* Validate block under the heap lock so preemption can't race the free. */
  if (block->magic != BLOCK_MAGIC_USED) {
    printk(KERN_ERR "KMALLOC: kfree of invalid pointer %p (magic=0x%x)\n", ptr,
           block->magic);
    unlock_heap(heap_flags);
    return;
  }

  kfree_block_locked(block);

  unlock_heap(heap_flags);
}

int kmalloc_set_owner(void *ptr, int owner) {
  (void)ptr;
  (void)owner;
  return 0;
}

size_t kmalloc_collect_owner(int owner) {
  (void)owner;
  return 0;
}

/* ===================================================================== */
/* Reallocation */
/* ===================================================================== */

void *krealloc(void *ptr, size_t new_size, uint32_t flags) {
  if (!ptr) {
    return kmalloc(new_size, flags);
  }

  if (new_size == 0) {
    kfree(ptr);
    return NULL;
  }

  struct block_header *block = data_to_block(ptr);
  size_t old_size = block->size - sizeof(struct block_header);
  int32_t old_owner = block->owner;

  /* If new size fits in current block, just return */
  if (new_size <= old_size) {
    return ptr;
  }

  /* Allocate new block */
  void *new_ptr = kmalloc(new_size, flags);
  if (!new_ptr) {
    return NULL;
  }
  kmalloc_set_owner(new_ptr, old_owner);

  /* Copy old data */
  uint8_t *src = (uint8_t *)ptr;
  uint8_t *dst = (uint8_t *)new_ptr;
  for (size_t i = 0; i < old_size; i++) {
    dst[i] = src[i];
  }

  /* Free old block */
  kfree(ptr);

  return new_ptr;
}

/* ===================================================================== */
/* Statistics */
/* ===================================================================== */

void kmalloc_get_stats(size_t *total, size_t *used, size_t *free_mem) {
  if (total)
    *total = heap_total;
  if (used)
    *used = heap_used;
  if (free_mem)
    *free_mem = heap_total - heap_used;
}
