/*
 * OS8 - Virtio MMIO Mouse/Tablet Driver
 *
 * Based on VibeOS implementation for QEMU virt machine.
 * Uses virtio-tablet for absolute positioning (EV_ABS events).
 */

#include "printk.h"
#include "types.h"
#include "drivers/trackpad.h"

/* ===================================================================== */
/* Virtio MMIO registers (QEMU virt machine) */
/* ===================================================================== */

#define VIRTIO_MMIO_BASE 0x0a000000
#define VIRTIO_MMIO_STRIDE 0x200

#define VIRTIO_MMIO_MAGIC 0x000
#define VIRTIO_MMIO_VERSION 0x004
#define VIRTIO_MMIO_DEVICE_ID 0x008
#define VIRTIO_MMIO_VENDOR_ID 0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020
#define VIRTIO_MMIO_QUEUE_SEL 0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX 0x034
#define VIRTIO_MMIO_QUEUE_NUM 0x038
#define VIRTIO_MMIO_QUEUE_READY 0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK 0x064
#define VIRTIO_MMIO_STATUS 0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW 0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW 0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW 0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH 0x0a4

#define VIRTIO_STATUS_ACK 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8

#define VIRTIO_DEV_INPUT 18

/* Linux input event types */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03

/* Relative axis codes */
#define REL_X 0x00
#define REL_Y 0x01
#define REL_WHEEL 0x08

/* Absolute axis codes */
#define ABS_X 0x00
#define ABS_Y 0x01

/* Button codes */
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#define BTN_TOUCH 0x14a

/* Virtio input config */
#define VIRTIO_INPUT_CFG_SELECT 0x100
#define VIRTIO_INPUT_CFG_SUBSEL 0x101
#define VIRTIO_INPUT_CFG_SIZE 0x102
#define VIRTIO_INPUT_CFG_DATA 0x108
#define VIRTIO_INPUT_CFG_ID_NAME 0x01

/* Virtqueue structures */
typedef struct __attribute__((packed)) {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} virtq_desc_t;

typedef struct __attribute__((packed)) {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[16];
} virtq_avail_t;

typedef struct __attribute__((packed)) {
  uint32_t id;
  uint32_t len;
} virtq_used_elem_t;

typedef struct __attribute__((packed)) {
  uint16_t flags;
  uint16_t idx;
  virtq_used_elem_t ring[16];
} virtq_used_t;

/* Input event structure */
typedef struct __attribute__((packed)) {
  uint16_t type;
  uint16_t code;
  uint32_t value;
} virtio_input_event_t;

#define QUEUE_SIZE 16
#define DESC_F_WRITE 2
#define MAX_POINTER_DEVS 4
#define MAX_KEYBOARD_DEVS 4

/* ===================================================================== */
/* State */
/* ===================================================================== */

struct virtio_input_dev {
  volatile uint32_t *base;
  virtq_desc_t *desc;
  virtq_avail_t *avail;
  virtq_used_t *used;
  virtio_input_event_t *events;
  uint16_t last_used_idx;
  char name[32];
  uint8_t queue_mem[4096];
  virtio_input_event_t event_bufs[QUEUE_SIZE];
} __attribute__((aligned(4096)));

static struct virtio_input_dev mouse_devs[MAX_POINTER_DEVS];
static int mouse_dev_count = 0;
static int mouse_prefer_named_mouse = 0;

/* Mouse state */
static int mouse_x = 16384; /* Raw 0-32767 */
static int mouse_y = 16384;
static uint8_t mouse_buttons = 0;
static int mouse_bounds_w = 1024;
static int mouse_bounds_h = 768;
static int mouse_scale = 2;
static int mouse_has_absolute = 0;

/* Keyboard state */
static struct virtio_input_dev kbd_devs[MAX_KEYBOARD_DEVS];
static int kbd_dev_count = 0;

/* Keyboard callback */
static void (*gui_key_callback)(int key) = 0;

/* Modifier key states */
static int shift_held = 0;
static int ctrl_held = 0;
static int alt_held = 0;
static int boot_verbose_requested = 0;

#define KEY_WINDOW_SWITCHER 0x110
#define KEY_CTRL_ALT_DEL 0x111

