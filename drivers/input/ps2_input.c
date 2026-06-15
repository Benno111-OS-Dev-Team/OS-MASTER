#include "printk.h"
#include "types.h"
#include "drivers/trackpad.h"

#if defined(ARCH_X86) || defined(ARCH_X86_64)

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_COMMAND_PORT 0x64

#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL 0x02
#define PS2_STATUS_MOUSE_DATA 0x20

#define PS2_CMD_READ_CONFIG 0x20
#define PS2_CMD_WRITE_CONFIG 0x60
#define PS2_CMD_DISABLE_MOUSE 0xA7
#define PS2_CMD_ENABLE_MOUSE 0xA8
#define PS2_CMD_TEST_MOUSE 0xA9
#define PS2_CMD_TEST_CONTROLLER 0xAA
#define PS2_CMD_DISABLE_KB 0xAD
#define PS2_CMD_ENABLE_KB 0xAE
#define PS2_CMD_WRITE_MOUSE 0xD4

#define KB_CMD_DEFAULTS 0xF6
#define KB_CMD_ENABLE 0xF4

#define MOUSE_CMD_RESET 0xFF
#define MOUSE_CMD_DEFAULTS 0xF6
#define MOUSE_CMD_ENABLE 0xF4
#define MOUSE_CMD_SET_RATE 0xF3
#define MOUSE_CMD_GET_ID 0xF2

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void io_wait(void) { __asm__ volatile("outb %%al, $0x80" : : "a"(0)); }

static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int alt_pressed = 0;
static int caps_lock = 0;
static int extended_scancode = 0;
static int boot_verbose_requested = 0;

static int mouse_x = 400;
static int mouse_y = 300;
static int mouse_buttons = 0;
static int mouse_max_x = 1024;
static int mouse_max_y = 768;
static int mouse_scale = 2;
static int mouse_packet_size = 3;
static int mouse_has_wheel = 0;
static int mouse_has_extra_buttons = 0;

static uint8_t mouse_packet[4];
static int mouse_packet_index = 0;

static void (*key_callback)(int key) = 0;
static void (*gui_key_callback)(int key) = 0;

static const char scancode_to_ascii[128] = {
    0,    27,   '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',
    '-',  '=',  '\b', '\t', 'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',
    'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',  'd',  'f',  'g',  'h',
    'j',  'k',  'l',  ';',  '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',
    'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',  0,    ' ',  0,    0,
};

static const char scancode_to_ascii_shift[128] = {
    0,    27,   '!',  '@',  '#',  '$',  '%',  '^',  '&',  '*',  '(',  ')',
    '_',  '+',  '\b', '\t', 'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',
    'O',  'P',  '{',  '}',  '\n', 0,    'A',  'S',  'D',  'F',  'G',  'H',
    'J',  'K',  'L',  ':',  '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',
    'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',  0,    ' ',  0,    0,
};

#define KEY_UP 0x100
#define KEY_DOWN 0x101
#define KEY_LEFT 0x102
#define KEY_RIGHT 0x103
#define KEY_WINDOW_SWITCHER 0x110
#define KEY_CTRL_ALT_DEL 0x111

static void ps2_wait_input(void) {
  int timeout = 100000;
  while ((inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) && timeout-- > 0) {
    io_wait();
  }
}

static void ps2_wait_output(void) {
  int timeout = 100000;
  while (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) && timeout-- > 0) {
    io_wait();
  }
}

static void ps2_send_command(uint8_t cmd) {
  ps2_wait_input();
  outb(PS2_COMMAND_PORT, cmd);
}

static void ps2_send_data(uint8_t data) {
  ps2_wait_input();
  outb(PS2_DATA_PORT, data);
}

static uint8_t ps2_read_data(void) {
  ps2_wait_output();
  return inb(PS2_DATA_PORT);
}

static void ps2_flush_output(void) {
  int timeout = 1000;
  while ((inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) && timeout-- > 0) {
    inb(PS2_DATA_PORT);
    io_wait();
  }
}

static int ps2_keyboard_send(uint8_t cmd) {
  int retries = 3;
  while (retries-- > 0) {
    ps2_wait_input();
    outb(PS2_DATA_PORT, cmd);
    ps2_wait_output();
    switch (inb(PS2_DATA_PORT)) {
    case 0xFA:
      return 0;
    case 0xFE:
      break;
    default:
      break;
    }
  }
  return -1;
}

