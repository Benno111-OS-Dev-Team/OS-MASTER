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
static uint64_t periodic_ticks;

void kintegrity_init(void) {
  uint64_t flags = spin_lock_irqsave(&integrity_lock);
  integrity_initialized = 1;
  last_phase = KINTEGRITY_PHASE_EARLY;
  periodic_ticks = 0;
  spin_unlock_irqrestore(&integrity_lock, flags);

  printk(KERN_INFO "INTEGRITY: runtime kernel checks enabled\n");
}

void kintegrity_mark_phase(kintegrity_phase_t phase, const char *name) {
  uint64_t flags;

  if (!integrity_initialized)
    kintegrity_init();

  flags = spin_lock_irqsave(&integrity_lock);
  if (phase < last_phase) {
    spin_unlock_irqrestore(&integrity_lock, flags);
    printk(KERN_CRIT "INTEGRITY: boot phase regression at %s (%d < %d)\n",
           name ? name : "(unnamed)", (int)phase, (int)last_phase);
    panic("Kernel boot flow integrity violation");
  }
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
