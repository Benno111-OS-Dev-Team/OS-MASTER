/*
 * USB core glue (xHCI/EHCI + HID keyboard)
 */

#include "../include/usb.h"
#include "../include/gui.h"

/* Forward declarations */
void usb_xhci_init(void);
void usb_xhci_poll(void);
int usb_xhci_ready(void);

void usb_ehci_init(void);
void usb_ehci_poll(void);
int usb_ehci_ready(void);

static usb_keyboard_callback_t keyboard_cb = 0;
static const int usb_report_size = 8;

#define USB_REPORT_QUEUE_SIZE 16
typedef struct {
  uint8_t data[8];
  int len;
} usb_report_t;

static usb_report_t report_queue[USB_REPORT_QUEUE_SIZE];
static int report_head = 0;
static int report_tail = 0;

void usb_set_keyboard_callback(usb_keyboard_callback_t cb) {
  keyboard_cb = cb;
}

void usb_hid_handle_key(int key) {
  if (keyboard_cb) {
    keyboard_cb(key);
  }
}

void usb_submit_hid_report(const uint8_t *rep, int len) {
  if (!rep || len <= 0) {
    return;
  }

  int next = (report_head + 1) % USB_REPORT_QUEUE_SIZE;
  if (next == report_tail) {
    return; /* drop when queue is full */
  }
  int copy_len = len;
  if (copy_len > usb_report_size) {
    copy_len = usb_report_size;
  }
  for (int i = 0; i < copy_len; i++) {
    report_queue[report_head].data[i] = rep[i];
  }
  report_queue[report_head].len = copy_len;
  report_head = next;
}

void usb_init(void) {
  usb_xhci_init();
  usb_ehci_init();
}

void usb_poll(void) {
  if (usb_xhci_ready()) {
    usb_xhci_poll();
  }
  if (usb_ehci_ready()) {
    usb_ehci_poll();
  }

  while (report_tail != report_head) {
    usb_report_t *rep = &report_queue[report_tail];
    usb_hid_kbd_handle_report(rep->data, rep->len);
    report_tail = (report_tail + 1) % USB_REPORT_QUEUE_SIZE;
  }
}
