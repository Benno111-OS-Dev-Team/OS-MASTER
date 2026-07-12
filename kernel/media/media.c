/*
 * OS8 - Media helpers (JPEG/MP3 decoding)
 */

#include "media/media.h"
#include "fs/vfs.h"
#include "gui/font.h"
#include "mm/kmalloc.h"
#include "printk.h"
#include "types.h"
#include "sandbox/sandbox.h"

static const char *media_persistent_roots[] = {"/Persist", "/persist", "/disk",
                                               "/mnt/disk"};
#define MEDIA_PERSISTENT_ROOT_COUNT                                           \
  ((int)(sizeof(media_persistent_roots) / sizeof(media_persistent_roots[0])))

static int media_strlen(const char *str) {
  int len = 0;
  while (str && str[len])
    len++;
  return len;
}

static int media_try_build_persistent_path(const char *path, char *out,
                                           size_t out_size) {
  for (int root_idx = 0; root_idx < MEDIA_PERSISTENT_ROOT_COUNT; root_idx++) {
    const char *root = media_persistent_roots[root_idx];
    struct file *dir = vfs_open(root, O_RDONLY, 0);
    if (!dir)
      continue;
    vfs_close(dir);

    int idx = 0;
    for (int i = 0; root[i] && idx < (int)out_size - 1; i++)
      out[idx++] = root[i];
    for (int i = 0; path[i] && idx < (int)out_size - 1; i++)
      out[idx++] = path[i];
    out[idx] = '\0';
    return 0;
  }
  return -ENOENT;
}

static void media_ensure_parent_dirs(const char *path) {
  char partial[256];
  int idx = 0;

  if (!path)
    return;

  for (int i = 0; path[i] && idx < (int)sizeof(partial) - 1; i++) {
    partial[idx++] = path[i];
    partial[idx] = '\0';
    if (i > 0 && path[i] == '/') {
      vfs_mkdir(partial, 0755);
    }
  }
}

static int media_write_raw_file(const char *path, const uint8_t *data,
                                size_t size) {
  struct file *f;
  ssize_t written;

  if (!path || (!data && size > 0))
    return -EINVAL;

  media_ensure_parent_dirs(path);
  vfs_unlink(path);
  f = vfs_open(path, O_CREAT | O_WRONLY, 0644);
  if (!f)
    return -ENOENT;
  written = vfs_write(f, (const char *)data, size);
  vfs_close(f);
  return (written < 0) ? (int)written : 0;
}

/* --------------------------------------------------------------------- */
/* File loading                                                          */
/* --------------------------------------------------------------------- */

static int media_load_file_from_exact_path(const char *path, uint8_t **out_data,
                                           size_t *out_size) {
  struct file *f;
  struct inode *inode;
  uint8_t *buf;
  size_t size;
  size_t total_read = 0;

  if (!path || !out_data || !out_size)
    return -EINVAL;

  f = vfs_open(path, O_RDONLY, 0);
  if (!f)
    return -ENOENT;

  inode = f->f_dentry ? f->f_dentry->d_inode : NULL;
  if (!inode || inode->i_size <= 0) {
    vfs_close(f);
    return -EINVAL;
  }

  size = (size_t)inode->i_size;
  if (size == 0) {
    vfs_close(f);
    *out_data = NULL;
    *out_size = 0;
    return 0;
  }

  buf = (uint8_t *)kmalloc(size, GFP_KERNEL);
  if (!buf) {
    vfs_close(f);
    return -ENOMEM;
  }

  while (total_read < size) {
    ssize_t read_bytes = vfs_read(f, (char *)buf + total_read, size - total_read);
    if (read_bytes < 0) {
      vfs_close(f);
      kfree(buf);
      return (int)read_bytes;
    }
    if (read_bytes == 0)
      break;
    total_read += (size_t)read_bytes;
  }
  vfs_close(f);

  if (total_read != size) {
    printk(KERN_ERR "MEDIA: short read for '%s' (%u/%u bytes)\n", path,
           (unsigned)total_read, (unsigned)size);
    kfree(buf);
    return -EIO;
  }

  *out_data = buf;
  *out_size = total_read;
  return 0;
}

int media_load_file(const char *path, uint8_t **out_data, size_t *out_size) {
  char persistent_path[256];
  int ret;

  if (!path || !out_data || !out_size)
    return -EINVAL;

  if (media_try_build_persistent_path(path, persistent_path,
                                      sizeof(persistent_path)) == 0) {
    ret = media_load_file_from_exact_path(persistent_path, out_data, out_size);
    if (ret == 0)
      return 0;
  }

  return media_load_file_from_exact_path(path, out_data, out_size);
}

void media_free_file(uint8_t *data) {
  if (data)
    kfree(data);
}

int media_install_file(const char *path, const uint8_t *data, size_t size) {
  char persistent_path[256];
  int ret = media_write_raw_file(path, data, size);
  if (ret < 0)
    return ret;

  if (media_try_build_persistent_path(path, persistent_path,
                                      sizeof(persistent_path)) == 0) {
    media_write_raw_file(persistent_path, data, size);
  }

  return 0;
}

int media_install_text_file(const char *path, const char *content) {
  if (!content)
    return -EINVAL;
  return media_install_file(path, (const uint8_t *)content,
                            (size_t)media_strlen(content));
}

/* --------------------------------------------------------------------- */
/* ZIP archive helpers                                                   */
/* --------------------------------------------------------------------- */

#define MEDIA_ZIP_LOCAL_HEADER_SIG 0x04034B50u
#define MEDIA_ZIP_CENTRAL_HEADER_SIG 0x02014B50u
#define MEDIA_ZIP_END_SIG 0x06054B50u
#define MEDIA_ZIP_VERSION 20

typedef struct {
  char *name;
  uint8_t *data;
  size_t size;
  uint32_t crc32;
} media_zip_entry_t;

typedef struct {
  media_zip_entry_t *entries;
  size_t count;
  size_t capacity;
} media_zip_builder_t;

typedef int (*media_zip_entry_cb_t)(void *ctx, const char *name,
                                    const uint8_t *data, size_t size,
                                    int is_dir);

typedef struct {
  media_zip_builder_t *builder;
  const char *src_root;
  const char *rel_root;
  int error;
} media_zip_walk_ctx_t;

typedef struct {
  const uint8_t *data;
  size_t size;
} media_zip_view_t;

static uint32_t media_zip_crc32(const uint8_t *data, size_t size) {
  uint32_t crc = 0xFFFFFFFFu;

  for (size_t i = 0; i < size; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      if (crc & 1u)
        crc = (crc >> 1) ^ 0xEDB88320u;
      else
        crc >>= 1;
    }
  }

  return ~crc;
}

static void media_zip_write_u16(uint8_t *buf, size_t *offset, uint16_t value) {
  buf[(*offset)++] = (uint8_t)(value & 0xFFu);
  buf[(*offset)++] = (uint8_t)((value >> 8) & 0xFFu);
}

static void media_zip_write_u32(uint8_t *buf, size_t *offset, uint32_t value) {
  buf[(*offset)++] = (uint8_t)(value & 0xFFu);
  buf[(*offset)++] = (uint8_t)((value >> 8) & 0xFFu);
  buf[(*offset)++] = (uint8_t)((value >> 16) & 0xFFu);
  buf[(*offset)++] = (uint8_t)((value >> 24) & 0xFFu);
}

static int media_zip_name_copy(char *dst, size_t dst_size, const char *src) {
  size_t idx = 0;

  if (!dst || dst_size == 0 || !src)
    return -EINVAL;

  while (*src == '/')
    src++;

  for (; src[idx] && idx < dst_size - 1; idx++) {
    if (src[idx] == '\\')
      dst[idx] = '/';
    else
      dst[idx] = src[idx];
  }
  dst[idx] = '\0';
  return (idx == 0) ? -EINVAL : 0;
}

static int media_zip_path_join(char *dst, size_t dst_size, const char *base,
                               const char *name) {
  size_t idx = 0;
  size_t base_len = 0;
  size_t name_idx = 0;

  if (!dst || dst_size == 0 || !name)
    return -EINVAL;

  if (base && base[0]) {
    while (base[base_len] && idx < dst_size - 1) {
      dst[idx++] = base[base_len++];
    }
    if (idx > 0 && dst[idx - 1] != '/' && idx < dst_size - 1)
      dst[idx++] = '/';
  }

  while (name[name_idx] == '/')
    name_idx++;
  while (name[name_idx] && idx < dst_size - 1) {
    dst[idx++] = (name[name_idx] == '\\') ? '/' : name[name_idx];
    name_idx++;
  }
  dst[idx] = '\0';
  return (idx == 0) ? -EINVAL : 0;
}

static char *media_zip_strdup(const char *src) {
  size_t len = 0;
  char *dst;

  if (!src)
    return NULL;
  while (src[len])
    len++;
  dst = (char *)kmalloc(len + 1, GFP_KERNEL);
  if (!dst)
    return NULL;
  for (size_t i = 0; i <= len; i++)
    dst[i] = src[i];
  return dst;
}

static int media_zip_builder_reserve(media_zip_builder_t *builder,
                                     size_t min_capacity) {
  size_t new_capacity;
  media_zip_entry_t *new_entries;

  if (!builder)
    return -EINVAL;
  if (builder->capacity >= min_capacity)
    return 0;

  new_capacity = builder->capacity ? builder->capacity * 2 : 8;
  while (new_capacity < min_capacity)
    new_capacity *= 2;

  new_entries = (media_zip_entry_t *)kmalloc(
      new_capacity * sizeof(media_zip_entry_t), GFP_KERNEL);
  if (!new_entries)
    return -ENOMEM;

  for (size_t i = 0; i < builder->count; i++)
    new_entries[i] = builder->entries[i];
  if (builder->entries)
    kfree(builder->entries);
  builder->entries = new_entries;
  builder->capacity = new_capacity;
  return 0;
}

static void media_zip_builder_free(media_zip_builder_t *builder) {
  if (!builder)
    return;

  for (size_t i = 0; i < builder->count; i++) {
    if (builder->entries[i].name)
      kfree(builder->entries[i].name);
    if (builder->entries[i].data)
      kfree(builder->entries[i].data);
  }
  if (builder->entries)
    kfree(builder->entries);
  builder->entries = NULL;
  builder->count = 0;
  builder->capacity = 0;
}

static int media_zip_builder_add_file(media_zip_builder_t *builder,
                                      const char *name,
                                      const uint8_t *data, size_t size) {
  media_zip_entry_t *entry;

  if (!builder || !name || !name[0] || (!data && size > 0))
    return -EINVAL;
  if (media_zip_builder_reserve(builder, builder->count + 1) != 0)
    return -ENOMEM;

  entry = &builder->entries[builder->count];
  entry->name = media_zip_strdup(name);
  if (!entry->name)
    return -ENOMEM;

  if (size > 0) {
    entry->data = (uint8_t *)kmalloc(size, GFP_KERNEL);
    if (!entry->data) {
      kfree(entry->name);
      entry->name = NULL;
      return -ENOMEM;
    }
    for (size_t i = 0; i < size; i++)
      entry->data[i] = data[i];
  } else {
    entry->data = NULL;
  }
  entry->size = size;
  entry->crc32 = media_zip_crc32(data ? data : (const uint8_t *)"", size);
  builder->count++;
  return 0;
}

static int media_zip_pack_tree_dir(media_zip_builder_t *builder,
                                   const char *src_root,
                                   const char *rel_root);

static int media_zip_pack_tree_callback(void *ctx, const char *name, int len,
                                        loff_t offset, ino_t ino,
                                        unsigned type) {
  media_zip_walk_ctx_t *walk = (media_zip_walk_ctx_t *)ctx;
  char child_src[256];
  char child_rel[256];
  struct file *dir;

  (void)offset;
  (void)ino;

  if (!walk || walk->error || !walk->builder || !walk->src_root || !name ||
      len <= 0)
    return 0;
  if ((len == 1 && name[0] == '.') ||
      (len == 2 && name[0] == '.' && name[1] == '.'))
    return 0;

  if (media_zip_path_join(child_src, sizeof(child_src), walk->src_root,
                          name) != 0) {
    walk->error = -ENAMETOOLONG;
    return 0;
  }
  if (media_zip_path_join(child_rel, sizeof(child_rel), walk->rel_root,
                          name) != 0) {
    walk->error = -ENAMETOOLONG;
    return 0;
  }

  if (type == 4) {
    dir = vfs_open(child_src, O_RDONLY, 0);
    if (!dir) {
      walk->error = -ENOENT;
      return 0;
    }
    vfs_close(dir);
    if (media_zip_pack_tree_dir(walk->builder, child_src, child_rel) != 0)
      walk->error = -EIO;
    return 0;
  }

  {
    uint8_t *data = NULL;
    size_t size = 0;
    int ret = media_load_file_from_exact_path(child_src, &data, &size);
    if (ret != 0) {
      walk->error = ret;
      return 0;
    }
    if (media_zip_builder_add_file(walk->builder, child_rel, data, size) != 0)
      walk->error = -ENOMEM;
    if (data)
      kfree(data);
  }
  return 0;
}

static int media_zip_pack_tree_dir(media_zip_builder_t *builder,
                                   const char *src_root,
                                   const char *rel_root) {
  struct file *dir;
  media_zip_walk_ctx_t ctx;

  if (!builder || !src_root || !src_root[0])
    return -EINVAL;

  dir = vfs_open(src_root, O_RDONLY, 0);
  if (!dir)
    return -ENOENT;

  ctx.builder = builder;
  ctx.src_root = src_root;
  ctx.rel_root = rel_root ? rel_root : "";
  ctx.error = 0;
  vfs_readdir(dir, &ctx, media_zip_pack_tree_callback);
  vfs_close(dir);
  return ctx.error;
}

int media_zip_pack_tree(const char *src_root, uint8_t **out_data,
                        size_t *out_size) {
  media_zip_builder_t builder;
  size_t total_size = 22;
  uint8_t *archive = NULL;
  size_t offset = 0;
  size_t central_dir_offset;
  size_t central_dir_size;

  if (!src_root || !src_root[0] || !out_data || !out_size)
    return -EINVAL;

  builder.entries = NULL;
  builder.count = 0;
  builder.capacity = 0;

  if (media_zip_pack_tree_dir(&builder, src_root, "") != 0) {
    media_zip_builder_free(&builder);
    return -EIO;
  }

  for (size_t i = 0; i < builder.count; i++) {
    size_t name_len = 0;
    media_zip_entry_t *entry = &builder.entries[i];
    while (entry->name[name_len])
      name_len++;
    total_size += 30 + name_len + entry->size;
    total_size += 46 + name_len;
  }

  archive = (uint8_t *)kmalloc(total_size, GFP_KERNEL);
  if (!archive) {
    media_zip_builder_free(&builder);
    return -ENOMEM;
  }

  for (size_t i = 0; i < builder.count; i++) {
    media_zip_entry_t *entry = &builder.entries[i];
    size_t name_len = 0;
    size_t local_header_offset = offset;
    while (entry->name[name_len])
      name_len++;

    media_zip_write_u32(archive, &offset, MEDIA_ZIP_LOCAL_HEADER_SIG);
    media_zip_write_u16(archive, &offset, 20);
    media_zip_write_u16(archive, &offset, 0);
    media_zip_write_u16(archive, &offset, 0);
    media_zip_write_u16(archive, &offset, 0);
    media_zip_write_u16(archive, &offset, 0);
    media_zip_write_u32(archive, &offset, entry->crc32);
    media_zip_write_u32(archive, &offset, (uint32_t)entry->size);
    media_zip_write_u32(archive, &offset, (uint32_t)entry->size);
    media_zip_write_u16(archive, &offset, (uint16_t)name_len);
    media_zip_write_u16(archive, &offset, 0);
    for (size_t j = 0; j < name_len; j++)
      archive[offset++] = (uint8_t)entry->name[j];
    for (size_t j = 0; j < entry->size; j++)
      archive[offset++] = entry->data[j];

    (void)local_header_offset;
  }

  central_dir_offset = offset;
  for (size_t i = 0; i < builder.count; i++) {
    media_zip_entry_t *entry = &builder.entries[i];
    size_t name_len = 0;
    size_t local_header_offset = 0;
    size_t data_offset = 0;

    while (entry->name[name_len])
      name_len++;

    data_offset = 0;
    for (size_t j = 0; j < i; j++) {
      media_zip_entry_t *prev = &builder.entries[j];
      size_t prev_name_len = 0;
      while (prev->name[prev_name_len])
        prev_name_len++;
      data_offset += 30 + prev_name_len + prev->size;
    }
    local_header_offset = data_offset;

    media_zip_write_u32(archive, &offset, MEDIA_ZIP_CENTRAL_HEADER_SIG);
    media_zip_write_u16(archive, &offset, 20);
    media_zip_write_u16(archive, &offset, 20);
    media_zip_write_u16(archive, &offset, 0);
    media_zip_write_u16(archive, &offset, 0);
    media_zip_write_u16(archive, &offset, 0);
    media_zip_write_u16(archive, &offset, 0);
    media_zip_write_u32(archive, &offset, entry->crc32);
    media_zip_write_u32(archive, &offset, (uint32_t)entry->size);
    media_zip_write_u32(archive, &offset, (uint32_t)entry->size);
    media_zip_write_u16(archive, &offset, (uint16_t)name_len);
    media_zip_write_u16(archive, &offset, 0);
    media_zip_write_u16(archive, &offset, 0);
    media_zip_write_u16(archive, &offset, 0);
    media_zip_write_u16(archive, &offset, 0);
    media_zip_write_u32(archive, &offset, 0);
    media_zip_write_u32(archive, &offset, (uint32_t)local_header_offset);
    for (size_t j = 0; j < name_len; j++)
      archive[offset++] = (uint8_t)entry->name[j];
  }

  central_dir_size = offset - central_dir_offset;
  media_zip_write_u32(archive, &offset, MEDIA_ZIP_END_SIG);
  media_zip_write_u16(archive, &offset, 0);
  media_zip_write_u16(archive, &offset, 0);
  media_zip_write_u16(archive, &offset, (uint16_t)builder.count);
  media_zip_write_u16(archive, &offset, (uint16_t)builder.count);
  media_zip_write_u32(archive, &offset, (uint32_t)central_dir_size);
  media_zip_write_u32(archive, &offset, (uint32_t)central_dir_offset);
  media_zip_write_u16(archive, &offset, 0);

  media_zip_builder_free(&builder);
  *out_data = archive;
  *out_size = offset;
  return 0;
}

static int media_zip_foreach(const uint8_t *data, size_t size,
                             media_zip_entry_cb_t cb, void *ctx) {
  size_t offset = 0;

  if (!data || size < 30 || !cb)
    return -EINVAL;

  while (offset + 30 <= size) {
    uint32_t sig;
    uint16_t method;
    uint16_t name_len;
    uint16_t extra_len;
    uint32_t comp_size;
    char name[256];

    sig = (uint32_t)data[offset] |
          ((uint32_t)data[offset + 1] << 8) |
          ((uint32_t)data[offset + 2] << 16) |
          ((uint32_t)data[offset + 3] << 24);
    if (sig != MEDIA_ZIP_LOCAL_HEADER_SIG)
      break;

    method = (uint16_t)data[offset + 8] | ((uint16_t)data[offset + 9] << 8);
    comp_size = (uint32_t)data[offset + 18] |
                ((uint32_t)data[offset + 19] << 8) |
                ((uint32_t)data[offset + 20] << 16) |
                ((uint32_t)data[offset + 21] << 24);
    name_len = (uint16_t)data[offset + 26] | ((uint16_t)data[offset + 27] << 8);
    extra_len = (uint16_t)data[offset + 28] | ((uint16_t)data[offset + 29] << 8);

    if (method != 0)
      return -EIO;
    if (offset + 30 + name_len + extra_len + comp_size > size)
      return -EIO;
    if (name_len >= sizeof(name))
      return -ENAMETOOLONG;

    for (size_t i = 0; i < name_len; i++)
      name[i] = (char)data[offset + 30 + i];
    name[name_len] = '\0';
    if (cb(ctx, name, data + offset + 30 + name_len + extra_len, comp_size,
           name_len > 0 && name[name_len - 1] == '/') != 0) {
      return -EIO;
    }

    offset += 30 + name_len + extra_len + comp_size;
  }

  return 0;
}

static int media_zip_count_cb(void *ctx, const char *name, const uint8_t *data,
                              size_t size, int is_dir) {
  int *count = (int *)ctx;
  size_t name_len = 0;

  (void)data;
  (void)size;

  if (!count || is_dir)
    return 0;
  if (!name)
    return 0;
  while (name[name_len])
    name_len++;
  if (name_len == 14 && name[0] == 'I' && name[1] == 'M' && name[2] == 'A' &&
      name[3] == 'G' && name[4] == 'E' && name[5] == '_' && name[6] == 'I' &&
      name[7] == 'N' && name[8] == 'F' && name[9] == 'O' && name[10] == '.' &&
      name[11] == 't' && name[12] == 'x' && name[13] == 't')
    return 0;
  (*count)++;
  return 0;
}

int media_zip_count_files(const uint8_t *data, size_t size) {
  int count = 0;
  if (media_zip_foreach(data, size, media_zip_count_cb, &count) != 0)
    return -1;
  return count;
}

typedef struct {
  const char *path;
  int found;
} media_zip_find_ctx_t;

static int media_zip_find_cb(void *ctx, const char *name, const uint8_t *data,
                             size_t size, int is_dir) {
  media_zip_find_ctx_t *find = (media_zip_find_ctx_t *)ctx;
  size_t idx = 0;
  size_t target_len = 0;
  size_t name_len = 0;

  (void)data;
  (void)size;

  if (!find || !find->path || is_dir)
    return 0;

  while (find->path[idx] == '/')
    idx++;
  if (find->path[idx] == '\0' || !name)
    return 0;

  while (find->path[idx + target_len])
    target_len++;
  while (name[name_len])
    name_len++;
  if (name_len != target_len)
    return 0;
  for (size_t i = 0; i < name_len; i++) {
    if (name[i] != find->path[idx + i])
      return 0;
  }
  find->found = 1;
  return 0;
}

int media_zip_has_entry(const uint8_t *data, size_t size, const char *path) {
  media_zip_find_ctx_t find;

  if (!data || !size || !path || !path[0])
    return 0;
  find.path = path;
  find.found = 0;
  if (media_zip_foreach(data, size, media_zip_find_cb, &find) != 0)
    return 0;
  return find.found;
}

typedef struct {
  const char *root;
  int copied_files;
  int failed_files;
} media_zip_extract_ctx_t;

static int media_zip_write_file(const char *path, const uint8_t *data,
                                size_t size) {
  struct file *f;
  ssize_t written = 0;

  if (!path || (!data && size > 0))
    return -EINVAL;

  media_ensure_parent_dirs(path);
  vfs_unlink(path);
  f = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (!f)
    return -ENOENT;
  if (size > 0)
    written = vfs_write(f, (const char *)data, size);
  vfs_close(f);
  if (written < 0)
    return (int)written;
  if ((size_t)written != size)
    return -EIO;
  return 0;
}

static int media_zip_extract_cb(void *ctx, const char *name, const uint8_t *data,
                                size_t size, int is_dir) {
  media_zip_extract_ctx_t *extract = (media_zip_extract_ctx_t *)ctx;
  char full_path[256];

  if (!extract || !extract->root || !extract->root[0] || !name || !name[0])
    return 0;

  if (media_zip_path_join(full_path, sizeof(full_path), extract->root, name) !=
      0)
    return 0;

  if (is_dir) {
    media_ensure_parent_dirs(full_path);
    vfs_mkdir(full_path, 0755);
    return 0;
  }

  if (media_zip_write_file(full_path, data, size) == 0)
    extract->copied_files++;
  else
    extract->failed_files++;
  return 0;
}

int media_zip_extract_to_root(const uint8_t *data, size_t size,
                              const char *dst_root, int *copied_files,
                              int *failed_files) {
  media_zip_extract_ctx_t extract;

  if (!data || !size || !dst_root || !dst_root[0])
    return -EINVAL;

  extract.root = dst_root;
  extract.copied_files = 0;
  extract.failed_files = 0;
  if (media_zip_foreach(data, size, media_zip_extract_cb, &extract) != 0)
    return -EIO;
  if (copied_files)
    *copied_files = extract.copied_files;
  if (failed_files)
    *failed_files = extract.failed_files;
  return (extract.copied_files > 0 && extract.failed_files == 0) ? 0 : -1;
}

typedef int (*media_zip_stream_entry_cb_t)(void *ctx, struct file *archive,
                                           const char *name,
                                           uint32_t comp_size, int is_dir);

static int media_zip_read_exact(struct file *file, uint8_t *buf, size_t size) {
  size_t total = 0;

  if (!file || (!buf && size > 0))
    return -EINVAL;
  while (total < size) {
    ssize_t read_len = vfs_read(file, (char *)buf + total, size - total);
    if (read_len <= 0)
      return -EIO;
    total += (size_t)read_len;
  }
  return 0;
}

static uint16_t media_zip_read_u16(const uint8_t *buf, size_t offset) {
  return (uint16_t)buf[offset] | ((uint16_t)buf[offset + 1] << 8);
}

static uint32_t media_zip_read_u32(const uint8_t *buf, size_t offset) {
  return (uint32_t)buf[offset] | ((uint32_t)buf[offset + 1] << 8) |
         ((uint32_t)buf[offset + 2] << 16) |
         ((uint32_t)buf[offset + 3] << 24);
}

static int media_zip_stream_skip(struct file *file, uint32_t size) {
  if (!file)
    return -EINVAL;
  if (size == 0)
    return 0;
  return vfs_lseek(file, (loff_t)size, SEEK_CUR) < 0 ? -EIO : 0;
}

static int media_zip_file_foreach(const char *archive_path,
                                  media_zip_stream_entry_cb_t cb, void *ctx) {
  struct file *archive;
  uint8_t header[30];
  int ret = 0;

  if (!archive_path || !archive_path[0] || !cb)
    return -EINVAL;

  archive = vfs_open(archive_path, O_RDONLY, 0);
  if (!archive)
    return -ENOENT;

  for (;;) {
    uint32_t sig;
    uint16_t flags;
    uint16_t method;
    uint32_t comp_size;
    uint16_t name_len;
    uint16_t extra_len;
    char name[256];

    ret = media_zip_read_exact(archive, header, sizeof(header));
    if (ret != 0) {
      ret = 0;
      break;
    }

    sig = media_zip_read_u32(header, 0);
    if (sig != MEDIA_ZIP_LOCAL_HEADER_SIG)
      break;

    flags = media_zip_read_u16(header, 6);
    method = media_zip_read_u16(header, 8);
    comp_size = media_zip_read_u32(header, 18);
    name_len = media_zip_read_u16(header, 26);
    extra_len = media_zip_read_u16(header, 28);

    if (method != 0 || (flags & 0x08u)) {
      ret = -EIO;
      break;
    }
    if (name_len >= sizeof(name)) {
      ret = -ENAMETOOLONG;
      break;
    }
    if (media_zip_read_exact(archive, (uint8_t *)name, name_len) != 0) {
      ret = -EIO;
      break;
    }
    name[name_len] = '\0';
    if (media_zip_stream_skip(archive, extra_len) != 0) {
      ret = -EIO;
      break;
    }

    ret = cb(ctx, archive, name, comp_size,
             name_len > 0 && name[name_len - 1] == '/');
    if (ret != 0)
      break;
  }

  vfs_close(archive);
  return ret;
}

