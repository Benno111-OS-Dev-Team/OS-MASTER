/*
 * OS8 Kernel Integrity Checks
 */

#include "integrity/integrity.h"
#include "mm/kmalloc.h"
#include "printk.h"
#include "sync/spinlock.h"

static DEFINE_SPINLOCK(integrity_lock);
static int integrity_initialized;
static kintegrity_phase_t last_phase;
static uint32_t phase_seen_mask;
static uint64_t periodic_ticks;

void kintegrity_init(void) {
  uint64_t flags = spin_lock_irqsave(&integrity_lock);
  integrity_initialized = 1;
  last_phase = KINTEGRITY_PHASE_EARLY;
  phase_seen_mask = 1U << KINTEGRITY_PHASE_EARLY;
  periodic_ticks = 0;
  spin_unlock_irqrestore(&integrity_lock, flags);

  printk(KERN_INFO "INTEGRITY: runtime kernel checks enabled\n");
}

void kintegrity_mark_phase(kintegrity_phase_t phase, const char *name) {
  uint64_t flags;
  uint32_t required_mask;

  if (!integrity_initialized)
    kintegrity_init();

  if (phase < KINTEGRITY_PHASE_EARLY || phase > KINTEGRITY_PHASE_READY) {
    printk(KERN_CRIT "INTEGRITY: invalid boot phase %d at %s\n", (int)phase,
           name ? name : "(unnamed)");
    panic("Kernel boot flow integrity violation");
  }

  flags = spin_lock_irqsave(&integrity_lock);
  if (phase < last_phase) {
    spin_unlock_irqrestore(&integrity_lock, flags);
    printk(KERN_CRIT "INTEGRITY: boot phase regression at %s (%d < %d)\n",
           name ? name : "(unnamed)", (int)phase, (int)last_phase);
    panic("Kernel boot flow integrity violation");
  }
  required_mask = (phase == KINTEGRITY_PHASE_EARLY)
                      ? 0
                      : ((1U << (uint32_t)phase) - 1U);
  if ((phase_seen_mask & required_mask) != required_mask) {
    uint32_t missing = required_mask & ~phase_seen_mask;
    spin_unlock_irqrestore(&integrity_lock, flags);
    printk(KERN_CRIT
           "INTEGRITY: boot phase gap at %s (phase %d missing mask 0x%x)\n",
           name ? name : "(unnamed)", (int)phase, missing);
    panic("Kernel boot flow integrity violation");
  }
  phase_seen_mask |= 1U << (uint32_t)phase;
  last_phase = phase;
  spin_unlock_irqrestore(&integrity_lock, flags);

  printk(KERN_INFO "INTEGRITY: phase %d checkpoint %s\n", (int)phase,
         name ? name : "(unnamed)");
  kintegrity_checkpoint(name);
}

void kintegrity_checkpoint(const char *name) {
  if (kmalloc_check_integrity(name ? name : "checkpoint") != 0)
    panic("Kernel heap integrity violation");
}

void kintegrity_periodic(void) {
  uint64_t flags;
  uint64_t tick;

  if (!integrity_initialized)
    return;

  flags = spin_lock_irqsave(&integrity_lock);
  periodic_ticks++;
  tick = periodic_ticks;
  spin_unlock_irqrestore(&integrity_lock, flags);

  if ((tick & 0x3f) == 0)
    kintegrity_checkpoint("periodic");
}