static void ps2_mouse_write(uint8_t data) {
  ps2_send_command(PS2_CMD_WRITE_MOUSE);
  ps2_send_data(data);
}

static int ps2_mouse_command(uint8_t cmd, uint8_t *response) {
  int retries = 3;

  while (retries-- > 0) {
    ps2_flush_output();
    ps2_mouse_write(cmd);
    ps2_wait_output();

    if (inb(PS2_DATA_PORT) != 0xFA) {
      continue;
    }

    if (response) {
      ps2_wait_output();
      *response = inb(PS2_DATA_PORT);
    }
    return 0;
  }

  return -1;
}

static int ps2_mouse_set_rate(uint8_t rate) {
  int retries = 3;

  while (retries-- > 0) {
    ps2_flush_output();
    ps2_mouse_write(MOUSE_CMD_SET_RATE);
    ps2_wait_output();
    if (inb(PS2_DATA_PORT) != 0xFA) {
      continue;
    }

    ps2_flush_output();
    ps2_mouse_write(rate);
    ps2_wait_output();
    if (inb(PS2_DATA_PORT) == 0xFA) {
      return 0;
    }
  }

  return -1;
}

static uint8_t ps2_mouse_get_id(void) {
  uint8_t id = 0;
  if (ps2_mouse_command(MOUSE_CMD_GET_ID, &id) == 0) {
    return id;
  }
  return 0;
}

static void ps2_mouse_detect_extensions(void) {
  uint8_t id;

  mouse_packet_size = 3;
  mouse_has_wheel = 0;
  mouse_has_extra_buttons = 0;

  /* IntelliMouse-compatible sequence: enables 4-byte packets on many touchpads. */
  if (ps2_mouse_set_rate(200) == 0 && ps2_mouse_set_rate(100) == 0 &&
      ps2_mouse_set_rate(80) == 0) {
    id = ps2_mouse_get_id();
    if (id == 0x03 || id == 0x04) {
      mouse_packet_size = 4;
      mouse_has_wheel = 1;
      mouse_has_extra_buttons = (id == 0x04);
      printk(KERN_INFO "INPUT: PS/2 extended pointer detected (id=0x%x)\n",
             id);
      return;
    }
  }

  /* Explorer sequence used by some newer PS/2 touchpads for extra buttons. */
  if (ps2_mouse_set_rate(200) == 0 && ps2_mouse_set_rate(200) == 0 &&
      ps2_mouse_set_rate(80) == 0) {
    id = ps2_mouse_get_id();
    if (id == 0x04) {
      mouse_packet_size = 4;
      mouse_has_wheel = 1;
      mouse_has_extra_buttons = 1;
      printk(KERN_INFO "INPUT: PS/2 explorer pointer detected (id=0x%x)\n",
             id);
      return;
    }
  }

  printk(KERN_INFO "INPUT: PS/2 standard pointer detected\n");
}

static void dispatch_key(int key) {
  (void)ctrl_pressed;
  (void)alt_pressed;

  if (!key) {
    return;
  }

  if (key == 'v' || key == 'V') {
    boot_verbose_requested = 1;
  }

  if (key_callback) {
    key_callback(key);
  }
  if (gui_key_callback) {
    gui_key_callback(key);
  }
}

