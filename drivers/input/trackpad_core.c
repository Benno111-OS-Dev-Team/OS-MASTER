#include "drivers/trackpad.h"
#include "printk.h"

#define MAX_TRACKPAD_DEVS 8

typedef struct {
  int active;
  char bus_name[8];
  char name[32];
  void *ctx;
  trackpad_poll_fn poll_fn;
} trackpad_dev_t;

static trackpad_dev_t trackpad_devs[MAX_TRACKPAD_DEVS];
static int trackpad_dev_count = 0;
static int trackpad_x = 512;
static int trackpad_y = 384;
static int trackpad_buttons = 0;
static int trackpad_bounds_w = 1024;
static int trackpad_bounds_h = 768;
static int trackpad_scale = 2;

static void tp_copy(char *dst, const char *src, int max) {
  int i = 0;
  if (!dst || max <= 0)
    return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  while (src[i] && i < max - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static int tp_clamp(int value, int lo, int hi) {
  if (value < lo)
    return lo;
  if (value > hi)
    return hi;
  return value;
}

int trackpad_input_init(void) {
  trackpad_x = trackpad_bounds_w / 2;
  trackpad_y = trackpad_bounds_h / 2;
  trackpad_buttons = 0;
  return 0;
}

void trackpad_input_set_bounds(int width, int height) {
  if (width > 0)
    trackpad_bounds_w = width;
  if (height > 0)
    trackpad_bounds_h = height;
  trackpad_x = tp_clamp(trackpad_x, 0, trackpad_bounds_w - 1);
  trackpad_y = tp_clamp(trackpad_y, 0, trackpad_bounds_h - 1);
}

void trackpad_input_set_scale(int scale) {
  if (scale < 1)
    scale = 1;
  if (scale > 8)
    scale = 8;
  trackpad_scale = scale;
}

int trackpad_has_pointer(void) { return trackpad_dev_count > 0; }

void trackpad_get_position(int *x, int *y) {
  if (x)
    *x = trackpad_x;
  if (y)
    *y = trackpad_y;
}

int trackpad_get_buttons(void) {
  if (trackpad_buttons < 0)
    return 0;
  return trackpad_buttons & 0x1F;
}

void trackpad_submit_report(const trackpad_report_t *report) {
  int max_x = (trackpad_bounds_w > 0) ? trackpad_bounds_w - 1 : 0;
  int max_y = (trackpad_bounds_h > 0) ? trackpad_bounds_h - 1 : 0;

  if (!report)
    return;

  if (report->absolute) {
    trackpad_x = tp_clamp(report->x, 0, max_x);
    trackpad_y = tp_clamp(report->y, 0, max_y);
  } else {
    trackpad_x =
        tp_clamp(trackpad_x + report->dx * trackpad_scale, 0, max_x);
    trackpad_y =
        tp_clamp(trackpad_y + report->dy * trackpad_scale, 0, max_y);
  }

  trackpad_buttons = report->buttons & 0x1F;

  /* Basic two-finger scroll fallback until a wheel path exists in the GUI. */
  if (report->fingers >= 2 && report->scroll_y != 0) {
    trackpad_y =
        tp_clamp(trackpad_y - report->scroll_y * 12, 0, max_y);
  }
}

int trackpad_decode_hid_report(const uint8_t *data, size_t len,
                               trackpad_report_t *out_report) {
  if (!data || !out_report || len < 6)
    return -1;

  out_report->buttons = data[1] & 0x07;
  out_report->absolute = 1;
  out_report->touching = (data[1] & 0x80) ? 1 : 0;
  out_report->x = (int)(data[2] | (data[3] << 8));
  out_report->y = (int)(data[4] | (data[5] << 8));
  out_report->dx = 0;
  out_report->dy = 0;
  out_report->scroll_x = 0;
  out_report->scroll_y = 0;
  out_report->fingers = (len > 6) ? data[6] : (out_report->touching ? 1 : 0);

  if (len > 8) {
    out_report->scroll_x = (int8_t)data[7];
    out_report->scroll_y = (int8_t)data[8];
  }

  return 0;
}

int trackpad_register_device(const char *bus_name, const char *name, void *ctx,
                             trackpad_poll_fn poll_fn) {
  trackpad_dev_t *dev;

  if (!poll_fn || trackpad_dev_count >= MAX_TRACKPAD_DEVS)
    return -1;

  dev = &trackpad_devs[trackpad_dev_count++];
  dev->active = 1;
  dev->ctx = ctx;
  dev->poll_fn = poll_fn;
  tp_copy(dev->bus_name, bus_name ? bus_name : "tp", sizeof(dev->bus_name));
  tp_copy(dev->name, name ? name : "Trackpad", sizeof(dev->name));

  printk(KERN_INFO "TRACKPAD: Registered %s trackpad '%s'\n", dev->bus_name,
         dev->name);
  return 0;
}

void trackpad_input_poll(void) {
  for (int i = 0; i < trackpad_dev_count; i++) {
    trackpad_report_t report;
    int ret;

    if (!trackpad_devs[i].active || !trackpad_devs[i].poll_fn)
      continue;

    ret = trackpad_devs[i].poll_fn(trackpad_devs[i].ctx, &report);
    if (ret > 0) {
      trackpad_submit_report(&report);
    }
  }
}
