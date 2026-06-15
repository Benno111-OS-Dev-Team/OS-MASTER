/*
 * UnixOS Kernel - Ramfs (RAM Filesystem)
 *
 * Simple in-memory filesystem for initial root filesystem.
 */

#include "fs/vfs.h"
#include "mm/kmalloc.h"
#include "printk.h"

/* ===================================================================== */
/* Ramfs structures */
/* ===================================================================== */

#define RAMFS_MAX_NAME 255
#define RAMFS_MAX_FILES 1024
#define RAMFS_BLOCK_SIZE 4096

struct ramfs_inode {
  ino_t ino;
  mode_t mode;
  uid_t uid;
  gid_t gid;
  size_t size;
  uint8_t *data;        /* File data */
  size_t data_capacity; /* Allocated size */
  struct ramfs_inode *parent;
  struct ramfs_inode *children; /* First child (for directories) */
  struct ramfs_inode *sibling;  /* Next sibling */
  char name[RAMFS_MAX_NAME + 1];
};

struct ramfs_sb_info {
  struct ramfs_inode *root;
  ino_t next_ino;
  size_t inode_count;
};

/* ===================================================================== */
/* Static data */
/* ===================================================================== */

static struct ramfs_sb_info ramfs_sb;

/* ===================================================================== */
/* Inode operations */
/* ===================================================================== */

static struct ramfs_inode *ramfs_alloc_inode(mode_t mode, const char *name) {
  struct ramfs_inode *inode = kzalloc(sizeof(struct ramfs_inode), GFP_KERNEL);
  if (!inode) {
    return NULL;
  }

  inode->ino = ramfs_sb.next_ino++;
  inode->mode = mode;
  inode->uid = 0;
  inode->gid = 0;
  inode->size = 0;
  inode->data = NULL;
  inode->data_capacity = 0;
  inode->parent = NULL;
  inode->children = NULL;
  inode->sibling = NULL;

  /* Copy name */
  int i;
  for (i = 0; i < RAMFS_MAX_NAME && name[i]; i++) {
    inode->name[i] = name[i];
  }
  inode->name[i] = '\0';

  ramfs_sb.inode_count++;

  return inode;
}

static void ramfs_free_inode(struct ramfs_inode *inode) {
  if (!inode)
    return;

  if (inode->data) {
    kfree(inode->data);
  }

  ramfs_sb.inode_count--;
  kfree(inode);
}

static struct ramfs_inode *ramfs_lookup_child(struct ramfs_inode *dir,
                                              const char *name) {
  if (!S_ISDIR(dir->mode)) {
    return NULL;
  }

  struct ramfs_inode *child = dir->children;
  while (child) {
    /* Compare names */
    const char *a = child->name;
    const char *b = name;
    while (*a && *b && *a == *b) {
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0') {
      return child;
    }
    child = child->sibling;
  }

  return NULL;
}

static int ramfs_add_child(struct ramfs_inode *dir, struct ramfs_inode *child) {
  if (!S_ISDIR(dir->mode)) {
    return -ENOTDIR;
  }

  child->parent = dir;
  child->sibling = dir->children;
  dir->children = child;

  return 0;
}

/* ===================================================================== */
/* File operations */
/* ===================================================================== */

static ssize_t ramfs_read(struct file *file, char *buf, size_t count,
                          loff_t *pos) {
  /* Get ramfs inode from file */
  struct ramfs_inode *inode = (struct ramfs_inode *)file->private_data;

  if (!inode || !inode->data) {
    return 0;
  }

  if (*pos >= (loff_t)inode->size) {
    return 0;
  }

  size_t available = inode->size - *pos;
  size_t to_read = count < available ? count : available;

  /* Copy data */
  for (size_t i = 0; i < to_read; i++) {
    buf[i] = inode->data[*pos + i];
  }

  *pos += to_read;
  return to_read;
}