static void handle_mouse_byte(uint8_t data) {
  mouse_packet[mouse_packet_index++] = data;
  if (mouse_packet_index == 1 && !(data & 0x08)) {
    mouse_packet_index = 0;
    return;
  }
  if (mouse_packet_index < mouse_packet_size) {
    return;
  }

  mouse_packet_index = 0;

  uint8_t flags = mouse_packet[0];
  int dx = mouse_packet[1];
  int dy = mouse_packet[2];

  if (flags & 0x10)
    dx -= 256;
  if (flags & 0x20)
    dy -= 256;
  if (flags & 0x40)
    dx = 0;
  if (flags & 0x80)
    dy = 0;

  mouse_x += dx * mouse_scale;
  mouse_y -= dy * mouse_scale;

  if (mouse_x < 0)
    mouse_x = 0;
  if (mouse_y < 0)
    mouse_y = 0;
  if (mouse_x >= mouse_max_x)
    mouse_x = mouse_max_x - 1;
  if (mouse_y >= mouse_max_y)
    mouse_y = mouse_max_y - 1;

  mouse_buttons = 0;
  if (flags & 0x01)
    mouse_buttons |= 1;
  if (flags & 0x02)
    mouse_buttons |= 2;
  if (flags & 0x04)
    mouse_buttons |= 4;

  if (mouse_packet_size >= 4) {
    uint8_t ext = mouse_packet[3];

    if (mouse_has_extra_buttons) {
      if (ext & 0x10)
        mouse_buttons |= 8;
      if (ext & 0x20)
        mouse_buttons |= 16;
    }

    if (mouse_has_wheel) {
      int wheel = ext & 0x0F;
      if (wheel & 0x08)
        wheel -= 16;

      /* Basic touchpad-friendly scroll fallback: convert wheel to small vertical motion. */
      if (wheel > 0) {
        mouse_y -= wheel * 12;
      } else if (wheel < 0) {
        mouse_y += (-wheel) * 12;
      }

      if (mouse_y < 0)
        mouse_y = 0;
      if (mouse_y >= mouse_max_y)
        mouse_y = mouse_max_y - 1;
    }
  }
}

static void handle_keyboard_byte(uint8_t scancode) {
  int key = 0;

  if (scancode == 0xE0) {
    extended_scancode = 1;
    return;
  }
  if (scancode == 0xE1) {
    extended_scancode = 2;
    return;
  }
  if (extended_scancode == 2) {
    extended_scancode--;
    return;
  }

  if (scancode & 0x80) {
    uint8_t released = scancode & 0x7F;
    if (extended_scancode) {
      extended_scancode = 0;
      if (released == 0x1D)
        ctrl_pressed = 0;
      else if (released == 0x38)
        alt_pressed = 0;
    } else {
      switch (released) {
      case 0x2A:
      case 0x36:
        shift_pressed = 0;
        break;
      case 0x1D:
        ctrl_pressed = 0;
        break;
      case 0x38:
        alt_pressed = 0;
        break;
      default:
        break;
      }
    }
    return;
  }

  if (extended_scancode) {
    extended_scancode = 0;
    switch (scancode) {
    case 0x48:
      key = KEY_UP;
      break;
    case 0x50:
      key = KEY_DOWN;
      break;
    case 0x4B:
      key = KEY_LEFT;
      break;
    case 0x4D:
      key = KEY_RIGHT;
      break;
    case 0x1D:
      ctrl_pressed = 1;
      return;
    case 0x38:
      alt_pressed = 1;
      return;
    case 0x53:
      if (ctrl_pressed && alt_pressed)
        key = KEY_CTRL_ALT_DEL;
      else
        key = '\b';
      break;
    case 0x1C:
      key = '\n';
      break;
    case 0x5B:
      key = 125;
      break;
    case 0x5C:
      key = 126;
      break;
    default:
      return;
    }
    dispatch_key(key);
    return;
  }

  switch (scancode) {
  case 0x2A:
  case 0x36:
    shift_pressed = 1;
    return;
  case 0x1D:
    ctrl_pressed = 1;
    return;
  case 0x38:
    alt_pressed = 1;
    return;
  case 0x3A:
    caps_lock = !caps_lock;
    return;
  default:
    break;
  }

  switch (scancode) {
  case 0x48:
    key = KEY_UP;
    break;
  case 0x50:
    key = KEY_DOWN;
    break;
  case 0x4B:
    key = KEY_LEFT;
    break;
  case 0x4D:
    key = KEY_RIGHT;
    break;
  default:
    if (scancode < 128) {
      if (scancode == 0x0F && alt_pressed) {
        key = KEY_WINDOW_SWITCHER;
        break;
      }
      int use_shift = shift_pressed;
      char base = scancode_to_ascii[scancode];
      if (caps_lock && base >= 'a' && base <= 'z') {
        use_shift = !use_shift;
      }
      key = use_shift ? scancode_to_ascii_shift[scancode]
                      : scancode_to_ascii[scancode];
    }
    break;
  }

  dispatch_key(key);
}

