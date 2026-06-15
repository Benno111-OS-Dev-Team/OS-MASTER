#include "drivers/storage.h"
#include "fs/iso9660.h"
#include "fs/vfs.h"
#include "mm/kmalloc.h"
#include "printk.h"

#define ISO_BLOCK_SIZE 2048

typedef struct iso_copy_ctx {
  int disk_index;
  const char *dst_root;
} iso_copy_ctx_t;

typedef struct iso_remove_ctx {
  char root[256];
  int failed;
} iso_remove_ctx_t;

typedef struct iso_volume_info {
  uint32_t primary_root_extent;
  uint32_t primary_root_size;
  uint32_t joliet_root_extent;
  uint32_t joliet_root_size;
  int has_primary;
  int has_joliet;
} iso_volume_info_t;

static uint32_t iso_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void iso_cooperative_yield(void) {
  extern void process_yield(void);
  process_yield();
}

static int iso_build_path(char *dst, int max, const char *root,
                          const char *name) {
  int idx = 0;
  if (!dst || max <= 0)
    return -1;
  if (root) {
    while (root[idx] && idx < max - 1) {
      dst[idx] = root[idx];
      idx++;
    }
  }
  if (idx > 0 && dst[idx - 1] != '/' && idx < max - 1)
    dst[idx++] = '/';
  for (int i = 0; name && name[i] && idx < max - 1; i++)
    dst[idx++] = name[i];
  dst[idx] = '\0';
  return 0;
}

static int iso_is_joliet_descriptor(const uint8_t *desc) {
  if (!desc)
    return 0;
  if (desc[0] != 2)
    return 0;
  if (desc[88] != 0x25 || desc[89] != 0x2F)
    return 0;
  return desc[90] == 0x40 || desc[90] == 0x43 || desc[90] == 0x45;
}

static void iso_trim_name(char *dst, int max, const uint8_t *src, int len,
                          int joliet) {
  int idx = 0;
  if (!dst || max <= 0)
    return;
  if (len == 1 && src[0] == 0) {
    dst[0] = '\0';
    return;
  }
  if (len == 1 && src[0] == 1) {
    dst[0] = '.';
    dst[1] = '.';
    dst[2] = '\0';
    return;
  }
  if (joliet) {
    for (int i = 0; i + 1 < len && idx < max - 1; i += 2) {
      uint8_t hi = src[i];
      uint8_t lo = src[i + 1];

      if (hi == 0 && lo == ';')
        break;
      if (hi == 0 && lo != 0) {
        dst[idx++] = (char)lo;
      } else if (hi == 0 && lo == 0) {
        break;
      } else {
        dst[idx++] = '_';
      }
    }
  } else {
    for (int i = 0; i < len && idx < max - 1; i++) {
      if (src[i] == ';')
        break;
      dst[idx++] = (char)src[i];
    }
  }
  if (idx > 0 && dst[idx - 1] == '.')
    idx--;
  dst[idx] = '\0';
}

static int iso_find_volume_info(int disk_index, iso_volume_info_t *info) {
  uint8_t *desc;
  int saw_primary = 0;

  if (!info)
    return -1;

  info->primary_root_extent = 0;
  info->primary_root_size = 0;
  info->joliet_root_extent = 0;
  info->joliet_root_size = 0;
  info->has_primary = 0;
  info->has_joliet = 0;

  desc = kmalloc(ISO_BLOCK_SIZE, GFP_KERNEL);
  if (!desc)
    return -1;

  for (uint32_t lba = 16; lba < 64; lba++) {
    if (storage_read_block(disk_index, lba, desc, ISO_BLOCK_SIZE) != 0) {
      kfree(desc);
      return -1;
    }
    if (desc[1] != 'C' || desc[2] != 'D' || desc[3] != '0' || desc[4] != '0' ||
        desc[5] != '1') {
      kfree(desc);
      return -1;
    }
    if (desc[0] == 1 && !saw_primary) {
      info->primary_root_extent = iso_le32(&desc[158]);
      info->primary_root_size = iso_le32(&desc[166]);
      info->has_primary = 1;
      saw_primary = 1;
    } else if (iso_is_joliet_descriptor(desc)) {
      info->joliet_root_extent = iso_le32(&desc[158]);
      info->joliet_root_size = iso_le32(&desc[166]);
      info->has_joliet = 1;
    } else if (desc[0] == 255) {
      kfree(desc);
      return saw_primary ? 0 : -1;
    }
    if ((lba & 3U) == 3U)
      iso_cooperative_yield();
  }

  kfree(desc);
  return saw_primary ? 0 : -1;
}