/* Linux keycode to ASCII mapping (not PS/2 scancodes!) */
/* virtio-keyboard sends Linux KEY_* codes, not PS/2 scancodes */
static const char keycode_to_ascii[128] = {
    0,    27,  '1',  '2',
    '3',  '4', '5',  '6', /* 0-7 */
    '7',  '8', '9',  '0',
    '-',  '=', '\b', '\t', /* 8-15: KEY_7..KEY_TAB */
    'q',  'w', 'e',  'r',
    't',  'y', 'u',  'i', /* 16-23: KEY_Q..KEY_I */
    'o',  'p', '[',  ']',
    '\n', 0,   'a',  's', /* 24-31: KEY_O..KEY_S */
    'd',  'f', 'g',  'h',
    'j',  'k', 'l',  ';', /* 32-39: KEY_D..KEY_SEMICOLON */
    '\'', '`', 0,    '\\',
    'z',  'x', 'c',  'v', /* 40-47: KEY_APOSTROPHE..KEY_V */
    'b',  'n', 'm',  ',',
    '.',  '/', 0,    '*', /* 48-55: KEY_B..KEY_KPASTERISK */
    0,    ' ', 0,    0,
    0,    0,   0,    0, /* 56-63: KEY_LEFTALT..KEY_F5 */
    0,    0,   0,    0,
    0,    0,   0,    '7', /* 64-71: KEY_F6..KEY_KP7 */
    '8',  '9', '-',  '4',
    '5',  '6', '+',  '1', /* 72-79: KEY_KP8..KEY_KP1 */
    '2',  '3', '0',  '.',
    0,    0,   0,    0, /* 80-87: KEY_KP2..KEY_F12 */
    0,    0,   0,    0,
    0,    0,   0,    0, /* 88-95 */
    0,    0,   0,    0,
    0,    0,   0,    0, /* 96-103 */
    0,    0,   0,    0,
    0,    0,   0,    0, /* 104-111 */
    0,    0,   0,    0,
    0,    0,   0,    0, /* 112-119 */
    0,    0,   0,    0,
    0,    0,   0,    0 /* 120-127 */
};

/* Shifted keycode to ASCII mapping for symbols */
static const char keycode_to_ascii_shifted[128] = {
    0,    27,  '!',  '@',
    '#',  '$', '%',  '^', /* 0-7: shift+1 = !, shift+2 = @, etc */
    '&',  '*', '(',  ')',
    '_',  '+', '\b', '\t', /* 8-15: shift+- = _, shift+= = + */
    'Q',  'W', 'E',  'R',
    'T',  'Y', 'U',  'I', /* 16-23: uppercase letters */
    'O',  'P', '{',  '}',
    '\n', 0,   'A',  'S', /* 24-31: shift+[ = {, shift+] = } */
    'D',  'F', 'G',  'H',
    'J',  'K', 'L',  ':', /* 32-39: shift+; = : */
    '"',  '~', 0,    '|',
    'Z',  'X', 'C',  'V', /* 40-47: shift+' = ", shift+` = ~, shift+\ = | */
    'B',  'N', 'M',  '<',
    '>',  '?', 0,    '*', /* 48-55: shift+, = <, shift+. = >, shift+/ = ? */
    0,    ' ', 0,    0,
    0,    0,   0,    0, /* 56-63 */
    0,    0,   0,    0,
    0,    0,   0,    '7', /* 64-71 */
    '8',  '9', '-',  '4',
    '5',  '6', '+',  '1', /* 72-79 */
    '2',  '3', '0',  '.',
    0,    0,   0,    0, /* 80-87 */
    0,    0,   0,    0,
    0,    0,   0,    0, /* 88-95 */
    0,    0,   0,    0,
    0,    0,   0,    0, /* 96-103 */
    0,    0,   0,    0,
    0,    0,   0,    0, /* 104-111 */
    0,    0,   0,    0,
    0,    0,   0,    0, /* 112-119 */
    0,    0,   0,    0,
    0,    0,   0,    0 /* 120-127 */
};

/* Key callback (forward declaration) */
static void (*key_callback)(int key) = 0;

/* Screen dimensions */
#define SCREEN_WIDTH 1024
#define SCREEN_HEIGHT 768

