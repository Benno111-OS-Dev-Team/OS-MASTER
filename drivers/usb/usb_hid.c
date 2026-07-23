/*
 * OS8 - USB HID Driver
 * Implements a minimal boot-keyboard path and shared input callbacks.
 */

#include "drivers/usb/usb.h"
#include "mm/kmalloc.h"
#include "printk.h"

#define USB_HID_MAX_KEYBOARDS 8
#define USB_HID_BOOT_REPORT_LEN 8

#define KEY_WINDOW_SWITCHER 0x110
#define KEY_CTRL_ALT_DEL 0x111

struct usb_hid_keyboard {
  struct usb_device *dev;
  uint8_t last_report[USB_HID_BOOT_REPORT_LEN];
  uint8_t modifiers;
  int active;
};

static struct usb_hid_keyboard g_usb_keyboards[USB_HID_MAX_KEYBOARDS];
static void (*g_key_callback)(int key) = 0;
static void (*g_gui_key_callback)(int key) = 0;

static const char hid_usage_to_ascii[256] = {
    ['\x04'] = 'a', ['\x05'] = 'b', ['\x06'] = 'c', ['\x07'] = 'd',
    ['\x08'] = 'e', ['\x09'] = 'f', ['\x0A'] = 'g', ['\x0B'] = 'h',
    ['\x0C'] = 'i', ['\x0D'] = 'j', ['\x0E'] = 'k', ['\x0F'] = 'l',
    ['\x10'] = 'm', ['\x11'] = 'n', ['\x12'] = 'o', ['\x13'] = 'p',
    ['\x14'] = 'q', ['\x15'] = 'r', ['\x16'] = 's', ['\x17'] = 't',
    ['\x18'] = 'u', ['\x19'] = 'v', ['\x1A'] = 'w', ['\x1B'] = 'x',
    ['\x1C'] = 'y', ['\x1D'] = 'z',
    ['\x1E'] = '1', ['\x1F'] = '2', ['\x20'] = '3', ['\x21'] = '4',
    ['\x22'] = '5', ['\x23'] = '6', ['\x24'] = '7', ['\x25'] = '8',
    ['\x26'] = '9', ['\x27'] = '0',
    ['\x28'] = '\n', ['\x29'] = 27,   ['\x2A'] = '\b', ['\x2B'] = '\t',
    ['\x2C'] = ' ',  ['\x2D'] = '-',  ['\x2E'] = '=',  ['\x2F'] = '[',
    ['\x30'] = ']',  ['\x31'] = '\\', ['\x33'] = ';',  ['\x34'] = '\'',
    ['\x35'] = '`',  ['\x36'] = ',',  ['\x37'] = '.',  ['\x38'] = '/',
};

static const char hid_usage_to_ascii_shifted[256] = {
    ['\x04'] = 'A', ['\x05'] = 'B', ['\x06'] = 'C', ['\x07'] = 'D',
    ['\x08'] = 'E', ['\x09'] = 'F', ['\x0A'] = 'G', ['\x0B'] = 'H',
    ['\x0C'] = 'I', ['\x0D'] = 'J', ['\x0E'] = 'K', ['\x0F'] = 'L',
    ['\x10'] = 'M', ['\x11'] = 'N', ['\x12'] = 'O', ['\x13'] = 'P',
    ['\x14'] = 'Q', ['\x15'] = 'R', ['\x16'] = 'S', ['\x17'] = 'T',
    ['\x18'] = 'U', ['\x19'] = 'V', ['\x1A'] = 'W', ['\x1B'] = 'X',
    ['\x1C'] = 'Y', ['\x1D'] = 'Z',
    ['\x1E'] = '!', ['\x1F'] = '@', ['\x20'] = '#', ['\x21'] = '$',
    ['\x22'] = '%', ['\x23'] = '^', ['\x24'] = '&', ['\x25'] = '*',
    ['\x26'] = '(', ['\x27'] = ')',
    ['\x28'] = '\n', ['\x29'] = 27,   ['\x2A'] = '\b', ['\x2B'] = '\t',
    ['\x2C'] = ' ',  ['\x2D'] = '_',  ['\x2E'] = '+',  ['\x2F'] = '{',
    ['\x30'] = '}',  ['\x31'] = '|',  ['\x33'] = ':',  ['\x34'] = '"',
    ['\x35'] = '~',  ['\x36'] = '<',  ['\x37'] = '>',  ['\x38'] = '?',
};

static int report_contains_usage(const uint8_t *report, uint8_t usage) {
  for (int i = 2; i < USB_HID_BOOT_REPORT_LEN; i++) {
    if (report[i] == usage)
      return 1;
  }
  return 0;
}

static void dispatch_key(int key) {
  if (!key)
    return;
  if (g_key_callback)
    g_key_callback(key);
  if (g_gui_key_callback)
    g_gui_key_callback(key);
}

static struct usb_hid_keyboard *usb_hid_find_keyboard(struct usb_device *dev) {
  if (!dev)
    return NULL;

