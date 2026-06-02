/*
 * VibeOS Userspace Library
 *
 * Programs receive a pointer to kernel API and call functions directly.
 * No syscalls needed - Win3.1 style!
 */

#ifndef _VIBE_H
#define _VIBE_H

typedef unsigned long size_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;
typedef signed short int16_t;

/* Shared application ABI */
#include "app_api.h"

// TTF glyph info (returned by ttf_get_glyph)
typedef struct {
    uint8_t *bitmap;     // Grayscale bitmap (0-255), do not free
    int width;           // Bitmap width
    int height;          // Bitmap height
    int xoff;            // X offset from cursor
    int yoff;            // Y offset from cursor (negative = above baseline)
    int advance;         // Cursor advance after glyph
} ttf_glyph_t;

// TTF font style flags
#define TTF_STYLE_NORMAL  0
#define TTF_STYLE_BOLD    1
#define TTF_STYLE_ITALIC  2

// TTF font sizes
#define TTF_SIZE_SMALL   12
#define TTF_SIZE_NORMAL  16
#define TTF_SIZE_LARGE   24
#define TTF_SIZE_XLARGE  32

// Window event types
#define WIN_EVENT_NONE       0
#define WIN_EVENT_MOUSE_DOWN 1
#define WIN_EVENT_MOUSE_UP   2
#define WIN_EVENT_MOUSE_MOVE 3
#define WIN_EVENT_KEY        4
#define WIN_EVENT_CLOSE      5
#define WIN_EVENT_FOCUS      6
#define WIN_EVENT_UNFOCUS    7
#define WIN_EVENT_RESIZE     8

// Mouse button masks
#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

// Special key codes (must match kernel/keyboard.c)
#define KEY_UP     0x100
#define KEY_DOWN   0x101
#define KEY_LEFT   0x102
#define KEY_RIGHT  0x103
#define KEY_HOME   0x104
#define KEY_END    0x105
#define KEY_DELETE 0x106
#define KEY_PGUP   0x107
#define KEY_PGDN   0x108

// Colors (must match kernel fb.h - these are RGB values)
#define COLOR_BLACK   0x00000000
#define COLOR_WHITE   0x00FFFFFF
#define COLOR_RED     0x00FF0000
#define COLOR_GREEN   0x0000FF00
#define COLOR_BLUE    0x000000FF
#define COLOR_CYAN    0x0000FFFF
#define COLOR_MAGENTA 0x00FF00FF
#define COLOR_YELLOW  0x00FFFF00
#define COLOR_AMBER   0x00FFBF00

// NULL pointer
#ifndef NULL
#define NULL ((void *)0)
#endif

// Network helper: make IP address from bytes
#define MAKE_IP(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))

// ============ String Functions ============

static inline size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static inline int strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a - *b;
}

static inline int strncmp(const char *a, const char *b, size_t n) {
    while (n > 0 && *a && *b && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) return 0;
    return *a - *b;
}

static inline char *strncpy_safe(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
    return dst;
}

static inline char *strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

static inline void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static inline void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

// Fast 64-bit aligned memset for 32-bit values (fills 2 pixels at a time)
static inline void memset32_fast(uint32_t *dst, uint32_t val, size_t count) {
    // Use 64-bit stores when aligned and count is sufficient
    if (((size_t)dst & 7) == 0 && count >= 2) {
        uint64_t pattern = ((uint64_t)val << 32) | val;
        uint64_t *d64 = (uint64_t *)dst;
        size_t n64 = count / 2;
        for (size_t i = 0; i < n64; i++) {
            d64[i] = pattern;
        }
        // Handle odd pixel at end
        if (count & 1) {
            dst[count - 1] = val;
        }
    } else {
        // Fallback for unaligned or small counts
        for (size_t i = 0; i < count; i++) {
            dst[i] = val;
        }
    }
}

// Fast 64-bit aligned memcpy (copies 8 bytes at a time)
static inline void memcpy64(void *dst, const void *src, size_t n) {
    // Use 64-bit loads/stores when both aligned
    if (((size_t)dst & 7) == 0 && ((size_t)src & 7) == 0 && n >= 8) {
        uint64_t *d = (uint64_t *)dst;
        const uint64_t *s = (const uint64_t *)src;
        size_t n64 = n / 8;
        for (size_t i = 0; i < n64; i++) {
            d[i] = s[i];
        }
        // Handle remainder bytes
        size_t rem = n & 7;
        if (rem) {
            uint8_t *d8 = (uint8_t *)(d + n64);
            const uint8_t *s8 = (const uint8_t *)(s + n64);
            for (size_t i = 0; i < rem; i++) {
                d8[i] = s8[i];
            }
        }
    } else {
        // Fallback for unaligned
        uint8_t *d = (uint8_t *)dst;
        const uint8_t *s = (const uint8_t *)src;
        while (n--) *d++ = *s++;
    }
}