/* ===================================================================== */
/* MMIO Helpers */
/* ===================================================================== */

static void mmio_barrier(void) {
#ifdef ARCH_ARM64
  asm volatile("dsb sy" ::: "memory");
#elif defined(ARCH_X86_64) || defined(ARCH_X86)
  asm volatile("mfence" ::: "memory");
#endif
}

static uint32_t mmio_read32(volatile uint32_t *addr) {
  uint32_t val = *addr;
  mmio_barrier();
  return val;
}

static void mmio_write32(volatile uint32_t *addr, uint32_t val) {
  mmio_barrier();
  *addr = val;
  mmio_barrier();
}

/* ===================================================================== */
/* Find Virtio Tablet Device */
/* ===================================================================== */

static int input_name_contains(const char *name, const char *needle) {
  int i;
  int j;

  for (i = 0; name[i]; i++) {
    for (j = 0; needle[j]; j++) {
      char a = name[i + j];
      char b = needle[j];

      if (!a) {
        return 0;
      }
      if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
      }
      if (b >= 'A' && b <= 'Z') {
        b = (char)(b - 'A' + 'a');
      }
      if (a != b) {
        break;
      }
    }
    if (!needle[j]) {
      return 1;
    }
  }

  return 0;
}

static void read_input_device_name(volatile uint8_t *base8, char *name,
                                   int name_len) {
  int j;
  uint8_t size;

  if (name_len <= 0) {
    return;
  }

  base8[VIRTIO_INPUT_CFG_SELECT] = VIRTIO_INPUT_CFG_ID_NAME;
  base8[VIRTIO_INPUT_CFG_SUBSEL] = 0;
  mmio_barrier();

  size = base8[VIRTIO_INPUT_CFG_SIZE];
  for (j = 0; j < name_len - 1 && j < size; j++) {
    name[j] = base8[VIRTIO_INPUT_CFG_DATA + j];
  }
  name[j] = '\0';
}

static int discover_virtio_pointers(void) {
  int count = 0;
  int found_named_mouse = 0;

  for (int i = 0; i < 32; i++) {
    volatile uint32_t *base =
        (volatile uint32_t *)(uintptr_t)(VIRTIO_MMIO_BASE +
                                         i * VIRTIO_MMIO_STRIDE);
    volatile uint8_t *base8 =
        (volatile uint8_t *)(uintptr_t)(VIRTIO_MMIO_BASE +
                                        i * VIRTIO_MMIO_STRIDE);

    uint32_t magic = mmio_read32(base + VIRTIO_MMIO_MAGIC / 4);
    uint32_t device_id = mmio_read32(base + VIRTIO_MMIO_DEVICE_ID / 4);

    if (magic != 0x74726976 || device_id != VIRTIO_DEV_INPUT) {
      continue;
    }

    char name[32];
    read_input_device_name(base8, name, sizeof(name));

    printk(KERN_INFO "MOUSE: Found input device: %s\n", name);

    if (input_name_contains(name, "tablet") ||
        input_name_contains(name, "touchpad") ||
        input_name_contains(name, "trackpad") ||
        input_name_contains(name, "mouse") ||
        input_name_contains(name, "pointer")) {
      if (count < MAX_POINTER_DEVS) {
        mouse_devs[count].base = base;
        mouse_devs[count].name[0] = '\0';
        for (int j = 0; j < (int)sizeof(mouse_devs[count].name) - 1 && name[j];
             j++) {
          mouse_devs[count].name[j] = name[j];
          mouse_devs[count].name[j + 1] = '\0';
        }
        if (input_name_contains(name, "mouse")) {
          found_named_mouse = 1;
        }
        count++;
      }
    }
  }

  mouse_prefer_named_mouse = found_named_mouse;
  return count;
}