static ssize_t ramfs_write(struct file *file, const char *buf, size_t count,
                           loff_t *pos) {
  struct ramfs_inode *inode = (struct ramfs_inode *)file->private_data;

  if (!inode) {
    return -EIO;
  }

  size_t new_size = *pos + count;

  /* Grow data buffer if needed */
  if (new_size > inode->data_capacity) {
    size_t new_cap =
        (new_size + RAMFS_BLOCK_SIZE - 1) & ~(RAMFS_BLOCK_SIZE - 1);
    uint8_t *new_data = kmalloc(new_cap, GFP_KERNEL);
    if (!new_data) {
      return -ENOMEM;
    }

    /* Copy old data */
    if (inode->data) {
      for (size_t i = 0; i < inode->size; i++) {
        new_data[i] = inode->data[i];
      }
      kfree(inode->data);
    }

    for (size_t i = inode->size; i < new_cap; i++) {
      new_data[i] = 0;
    }

    inode->data = new_data;
    inode->data_capacity = new_cap;
  }

  if ((size_t)*pos > inode->size) {
    for (size_t i = inode->size; i < (size_t)*pos; i++) {
      inode->data[i] = 0;
    }
  }

  /* Write data */
  for (size_t i = 0; i < count; i++) {
    inode->data[*pos + i] = buf[i];
  }

  *pos += count;

  if (*pos > (loff_t)inode->size) {
    inode->size = *pos;
  }
  if (file->f_dentry && file->f_dentry->d_inode) {
    file->f_dentry->d_inode->i_size = inode->size;
  }

  return count;
}

static int ramfs_open(struct inode *vfs_inode, struct file *file) {
  /* Store ramfs inode in file private data */
  file->private_data = vfs_inode->i_private;
  return 0;
}

static int ramfs_release(struct inode *vfs_inode, struct file *file) {
  (void)vfs_inode;
  file->private_data = NULL;
  return 0;
}

static const struct file_operations ramfs_file_ops = {
    .read = ramfs_read,
    .write = ramfs_write,
    .open = ramfs_open,
    .release = ramfs_release,
    .llseek = NULL, /* Use default */
    .readdir = NULL,
    .ioctl = NULL,
    .mmap = NULL,
};

/* ===================================================================== */
/* Directory operations */
/* ===================================================================== */

static int ramfs_readdir_callback(void *ctx, const char *name, int len,
                                  loff_t offset, ino_t ino, unsigned type) {
  (void)ctx;
  (void)offset;
  (void)ino;
  (void)type;

  /* Print entry for now */
  printk("%.*s\n", len, name);
  return 0;
}

static int ramfs_readdir(struct file *file, void *ctx,
                         int (*filldir)(void *, const char *, int, loff_t,
                                        ino_t, unsigned)) {
  struct ramfs_inode *dir = (struct ramfs_inode *)file->private_data;

  if (!dir || !S_ISDIR(dir->mode)) {
    return -ENOTDIR;
  }

  loff_t pos = 0;

  /* . and .. */
  if (filldir) {
    filldir(ctx, ".", 1, pos++, dir->ino, S_IFDIR >> 12);
    filldir(ctx, "..", 2, pos++, dir->parent ? dir->parent->ino : dir->ino,
            S_IFDIR >> 12);
  }

  /* Children */
  struct ramfs_inode *child = dir->children;
  while (child) {
    int len = 0;
    while (child->name[len])
      len++;

    if (filldir) {
      filldir(ctx, child->name, len, pos++, child->ino, child->mode >> 12);
    }
    child = child->sibling;
  }

  return 0;
}

static const struct file_operations ramfs_dir_ops = {
    .read = NULL,
    .write = NULL,
    .open = ramfs_open,
    .release = ramfs_release,
    .llseek = NULL,
    .readdir = ramfs_readdir,
    .ioctl = NULL,
    .mmap = NULL,
};

static struct inode_operations ramfs_inode_ops; /* Forward decl */

static struct dentry *ramfs_lookup(struct inode *dir, struct dentry *dentry) {
  struct ramfs_inode *ram_dir = (struct ramfs_inode *)dir->i_private;
  struct ramfs_inode *ram_child = ramfs_lookup_child(ram_dir, dentry->d_name);

  if (!ram_child)
    return NULL;

  /* Create VFS inode for child */
  struct inode *inode = kzalloc(sizeof(struct inode), GFP_KERNEL);
  if (!inode)
    return NULL;

  inode->i_ino = ram_child->ino;
  inode->i_mode = ram_child->mode;
  inode->i_size = ram_child->size;
  inode->i_sb = dir->i_sb;
  inode->i_op = &ramfs_inode_ops;
  inode->i_fop = S_ISDIR(inode->i_mode) ? &ramfs_dir_ops : &ramfs_file_ops;
  inode->i_private = ram_child;

  dentry->d_inode = inode;

