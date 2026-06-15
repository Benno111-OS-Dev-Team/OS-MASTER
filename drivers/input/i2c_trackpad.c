#include "drivers/trackpad.h"

int i2c_trackpad_register(const char *name, void *ctx,
                          trackpad_poll_fn poll_fn) {
  return trackpad_register_device("i2c", name, ctx, poll_fn);
}