static int init_virtio_input_device(struct virtio_input_dev *dev) {
  uint64_t desc_addr;
  uint64_t avail_addr;
  uint64_t used_addr;
  uint32_t max_queue;

  /* Reset device */
  mmio_write32(dev->base + VIRTIO_MMIO_STATUS / 4, 0);
  while (mmio_read32(dev->base + VIRTIO_MMIO_STATUS / 4) != 0) {
    asm volatile("nop");
  }

  mmio_write32(dev->base + VIRTIO_MMIO_STATUS / 4, VIRTIO_STATUS_ACK);
  mmio_write32(dev->base + VIRTIO_MMIO_STATUS / 4,
               VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

  mmio_write32(dev->base + VIRTIO_MMIO_DRIVER_FEATURES / 4, 0);
  mmio_write32(dev->base + VIRTIO_MMIO_STATUS / 4,
               VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                   VIRTIO_STATUS_FEATURES_OK);

  mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_SEL / 4, 0);

  max_queue = mmio_read32(dev->base + VIRTIO_MMIO_QUEUE_NUM_MAX / 4);
  if (max_queue < QUEUE_SIZE) {
    return -1;
  }

  mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_NUM / 4, QUEUE_SIZE);

  dev->desc = (virtq_desc_t *)dev->queue_mem;
  dev->avail =
      (virtq_avail_t *)(dev->queue_mem + QUEUE_SIZE * sizeof(virtq_desc_t));
  dev->used = (virtq_used_t *)(dev->queue_mem + 2048);
  dev->events = dev->event_bufs;
  dev->last_used_idx = 0;

  desc_addr = (uint64_t)(uintptr_t)dev->desc;
  avail_addr = (uint64_t)(uintptr_t)dev->avail;
  used_addr = (uint64_t)(uintptr_t)dev->used;

  mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_DESC_LOW / 4,
               (uint32_t)desc_addr);
  mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_DESC_HIGH / 4,
               (uint32_t)(desc_addr >> 32));
  mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_AVAIL_LOW / 4,
               (uint32_t)avail_addr);
  mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_AVAIL_HIGH / 4,
               (uint32_t)(avail_addr >> 32));
  mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_USED_LOW / 4,
               (uint32_t)used_addr);
  mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_USED_HIGH / 4,
               (uint32_t)(used_addr >> 32));

  for (int i = 0; i < QUEUE_SIZE; i++) {
    dev->desc[i].addr = (uint64_t)(uintptr_t)&dev->events[i];
    dev->desc[i].len = sizeof(virtio_input_event_t);
    dev->desc[i].flags = DESC_F_WRITE;
    dev->desc[i].next = 0;
  }

  dev->avail->flags = 0;
  for (int i = 0; i < QUEUE_SIZE; i++) {
    dev->avail->ring[i] = i;
  }
  dev->avail->idx = QUEUE_SIZE;

  mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_READY / 4, 1);
  mmio_write32(dev->base + VIRTIO_MMIO_STATUS / 4,
               VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                   VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
  mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_NOTIFY / 4, 0);

  if (mmio_read32(dev->base + VIRTIO_MMIO_STATUS / 4) & 0x40) {
    return -1;
  }

  return 0;
}

/* ===================================================================== */
/* Mouse Polling */
/* ===================================================================== */