static void iso_ensure_parent_dirs(const char *path) {
  char partial[256];
  int idx = 0;

  if (!path)
    return;
  for (int i = 0; path[i] && idx < (int)sizeof(partial) - 1; i++) {
    partial[idx++] = path[i];
    partial[idx] = '\0';
    if (i > 0 && path[i] == '/')
      vfs_mkdir(partial, 0755);
  }
}

static int iso_remove_tree_callback(void *ctx, const char *name, int len,
                                    loff_t offset, ino_t ino, unsigned type) {
  iso_remove_ctx_t *rm = (iso_remove_ctx_t *)ctx;
  char path[256];
  struct file *dir;

  (void)offset;
  (void)ino;

  if (!rm || !name || len <= 0)
    return 0;
  if ((len == 1 && name[0] == '.') ||
      (len == 2 && name[0] == '.' && name[1] == '.'))
    return 0;
  if (iso_build_path(path, sizeof(path), rm->root, name) != 0)
    return 0;

  if (type == 4) {
    iso_remove_ctx_t child = {{0}, 0};
    int i = 0;
    while (path[i] && i < (int)sizeof(child.root) - 1) {
      child.root[i] = path[i];
      i++;
    }
    child.root[i] = '\0';
    dir = vfs_open(path, O_RDONLY, 0);
    if (dir) {
      vfs_readdir(dir, &child, iso_remove_tree_callback);
      vfs_close(dir);
    }
    if (child.failed)
      rm->failed = child.failed;
    if (vfs_rmdir(path) != 0)
      rm->failed = 1;
    return 0;
  }

  if (vfs_unlink(path) != 0)
    rm->failed = 1;
  return 0;
}

static int iso_clear_destination(const char *dst_root) {
  struct file *dir;
  iso_remove_ctx_t ctx = {{0}, 0};
  int i = 0;

  if (!dst_root)
    return -1;
  while (dst_root[i] && i < (int)sizeof(ctx.root) - 1) {
    ctx.root[i] = dst_root[i];
    i++;
  }
  ctx.root[i] = '\0';

  dir = vfs_open(dst_root, O_RDONLY, 0);
  if (!dir) {
    vfs_mkdir(dst_root, 0755);
    return 0;
  }
  vfs_readdir(dir, &ctx, iso_remove_tree_callback);
  vfs_close(dir);
  return ctx.failed ? -1 : 0;
}

static int iso_copy_file_from_extent(int disk_index, uint32_t extent,
                                     uint32_t file_size, const char *dst_path) {
  uint8_t *sector;
  struct file *f;
  uint32_t remaining = file_size;
  uint32_t lba = extent;

  sector = kmalloc(ISO_BLOCK_SIZE, GFP_KERNEL);
  if (!sector)
    return -1;

  iso_ensure_parent_dirs(dst_path);
  vfs_unlink(dst_path);
  f = vfs_open(dst_path, O_CREAT | O_WRONLY, 0644);
  if (!f) {
    kfree(sector);
    return -1;
  }

  while (remaining > 0) {
    uint32_t chunk = remaining > ISO_BLOCK_SIZE ? ISO_BLOCK_SIZE : remaining;
    ssize_t written;

    if (storage_read_block(disk_index, lba++, sector, ISO_BLOCK_SIZE) != 0) {
      vfs_close(f);
      kfree(sector);
      return -1;
    }

    written = vfs_write(f, (const char *)sector, chunk);
    if (written != (ssize_t)chunk) {
      vfs_close(f);
      kfree(sector);
      return -1;
    }

    remaining -= chunk;
    if ((lba & 3U) == 0U)
      iso_cooperative_yield();
  }

  vfs_close(f);
  kfree(sector);
  return 0;
}