  return NULL; /* NULL means success/found in cache (we just populated it) */
}

static int ramfs_create(struct inode *dir, struct dentry *dentry, mode_t mode) {
  struct ramfs_inode *ram_dir = (struct ramfs_inode *)dir->i_private;
  if (!ram_dir || !dentry || !dentry->d_name[0]) {
    return -EINVAL;
  }
  if (ramfs_lookup_child(ram_dir, dentry->d_name)) {
    return -EEXIST;
  }
  struct ramfs_inode *ram_file =
      ramfs_alloc_inode(S_IFREG | mode, dentry->d_name);

  if (!ram_file)
    return -ENOMEM;

  if (ramfs_add_child(ram_dir, ram_file) != 0) {
    ramfs_free_inode(ram_file);
    return -ENOTDIR;
  }

  /* Create VFS inode */
  struct inode *inode = kzalloc(sizeof(struct inode), GFP_KERNEL);
  if (!inode)
    return -ENOMEM;

  inode->i_ino = ram_file->ino;
  inode->i_mode = ram_file->mode;
  inode->i_size = 0;
  inode->i_sb = dir->i_sb;
  inode->i_op = &ramfs_inode_ops;
  inode->i_fop = &ramfs_file_ops;
  inode->i_private = ram_file;

  dentry->d_inode = inode;

  return 0;
}

static int ramfs_mkdir(struct inode *dir, struct dentry *dentry, mode_t mode) {
  struct ramfs_inode *ram_dir = (struct ramfs_inode *)dir->i_private;
  if (!ram_dir || !dentry || !dentry->d_name[0]) {
    return -EINVAL;
  }
  if (ramfs_lookup_child(ram_dir, dentry->d_name)) {
    return -EEXIST;
  }
  struct ramfs_inode *ram_child =
      ramfs_alloc_inode(S_IFDIR | mode, dentry->d_name);

  if (!ram_child)
    return -ENOMEM;

  if (ramfs_add_child(ram_dir, ram_child) != 0) {
    ramfs_free_inode(ram_child);
    return -ENOTDIR;
  }

  /* Create VFS inode */
  struct inode *inode = kzalloc(sizeof(struct inode), GFP_KERNEL);
  if (!inode)
    return -ENOMEM;

  inode->i_ino = ram_child->ino;
  inode->i_mode = ram_child->mode;
  inode->i_size = 0;
  inode->i_sb = dir->i_sb;
  inode->i_op = &ramfs_inode_ops;
  inode->i_fop = &ramfs_dir_ops;
  inode->i_private = ram_child;

  dentry->d_inode = inode;

  return 0;
}

static int ramfs_rename(struct inode *old_dir, struct dentry *old_dentry,
                        struct inode *new_dir, struct dentry *new_dentry) {
  struct ramfs_inode *old_ram_dir = (struct ramfs_inode *)old_dir->i_private;
  struct ramfs_inode *new_ram_dir = (struct ramfs_inode *)new_dir->i_private;
  struct ramfs_inode *target =
      (struct ramfs_inode *)old_dentry->d_inode->i_private;

  if (!target)
    return -ENOENT;

  /* TODO: Check if new name exists (overwrite) */
  /* For now, just fail if exists for simplicity, or we should support
   * overwrite? */
  /* Let's keep it simple: if new exists, fail */
  if (ramfs_lookup_child(new_ram_dir, new_dentry->d_name)) {
    return -EEXIST;
  }

  /* 1. Unlink from old_dir */
  if (old_ram_dir->children == target) {
    old_ram_dir->children = target->sibling;
  } else {
    struct ramfs_inode *curr = old_ram_dir->children;
    while (curr && curr->sibling != target) {
      curr = curr->sibling;
    }
    if (curr)
      curr->sibling = target->sibling;
  }

  /* 2. Link to new_dir */
  /* Only if different directory, but even if same dir we need to rename */
  if (old_ram_dir != new_ram_dir) {
    target->sibling = new_ram_dir->children;
    new_ram_dir->children = target;
    target->parent = new_ram_dir;
  } else {
    /* Re-link in same dir (it was removed above) */
    target->sibling = old_ram_dir->children;
    old_ram_dir->children = target;
  }

  /* 3. Rename */
  int i;
  for (i = 0; i < RAMFS_MAX_NAME && new_dentry->d_name[i]; i++) {
    target->name[i] = new_dentry->d_name[i];
  }
  target->name[i] = '\0';

  return 0;
}