void mouse_poll(void) {
  if (mouse_dev_count <= 0) {
    return;
  }

  for (int dev_idx = 0; dev_idx < mouse_dev_count; dev_idx++) {
    struct virtio_input_dev *dev = &mouse_devs[dev_idx];
    uint16_t current_used;

    if (!dev->base || !dev->used) {
      continue;
    }
    if (mouse_prefer_named_mouse &&
        !input_name_contains(dev->name, "mouse")) {
      continue;
    }

    mmio_barrier();
    current_used = dev->used->idx;

    while (dev->last_used_idx != current_used) {
      uint16_t idx = dev->last_used_idx % QUEUE_SIZE;
      uint32_t desc_idx = dev->used->ring[idx].id;
      virtio_input_event_t *ev = &dev->events[desc_idx];

      /* Process event */
      if (ev->type == EV_ABS) {
        mouse_has_absolute = 1;
        if (ev->code == ABS_X) {
          mouse_x = ev->value;
        } else if (ev->code == ABS_Y) {
          mouse_y = ev->value;
        }
      } else if (ev->type == EV_REL) {
        if (ev->code == REL_X) {
          mouse_x += (int)ev->value * mouse_scale;
        } else if (ev->code == REL_Y) {
          mouse_y += (int)ev->value * mouse_scale;
        } else if (ev->code == REL_WHEEL) {
          /* Wheel support can be consumed later by the GUI. */
        }
      } else if (ev->type == EV_KEY) {
        int pressed = (ev->value != 0);
        if (ev->code == BTN_LEFT) {
          if (pressed)
            mouse_buttons |= 1;
          else
            mouse_buttons &= ~1;
        } else if (ev->code == BTN_RIGHT) {
          if (pressed)
            mouse_buttons |= 2;
          else
            mouse_buttons &= ~2;
        } else if (ev->code == BTN_MIDDLE) {
          if (pressed)
            mouse_buttons |= 4;
          else
            mouse_buttons &= ~4;
        } else if (ev->code == BTN_TOUCH) {
          if (pressed)
            mouse_buttons |= 1;
          else
            mouse_buttons &= ~1;
        }
      }

      if (mouse_has_absolute) {
        if (mouse_x < 0)
          mouse_x = 0;
        if (mouse_y < 0)
          mouse_y = 0;
        if (mouse_x > 32767)
          mouse_x = 32767;
        if (mouse_y > 32767)
          mouse_y = 32767;
      } else {
        int max_x = mouse_bounds_w - 1;
        int max_y = mouse_bounds_h - 1;
        if (max_x < 0)
          max_x = 0;
        if (max_y < 0)
          max_y = 0;
        if (mouse_x < 0)
          mouse_x = 0;
        if (mouse_y < 0)
          mouse_y = 0;
        if (mouse_x > max_x)
          mouse_x = max_x;
        if (mouse_y > max_y)
          mouse_y = max_y;
      }

      /* Re-add descriptor to available ring */
      uint16_t avail_idx = dev->avail->idx % QUEUE_SIZE;
      dev->avail->ring[avail_idx] = desc_idx;
      dev->avail->idx++;

      dev->last_used_idx++;
    }

    mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_NOTIFY / 4, 0);
    mmio_write32(dev->base + VIRTIO_MMIO_INTERRUPT_ACK / 4,
                 mmio_read32(dev->base + VIRTIO_MMIO_INTERRUPT_STATUS / 4));
  }
}

/* ===================================================================== */
/* Mouse API */
/* ===================================================================== */

void mouse_get_position(int *x, int *y) {
  if (trackpad_has_pointer()) {
    trackpad_input_poll();
    trackpad_get_position(x, y);
    return;
  }
  mouse_poll();

  if (mouse_has_absolute) {
    if (x)
      *x = (mouse_x * mouse_bounds_w) / 32768;
    if (y)
      *y = (mouse_y * mouse_bounds_h) / 32768;
  } else {
    if (x)
      *x = mouse_x;
    if (y)
      *y = mouse_y;
  }
}

int mouse_get_buttons(void) {
  if (trackpad_has_pointer()) {
    trackpad_input_poll();
    return trackpad_get_buttons();
  }
  mouse_poll();
  return mouse_buttons;
}

/* ===================================================================== */
/* Initialization */
/* ===================================================================== */

int mouse_init(void) {
  printk(KERN_INFO "MOUSE: Initializing virtio pointer device...\n");
  trackpad_input_init();

  mouse_dev_count = discover_virtio_pointers();
  if (mouse_dev_count <= 0) {
    printk(KERN_WARNING "MOUSE: No virtio pointer device found\n");
    return -1;
  }

  for (int i = 0; i < mouse_dev_count; i++) {
    if (init_virtio_input_device(&mouse_devs[i]) != 0) {
      printk(KERN_WARNING "MOUSE: Failed to initialize %s\n",
             mouse_devs[i].name[0] ? mouse_devs[i].name : "pointer");
      mouse_devs[i].base = 0;
    } else {
      printk(KERN_INFO "MOUSE: Attached %s\n",
             mouse_devs[i].name[0] ? mouse_devs[i].name : "pointer");
    }
  }

  if (mouse_prefer_named_mouse) {
    printk(KERN_INFO
           "MOUSE: Preferring detected mouse device(s) over tablet/touchpad\n");
  }

  mouse_x = mouse_bounds_w / 2;
  mouse_y = mouse_bounds_h / 2;
  mouse_has_absolute = 0;

  return 0;
}