static int iso_copy_dir_recursive(int disk_index, uint32_t extent, uint32_t size,
                                  const char *dst_root, int joliet) {
  uint8_t *dir_data;
  uint8_t *sector;
  uint32_t sector_count = (size + ISO_BLOCK_SIZE - 1) / ISO_BLOCK_SIZE;
  uint32_t total_size = sector_count * ISO_BLOCK_SIZE;
  uint32_t pos = 0;
  int copied_entries = 0;

  if (sector_count > 0x10000U)
    return -1;
  if (sector_count != 0 && total_size / sector_count != ISO_BLOCK_SIZE)
    return -1;

  sector = kmalloc(ISO_BLOCK_SIZE, GFP_KERNEL);
  if (!sector)
    return -1;
  dir_data = kmalloc(total_size ? total_size : ISO_BLOCK_SIZE, GFP_KERNEL);
  if (!dir_data) {
    kfree(sector);
    return -1;
  }

  for (uint32_t i = 0; i < sector_count; i++) {
    if (storage_read_block(disk_index, extent + i, sector, ISO_BLOCK_SIZE) != 0) {
      kfree(sector);
      kfree(dir_data);
      return -1;
    }
    for (uint32_t j = 0; j < ISO_BLOCK_SIZE; j++)
      dir_data[i * ISO_BLOCK_SIZE + j] = sector[j];
    if ((i & 3U) == 3U)
      iso_cooperative_yield();
  }

  kfree(sector);

  while (pos < size) {
    uint8_t record_len = dir_data[pos];
    char name[NAME_MAX + 1];
    char child_path[256];
    uint32_t child_extent;
    uint32_t child_size;
    uint8_t flags;
    uint8_t name_len;

    if (record_len == 0) {
      pos = ((pos / ISO_BLOCK_SIZE) + 1) * ISO_BLOCK_SIZE;
      continue;
    }
    if (record_len < 34) {
      printk(KERN_WARNING
             "ISO9660: invalid record length %u at offset %u in '%s'\n",
             record_len, pos, dst_root ? dst_root : "/");
      pos = ((pos / ISO_BLOCK_SIZE) + 1) * ISO_BLOCK_SIZE;
      continue;
    }
    if (pos + record_len > total_size)
      break;
    name_len = dir_data[pos + 32];
    if ((uint32_t)(33 + name_len) > record_len) {
      printk(KERN_WARNING
             "ISO9660: invalid name length %u at offset %u in '%s'\n",
             name_len, pos, dst_root ? dst_root : "/");
      pos = ((pos / ISO_BLOCK_SIZE) + 1) * ISO_BLOCK_SIZE;
      continue;
    }

    child_extent = iso_le32(&dir_data[pos + 2]);
    child_size = iso_le32(&dir_data[pos + 10]);
    flags = dir_data[pos + 25];
    iso_trim_name(name, sizeof(name), &dir_data[pos + 33], name_len, joliet);

    if (name[0] != '\0' &&
        !(name[0] == '.' && name[1] == '\0') &&
        !(name[0] == '.' && name[1] == '.' && name[2] == '\0')) {
      iso_build_path(child_path, sizeof(child_path), dst_root, name);
      if (flags & 0x02) {
        vfs_mkdir(child_path, 0755);
        if (iso_copy_dir_recursive(disk_index, child_extent, child_size,
                                   child_path, joliet) < 0) {
          kfree(dir_data);
          return -1;
        }
        copied_entries++;
      } else {
        if (iso_copy_file_from_extent(disk_index, child_extent, child_size,
                                      child_path) != 0) {
          kfree(dir_data);
          return -1;
        }
        copied_entries++;
      }

      if ((copied_entries & 7) == 0)
        iso_cooperative_yield();
    }

    pos += record_len;
  }

  kfree(dir_data);
  return copied_entries;
}