static int ramfs_unlink(struct inode *dir, struct dentry *dentry) {
  struct ramfs_inode *ram_dir = (struct ramfs_inode *)dir->i_private;
  struct ramfs_inode *target = ramfs_lookup_child(ram_dir, dentry->d_name);

  if (!target)
    return -ENOENT;

  /* Must be a file, not a directory */
  if (S_ISDIR(target->mode))
    return -EISDIR;

  /* Remove from parent's child list */
  struct ramfs_inode **prev = &ram_dir->children;
  while (*prev) {
    if (*prev == target) {
      *prev = target->sibling;
      break;
    }
    prev = &((*prev)->sibling);
  }

  /* Free the inode and its data */
  ramfs_free_inode(target);

  return 0;
}

static int ramfs_rmdir(struct inode *dir, struct dentry *dentry) {
  struct ramfs_inode *ram_dir = (struct ramfs_inode *)dir->i_private;
  struct ramfs_inode *target = ramfs_lookup_child(ram_dir, dentry->d_name);

  if (!target)
    return -ENOENT;

  /* Must be a directory */
  if (!S_ISDIR(target->mode))
    return -ENOTDIR;

  /* Directory must be empty */
  if (target->children)
    return -ENOTEMPTY;

  /* Remove from parent's child list */
  struct ramfs_inode **prev = &ram_dir->children;
  while (*prev) {
    if (*prev == target) {
      *prev = target->sibling;
      break;
    }
    prev = &((*prev)->sibling);
  }

  /* Free the inode */
  ramfs_free_inode(target);

  return 0;
}

static struct inode_operations ramfs_inode_ops = {
    .lookup = ramfs_lookup,
    .create = ramfs_create,
    .mkdir = ramfs_mkdir,
    .rmdir = ramfs_rmdir,
    .unlink = ramfs_unlink,
    .rename = ramfs_rename,
};

/* ===================================================================== */
/* Mounting */
/* ===================================================================== */

static struct super_block *ramfs_mount(struct file_system_type *fs_type,
                                       int flags, const char *dev_name,
                                       void *data) {
  (void)flags;
  (void)dev_name;
  (void)data;

  printk(KERN_INFO "RAMFS: Mounting ramfs\n");

  /* Initialize superblock info */
  ramfs_sb.next_ino = 1;
  ramfs_sb.inode_count = 0;

  /* Create ramfs root inode */
  ramfs_sb.root = ramfs_alloc_inode(S_IFDIR | 0755, "");
  if (!ramfs_sb.root) {
    return NULL;
  }

  /* Create superblock */
  static struct super_block sb;
  sb.s_blocksize = RAMFS_BLOCK_SIZE;
  sb.s_type = fs_type;
  sb.s_disk_index = -1;
  sb.s_fs_info = &ramfs_sb;

  /* Create VFS root inode */
  static struct inode vfs_root_inode;
  vfs_root_inode.i_ino = ramfs_sb.root->ino;
  vfs_root_inode.i_mode = S_IFDIR | 0755;
  vfs_root_inode.i_size = 0;
  vfs_root_inode.i_sb = &sb;
  vfs_root_inode.i_op = &ramfs_inode_ops;
  vfs_root_inode.i_fop = &ramfs_dir_ops;
  vfs_root_inode.i_private = ramfs_sb.root; /* Link to ramfs inode */

  /* Create root dentry */
  static struct dentry root_dentry;
  root_dentry.d_name[0] = '/';
  root_dentry.d_name[1] = '\0';
  root_dentry.d_inode = &vfs_root_inode; /* Point to VFS inode */
  root_dentry.d_parent = &root_dentry;
  root_dentry.d_child = NULL;
  root_dentry.d_sibling = NULL;
  root_dentry.d_sb = &sb;

  sb.s_root = &root_dentry;

  printk(KERN_INFO "RAMFS: Mounted successfully\n");

  return &sb;
}

static void ramfs_kill_sb(struct super_block *sb) {
  (void)sb;
  printk(KERN_INFO "RAMFS: Unmounting\n");
  /* TODO: Free all inodes */
}

/* ===================================================================== */
/* Filesystem type */
/* ===================================================================== */

static struct file_system_type ramfs_fs_type = {
    .name = "ramfs",
    .fs_flags = 0,
    .mount = ramfs_mount,
    .kill_sb = ramfs_kill_sb,
    .next = NULL,
};

