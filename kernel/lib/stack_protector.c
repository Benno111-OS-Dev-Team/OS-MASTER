/*
 * Stack Protector Support
 *
 * Provides __stack_chk_guard and __stack_chk_fail for -fstack-protector-strong.
 */

#include "printk.h"

/* Stack canary value - initialized at boot with random value if available */
unsigned long __stack_chk_guard = 0xDEADBEEFCAFEBABEUL;

/*
 * Called when stack corruption is detected.
 * This function should never return.
 */
void __stack_chk_fail(void) {
  panic("STACK SMASHING DETECTED");
}
