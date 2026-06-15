/*
 * Shared password hashing helpers for kernel and userspace account flows.
 *
 * This is intentionally lightweight so the same account format can be used
 * across the GUI setup path and the console login program.
 */

#ifndef SHARED_PASSWORD_HASH_H
#define SHARED_PASSWORD_HASH_H

static inline unsigned long long vib_password_hash_mix(unsigned long long state,
                                                       unsigned char byte) {
  state ^= (unsigned long long)byte;
  state *= 1099511628211ULL;
  state ^= state >> 32;
  state *= 0x9E3779B185EBCA87ULL;
  return state;
}

static inline unsigned long long
vib_password_hash_feed(unsigned long long state, const char *text) {
  int i = 0;
  while (text && text[i]) {
    state = vib_password_hash_mix(state, (unsigned char)text[i]);
    i++;
  }
  return state;
}

static inline void vib_password_hash_hex(const char *username,
                                         const char *password, char *out,
                                         int max) {
  static const char *pepper_a = "vib-os-account-v1";
  static const char *pepper_b = "setup-account";
  static const char hex[] = "0123456789abcdef";
  unsigned long long h1 = 1469598103934665603ULL;
  unsigned long long h2 = 0xD6E8FEB86659FD93ULL;
  int i;

  if (!out || max <= 0)
    return;
  if (max < 33) {
    out[0] = '\0';
    return;
  }

  h1 = vib_password_hash_feed(h1, pepper_a);
  h1 = vib_password_hash_feed(h1, username);
  h1 = vib_password_hash_feed(h1, ":");
  h1 = vib_password_hash_feed(h1, password);

  h2 = vib_password_hash_feed(h2, pepper_b);
  h2 = vib_password_hash_feed(h2, password);
  h2 = vib_password_hash_feed(h2, ":");
  h2 = vib_password_hash_feed(h2, username);

  for (i = 0; i < 256; i++) {
    h1 = vib_password_hash_mix(h1, (unsigned char)(i + 17));
    h1 = vib_password_hash_feed(h1, password);
    h1 = vib_password_hash_feed(h1, username);

    h2 = vib_password_hash_mix(h2, (unsigned char)(i + 73));
    h2 = vib_password_hash_feed(h2, username);
    h2 = vib_password_hash_feed(h2, password);
  }

  for (i = 0; i < 16; i++) {
    unsigned int shift = (unsigned int)((15 - i) * 4);
    out[i] = hex[(unsigned int)((h1 >> shift) & 0xFULL)];
    out[16 + i] = hex[(unsigned int)((h2 >> shift) & 0xFULL)];
  }
  out[32] = '\0';
}

static inline int vib_secure_string_eq(const char *a, const char *b) {
  unsigned int diff = 0;
  int i = 0;

  while ((a && a[i]) || (b && b[i])) {
    unsigned char ac = (a && a[i]) ? (unsigned char)a[i] : 0;
    unsigned char bc = (b && b[i]) ? (unsigned char)b[i] : 0;
    diff |= (unsigned int)(ac ^ bc);
    i++;
  }

  return diff == 0;
}

#endif
