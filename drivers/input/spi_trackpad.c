#include "drivers/trackpad.h"

int spi_trackpad_register(const char *name, void *ctx,
                          trackpad_poll_fn poll_fn) {
  return trackpad_register_device("spi", name, ctx, poll_fn);
}