int iso9660_copy_to_ramfs(const char *disk_location, const char *dst_root) {
  int disk_index;
  iso_volume_info_t volume_info;
  struct {
    uint32_t extent;
    uint32_t size;
    int joliet;
    int available;
  } attempts[2];
  int attempt_count = 0;

  if (!disk_location || !dst_root)
    return -1;

  disk_index = storage_get_disk_index_by_location(disk_location);
  if (disk_index < 0)
    return -1;
  if (iso_find_volume_info(disk_index, &volume_info) != 0)
    return -1;
  if (volume_info.has_joliet) {
    attempts[attempt_count].extent = volume_info.joliet_root_extent;
    attempts[attempt_count].size = volume_info.joliet_root_size;
    attempts[attempt_count].joliet = 1;
    attempts[attempt_count].available = 1;
    attempt_count++;
  }
  if (volume_info.has_primary) {
    attempts[attempt_count].extent = volume_info.primary_root_extent;
    attempts[attempt_count].size = volume_info.primary_root_size;
    attempts[attempt_count].joliet = 0;
    attempts[attempt_count].available = 1;
    attempt_count++;
  }

  for (int i = 0; i < attempt_count; i++) {
    int copied;

    if (!attempts[i].available)
      continue;
    if (iso_clear_destination(dst_root) != 0)
      return -1;
    vfs_mkdir(dst_root, 0755);
    printk(KERN_INFO "ISO9660: Copying '%s' to '%s' using %s catalog\n",
           disk_location, dst_root, attempts[i].joliet ? "Joliet" : "primary");
    copied = iso_copy_dir_recursive(disk_index, attempts[i].extent, attempts[i].size,
                                    dst_root, attempts[i].joliet);
    if (copied > 0)
      return 0;
  }

  return -1;
}

/* ===================================================================== */
/* Mountable ISO9660 tree */
/* ===================================================================== */

typedef struct iso9660_node {
  char name[NAME_MAX + 1];
  int disk_index;
  uint32_t extent;
  uint32_t size;
  int is_dir;
  uint8_t *data;
  struct iso9660_node *parent;
  struct iso9660_node *children;
  struct iso9660_node *next_sibling;
} iso9660_node_t;

static struct file_operations iso9660_dir_file_ops;
static struct file_operations iso9660_reg_file_ops;
static struct inode_operations iso9660_dir_inode_ops;

static iso9660_node_t *iso9660_node_alloc(const char *name, int is_dir) {
  iso9660_node_t *node = kmalloc(sizeof(*node), GFP_KERNEL);
  if (!node)
    return NULL;

  for (int i = 0; i < (int)sizeof(*node); i++)
    ((uint8_t *)node)[i] = 0;

  if (name) {
    int i = 0;
    while (name[i] && i < NAME_MAX) {
      node->name[i] = name[i];
      i++;
    }
    node->name[i] = '\0';
  }
  node->is_dir = is_dir ? 1 : 0;
  return node;
}

static void iso9660_node_add_child(iso9660_node_t *parent,
                                   iso9660_node_t *child) {
  if (!parent || !child)
    return;
  child->parent = parent;
  child->next_sibling = parent->children;
  parent->children = child;
}

static iso9660_node_t *iso9660_node_find_child(iso9660_node_t *parent,
                                               const char *name) {
  iso9660_node_t *child;

  if (!parent || !name)
    return NULL;

  child = parent->children;
  while (child) {
    int i = 0;
    while (child->name[i] && name[i] && child->name[i] == name[i])
      i++;
    if (child->name[i] == '\0' && name[i] == '\0')
      return child;
    child = child->next_sibling;
  }

  return NULL;
}

static void iso9660_node_free(iso9660_node_t *node) {
  iso9660_node_t *child;

  if (!node)
    return;

  child = node->children;
  while (child) {
    iso9660_node_t *next = child->next_sibling;
    iso9660_node_free(child);
    child = next;
  }

  if (node->data)
    kfree(node->data);
  kfree(node);
}

static int iso9660_copy_file_to_node(int disk_index, uint32_t extent,
                                     uint32_t file_size,
                                     iso9660_node_t *node) {
  if (!node)
    return -1;

  node->disk_index = disk_index;
  node->extent = extent;
  node->size = file_size;
  return 0;
}

