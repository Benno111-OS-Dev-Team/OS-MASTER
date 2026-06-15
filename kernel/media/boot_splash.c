/*
 * Shared boot splash image cache.
 */

#include "media/media.h"
#include "media/seed_assets.h"
#include "fs/vfs.h"
#include "printk.h"
#include "types.h"

static media_image_t g_boot_logo;
static int g_boot_logo_state;

int boot_splash_prepare(void) {
  if (g_boot_logo_state == 1)
    return 0;

  if (media_decode_png(bootstrap_logo_png, bootstrap_logo_png_len,
                       &g_boot_logo) == 0 &&
      g_boot_logo.width && g_boot_logo.height && g_boot_logo.pixels) {
    g_boot_logo_state = 1;
    printk(KERN_INFO "BOOT: cached splash logo %ux%u\n", g_boot_logo.width,
           g_boot_logo.height);
    return 0;
  }

  return -EINVAL;
}

const media_image_t *boot_splash_get_logo(void) {
  if (boot_splash_prepare() == 0)
    return &g_boot_logo;
  return NULL;
}
