#ifndef _DRIVERS_TRACKPAD_H
#define _DRIVERS_TRACKPAD_H

#include "types.h"

typedef struct {
  int x;
  int y;
  int dx;
  int dy;
  int scroll_x;
  int scroll_y;
  uint8_t fingers;
  uint8_t buttons;
  uint8_t absolute;
  uint8_t touching;
} trackpad_report_t;

typedef int (*trackpad_poll_fn)(void *ctx, trackpad_report_t *out_report);

int trackpad_input_init(void);
void trackpad_input_poll(void);
void trackpad_input_set_bounds(int width, int height);
void trackpad_input_set_scale(int scale);
void trackpad_get_position(int *x, int *y);
int trackpad_get_buttons(void);
int trackpad_has_pointer(void);
void trackpad_submit_report(const trackpad_report_t *report);
int trackpad_decode_hid_report(const uint8_t *data, size_t len,
                               trackpad_report_t *out_report);
int trackpad_register_device(const char *bus_name, const char *name, void *ctx,
                             trackpad_poll_fn poll_fn);
int i2c_trackpad_register(const char *name, void *ctx,
                          trackpad_poll_fn poll_fn);
int spi_trackpad_register(const char *name, void *ctx,
                          trackpad_poll_fn poll_fn);

#endif