  for (int i = 0; i < USB_HID_MAX_KEYBOARDS; i++) {
    if (g_usb_keyboards[i].active && g_usb_keyboards[i].dev == dev)
      return &g_usb_keyboards[i];
  }
  return NULL;
}

static int usb_hid_mod_active(uint8_t modifiers, uint8_t mask) {
  return (modifiers & mask) != 0;
}

static uint8_t usb_hid_active_modifiers(void) {
  uint8_t modifiers = 0;

  for (int i = 0; i < USB_HID_MAX_KEYBOARDS; i++) {
    if (!g_usb_keyboards[i].active)
      continue;
    modifiers |= g_usb_keyboards[i].modifiers;
  }

  return modifiers;
}

static void usb_hid_process_keypress(uint8_t modifiers, uint8_t usage) {
  int shift = usb_hid_mod_active(modifiers, 0x02 | 0x20);
  int ctrl = usb_hid_mod_active(modifiers, 0x01 | 0x10);
  int alt = usb_hid_mod_active(modifiers, 0x04 | 0x40);
  int key = 0;

  switch (usage) {
  case 0x4F:
    key = 0x103;
    break;
  case 0x50:
    key = 0x102;
    break;
  case 0x51:
    key = 0x101;
    break;
  case 0x52:
    key = 0x100;
    break;
  case 0x2B:
    if (alt)
      key = KEY_WINDOW_SWITCHER;
    break;
  case 0x4C:
    if (ctrl && alt)
      key = KEY_CTRL_ALT_DEL;
    break;
  default:
    break;
  }

  if (key) {
    dispatch_key(key);
    return;
  }

  {
    char ascii;

    if (ctrl) {
      char base = hid_usage_to_ascii[usage];
      if (base >= 'a' && base <= 'z')
        ascii = (char)(base - 'a' + 1);
      else if (base >= 'A' && base <= 'Z')
        ascii = (char)(base - 'A' + 1);
      else
        ascii = 0;
    } else {
      ascii = shift ? hid_usage_to_ascii_shifted[usage]
                    : hid_usage_to_ascii[usage];
    }

    if (ascii)
      dispatch_key(ascii);
  }
}

int usb_hid_init(struct usb_device *dev) {
  struct usb_hid_keyboard *kbd;

  if (!dev)
    return -1;
  if (usb_hid_find_keyboard(dev))
    return 0;

  for (int i = 0; i < USB_HID_MAX_KEYBOARDS; i++) {
    if (!g_usb_keyboards[i].active) {
      kbd = &g_usb_keyboards[i];
      for (int j = 0; j < USB_HID_BOOT_REPORT_LEN; j++)
        kbd->last_report[j] = 0;
      kbd->modifiers = 0;
      kbd->dev = dev;
      kbd->active = 1;
      dev->data = kbd;
      printk(KERN_INFO
             "USB-HID: Registered boot-keyboard candidate at address %u\n",
             dev->dev_addr);
      return 0;
    }
  }

  printk(KERN_WARNING "USB-HID: No free keyboard slots for address %u\n",
         dev->dev_addr);
  return -1;
}

void usb_hid_remove(struct usb_device *dev) {
  struct usb_hid_keyboard *kbd = usb_hid_find_keyboard(dev);

  if (!kbd)
    return;

  printk(KERN_INFO "USB-HID: Removed keyboard candidate at address %u\n",
         dev->dev_addr);
  kbd->dev = NULL;
  kbd->active = 0;
  kbd->modifiers = 0;
  for (int i = 0; i < USB_HID_BOOT_REPORT_LEN; i++)
    kbd->last_report[i] = 0;
  if (dev)
    dev->data = NULL;
}

void usb_hid_set_key_callback(void (*callback)(int key)) {
  g_key_callback = callback;
}

void usb_hid_set_gui_key_callback(void (*callback)(int key)) {
  g_gui_key_callback = callback;
}

void usb_hid_poll_keyboards(void) {
  /*
   * The boot-keyboard decoder is ready, but the current xHCI layer still
   * needs real interrupt-IN transfers to fetch reports from hardware.
   */
}

int usb_hid_submit_boot_keyboard_report(struct usb_device *dev,
                                        const uint8_t *report, size_t len) {
  struct usb_hid_keyboard *kbd;
  uint8_t modifiers;

  if (!dev || !report || len < USB_HID_BOOT_REPORT_LEN)
    return -1;

  kbd = usb_hid_find_keyboard(dev);
  if (!kbd)
    return -1;

  modifiers = report[0];
  kbd->modifiers = modifiers;
  modifiers = usb_hid_active_modifiers();
  for (int i = 2; i < USB_HID_BOOT_REPORT_LEN; i++) {
    uint8_t usage = report[i];
    if (!usage || report_contains_usage(kbd->last_report, usage))
      continue;
    usb_hid_process_keypress(modifiers, usage);
  }

  for (int i = 0; i < USB_HID_BOOT_REPORT_LEN; i++)
    kbd->last_report[i] = report[i];
  return 0;
}

void usb_hid_irq(struct usb_device *dev) {
  (void)dev;
  printk(KERN_DEBUG "USB-HID: Interrupt received\n");
}