int input_init(void) {
  uint8_t config;

  printk(KERN_INFO "INPUT: Initializing PS/2 keyboard and mouse\n");
  trackpad_input_init();

  ps2_flush_output();
  ps2_send_command(PS2_CMD_DISABLE_KB);
  ps2_send_command(PS2_CMD_DISABLE_MOUSE);
  ps2_flush_output();

  ps2_send_command(PS2_CMD_READ_CONFIG);
  config = ps2_read_data();
  config &= ~0x03;
  config |= 0x44;
  config &= ~0x30;
  ps2_send_command(PS2_CMD_WRITE_CONFIG);
  ps2_send_data(config);

  ps2_send_command(PS2_CMD_TEST_CONTROLLER);
  for (int i = 0; i < 10000; i++) {
    if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
      (void)inb(PS2_DATA_PORT);
      break;
    }
    io_wait();
  }

  ps2_send_command(PS2_CMD_WRITE_CONFIG);
  ps2_send_data(config);
  ps2_send_command(PS2_CMD_ENABLE_KB);
  ps2_send_command(PS2_CMD_ENABLE_MOUSE);
  ps2_send_command(PS2_CMD_TEST_MOUSE);
  ps2_flush_output();

  (void)ps2_keyboard_send(KB_CMD_DEFAULTS);
  (void)ps2_keyboard_send(KB_CMD_ENABLE);

  for (int retry = 0; retry < 3; retry++) {
    ps2_flush_output();
    (void)ps2_mouse_command(MOUSE_CMD_RESET, NULL);
    for (int i = 0; i < 20000; i++) {
      io_wait();
    }
    ps2_flush_output();
    (void)ps2_mouse_command(MOUSE_CMD_DEFAULTS, NULL);
    ps2_flush_output();
    (void)ps2_mouse_set_rate(100);
    ps2_flush_output();
    ps2_mouse_detect_extensions();
    ps2_flush_output();
    (void)ps2_mouse_command(MOUSE_CMD_ENABLE, NULL);
    ps2_flush_output();
  }

  ps2_send_command(PS2_CMD_READ_CONFIG);
  config = ps2_read_data();
  config |= 0x03;
  config &= ~0x30;
  ps2_send_command(PS2_CMD_WRITE_CONFIG);
  ps2_send_data(config);
  ps2_flush_output();

  printk(KERN_INFO "INPUT: PS/2 ready\n");
  return 0;
}

void input_set_key_callback(void (*callback)(int key)) { key_callback = callback; }

void input_set_gui_key_callback(void (*callback)(int key)) {
  gui_key_callback = callback;
}

void input_set_mouse_bounds(int width, int height) {
  if (width > 0)
    mouse_max_x = width;
  if (height > 0)
    mouse_max_y = height;
  mouse_x = mouse_max_x / 2;
  mouse_y = mouse_max_y / 2;
  trackpad_input_set_bounds(width, height);
}

void input_set_mouse_scale(int scale) {
  if (scale < 1)
    scale = 1;
  if (scale > 8)
    scale = 8;
  mouse_scale = scale;
  trackpad_input_set_scale(scale);
}

void input_poll(void) {
  trackpad_input_poll();

  for (int i = 0; i < 64; i++) {
    uint8_t status = inb(PS2_STATUS_PORT);
    if (!(status & PS2_STATUS_OUTPUT_FULL)) {
      break;
    }

    uint8_t data = inb(PS2_DATA_PORT);
    if (status & PS2_STATUS_MOUSE_DATA) {
      handle_mouse_byte(data);
    } else {
      handle_keyboard_byte(data);
    }
  }
}

int input_boot_verbose_requested(void) { return boot_verbose_requested; }

void mouse_get_position(int *x, int *y) {
  if (trackpad_has_pointer()) {
    trackpad_get_position(x, y);
    return;
  }
  if (mouse_x < 0)
    mouse_x = 0;
  if (mouse_y < 0)
    mouse_y = 0;
  if (mouse_max_x > 0 && mouse_x >= mouse_max_x)
    mouse_x = mouse_max_x - 1;
  if (mouse_max_y > 0 && mouse_y >= mouse_max_y)
    mouse_y = mouse_max_y - 1;
  if (x)
    *x = mouse_x;
  if (y)
    *y = mouse_y;
}

int mouse_get_buttons(void) {
  int buttons;

  if (trackpad_has_pointer())
    buttons = trackpad_get_buttons();
  else
    buttons = mouse_buttons;

  if (buttons < 0)
    return 0;
  return buttons & 0x1F;
}

#endif