static int media_zip_stream_count_cb(void *ctx, struct file *archive,
                                     const char *name, uint32_t comp_size,
                                     int is_dir) {
  int *count = (int *)ctx;
  size_t name_len = 0;

  if (!count || !name)
    return -EINVAL;
  while (name[name_len])
    name_len++;
  if (!is_dir &&
      !(name_len == 14 && name[0] == 'I' && name[1] == 'M' &&
        name[2] == 'A' && name[3] == 'G' && name[4] == 'E' &&
        name[5] == '_' && name[6] == 'I' && name[7] == 'N' &&
        name[8] == 'F' && name[9] == 'O' && name[10] == '.' &&
        name[11] == 't' && name[12] == 'x' && name[13] == 't')) {
    (*count)++;
  }
  return media_zip_stream_skip(archive, comp_size);
}

int media_zip_count_file_entries(const char *archive_path) {
  int count = 0;

  if (media_zip_file_foreach(archive_path, media_zip_stream_count_cb, &count) !=
      0)
    return -1;
  return count;
}

typedef struct {
  const char *path;
  int found;
} media_zip_stream_find_ctx_t;

static int media_zip_stream_find_cb(void *ctx, struct file *archive,
                                    const char *name, uint32_t comp_size,
                                    int is_dir) {
  media_zip_stream_find_ctx_t *find = (media_zip_stream_find_ctx_t *)ctx;
  size_t idx = 0;
  size_t target_len = 0;
  size_t name_len = 0;

  if (!find || !find->path)
    return -EINVAL;
  if (!is_dir && name) {
    while (find->path[idx] == '/')
      idx++;
    while (find->path[idx + target_len])
      target_len++;
    while (name[name_len])
      name_len++;
    if (name_len == target_len) {
      int matches = 1;
      for (size_t i = 0; i < name_len; i++) {
        if (name[i] != find->path[idx + i]) {
          matches = 0;
          break;
        }
      }
      if (matches)
        find->found = 1;
    }
  }
  return media_zip_stream_skip(archive, comp_size);
}

int media_zip_file_has_entry(const char *archive_path, const char *path) {
  media_zip_stream_find_ctx_t find;

  if (!archive_path || !archive_path[0] || !path || !path[0])
    return 0;
  find.path = path;
  find.found = 0;
  if (media_zip_file_foreach(archive_path, media_zip_stream_find_cb, &find) !=
      0)
    return 0;
  return find.found;
}

typedef struct {
  const char *root;
  int copied_files;
  int failed_files;
} media_zip_stream_extract_ctx_t;

static int media_zip_stream_write_file(struct file *archive, const char *path,
                                       uint32_t size) {
  struct file *out;
  uint8_t buf[512];
  uint32_t remaining = size;

  if (!archive || !path)
    return -EINVAL;

  media_ensure_parent_dirs(path);
  vfs_unlink(path);
  out = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (!out) {
    if (media_zip_stream_skip(archive, size) != 0)
      return -EIO;
    return -ENOENT;
  }

  while (remaining > 0) {
    size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
    ssize_t written;

    if (media_zip_read_exact(archive, buf, chunk) != 0) {
      vfs_close(out);
      return -EIO;
    }
    written = vfs_write(out, (const char *)buf, chunk);
    if (written < 0 || (size_t)written != chunk) {
      vfs_close(out);
      if (remaining > chunk)
        media_zip_stream_skip(archive, remaining - (uint32_t)chunk);
      return -ENOSPC;
    }
    remaining -= (uint32_t)chunk;
  }

  vfs_close(out);
  return 0;
}

static int media_zip_stream_extract_cb(void *ctx, struct file *archive,
                                       const char *name, uint32_t comp_size,
                                       int is_dir) {
  media_zip_stream_extract_ctx_t *extract =
      (media_zip_stream_extract_ctx_t *)ctx;
  char full_path[256];

  if (!extract || !extract->root || !extract->root[0] || !name || !name[0])
    return media_zip_stream_skip(archive, comp_size);
  if (media_zip_path_join(full_path, sizeof(full_path), extract->root, name) !=
      0)
    return media_zip_stream_skip(archive, comp_size);

  if (is_dir) {
    media_ensure_parent_dirs(full_path);
    vfs_mkdir(full_path, 0755);
    return media_zip_stream_skip(archive, comp_size);
  }

  {
    int ret = media_zip_stream_write_file(archive, full_path, comp_size);
    if (ret == 0)
      extract->copied_files++;
    else
      extract->failed_files++;
    return ret == -EIO ? -EIO : 0;
  }
}

int media_zip_extract_file_to_root(const char *archive_path,
                                   const char *dst_root, int *copied_files,
                                   int *failed_files) {
  media_zip_stream_extract_ctx_t extract;

  if (!archive_path || !archive_path[0] || !dst_root || !dst_root[0])
    return -EINVAL;

  extract.root = dst_root;
  extract.copied_files = 0;
  extract.failed_files = 0;
  if (media_zip_file_foreach(archive_path, media_zip_stream_extract_cb,
                             &extract) != 0)
    return -EIO;
  if (copied_files)
    *copied_files = extract.copied_files;
  if (failed_files)
    *failed_files = extract.failed_files;
  return (extract.copied_files > 0 && extract.failed_files == 0) ? 0 : -1;
}

/* --------------------------------------------------------------------- */
/* JPEG decoding (picojpeg)                                               */
/* --------------------------------------------------------------------- */

#include "picojpeg.h"

typedef struct {
  const uint8_t *data;
  size_t size;
  size_t offset;
} jpeg_mem_t;

static unsigned char jpeg_need_bytes(unsigned char *pBuf,
                                     unsigned char buf_size,
                                     unsigned char *pBytes_actually_read,
                                     void *pCallback_data) {
  jpeg_mem_t *mem = (jpeg_mem_t *)pCallback_data;
  if (!mem || mem->offset >= mem->size) {
    *pBytes_actually_read = 0;
    return 0;
  }

  size_t remaining = mem->size - mem->offset;
  size_t to_copy = remaining < buf_size ? remaining : buf_size;
  for (size_t i = 0; i < to_copy; i++) {
    pBuf[i] = mem->data[mem->offset + i];
  }

  mem->offset += to_copy;
  *pBytes_actually_read = (unsigned char)to_copy;
  return 0;
}

int media_decode_jpeg_buffer(const uint8_t *data, size_t size,
                             media_image_t *out, uint32_t *buffer,
                             size_t buffer_size) {
  if (!data || !size || !out)
    return -EINVAL;

  jpeg_mem_t mem = {data, size, 0};
  pjpeg_image_info_t info;
  unsigned char status = pjpeg_decode_init(&info, jpeg_need_bytes, &mem, 0);
  if (status) {
    printk(KERN_ERR "JPEG: decode_init failed (%u)\n", status);
    return -EINVAL;
  }

  if (info.m_width <= 0 || info.m_height <= 0)
    return -EINVAL;

  /* Check for integer overflow in pixel count calculation */
  if ((size_t)info.m_width > SIZE_MAX / (size_t)info.m_height) {
    printk(KERN_ERR "JPEG: dimensions too large (integer overflow)\n");
    return -EINVAL;
  }

  size_t pixel_count = (size_t)info.m_width * (size_t)info.m_height;

  /* Prevent excessively large allocations (64MB max image - 4K support) */
  if (pixel_count > 16 * 1024 * 1024) {
    printk(KERN_ERR "JPEG: image too large (%zu pixels)\n", pixel_count);
    return -EINVAL;
  }

  size_t required_bytes = pixel_count * sizeof(uint32_t);

  uint32_t *pixels = NULL;
  bool allocated = false;

  if (buffer) {
    if (buffer_size < required_bytes) {
      printk(KERN_ERR "JPEG: buffer too small (need %d, got %d)\n",
             (int)required_bytes, (int)buffer_size);
      return -ENOMEM;
    }
    pixels = buffer;
  } else {
    pixels = (uint32_t *)kmalloc(required_bytes, GFP_KERNEL);
    if (!pixels)
      return -ENOMEM;
    allocated = true;
  }

  int mcu_x = 0;
  int mcu_y = 0;
  while (1) {
    status = pjpeg_decode_mcu();
    if (status) {
      if (status == PJPG_NO_MORE_BLOCKS)
        break;
      printk(KERN_ERR "JPEG: decode_mcu failed (%u)\n", status);
      if (allocated)
        kfree(pixels);
      return -EINVAL;
    }

    int mcu_width = info.m_MCUWidth;
    int mcu_height = info.m_MCUHeight;
    int blocks_per_row = mcu_width / 8;

    for (int y = 0; y < mcu_height; y++) {
      int yy = mcu_y * mcu_height + y;
      if (yy >= info.m_height)
        continue;
      for (int x = 0; x < mcu_width; x++) {
        int xx = mcu_x * mcu_width + x;
        if (xx >= info.m_width)
          continue;

        int block_x = x / 8;
        int block_y = y / 8;
        int block_index = block_y * blocks_per_row + block_x;
        int block_offset = block_index * 64;
        int pixel_offset = block_offset + (y % 8) * 8 + (x % 8);

        uint8_t r = info.m_pMCUBufR[pixel_offset];
        uint8_t g = info.m_pMCUBufG ? info.m_pMCUBufG[pixel_offset] : r;
        uint8_t b = info.m_pMCUBufB ? info.m_pMCUBufB[pixel_offset] : r;
        pixels[yy * info.m_width + xx] =
            ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
      }
    }

    mcu_x++;
    if (mcu_x == info.m_MCUSPerRow) {
      mcu_x = 0;
      mcu_y++;
    }
  }

  out->width = (uint32_t)info.m_width;
  out->height = (uint32_t)info.m_height;
  out->pixels = pixels;
  return 0;
}

int media_decode_jpeg(const uint8_t *data, size_t size, media_image_t *out) {
  return media_decode_jpeg_buffer(data, size, out, NULL, 0);
}

void media_free_image(media_image_t *image) {
  if (!image)
    return;
  if (image->pixels) {
    kfree(image->pixels);
    image->pixels = NULL;
  }
  image->width = 0;
  image->height = 0;
}

/* --------------------------------------------------------------------- */
/* SVG decode (embedded data URI image extraction)                        */
/* --------------------------------------------------------------------- */

static int media_bytes_starts_with(const uint8_t *data, size_t size,
                                   size_t at, const char *needle) {
  size_t n = 0;
  if (!data || !needle || at >= size)
    return 0;
  while (needle[n]) {
    if (at + n >= size || data[at + n] != (uint8_t)needle[n])
      return 0;
    n++;
  }
  return 1;
}

static int media_find_bytes_from(const uint8_t *data, size_t size, size_t start,
                                 const char *needle, size_t *out_pos) {
  size_t needle_len = 0;
  if (!data || !needle || !out_pos)
    return -EINVAL;

  while (needle[needle_len])
    needle_len++;
  if (needle_len == 0 || needle_len > size)
    return -EINVAL;

  if (start >= size)
    return -ENOENT;

  for (size_t i = start; i + needle_len <= size; i++) {
    if (media_bytes_starts_with(data, size, i, needle)) {
      *out_pos = i;
      return 0;
    }
  }
  return -ENOENT;
}

static int media_base64_value(uint8_t ch) {
  if (ch >= 'A' && ch <= 'Z')
    return (int)(ch - 'A');
  if (ch >= 'a' && ch <= 'z')
    return (int)(ch - 'a') + 26;
  if (ch >= '0' && ch <= '9')
    return (int)(ch - '0') + 52;
  if (ch == '+')
    return 62;
  if (ch == '/')
    return 63;
  return -1;
}

static int media_decode_base64(const uint8_t *src, size_t src_len, uint8_t **out,
                               size_t *out_len) {
  uint8_t *dst;
  size_t cap;
  size_t wr = 0;
  uint32_t acc = 0;
  int acc_bits = 0;
  int saw_padding = 0;

  if (!src || !out || !out_len)
    return -EINVAL;

  cap = (src_len / 4) * 3 + 3;
  dst = (uint8_t *)kmalloc(cap, GFP_KERNEL);
  if (!dst)
    return -ENOMEM;

  for (size_t i = 0; i < src_len; i++) {
    uint8_t ch = src[i];
    int val;

    if (ch == '=')
      saw_padding = 1;
    if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ')
      continue;
    if (ch == '=') {
      break;
    }

    val = media_base64_value(ch);
    if (val < 0 || saw_padding) {
      kfree(dst);
      return -EINVAL;
    }

    acc = (acc << 6) | (uint32_t)val;
    acc_bits += 6;

    while (acc_bits >= 8) {
      acc_bits -= 8;
      if (wr >= cap) {
        kfree(dst);
        return -EFBIG;
      }
      dst[wr++] = (uint8_t)((acc >> acc_bits) & 0xFF);
    }
  }

  *out = dst;
  *out_len = wr;
  return 0;
}

typedef struct {
  double x;
  double y;
} svg_point_t;

typedef struct {
  double a, b, c, d, e, f;
} svg_transform_t;

typedef struct {
  int kind;
  uint32_t color;
  uint8_t alpha;
  char ref[32];
} svg_paint_t;

typedef struct {
  svg_paint_t fill;
  svg_paint_t stroke;
  uint32_t current_color;
  double font_size;
  double stroke_width;
  double stroke_dasharray[8];
  int stroke_dash_count;
  double stroke_dashoffset;
  int fill_rule_nonzero;
  int stroke_linecap;
  int stroke_linejoin;
  int text_anchor;
  int has_font_size;
  int display_none;
  int visibility_hidden;
  int preserve_space;
  double stroke_miterlimit;
  uint8_t opacity;
  uint8_t fill_opacity;
  uint8_t stroke_opacity;
} svg_style_t;

typedef struct {
  int seen_non_space;
  int pending_space;
} svg_text_ws_state_t;

#define SVG_GRADIENT_MAX_STOPS 8

typedef struct {
  char id[32];
  int kind;
  int units;
  int spread_method;
  svg_transform_t transform;
  double x1, y1, x2, y2;
  double cx, cy, r;
  double fx, fy, fr;
  uint32_t color0, color1;
  uint8_t alpha0, alpha1;
  double stop_offsets[SVG_GRADIENT_MAX_STOPS];
  uint32_t stop_colors[SVG_GRADIENT_MAX_STOPS];
  uint8_t stop_alphas[SVG_GRADIENT_MAX_STOPS];
  int stop_count;
} svg_gradient_t;

typedef struct {
  media_image_t image;
  svg_gradient_t gradients[16];
  int gradient_count;
  svg_transform_t root_transform;
} svg_render_ctx_t;

typedef struct {
  svg_point_t *points;
  uint8_t *moves;
  uint8_t *closes;
  int count;
  int cap;
} svg_path_buffer_t;

#define SVG_PAINT_NONE 0
#define SVG_PAINT_SOLID 1
#define SVG_PAINT_URL 2

#define SVG_GRADIENT_LINEAR 1
#define SVG_GRADIENT_RADIAL 2
#define SVG_GRADIENT_UNITS_OBJECT 0
#define SVG_GRADIENT_UNITS_USERSPACE 1
#define SVG_GRADIENT_SPREAD_PAD 0
#define SVG_GRADIENT_SPREAD_REFLECT 1
#define SVG_GRADIENT_SPREAD_REPEAT 2

#define SVG_STROKE_JOIN_MITER 0
#define SVG_STROKE_JOIN_ROUND 1
#define SVG_STROKE_JOIN_BEVEL 2
#define SVG_STROKE_CAP_BUTT 0
#define SVG_STROKE_CAP_ROUND 1
#define SVG_STROKE_CAP_SQUARE 2

#define SVG_TEXT_ANCHOR_START 0
#define SVG_TEXT_ANCHOR_MIDDLE 1
#define SVG_TEXT_ANCHOR_END 2

static double svg_absd(double v) { return v < 0.0 ? -v : v; }

static double svg_min(double a, double b) { return a < b ? a : b; }

static double svg_max(double a, double b) { return a > b ? a : b; }

