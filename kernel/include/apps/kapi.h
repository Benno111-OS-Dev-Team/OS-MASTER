/*
 * OS8 Kernel API for Userspace Applications
 *
 * IMPORTANT: This header wraps the shared app ABI in shared-api/app_api.h.
 * Userspace programs depend on the exact field layout.
 */

#ifndef KAPI_H
#define KAPI_H

#include "types.h"
#include "app_api.h"

/* Initialize the kernel API */
void kapi_init(kapi_t *api);

/* Get the global kapi instance */
kapi_t *kapi_get(void);

/* Tick the timer */
void kapi_tick(void);

/* Launch an embedded application */
int app_run(const char *name, int argc, char **argv);

#endif /* KAPI_H */