/* ===================================================================== */
/* Keyboard Functions */
/* ===================================================================== */

static int discover_virtio_keyboards(void) {
  int count = 0;

  for (int i = 0; i < 32; i++) {
    volatile uint32_t *base =
        (volatile uint32_t *)(uintptr_t)(VIRTIO_MMIO_BASE +
                                         i * VIRTIO_MMIO_STRIDE);
    volatile uint8_t *base8 =
        (volatile uint8_t *)(uintptr_t)(VIRTIO_MMIO_BASE +
                                        i * VIRTIO_MMIO_STRIDE);

    uint32_t magic = mmio_read32(base + VIRTIO_MMIO_MAGIC / 4);
    uint32_t device_id = mmio_read32(base + VIRTIO_MMIO_DEVICE_ID / 4);

    if (magic != 0x74726976 || device_id != VIRTIO_DEV_INPUT) {
      continue;
    }

    char name[32];
    read_input_device_name(base8, name, sizeof(name));

    printk(KERN_INFO "KEYBOARD: Checking device: %s\n", name);

    /* Look for "Keyboard" or "keyboard" anywhere in name */
    int found_kbd = 0;
    for (int j = 0; name[j] && name[j + 7]; j++) {
      if ((name[j] == 'K' || name[j] == 'k') &&
          (name[j + 1] == 'e' || name[j + 1] == 'E') &&
          (name[j + 2] == 'y' || name[j + 2] == 'Y')) {
        found_kbd = 1;
        break;
      }
    }

    if (found_kbd) {
      if (count < MAX_KEYBOARD_DEVS) {
        kbd_devs[count].base = base;
        for (int j = 0; j < (int)sizeof(kbd_devs[count].name) - 1 && name[j];
             j++) {
          kbd_devs[count].name[j] = name[j];
          kbd_devs[count].name[j + 1] = '\0';
        }
        count++;
      }
    }
  }

  return count;
}

static void keyboard_poll(void) {
  if (kbd_dev_count <= 0) {
    return;
  }

  for (int dev_idx = 0; dev_idx < kbd_dev_count; dev_idx++) {
    struct virtio_input_dev *dev = &kbd_devs[dev_idx];
    uint16_t current_used;

    if (!dev->base || !dev->used) {
      continue;
    }

    mmio_barrier();
    current_used = dev->used->idx;

    while (dev->last_used_idx != current_used) {
      uint16_t idx = dev->last_used_idx % QUEUE_SIZE;
      uint32_t desc_idx = dev->used->ring[idx].id;
      virtio_input_event_t *ev = &dev->events[desc_idx];

      /* Process keyboard event */
      if (ev->type == EV_KEY) {
        /* Track shift key state */
        if (ev->code == 42 || ev->code == 54) {
          shift_held = (ev->value != 0);
        }

        /* Track Ctrl key state */
        if (ev->code == 29 || ev->code == 97) {
          ctrl_held = (ev->value != 0);
        }

        if (ev->code == 56 || ev->code == 100) {
          alt_held = (ev->value != 0);
        }

        if (ev->value == 1) {
          int processed = 0;
          int vibe_key = 0;
          char ascii = 0;

          if (ev->code == 103)
            vibe_key = 0x100;
          else if (ev->code == 108)
            vibe_key = 0x101;
          else if (ev->code == 105)
            vibe_key = 0x102;
          else if (ev->code == 106)
            vibe_key = 0x103;
          else if (ev->code == 29 || ev->code == 97)
            processed = 1;
          else if (ev->code == 56 || ev->code == 100)
            processed = 1;
          else if (ev->code == 42 || ev->code == 54)
            processed = 1;
          else if (ev->code == 125 || ev->code == 126)
            vibe_key = ev->code;
          else if (ev->code == 28)
            vibe_key = '\n';
          else if (ev->code == 57)
            vibe_key = ' ';
          else if (ev->code == 1)
            vibe_key = 27;
          else if (ev->code == 15 && alt_held)
            vibe_key = KEY_WINDOW_SWITCHER;
          else if (ev->code == 111 && ctrl_held && alt_held)
            vibe_key = KEY_CTRL_ALT_DEL;

          if (vibe_key) {
            if (vibe_key == 'v' || vibe_key == 'V') {
              boot_verbose_requested = 1;
            }
            if (key_callback)
              key_callback(vibe_key);
            if (gui_key_callback)
              gui_key_callback(vibe_key);
            processed = 1;
          }

          if (!processed && ev->code < 128) {
            if (ctrl_held) {
              char base = keycode_to_ascii[ev->code];
              if (base >= 'a' && base <= 'z') {
                ascii = base - 'a' + 1;
              } else if (base >= 'A' && base <= 'Z') {
                ascii = base - 'A' + 1;
              } else {
                ascii = 0;
              }
            } else if (shift_held) {
              ascii = keycode_to_ascii_shifted[ev->code];
            } else {
              ascii = keycode_to_ascii[ev->code];
            }

            if (ascii == 'v' || ascii == 'V') {
              boot_verbose_requested = 1;
            }
            if (key_callback && ascii) {
              key_callback(ascii);
            }
            if (gui_key_callback && ascii) {
              gui_key_callback(ascii);
            }
          }
        }
      }

      /* Re-add descriptor to available ring */
      uint16_t avail_idx = dev->avail->idx % QUEUE_SIZE;
      dev->avail->ring[avail_idx] = desc_idx;
      dev->avail->idx++;

      dev->last_used_idx++;
    }

    mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_NOTIFY / 4, 0);
    mmio_write32(dev->base + VIRTIO_MMIO_INTERRUPT_ACK / 4,
                 mmio_read32(dev->base + VIRTIO_MMIO_INTERRUPT_STATUS / 4));
  }
}