static int iso9660_build_tree_recursive(int disk_index, uint32_t extent,
                                        uint32_t size, iso9660_node_t *parent,
                                        int joliet) {
  uint8_t *dir_data;
  uint8_t *sector;
  uint32_t sector_count = (size + ISO_BLOCK_SIZE - 1) / ISO_BLOCK_SIZE;
  uint32_t total_size = sector_count * ISO_BLOCK_SIZE;
  uint32_t pos = 0;

  if (!parent || !parent->is_dir)
    return -1;
  if (sector_count > 0x10000U)
    return -1;
  if (sector_count != 0 && total_size / sector_count != ISO_BLOCK_SIZE)
    return -1;

  sector = kmalloc(ISO_BLOCK_SIZE, GFP_KERNEL);
  if (!sector)
    return -1;

  dir_data = kmalloc(total_size ? total_size : ISO_BLOCK_SIZE, GFP_KERNEL);
  if (!dir_data) {
    kfree(sector);
    return -1;
  }

  for (uint32_t i = 0; i < sector_count; i++) {
    if (storage_read_block(disk_index, extent + i, sector, ISO_BLOCK_SIZE) != 0) {
      kfree(sector);
      kfree(dir_data);
      return -1;
    }
    for (uint32_t j = 0; j < ISO_BLOCK_SIZE; j++)
      dir_data[i * ISO_BLOCK_SIZE + j] = sector[j];
    if ((i & 3U) == 3U)
      iso_cooperative_yield();
  }

  kfree(sector);

  while (pos < size) {
    uint8_t record_len = dir_data[pos];
    char name[NAME_MAX + 1];
    uint32_t child_extent;
    uint32_t child_size;
    uint8_t flags;
    uint8_t name_len;

    if (record_len == 0) {
      pos = ((pos / ISO_BLOCK_SIZE) + 1) * ISO_BLOCK_SIZE;
      continue;
    }
    if (record_len < 34) {
      printk(KERN_WARNING
             "ISO9660: invalid record length %u at offset %u while mounting\n",
             record_len, pos);
      pos = ((pos / ISO_BLOCK_SIZE) + 1) * ISO_BLOCK_SIZE;
      continue;
    }
    if (pos + record_len > total_size)
      break;

    name_len = dir_data[pos + 32];
    if ((uint32_t)(33 + name_len) > record_len) {
      printk(KERN_WARNING
             "ISO9660: invalid name length %u at offset %u while mounting\n",
             name_len, pos);
      pos = ((pos / ISO_BLOCK_SIZE) + 1) * ISO_BLOCK_SIZE;
      continue;
    }

    child_extent = iso_le32(&dir_data[pos + 2]);
    child_size = iso_le32(&dir_data[pos + 10]);
    flags = dir_data[pos + 25];
    iso_trim_name(name, sizeof(name), &dir_data[pos + 33], name_len, joliet);

    if (name[0] != '\0' &&
        !(name[0] == '.' && name[1] == '\0') &&
        !(name[0] == '.' && name[1] == '.' && name[2] == '\0')) {
      iso9660_node_t *child = iso9660_node_alloc(name, (flags & 0x02) != 0);
      if (!child) {
        kfree(dir_data);
        return -1;
      }
      child->disk_index = disk_index;
      child->extent = child_extent;
      child->size = child_size;
      iso9660_node_add_child(parent, child);

      if (child->is_dir) {
        if (iso9660_build_tree_recursive(disk_index, child_extent, child_size,
                                         child, joliet) < 0) {
          kfree(dir_data);
          return -1;
        }
      } else if (iso9660_copy_file_to_node(disk_index, child_extent, child_size,
                                           child) != 0) {
        kfree(dir_data);
        return -1;
      }
    }

    pos += record_len;
  }

  kfree(dir_data);
  return 0;
}

static int iso9660_name_length(const char *name) {
  int len = 0;
  if (!name)
    return 0;
  while (name[len])
    len++;
  return len;
}