// Check if character is whitespace
static inline int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Check if character is printable
static inline int isprint(int c) {
    return c >= 32 && c < 127;
}

// ============ Smart I/O Helpers ============
// These automatically use stdio hooks when available (for terminal emulator)
// Otherwise fall back to console I/O

// Print a character - uses stdio hooks if set, else console
static inline void vibe_putc(kapi_t *k, char c) {
    if (k->stdio_putc) k->stdio_putc(c);
    else k->putc(c);
}

// Print a string - uses stdio hooks if set, else console
static inline void vibe_puts(kapi_t *k, const char *s) {
    if (k->stdio_puts) k->stdio_puts(s);
    else k->puts(s);
}

// Read a character - uses stdio hooks if set, else console
static inline int vibe_getc(kapi_t *k) {
    if (k->stdio_getc) return k->stdio_getc();
    else return k->getc();
}

// Check if key available - uses stdio hooks if set, else console
static inline int vibe_has_key(kapi_t *k) {
    if (k->stdio_has_key) return k->stdio_has_key();
    else return k->has_key();
}

// Print an integer (decimal)
static inline void vibe_print_int(kapi_t *k, int n) {
    if (n < 0) {
        vibe_putc(k, '-');
        n = -n;
    }
    if (n == 0) {
        vibe_putc(k, '0');
        return;
    }
    char buf[12];
    int i = 0;
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0) {
        vibe_putc(k, buf[--i]);
    }
}

// Print an unsigned integer (decimal)
static inline void vibe_print_uint(kapi_t *k, unsigned int n) {
    if (n == 0) {
        vibe_putc(k, '0');
        return;
    }
    char buf[12];
    int i = 0;
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0) {
        vibe_putc(k, buf[--i]);
    }
}

// Print a hex number (no 0x prefix)
static inline void vibe_print_hex(kapi_t *k, uint32_t n) {
    if (n == 0) {
        vibe_putc(k, '0');
        return;
    }
    char buf[9];
    int i = 0;
    while (n > 0) {
        int digit = n & 0xF;
        buf[i++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
        n >>= 4;
    }
    while (i > 0) {
        vibe_putc(k, buf[--i]);
    }
}

// Print size in human-readable format (KB, MB, GB)
static inline void vibe_print_size(kapi_t *k, uint64_t bytes) {
    if (bytes < 1024) {
        vibe_print_uint(k, (unsigned int)bytes);
        vibe_puts(k, " B");
    } else if (bytes < 1024 * 1024) {
        vibe_print_uint(k, (unsigned int)(bytes / 1024));
        vibe_puts(k, " KB");
    } else if (bytes < 1024ULL * 1024 * 1024) {
        vibe_print_uint(k, (unsigned int)(bytes / (1024 * 1024)));
        vibe_puts(k, " MB");
    } else {
        vibe_print_uint(k, (unsigned int)(bytes / (1024ULL * 1024 * 1024)));
        vibe_puts(k, " GB");
    }
}

// ============ Files and Disks ============

static inline int vibe_save_file(kapi_t *k, const char *path, const void *data,
                                 size_t size, uint32_t flags) {
    if (!k || !k->save_file) return -1;
    return k->save_file(path, data, size, flags);
}

static inline int vibe_disk_count(kapi_t *k) {
    if (!k || !k->disk_count) return 0;
    return k->disk_count();
}

static inline int vibe_disk_info(kapi_t *k, int disk_index,
                                 os8_disk_info_t *info) {
    if (!k || !k->disk_info) return -1;
    return k->disk_info(disk_index, info);
}

static inline int vibe_partition_count(kapi_t *k, int disk_index) {
    if (!k || !k->partition_count) return 0;
    return k->partition_count(disk_index);
}

static inline int vibe_partition_info(kapi_t *k, int disk_index,
                                      int partition_index,
                                      os8_partition_info_t *info) {
    if (!k || !k->partition_info) return -1;
    return k->partition_info(disk_index, partition_index, info);
}

static inline int vibe_partition_create(kapi_t *k, int disk_index,
                                        uint32_t kind, uint32_t size_mib) {
    if (!k || !k->partition_create) return -1;
    return k->partition_create(disk_index, kind, size_mib);
}

static inline int vibe_partition_update(kapi_t *k, int disk_index,
                                        int partition_index, uint32_t kind,
                                        uint32_t size_mib) {
    if (!k || !k->partition_update) return -1;
    return k->partition_update(disk_index, partition_index, kind, size_mib);
}

static inline int vibe_partition_delete(kapi_t *k, int disk_index,
                                        int partition_index) {
    if (!k || !k->partition_delete) return -1;
    return k->partition_delete(disk_index, partition_index);
}

static inline int vibe_partition_format(kapi_t *k, int disk_index,
                                        int partition_index,
                                        uint32_t filesystem) {
    if (!k || !k->partition_format) return -1;
    return k->partition_format(disk_index, partition_index, filesystem);
}

#endif