static double svg_clamp(double v, double lo, double hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

static double svg_wrap_radians(double radians) {
  const double pi = 3.14159265358979323846;
  const double tau = 6.28318530717958647692;
  while (radians > pi)
    radians -= tau;
  while (radians < -pi)
    radians += tau;
  return radians;
}

static double svg_sin_approx(double radians) {
  double x = svg_wrap_radians(radians);
  double x2 = x * x;
  return x * (1.0 - x2 / 6.0 + (x2 * x2) / 120.0 -
              (x2 * x2 * x2) / 5040.0);
}

static double svg_cos_approx(double radians) {
  double x = svg_wrap_radians(radians);
  double x2 = x * x;
  return 1.0 - x2 / 2.0 + (x2 * x2) / 24.0 - (x2 * x2 * x2) / 720.0;
}

static double svg_tan_approx(double radians) {
  double c = svg_cos_approx(radians);
  if (svg_absd(c) < 1e-6)
    return radians >= 0.0 ? 1e6 : -1e6;
  return svg_sin_approx(radians) / c;
}

static double svg_sqrt_approx(double v) {
  double x;
  if (v <= 0.0)
    return 0.0;
  x = v > 1.0 ? v : 1.0;
  for (int i = 0; i < 8; i++)
    x = 0.5 * (x + v / x);
  return x;
}

static double svg_atan_approx(double z) {
  double az = svg_absd(z);
  if (az <= 1.0)
    return z / (1.0 + 0.28 * z * z);
  if (z > 0.0)
    return 1.57079632679489661923 - z / (z * z + 0.28);
  return -1.57079632679489661923 - z / (z * z + 0.28);
}

static double svg_atan2_approx(double y, double x) {
  if (x > 0.0)
    return svg_atan_approx(y / x);
  if (x < 0.0 && y >= 0.0)
    return svg_atan_approx(y / x) + 3.14159265358979323846;
  if (x < 0.0 && y < 0.0)
    return svg_atan_approx(y / x) - 3.14159265358979323846;
  if (y > 0.0)
    return 1.57079632679489661923;
  if (y < 0.0)
    return -1.57079632679489661923;
  return 0.0;
}

static void svg_parse_dasharray_value(const uint8_t *data, size_t len,
                                      svg_style_t *style);

static int svg_match_text(const uint8_t *s, size_t len, const char *text) {
  size_t i = 0;
  while (text[i]) {
    if (i >= len || s[i] != (uint8_t)text[i])
      return 0;
    i++;
  }
  return i == len;
}

static int svg_match_text_ci(const uint8_t *s, size_t len, const char *text) {
  size_t i = 0;
  while (text[i]) {
    uint8_t lhs;
    uint8_t rhs;
    if (i >= len)
      return 0;
    lhs = s[i];
    rhs = (uint8_t)text[i];
    if (lhs >= 'A' && lhs <= 'Z')
      lhs = (uint8_t)(lhs + ('a' - 'A'));
    if (rhs >= 'A' && rhs <= 'Z')
      rhs = (uint8_t)(rhs + ('a' - 'A'));
    if (lhs != rhs)
      return 0;
    i++;
  }
  return i == len;
}

static int svg_is_space(uint8_t ch) {
  return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == ',';
}

static int svg_tag_is_self_closing(const uint8_t *data, size_t start, size_t end) {
  size_t pos = end;
  if (!data || end <= start)
    return 0;
  while (pos > start && svg_is_space(data[pos - 1]))
    pos--;
  return pos > start && data[pos - 1] == '/';
}

static svg_transform_t svg_transform_identity(void) {
  svg_transform_t t = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
  return t;
}

static svg_transform_t svg_transform_translate(double tx, double ty) {
  svg_transform_t t = svg_transform_identity();
  t.e = tx;
  t.f = ty;
  return t;
}

static int svg_parse_number(const uint8_t *data, size_t size, size_t *pos,
                            double *out);
static int svg_parse_opacity_value(const uint8_t *data, size_t len,
                                   uint8_t *out_alpha);
static int svg_parse_color(const uint8_t *data, size_t len, uint32_t *color,
                           uint8_t *alpha);
static int svg_trimmed_range(const uint8_t *data, size_t start, size_t end,
                             size_t *trimmed_start, size_t *trimmed_end);
static int svg_attr_name_equals(const uint8_t *data, size_t start, size_t end,
                                const char *name);

static int svg_value_has_percent(const uint8_t *data, size_t start, size_t len) {
  size_t end = start + len;
  while (end > start && svg_is_space(data[end - 1]))
    end--;
  return end > start && data[end - 1] == '%';
}

static void svg_parse_gradient_number(const uint8_t *data, size_t start, size_t len,
                                      double *out) {
  size_t pos = start;
  if (!out)
    return;
  svg_parse_number(data, start + len, &pos, out);
  if (svg_value_has_percent(data, start, len))
    *out /= 100.0;
}

static double svg_viewport_normalized_diagonal(double width, double height) {
  if (width <= 0.0 && height <= 0.0)
    return 0.0;
  return svg_sqrt_approx((width * width + height * height) * 0.5);
}

static void svg_parse_length_number(const uint8_t *data, size_t start, size_t len,
                                    double percent_scale, double *out) {
  size_t pos = start;
  if (!out)
    return;
  svg_parse_number(data, start + len, &pos, out);
  if (svg_value_has_percent(data, start, len))
    *out = (*out * percent_scale) / 100.0;
}

static void svg_parse_stop_style_declarations(const uint8_t *data, size_t len,
                                              uint32_t *color,
                                              uint8_t *alpha) {
  size_t pos = 0;
  if (!data)
    return;
  while (pos < len) {
    size_t name_start = pos;
    size_t name_end;
    size_t value_start;
    size_t value_end;
    size_t trimmed_start;
    size_t trimmed_end;
    while (pos < len && data[pos] != ':' && data[pos] != ';')
      pos++;
    name_end = pos;
    if (pos >= len || data[pos] != ':') {
      while (pos < len && data[pos] != ';')
        pos++;
      if (pos < len)
        pos++;
      continue;
    }
    pos++;
    value_start = pos;
    while (pos < len && data[pos] != ';')
      pos++;
    value_end = pos;
    if (svg_trimmed_range(data, name_start, name_end, &trimmed_start,
                          &trimmed_end) == 0) {
      size_t value_trim_start;
      size_t value_trim_end;
      if (svg_trimmed_range(data, value_start, value_end, &value_trim_start,
                            &value_trim_end) == 0) {
        if (svg_attr_name_equals(data, trimmed_start, trimmed_end,
                                 "stop-color")) {
          svg_parse_color(data + value_trim_start,
                          value_trim_end - value_trim_start, color, alpha);
        } else if (svg_attr_name_equals(data, trimmed_start, trimmed_end,
                                        "stop-opacity")) {
          uint8_t parsed_alpha = *alpha;
          if (svg_parse_opacity_value(data + value_trim_start,
                                      value_trim_end - value_trim_start,
                                      &parsed_alpha) == 0) {
            *alpha = parsed_alpha;
          }
        }
      }
    }
    if (pos < len)
      pos++;
  }
}

static svg_transform_t svg_transform_multiply(svg_transform_t lhs,
                                              svg_transform_t rhs) {
  svg_transform_t out;
  out.a = lhs.a * rhs.a + lhs.c * rhs.b;
  out.b = lhs.b * rhs.a + lhs.d * rhs.b;
  out.c = lhs.a * rhs.c + lhs.c * rhs.d;
  out.d = lhs.b * rhs.c + lhs.d * rhs.d;
  out.e = lhs.a * rhs.e + lhs.c * rhs.f + lhs.e;
  out.f = lhs.b * rhs.e + lhs.d * rhs.f + lhs.f;
  return out;
}

static svg_point_t svg_transform_point(svg_transform_t t, svg_point_t p) {
  svg_point_t out;
  out.x = t.a * p.x + t.c * p.y + t.e;
  out.y = t.b * p.x + t.d * p.y + t.f;
  return out;
}

static int svg_parse_number(const uint8_t *data, size_t size, size_t *pos,
                            double *out) {
  int sign = 1;
  double value = 0.0;
  double frac = 0.0;
  double scale = 1.0;
  int saw_digit = 0;
  int exp_sign = 1;
  int exp_value = 0;
  int have_exp = 0;

  while (*pos < size && svg_is_space(data[*pos]))
    (*pos)++;
  if (*pos >= size)
    return -EINVAL;

  if (data[*pos] == '-') {
    sign = -1;
    (*pos)++;
  } else if (data[*pos] == '+') {
    (*pos)++;
  }

  while (*pos < size && data[*pos] >= '0' && data[*pos] <= '9') {
    value = value * 10.0 + (double)(data[*pos] - '0');
    (*pos)++;
    saw_digit = 1;
  }

  if (*pos < size && data[*pos] == '.') {
    (*pos)++;
    while (*pos < size && data[*pos] >= '0' && data[*pos] <= '9') {
      frac = frac * 10.0 + (double)(data[*pos] - '0');
      scale *= 10.0;
      (*pos)++;
      saw_digit = 1;
    }
  }

  if (!saw_digit)
    return -EINVAL;

  value += frac / scale;

  if (*pos < size && (data[*pos] == 'e' || data[*pos] == 'E')) {
    size_t exp_pos = *pos + 1;
    have_exp = 1;
    if (exp_pos < size && data[exp_pos] == '-') {
      exp_sign = -1;
      exp_pos++;
    } else if (exp_pos < size && data[exp_pos] == '+') {
      exp_pos++;
    }
    if (exp_pos >= size || data[exp_pos] < '0' || data[exp_pos] > '9')
      return -EINVAL;
    while (exp_pos < size && data[exp_pos] >= '0' && data[exp_pos] <= '9') {
      exp_value = exp_value * 10 + (int)(data[exp_pos] - '0');
      exp_pos++;
    }
    *pos = exp_pos;
  }

  if (have_exp) {
    while (exp_value-- > 0) {
      if (exp_sign > 0)
        value *= 10.0;
      else
        value /= 10.0;
    }
  }

  *out = value * (double)sign;
  while (*pos < size && svg_is_space(data[*pos]))
    (*pos)++;
  return 0;
}

static int svg_find_attr(const uint8_t *data, size_t start, size_t end,
                         const char *name, size_t *value_start,
                         size_t *value_len) {
  size_t name_len = 0;
  while (name[name_len])
    name_len++;
  for (size_t i = start; i + name_len + 2 <= end; i++) {
    if ((i > start && !svg_is_space(data[i - 1]) && data[i - 1] != '<') ||
        !media_bytes_starts_with(data, end, i, name))
      continue;
    i += name_len;
    while (i < end && svg_is_space(data[i]))
      i++;
    if (i >= end || data[i] != '=')
      continue;
    i++;
    while (i < end && svg_is_space(data[i]))
      i++;
    if (i >= end || (data[i] != '"' && data[i] != '\''))
      continue;
    {
      uint8_t quote = data[i++];
      size_t val = i;
      while (i < end && data[i] != quote)
        i++;
      if (i <= end) {
        *value_start = val;
        *value_len = i - val;
        return 0;
      }
    }
  }
  return -ENOENT;
}

static int svg_find_element_by_id(const uint8_t *data, size_t size, const char *id,
                                  size_t *tag_start, size_t *tag_end) {
  size_t id_len = 0;

  if (!data || !id)
    return -EINVAL;
  while (id[id_len])
    id_len++;

  for (size_t i = 0; i < size; i++) {
    size_t end;
    size_t value_start = 0;
    size_t value_len = 0;

    if (data[i] != '<')
      continue;
    if (i + 1 < size && (data[i + 1] == '/' || data[i + 1] == '!' ||
                         data[i + 1] == '?'))
      continue;
    end = i + 1;
    while (end < size && data[end] != '>')
      end++;
    if (end >= size)
      break;
    if (svg_find_attr(data, i, end, "id", &value_start, &value_len) == 0 &&
        value_len == id_len &&
        media_bytes_starts_with(data + value_start, value_len, 0, id)) {
      if (tag_start)
        *tag_start = i;
      if (tag_end)
        *tag_end = end;
      return 0;
    }
    i = end;
  }
  return -ENOENT;
}

static uint8_t svg_ascii_tolower(uint8_t ch) {
  if (ch >= 'A' && ch <= 'Z')
    return (uint8_t)(ch + ('a' - 'A'));
  return ch;
}

static int svg_parse_hex_digit(uint8_t ch) {
  ch = svg_ascii_tolower(ch);
  if (ch >= '0' && ch <= '9')
    return (int)(ch - '0');
  if (ch >= 'a' && ch <= 'f')
    return 10 + (int)(ch - 'a');
  return -1;
}

static uint8_t svg_color_component_from_unit(double value) {
  value = svg_clamp(value, 0.0, 255.0);
  return (uint8_t)(value + 0.5);
}

static int svg_parse_rgb_component(const uint8_t *data, size_t len, size_t *pos,
                                   uint8_t *out) {
  double value = 0.0;
  if (!out || svg_parse_number(data, len, pos, &value) != 0)
    return -EINVAL;
  if (*pos < len && data[*pos] == '%') {
    value = svg_clamp(value, 0.0, 100.0) * 255.0 / 100.0;
    (*pos)++;
    while (*pos < len && svg_is_space(data[*pos]))
      (*pos)++;
  }
  *out = svg_color_component_from_unit(value);
  return 0;
}

static int svg_parse_alpha_component(const uint8_t *data, size_t len, size_t *pos,
                                     uint8_t *out) {
  double value = 1.0;
  if (!out || svg_parse_number(data, len, pos, &value) != 0)
    return -EINVAL;
  if (*pos < len && data[*pos] == '%') {
    value = svg_clamp(value, 0.0, 100.0) / 100.0;
    (*pos)++;
    while (*pos < len && svg_is_space(data[*pos]))
      (*pos)++;
  }
  value = svg_clamp(value, 0.0, 1.0);
  *out = (uint8_t)(value * 255.0 + 0.5);
  return 0;
}

static int svg_parse_color(const uint8_t *data, size_t len, uint32_t *color,
                           uint8_t *alpha) {
  typedef struct {
    const char *name;
    uint32_t color;
  } svg_named_color_t;
  static const svg_named_color_t k_named_colors[] = {
      {"black", 0x000000},   {"white", 0xFFFFFF}, {"red", 0xFF0000},
      {"green", 0x008000},   {"blue", 0x0000FF},  {"yellow", 0xFFFF00},
      {"gray", 0x808080},    {"grey", 0x808080},  {"silver", 0xC0C0C0},
      {"maroon", 0x800000},  {"purple", 0x800080},{"fuchsia", 0xFF00FF},
      {"lime", 0x00FF00},    {"olive", 0x808000}, {"navy", 0x000080},
      {"teal", 0x008080},    {"aqua", 0x00FFFF},  {"cyan", 0x00FFFF},
      {"magenta", 0xFF00FF}, {"orange", 0xFFA500},{"brown", 0xA52A2A},
  };
  if (len == 4 && data[0] == '#') {
    int r = svg_parse_hex_digit(data[1]);
    int g = svg_parse_hex_digit(data[2]);
    int b = svg_parse_hex_digit(data[3]);
    if (r < 0 || g < 0 || b < 0)
      return -EINVAL;
    *color = (uint32_t)((r << 20) | (r << 16) | (g << 12) | (g << 8) |
                        (b << 4) | b);
    *alpha = 255;
    return 0;
  }
  if (len == 5 && data[0] == '#') {
    int r = svg_parse_hex_digit(data[1]);
    int g = svg_parse_hex_digit(data[2]);
    int b = svg_parse_hex_digit(data[3]);
    int a = svg_parse_hex_digit(data[4]);
    if (r < 0 || g < 0 || b < 0 || a < 0)
      return -EINVAL;
    *color = (uint32_t)((r << 20) | (r << 16) | (g << 12) | (g << 8) |
                        (b << 4) | b);
    *alpha = (uint8_t)((a << 4) | a);
    return 0;
  }
  if (len == 7 && data[0] == '#') {
    *color = 0;
    for (size_t i = 1; i < 7; i++) {
      int value = svg_parse_hex_digit(data[i]);
      if (value < 0 || value > 15)
        return -EINVAL;
      *color = (*color << 4) | (uint32_t)value;
    }
    *alpha = 255;
    return 0;
  }
  if (len == 9 && data[0] == '#') {
    uint32_t rgba = 0;
    for (size_t i = 1; i < 9; i++) {
      int value = svg_parse_hex_digit(data[i]);
      if (value < 0 || value > 15)
        return -EINVAL;
      rgba = (rgba << 4) | (uint32_t)value;
    }
    *color = (rgba >> 8) & 0xFFFFFFU;
    *alpha = (uint8_t)(rgba & 0xFFU);
    return 0;
  }
  if (svg_match_text_ci(data, len, "none"))
    return 1;
  if (svg_match_text_ci(data, len, "transparent")) {
    *color = 0x000000;
    *alpha = 0;
    return 0;
  }
  if (svg_match_text_ci(data, len, "currentColor")) {
    *color = 0x000000;
    *alpha = 255;
    return 0;
  }
  if (len >= 5 && svg_ascii_tolower(data[0]) == 'r' &&
      svg_ascii_tolower(data[1]) == 'g' && svg_ascii_tolower(data[2]) == 'b') {
    int has_alpha = 0;
    size_t pos;
    uint8_t r, g, b, a = 255;
    if (svg_ascii_tolower(data[3]) == 'a')
      has_alpha = 1;
    if ((has_alpha && (len < 6 || data[4] != '(')) ||
        (!has_alpha && (len < 5 || data[3] != '(')))
      return -EINVAL;
    pos = has_alpha ? 5 : 4;
    if (svg_parse_rgb_component(data, len, &pos, &r) != 0 ||
        svg_parse_rgb_component(data, len, &pos, &g) != 0 ||
        svg_parse_rgb_component(data, len, &pos, &b) != 0)
      return -EINVAL;
    if (has_alpha) {
      while (pos < len && (svg_is_space(data[pos]) || data[pos] == '/'))
        pos++;
      if (svg_parse_alpha_component(data, len, &pos, &a) != 0)
        return -EINVAL;
    }
    while (pos < len && svg_is_space(data[pos]))
      pos++;
    if (pos >= len || data[pos] != ')')
      return -EINVAL;
    *color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    *alpha = a;
    return 0;
  }
  for (size_t i = 0; i < sizeof(k_named_colors) / sizeof(k_named_colors[0]); i++) {
    if (svg_match_text_ci(data, len, k_named_colors[i].name)) {
      *color = k_named_colors[i].color;
      *alpha = 255;
      return 0;
    }
  }
  return -EINVAL;
}

static int svg_parse_opacity_value(const uint8_t *data, size_t len,
                                   uint8_t *out_alpha) {
  size_t pos = 0;
  double opacity = 1.0;

  if (!data || !out_alpha)
    return -EINVAL;
  if (svg_parse_number(data, len, &pos, &opacity) != 0)
    return -EINVAL;
  if (pos < len && data[pos] == '%')
    opacity /= 100.0;
  opacity = svg_clamp(opacity, 0.0, 1.0);
  *out_alpha = (uint8_t)(opacity * 255.0 + 0.5);
  return 0;
}

static void svg_init_default_style(svg_style_t *style) {
  style->fill.kind = SVG_PAINT_SOLID;
  style->fill.color = 0x000000;
  style->fill.alpha = 255;
  style->fill.ref[0] = '\0';
  style->stroke.kind = SVG_PAINT_NONE;
  style->stroke.color = 0x000000;
  style->stroke.alpha = 255;
  style->stroke.ref[0] = '\0';
  style->current_color = 0x000000;
  style->font_size = 16.0;
  style->stroke_width = 1.0;
  style->stroke_dash_count = 0;
  style->stroke_dashoffset = 0.0;
  style->fill_rule_nonzero = 1;
  style->stroke_linecap = SVG_STROKE_CAP_BUTT;
  style->stroke_linejoin = SVG_STROKE_JOIN_MITER;
  style->text_anchor = SVG_TEXT_ANCHOR_START;
  style->has_font_size = 0;
  style->display_none = 0;
  style->visibility_hidden = 0;
  style->preserve_space = 0;
  style->stroke_miterlimit = 4.0;
  style->opacity = 255;
  style->fill_opacity = 255;
  style->stroke_opacity = 255;
}

static int svg_style_is_displayed(const svg_style_t *style) {
  return style && !style->display_none;
}

static int svg_style_is_visible(const svg_style_t *style) {
  return style && !style->visibility_hidden;
}

static void svg_copy_ref(char *dst, size_t dst_size, const uint8_t *src,
                         size_t len) {
  size_t n = 0;
  if (!dst || dst_size == 0)
    return;
  while (n + 1 < dst_size && n < len) {
    dst[n] = (char)src[n];
    n++;
  }
  dst[n] = '\0';
}

static void svg_parse_paint(const uint8_t *data, size_t len, svg_paint_t *paint) {
  uint32_t color = 0;
  uint8_t alpha = 255;
  if (len >= 6 && media_bytes_starts_with(data, len, 0, "url(#") &&
      data[len - 1] == ')') {
    paint->kind = SVG_PAINT_URL;
    paint->color = 0;
    paint->alpha = 255;
    svg_copy_ref(paint->ref, sizeof(paint->ref), data + 5, len - 6);
    return;
  }
  if (svg_parse_color(data, len, &color, &alpha) == 0) {
    paint->kind = SVG_PAINT_SOLID;
    paint->color = color;
    paint->alpha = alpha;
    paint->ref[0] = '\0';
    return;
  }
  if (svg_parse_color(data, len, &color, &alpha) == 1) {
    paint->kind = SVG_PAINT_NONE;
    paint->ref[0] = '\0';
  }
}

static void svg_parse_paint_with_current(const uint8_t *data, size_t len,
                                         uint32_t current_color,
                                         svg_paint_t *paint) {
  if (len == 12 && svg_match_text(data, len, "currentColor")) {
    paint->kind = SVG_PAINT_SOLID;
    paint->color = current_color;
    paint->alpha = 255;
    paint->ref[0] = '\0';
    return;
  }
  svg_parse_paint(data, len, paint);
}

static int svg_trimmed_range(const uint8_t *data, size_t start, size_t end,
                             size_t *trimmed_start, size_t *trimmed_end) {
  while (start < end && svg_is_space(data[start]))
    start++;
  while (end > start && svg_is_space(data[end - 1]))
    end--;
  if (trimmed_start)
    *trimmed_start = start;
  if (trimmed_end)
    *trimmed_end = end;
  return end > start ? 0 : -ENOENT;
}

static int svg_attr_name_equals(const uint8_t *data, size_t start, size_t end,
                                const char *name) {
  size_t idx = 0;
  while (start < end && name[idx]) {
    if (data[start] != (uint8_t)name[idx])
      return 0;
    start++;
    idx++;
  }
  return start == end && name[idx] == '\0';
}

static void svg_apply_style_property(const uint8_t *name, size_t name_len,
                                     const uint8_t *value, size_t value_len,
                                     svg_style_t *style) {
  size_t ns = 0, ne = name_len, vs = 0, ve = value_len;
  size_t pos;
  double width;

  if (!style)
    return;
  if (svg_trimmed_range(name, 0, name_len, &ns, &ne) != 0 ||
      svg_trimmed_range(value, 0, value_len, &vs, &ve) != 0)
    return;

  if (svg_attr_name_equals(name, ns, ne, "fill")) {
    svg_parse_paint_with_current(value + vs, ve - vs, style->current_color,
                                 &style->fill);
  } else if (svg_attr_name_equals(name, ns, ne, "stroke")) {
    svg_parse_paint_with_current(value + vs, ve - vs, style->current_color,
                                 &style->stroke);
  } else if (svg_attr_name_equals(name, ns, ne, "color")) {
    uint32_t color = style->current_color;
    uint8_t alpha = 255;
    if (svg_parse_color(value + vs, ve - vs, &color, &alpha) == 0)
      style->current_color = color;
  } else if (svg_attr_name_equals(name, ns, ne, "stroke-width")) {
    pos = vs;
    width = style->stroke_width;
    if (svg_parse_number(value, ve, &pos, &width) == 0)
      style->stroke_width = width;
  } else if (svg_attr_name_equals(name, ns, ne, "stroke-dasharray")) {
    svg_parse_dasharray_value(value + vs, ve - vs, style);
  } else if (svg_attr_name_equals(name, ns, ne, "stroke-dashoffset")) {
    double offset = style->stroke_dashoffset;
    pos = vs;
    if (svg_parse_number(value, ve, &pos, &offset) == 0)
      style->stroke_dashoffset = offset;
  } else if (svg_attr_name_equals(name, ns, ne, "fill-rule")) {
    style->fill_rule_nonzero =
        svg_match_text(value + vs, ve - vs, "nonzero") ? 1 : 0;
  } else if (svg_attr_name_equals(name, ns, ne, "stroke-linecap")) {
    if (svg_match_text(value + vs, ve - vs, "round"))
      style->stroke_linecap = SVG_STROKE_CAP_ROUND;
    else if (svg_match_text(value + vs, ve - vs, "square"))
      style->stroke_linecap = SVG_STROKE_CAP_SQUARE;
    else
      style->stroke_linecap = SVG_STROKE_CAP_BUTT;
  } else if (svg_attr_name_equals(name, ns, ne, "stroke-linejoin")) {
    if (svg_match_text(value + vs, ve - vs, "round"))
      style->stroke_linejoin = SVG_STROKE_JOIN_ROUND;
    else if (svg_match_text(value + vs, ve - vs, "bevel"))
      style->stroke_linejoin = SVG_STROKE_JOIN_BEVEL;
    else
      style->stroke_linejoin = SVG_STROKE_JOIN_MITER;
  } else if (svg_attr_name_equals(name, ns, ne, "text-anchor")) {
    if (svg_match_text(value + vs, ve - vs, "middle"))
      style->text_anchor = SVG_TEXT_ANCHOR_MIDDLE;
    else if (svg_match_text(value + vs, ve - vs, "end"))
      style->text_anchor = SVG_TEXT_ANCHOR_END;
    else
      style->text_anchor = SVG_TEXT_ANCHOR_START;
  } else if (svg_attr_name_equals(name, ns, ne, "font-size")) {
    double size = style->font_size;
    pos = vs;
    if (svg_parse_number(value, ve, &pos, &size) == 0 && size > 0.0) {
      style->font_size = size;
      style->has_font_size = 1;
    }
  } else if (svg_attr_name_equals(name, ns, ne, "stroke-miterlimit")) {
    double limit = style->stroke_miterlimit;
    pos = vs;
    if (svg_parse_number(value, ve, &pos, &limit) == 0 && limit > 0.0)
      style->stroke_miterlimit = limit;
  } else if (svg_attr_name_equals(name, ns, ne, "display")) {
    style->display_none = svg_match_text(value + vs, ve - vs, "none") ? 1 : 0;
  } else if (svg_attr_name_equals(name, ns, ne, "visibility")) {
    style->visibility_hidden =
        (svg_match_text(value + vs, ve - vs, "hidden") ||
         svg_match_text(value + vs, ve - vs, "collapse"))
            ? 1
            : 0;
  } else if (svg_attr_name_equals(name, ns, ne, "opacity")) {
    svg_parse_opacity_value(value + vs, ve - vs, &style->opacity);
  } else if (svg_attr_name_equals(name, ns, ne, "fill-opacity")) {
    svg_parse_opacity_value(value + vs, ve - vs, &style->fill_opacity);
  } else if (svg_attr_name_equals(name, ns, ne, "stroke-opacity")) {
    svg_parse_opacity_value(value + vs, ve - vs, &style->stroke_opacity);
  }
}

static void svg_parse_dasharray_value(const uint8_t *data, size_t len,
                                      svg_style_t *style) {
  size_t pos = 0;
  int count = 0;
  int have_positive = 0;

  if (!data || !style)
    return;
  if (svg_match_text(data, len, "none")) {
    style->stroke_dash_count = 0;
    style->stroke_dashoffset = 0.0;
    return;
  }

  while (pos < len && count < (int)(sizeof(style->stroke_dasharray) /
                                    sizeof(style->stroke_dasharray[0]))) {
    double value = 0.0;
    while (pos < len && svg_is_space(data[pos]))
      pos++;
    if (pos >= len)
      break;
    if (svg_parse_number(data, len, &pos, &value) != 0)
      break;
    if (value < 0.0)
      value = 0.0;
    if (value > 0.0)
      have_positive = 1;
    style->stroke_dasharray[count++] = value;
  }

  if (!have_positive)
    count = 0;
  style->stroke_dash_count = count;
}

static void svg_apply_style_declarations(const uint8_t *data, size_t len,
                                         svg_style_t *style) {
  size_t pos = 0;

  while (pos < len) {
    size_t name_start = pos;
    size_t name_end;
    size_t value_start;
    size_t value_end;

    while (pos < len && data[pos] != ':' && data[pos] != ';')
      pos++;
    name_end = pos;
    if (pos >= len || data[pos] != ':') {
      while (pos < len && data[pos] != ';')
        pos++;
      if (pos < len)
        pos++;
      continue;
    }

    pos++;
    value_start = pos;
    while (pos < len && data[pos] != ';')
      pos++;
    value_end = pos;
    svg_apply_style_property(data + name_start, name_end - name_start,
                             data + value_start, value_end - value_start, style);
    if (pos < len)
      pos++;
  }
}

static void svg_apply_style_attrs(const uint8_t *data, size_t start, size_t end,
                                  svg_style_t *style) {
  size_t value_start = 0, value_len = 0;
  uint32_t color = style->current_color;
  uint8_t alpha = 255;
  if (svg_find_attr(data, start, end, "color", &value_start, &value_len) == 0 &&
      svg_parse_color(data + value_start, value_len, &color, &alpha) == 0)
    style->current_color = color;
  if (svg_find_attr(data, start, end, "fill", &value_start, &value_len) == 0)
    svg_parse_paint_with_current(data + value_start, value_len,
                                 style->current_color, &style->fill);
  if (svg_find_attr(data, start, end, "stroke", &value_start, &value_len) == 0)
    svg_parse_paint_with_current(data + value_start, value_len,
                                 style->current_color, &style->stroke);
  if (svg_find_attr(data, start, end, "stroke-width", &value_start, &value_len) == 0) {
    size_t pos = value_start;
    double width = style->stroke_width;
    if (svg_parse_number(data, end, &pos, &width) == 0)
      style->stroke_width = width;
  }
  if (svg_find_attr(data, start, end, "stroke-dasharray", &value_start, &value_len) == 0)
    svg_parse_dasharray_value(data + value_start, value_len, style);
  if (svg_find_attr(data, start, end, "stroke-dashoffset", &value_start, &value_len) == 0) {
    size_t pos = value_start;
    double offset = style->stroke_dashoffset;
    if (svg_parse_number(data, end, &pos, &offset) == 0)
      style->stroke_dashoffset = offset;
  }
  if (svg_find_attr(data, start, end, "fill-rule", &value_start, &value_len) == 0) {
    style->fill_rule_nonzero =
        svg_match_text(data + value_start, value_len, "nonzero") ? 1 : 0;
  }
  if (svg_find_attr(data, start, end, "stroke-linecap", &value_start, &value_len) == 0) {
    if (svg_match_text(data + value_start, value_len, "round"))
      style->stroke_linecap = SVG_STROKE_CAP_ROUND;
    else if (svg_match_text(data + value_start, value_len, "square"))
      style->stroke_linecap = SVG_STROKE_CAP_SQUARE;
    else
      style->stroke_linecap = SVG_STROKE_CAP_BUTT;
  }
  if (svg_find_attr(data, start, end, "stroke-linejoin", &value_start, &value_len) == 0) {
    if (svg_match_text(data + value_start, value_len, "round"))
      style->stroke_linejoin = SVG_STROKE_JOIN_ROUND;
    else if (svg_match_text(data + value_start, value_len, "bevel"))
      style->stroke_linejoin = SVG_STROKE_JOIN_BEVEL;
    else
      style->stroke_linejoin = SVG_STROKE_JOIN_MITER;
  }
  if (svg_find_attr(data, start, end, "text-anchor", &value_start, &value_len) == 0) {
    if (svg_match_text(data + value_start, value_len, "middle"))
      style->text_anchor = SVG_TEXT_ANCHOR_MIDDLE;
    else if (svg_match_text(data + value_start, value_len, "end"))
      style->text_anchor = SVG_TEXT_ANCHOR_END;
    else
      style->text_anchor = SVG_TEXT_ANCHOR_START;
  }
  if (svg_find_attr(data, start, end, "font-size", &value_start, &value_len) == 0) {
    size_t pos = value_start;
    double size = style->font_size;
    if (svg_parse_number(data, end, &pos, &size) == 0 && size > 0.0) {
      style->font_size = size;
      style->has_font_size = 1;
    }
  }
  if (svg_find_attr(data, start, end, "stroke-miterlimit", &value_start, &value_len) == 0) {
    size_t pos = value_start;
    double limit = style->stroke_miterlimit;
    if (svg_parse_number(data, end, &pos, &limit) == 0 && limit > 0.0)
      style->stroke_miterlimit = limit;
  }
  if (svg_find_attr(data, start, end, "display", &value_start, &value_len) == 0)
    style->display_none = svg_match_text(data + value_start, value_len, "none") ? 1 : 0;
  if (svg_find_attr(data, start, end, "visibility", &value_start, &value_len) == 0) {
    style->visibility_hidden =
        (svg_match_text(data + value_start, value_len, "hidden") ||
         svg_match_text(data + value_start, value_len, "collapse"))
            ? 1
            : 0;
  }
  if (svg_find_attr(data, start, end, "xml:space", &value_start, &value_len) == 0) {
    if (svg_match_text(data + value_start, value_len, "preserve"))
      style->preserve_space = 1;
    else if (svg_match_text(data + value_start, value_len, "default"))
      style->preserve_space = 0;
  }
  if (svg_find_attr(data, start, end, "opacity", &value_start, &value_len) == 0)
    svg_parse_opacity_value(data + value_start, value_len, &style->opacity);
  if (svg_find_attr(data, start, end, "fill-opacity", &value_start, &value_len) == 0)
    svg_parse_opacity_value(data + value_start, value_len, &style->fill_opacity);
  if (svg_find_attr(data, start, end, "stroke-opacity", &value_start, &value_len) == 0)
    svg_parse_opacity_value(data + value_start, value_len, &style->stroke_opacity);
  if (svg_find_attr(data, start, end, "style", &value_start, &value_len) == 0)
    svg_apply_style_declarations(data + value_start, value_len, style);
}

static svg_transform_t svg_parse_transform_value(const uint8_t *data, size_t len) {
  svg_transform_t out = svg_transform_identity();
  size_t pos = 0;
  while (pos < len) {
    while (pos < len && svg_is_space(data[pos]))
      pos++;
    if (pos >= len)
      break;
    if (pos + 10 <= len && svg_match_text(data + pos, 9, "translate")) {
      double tx = 0.0, ty = 0.0;
      svg_transform_t t = svg_transform_identity();
      pos += 9;
      while (pos < len && data[pos] != '(')
        pos++;
      if (pos < len)
        pos++;
      svg_parse_number(data, len, &pos, &tx);
      if (pos < len && data[pos] != ')')
        svg_parse_number(data, len, &pos, &ty);
      while (pos < len && data[pos] != ')')
        pos++;
      if (pos < len)
        pos++;
      t.e = tx;
      t.f = ty;
      out = svg_transform_multiply(out, t);
    } else if (pos + 6 <= len && svg_match_text(data + pos, 5, "scale")) {
      double sx = 1.0, sy = 1.0;
      svg_transform_t t = svg_transform_identity();
      pos += 5;
      while (pos < len && data[pos] != '(')
        pos++;
      if (pos < len)
        pos++;
      svg_parse_number(data, len, &pos, &sx);
      sy = sx;
      if (pos < len && data[pos] != ')')
        svg_parse_number(data, len, &pos, &sy);
      while (pos < len && data[pos] != ')')
        pos++;
      if (pos < len)
        pos++;
      t.a = sx;
      t.d = sy;
      out = svg_transform_multiply(out, t);
    } else if (pos + 7 <= len && svg_match_text(data + pos, 6, "matrix")) {
      double v[6] = {1, 0, 0, 1, 0, 0};
      svg_transform_t t;
      pos += 6;
      while (pos < len && data[pos] != '(')
        pos++;
      if (pos < len)
        pos++;
      for (int i = 0; i < 6; i++)
        svg_parse_number(data, len, &pos, &v[i]);
      while (pos < len && data[pos] != ')')
        pos++;
      if (pos < len)
        pos++;
      t.a = v[0];
      t.b = v[1];
      t.c = v[2];
      t.d = v[3];
      t.e = v[4];
      t.f = v[5];
      out = svg_transform_multiply(out, t);
    } else if (pos + 7 <= len && svg_match_text(data + pos, 6, "rotate")) {
      double angle = 0.0;
      double cx = 0.0, cy = 0.0;
      double radians;
      double s, c;
      svg_transform_t t;
      pos += 6;
      while (pos < len && data[pos] != '(')
        pos++;
      if (pos < len)
        pos++;
      svg_parse_number(data, len, &pos, &angle);
      if (pos < len && data[pos] != ')') {
        svg_parse_number(data, len, &pos, &cx);
        if (pos < len && data[pos] != ')')
          svg_parse_number(data, len, &pos, &cy);
      }
      while (pos < len && data[pos] != ')')
        pos++;
      if (pos < len)
        pos++;
      radians = angle * 3.14159265358979323846 / 180.0;
      s = svg_sin_approx(radians);
      c = svg_cos_approx(radians);
      t.a = c;
      t.b = s;
      t.c = -s;
      t.d = c;
      t.e = cx - c * cx + s * cy;
      t.f = cy - s * cx - c * cy;
      out = svg_transform_multiply(out, t);
    } else if (pos + 6 <= len && svg_match_text(data + pos, 5, "skewX")) {
      double angle = 0.0;
      double radians;
      svg_transform_t t = svg_transform_identity();
      pos += 5;
      while (pos < len && data[pos] != '(')
        pos++;
      if (pos < len)
        pos++;
      svg_parse_number(data, len, &pos, &angle);
      while (pos < len && data[pos] != ')')
        pos++;
      if (pos < len)
        pos++;
      radians = angle * 3.14159265358979323846 / 180.0;
      t.c = svg_tan_approx(radians);
      out = svg_transform_multiply(out, t);
    } else if (pos + 6 <= len && svg_match_text(data + pos, 5, "skewY")) {
      double angle = 0.0;
      double radians;
      svg_transform_t t = svg_transform_identity();
      pos += 5;
      while (pos < len && data[pos] != '(')
        pos++;
      if (pos < len)
        pos++;
      svg_parse_number(data, len, &pos, &angle);
      while (pos < len && data[pos] != ')')
        pos++;
      if (pos < len)
        pos++;
      radians = angle * 3.14159265358979323846 / 180.0;
      t.b = svg_tan_approx(radians);
      out = svg_transform_multiply(out, t);
    } else {
      pos++;
    }
  }
  return out;
}

static svg_transform_t svg_parse_transform_attr(const uint8_t *data, size_t start,
                                                size_t end) {
  size_t value_start = 0, value_len = 0;
  if (svg_find_attr(data, start, end, "transform", &value_start, &value_len) != 0)
    return svg_transform_identity();
  return svg_parse_transform_value(data + value_start, value_len);
}

static void svg_blend_pixel(media_image_t *img, int x, int y, uint32_t color,
                            uint8_t alpha) {
  uint32_t *dst;
  uint32_t bg;
  uint32_t rb, gb, bb;
  uint32_t rf, gf, bf;
  if (!img || !img->pixels || x < 0 || y < 0 || x >= (int)img->width ||
      y >= (int)img->height || alpha == 0)
    return;
  dst = &img->pixels[y * img->width + x];
  bg = *dst;
  rb = (bg >> 16) & 0xFF;
  gb = (bg >> 8) & 0xFF;
  bb = bg & 0xFF;
  rf = (color >> 16) & 0xFF;
  gf = (color >> 8) & 0xFF;
  bf = color & 0xFF;
  rb = (rb * (255 - alpha) + rf * alpha) / 255;
  gb = (gb * (255 - alpha) + gf * alpha) / 255;
  bb = (bb * (255 - alpha) + bf * alpha) / 255;
  *dst = 0xFF000000U | (rb << 16) | (gb << 8) | bb;
}

static uint8_t svg_multiply_alpha(uint8_t lhs, uint8_t rhs) {
  return (uint8_t)(((uint32_t)lhs * (uint32_t)rhs + 127U) / 255U);
}

static uint8_t svg_fill_alpha_mul(const svg_style_t *style) {
  if (!style)
    return 255;
  return svg_multiply_alpha(style->opacity, style->fill_opacity);
}

static uint8_t svg_stroke_alpha_mul(const svg_style_t *style) {
  if (!style)
    return 255;
  return svg_multiply_alpha(style->opacity, style->stroke_opacity);
}

static svg_gradient_t *svg_find_gradient(svg_render_ctx_t *ctx, const char *id) {
  if (!ctx || !id)
    return NULL;
  for (int i = 0; i < ctx->gradient_count; i++) {
    const char *lhs = ctx->gradients[i].id;
    int j = 0;
    while (lhs[j] && id[j] && lhs[j] == id[j])
      j++;
    if (!lhs[j] && !id[j])
      return &ctx->gradients[i];
  }
  return NULL;
}

static void svg_gradient_linear_points(const svg_gradient_t *g, double bbox_x,
                                       double bbox_y, double bbox_w,
                                       double bbox_h, svg_point_t *p1,
                                       svg_point_t *p2) {
  svg_point_t raw_p1;
  svg_point_t raw_p2;
  if (!g || !p1 || !p2)
    return;
  if (g->units == SVG_GRADIENT_UNITS_USERSPACE) {
    raw_p1 = (svg_point_t){g->x1, g->y1};
    raw_p2 = (svg_point_t){g->x2, g->y2};
  } else {
    raw_p1 = (svg_point_t){bbox_x + g->x1 * bbox_w, bbox_y + g->y1 * bbox_h};
    raw_p2 = (svg_point_t){bbox_x + g->x2 * bbox_w, bbox_y + g->y2 * bbox_h};
  }
  *p1 = svg_transform_point(g->transform, raw_p1);
  *p2 = svg_transform_point(g->transform, raw_p2);
}

static void svg_gradient_radial_axes(const svg_gradient_t *g, double bbox_x,
                                     double bbox_y, double bbox_w,
                                     double bbox_h, svg_point_t *center,
                                     svg_point_t *focus,
                                     svg_point_t *rx_point,
                                     svg_point_t *ry_point) {
  svg_point_t raw_center;
  svg_point_t raw_focus;
  svg_point_t raw_rx;
  svg_point_t raw_ry;
  if (!g || !center || !focus || !rx_point || !ry_point)
    return;
  if (g->units == SVG_GRADIENT_UNITS_USERSPACE) {
    raw_center = (svg_point_t){g->cx, g->cy};
    raw_focus = (svg_point_t){g->fx, g->fy};
    raw_rx = (svg_point_t){g->cx + g->r, g->cy};
    raw_ry = (svg_point_t){g->cx, g->cy + g->r};
  } else {
    raw_center = (svg_point_t){bbox_x + g->cx * bbox_w, bbox_y + g->cy * bbox_h};
    raw_focus = (svg_point_t){bbox_x + g->fx * bbox_w, bbox_y + g->fy * bbox_h};
    raw_rx =
        (svg_point_t){bbox_x + (g->cx + g->r) * bbox_w, bbox_y + g->cy * bbox_h};
    raw_ry =
        (svg_point_t){bbox_x + g->cx * bbox_w, bbox_y + (g->cy + g->r) * bbox_h};
  }
  *center = svg_transform_point(g->transform, raw_center);
  *focus = svg_transform_point(g->transform, raw_focus);
  *rx_point = svg_transform_point(g->transform, raw_rx);
  *ry_point = svg_transform_point(g->transform, raw_ry);
}

static void svg_gradient_sync_endpoints(svg_gradient_t *g) {
  if (!g)
    return;
  if (g->stop_count > 0) {
    g->color0 = g->stop_colors[0];
    g->alpha0 = g->stop_alphas[0];
    g->color1 = g->stop_colors[g->stop_count - 1];
    g->alpha1 = g->stop_alphas[g->stop_count - 1];
  }
}

static void svg_gradient_add_stop(svg_gradient_t *g, double offset,
                                  uint32_t color, uint8_t alpha) {
  int insert;
  if (!g)
    return;
  offset = svg_clamp(offset, 0.0, 1.0);
  insert = g->stop_count;
  if (insert >= SVG_GRADIENT_MAX_STOPS)
    insert = SVG_GRADIENT_MAX_STOPS - 1;
  while (insert > 0 && g->stop_offsets[insert - 1] > offset) {
    if (insert < SVG_GRADIENT_MAX_STOPS) {
      g->stop_offsets[insert] = g->stop_offsets[insert - 1];
      g->stop_colors[insert] = g->stop_colors[insert - 1];
      g->stop_alphas[insert] = g->stop_alphas[insert - 1];
    }
    insert--;
  }
  g->stop_offsets[insert] = offset;
  g->stop_colors[insert] = color;
  g->stop_alphas[insert] = alpha;
  if (g->stop_count < SVG_GRADIENT_MAX_STOPS)
    g->stop_count++;
  svg_gradient_sync_endpoints(g);
}

static double svg_gradient_apply_spread(const svg_gradient_t *g, double t) {
  if (!g)
    return svg_clamp(t, 0.0, 1.0);
  if (g->spread_method == SVG_GRADIENT_SPREAD_REPEAT) {
    while (t < 0.0)
      t += 1.0;
    while (t >= 1.0)
      t -= 1.0;
    return t;
  }
  if (g->spread_method == SVG_GRADIENT_SPREAD_REFLECT) {
    while (t < 0.0)
      t += 2.0;
    while (t >= 2.0)
      t -= 2.0;
    if (t > 1.0)
      t = 2.0 - t;
    return t;
  }
  return svg_clamp(t, 0.0, 1.0);
}

static void svg_sample_paint(svg_render_ctx_t *ctx, const svg_paint_t *paint,
                             svg_transform_t transform, double x, double y,
                             double bbox_x, double bbox_y, double bbox_w,
                             double bbox_h,
                             uint32_t *color, uint8_t *alpha) {
  *color = 0;
  *alpha = 0;
  if (!paint || paint->kind == SVG_PAINT_NONE)
    return;
  if (paint->kind == SVG_PAINT_SOLID) {
    *color = paint->color;
    *alpha = paint->alpha;
    return;
  }
  if (paint->kind == SVG_PAINT_URL) {
    svg_gradient_t *g = svg_find_gradient(ctx, paint->ref);
    double t = 0.0;
    if (!g)
      return;
    if (g->kind == SVG_GRADIENT_LINEAR) {
      svg_point_t p1_local;
      svg_point_t p2_local;
      svg_point_t p1;
      svg_point_t p2;
      svg_gradient_linear_points(g, bbox_x, bbox_y, bbox_w, bbox_h, &p1_local,
                                 &p2_local);
      p1 = svg_transform_point(transform, p1_local);
      p2 = svg_transform_point(transform, p2_local);
      double dx = p2.x - p1.x;
      double dy = p2.y - p1.y;
      double denom = dx * dx + dy * dy;
      if (denom > 0.0)
        t = ((x - p1.x) * dx + (y - p1.y) * dy) / denom;
    } else {
      svg_point_t c_local;
      svg_point_t f_local;
      svg_point_t rx_local;
      svg_point_t ry_local;
      svg_point_t c;
      svg_point_t f;
      svg_point_t rx_world;
      svg_point_t ry_world;
      double vx_x, vx_y, vy_x, vy_y;
      double wx, wy;
      double fw_x, fw_y;
      double focal_radius = 0.0;
      double det;
      svg_gradient_radial_axes(g, bbox_x, bbox_y, bbox_w, bbox_h, &c_local,
                               &f_local, &rx_local, &ry_local);
      c = svg_transform_point(transform, c_local);
      f = svg_transform_point(transform, f_local);
      rx_world = svg_transform_point(transform, rx_local);
      ry_world = svg_transform_point(transform, ry_local);
      vx_x = rx_world.x - c.x;
      vx_y = rx_world.y - c.y;
      vy_x = ry_world.x - c.x;
      vy_y = ry_world.y - c.y;
      wx = x - c.x;
      wy = y - c.y;
      fw_x = f.x - c.x;
      fw_y = f.y - c.y;
      det = vx_x * vy_y - vx_y * vy_x;
      if (svg_absd(det) > 1e-9) {
        double u = (wx * vy_y - wy * vy_x) / det;
        double v = (vx_x * wy - vx_y * wx) / det;
        double fu = (fw_x * vy_y - fw_y * vy_x) / det;
        double fv = (vx_x * fw_y - vx_y * fw_x) / det;
        double f_len2 = fu * fu + fv * fv;
        double a;
        double b;
        double c_term;
        double disc;

        if (g->r > 1e-9)
          focal_radius = g->fr / g->r;
        if (focal_radius < 0.0)
          focal_radius = 0.0;
        if (focal_radius > 0.999)
          focal_radius = 0.999;

        if (f_len2 >= 1.0) {
          double scale = 0.999 / svg_sqrt_approx(f_len2);
          fu *= scale;
          fv *= scale;
        }

        a = (u - fu) * (u - fu) + (v - fv) * (v - fv) -
            (1.0 - focal_radius) * (1.0 - focal_radius);
        b = 2.0 * (fu * (u - fu) + fv * (v - fv) +
                   focal_radius * (1.0 - focal_radius));
        c_term = fu * fu + fv * fv - focal_radius * focal_radius;
        if (svg_absd(u - fu) < 1e-9 && svg_absd(v - fv) < 1e-9 &&
            focal_radius <= 1e-9) {
          t = 0.0;
        } else {
          disc = b * b - 4.0 * a * c_term;
          if (a > 1e-12 && disc >= 0.0) {
            double sqrt_disc = svg_sqrt_approx(disc);
            double root0 = (-b - sqrt_disc) / (2.0 * a);
            double root1 = (-b + sqrt_disc) / (2.0 * a);
            double chosen = 0.0;
            if (root0 >= 0.0 && root1 >= 0.0)
              chosen = root0 < root1 ? root0 : root1;
            else if (root0 >= 0.0)
              chosen = root0;
            else if (root1 >= 0.0)
              chosen = root1;
            else
              chosen = root0 > root1 ? root0 : root1;
            if (chosen >= 0.0)
              t = chosen;
            else
              t = svg_sqrt_approx(u * u + v * v);
          } else if (svg_absd(a) <= 1e-12 && svg_absd(b) > 1e-12) {
            t = -c_term / b;
            if (t < 0.0)
              t = svg_sqrt_approx(u * u + v * v);
          } else {
            t = svg_sqrt_approx(u * u + v * v);
          }
        }
      } else {
        double rr = vx_x * vx_x + vx_y * vx_y;
        if (rr > 0.0) {
          if (g->r > 1e-9)
            focal_radius = g->fr / g->r;
          if (focal_radius < 0.0)
            focal_radius = 0.0;
          if (focal_radius > 0.999)
            focal_radius = 0.999;
          if (focal_radius > 0.0) {
            double radial = svg_sqrt_approx((wx * wx + wy * wy) / rr);
            if (1.0 - focal_radius > 1e-9)
              t = (radial - focal_radius) / (1.0 - focal_radius);
            else
              t = radial;
          } else {
            t = svg_sqrt_approx((wx * wx + wy * wy) / rr);
          }
        }
      }
    }
    t = svg_gradient_apply_spread(g, t);
    if (g->stop_count > 0) {
      int upper = 0;
      while (upper < g->stop_count && g->stop_offsets[upper] < t)
        upper++;
      if (upper <= 0) {
        *color = g->stop_colors[0];
        *alpha = g->stop_alphas[0];
      } else if (upper >= g->stop_count) {
        *color = g->stop_colors[g->stop_count - 1];
        *alpha = g->stop_alphas[g->stop_count - 1];
      } else {
        int lower = upper - 1;
        double span = g->stop_offsets[upper] - g->stop_offsets[lower];
        double local_t = span > 1e-9 ? (t - g->stop_offsets[lower]) / span : 1.0;
        uint32_t c0 = g->stop_colors[lower];
        uint32_t c1 = g->stop_colors[upper];
        uint32_t r0 = (c0 >> 16) & 0xFF;
        uint32_t g0 = (c0 >> 8) & 0xFF;
        uint32_t b0 = c0 & 0xFF;
        uint32_t r1 = (c1 >> 16) & 0xFF;
        uint32_t g1 = (c1 >> 8) & 0xFF;
        uint32_t b1 = c1 & 0xFF;
        uint32_t rf =
            (uint32_t)((double)r0 + ((double)r1 - (double)r0) * local_t);
        uint32_t gf =
            (uint32_t)((double)g0 + ((double)g1 - (double)g0) * local_t);
        uint32_t bf =
            (uint32_t)((double)b0 + ((double)b1 - (double)b0) * local_t);
        *color = (rf << 16) | (gf << 8) | bf;
        *alpha = (uint8_t)((double)g->stop_alphas[lower] +
                           ((double)g->stop_alphas[upper] -
                            (double)g->stop_alphas[lower]) *
                               local_t);
      }
    } else {
      uint32_t r0 = (g->color0 >> 16) & 0xFF, g0 = (g->color0 >> 8) & 0xFF,
               b0 = g->color0 & 0xFF;
      uint32_t r1 = (g->color1 >> 16) & 0xFF, g1 = (g->color1 >> 8) & 0xFF,
               b1 = g->color1 & 0xFF;
      uint32_t rf = (uint32_t)((double)r0 + ((double)r1 - (double)r0) * t);
      uint32_t gf = (uint32_t)((double)g0 + ((double)g1 - (double)g0) * t);
      uint32_t bf = (uint32_t)((double)b0 + ((double)b1 - (double)b0) * t);
      *color = (rf << 16) | (gf << 8) | bf;
      *alpha = (uint8_t)((double)g->alpha0 +
                         ((double)g->alpha1 - (double)g->alpha0) * t);
    }
  }
}

static int svg_parse_attr_number_if_present(const uint8_t *data, size_t start,
                                            size_t end, const char *name,
                                            double *out_value) {
  size_t value_start = 0, value_len = 0, pos;
  if (svg_find_attr(data, start, end, name, &value_start, &value_len) != 0)
    return 0;
  pos = value_start;
  if (svg_parse_number(data, value_start + value_len, &pos, out_value) != 0)
    return 0;
  return 1;
}

static int svg_find_closing_tag(const uint8_t *data, size_t size,
                                size_t from_pos, const char *name,
                                size_t *tag_start, size_t *tag_end) {
  size_t name_len = 0;
  while (name[name_len])
    name_len++;
  for (size_t i = from_pos; i + name_len + 3 < size; i++) {
    if (data[i] != '<' || data[i + 1] != '/')
      continue;
    if (!media_bytes_starts_with(data, size, i + 2, name))
      continue;
    if (i + 2 + name_len < size &&
        data[i + 2 + name_len] != '>' &&
        !svg_is_space(data[i + 2 + name_len]))
      continue;
    if (tag_start)
      *tag_start = i;
    i += 2 + name_len;
    while (i < size && data[i] != '>')
      i++;
    if (i >= size)
      return -ENOENT;
    if (tag_end)
      *tag_end = i;
    return 0;
  }
  return -ENOENT;
}

static double svg_measure_text_block_width_state(
    const uint8_t *text, size_t len, double font_size,
    const svg_style_t *style, svg_text_ws_state_t *ws) {
  double cell;
  int visible = 0;
  svg_text_ws_state_t local = {0};

  if (!text || !style || len == 0)
    return 0.0;
  if (font_size <= 0.0)
    font_size = (double)FONT_HEIGHT;
  cell = font_size / (double)FONT_HEIGHT;
  if (cell <= 0.0)
    cell = 1.0;

  for (size_t i = 0; i < len; i++) {
    uint8_t ch = text[i];

    if (style->preserve_space) {
      if (ch == '\r' || ch == '\n' || ch == '\t')
        continue;
      visible++;
      continue;
    }

    if (svg_is_space(ch)) {
      if (ws) {
        if (ws->seen_non_space)
          ws->pending_space = 1;
      } else if (local.seen_non_space) {
        local.pending_space = 1;
      }
      continue;
    }
    if (ws ? ws->pending_space != 0 : local.pending_space != 0) {
      visible++;
      if (ws)
        ws->pending_space = 0;
      else
        local.pending_space = 0;
    }
    visible++;
    if (ws)
      ws->seen_non_space = 1;
    else
      local.seen_non_space = 1;
  }

  return (double)visible * cell * (double)FONT_WIDTH;
}

static void svg_render_text_block(svg_render_ctx_t *ctx, const uint8_t *text,
                                  size_t len, const svg_style_t *style,
                                  svg_transform_t transform, double baseline_x,
                                  double baseline_y, double font_size,
                                  svg_text_ws_state_t *ws) {
  double cell;
  double pen_x;
  double pen_y;
  double bbox_x;
  double bbox_y;
  double bbox_w;
  double bbox_h;
  uint8_t fill_mul;
  uint8_t stroke_mul;

  if (!ctx || !text || !style || len == 0 || !svg_style_is_visible(style))
    return;
  if (font_size <= 0.0)
    font_size = (double)FONT_HEIGHT;

  cell = font_size / (double)FONT_HEIGHT;
  if (cell <= 0.0)
    cell = 1.0;
  pen_x = baseline_x;
  pen_y = baseline_y - font_size;
  bbox_x = baseline_x;
  bbox_y = baseline_y - font_size;
  if (style->preserve_space)
    bbox_w = svg_measure_text_block_width_state(text, len, font_size, style, NULL);
  else {
    svg_text_ws_state_t preview = ws ? *ws : (svg_text_ws_state_t){0};
    bbox_w =
        svg_measure_text_block_width_state(text, len, font_size, style, &preview);
  }
  bbox_h = font_size;
  fill_mul = svg_fill_alpha_mul(style);
  stroke_mul = svg_stroke_alpha_mul(style);

  for (size_t i = 0; i < len; i++) {
    uint8_t ch = text[i];
    const uint8_t *glyph;

    if (style->preserve_space) {
      if (ch == '\r' || ch == '\n' || ch == '\t')
        continue;
    } else {
      if (svg_is_space(ch)) {
        if (ws && ws->seen_non_space)
          ws->pending_space = 1;
        continue;
      }
      if (ws && ws->pending_space) {
        ch = ' ';
        ws->pending_space = 0;
        i--;
      } else if (ws) {
        ws->seen_non_space = 1;
      }
    }
    glyph = font_data[ch];

    if (style->stroke.kind != SVG_PAINT_NONE && style->stroke_width > 0.0 &&
        stroke_mul > 0) {
      double expand = style->stroke_width * 0.5;
      for (int row = 0; row < FONT_HEIGHT; row++) {
        for (int col = 0; col < FONT_WIDTH; col++) {
          if ((glyph[row] & (0x80u >> col)) == 0)
            continue;
          svg_point_t p0 =
              svg_transform_point(transform,
                                  (svg_point_t){pen_x + (double)col * cell - expand,
                                                pen_y + (double)row * cell - expand});
          svg_point_t p1 =
              svg_transform_point(transform,
                                  (svg_point_t){pen_x + (double)(col + 1) * cell + expand,
                                                pen_y + (double)(row + 1) * cell + expand});
          int min_x = (int)svg_min(p0.x, p1.x);
          int max_x = (int)(svg_max(p0.x, p1.x) + 0.999);
          int min_y = (int)svg_min(p0.y, p1.y);
          int max_y = (int)(svg_max(p0.y, p1.y) + 0.999);
          for (int py = min_y; py < max_y; py++) {
            for (int px = min_x; px < max_x; px++) {
              uint32_t color;
              uint8_t alpha;
              svg_sample_paint(ctx, &style->stroke, transform, (double)px + 0.5,
                               (double)py + 0.5, bbox_x, bbox_y, bbox_w, bbox_h,
                               &color, &alpha);
              alpha = svg_multiply_alpha(alpha, stroke_mul);
              svg_blend_pixel(&ctx->image, px, py, color, alpha);
            }
          }
        }
      }
    }

    if (style->fill.kind != SVG_PAINT_NONE && fill_mul > 0) {
      for (int row = 0; row < FONT_HEIGHT; row++) {
        for (int col = 0; col < FONT_WIDTH; col++) {
          if ((glyph[row] & (0x80u >> col)) == 0)
            continue;
          svg_point_t p0 =
              svg_transform_point(transform,
                                  (svg_point_t){pen_x + (double)col * cell,
                                                pen_y + (double)row * cell});
          svg_point_t p1 =
              svg_transform_point(transform,
                                  (svg_point_t){pen_x + (double)(col + 1) * cell,
                                                pen_y + (double)(row + 1) * cell});
          int min_x = (int)svg_min(p0.x, p1.x);
          int max_x = (int)(svg_max(p0.x, p1.x) + 0.999);
          int min_y = (int)svg_min(p0.y, p1.y);
          int max_y = (int)(svg_max(p0.y, p1.y) + 0.999);
          for (int py = min_y; py < max_y; py++) {
            for (int px = min_x; px < max_x; px++) {
              uint32_t color;
              uint8_t alpha;
              svg_sample_paint(ctx, &style->fill, transform, (double)px + 0.5,
                               (double)py + 0.5, bbox_x, bbox_y, bbox_w, bbox_h,
                               &color, &alpha);
              alpha = svg_multiply_alpha(alpha, fill_mul);
              svg_blend_pixel(&ctx->image, px, py, color, alpha);
            }
          }
        }
      }
    }

    pen_x += cell * (double)FONT_WIDTH;
    if (!style->preserve_space && ws && i + 1 < len && !svg_is_space(text[i + 1]))
      ws->seen_non_space = 1;
  }
}

static double svg_measure_text_block_width(const uint8_t *text, size_t len,
                                           double font_size,
                                           const svg_style_t *style,
                                           svg_text_ws_state_t *ws) {
  return svg_measure_text_block_width_state(text, len, font_size, style, ws);
}

static double svg_measure_text_container_width(const uint8_t *data, size_t size,
                                               size_t tag_start, size_t tag_end,
                                               const char *tag_name,
                                               const svg_style_t *base_style,
                                               double initial_x,
                                               double initial_font_size,
                                               svg_text_ws_state_t *ws) {
  size_t close_start = 0, close_end = 0;
  svg_style_t text_style;
  double current_x = initial_x;
  double current_y = 0.0;
  double font_size = initial_font_size;
  size_t pos;

  if (!data || !base_style || !tag_name)
    return 0.0;
  if (svg_find_closing_tag(data, size, tag_end + 1, tag_name, &close_start,
                           &close_end) != 0)
    return 0.0;

  text_style = *base_style;
  svg_apply_style_attrs(data, tag_start, tag_end, &text_style);
  if (!svg_style_is_displayed(&text_style))
    return 0.0;
  if (text_style.has_font_size)
    font_size = text_style.font_size;
  svg_parse_attr_number_if_present(data, tag_start, tag_end, "y", &current_y);

  pos = tag_end + 1;
  while (pos < close_start) {
    if (data[pos] == '<') {
      size_t inner_start = pos;
      size_t inner_end;
      int closing = 0;
      pos++;
      if (pos >= close_start)
        break;
      if (data[pos] == '/') {
        closing = 1;
        pos++;
      }
      while (pos < close_start && data[pos] != '>')
        pos++;
      if (pos >= close_start)
        break;
      inner_end = pos;

      if (!closing &&
          media_bytes_starts_with(data, inner_end, inner_start + 1, "tspan")) {
        size_t tspan_close_start = 0, tspan_close_end = 0;
        svg_style_t span_style = text_style;
        double span_x = current_x;
        double span_y = current_y;
        double dx = 0.0, dy = 0.0;
        double span_font_size = font_size;
        size_t text_start;

        svg_apply_style_attrs(data, inner_start, inner_end, &span_style);
        if (span_style.has_font_size)
          span_font_size = span_style.font_size;
        if (svg_parse_attr_number_if_present(data, inner_start, inner_end, "x",
                                             &span_x) != 0)
          span_x = current_x;
        if (svg_parse_attr_number_if_present(data, inner_start, inner_end, "y",
                                             &span_y) != 0)
          span_y = current_y;
        if (svg_parse_attr_number_if_present(data, inner_start, inner_end, "dx",
                                             &dx))
          span_x += dx;
        if (svg_parse_attr_number_if_present(data, inner_start, inner_end, "dy",
                                             &dy))
          span_y += dy;
        if (svg_find_closing_tag(data, close_start, inner_end + 1, "tspan",
                                 &tspan_close_start, &tspan_close_end) != 0)
          break;
        if (svg_style_is_displayed(&span_style)) {
          text_start = inner_end + 1;
          current_x =
              span_x +
              svg_measure_text_container_width(data, close_start, inner_start,
                                               inner_end, "tspan", &span_style,
                                               span_x, span_font_size, ws);
          current_y = span_y;
          font_size = span_font_size;
        }
        pos = tspan_close_end + 1;
        continue;
      }

      pos = inner_end + 1;
      continue;
    }

    {
      size_t text_start = pos;
      while (pos < close_start && data[pos] != '<')
        pos++;
      if (pos > text_start)
        current_x +=
            svg_measure_text_block_width(data + text_start, pos - text_start,
                                         font_size, &text_style, ws);
    }
  }

  return current_x - initial_x;
}

static double svg_measure_text_element_width(const uint8_t *data, size_t size,
                                             size_t tag_start, size_t tag_end,
                                             const svg_style_t *base_style,
                                             double initial_x,
                                             double initial_font_size) {
  svg_text_ws_state_t ws = {0};
  return svg_measure_text_container_width(data, size, tag_start, tag_end, "text",
                                          base_style, initial_x,
                                          initial_font_size, &ws);
}

static void svg_render_text_container_range(
    svg_render_ctx_t *ctx, const uint8_t *data, size_t close_start,
    size_t tag_start, size_t tag_end, const svg_style_t *container_style,
    svg_transform_t transform, double *current_x, double *current_y,
    double *font_size, svg_text_ws_state_t *ws) {
  size_t pos;

  if (!ctx || !data || !container_style || !current_x || !current_y ||
      !font_size || !ws)
    return;

  pos = tag_end + 1;
  while (pos < close_start) {
    if (data[pos] == '<') {
      size_t inner_start = pos;
      size_t inner_end;
      int closing = 0;
      pos++;
      if (pos >= close_start)
        break;
      if (data[pos] == '/') {
        closing = 1;
        pos++;
      }
      while (pos < close_start && data[pos] != '>')
        pos++;
      if (pos >= close_start)
        break;
      inner_end = pos;

      if (!closing &&
          media_bytes_starts_with(data, inner_end, inner_start + 1, "tspan")) {
        size_t tspan_close_start = 0, tspan_close_end = 0;
        svg_style_t span_style = *container_style;
        double span_x = *current_x;
        double span_y = *current_y;
        double dx = 0.0, dy = 0.0;
        double span_font_size = *font_size;
        svg_text_ws_state_t preview = *ws;
        double advance;

        svg_apply_style_attrs(data, inner_start, inner_end, &span_style);
        if (span_style.has_font_size)
          span_font_size = span_style.font_size;
        if (svg_parse_attr_number_if_present(data, inner_start, inner_end, "x",
                                             &span_x) != 0)
          span_x = *current_x;
        if (svg_parse_attr_number_if_present(data, inner_start, inner_end, "y",
                                             &span_y) != 0)
          span_y = *current_y;
        if (svg_parse_attr_number_if_present(data, inner_start, inner_end, "dx",
                                             &dx))
          span_x += dx;
        if (svg_parse_attr_number_if_present(data, inner_start, inner_end, "dy",
                                             &dy))
          span_y += dy;
        if (svg_parse_attr_number_if_present(data, inner_start, inner_end,
                                             "font-size", &span_font_size)) {
          span_style.font_size = span_font_size;
          span_style.has_font_size = 1;
        }
        if (svg_find_closing_tag(data, close_start, inner_end + 1, "tspan",
                                 &tspan_close_start, &tspan_close_end) != 0)
          break;
        if (svg_style_is_displayed(&span_style)) {
          advance = svg_measure_text_container_width(
              data, close_start, inner_start, inner_end, "tspan", &span_style,
              span_x, span_font_size, &preview);
          svg_render_text_container_range(ctx, data, tspan_close_start,
                                          inner_start, inner_end, &span_style,
                                          transform, &span_x, &span_y,
                                          &span_font_size, ws);
          *current_x = span_x + advance;
          *current_y = span_y;
          *font_size = span_font_size;
        }
        pos = tspan_close_end + 1;
        continue;
      }

      pos = inner_end + 1;
      continue;
    }

    {
      size_t text_start = pos;
      while (pos < close_start && data[pos] != '<')
        pos++;
      if (pos > text_start) {
        double advance;
        svg_text_ws_state_t preview = *ws;

        advance = svg_measure_text_block_width(data + text_start,
                                               pos - text_start, *font_size,
                                               container_style, &preview);
        svg_render_text_block(ctx, data + text_start, pos - text_start,
                              container_style, transform, *current_x,
                              *current_y, *font_size, ws);
        *current_x += advance;
      }
    }
  }
}

static void svg_render_text_element(svg_render_ctx_t *ctx, const uint8_t *data,
                                    size_t size, size_t tag_start,
                                    size_t tag_end, size_t *consumed_end,
                                    const svg_style_t *base_style,
                                    svg_transform_t transform) {
  size_t close_start = 0, close_end = 0;
  svg_style_t text_style;
  double current_x = 0.0, current_y = 0.0;
  double font_size = 16.0;
  size_t pos;
  svg_text_ws_state_t ws = {0};

  if (!ctx || !data || !consumed_end || !base_style)
    return;
  if (svg_find_closing_tag(data, size, tag_end + 1, "text", &close_start,
                           &close_end) != 0)
    return;

  text_style = *base_style;
  svg_apply_style_attrs(data, tag_start, tag_end, &text_style);
  if (!svg_style_is_displayed(&text_style)) {
    *consumed_end = close_end;
    return;
  }
  if (text_style.has_font_size)
    font_size = text_style.font_size;
  svg_parse_attr_number_if_present(data, tag_start, tag_end, "x", &current_x);
  svg_parse_attr_number_if_present(data, tag_start, tag_end, "y", &current_y);
  if (svg_parse_attr_number_if_present(data, tag_start, tag_end, "font-size",
                                       &font_size)) {
    text_style.font_size = font_size;
    text_style.has_font_size = 1;
  }
  if (text_style.text_anchor != SVG_TEXT_ANCHOR_START) {
    double width = svg_measure_text_element_width(data, size, tag_start, tag_end,
                                                  &text_style, current_x,
                                                  font_size);
    if (text_style.text_anchor == SVG_TEXT_ANCHOR_MIDDLE)
      current_x -= width * 0.5;
    else if (text_style.text_anchor == SVG_TEXT_ANCHOR_END)
      current_x -= width;
  }

  pos = tag_end + 1;
  (void)pos;
  svg_render_text_container_range(ctx, data, close_start, tag_start, tag_end,
                                  &text_style, transform, &current_x,
                                  &current_y, &font_size, &ws);

  *consumed_end = close_end;
}

static int svg_path_buffer_append(svg_path_buffer_t *buf, svg_point_t point,
                                  int move, int close_here) {
  svg_point_t *new_points;
  uint8_t *new_moves;
  uint8_t *new_closes;
  int new_cap;
  if (buf->count >= buf->cap) {
    new_cap = buf->cap ? buf->cap * 2 : 128;
    new_points = (svg_point_t *)kmalloc(sizeof(svg_point_t) * (size_t)new_cap, GFP_KERNEL);
    new_moves = (uint8_t *)kmalloc((size_t)new_cap, GFP_KERNEL);
    new_closes = (uint8_t *)kmalloc((size_t)new_cap, GFP_KERNEL);
    if (!new_points || !new_moves || !new_closes) {
      if (new_points)
        kfree(new_points);
      if (new_moves)
        kfree(new_moves);
      if (new_closes)
        kfree(new_closes);
      return -ENOMEM;
    }
    for (int i = 0; i < buf->count; i++) {
      new_points[i] = buf->points[i];
      new_moves[i] = buf->moves[i];
      new_closes[i] = buf->closes[i];
    }
    if (buf->points)
      kfree(buf->points);
    if (buf->moves)
      kfree(buf->moves);
    if (buf->closes)
      kfree(buf->closes);
    buf->points = new_points;
    buf->moves = new_moves;
    buf->closes = new_closes;
    buf->cap = new_cap;
  }
  buf->points[buf->count] = point;
  buf->moves[buf->count] = (uint8_t)(move ? 1 : 0);
  buf->closes[buf->count] = (uint8_t)(close_here ? 1 : 0);
  buf->count++;
  return 0;
}

static void svg_path_buffer_free(svg_path_buffer_t *buf) {
  if (!buf)
    return;
  if (buf->points)
    kfree(buf->points);
  if (buf->moves)
    kfree(buf->moves);
  if (buf->closes)
    kfree(buf->closes);
  buf->points = NULL;
  buf->moves = NULL;
  buf->closes = NULL;
  buf->count = 0;
  buf->cap = 0;
}

static void svg_append_cubic(svg_path_buffer_t *buf, svg_point_t p0,
                             svg_point_t p1, svg_point_t p2, svg_point_t p3) {
  for (int i = 1; i <= 16; i++) {
    double t = (double)i / 16.0;
    double mt = 1.0 - t;
    svg_point_t p;
    p.x = mt * mt * mt * p0.x + 3.0 * mt * mt * t * p1.x +
          3.0 * mt * t * t * p2.x + t * t * t * p3.x;
    p.y = mt * mt * mt * p0.y + 3.0 * mt * mt * t * p1.y +
          3.0 * mt * t * t * p2.y + t * t * t * p3.y;
    svg_path_buffer_append(buf, p, 0, 0);
  }
}

static void svg_append_quadratic(svg_path_buffer_t *buf, svg_point_t p0,
                                 svg_point_t p1, svg_point_t p2) {
  for (int i = 1; i <= 12; i++) {
    double t = (double)i / 12.0;
    double mt = 1.0 - t;
    svg_point_t p;
    p.x = mt * mt * p0.x + 2.0 * mt * t * p1.x + t * t * p2.x;
    p.y = mt * mt * p0.y + 2.0 * mt * t * p1.y + t * t * p2.y;
    svg_path_buffer_append(buf, p, 0, 0);
  }
}

static svg_point_t svg_map_arc_point(double cx, double cy, double rx, double ry,
                                     double cos_phi, double sin_phi, double ux,
                                     double uy) {
  svg_point_t p;
  p.x = cx + cos_phi * rx * ux - sin_phi * ry * uy;
  p.y = cy + sin_phi * rx * ux + cos_phi * ry * uy;
  return p;
}

static void svg_append_arc(svg_path_buffer_t *buf, svg_point_t current,
                           double rx, double ry, double x_axis_rotation,
                           int large_arc_flag, int sweep_flag,
                           svg_point_t end_point) {
  double phi;
  double cos_phi;
  double sin_phi;
  double dx2;
  double dy2;
  double x1p;
  double y1p;
  double lambda;
  double rx_sq;
  double ry_sq;
  double x1p_sq;
  double y1p_sq;
  double numerator;
  double denominator;
  double factor;
  double cxp;
  double cyp;
  double cx;
  double cy;
  double ux;
  double uy;
  double vx;
  double vy;
  double theta1;
  double delta_theta;
  double abs_delta;
  int segments;

  rx = svg_absd(rx);
  ry = svg_absd(ry);
  if (rx <= 0.0 || ry <= 0.0) {
    svg_path_buffer_append(buf, end_point, 0, 0);
    return;
  }

  phi = x_axis_rotation * 3.14159265358979323846 / 180.0;
  cos_phi = svg_cos_approx(phi);
  sin_phi = svg_sin_approx(phi);
  dx2 = (current.x - end_point.x) * 0.5;
  dy2 = (current.y - end_point.y) * 0.5;
  x1p = cos_phi * dx2 + sin_phi * dy2;
  y1p = -sin_phi * dx2 + cos_phi * dy2;
  if (svg_absd(x1p) < 1e-9 && svg_absd(y1p) < 1e-9)
    return;

  lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
  if (lambda > 1.0) {
    double scale = svg_sqrt_approx(lambda);
    rx *= scale;
    ry *= scale;
  }

  rx_sq = rx * rx;
  ry_sq = ry * ry;
  x1p_sq = x1p * x1p;
  y1p_sq = y1p * y1p;
  numerator = rx_sq * ry_sq - rx_sq * y1p_sq - ry_sq * x1p_sq;
  denominator = rx_sq * y1p_sq + ry_sq * x1p_sq;
  if (denominator <= 0.0) {
    svg_path_buffer_append(buf, end_point, 0, 0);
    return;
  }
  if (numerator < 0.0)
    numerator = 0.0;

  factor = svg_sqrt_approx(numerator / denominator);
  if (large_arc_flag == sweep_flag)
    factor = -factor;

  cxp = factor * ((rx * y1p) / ry);
  cyp = factor * (-(ry * x1p) / rx);
  cx = cos_phi * cxp - sin_phi * cyp + (current.x + end_point.x) * 0.5;
  cy = sin_phi * cxp + cos_phi * cyp + (current.y + end_point.y) * 0.5;

  ux = (x1p - cxp) / rx;
  uy = (y1p - cyp) / ry;
  vx = (-x1p - cxp) / rx;
  vy = (-y1p - cyp) / ry;
  theta1 = svg_atan2_approx(uy, ux);
  delta_theta = svg_atan2_approx(ux * vy - uy * vx, ux * vx + uy * vy);
  if (!sweep_flag && delta_theta > 0.0)
    delta_theta -= 6.28318530717958647692;
  else if (sweep_flag && delta_theta < 0.0)
    delta_theta += 6.28318530717958647692;

  abs_delta = svg_absd(delta_theta);
  segments = (int)(abs_delta / (3.14159265358979323846 / 8.0));
  if ((double)segments * (3.14159265358979323846 / 8.0) < abs_delta)
    segments++;
  if (segments < 1)
    segments = 1;

  for (int seg = 0; seg < segments; seg++) {
    double t1 = theta1 + delta_theta * ((double)seg / (double)segments);
    double t2 = theta1 + delta_theta * ((double)(seg + 1) / (double)segments);
    double dt = t2 - t1;
    double alpha;
    double cos_t1 = svg_cos_approx(t1);
    double sin_t1 = svg_sin_approx(t1);
    double cos_t2 = svg_cos_approx(t2);
    double sin_t2 = svg_sin_approx(t2);
    double tan_term = svg_sin_approx(dt * 0.25) / svg_cos_approx(dt * 0.25);
    svg_point_t p1;
    svg_point_t p2;
    svg_point_t p3;

    alpha = (4.0 / 3.0) * tan_term;
    p1 = svg_map_arc_point(cx, cy, rx, ry, cos_phi, sin_phi,
                           cos_t1 - alpha * sin_t1,
                           sin_t1 + alpha * cos_t1);
    p2 = svg_map_arc_point(cx, cy, rx, ry, cos_phi, sin_phi,
                           cos_t2 + alpha * sin_t2,
                           sin_t2 - alpha * cos_t2);
    p3 = svg_map_arc_point(cx, cy, rx, ry, cos_phi, sin_phi, cos_t2, sin_t2);
    svg_append_cubic(buf, current, p1, p2, p3);
    current = p3;
  }
}

static void svg_append_rounded_rect(svg_path_buffer_t *buf, double x, double y,
                                    double w, double h, double rx,
                                    double ry) {
  const double kappa = 0.5522847498307936;
  double cx = rx * kappa;
  double cy = ry * kappa;

  svg_path_buffer_append(buf, (svg_point_t){x + rx, y}, 1, 0);
  svg_path_buffer_append(buf, (svg_point_t){x + w - rx, y}, 0, 0);
  svg_append_cubic(buf, (svg_point_t){x + w - rx, y},
                   (svg_point_t){x + w - rx + cx, y},
                   (svg_point_t){x + w, y + ry - cy},
                   (svg_point_t){x + w, y + ry});
  svg_path_buffer_append(buf, (svg_point_t){x + w, y + h - ry}, 0, 0);
  svg_append_cubic(buf, (svg_point_t){x + w, y + h - ry},
                   (svg_point_t){x + w, y + h - ry + cy},
                   (svg_point_t){x + w - rx + cx, y + h},
                   (svg_point_t){x + w - rx, y + h});
  svg_path_buffer_append(buf, (svg_point_t){x + rx, y + h}, 0, 0);
  svg_append_cubic(buf, (svg_point_t){x + rx, y + h},
                   (svg_point_t){x + rx - cx, y + h},
                   (svg_point_t){x, y + h - ry + cy},
                   (svg_point_t){x, y + h - ry});
  svg_path_buffer_append(buf, (svg_point_t){x, y + ry}, 0, 0);
  svg_append_cubic(buf, (svg_point_t){x, y + ry},
                   (svg_point_t){x, y + ry - cy},
                   (svg_point_t){x + rx - cx, y},
                   (svg_point_t){x + rx, y});
  if (buf->count > 0)
    buf->closes[buf->count - 1] = 1;
}

static void svg_append_ellipse_path(svg_path_buffer_t *buf, double cx, double cy,
                                    double rx, double ry) {
  const double kappa = 0.5522847498307936;
  double cpx = rx * kappa;
  double cpy = ry * kappa;

  svg_path_buffer_append(buf, (svg_point_t){cx + rx, cy}, 1, 0);
  svg_append_cubic(buf, (svg_point_t){cx + rx, cy},
                   (svg_point_t){cx + rx, cy + cpy},
                   (svg_point_t){cx + cpx, cy + ry},
                   (svg_point_t){cx, cy + ry});
  svg_append_cubic(buf, (svg_point_t){cx, cy + ry},
                   (svg_point_t){cx - cpx, cy + ry},
                   (svg_point_t){cx - rx, cy + cpy},
                   (svg_point_t){cx - rx, cy});
  svg_append_cubic(buf, (svg_point_t){cx - rx, cy},
                   (svg_point_t){cx - rx, cy - cpy},
                   (svg_point_t){cx - cpx, cy - ry},
                   (svg_point_t){cx, cy - ry});
  svg_append_cubic(buf, (svg_point_t){cx, cy - ry},
                   (svg_point_t){cx + cpx, cy - ry},
                   (svg_point_t){cx + rx, cy - cpy},
                   (svg_point_t){cx + rx, cy});
  if (buf->count > 0)
    buf->closes[buf->count - 1] = 1;
}

static int svg_parse_path_data(const uint8_t *data, size_t len,
                               svg_path_buffer_t *buf) {
  size_t pos = 0;
  char cmd = 0;
  svg_point_t current = {0.0, 0.0};
  svg_point_t start = {0.0, 0.0};
  svg_point_t last_cubic = {0.0, 0.0};
  svg_point_t last_quad = {0.0, 0.0};
  int have_cubic = 0;
  int have_quad = 0;

  while (pos < len) {
    while (pos < len && svg_is_space(data[pos]))
      pos++;
    if (pos >= len)
      break;
    if ((data[pos] >= 'A' && data[pos] <= 'Z') || (data[pos] >= 'a' && data[pos] <= 'z')) {
      cmd = (char)data[pos++];
      if (cmd == 'Z' || cmd == 'z') {
        if (buf->count > 0)
          buf->closes[buf->count - 1] = 1;
        current = start;
        have_cubic = have_quad = 0;
      }
      continue;
    }
    if (!cmd)
      break;
    switch (cmd) {
    case 'M':
    case 'm': {
      double x = 0.0, y = 0.0;
      if (svg_parse_number(data, len, &pos, &x) != 0 ||
          svg_parse_number(data, len, &pos, &y) != 0)
        return -EINVAL;
      if (cmd == 'm') {
        x += current.x;
        y += current.y;
      }
      current.x = x;
      current.y = y;
      start = current;
      svg_path_buffer_append(buf, current, 1, 0);
      cmd = (cmd == 'm') ? 'l' : 'L';
      have_cubic = have_quad = 0;
      break;
    }
    case 'L':
    case 'l': {
      double x = 0.0, y = 0.0;
      if (svg_parse_number(data, len, &pos, &x) != 0 ||
          svg_parse_number(data, len, &pos, &y) != 0)
        return -EINVAL;
      if (cmd == 'l') {
        x += current.x;
        y += current.y;
      }
      current.x = x;
      current.y = y;
      svg_path_buffer_append(buf, current, 0, 0);
      have_cubic = have_quad = 0;
      break;
    }
    case 'H':
    case 'h': {
      double x = 0.0;
      if (svg_parse_number(data, len, &pos, &x) != 0)
        return -EINVAL;
      current.x = (cmd == 'h') ? current.x + x : x;
      svg_path_buffer_append(buf, current, 0, 0);
      have_cubic = have_quad = 0;
      break;
    }
    case 'V':
    case 'v': {
      double y = 0.0;
      if (svg_parse_number(data, len, &pos, &y) != 0)
        return -EINVAL;
      current.y = (cmd == 'v') ? current.y + y : y;
      svg_path_buffer_append(buf, current, 0, 0);
      have_cubic = have_quad = 0;
      break;
    }
    case 'C':
    case 'c': {
      double v[6];
      svg_point_t p1, p2, p3;
      for (int i = 0; i < 6; i++) {
        if (svg_parse_number(data, len, &pos, &v[i]) != 0)
          return -EINVAL;
      }
      p1.x = v[0]; p1.y = v[1];
      p2.x = v[2]; p2.y = v[3];
      p3.x = v[4]; p3.y = v[5];
      if (cmd == 'c') {
        p1.x += current.x; p1.y += current.y;
        p2.x += current.x; p2.y += current.y;
        p3.x += current.x; p3.y += current.y;
      }
      svg_append_cubic(buf, current, p1, p2, p3);
      current = p3;
      last_cubic = p2;
      have_cubic = 1;
      have_quad = 0;
      break;
    }
    case 'S':
    case 's': {
      double v[4];
      svg_point_t p1, p2, p3;
      p1 = have_cubic ? (svg_point_t){2.0 * current.x - last_cubic.x,
                                      2.0 * current.y - last_cubic.y}
                      : current;
      for (int i = 0; i < 4; i++) {
        if (svg_parse_number(data, len, &pos, &v[i]) != 0)
          return -EINVAL;
      }
      p2.x = v[0]; p2.y = v[1];
      p3.x = v[2]; p3.y = v[3];
      if (cmd == 's') {
        p2.x += current.x; p2.y += current.y;
        p3.x += current.x; p3.y += current.y;
      }
      svg_append_cubic(buf, current, p1, p2, p3);
      current = p3;
      last_cubic = p2;
      have_cubic = 1;
      have_quad = 0;
      break;
    }
    case 'Q':
    case 'q': {
      double v[4];
      svg_point_t p1, p2;
      for (int i = 0; i < 4; i++) {
        if (svg_parse_number(data, len, &pos, &v[i]) != 0)
          return -EINVAL;
      }
      p1.x = v[0]; p1.y = v[1];
      p2.x = v[2]; p2.y = v[3];
      if (cmd == 'q') {
        p1.x += current.x; p1.y += current.y;
        p2.x += current.x; p2.y += current.y;
      }
      svg_append_quadratic(buf, current, p1, p2);
      current = p2;
      last_quad = p1;
      have_quad = 1;
      have_cubic = 0;
      break;
    }
    case 'T':
    case 't': {
      double v[2];
      svg_point_t p1, p2;
      p1 = have_quad ? (svg_point_t){2.0 * current.x - last_quad.x,
                                     2.0 * current.y - last_quad.y}
                     : current;
      for (int i = 0; i < 2; i++) {
        if (svg_parse_number(data, len, &pos, &v[i]) != 0)
          return -EINVAL;
      }
      p2.x = v[0]; p2.y = v[1];
      if (cmd == 't') {
        p2.x += current.x; p2.y += current.y;
      }
      svg_append_quadratic(buf, current, p1, p2);
      current = p2;
      last_quad = p1;
      have_quad = 1;
      have_cubic = 0;
      break;
    }
    case 'A':
    case 'a': {
      double v[7];
      svg_point_t end_point;
      int large_arc_flag;
      int sweep_flag;
      for (int i = 0; i < 7; i++) {
        if (svg_parse_number(data, len, &pos, &v[i]) != 0)
          return -EINVAL;
      }
      end_point.x = v[5];
      end_point.y = v[6];
      if (cmd == 'a') {
        end_point.x += current.x;
        end_point.y += current.y;
      }
      large_arc_flag = v[3] != 0.0 ? 1 : 0;
      sweep_flag = v[4] != 0.0 ? 1 : 0;
      svg_append_arc(buf, current, v[0], v[1], v[2], large_arc_flag,
                     sweep_flag, end_point);
      current = end_point;
      have_cubic = 0;
      have_quad = 0;
      break;
    }
    default:
      pos++;
      break;
    }
  }
  return buf->count > 0 ? 0 : -ENOENT;
}

static void svg_draw_disc(svg_render_ctx_t *ctx, double cx, double cy, double radius,
                          const svg_paint_t *paint, uint8_t alpha_mul,
                          svg_transform_t transform, double bbox_x,
                          double bbox_y, double bbox_w, double bbox_h) {
  int min_x = (int)(cx - radius - 1.0);
  int max_x = (int)(cx + radius + 1.0);
  int min_y = (int)(cy - radius - 1.0);
  int max_y = (int)(cy + radius + 1.0);
  double rr = radius * radius;
  for (int y = min_y; y <= max_y; y++) {
    for (int x = min_x; x <= max_x; x++) {
      double dx = ((double)x + 0.5) - cx;
      double dy = ((double)y + 0.5) - cy;
      if (dx * dx + dy * dy <= rr) {
        uint32_t color;
        uint8_t alpha;
        svg_sample_paint(ctx, paint, transform, (double)x + 0.5, (double)y + 0.5,
                         bbox_x, bbox_y, bbox_w, bbox_h, &color, &alpha);
        alpha = svg_multiply_alpha(alpha, alpha_mul);
        svg_blend_pixel(&ctx->image, x, y, color, alpha);
      }
    }
  }
}

static void svg_fill_polygon(svg_render_ctx_t *ctx, const svg_point_t *points,
                             int count, const svg_paint_t *paint,
                             uint8_t alpha_mul, svg_transform_t transform,
                             double bbox_x, double bbox_y, double bbox_w,
                             double bbox_h) {
  double min_x = 1e9, min_y = 1e9, max_x = -1e9, max_y = -1e9;
  if (!ctx || !points || count < 3 || !paint)
    return;
  for (int i = 0; i < count; i++) {
    min_x = svg_min(min_x, points[i].x);
    min_y = svg_min(min_y, points[i].y);
    max_x = svg_max(max_x, points[i].x);
    max_y = svg_max(max_y, points[i].y);
  }
  for (int y = (int)(min_y - 1.0); y <= (int)(max_y + 1.0); y++) {
    for (int x = (int)(min_x - 1.0); x <= (int)(max_x + 1.0); x++) {
      double px = (double)x + 0.5;
      double py = (double)y + 0.5;
      int inside = 0;
      for (int i = 0, j = count - 1; i < count; j = i++) {
        double yi = points[i].y;
        double yj = points[j].y;
        if (((yi > py) != (yj > py)) &&
            (px < (points[j].x - points[i].x) * (py - yi) / (yj - yi) +
                      points[i].x))
          inside = !inside;
      }
      if (inside) {
        uint32_t color;
        uint8_t alpha;
        svg_sample_paint(ctx, paint, transform, px, py, bbox_x, bbox_y, bbox_w,
                         bbox_h, &color, &alpha);
        alpha = svg_multiply_alpha(alpha, alpha_mul);
        svg_blend_pixel(&ctx->image, x, y, color, alpha);
      }
    }
  }
}

static int svg_compute_segment_quad(svg_point_t a, svg_point_t b, double radius,
                                    svg_point_t quad[4]) {
  double dx = b.x - a.x;
  double dy = b.y - a.y;
  double len = svg_sqrt_approx(dx * dx + dy * dy);
  double nx;
  double ny;
  if (len <= 1e-9)
    return 0;
  nx = -dy / len * radius;
  ny = dx / len * radius;
  quad[0] = (svg_point_t){a.x + nx, a.y + ny};
  quad[1] = (svg_point_t){b.x + nx, b.y + ny};
  quad[2] = (svg_point_t){b.x - nx, b.y - ny};
  quad[3] = (svg_point_t){a.x - nx, a.y - ny};
  return 1;
}

static int svg_expand_segment_for_caps(svg_point_t *a, svg_point_t *b, double radius,
                                       int linecap, int cap_start, int cap_end) {
  double dx;
  double dy;
  double len;
  double ux;
  double uy;

  if (!a || !b)
    return 0;
  dx = b->x - a->x;
  dy = b->y - a->y;
  len = svg_sqrt_approx(dx * dx + dy * dy);
  if (len <= 1e-9)
    return 0;
  if (linecap != SVG_STROKE_CAP_SQUARE)
    return 1;

  ux = dx / len;
  uy = dy / len;
  if (cap_start) {
    a->x -= ux * radius;
    a->y -= uy * radius;
  }
  if (cap_end) {
    b->x += ux * radius;
    b->y += uy * radius;
  }
  return 1;
}

static int svg_dash_pattern_count(const svg_style_t *style) {
  if (!style || style->stroke_dash_count <= 0)
    return 0;
  if ((style->stroke_dash_count & 1) != 0)
    return style->stroke_dash_count * 2;
  return style->stroke_dash_count;
}

static double svg_dash_pattern_value(const svg_style_t *style, int index) {
  int base_count;

  if (!style || style->stroke_dash_count <= 0)
    return 0.0;
  base_count = style->stroke_dash_count;
  while (index < 0)
    index += svg_dash_pattern_count(style);
  return style->stroke_dasharray[index % base_count];
}

static double svg_dash_cycle_length(const svg_style_t *style) {
  int count = svg_dash_pattern_count(style);
  double total = 0.0;

  for (int i = 0; i < count; i++)
    total += svg_dash_pattern_value(style, i);
  return total;
}

static void svg_dash_init(const svg_style_t *style, int *index,
                          double *remaining, int *draw) {
  int count = svg_dash_pattern_count(style);
  double cycle = svg_dash_cycle_length(style);
  double offset;

  if (!index || !remaining || !draw) {
    return;
  }
  *index = 0;
  *remaining = 0.0;
  *draw = 1;
  if (!style || count <= 0 || cycle <= 1e-9)
    return;

  offset = style->stroke_dashoffset;
  while (offset < 0.0)
    offset += cycle;
  while (offset >= cycle)
    offset -= cycle;

  for (int i = 0; i < count; i++) {
    double part = svg_dash_pattern_value(style, i);
    if (part <= 1e-9)
      continue;
    if (offset < part) {
      *index = i;
      *remaining = part - offset;
      *draw = ((i & 1) == 0);
      return;
    }
    offset -= part;
  }

  *index = 0;
  *remaining = svg_dash_pattern_value(style, 0);
  *draw = 1;
}

static void svg_dash_advance(const svg_style_t *style, int *index,
                             double *remaining, int *draw) {
  int count = svg_dash_pattern_count(style);

  if (!style || !index || !remaining || !draw || count <= 0)
    return;
  do {
    *index = (*index + 1) % count;
    *remaining = svg_dash_pattern_value(style, *index);
    *draw = ((*index & 1) == 0);
  } while (*remaining <= 1e-9);
}

static void svg_stroke_segment_solid(svg_render_ctx_t *ctx, svg_point_t a,
                                     svg_point_t b, const svg_style_t *style,
                                     svg_transform_t transform,
                                     int round_cap_start, int round_cap_end,
                                     double bbox_x, double bbox_y,
                                     double bbox_w, double bbox_h) {
  svg_point_t quad[4];
  double radius = style->stroke_width * 0.5;
  uint8_t alpha_mul = svg_stroke_alpha_mul(style);
  svg_point_t body_a = a;
  svg_point_t body_b = b;
  if (!svg_expand_segment_for_caps(&body_a, &body_b, radius, style->stroke_linecap,
                                   round_cap_start, round_cap_end))
    return;
  if (!svg_compute_segment_quad(body_a, body_b, radius, quad))
    return;
  svg_fill_polygon(ctx, quad, 4, &style->stroke, alpha_mul, transform, bbox_x,
                   bbox_y, bbox_w, bbox_h);
  if (style->stroke_linecap == SVG_STROKE_CAP_ROUND && round_cap_start)
    svg_draw_disc(ctx, a.x, a.y, radius, &style->stroke, alpha_mul, transform,
                  bbox_x, bbox_y, bbox_w, bbox_h);
  if (style->stroke_linecap == SVG_STROKE_CAP_ROUND && round_cap_end)
    svg_draw_disc(ctx, b.x, b.y, radius, &style->stroke, alpha_mul, transform,
                  bbox_x, bbox_y, bbox_w, bbox_h);
}

static void svg_stroke_segment(svg_render_ctx_t *ctx, svg_point_t a, svg_point_t b,
                               const svg_style_t *style, svg_transform_t transform,
                               int round_cap_start, int round_cap_end,
                               double bbox_x, double bbox_y, double bbox_w,
                               double bbox_h) {
  svg_stroke_segment_solid(ctx, a, b, style, transform, round_cap_start,
                           round_cap_end, bbox_x, bbox_y, bbox_w, bbox_h);
}

static void svg_stroke_segment_dashed(svg_render_ctx_t *ctx, svg_point_t a,
                                      svg_point_t b, const svg_style_t *style,
                                      svg_transform_t transform,
                                      double bbox_x, double bbox_y,
                                      double bbox_w, double bbox_h,
                                      int *dash_index,
                                      double *dash_remaining, int *dash_draw) {
  double dx;
  double dy;
  double len;
  double ux;
  double uy;
  double pos = 0.0;

  if (!ctx || !style || !dash_index || !dash_remaining || !dash_draw)
    return;
  dx = b.x - a.x;
  dy = b.y - a.y;
  len = svg_sqrt_approx(dx * dx + dy * dy);
  if (len <= 1e-9)
    return;
  ux = dx / len;
  uy = dy / len;

  while (pos < len - 1e-9) {
    double step = *dash_remaining;
    if (step <= 1e-9) {
      svg_dash_advance(style, dash_index, dash_remaining, dash_draw);
      step = *dash_remaining;
      if (step <= 1e-9)
        break;
    }
    if (step > len - pos)
      step = len - pos;
    if (*dash_draw && step > 1e-9) {
      svg_point_t da = {a.x + ux * pos, a.y + uy * pos};
      svg_point_t db = {a.x + ux * (pos + step), a.y + uy * (pos + step)};
      svg_stroke_segment_solid(ctx, da, db, style, transform, 1, 1, bbox_x,
                               bbox_y, bbox_w, bbox_h);
    }
    pos += step;
    *dash_remaining -= step;
    if (*dash_remaining <= 1e-9)
      svg_dash_advance(style, dash_index, dash_remaining, dash_draw);
  }
}

static void svg_stroke_join(svg_render_ctx_t *ctx, svg_point_t prev, svg_point_t curr,
                            svg_point_t next, const svg_style_t *style,
                            svg_transform_t transform, double bbox_x,
                            double bbox_y, double bbox_w, double bbox_h) {
  double radius = style->stroke_width * 0.5;
  double d0x = curr.x - prev.x;
  double d0y = curr.y - prev.y;
  double d1x = next.x - curr.x;
  double d1y = next.y - curr.y;
  double len0 = svg_sqrt_approx(d0x * d0x + d0y * d0y);
  double len1 = svg_sqrt_approx(d1x * d1x + d1y * d1y);
  double cross;
  double side;
  double n0x, n0y, n1x, n1y;
  svg_point_t p0, p1;
  uint8_t alpha_mul = svg_stroke_alpha_mul(style);
  if (len0 <= 1e-9 || len1 <= 1e-9)
    return;
  d0x /= len0; d0y /= len0;
  d1x /= len1; d1y /= len1;
  cross = d0x * d1y - d0y * d1x;
  if (svg_absd(cross) <= 1e-9)
    return;
  if (style->stroke_linejoin == SVG_STROKE_JOIN_ROUND) {
    svg_draw_disc(ctx, curr.x, curr.y, radius, &style->stroke, alpha_mul,
                  transform, bbox_x, bbox_y, bbox_w, bbox_h);
    return;
  }
  side = cross > 0.0 ? 1.0 : -1.0;
  n0x = -d0y * radius * side;
  n0y = d0x * radius * side;
  n1x = -d1y * radius * side;
  n1y = d1x * radius * side;
  p0 = (svg_point_t){curr.x + n0x, curr.y + n0y};
  p1 = (svg_point_t){curr.x + n1x, curr.y + n1y};
  if (style->stroke_linejoin == SVG_STROKE_JOIN_BEVEL) {
    svg_point_t tri[3] = {curr, p0, p1};
    svg_fill_polygon(ctx, tri, 3, &style->stroke, alpha_mul, transform, bbox_x,
                     bbox_y, bbox_w, bbox_h);
    return;
  }
  {
    double denom = d0x * d1y - d0y * d1x;
    double t;
    svg_point_t miter;
    double miter_len;
    if (svg_absd(denom) <= 1e-9) {
      svg_point_t tri[3] = {curr, p0, p1};
      svg_fill_polygon(ctx, tri, 3, &style->stroke, alpha_mul, transform, bbox_x,
                       bbox_y, bbox_w, bbox_h);
      return;
    }
    t = ((p1.x - p0.x) * d1y - (p1.y - p0.y) * d1x) / denom;
    miter = (svg_point_t){p0.x + d0x * t, p0.y + d0y * t};
    miter_len = svg_sqrt_approx((miter.x - curr.x) * (miter.x - curr.x) +
                                (miter.y - curr.y) * (miter.y - curr.y));
    if (miter_len > style->stroke_miterlimit * radius) {
      svg_point_t tri[3] = {curr, p0, p1};
      svg_fill_polygon(ctx, tri, 3, &style->stroke, alpha_mul, transform, bbox_x,
                       bbox_y, bbox_w, bbox_h);
    } else {
      svg_point_t tri[3] = {p0, miter, p1};
      svg_fill_polygon(ctx, tri, 3, &style->stroke, alpha_mul, transform, bbox_x,
                       bbox_y, bbox_w, bbox_h);
    }
  }
}

static void svg_fill_path(svg_render_ctx_t *ctx, const svg_path_buffer_t *buf,
                          const svg_style_t *style, svg_transform_t transform,
                          double bbox_x, double bbox_y, double bbox_w,
                          double bbox_h) {
  double min_x = 1e9, min_y = 1e9, max_x = -1e9, max_y = -1e9;
  for (int i = 0; i < buf->count; i++) {
    min_x = svg_min(min_x, buf->points[i].x);
    min_y = svg_min(min_y, buf->points[i].y);
    max_x = svg_max(max_x, buf->points[i].x);
    max_y = svg_max(max_y, buf->points[i].y);
  }
  for (int y = (int)(min_y - 1.0); y <= (int)(max_y + 1.0); y++) {
    for (int x = (int)(min_x - 1.0); x <= (int)(max_x + 1.0); x++) {
      double px = (double)x + 0.5;
      double py = (double)y + 0.5;
      int winding = 0;
      int parity = 0;
      int subpath_start = -1;
      for (int i = 0; i < buf->count; i++) {
        int next = i + 1;
        if (buf->moves[i])
          subpath_start = i;
        if (next >= buf->count || buf->moves[next]) {
          if (buf->closes[i] && subpath_start >= 0)
            next = subpath_start;
          else
            continue;
        }
        {
          svg_point_t a = buf->points[i];
          svg_point_t b = buf->points[next];
          if (((a.y <= py) && (b.y > py)) || ((a.y > py) && (b.y <= py))) {
            double isect = a.x + (py - a.y) * (b.x - a.x) / (b.y - a.y);
            if (isect > px) {
              parity ^= 1;
              if (style->fill_rule_nonzero)
                winding += (b.y > a.y) ? 1 : -1;
            }
          }
        }
      }
      if ((style->fill_rule_nonzero && winding != 0) ||
          (!style->fill_rule_nonzero && parity)) {
        uint32_t color;
        uint8_t alpha;
        svg_sample_paint(ctx, &style->fill, transform, px, py, bbox_x, bbox_y,
                         bbox_w, bbox_h, &color, &alpha);
        alpha = svg_multiply_alpha(alpha, svg_fill_alpha_mul(style));
        svg_blend_pixel(&ctx->image, x, y, color, alpha);
      }
    }
  }
}

static void svg_render_path_buffer(svg_render_ctx_t *ctx, const svg_path_buffer_t *buf,
                                   const svg_style_t *style,
                                   svg_transform_t transform) {
  svg_path_buffer_t tx = {0};
  double bbox_min_x = 1e9;
  double bbox_min_y = 1e9;
  double bbox_max_x = -1e9;
  double bbox_max_y = -1e9;
  double bbox_w;
  double bbox_h;
  if (buf->count <= 0)
    return;
  for (int i = 0; i < buf->count; i++) {
    bbox_min_x = svg_min(bbox_min_x, buf->points[i].x);
    bbox_min_y = svg_min(bbox_min_y, buf->points[i].y);
    bbox_max_x = svg_max(bbox_max_x, buf->points[i].x);
    bbox_max_y = svg_max(bbox_max_y, buf->points[i].y);
    svg_path_buffer_append(&tx, svg_transform_point(transform, buf->points[i]),
                           buf->moves[i], buf->closes[i]);
  }
  bbox_w = bbox_max_x - bbox_min_x;
  bbox_h = bbox_max_y - bbox_min_y;
  if (style->fill.kind != SVG_PAINT_NONE)
    svg_fill_path(ctx, &tx, style, transform, bbox_min_x, bbox_min_y, bbox_w,
                  bbox_h);
  if (style->stroke.kind != SVG_PAINT_NONE && style->stroke_width > 0.0) {
    for (int i = 0; i < tx.count; ) {
      int start = i;
      int end = i;
      int closed = 0;
      int dashed = style->stroke_dash_count > 0;
      int dash_index = 0;
      double dash_remaining = 0.0;
      int dash_draw = 1;

      if (dashed)
        svg_dash_init(style, &dash_index, &dash_remaining, &dash_draw);
      while (end + 1 < tx.count && !tx.moves[end + 1]) {
        if (tx.closes[end]) {
          closed = 1;
          break;
        }
        end++;
      }
      if (tx.closes[end])
        closed = 1;
      if (end > start || closed) {
        for (int seg = start; seg < end; seg++) {
          int round_start = (seg == start) && !closed;
          int round_end = (seg == end - 1) && !closed;
          if (dashed) {
            svg_stroke_segment_dashed(ctx, tx.points[seg], tx.points[seg + 1],
                                      style, transform, bbox_min_x, bbox_min_y,
                                      bbox_w, bbox_h, &dash_index,
                                      &dash_remaining, &dash_draw);
          } else {
            svg_stroke_segment(ctx, tx.points[seg], tx.points[seg + 1], style,
                               transform, round_start, round_end, bbox_min_x,
                               bbox_min_y, bbox_w, bbox_h);
          }
        }
        if (closed) {
          if (dashed) {
            svg_stroke_segment_dashed(ctx, tx.points[end], tx.points[start],
                                      style, transform, bbox_min_x, bbox_min_y,
                                      bbox_w, bbox_h, &dash_index,
                                      &dash_remaining, &dash_draw);
          } else {
            svg_stroke_segment(ctx, tx.points[end], tx.points[start], style,
                               transform, 0, 0, bbox_min_x, bbox_min_y, bbox_w,
                               bbox_h);
          }
        }
        if (!dashed) {
          for (int v = start + 1; v < end; v++)
            svg_stroke_join(ctx, tx.points[v - 1], tx.points[v], tx.points[v + 1],
                            style, transform, bbox_min_x, bbox_min_y, bbox_w,
                            bbox_h);
          if (closed && end > start + 1) {
            svg_stroke_join(ctx, tx.points[end - 1], tx.points[end], tx.points[start],
                            style, transform, bbox_min_x, bbox_min_y, bbox_w,
                            bbox_h);
            svg_stroke_join(ctx, tx.points[end], tx.points[start], tx.points[start + 1],
                            style, transform, bbox_min_x, bbox_min_y, bbox_w,
                            bbox_h);
          }
        }
      } else if (end == start &&
                 style->stroke_linecap == SVG_STROKE_CAP_ROUND) {
        svg_draw_disc(ctx, tx.points[start].x, tx.points[start].y,
                      style->stroke_width * 0.5, &style->stroke,
                      svg_stroke_alpha_mul(style), transform, bbox_min_x,
                      bbox_min_y, bbox_w, bbox_h);
      }
      i = end + 1;
    }
  }
  svg_path_buffer_free(&tx);
}

static void svg_parse_points_list(const uint8_t *data, size_t len,
                                  svg_path_buffer_t *buf, int close_shape) {
  size_t pos = 0;
  int first = 1;
  while (pos < len) {
    double x = 0.0, y = 0.0;
    if (svg_parse_number(data, len, &pos, &x) != 0 ||
        svg_parse_number(data, len, &pos, &y) != 0)
      break;
    svg_path_buffer_append(buf, (svg_point_t){x, y}, first, 0);
    first = 0;
  }
  if (close_shape && buf->count > 0)
    buf->closes[buf->count - 1] = 1;
}

static int svg_alloc_image(media_image_t *out, uint32_t width, uint32_t height) {
  uint32_t *pixels;
  uint64_t count = (uint64_t)width * (uint64_t)height;
  if (!out || !width || !height || count > 0x1FFFFFFF)
    return -EINVAL;
  pixels = (uint32_t *)kmalloc((size_t)count * sizeof(uint32_t), GFP_KERNEL);
  if (!pixels)
    return -ENOMEM;
  for (uint64_t i = 0; i < count; i++)
    pixels[i] = 0x00000000;
  out->width = width;
  out->height = height;
  out->pixels = pixels;
  return 0;
}

static int svg_parse_viewbox(const uint8_t *data, size_t start, size_t end,
                             double *x, double *y, double *w, double *h) {
  size_t value_start = 0, value_len = 0, pos;
  if (svg_find_attr(data, start, end, "viewBox", &value_start, &value_len) != 0)
    return -ENOENT;
  pos = value_start;
  if (svg_parse_number(data, value_start + value_len, &pos, x) != 0 ||
      svg_parse_number(data, value_start + value_len, &pos, y) != 0 ||
      svg_parse_number(data, value_start + value_len, &pos, w) != 0 ||
      svg_parse_number(data, value_start + value_len, &pos, h) != 0)
    return -EINVAL;
  return 0;
}

static int svg_parse_dimension_attr(const uint8_t *data, size_t start, size_t end,
                                    const char *name, double *out) {
  size_t value_start = 0, value_len = 0, pos;
  if (svg_find_attr(data, start, end, name, &value_start, &value_len) != 0)
    return -ENOENT;
  pos = value_start;
  return svg_parse_number(data, value_start + value_len, &pos, out);
}

static int svg_parse_length_attr(const uint8_t *data, size_t start, size_t end,
                                 const char *name, double relative,
                                 double *out) {
  size_t value_start = 0, value_len = 0;

  if (svg_find_attr(data, start, end, name, &value_start, &value_len) != 0)
    return -ENOENT;
  svg_parse_length_number(data, value_start, value_len, relative, out);
  return 0;
}

static void svg_parse_preserve_aspect_ratio(const uint8_t *data, size_t start,
                                            size_t end, int *align_x,
                                            int *align_y, int *meet_mode,
                                            int *preserve_none) {
  size_t value_start = 0, value_len = 0, s = 0, e = 0;

  if (align_x)
    *align_x = 1;
  if (align_y)
    *align_y = 1;
  if (meet_mode)
    *meet_mode = 1;
  if (preserve_none)
    *preserve_none = 0;

  if (svg_find_attr(data, start, end, "preserveAspectRatio", &value_start,
                    &value_len) != 0)
    return;
  if (svg_trimmed_range(data, value_start, value_start + value_len, &s, &e) != 0)
    return;
  if (svg_match_text(data + s, e - s, "none")) {
    if (preserve_none)
      *preserve_none = 1;
    return;
  }

  if (e - s >= 8) {
    if (media_bytes_starts_with(data, e, s, "xMin"))
      *align_x = 0;
    else if (media_bytes_starts_with(data, e, s, "xMid"))
      *align_x = 1;
    else if (media_bytes_starts_with(data, e, s, "xMax"))
      *align_x = 2;

    if (media_bytes_starts_with(data, e, s + 4, "YMin"))
      *align_y = 0;
    else if (media_bytes_starts_with(data, e, s + 4, "YMid"))
      *align_y = 1;
    else if (media_bytes_starts_with(data, e, s + 4, "YMax"))
      *align_y = 2;
  }

  while (s < e && data[s] != ' ')
    s++;
  while (s < e && svg_is_space(data[s]))
    s++;
  if (s < e) {
    if (svg_match_text(data + s, e - s, "slice"))
      *meet_mode = 0;
    else if (svg_match_text(data + s, e - s, "meet"))
      *meet_mode = 1;
  }
}

static void svg_add_gradient(svg_render_ctx_t *ctx, const svg_gradient_t *gradient) {
  if (!ctx || !gradient || ctx->gradient_count >= (int)(sizeof(ctx->gradients) / sizeof(ctx->gradients[0])))
    return;
  ctx->gradients[ctx->gradient_count++] = *gradient;
}

static void svg_init_gradient_defaults(svg_gradient_t *g) {
  if (!g)
    return;
  g->units = SVG_GRADIENT_UNITS_OBJECT;
  g->spread_method = SVG_GRADIENT_SPREAD_PAD;
  g->transform = svg_transform_identity();
  g->x1 = g->y1 = 0.0;
  g->x2 = 1.0;
  g->y2 = 0.0;
  g->cx = 0.5;
  g->cy = 0.5;
  g->r = 0.5;
  g->fx = 0.5;
  g->fy = 0.5;
  g->fr = 0.0;
  g->color0 = g->color1 = 0x000000;
  g->alpha0 = g->alpha1 = 255;
  g->stop_count = 0;
  g->id[0] = '\0';
}

static size_t svg_parse_defs_block(svg_render_ctx_t *ctx, const uint8_t *data,
                                   size_t size, size_t tag_start,
                                   size_t tag_end) {
  size_t close_start = 0, close_end = tag_end;
  svg_gradient_t active_gradient;
  int have_active_gradient = 0;

  if (!ctx || !data)
    return tag_end;
  if (svg_find_closing_tag(data, size, tag_end + 1, "defs", &close_start,
                           &close_end) != 0)
    return tag_end;

  for (size_t i = tag_end + 1; i < close_start; i++) {
    size_t child_start;
    size_t child_end;
    int closing;

    if (data[i] != '<')
      continue;
    child_start = i;
    i++;
    if (i >= close_start)
      break;
    closing = data[i] == '/';
    if (closing) {
      while (i < close_start && data[i] != '>')
        i++;
      if (have_active_gradient &&
          (media_bytes_starts_with(data, i, child_start + 2, "linearGradient") ||
           media_bytes_starts_with(data, i, child_start + 2, "radialGradient"))) {
        svg_add_gradient(ctx, &active_gradient);
        have_active_gradient = 0;
      }
      continue;
    }
    if (data[i] == '!' || data[i] == '?')
      continue;
    while (i < close_start && data[i] != '>')
      i++;
    if (i >= close_start)
      break;
    child_end = i;

    if (media_bytes_starts_with(data, child_end, child_start + 1,
                                "linearGradient") ||
        media_bytes_starts_with(data, child_end, child_start + 1,
                                "radialGradient")) {
      size_t vs = 0, vl = 0;
      svg_gradient_t *base = NULL;
      svg_gradient_t g;
      int self_closing = svg_tag_is_self_closing(data, child_start, child_end);
      int kind =
          media_bytes_starts_with(data, child_end, child_start + 1,
                                  "linearGradient")
              ? SVG_GRADIENT_LINEAR
              : SVG_GRADIENT_RADIAL;
      if ((svg_find_attr(data, child_start, child_end, "href", &vs, &vl) == 0 ||
           svg_find_attr(data, child_start, child_end, "xlink:href", &vs,
                         &vl) == 0) &&
          vl > 1 && data[vs] == '#') {
        char ref_id[32];
        svg_copy_ref(ref_id, sizeof(ref_id), data + vs + 1, vl - 1);
        base = svg_find_gradient(ctx, ref_id);
      }
      if (base) {
        g = *base;
      } else {
        svg_init_gradient_defaults(&g);
      }
      g.kind = kind;
      if (svg_find_attr(data, child_start, child_end, "id", &vs, &vl) == 0)
        svg_copy_ref(g.id, sizeof(g.id), data + vs, vl);
      if (svg_find_attr(data, child_start, child_end, "gradientUnits", &vs,
                        &vl) == 0 &&
          svg_match_text(data + vs, vl, "userSpaceOnUse")) {
        g.units = SVG_GRADIENT_UNITS_USERSPACE;
      }
      if (svg_find_attr(data, child_start, child_end, "spreadMethod", &vs,
                        &vl) == 0) {
        if (svg_match_text(data + vs, vl, "reflect")) {
          g.spread_method = SVG_GRADIENT_SPREAD_REFLECT;
        } else if (svg_match_text(data + vs, vl, "repeat")) {
          g.spread_method = SVG_GRADIENT_SPREAD_REPEAT;
        } else {
          g.spread_method = SVG_GRADIENT_SPREAD_PAD;
        }
      }
      if (svg_find_attr(data, child_start, child_end, "gradientTransform", &vs,
                        &vl) == 0)
        g.transform = svg_parse_transform_value(data + vs, vl);
      if (g.kind == SVG_GRADIENT_LINEAR) {
        if (svg_find_attr(data, child_start, child_end, "x1", &vs, &vl) == 0)
          svg_parse_gradient_number(data, vs, vl, &g.x1);
        if (svg_find_attr(data, child_start, child_end, "y1", &vs, &vl) == 0)
          svg_parse_gradient_number(data, vs, vl, &g.y1);
        if (svg_find_attr(data, child_start, child_end, "x2", &vs, &vl) == 0)
          svg_parse_gradient_number(data, vs, vl, &g.x2);
        if (svg_find_attr(data, child_start, child_end, "y2", &vs, &vl) == 0)
          svg_parse_gradient_number(data, vs, vl, &g.y2);
      } else {
        if (svg_find_attr(data, child_start, child_end, "cx", &vs, &vl) == 0)
          svg_parse_gradient_number(data, vs, vl, &g.cx);
        if (svg_find_attr(data, child_start, child_end, "cy", &vs, &vl) == 0)
          svg_parse_gradient_number(data, vs, vl, &g.cy);
        if (svg_find_attr(data, child_start, child_end, "r", &vs, &vl) == 0)
          svg_parse_gradient_number(data, vs, vl, &g.r);
        if (svg_find_attr(data, child_start, child_end, "fx", &vs, &vl) == 0)
          svg_parse_gradient_number(data, vs, vl, &g.fx);
        else
          g.fx = g.cx;
        if (svg_find_attr(data, child_start, child_end, "fy", &vs, &vl) == 0)
          svg_parse_gradient_number(data, vs, vl, &g.fy);
        else
          g.fy = g.cy;
        if (svg_find_attr(data, child_start, child_end, "fr", &vs, &vl) == 0)
          svg_parse_gradient_number(data, vs, vl, &g.fr);
        else
          g.fr = 0.0;
        if (g.fr < 0.0)
          g.fr = 0.0;
        if (g.fr > g.r)
          g.fr = g.r;
      }
      if (self_closing) {
        svg_add_gradient(ctx, &g);
        have_active_gradient = 0;
      } else {
        active_gradient = g;
        have_active_gradient = 1;
      }
    } else if (have_active_gradient &&
               media_bytes_starts_with(data, child_end, child_start + 1,
                                       "stop")) {
      size_t vs = 0, vl = 0, pos;
      double offset = 0.0;
      uint32_t color = 0x000000;
      uint8_t alpha = 255;

      if (svg_find_attr(data, child_start, child_end, "offset", &vs, &vl) ==
          0) {
        pos = vs;
        svg_parse_number(data, vs + vl, &pos, &offset);
        if (vs + vl > vs && data[vs + vl - 1] == '%')
          offset /= 100.0;
      }
      if (svg_find_attr(data, child_start, child_end, "stop-color", &vs, &vl) ==
          0)
        svg_parse_color(data + vs, vl, &color, &alpha);
      if (svg_find_attr(data, child_start, child_end, "style", &vs, &vl) == 0)
        svg_parse_stop_style_declarations(data + vs, vl, &color, &alpha);
      if (svg_find_attr(data, child_start, child_end, "stop-opacity", &vs,
                        &vl) == 0) {
        double opacity = 1.0;
        pos = vs;
        if (svg_parse_number(data, vs + vl, &pos, &opacity) == 0) {
          if (opacity < 0.0)
            opacity = 0.0;
          if (opacity > 1.0)
            opacity = 1.0;
          alpha = (uint8_t)(opacity * 255.0);
        }
      }
      svg_gradient_add_stop(&active_gradient, offset, color, alpha);
    }
  }

  return close_end;
}

static int svg_decode_data_uri_image(const uint8_t *data, size_t len,
                                     media_image_t *image) {
  static const char *k_png = "data:image/png;base64,";
  static const char *k_jpg = "data:image/jpeg;base64,";
  static const char *k_jpg2 = "data:image/jpg;base64,";
  const char *prefix = NULL;
  size_t prefix_len = 0;
  uint8_t *decoded = NULL;
  size_t decoded_len = 0;
  int ret;
  if (len >= 22 && svg_match_text(data, 22, k_png)) {
    prefix = k_png;
    prefix_len = 22;
  } else if (len >= 23 && svg_match_text(data, 23, k_jpg)) {
    prefix = k_jpg;
    prefix_len = 23;
  } else if (len >= 22 && svg_match_text(data, 22, k_jpg2)) {
    prefix = k_jpg2;
    prefix_len = 22;
  }
  if (!prefix)
    return -ENOENT;
  ret = media_decode_base64(data + prefix_len, len - prefix_len, &decoded, &decoded_len);
  if (ret != 0)
    return ret;
  ret = (prefix == k_png) ? media_decode_png(decoded, decoded_len, image)
                          : media_decode_jpeg(decoded, decoded_len, image);
  kfree(decoded);
  return ret;
}

static void svg_blit_image_region(svg_render_ctx_t *ctx, media_image_t *src,
                                  double x, double y, double w, double h,
                                  double src_x, double src_y, double src_w,
                                  double src_h, uint8_t opacity_mul) {
  int dst_w = (int)(w > 0.0 ? w : (double)src->width);
  int dst_h = (int)(h > 0.0 ? h : (double)src->height);
  if (!ctx || !src || !src->pixels || dst_w <= 0 || dst_h <= 0 || src_w <= 0.0 ||
      src_h <= 0.0)
    return;
  for (int yy = 0; yy < dst_h; yy++) {
    double sample_y = src_y + (((double)yy + 0.5) * src_h) / (double)dst_h;
    int sy = (int)sample_y;
    if (sy < 0)
      sy = 0;
    if (sy >= (int)src->height)
      sy = (int)src->height - 1;
    for (int xx = 0; xx < dst_w; xx++) {
      double sample_x = src_x + (((double)xx + 0.5) * src_w) / (double)dst_w;
      int sx = (int)sample_x;
      if (sx < 0)
        sx = 0;
      if (sx >= (int)src->width)
        sx = (int)src->width - 1;
      uint32_t pixel = src->pixels[sy * src->width + sx];
      uint8_t alpha = (pixel >> 24) & 0xFF;
      if (alpha == 0)
        alpha = 255;
      alpha = svg_multiply_alpha(alpha, opacity_mul);
      if (alpha == 0)
        continue;
      svg_blend_pixel(&ctx->image, (int)x + xx, (int)y + yy, pixel & 0xFFFFFF, alpha);
    }
  }
}

static void svg_blit_image(svg_render_ctx_t *ctx, media_image_t *src,
                           double x, double y, double w, double h) {
  if (!src)
    return;
  svg_blit_image_region(ctx, src, x, y, w, h, 0.0, 0.0, (double)src->width,
                        (double)src->height, 255);
}

static int svg_render_referenced_element_internal(
    svg_render_ctx_t *ctx, const uint8_t *data, size_t size, size_t tag_start,
    size_t tag_end, const svg_style_t *base_style,
    svg_transform_t base_transform, double viewport_w, double viewport_h,
    double viewport_diag, double use_width, double use_height, int depth) {
  svg_style_t style;
  svg_transform_t transform;

  if (!ctx || !data || !base_style || depth > 8)
    return 0;

  style = *base_style;
  transform =
      svg_transform_multiply(base_transform,
                             svg_parse_transform_attr(data, tag_start, tag_end));
  svg_apply_style_attrs(data, tag_start, tag_end, &style);
  if (!svg_style_is_displayed(&style))
    return 1;

  if (media_bytes_starts_with(data, tag_end, tag_start + 1, "g") ||
      media_bytes_starts_with(data, tag_end, tag_start + 1, "svg") ||
      media_bytes_starts_with(data, tag_end, tag_start + 1, "symbol")) {
    size_t close_start = 0, close_end = 0;
    const char *tag_name;
    int is_svg_like = 0;
    double local_x = 0.0;
    double local_y = 0.0;
    double local_view_x = 0.0, local_view_y = 0.0, local_view_w = 0.0,
           local_view_h = 0.0;
    double local_width = use_width;
    double local_height = use_height;
    int align_x = 1, align_y = 1, meet_mode = 1, preserve_none = 0;

    if (media_bytes_starts_with(data, tag_end, tag_start + 1, "svg")) {
      tag_name = "svg";
      is_svg_like = 1;
    } else if (media_bytes_starts_with(data, tag_end, tag_start + 1, "symbol")) {
      tag_name = "symbol";
      is_svg_like = 1;
    } else {
      tag_name = "g";
    }

    if (is_svg_like) {
      svg_parse_length_attr(data, tag_start, tag_end, "x", viewport_w, &local_x);
      svg_parse_length_attr(data, tag_start, tag_end, "y", viewport_h, &local_y);
      transform = svg_transform_multiply(transform,
                                         svg_transform_translate(local_x, local_y));
      if (svg_parse_viewbox(data, tag_start, tag_end, &local_view_x, &local_view_y,
                            &local_view_w, &local_view_h) != 0) {
        local_view_x = 0.0;
        local_view_y = 0.0;
      }
      if (local_width <= 0.0)
        svg_parse_length_attr(data, tag_start, tag_end, "width", viewport_w,
                              &local_width);
      if (local_height <= 0.0)
        svg_parse_length_attr(data, tag_start, tag_end, "height", viewport_h,
                              &local_height);
      if (local_width <= 0.0)
        local_width = local_view_w > 0.0 ? local_view_w : viewport_w;
      if (local_height <= 0.0)
        local_height = local_view_h > 0.0 ? local_view_h : viewport_h;
      svg_parse_preserve_aspect_ratio(data, tag_start, tag_end, &align_x, &align_y,
                                      &meet_mode, &preserve_none);
      if (local_view_w > 0.0 && local_view_h > 0.0 && local_width > 0.0 &&
          local_height > 0.0) {
        double scale_x = local_width / local_view_w;
        double scale_y = local_height / local_view_h;
        double tx;
        double ty;
        svg_transform_t local_transform;

        if (!preserve_none) {
          double scale = meet_mode ? svg_min(scale_x, scale_y)
                                   : svg_max(scale_x, scale_y);
          double scaled_w = local_view_w * scale;
          double scaled_h = local_view_h * scale;
          double extra_x = local_width - scaled_w;
          double extra_y = local_height - scaled_h;
          scale_x = scale_y = scale;
          tx = -local_view_x * scale_x;
          ty = -local_view_y * scale_y;
          if (align_x == 1)
            tx += extra_x * 0.5;
          else if (align_x == 2)
            tx += extra_x;
          if (align_y == 1)
            ty += extra_y * 0.5;
          else if (align_y == 2)
            ty += extra_y;
        } else {
          tx = -local_view_x * scale_x;
          ty = -local_view_y * scale_y;
        }
        local_transform.a = scale_x;
        local_transform.b = 0.0;
        local_transform.c = 0.0;
        local_transform.d = scale_y;
        local_transform.e = tx;
        local_transform.f = ty;
        transform = svg_transform_multiply(transform, local_transform);
      }
    }

    if (svg_find_closing_tag(data, size, tag_end + 1, tag_name, &close_start,
                             &close_end) != 0)
      return 0;
    for (size_t i = tag_end + 1; i < close_start; i++) {
      size_t child_start;
      size_t child_end;
      size_t ref_start = 0, ref_end = 0;
      int closing;
      size_t vs = 0, vl = 0;
      double use_x = 0.0, use_y = 0.0;
      double child_use_w = -1.0, child_use_h = -1.0;
      svg_transform_t use_transform;

      if (data[i] != '<')
        continue;
      child_start = i;
      i++;
      if (i >= close_start)
        break;
      closing = data[i] == '/';
      if (closing)
        continue;
      if (data[i] == '!' || data[i] == '?')
        continue;
      while (i < close_start && data[i] != '>')
        i++;
      if (i >= close_start)
        break;
      child_end = i;

      if (media_bytes_starts_with(data, child_end, child_start + 1, "defs")) {
        i = svg_parse_defs_block(ctx, data, close_start, child_start, child_end);
        continue;
      }
      if (media_bytes_starts_with(data, child_end, child_start + 1, "use")) {
        if (svg_find_attr(data, child_start, child_end, "x", &vs, &vl) == 0)
          svg_parse_length_number(data, vs, vl, viewport_w, &use_x);
        if (svg_find_attr(data, child_start, child_end, "y", &vs, &vl) == 0)
          svg_parse_length_number(data, vs, vl, viewport_h, &use_y);
        if (svg_find_attr(data, child_start, child_end, "width", &vs, &vl) == 0)
          svg_parse_length_number(data, vs, vl, viewport_w, &child_use_w);
        if (svg_find_attr(data, child_start, child_end, "height", &vs, &vl) == 0)
          svg_parse_length_number(data, vs, vl, viewport_h, &child_use_h);
        if ((svg_find_attr(data, child_start, child_end, "href", &vs, &vl) ==
                 0 ||
             svg_find_attr(data, child_start, child_end, "xlink:href", &vs,
                           &vl) == 0) &&
            vl > 1 && data[vs] == '#') {
          char ref_id[32];
          svg_copy_ref(ref_id, sizeof(ref_id), data + vs + 1, vl - 1);
          if (svg_find_element_by_id(data, size, ref_id, &ref_start, &ref_end) ==
              0) {
            use_transform = svg_transform_multiply(
                transform, svg_transform_translate(use_x, use_y));
            svg_render_referenced_element_internal(
                ctx, data, size, ref_start, ref_end, &style, use_transform,
                viewport_w, viewport_h, viewport_diag, child_use_w, child_use_h,
                depth + 1);
          }
        }
        continue;
      }

      svg_render_referenced_element_internal(
          ctx, data, size, child_start, child_end, &style, transform,
          viewport_w, viewport_h, viewport_diag, -1.0, -1.0, depth + 1);
    }
    return 1;
  }

  if (media_bytes_starts_with(data, tag_end, tag_start + 1, "use")) {
    size_t vs = 0, vl = 0;
    double use_x = 0.0, use_y = 0.0;
    double child_use_w = -1.0, child_use_h = -1.0;
    size_t ref_tag_start = 0, ref_tag_end = 0;
    svg_transform_t use_transform;

    if (svg_find_attr(data, tag_start, tag_end, "x", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_w, &use_x);
    if (svg_find_attr(data, tag_start, tag_end, "y", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_h, &use_y);
    if (svg_find_attr(data, tag_start, tag_end, "width", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_w, &child_use_w);
    if (svg_find_attr(data, tag_start, tag_end, "height", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_h, &child_use_h);
    if ((svg_find_attr(data, tag_start, tag_end, "href", &vs, &vl) == 0 ||
         svg_find_attr(data, tag_start, tag_end, "xlink:href", &vs, &vl) == 0) &&
        vl > 1 && data[vs] == '#') {
      char ref_id[32];
      svg_copy_ref(ref_id, sizeof(ref_id), data + vs + 1, vl - 1);
      if (svg_find_element_by_id(data, size, ref_id, &ref_tag_start,
                                 &ref_tag_end) == 0) {
        if (!svg_style_is_visible(&style))
          return 1;
        use_transform = svg_transform_multiply(
            transform, svg_transform_translate(use_x, use_y));
        return svg_render_referenced_element_internal(
            ctx, data, size, ref_tag_start, ref_tag_end, &style, use_transform,
            viewport_w, viewport_h, viewport_diag, child_use_w, child_use_h,
            depth + 1);
      }
    }
    return 0;
  }

  if (media_bytes_starts_with(data, tag_end, tag_start + 1, "path")) {
    size_t vs = 0, vl = 0;
    svg_path_buffer_t buf = {0};
    if (!svg_style_is_visible(&style))
      return 1;
    if (svg_find_attr(data, tag_start, tag_end, "d", &vs, &vl) == 0 &&
        svg_parse_path_data(data + vs, vl, &buf) == 0)
      svg_render_path_buffer(ctx, &buf, &style, transform);
    svg_path_buffer_free(&buf);
    return 1;
  }
  if (media_bytes_starts_with(data, tag_end, tag_start + 1, "polyline") ||
      media_bytes_starts_with(data, tag_end, tag_start + 1, "polygon")) {
    size_t vs = 0, vl = 0;
    svg_path_buffer_t buf = {0};
    if (!svg_style_is_visible(&style))
      return 1;
    if (svg_find_attr(data, tag_start, tag_end, "points", &vs, &vl) == 0) {
      svg_parse_points_list(data + vs, vl, &buf,
                            media_bytes_starts_with(data, tag_end, tag_start + 1,
                                                    "polygon"));
      svg_render_path_buffer(ctx, &buf, &style, transform);
    }
    svg_path_buffer_free(&buf);
    return 1;
  }
  if (media_bytes_starts_with(data, tag_end, tag_start + 1, "line")) {
    double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
    size_t vs = 0, vl = 0;
    if (!svg_style_is_visible(&style))
      return 1;
    if (svg_find_attr(data, tag_start, tag_end, "x1", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_w, &x1);
    if (svg_find_attr(data, tag_start, tag_end, "y1", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_h, &y1);
    if (svg_find_attr(data, tag_start, tag_end, "x2", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_w, &x2);
    if (svg_find_attr(data, tag_start, tag_end, "y2", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_h, &y2);
    {
      svg_point_t a = svg_transform_point(transform, (svg_point_t){x1, y1});
      svg_point_t b = svg_transform_point(transform, (svg_point_t){x2, y2});
      if (style.stroke_dash_count > 0) {
        int dash_index = 0;
        double dash_remaining = 0.0;
        int dash_draw = 1;
        svg_dash_init(&style, &dash_index, &dash_remaining, &dash_draw);
        svg_stroke_segment_dashed(ctx, a, b, &style, transform, svg_min(x1, x2),
                                  svg_min(y1, y2), svg_absd(x2 - x1),
                                  svg_absd(y2 - y1), &dash_index,
                                  &dash_remaining, &dash_draw);
      } else {
        svg_stroke_segment(ctx, a, b, &style, transform, 1, 1,
                           svg_min(x1, x2), svg_min(y1, y2), svg_absd(x2 - x1),
                           svg_absd(y2 - y1));
      }
    }
    return 1;
  }
  if (media_bytes_starts_with(data, tag_end, tag_start + 1, "rect")) {
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0, rx = 0.0, ry = 0.0;
    size_t vs = 0, vl = 0;
    svg_path_buffer_t buf = {0};
    if (!svg_style_is_visible(&style))
      return 1;
    if (svg_find_attr(data, tag_start, tag_end, "x", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_w, &x);
    if (svg_find_attr(data, tag_start, tag_end, "y", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_h, &y);
    if (svg_find_attr(data, tag_start, tag_end, "width", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_w, &w);
    if (svg_find_attr(data, tag_start, tag_end, "height", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_h, &h);
    if (svg_find_attr(data, tag_start, tag_end, "rx", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_w, &rx);
    if (svg_find_attr(data, tag_start, tag_end, "ry", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_h, &ry);
    if (rx > 0.0 && ry <= 0.0)
      ry = rx;
    if (ry > 0.0 && rx <= 0.0)
      rx = ry;
    rx = svg_clamp(rx, 0.0, w * 0.5);
    ry = svg_clamp(ry, 0.0, h * 0.5);
    if (rx > 0.0 && ry > 0.0) {
      svg_append_rounded_rect(&buf, x, y, w, h, rx, ry);
    } else {
      svg_path_buffer_append(&buf, (svg_point_t){x, y}, 1, 0);
      svg_path_buffer_append(&buf, (svg_point_t){x + w, y}, 0, 0);
      svg_path_buffer_append(&buf, (svg_point_t){x + w, y + h}, 0, 0);
      svg_path_buffer_append(&buf, (svg_point_t){x, y + h}, 0, 1);
    }
    svg_render_path_buffer(ctx, &buf, &style, transform);
    svg_path_buffer_free(&buf);
    return 1;
  }
  if (media_bytes_starts_with(data, tag_end, tag_start + 1, "circle")) {
    double cx = 0.0, cy = 0.0, r = 0.0;
    size_t vs = 0, vl = 0;
    svg_path_buffer_t buf = {0};
    if (!svg_style_is_visible(&style))
      return 1;
    if (svg_find_attr(data, tag_start, tag_end, "cx", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_w, &cx);
    if (svg_find_attr(data, tag_start, tag_end, "cy", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_h, &cy);
    if (svg_find_attr(data, tag_start, tag_end, "r", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_diag, &r);
    if (r > 0.0) {
      svg_append_ellipse_path(&buf, cx, cy, r, r);
      svg_render_path_buffer(ctx, &buf, &style, transform);
    }
    svg_path_buffer_free(&buf);
    return 1;
  }
  if (media_bytes_starts_with(data, tag_end, tag_start + 1, "ellipse")) {
    double cx = 0.0, cy = 0.0, rx = 0.0, ry = 0.0;
    size_t vs = 0, vl = 0;
    svg_path_buffer_t buf = {0};
    if (!svg_style_is_visible(&style))
      return 1;
    if (svg_find_attr(data, tag_start, tag_end, "cx", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_w, &cx);
    if (svg_find_attr(data, tag_start, tag_end, "cy", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_h, &cy);
    if (svg_find_attr(data, tag_start, tag_end, "rx", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_w, &rx);
    if (svg_find_attr(data, tag_start, tag_end, "ry", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_h, &ry);
    if (rx > 0.0 && ry > 0.0) {
      svg_append_ellipse_path(&buf, cx, cy, rx, ry);
      svg_render_path_buffer(ctx, &buf, &style, transform);
    }
    svg_path_buffer_free(&buf);
    return 1;
  }
  if (media_bytes_starts_with(data, tag_end, tag_start + 1, "image")) {
    size_t vs = 0, vl = 0;
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
    double draw_x, draw_y, draw_w, draw_h;
    double src_x = 0.0, src_y = 0.0;
    double src_w, src_h;
    int align_x = 1, align_y = 1, meet_mode = 1, preserve_none = 0;
    media_image_t embedded = {0};
    if (!svg_style_is_visible(&style))
      return 1;
    if (svg_find_attr(data, tag_start, tag_end, "x", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_w, &x);
    if (svg_find_attr(data, tag_start, tag_end, "y", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_h, &y);
    if (svg_find_attr(data, tag_start, tag_end, "width", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_w, &w);
    if (svg_find_attr(data, tag_start, tag_end, "height", &vs, &vl) == 0)
      svg_parse_length_number(data, vs, vl, viewport_h, &h);
    if ((svg_find_attr(data, tag_start, tag_end, "href", &vs, &vl) == 0 ||
         svg_find_attr(data, tag_start, tag_end, "xlink:href", &vs, &vl) == 0) &&
        svg_decode_data_uri_image(data + vs, vl, &embedded) == 0) {
      svg_point_t p = svg_transform_point(transform, (svg_point_t){x, y});
      draw_x = p.x;
      draw_y = p.y;
      draw_w = w > 0.0 ? w * transform.a : (double)embedded.width;
      draw_h = h > 0.0 ? h * transform.d : (double)embedded.height;
      src_w = (double)embedded.width;
      src_h = (double)embedded.height;
      svg_parse_preserve_aspect_ratio(data, tag_start, tag_end, &align_x,
                                      &align_y, &meet_mode, &preserve_none);
      if (!preserve_none && draw_w > 0.0 && draw_h > 0.0 && src_w > 0.0 &&
          src_h > 0.0) {
        double scale_x = draw_w / src_w;
        double scale_y = draw_h / src_h;
        if (meet_mode) {
          double scale = svg_min(scale_x, scale_y);
          double fit_w = src_w * scale;
          double fit_h = src_h * scale;
          double extra_x = draw_w - fit_w;
          double extra_y = draw_h - fit_h;
          draw_w = fit_w;
          draw_h = fit_h;
          if (align_x == 1)
            draw_x += extra_x * 0.5;
          else if (align_x == 2)
            draw_x += extra_x;
          if (align_y == 1)
            draw_y += extra_y * 0.5;
          else if (align_y == 2)
            draw_y += extra_y;
        } else {
          double scale = svg_max(scale_x, scale_y);
          double crop_w = draw_w / scale;
          double crop_h = draw_h / scale;
          double extra_x = src_w - crop_w;
          double extra_y = src_h - crop_h;
          src_w = crop_w;
          src_h = crop_h;
          if (align_x == 1)
            src_x += extra_x * 0.5;
          else if (align_x == 2)
            src_x += extra_x;
          if (align_y == 1)
            src_y += extra_y * 0.5;
          else if (align_y == 2)
            src_y += extra_y;
        }
      }
      if (draw_w < 0.0) {
        draw_x += draw_w;
        draw_w = -draw_w;
      }
      if (draw_h < 0.0) {
        draw_y += draw_h;
        draw_h = -draw_h;
      }
      svg_blit_image_region(ctx, &embedded, draw_x, draw_y, draw_w, draw_h,
                            src_x, src_y, src_w, src_h, style.opacity);
      media_free_image(&embedded);
    }
    return 1;
  }
  if (media_bytes_starts_with(data, tag_end, tag_start + 1, "text")) {
    size_t consumed_end = tag_end;
    svg_render_text_element(ctx, data, size, tag_start, tag_end, &consumed_end,
                            &style, transform);
    return 1;
  }

  return 0;
}

static int svg_render_referenced_element(svg_render_ctx_t *ctx,
                                         const uint8_t *data, size_t size,
                                         size_t tag_start, size_t tag_end,
                                         const svg_style_t *base_style,
                                         svg_transform_t base_transform,
                                         double viewport_w, double viewport_h,
                                         double viewport_diag) {
  return svg_render_referenced_element_internal(
      ctx, data, size, tag_start, tag_end, base_style, base_transform,
      viewport_w, viewport_h, viewport_diag, -1.0, -1.0, 0);
}

static int media_decode_svg_vector(const uint8_t *data, size_t size,
                                   media_image_t *out) {
  svg_render_ctx_t ctx;
  svg_style_t style_stack[16];
  svg_transform_t transform_stack[16];
  int sp = 0;
  int in_defs = 0;
  svg_gradient_t active_gradient;
  int have_active_gradient = 0;
  size_t svg_start = 0, svg_end = 0;
  double view_x = 0.0, view_y = 0.0, view_w = 0.0, view_h = 0.0;
  double width = 0.0, height = 0.0;
  double viewport_w;
  double viewport_h;
  double viewport_diag;
  int align_x = 1, align_y = 1, meet_mode = 1, preserve_none = 0;
  int ret;

  if (!data || !out || size == 0)
    return -EINVAL;

  for (size_t i = 0; i + 4 < size; i++) {
    if (data[i] == '<' && media_bytes_starts_with(data, size, i + 1, "svg")) {
      svg_start = i;
      while (i < size && data[i] != '>')
        i++;
      svg_end = i;
      break;
    }
  }
  if (svg_end <= svg_start)
    return -ENOENT;

  if (svg_parse_viewbox(data, svg_start, svg_end, &view_x, &view_y, &view_w, &view_h) != 0) {
    svg_parse_dimension_attr(data, svg_start, svg_end, "width", &view_w);
    svg_parse_dimension_attr(data, svg_start, svg_end, "height", &view_h);
    view_x = 0.0;
    view_y = 0.0;
  }
  svg_parse_dimension_attr(data, svg_start, svg_end, "width", &width);
  svg_parse_dimension_attr(data, svg_start, svg_end, "height", &height);
  svg_parse_preserve_aspect_ratio(data, svg_start, svg_end, &align_x, &align_y,
                                  &meet_mode, &preserve_none);
  if (width <= 0.0)
    width = view_w;
  if (height <= 0.0)
    height = view_h;
  if (width <= 0.0 || height <= 0.0)
    return -EINVAL;
  viewport_w = view_w > 0.0 ? view_w : width;
  viewport_h = view_h > 0.0 ? view_h : height;
  viewport_diag = svg_viewport_normalized_diagonal(viewport_w, viewport_h);
  ret = svg_alloc_image(out, (uint32_t)(width + 0.5), (uint32_t)(height + 0.5));
  if (ret != 0)
    return ret;

  ctx.image = *out;
  ctx.gradient_count = 0;
  ctx.root_transform = svg_transform_identity();
  if (view_w > 0.0 && view_h > 0.0) {
    double scale_x = width / view_w;
    double scale_y = height / view_h;
    double tx;
    double ty;
    svg_transform_t t;
    if (!preserve_none) {
      double scale = meet_mode ? svg_min(scale_x, scale_y)
                               : svg_max(scale_x, scale_y);
      double scaled_w = view_w * scale;
      double scaled_h = view_h * scale;
      double extra_x = width - scaled_w;
      double extra_y = height - scaled_h;
      scale_x = scale_y = scale;
      tx = -view_x * scale_x;
      ty = -view_y * scale_y;
      if (align_x == 1)
        tx += extra_x * 0.5;
      else if (align_x == 2)
        tx += extra_x;
      if (align_y == 1)
        ty += extra_y * 0.5;
      else if (align_y == 2)
        ty += extra_y;
    } else {
      tx = -view_x * scale_x;
      ty = -view_y * scale_y;
    }
    t.a = scale_x;
    t.b = 0.0;
    t.c = 0.0;
    t.d = scale_y;
    t.e = tx;
    t.f = ty;
    ctx.root_transform = t;
  }

  svg_init_default_style(&style_stack[0]);
  transform_stack[0] = ctx.root_transform;

  for (size_t i = svg_end + 1; i < size; i++) {
    size_t tag_start, tag_end;
    int closing;
    if (data[i] != '<')
      continue;
    tag_start = i;
    i++;
    if (i >= size)
      break;
    closing = data[i] == '/';
    if (closing)
      i++;
    while (i < size && data[i] != '>')
      i++;
    if (i >= size)
      break;
    tag_end = i;

    if (closing) {
      if (media_bytes_starts_with(data, tag_end, tag_start + 2, "defs"))
        in_defs = 0;
      else if ((media_bytes_starts_with(data, tag_end, tag_start + 2, "g") ||
                media_bytes_starts_with(data, tag_end, tag_start + 2, "svg")) &&
               sp > 0)
        sp--;
      if (have_active_gradient &&
          (media_bytes_starts_with(data, tag_end, tag_start + 2, "linearGradient") ||
           media_bytes_starts_with(data, tag_end, tag_start + 2, "radialGradient"))) {
        svg_add_gradient(&ctx, &active_gradient);
        have_active_gradient = 0;
      }
      continue;
    }

    if (media_bytes_starts_with(data, tag_end, tag_start + 1, "defs")) {
      in_defs = 1;
      continue;
    }

    if (in_defs) {
      if (media_bytes_starts_with(data, tag_end, tag_start + 1, "linearGradient") ||
          media_bytes_starts_with(data, tag_end, tag_start + 1, "radialGradient")) {
        size_t vs = 0, vl = 0;
        svg_gradient_t *base = NULL;
        svg_gradient_t g;
        int self_closing = svg_tag_is_self_closing(data, tag_start, tag_end);
        int kind = media_bytes_starts_with(data, tag_end, tag_start + 1, "linearGradient")
                       ? SVG_GRADIENT_LINEAR
                       : SVG_GRADIENT_RADIAL;
        if ((svg_find_attr(data, tag_start, tag_end, "href", &vs, &vl) == 0 ||
             svg_find_attr(data, tag_start, tag_end, "xlink:href", &vs, &vl) == 0) &&
            vl > 1 && data[vs] == '#') {
          char ref_id[32];
          svg_copy_ref(ref_id, sizeof(ref_id), data + vs + 1, vl - 1);
          base = svg_find_gradient(&ctx, ref_id);
        }
        if (base) {
          g = *base;
        } else {
          g.units = SVG_GRADIENT_UNITS_OBJECT;
          g.spread_method = SVG_GRADIENT_SPREAD_PAD;
          g.transform = svg_transform_identity();
          g.x1 = g.y1 = 0.0;
          g.x2 = 1.0;
          g.y2 = 0.0;
          g.cx = 0.5;
          g.cy = 0.5;
          g.r = 0.5;
          g.fx = 0.5;
          g.fy = 0.5;
          g.fr = 0.0;
          g.color0 = g.color1 = 0x000000;
          g.alpha0 = g.alpha1 = 255;
          g.stop_count = 0;
          g.id[0] = '\0';
        }
        g.kind = kind;
        if (svg_find_attr(data, tag_start, tag_end, "id", &vs, &vl) == 0)
          svg_copy_ref(g.id, sizeof(g.id), data + vs, vl);
        if (svg_find_attr(data, tag_start, tag_end, "gradientUnits", &vs, &vl) == 0 &&
            svg_match_text(data + vs, vl, "userSpaceOnUse")) {
          g.units = SVG_GRADIENT_UNITS_USERSPACE;
        }
        if (svg_find_attr(data, tag_start, tag_end, "spreadMethod", &vs, &vl) == 0) {
          if (svg_match_text(data + vs, vl, "reflect")) {
            g.spread_method = SVG_GRADIENT_SPREAD_REFLECT;
          } else if (svg_match_text(data + vs, vl, "repeat")) {
            g.spread_method = SVG_GRADIENT_SPREAD_REPEAT;
          } else {
            g.spread_method = SVG_GRADIENT_SPREAD_PAD;
          }
        }
        if (svg_find_attr(data, tag_start, tag_end, "gradientTransform", &vs, &vl) == 0)
          g.transform = svg_parse_transform_value(data + vs, vl);
        if (g.kind == SVG_GRADIENT_LINEAR) {
          if (svg_find_attr(data, tag_start, tag_end, "x1", &vs, &vl) == 0) {
            svg_parse_gradient_number(data, vs, vl, &g.x1);
          }
          if (svg_find_attr(data, tag_start, tag_end, "y1", &vs, &vl) == 0) {
            svg_parse_gradient_number(data, vs, vl, &g.y1);
          }
          if (svg_find_attr(data, tag_start, tag_end, "x2", &vs, &vl) == 0) {
            svg_parse_gradient_number(data, vs, vl, &g.x2);
          }
          if (svg_find_attr(data, tag_start, tag_end, "y2", &vs, &vl) == 0) {
            svg_parse_gradient_number(data, vs, vl, &g.y2);
          }
        } else {
          if (svg_find_attr(data, tag_start, tag_end, "cx", &vs, &vl) == 0) {
            svg_parse_gradient_number(data, vs, vl, &g.cx);
          }
          if (svg_find_attr(data, tag_start, tag_end, "cy", &vs, &vl) == 0) {
            svg_parse_gradient_number(data, vs, vl, &g.cy);
          }
          if (svg_find_attr(data, tag_start, tag_end, "r", &vs, &vl) == 0) {
            svg_parse_gradient_number(data, vs, vl, &g.r);
          }
          if (svg_find_attr(data, tag_start, tag_end, "fx", &vs, &vl) == 0) {
            svg_parse_gradient_number(data, vs, vl, &g.fx);
          } else {
            g.fx = g.cx;
          }
          if (svg_find_attr(data, tag_start, tag_end, "fy", &vs, &vl) == 0) {
            svg_parse_gradient_number(data, vs, vl, &g.fy);
          } else {
            g.fy = g.cy;
          }
          if (svg_find_attr(data, tag_start, tag_end, "fr", &vs, &vl) == 0) {
            svg_parse_gradient_number(data, vs, vl, &g.fr);
          } else {
            g.fr = 0.0;
          }
          if (g.fr < 0.0)
            g.fr = 0.0;
          if (g.fr > g.r)
            g.fr = g.r;
        }
        if (self_closing) {
          svg_add_gradient(&ctx, &g);
          have_active_gradient = 0;
        } else {
          active_gradient = g;
          have_active_gradient = 1;
        }
      } else if (have_active_gradient &&
                 media_bytes_starts_with(data, tag_end, tag_start + 1, "stop")) {
        size_t vs = 0, vl = 0, pos;
        double offset = 0.0;
        uint32_t color = 0x000000;
        uint8_t alpha = 255;
        if (svg_find_attr(data, tag_start, tag_end, "offset", &vs, &vl) == 0) {
          pos = vs;
          svg_parse_number(data, vs + vl, &pos, &offset);
          if (vs + vl > vs && data[vs + vl - 1] == '%')
            offset /= 100.0;
        }
        if (svg_find_attr(data, tag_start, tag_end, "stop-color", &vs, &vl) == 0)
          svg_parse_color(data + vs, vl, &color, &alpha);
        if (svg_find_attr(data, tag_start, tag_end, "style", &vs, &vl) == 0) {
          svg_parse_stop_style_declarations(data + vs, vl, &color, &alpha);
        }
        if (svg_find_attr(data, tag_start, tag_end, "stop-opacity", &vs, &vl) == 0) {
          double opacity = 1.0;
          pos = vs;
          if (svg_parse_number(data, vs + vl, &pos, &opacity) == 0) {
            if (opacity < 0.0) opacity = 0.0;
            if (opacity > 1.0) opacity = 1.0;
            alpha = (uint8_t)(opacity * 255.0);
          }
        }
        svg_gradient_add_stop(&active_gradient, offset, color, alpha);
      }
      continue;
    }

    if (media_bytes_starts_with(data, tag_end, tag_start + 1, "svg") &&
        tag_start != svg_start) {
      size_t close_start = 0, close_end = 0;

      svg_render_referenced_element_internal(
          &ctx, data, size, tag_start, tag_end, &style_stack[sp],
          transform_stack[sp], viewport_w,
          viewport_h, viewport_diag, -1.0, -1.0, 0);
      if (svg_find_closing_tag(data, size, tag_end + 1, "svg", &close_start,
                               &close_end) == 0)
        i = close_end;
      continue;
    }

    if (media_bytes_starts_with(data, tag_end, tag_start + 1, "symbol")) {
      size_t close_start = 0, close_end = 0;

      if (svg_find_closing_tag(data, size, tag_end + 1, "symbol", &close_start,
                               &close_end) == 0)
        i = close_end;
      continue;
    }

    if (media_bytes_starts_with(data, tag_end, tag_start + 1, "g") ||
        media_bytes_starts_with(data, tag_end, tag_start + 1, "svg")) {
      if (sp + 1 < 16) {
        style_stack[sp + 1] = style_stack[sp];
        transform_stack[sp + 1] =
            svg_transform_multiply(transform_stack[sp],
                                   svg_parse_transform_attr(data, tag_start, tag_end));
        svg_apply_style_attrs(data, tag_start, tag_end, &style_stack[sp + 1]);
        sp++;
      }
      continue;
    }

    {
      svg_style_t style = style_stack[sp];
      svg_transform_t transform =
          svg_transform_multiply(transform_stack[sp],
                                 svg_parse_transform_attr(data, tag_start, tag_end));
      svg_apply_style_attrs(data, tag_start, tag_end, &style);

      if (media_bytes_starts_with(data, tag_end, tag_start + 1, "text")) {
        size_t consumed_end = tag_end;
        svg_render_text_element(&ctx, data, size, tag_start, tag_end,
                                &consumed_end, &style, transform);
        i = consumed_end;
        continue;
      }

      if (!svg_style_is_displayed(&style))
        continue;

      if (media_bytes_starts_with(data, tag_end, tag_start + 1, "use")) {
        size_t vs = 0, vl = 0;
        double use_x = 0.0, use_y = 0.0;
        double child_use_w = -1.0, child_use_h = -1.0;
        size_t ref_tag_start = 0, ref_tag_end = 0;
        svg_transform_t use_transform;

        if (!svg_style_is_visible(&style))
          continue;
        if (svg_find_attr(data, tag_start, tag_end, "x", &vs, &vl) == 0)
          svg_parse_length_number(data, vs, vl, viewport_w, &use_x);
        if (svg_find_attr(data, tag_start, tag_end, "y", &vs, &vl) == 0)
          svg_parse_length_number(data, vs, vl, viewport_h, &use_y);
        if (svg_find_attr(data, tag_start, tag_end, "width", &vs, &vl) == 0)
          svg_parse_length_number(data, vs, vl, viewport_w, &child_use_w);
        if (svg_find_attr(data, tag_start, tag_end, "height", &vs, &vl) == 0)
          svg_parse_length_number(data, vs, vl, viewport_h, &child_use_h);
        if ((svg_find_attr(data, tag_start, tag_end, "href", &vs, &vl) == 0 ||
             svg_find_attr(data, tag_start, tag_end, "xlink:href", &vs, &vl) == 0) &&
            vl > 1 && data[vs] == '#') {
          char ref_id[32];
          svg_copy_ref(ref_id, sizeof(ref_id), data + vs + 1, vl - 1);
          if (svg_find_element_by_id(data, size, ref_id, &ref_tag_start,
                                     &ref_tag_end) == 0) {
            use_transform = svg_transform_multiply(
                transform, svg_transform_translate(use_x, use_y));
            svg_render_referenced_element_internal(
                &ctx, data, size, ref_tag_start, ref_tag_end, &style,
                use_transform, viewport_w, viewport_h, viewport_diag,
                child_use_w, child_use_h, 0);
          }
        }
        continue;
      }

      if (media_bytes_starts_with(data, tag_end, tag_start + 1, "path")) {
        size_t vs = 0, vl = 0;
        svg_path_buffer_t buf = {0};
        if (!svg_style_is_visible(&style))
          continue;
        if (svg_find_attr(data, tag_start, tag_end, "d", &vs, &vl) == 0 &&
            svg_parse_path_data(data + vs, vl, &buf) == 0)
          svg_render_path_buffer(&ctx, &buf, &style, transform);
        svg_path_buffer_free(&buf);
      } else if (media_bytes_starts_with(data, tag_end, tag_start + 1, "polyline") ||
                 media_bytes_starts_with(data, tag_end, tag_start + 1, "polygon")) {
        size_t vs = 0, vl = 0;
        svg_path_buffer_t buf = {0};
        if (!svg_style_is_visible(&style))
          continue;
        if (svg_find_attr(data, tag_start, tag_end, "points", &vs, &vl) == 0) {
          svg_parse_points_list(data + vs, vl, &buf,
                                media_bytes_starts_with(data, tag_end, tag_start + 1,
                                                        "polygon"));
          svg_render_path_buffer(&ctx, &buf, &style, transform);
        }
        svg_path_buffer_free(&buf);
      } else if (media_bytes_starts_with(data, tag_end, tag_start + 1, "line")) {
        double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
        size_t vs = 0, vl = 0, pos;
        if (!svg_style_is_visible(&style))
          continue;
        if (svg_find_attr(data, tag_start, tag_end, "x1", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_w, &x1);
        }
        if (svg_find_attr(data, tag_start, tag_end, "y1", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_h, &y1);
        }
        if (svg_find_attr(data, tag_start, tag_end, "x2", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_w, &x2);
        }
        if (svg_find_attr(data, tag_start, tag_end, "y2", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_h, &y2);
        }
        {
          svg_point_t a = svg_transform_point(transform, (svg_point_t){x1, y1});
          svg_point_t b = svg_transform_point(transform, (svg_point_t){x2, y2});
          if (style.stroke_dash_count > 0) {
            int dash_index = 0;
            double dash_remaining = 0.0;
            int dash_draw = 1;
            svg_dash_init(&style, &dash_index, &dash_remaining, &dash_draw);
            svg_stroke_segment_dashed(&ctx, a, b, &style, transform,
                                      svg_min(x1, x2), svg_min(y1, y2),
                                      svg_absd(x2 - x1), svg_absd(y2 - y1),
                                      &dash_index, &dash_remaining, &dash_draw);
          } else {
            svg_stroke_segment(&ctx, a, b, &style, transform, 1, 1,
                               svg_min(x1, x2), svg_min(y1, y2),
                               svg_absd(x2 - x1), svg_absd(y2 - y1));
          }
        }
      } else if (media_bytes_starts_with(data, tag_end, tag_start + 1, "rect")) {
        double x = 0.0, y = 0.0, w = 0.0, h = 0.0, rx = 0.0, ry = 0.0;
        size_t vs = 0, vl = 0;
        svg_path_buffer_t buf = {0};
        if (!svg_style_is_visible(&style))
          continue;
        if (svg_find_attr(data, tag_start, tag_end, "x", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_w, &x);
        }
        if (svg_find_attr(data, tag_start, tag_end, "y", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_h, &y);
        }
        if (svg_find_attr(data, tag_start, tag_end, "width", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_w, &w);
        }
        if (svg_find_attr(data, tag_start, tag_end, "height", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_h, &h);
        }
        if (svg_find_attr(data, tag_start, tag_end, "rx", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_w, &rx);
        }
        if (svg_find_attr(data, tag_start, tag_end, "ry", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_h, &ry);
        }
        if (rx > 0.0 && ry <= 0.0)
          ry = rx;
        if (ry > 0.0 && rx <= 0.0)
          rx = ry;
        rx = svg_clamp(rx, 0.0, w * 0.5);
        ry = svg_clamp(ry, 0.0, h * 0.5);
        if (rx > 0.0 && ry > 0.0) {
          svg_append_rounded_rect(&buf, x, y, w, h, rx, ry);
        } else {
          svg_path_buffer_append(&buf, (svg_point_t){x, y}, 1, 0);
          svg_path_buffer_append(&buf, (svg_point_t){x + w, y}, 0, 0);
          svg_path_buffer_append(&buf, (svg_point_t){x + w, y + h}, 0, 0);
          svg_path_buffer_append(&buf, (svg_point_t){x, y + h}, 0, 1);
        }
        svg_render_path_buffer(&ctx, &buf, &style, transform);
        svg_path_buffer_free(&buf);
      } else if (media_bytes_starts_with(data, tag_end, tag_start + 1, "circle")) {
        double cx = 0.0, cy = 0.0, r = 0.0;
        size_t vs = 0, vl = 0;
        svg_path_buffer_t buf = {0};
        if (!svg_style_is_visible(&style))
          continue;
        if (svg_find_attr(data, tag_start, tag_end, "cx", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_w, &cx);
        }
        if (svg_find_attr(data, tag_start, tag_end, "cy", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_h, &cy);
        }
        if (svg_find_attr(data, tag_start, tag_end, "r", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_diag, &r);
        }
        if (r > 0.0) {
          svg_append_ellipse_path(&buf, cx, cy, r, r);
          svg_render_path_buffer(&ctx, &buf, &style, transform);
        }
        svg_path_buffer_free(&buf);
      } else if (media_bytes_starts_with(data, tag_end, tag_start + 1, "ellipse")) {
        double cx = 0.0, cy = 0.0, rx = 0.0, ry = 0.0;
        size_t vs = 0, vl = 0;
        svg_path_buffer_t buf = {0};
        if (!svg_style_is_visible(&style))
          continue;
        if (svg_find_attr(data, tag_start, tag_end, "cx", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_w, &cx);
        }
        if (svg_find_attr(data, tag_start, tag_end, "cy", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_h, &cy);
        }
        if (svg_find_attr(data, tag_start, tag_end, "rx", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_w, &rx);
        }
        if (svg_find_attr(data, tag_start, tag_end, "ry", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_h, &ry);
        }
        if (rx > 0.0 && ry > 0.0) {
          svg_append_ellipse_path(&buf, cx, cy, rx, ry);
          svg_render_path_buffer(&ctx, &buf, &style, transform);
        }
        svg_path_buffer_free(&buf);
      } else if (media_bytes_starts_with(data, tag_end, tag_start + 1, "image")) {
        size_t vs = 0, vl = 0;
        double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
        double draw_x, draw_y, draw_w, draw_h;
        double src_x = 0.0, src_y = 0.0;
        double src_w, src_h;
        int align_x = 1, align_y = 1, meet_mode = 1, preserve_none = 0;
        media_image_t embedded = {0};
        if (!svg_style_is_visible(&style))
          continue;
        if (svg_find_attr(data, tag_start, tag_end, "x", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_w, &x);
        }
        if (svg_find_attr(data, tag_start, tag_end, "y", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_h, &y);
        }
        if (svg_find_attr(data, tag_start, tag_end, "width", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_w, &w);
        }
        if (svg_find_attr(data, tag_start, tag_end, "height", &vs, &vl) == 0) {
          svg_parse_length_number(data, vs, vl, viewport_h, &h);
        }
        if ((svg_find_attr(data, tag_start, tag_end, "href", &vs, &vl) == 0 ||
             svg_find_attr(data, tag_start, tag_end, "xlink:href", &vs, &vl) == 0) &&
            svg_decode_data_uri_image(data + vs, vl, &embedded) == 0) {
          svg_point_t p = svg_transform_point(transform, (svg_point_t){x, y});
          draw_x = p.x;
          draw_y = p.y;
          draw_w = w > 0.0 ? w * transform.a : (double)embedded.width;
          draw_h = h > 0.0 ? h * transform.d : (double)embedded.height;
          src_w = (double)embedded.width;
          src_h = (double)embedded.height;
          svg_parse_preserve_aspect_ratio(data, tag_start, tag_end, &align_x,
                                          &align_y, &meet_mode, &preserve_none);
          if (!preserve_none && draw_w > 0.0 && draw_h > 0.0 && src_w > 0.0 &&
              src_h > 0.0) {
            double scale_x = draw_w / src_w;
            double scale_y = draw_h / src_h;
            if (meet_mode) {
              double scale = svg_min(scale_x, scale_y);
              double fit_w = src_w * scale;
              double fit_h = src_h * scale;
              double extra_x = draw_w - fit_w;
              double extra_y = draw_h - fit_h;
              draw_w = fit_w;
              draw_h = fit_h;
              if (align_x == 1)
                draw_x += extra_x * 0.5;
              else if (align_x == 2)
                draw_x += extra_x;
              if (align_y == 1)
                draw_y += extra_y * 0.5;
              else if (align_y == 2)
                draw_y += extra_y;
            } else {
              double scale = svg_max(scale_x, scale_y);
              double crop_w = draw_w / scale;
              double crop_h = draw_h / scale;
              double extra_x = src_w - crop_w;
              double extra_y = src_h - crop_h;
              src_w = crop_w;
              src_h = crop_h;
              if (align_x == 1)
                src_x += extra_x * 0.5;
              else if (align_x == 2)
                src_x += extra_x;
              if (align_y == 1)
                src_y += extra_y * 0.5;
              else if (align_y == 2)
                src_y += extra_y;
            }
          }
          if (draw_w < 0.0) {
            draw_x += draw_w;
            draw_w = -draw_w;
          }
          if (draw_h < 0.0) {
            draw_y += draw_h;
            draw_h = -draw_h;
          }
          svg_blit_image_region(&ctx, &embedded, draw_x, draw_y, draw_w, draw_h,
                                src_x, src_y, src_w, src_h, style.opacity);
          media_free_image(&embedded);
        }
      }
    }
  }

  *out = ctx.image;
  return 0;
}

static int media_decode_svg_data_uri_fallback(const uint8_t *data, size_t size,
                                              media_image_t *out) {
  static const char *k_svg_png = "data:image/png;base64,";
  static const char *k_svg_jpg = "data:image/jpeg;base64,";
  static const char *k_svg_jpg_alt = "data:image/jpg;base64,";
  const char *uris[3] = {k_svg_png, k_svg_jpg, k_svg_jpg_alt};
  media_image_t best = {0, 0, NULL};
  int found_any = 0;

  if (!data || !size || !out)
    return -EINVAL;

  for (int u = 0; u < 3; u++) {
    const char *uri = uris[u];
    size_t pos = 0;
    size_t uri_len = 0;

    while (uri[uri_len])
      uri_len++;

    while (media_find_bytes_from(data, size, pos, uri, &pos) == 0) {
      size_t base64_start = pos + uri_len;
      size_t base64_end = base64_start;
      uint8_t *decoded = NULL;
      size_t decoded_len = 0;
      media_image_t candidate = {0, 0, NULL};
      int ret;

      if (base64_start >= size)
        break;

      while (base64_end < size) {
        uint8_t ch = data[base64_end];
        if (ch == '"' || ch == '\'' || ch == '<' || ch == '>')
          break;
        base64_end++;
      }

      if (base64_end > base64_start) {
        ret = media_decode_base64(data + base64_start, base64_end - base64_start,
                                  &decoded, &decoded_len);
        if (ret == 0) {
          if (uri == k_svg_png)
            ret = media_decode_png(decoded, decoded_len, &candidate);
          else
            ret = media_decode_jpeg(decoded, decoded_len, &candidate);
        }
      } else {
        ret = -EINVAL;
      }

      if (decoded)
        kfree(decoded);

      if (ret == 0 && candidate.pixels && candidate.width && candidate.height) {
        uint64_t cand_px = (uint64_t)candidate.width * (uint64_t)candidate.height;
        uint64_t best_px = (uint64_t)best.width * (uint64_t)best.height;
        found_any = 1;
        if (cand_px > best_px) {
          media_free_image(&best);
          best = candidate;
        } else {
          media_free_image(&candidate);
        }
      } else {
        media_free_image(&candidate);
      }

      pos++;
      if (pos >= size)
        break;
    }
  }

  if (!found_any || !best.pixels)
    return -ENOENT;

  *out = best;
  return 0;
}

int media_decode_svg(const uint8_t *data, size_t size, media_image_t *out) {
  int ret;
  if (!out)
    return -EINVAL;
  out->width = 0;
  out->height = 0;
  out->pixels = NULL;

  ret = media_decode_svg_vector(data, size, out);
  if (ret == 0 && out->pixels && out->width && out->height)
    return 0;
  media_free_image(out);
  return media_decode_svg_data_uri_fallback(data, size, out);
}

/* --------------------------------------------------------------------- */
/* MP3 decoding (minimp3)                                                 */
/* --------------------------------------------------------------------- */

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#define MINIMP3_NO_SIMD
#define malloc(sz) kmalloc((sz))
#define free(ptr) kfree((ptr))
#define realloc(ptr, sz) krealloc((ptr), (sz), 0)
#include "minimp3_ex.h"
#undef malloc
#undef free
#undef realloc

int media_decode_mp3(const uint8_t *data, size_t size, media_audio_t *out) {
  if (!data || !size || !out)
    return -EINVAL;

  mp3dec_t dec;
  mp3dec_file_info_t info;
  int ret = mp3dec_load_buf(&dec, data, size, &info, NULL, NULL);
  if (ret < 0 || !info.buffer || !info.samples) {
    return -EINVAL;
  }

  out->samples = (int16_t *)info.buffer;
  out->sample_count =
      (uint32_t)(info.samples / (info.channels ? info.channels : 1));
  out->sample_rate = (uint32_t)info.hz;
  out->channels = (uint8_t)info.channels;
  return 0;
}

void media_free_audio(media_audio_t *audio) {
  if (!audio)
    return;
  if (audio->samples) {
    kfree(audio->samples);
    audio->samples = NULL;
  }
  audio->sample_count = 0;
  audio->sample_rate = 0;
  audio->channels = 0;
}

/* --------------------------------------------------------------------- */
/* PNG decoding (tPNG)                                                    */
/* --------------------------------------------------------------------- */

#include "tpng.h"

int media_decode_png(const uint8_t *data, size_t size, media_image_t *out) {
  if (!data || !size || !out)
    return -EINVAL;

  uint32_t width = 0, height = 0;
  uint8_t *rgba = tpng_decode(data, (uint32_t)size, &width, &height);

  if (!rgba || width == 0 || height == 0) {
    printk(KERN_ERR "PNG: decode failed\n");
    if (rgba)
      kfree(rgba);
    return -EINVAL;
  }

  /* Check for excessively large images (16M pixels max, same as JPEG) */
  size_t pixel_count = (size_t)width * (size_t)height;
  if (pixel_count > 16 * 1024 * 1024) {
    printk(KERN_ERR "PNG: image too large (%zu pixels)\n", pixel_count);
    kfree(rgba);
    return -EINVAL;
  }

  /* Convert RGBA (uint8_t*) to 0xAARRGGBB so PNG transparency is preserved. */
  uint32_t *pixels =
      (uint32_t *)kmalloc(pixel_count * sizeof(uint32_t), GFP_KERNEL);
  if (!pixels) {
    kfree(rgba);
    return -ENOMEM;
  }

  for (size_t i = 0; i < pixel_count; i++) {
    uint8_t r = rgba[i * 4 + 0];
    uint8_t g = rgba[i * 4 + 1];
    uint8_t b = rgba[i * 4 + 2];
    uint8_t a = rgba[i * 4 + 3];
    pixels[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                ((uint32_t)g << 8) | b;
  }

  kfree(rgba);

  out->width = width;
  out->height = height;
  out->pixels = pixels;
  return 0;
}