static void iso9660_fill_inode(struct inode *inode, struct super_block *sb,
                               iso9660_node_t *node) {
  if (!inode || !sb || !node)
    return;

  inode->i_sb = sb;
  inode->i_private = node;
  inode->i_ino = node->extent ? node->extent : 1;
  inode->i_mode = node->is_dir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
  inode->i_nlink = 1;
  inode->i_size = node->size;
  inode->i_blocks = 0;
  inode->i_blksize = ISO_BLOCK_SIZE;
  if (node->is_dir) {
    inode->i_op = &iso9660_dir_inode_ops;
    inode->i_fop = &iso9660_dir_file_ops;
  } else {
    inode->i_op = NULL;
    inode->i_fop = &iso9660_reg_file_ops;
  }
}

static ssize_t iso9660_read(struct file *file, char *buf, size_t count,
                            loff_t *pos) {
  iso9660_node_t *node;
  uint8_t *sector;
  size_t available;
  size_t to_copy;
  size_t total_read = 0;
  uint32_t file_offset;

  if (!file || !buf || !pos)
    return -EINVAL;

  node = (iso9660_node_t *)file->private_data;
  if (!node || node->is_dir)
    return -EISDIR;
  if (*pos < 0)
    return -EINVAL;
  if (node->size == 0)
    return 0;
  if (node->disk_index < 0)
    return -EIO;
  if ((uint32_t)*pos >= node->size)
    return 0;

  available = (size_t)(node->size - (uint32_t)*pos);
  to_copy = count < available ? count : available;

  sector = kmalloc(ISO_BLOCK_SIZE, GFP_KERNEL);
  if (!sector)
    return -ENOMEM;

  file_offset = (uint32_t)*pos;
  while (total_read < to_copy) {
    uint32_t block_index = file_offset / ISO_BLOCK_SIZE;
    uint32_t block_offset = file_offset % ISO_BLOCK_SIZE;
    uint32_t chunk = (uint32_t)(to_copy - total_read);

    if (chunk > ISO_BLOCK_SIZE - block_offset)
      chunk = ISO_BLOCK_SIZE - block_offset;

    if (storage_read_block(node->disk_index, node->extent + block_index, sector,
                           ISO_BLOCK_SIZE) != 0) {
      kfree(sector);
      return total_read > 0 ? (ssize_t)total_read : -EIO;
    }

    for (uint32_t i = 0; i < chunk; i++)
      buf[total_read + i] = (char)sector[block_offset + i];

    total_read += chunk;
    file_offset += chunk;
    if (((node->extent + block_index) & 3U) == 3U)
      iso_cooperative_yield();
  }

  kfree(sector);
  *pos += (loff_t)to_copy;
  return (ssize_t)to_copy;
}

static int iso9660_readdir(struct file *file, void *ctx,
                           int (*filldir)(void *, const char *, int, loff_t,
                                          ino_t, unsigned)) {
  iso9660_node_t *node;
  iso9660_node_t *child;

  if (!file || !filldir)
    return -EINVAL;

  node = (iso9660_node_t *)file->private_data;
  if (!node || !node->is_dir)
    return -ENOTDIR;

  filldir(ctx, ".", 1, 0, 0, 4);
  filldir(ctx, "..", 2, 0, 0, 4);
  child = node->children;
  while (child) {
    filldir(ctx, child->name, iso9660_name_length(child->name), 0, child->extent,
            child->is_dir ? 4 : 8);
    child = child->next_sibling;
  }
  return 0;
}

static struct dentry *iso9660_lookup(struct inode *dir, struct dentry *dentry) {
  iso9660_node_t *parent;
  iso9660_node_t *child;
  struct inode *inode;

  if (!dir || !dentry || !dir->i_private)
    return NULL;

  parent = (iso9660_node_t *)dir->i_private;
  child = iso9660_node_find_child(parent, dentry->d_name);
  if (!child)
    return NULL;

  inode = kmalloc(sizeof(*inode), GFP_KERNEL);
  if (!inode)
    return NULL;
  for (int i = 0; i < (int)sizeof(*inode); i++)
    ((uint8_t *)inode)[i] = 0;

  iso9660_fill_inode(inode, dir->i_sb, child);
  dentry->d_inode = inode;
  dentry->d_sb = dir->i_sb;
  return dentry;
}

