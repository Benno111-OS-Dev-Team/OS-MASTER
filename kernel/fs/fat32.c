/*
 * OS8 - FAT32 Filesystem Implementation
 */

#include "drivers/storage.h"
#include "fs/fat32.h"
#include "fs/vfs.h"
#include "mm/kmalloc.h"
#include "printk.h"
#include "string.h"

/* In-memory Superblock Info */
struct fat32_sb_info {
  int disk_index;
  uint32_t fat_start_sector;
  uint32_t data_start_sector;
  uint32_t sectors_per_cluster;
  uint32_t bytes_per_sector;
  uint32_t bytes_per_cluster;
  uint32_t root_cluster;
  uint32_t fat_size; // in sectors
};

/* In-memory Inode Info */
struct fat32_inode_info {
  uint32_t first_cluster;
};

/* Forward declarations */
static struct file_operations fat32_file_ops;
static struct inode_operations fat32_dir_ops;

/* ===================================================================== */
/* Helper Functions */
/* ===================================================================== */

static uint32_t cluster_to_sector(struct fat32_sb_info *sbi, uint32_t cluster) {
  return sbi->data_start_sector + ((cluster - 2) * sbi->sectors_per_cluster);
}

static int fat32_read_sector(struct fat32_sb_info *sbi, uint32_t sector,
                             void *buf) {
  if (!sbi || !buf || sbi->disk_index < 0)
    return -EINVAL;
  return storage_read_block(sbi->disk_index, sector, buf,
                            sbi->bytes_per_sector);
}

static int fat32_parse_bpb(struct fat32_sb_info *sbi,
                           const struct fat32_bpb *bpb) {
  uint32_t total_sectors;

  if (!sbi || !bpb)
    return -EINVAL;
  if (bpb->bytes_per_sector != 512 || bpb->sectors_per_cluster == 0 ||
      bpb->reserved_sectors == 0 || bpb->num_fats == 0 ||
      bpb->fat_size_32 == 0 || bpb->root_cluster < 2) {
    return -EINVAL;
  }

  total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16
                                        : bpb->total_sectors_32;
  if (total_sectors == 0)
    return -EINVAL;

  sbi->bytes_per_sector = bpb->bytes_per_sector;
  sbi->sectors_per_cluster = bpb->sectors_per_cluster;
  sbi->bytes_per_cluster = sbi->bytes_per_sector * sbi->sectors_per_cluster;
  sbi->fat_size = bpb->fat_size_32;
  sbi->fat_start_sector = bpb->reserved_sectors;
  sbi->data_start_sector =
      bpb->reserved_sectors + ((uint32_t)bpb->num_fats * bpb->fat_size_32);
  sbi->root_cluster = bpb->root_cluster;

  if (sbi->data_start_sector >= total_sectors)
    return -EINVAL;
  return 0;
}

/* ===================================================================== */
/* File Operations */
/* ===================================================================== */

static ssize_t fat32_read(struct file *file, char *buf, size_t count,
                          loff_t *pos) {
  (void)file;
  (void)buf;
  (void)count;
  (void)pos;
  // Unsupported
  return 0;
}

static struct file_operations fat32_file_ops = {
    .read = fat32_read,
    // .write = fat32_write
};

/* ===================================================================== */
/* Inode Operations */
/* ===================================================================== */

static struct dentry *fat32_lookup(struct inode *dir, struct dentry *dentry) {
  (void)dir;
  (void)dentry;
  // Unsupported: Lookup name in directory
  return NULL;
}

static struct inode_operations fat32_dir_ops = {
    .lookup = fat32_lookup,
    // .create = fat32_create,
    // .mkdir = fat32_mkdir
};

/* ===================================================================== */
/* Superblock / Mount */
/* ===================================================================== */

static struct super_block *fat32_mount(struct file_system_type *fs_type,
                                       int flags, const char *dev_name,
                                       void *data) {
  (void)fs_type;
  (void)flags;
  (void)dev_name;
  (void)data;
  struct super_block *sb = kzalloc(sizeof(struct super_block), GFP_KERNEL);
  if (!sb)
    return NULL;

  struct fat32_sb_info *sbi = kzalloc(sizeof(struct fat32_sb_info), GFP_KERNEL);
  if (!sbi) {
    kfree(sb);
    return NULL;
  }
  sb->s_fs_info = sbi;
  sb->s_type = fs_type;
  sb->s_disk_index = storage_get_disk_index_by_location(dev_name);
  sbi->disk_index = sb->s_disk_index;
  sbi->bytes_per_sector = 512;
  if (sbi->disk_index < 0) {
    printk(KERN_ERR "FAT32: unknown block device '%s'\n",
           dev_name ? dev_name : "(null)");
    kfree(sbi);
    kfree(sb);
    return NULL;
  }

  // Read Boot Sector (Sector 0)
  struct fat32_bpb *bpb = kzalloc(512, GFP_KERNEL);
  if (!bpb) {
    kfree(sbi);
    kfree(sb);
    return NULL;
  }
  if (fat32_read_sector(sbi, 0, bpb) != 0 || fat32_parse_bpb(sbi, bpb) != 0) {
    printk(KERN_ERR "FAT32: invalid boot sector on '%s'\n",
           dev_name ? dev_name : "(null)");
    kfree(bpb);
    kfree(sbi);
    kfree(sb);
    return NULL;
  }

  // Setup root inode
  struct inode *root = kzalloc(sizeof(struct inode), GFP_KERNEL);
  if (!root) {
    kfree(bpb);
    kfree(sbi);
    kfree(sb);
    return NULL;
  }
  root->i_sb = sb;
  root->i_mode = S_IFDIR | 0755;
  root->i_op = &fat32_dir_ops;
  root->i_fop = &fat32_file_ops; // Dirs communicate via file ops nicely in
                                 // unix? Usually readdir.
  root->i_private = kzalloc(sizeof(struct fat32_inode_info), GFP_KERNEL);
  if (!root->i_private) {
    kfree(root);
    kfree(bpb);
    kfree(sbi);
    kfree(sb);
    return NULL;
  }
  ((struct fat32_inode_info *)root->i_private)->first_cluster =
      sbi->root_cluster;

  struct dentry *root_dentry = kzalloc(sizeof(struct dentry), GFP_KERNEL);
  if (!root_dentry) {
    kfree(root->i_private);
    kfree(root);
    kfree(bpb);
    kfree(sbi);
    kfree(sb);
    return NULL;
  }
  root_dentry->d_inode = root;
  root_dentry->d_sb = sb;
  root_dentry->d_parent = NULL; // Root

  sb->s_root = root_dentry;

  kfree(bpb);
  return sb;
}

static void fat32_kill_sb(struct super_block *sb) {
  if (!sb)
    return;

  if (sb->s_root) {
    if (sb->s_root->d_inode) {
      if (sb->s_root->d_inode->i_private)
        kfree(sb->s_root->d_inode->i_private);
      kfree(sb->s_root->d_inode);
    }
    kfree(sb->s_root);
  }
  if (sb->s_fs_info)
    kfree(sb->s_fs_info);
  kfree(sb);
}

struct file_system_type fat32_fs_type = {
    .name = "fat32", .mount = fat32_mount, .kill_sb = fat32_kill_sb, .next = NULL};