/* ===================================================================== */
/* Initialization */
/* ===================================================================== */

int ramfs_init(void) {
  printk(KERN_INFO "RAMFS: Registering ramfs filesystem\n");
  return register_filesystem(&ramfs_fs_type);
}

/* ===================================================================== */
/* Helper: Create a file in ramfs */
/* ===================================================================== */

/* Forward declaration */
static struct ramfs_inode *ramfs_get_parent_dir(const char *path,
                                                char *filename);
static int ramfs_split_path(const char *path, char *parent_path,
                            size_t parent_size, char *name,
                            size_t name_size);
static struct ramfs_inode *ramfs_lookup_inode_path(const char *path);

int ramfs_create_file(const char *path, mode_t mode, const char *content) {
  if (!ramfs_sb.root) {
    return -ENOENT;
  }

  /* Parse path to find parent directory and filename */
  char filename[RAMFS_MAX_NAME + 1];
  struct ramfs_inode *parent = ramfs_get_parent_dir(path, filename);
  if (!parent) {
    return -ENOENT;
  }

  if (ramfs_lookup_child(parent, filename)) {
    return -EEXIST;
  }
  struct ramfs_inode *file = ramfs_alloc_inode(S_IFREG | mode, filename);
  if (!file) {
    return -ENOMEM;
  }

  /* Copy content if provided */
  if (content) {
    size_t len = 0;
    while (content[len])
      len++;

    file->data = kmalloc(len, GFP_KERNEL);
    if (!file->data) {
      ramfs_free_inode(file);
      return -ENOMEM;
    }
    for (size_t i = 0; i < len; i++) {
      file->data[i] = content[i];
    }
    file->size = len;
    file->data_capacity = len;
  }

  if (ramfs_add_child(parent, file) != 0) {
    ramfs_free_inode(file);
    return -ENOTDIR;
  }

  printk(KERN_INFO "RAMFS: Created file '%s'\n", path);

  return 0;
}

int ramfs_truncate_file(void *inode_private) {
  struct ramfs_inode *inode = (struct ramfs_inode *)inode_private;

  if (!inode || S_ISDIR(inode->mode)) {
    return -EINVAL;
  }

  inode->size = 0;
  return 0;
}

/* Helper to find or create parent directory from path */
static struct ramfs_inode *ramfs_get_parent_dir(const char *path,
                                                char *filename) {
  char parent_path[256];
  struct ramfs_inode *dir;

  if (ramfs_split_path(path, parent_path, sizeof(parent_path), filename,
                       RAMFS_MAX_NAME + 1) != 0)
    return NULL;

  if (parent_path[0] == '\0')
    return ramfs_sb.root;

  dir = ramfs_lookup_inode_path(parent_path);
  if (!dir || !S_ISDIR(dir->mode))
    return NULL;
  return dir;
}

int ramfs_create_file_bytes(const char *path, mode_t mode, const uint8_t *data,
                            size_t size) {
  if (!ramfs_sb.root) {
    return -ENOENT;
  }

  char filename[RAMFS_MAX_NAME + 1];
  struct ramfs_inode *parent = ramfs_get_parent_dir(path, filename);
  if (!parent) {
    return -ENOENT;
  }

  struct ramfs_inode *file = ramfs_alloc_inode(S_IFREG | mode, filename);
  if (!file) {
    return -ENOMEM;
  }
  if (ramfs_lookup_child(parent, filename)) {
    ramfs_free_inode(file);
    return -EEXIST;
  }

  if (data && size > 0) {
    file->data = kmalloc(size, GFP_KERNEL);
    if (!file->data) {
      ramfs_free_inode(file);
      return -ENOMEM;
    }
    for (size_t i = 0; i < size; i++) {
      file->data[i] = data[i];
    }
    file->size = size;
    file->data_capacity = size;
  }

  if (ramfs_add_child(parent, file) != 0) {
    ramfs_free_inode(file);
    return -ENOTDIR;
  }
  printk(KERN_INFO "RAMFS: Created file '%s' (%lu bytes)\n", path,
         (unsigned long)size);
  return 0;
}

