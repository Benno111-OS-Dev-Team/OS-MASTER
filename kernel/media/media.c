/*
 * OS8 - Media helpers (JPEG/MP3 decoding)
 */

#include "media/media.h"
#include "fs/vfs.h"
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

int media_decode_svg(const uint8_t *data, size_t size, media_image_t *out) {
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