static struct super_block *iso9660_mount(struct file_system_type *fs_type,
                                         int flags, const char *dev_name,
                                         void *data) {
  int disk_index;
  iso_volume_info_t volume_info;
  struct super_block *sb;
  struct inode *root_inode;
  struct dentry *root_dentry;
  iso9660_node_t *root_node = NULL;

  (void)flags;
  (void)data;

  if (!dev_name || dev_name[0] == '\0')
    return NULL;

  disk_index = storage_get_disk_index_by_location(dev_name);
  if (disk_index < 0)
    return NULL;
  if (iso_find_volume_info(disk_index, &volume_info) != 0)
    return NULL;

  if (volume_info.has_joliet)
    root_node = iso9660_node_alloc("", 1);
  if (!root_node && volume_info.has_primary)
    root_node = iso9660_node_alloc("", 1);
  if (!root_node)
    return NULL;
  root_node->disk_index = disk_index;

  if (volume_info.has_joliet) {
    if (iso9660_build_tree_recursive(disk_index, volume_info.joliet_root_extent,
                                     volume_info.joliet_root_size, root_node,
                                     1) == 0) {
      goto built_tree;
    }
    iso9660_node_free(root_node);
    root_node = iso9660_node_alloc("", 1);
    if (!root_node)
      return NULL;
    root_node->disk_index = disk_index;
  }

  if (volume_info.has_primary) {
    if (iso9660_build_tree_recursive(disk_index, volume_info.primary_root_extent,
                                     volume_info.primary_root_size, root_node,
                                     0) == 0) {
      goto built_tree;
    }
  }

  iso9660_node_free(root_node);
  return NULL;

built_tree:
  sb = kmalloc(sizeof(*sb), GFP_KERNEL);
  if (!sb) {
    iso9660_node_free(root_node);
    return NULL;
  }
  root_inode = kmalloc(sizeof(*root_inode), GFP_KERNEL);
  if (!root_inode) {
    kfree(sb);
    iso9660_node_free(root_node);
    return NULL;
  }
  root_dentry = kmalloc(sizeof(*root_dentry), GFP_KERNEL);
  if (!root_dentry) {
    kfree(root_inode);
    kfree(sb);
    iso9660_node_free(root_node);
    return NULL;
  }

  for (int i = 0; i < (int)sizeof(*sb); i++)
    ((uint8_t *)sb)[i] = 0;
  for (int i = 0; i < (int)sizeof(*root_inode); i++)
    ((uint8_t *)root_inode)[i] = 0;
  for (int i = 0; i < (int)sizeof(*root_dentry); i++)
    ((uint8_t *)root_dentry)[i] = 0;

  sb->s_blocksize = ISO_BLOCK_SIZE;
  sb->s_type = fs_type;
  sb->s_disk_index = disk_index;
  sb->s_fs_info = root_node;

  iso9660_fill_inode(root_inode, sb, root_node);
  root_dentry->d_name[0] = '/';
  root_dentry->d_name[1] = '\0';
  root_dentry->d_inode = root_inode;
  root_dentry->d_parent = root_dentry;
  root_dentry->d_child = NULL;
  root_dentry->d_sibling = NULL;
  root_dentry->d_sb = sb;

  sb->s_root = root_dentry;
  return sb;
}

static void iso9660_kill_sb(struct super_block *sb) {
  if (!sb)
    return;
  if (sb->s_fs_info)
    iso9660_node_free((iso9660_node_t *)sb->s_fs_info);
  if (sb->s_root) {
    if (sb->s_root->d_inode)
      kfree(sb->s_root->d_inode);
    kfree(sb->s_root);
  }
  kfree(sb);
}

static struct file_operations iso9660_dir_file_ops = {
    .readdir = iso9660_readdir,
};

static struct file_operations iso9660_reg_file_ops = {
    .read = iso9660_read,
};

static struct inode_operations iso9660_dir_inode_ops = {
    .lookup = iso9660_lookup,
};

static struct file_system_type iso9660_fs_type = {
    .name = "iso9660",
    .fs_flags = 0,
    .mount = iso9660_mount,
    .kill_sb = iso9660_kill_sb,
    .next = NULL,
};

int iso9660_init(void) { return register_filesystem(&iso9660_fs_type); }