int ramfs_create_dir(const char *path, mode_t mode) {
  char parent_path[256];
  char dirname[RAMFS_MAX_NAME + 1];
  struct ramfs_inode *parent;
  struct ramfs_inode *existing;

  if (!ramfs_sb.root) {
    return -ENOENT;
  }

  if (ramfs_split_path(path, parent_path, sizeof(parent_path), dirname,
                       sizeof(dirname)) != 0)
    return -EINVAL;

  parent = ramfs_sb.root;
  if (parent_path[0] != '\0') {
    parent = ramfs_lookup_inode_path(parent_path);
    if (!parent || !S_ISDIR(parent->mode))
      return -ENOENT;
  }

  existing = ramfs_lookup_child(parent, dirname);
  if (existing && S_ISDIR(existing->mode))
    return 0;
  if (existing)
    return -EEXIST;

  struct ramfs_inode *dir = ramfs_alloc_inode(S_IFDIR | mode, dirname);
  if (!dir) {
    return -ENOMEM;
  }

  ramfs_add_child(parent, dir);

  printk(KERN_INFO "RAMFS: Created directory '%s'\n", path);

  return 0;
}

static int ramfs_split_path(const char *path, char *parent_path,
                            size_t parent_size, char *name,
                            size_t name_size) {
  const char *start;
  const char *last_slash = NULL;
  size_t parent_len = 0;
  size_t name_len = 0;

  if (!path || !name || name_size == 0)
    return -EINVAL;

  start = path;
  while (*start == '/')
    start++;
  if (*start == '\0')
    return -EINVAL;

  for (const char *p = start; *p; p++) {
    if (*p == '/')
      last_slash = p;
  }

  if (!last_slash) {
    if (parent_path && parent_size > 0)
      parent_path[0] = '\0';
    while (start[name_len] && name_len < name_size - 1) {
      name[name_len] = start[name_len];
      name_len++;
    }
    name[name_len] = '\0';
    return 0;
  }

  parent_len = (size_t)(last_slash - start);
  if (parent_path && parent_size > 0) {
    size_t copy_len = parent_len < parent_size - 1 ? parent_len : parent_size - 1;
    for (size_t i = 0; i < copy_len; i++)
      parent_path[i] = start[i];
    parent_path[copy_len] = '\0';
  }

  last_slash++;
  while (last_slash[name_len] && name_len < name_size - 1) {
    name[name_len] = last_slash[name_len];
    name_len++;
  }
  name[name_len] = '\0';
  return (name_len == 0) ? -EINVAL : 0;
}

static struct ramfs_inode *ramfs_lookup_inode_path(const char *path) {
  struct ramfs_inode *current;
  char component[RAMFS_MAX_NAME + 1];

  if (!ramfs_sb.root || !path)
    return NULL;

  while (*path == '/')
    path++;
  if (*path == '\0')
    return ramfs_sb.root;

  current = ramfs_sb.root;
  while (*path) {
    int i = 0;

    while (*path && *path != '/' && i < RAMFS_MAX_NAME)
      component[i++] = *path++;
    component[i] = '\0';
    while (*path == '/')
      path++;

    current = ramfs_lookup_child(current, component);
    if (!current)
      return NULL;
  }

  return current;
}

/* ===================================================================== */
/* Public path lookup for vfs_compat */
/* ===================================================================== */

int ramfs_lookup_path_info(const char *path, size_t *out_size, int *out_is_dir,
                           void **out_data) {
  if (!ramfs_sb.root) {
    return -1;
  }

  /* Skip leading slash */
  if (*path == '/')
    path++;

  /* Empty path means root */
  if (*path == '\0') {
    if (out_size)
      *out_size = 0;
    if (out_is_dir)
      *out_is_dir = 1;
    if (out_data)
      *out_data = NULL;
    return 0;
  }

  struct ramfs_inode *current = ramfs_sb.root;
  char component[RAMFS_MAX_NAME + 1];

  while (*path) {
    /* Extract next path component */
    int i = 0;
    while (*path && *path != '/' && i < RAMFS_MAX_NAME) {
      component[i++] = *path++;
    }
    component[i] = '\0';

    /* Skip trailing slashes */
    while (*path == '/')
      path++;

    /* Look up component in current directory */
    current = ramfs_lookup_child(current, component);
    if (!current) {
      return -1; /* Not found */
    }
  }

  /* Found - fill out info */
  if (out_size)
    *out_size = current->size;
  if (out_is_dir)
    *out_is_dir = S_ISDIR(current->mode) ? 1 : 0;
  if (out_data)
    *out_data = current;

  return 0;
}