static int keyboard_init(void) {
  printk(KERN_INFO "KEYBOARD: Initializing virtio-keyboard...\n");

  kbd_dev_count = discover_virtio_keyboards();
  if (kbd_dev_count <= 0) {
    printk(KERN_WARNING "KEYBOARD: No virtio keyboard found\n");
    return -1;
  }

  for (int i = 0; i < kbd_dev_count; i++) {
    if (init_virtio_input_device(&kbd_devs[i]) != 0) {
      printk(KERN_WARNING "KEYBOARD: Failed to initialize %s\n",
             kbd_devs[i].name[0] ? kbd_devs[i].name : "keyboard");
      kbd_devs[i].base = 0;
    } else {
      printk(KERN_INFO "KEYBOARD: Attached %s\n",
             kbd_devs[i].name[0] ? kbd_devs[i].name : "keyboard");
    }
  }

  return 0;
}

/* ===================================================================== */
/* Compatibility API for main.c */
/* ===================================================================== */

int input_init(void) {
  printk(KERN_INFO "INPUT: Initializing input system\n");
  mouse_init();
  keyboard_init();
  printk(KERN_INFO "INPUT: Ready\n");
  return 0;
}

void input_set_key_callback(void (*callback)(int key)) {
  key_callback = callback;
}

void input_set_gui_key_callback(void (*callback)(int key)) {
  gui_key_callback = callback;
}

void input_set_mouse_bounds(int width, int height) {
  if (width > 0) {
    mouse_bounds_w = width;
  }
  if (height > 0) {
    mouse_bounds_h = height;
  }

  if (!mouse_has_absolute) {
    mouse_x = mouse_bounds_w / 2;
    mouse_y = mouse_bounds_h / 2;
  }
  trackpad_input_set_bounds(width, height);
}

void input_set_mouse_scale(int scale) {
  if (scale < 1) {
    scale = 1;
  }
  if (scale > 8) {
    scale = 8;
  }
  mouse_scale = scale;
  trackpad_input_set_scale(scale);
}

void input_poll(void) {
  /* Poll UART for keyboard input */
  extern int uart_getc_nonblock(void);
  int c = uart_getc_nonblock();
  if (c >= 0 && key_callback) {
    key_callback(c);
  }

  /* Poll virtio keyboard */
  keyboard_poll();

  /* Poll registered I2C/SPI trackpads */
  trackpad_input_poll();

  /* Poll mouse */
  mouse_poll();
}

int input_boot_verbose_requested(void) { return boot_verbose_requested; }
