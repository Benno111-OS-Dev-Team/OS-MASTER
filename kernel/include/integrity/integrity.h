/*
 * OS8 Kernel Integrity Checks
 *
 * Lightweight runtime checks for boot code flow and kernel memory metadata.
 */

#ifndef _KERNEL_INTEGRITY_H
#define _KERNEL_INTEGRITY_H

#include "types.h"

typedef enum {
  KINTEGRITY_PHASE_EARLY = 0,
  KINTEGRITY_PHASE_ARCH,
  KINTEGRITY_PHASE_MEMORY,
  KINTEGRITY_PHASE_PROCESS,
  KINTEGRITY_PHASE_FS,
  KINTEGRITY_PHASE_DRIVERS,
  KINTEGRITY_PHASE_READY,
} kintegrity_phase_t;

void kintegrity_init(void);
void kintegrity_mark_phase(kintegrity_phase_t phase, const char *name);
void kintegrity_checkpoint(const char *name);
void kintegrity_periodic(void);

#endif /* _KERNEL_INTEGRITY_H */
