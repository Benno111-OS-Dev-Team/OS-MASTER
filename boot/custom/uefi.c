#include "uefi.h"

void *efi_memcpy(void *dst, const void *src, uint64_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  while (n--) *d++ = *s++;
  return dst;
}

void *efi_memset(void *dst, int c, uint64_t n) {
  uint8_t *d = (uint8_t *)dst;
  while (n--) *d++ = (uint8_t)c;
  return dst;
}

int efi_memcmp(const void *a, const void *b, uint64_t n) {
  const uint8_t *x = (const uint8_t *)a;
  const uint8_t *y = (const uint8_t *)b;
  for (uint64_t i = 0; i < n; i++) {
    if (x[i] != y[i]) return (int)x[i] - (int)y[i];
  }
  return 0;
}

uint64_t efi_strlen(const char *s) {
  uint64_t n = 0;
  while (s && s[n]) n++;
  return n;
}

int efi_streq(const char *a, const char *b) {
  uint64_t i = 0;
  while (a[i] && b[i] && a[i] == b[i]) i++;
  return a[i] == b[i];
}

int efi_guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
  return efi_memcmp(a, b, sizeof(*a)) == 0;
}

void efi_print(EFI_SYSTEM_TABLE *st, const char *s) {
  CHAR16 buf[256];
  while (*s) {
    uint64_t i = 0;
    while (s[i] && i < 255) {
      if (s[i] == '\n') {
        buf[i++] = '\r';
        break;
      }
      buf[i] = (CHAR16)s[i];
      i++;
    }
    buf[i] = 0;
    st->ConOut->OutputString(st->ConOut, buf);
    s += (s[i - 1] == '\r') ? i - 1 : i;
    if (*s == '\n') s++;
  }
}

void efi_print_hex(EFI_SYSTEM_TABLE *st, uint64_t value) {
  char buf[19];
  const char *hex = "0123456789abcdef";
  buf[0] = '0';
  buf[1] = 'x';
  for (int i = 0; i < 16; i++) {
    buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xf];
  }
  buf[18] = 0;
  efi_print(st, buf);
}

EFI_STATUS efi_open_root(EFI_HANDLE image, EFI_SYSTEM_TABLE *st, EFI_FILE_PROTOCOL **root) {
  EFI_GUID loaded_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
  EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
  EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
  EFI_STATUS status;

  status = st->BootServices->OpenProtocol(image, &loaded_guid, (void **)&loaded,
                                          image, NULL,
                                          EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
  if (EFI_ERROR(status)) return status;
  status = st->BootServices->OpenProtocol(loaded->DeviceHandle, &fs_guid, (void **)&fs,
                                          image, NULL,
                                          EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
  if (EFI_ERROR(status)) return status;
  return fs->OpenVolume(fs, root);
}

EFI_STATUS efi_read_file(EFI_HANDLE image, EFI_SYSTEM_TABLE *st, const CHAR16 *path,
                         void **buffer, uint64_t *size) {
  EFI_GUID info_guid = EFI_FILE_INFO_GUID;
  EFI_FILE_PROTOCOL *root = NULL;
  EFI_FILE_PROTOCOL *file = NULL;
  EFI_STATUS status = efi_open_root(image, st, &root);
  if (EFI_ERROR(status)) return status;

  status = root->Open(root, &file, (CHAR16 *)path, EFI_FILE_MODE_READ, EFI_FILE_READ_ONLY);
  if (EFI_ERROR(status)) {
    root->Close(root);
    return status;
  }

  uint8_t info_buf[512];
  uint64_t info_size = sizeof(info_buf);
  status = file->GetInfo(file, &info_guid, &info_size, info_buf);
  if (EFI_ERROR(status)) {
    file->Close(file);
    root->Close(root);
    return status;
  }

  EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
  void *data = NULL;
  status = st->BootServices->AllocatePool(EfiLoaderData, info->FileSize + 1, &data);
  if (EFI_ERROR(status)) {
    file->Close(file);
    root->Close(root);
    return status;
  }

  uint64_t read_size = info->FileSize;
  status = file->Read(file, &read_size, data);
  if (!EFI_ERROR(status)) {
    ((uint8_t *)data)[read_size] = 0;
    *buffer = data;
    *size = read_size;
  }
  file->Close(file);
  root->Close(root);
  return status;
}

const char *cfg_get(const char *cfg, const char *key, char *out, uint64_t out_len) {
  uint64_t key_len = efi_strlen(key);
  const char *p = cfg;
  if (!cfg || !key || !out || out_len == 0) return NULL;
  while (*p) {
    while (*p == '\r' || *p == '\n' || *p == ' ') p++;
    if (!efi_memcmp(p, key, key_len) && p[key_len] == '=') {
      p += key_len + 1;
      uint64_t i = 0;
      while (p[i] && p[i] != '\r' && p[i] != '\n' && i + 1 < out_len) {
        out[i] = p[i];
        i++;
      }
      out[i] = 0;
      return out;
    }
    while (*p && *p != '\n') p++;
  }
  out[0] = 0;
  return NULL;
}

static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

void sha256(const uint8_t *data, uint64_t len, uint8_t out[32]) {
  static const uint32_t k[64] = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  uint64_t bit_len = len * 8;
  uint64_t padded_len = len + 1 + 8;
  if (padded_len & 63ULL) padded_len = (padded_len + 63ULL) & ~63ULL;
  for (uint64_t offset = 0; offset < padded_len; offset += 64) {
    uint8_t block[64];
    efi_memset(block, 0, 64);
    for (uint64_t i = 0; i < 64; i++) {
      uint64_t pos = offset + i;
      if (pos < len) {
        block[i] = data[pos];
      } else if (pos == len) {
        block[i] = 0x80;
      }
    }
    if (offset + 64 == padded_len) {
      for (int i = 0; i < 8; i++) block[63 - i] = (uint8_t)(bit_len >> (i * 8));
    }
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
      w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
             ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
      uint32_t ch = (e & f) ^ ((~e) & g);
      uint32_t temp1 = hh + S1 + ch + k[i] + w[i];
      uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t temp2 = S0 + maj;
      hh=g; g=f; f=e; e=d+temp1; d=c; c=b; b=a; a=temp1+temp2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
  }
  for (int i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)h[i];
  }
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int hex_to_bytes(const char *hex, uint8_t *out, uint64_t out_len) {
  for (uint64_t i = 0; i < out_len; i++) {
    int hi = hexval(hex[i * 2]);
    int lo = hexval(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return 0;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return 1;
}
