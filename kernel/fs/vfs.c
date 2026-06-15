/*
 * UnixOS Kernel - Virtual Filesystem Implementation
 */

#include "fs/vfs.h"
#include "fs/fat32.h"
#include "drivers/storage.h"
#include "mm/kmalloc.h"
#include "printk.h"

extern int ramfs_truncate_file(void *inode_private);

/* ===================================================================== */
/* Static data */
/* ===================================================================== */

/* Registered filesystems */
static struct file_system_type *file_systems = NULL;

/* Mount points */
static struct vfsmount *mounts[MAX_MOUNTS];
static int mount_count = 0;
static struct vfsmount mount_pool[MAX_MOUNTS];
static int mount_slot_used[MAX_MOUNTS];

/* Root filesystem */
static struct vfsmount *root_mount = NULL;
static struct dentry *root_dentry = NULL;

/* ===================================================================== */
/* Helper functions */
/* ===================================================================== */

static struct file_system_type *find_filesystem(const char *name) {
  struct file_system_type *fs = file_systems;
  while (fs) {
    /* Compare names */
    const char *a = fs->name;
    const char *b = name;
    while (*a && *b && *a == *b) {
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0') {
      return fs;
    }
    fs = fs->next;
  }
  return NULL;
}

static int path_compare(const char *a, const char *b) {
  int a_len = 0;
  int b_len = 0;

  if (!a)
    a = "";
  if (!b)
    b = "";

  while (a[a_len])
    a_len++;
  while (b[b_len])
    b_len++;

  while (a_len > 1 && a[a_len - 1] == '/')
    a_len--;
  while (b_len > 1 && b[b_len - 1] == '/')
    b_len--;

  for (int i = 0;; i++) {
    char ac = (i < a_len) ? a[i] : '\0';
    char bc = (i < b_len) ? b[i] : '\0';
    if (ac != bc)
      return (int)((unsigned char)ac) - (int)((unsigned char)bc);
    if (ac == '\0')
      return 0;
  }
}

static void path_copy(char *dst, const char *src, int max) {
  int i = 0;
  if (!dst || max <= 0)
    return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  while (src[i] && i < max - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static int dentry_is_mount_root(const struct dentry *dentry) {
  if (!dentry)
    return 0;
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mounts[i] && mounts[i]->mnt_root == dentry)
      return 1;
  }
  return 0;
}

static int path_has_mount_prefix(const char *path, const char *target,
                                 size_t *matched_len) {
  size_t i = 0;

  if (!path || !target)
    return 0;

  while (target[i] && path[i] && path[i] == target[i])
    i++;
  if (target[i] != '\0')
    return 0;

  if (i == 1 && target[0] == '/') {
    if (matched_len)
      *matched_len = 1;
    return 1;
  }

  if (path[i] != '\0' && path[i] != '/')
    return 0;

  if (matched_len)
    *matched_len = i;
  return 1;
}

static struct vfsmount *find_mount_for_path(const char *path,
                                            size_t *matched_len) {
  struct vfsmount *best = NULL;
  size_t best_len = 0;

  if (!path)
    return NULL;

  for (int i = 0; i < MAX_MOUNTS; i++) {
    size_t len = 0;

    if (!mounts[i] || !mounts[i]->mnt_root)
      continue;
    if (!path_has_mount_prefix(path, mounts[i]->mnt_target, &len))
      continue;
    if (len >= best_len) {
      best = mounts[i];
      best_len = len;
    }
  }

  if (matched_len)
    *matched_len = best_len;
  return best;
}

static struct dentry *resolve_path_root(const char *path, const char **relative) {
  struct vfsmount *mnt;
  size_t matched_len = 0;
  const char *rest = path;

  if (relative)
    *relative = path;

  if (!path)
    return root_dentry;

  mnt = find_mount_for_path(path, &matched_len);
  if (mnt && mnt->mnt_root) {
    rest = path + matched_len;
    while (*rest == '/')
      rest++;
    if (relative)
      *relative = rest;
    return mnt->mnt_root;
  }

  return root_dentry;
}

static size_t path_length(const char *path) {
  size_t len = 0;

  if (!path)
    return 0;
  while (path[len])
    len++;
  return len;
}

static int vfs_ensure_parent_dirs(const char *path) {
  char current[PATH_MAX];
  size_t len;
  size_t last_slash = 0;

  if (!path || path[0] != '/')
    return -EINVAL;

  len = path_length(path);
  if (len >= sizeof(current))
    return -EINVAL;

  for (size_t i = 1; i < len; i++) {
    if (path[i] == '/')
      last_slash = i;
  }
  if (last_slash == 0)
    return 0;

  current[0] = '/';
  current[1] = '\0';
  for (size_t i = 1; i < last_slash; i++) {
    if (path[i] == '/') {
      current[i] = '\0';
      if (i > 1)
        vfs_mkdir(current, 0755);
    }
    current[i] = path[i];
    current[i + 1] = '\0';
  }

  current[last_slash] = '\0';
  if (last_slash > 1)
    vfs_mkdir(current, 0755);
  return 0;
}

static void vfs_free_dentry_chain(struct dentry *dentry) {
  while (dentry && dentry != root_dentry && !dentry_is_mount_root(dentry)) {
    struct dentry *parent = dentry->d_parent;
    kfree(dentry);
    if (!parent || parent == dentry)
      break;
    dentry = parent;
  }
}

static struct vfsmount *find_mount_by_target(const char *target) {
  if (!target)
    return NULL;
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mounts[i] && path_compare(mounts[i]->mnt_target, target) == 0)
      return mounts[i];
  }
  return NULL;
}

static int resolve_disk_index(const char *source) {
  if (!source || source[0] == '\0')
    return -1;
  return storage_get_disk_index_by_location(source);
}

/* ===================================================================== */
/* VFS initialization */
/* ===================================================================== */

void vfs_init(void) {
  printk(KERN_INFO "VFS: Initializing virtual filesystem\n");

  /* Clear mount table */
  for (int i = 0; i < MAX_MOUNTS; i++) {
    mounts[i] = NULL;
    mount_slot_used[i] = 0;
  }

  /* Register built-in filesystems */
  register_filesystem(&fat32_fs_type);
  /* register_filesystem(&ramfs_type); */
  /* register_filesystem(&procfs_type); */
  /* register_filesystem(&sysfs_type); */
  /* register_filesystem(&devfs_type); */

  printk(KERN_INFO "VFS: Initialized\n");
}

/* ===================================================================== */
/* Filesystem registration */
/* ===================================================================== */

int register_filesystem(struct file_system_type *fs) {
  if (!fs || !fs->name) {
    return -EINVAL;
  }

  /* Check for duplicate */
  if (find_filesystem(fs->name)) {
    printk(KERN_WARNING "VFS: Filesystem '%s' already registered\n", fs->name);
    return -EBUSY;
  }

  /* Add to list */
  fs->next = file_systems;
  file_systems = fs;

  printk(KERN_INFO "VFS: Registered filesystem '%s'\n", fs->name);

  return 0;
}

/* ===================================================================== */
/* Path lookup */
/* ===================================================================== */

static struct dentry *vfs_lookup_path(const char *path, const char **filename) {
  const char *p;
  struct dentry *curr;

  if (!root_dentry)
    return NULL;

  curr = resolve_path_root(path, &p);
  if (!curr)
    return NULL;
  p = (const char *)p;

  /* Skip leading / */
  while (*p == '/')
    p++;

  if (*p == '\0') {
    if (filename)
      *filename = NULL;
    return curr;
  }

  static char buf[NAME_MAX + 1];

  while (*p) {
    /* Extract next component */
    int len = 0;
    const char *start = p;
    while (*p && *p != '/') {
      if (len < NAME_MAX)
        buf[len++] = *p;
      p++;
    }
    buf[len] = '\0';

    while (*p == '/')
      p++;

    /* If this is the last component, return parent and filename */
    if (*p == '\0' && filename) {
      *filename = start; /* Pointer into original string - careful */
      /* Actually, we need to copy it because original might be const */
      /* But caller usually passes non-const or we can just return curr */
      /* Better design: return parent dentry and pointer to last component in
       * path */
      return curr; /* curr is the directory containing the file */
                   /* Wait, this logic is tricky. Let's do simple traversal */
    }

    /* Lookup child */
    if (!curr->d_inode || !curr->d_inode->i_op ||
        !curr->d_inode->i_op->lookup) {
      vfs_free_dentry_chain(curr);
      return NULL;
    }

    struct dentry target;
    for (int i = 0; i <= len; i++)
      target.d_name[i] = buf[i];

    /* In this simplified VFS, lookup populates the dentry if found */
    /* We need to allocate a real dentry to return/store */
    /* For now, simplified: rely on ramfs creating the inode and we assume we
     * traverse */

    /* Simple hack for ramfs traversal without full dcache: */
    /* We construct a dummy dentry, pass to lookup. If lookup populates d_inode,
     * we proceed. */

    /* Allocate a dentry to be safe/consistent */
    struct dentry *child = kzalloc(sizeof(struct dentry), GFP_KERNEL);
    if (!child) {
      vfs_free_dentry_chain(curr);
      return NULL;
    }

    for (int i = 0; i <= len; i++)
      child->d_name[i] = buf[i];
    child->d_parent = curr;
    child->d_sb = curr->d_sb;

    if (curr->d_inode->i_op->lookup(curr->d_inode, child) != NULL) {
      /* If it returns a dentry, use it */
      /* (Not implemented in ramfs, it returns NULL on success with populated
       * pointer) */
    }

    if (!child->d_inode) {
      /* Not found */
      vfs_free_dentry_chain(child);
      return NULL;
    }

    curr = child;
  }

  if (filename)
    *filename = NULL;
  return curr;
}

/* Helper to find parent and last component */
static struct dentry *vfs_lookup_parent(const char *path, char *name_buf) {
  const char *p;
  struct dentry *curr;

  if (!root_dentry)
    return NULL;

  curr = resolve_path_root(path, &p);
  if (!curr)
    return NULL;

  /* Skip leading / */
  while (*p == '/')
    p++;

  if (*p == '\0')
    return NULL; /* Root has no parent */

  static char buf[NAME_MAX + 1];

  while (*p) {
    /* Extract next component */
    int len = 0;
    while (*p && *p != '/') {
      if (len < NAME_MAX)
        buf[len++] = *p;
      p++;
    }
    buf[len] = '\0';

    while (*p == '/')
      p++;

    if (*p == '\0') {
      /* This was the last component */
      if (name_buf) {
        for (int i = 0; i <= len; i++)
          name_buf[i] = buf[i];
      }
      return curr;
    }

    /* Traverse down */
    if (!curr->d_inode || !curr->d_inode->i_op ||
        !curr->d_inode->i_op->lookup) {
      vfs_free_dentry_chain(curr);
      return NULL;
    }

    struct dentry *child = kzalloc(sizeof(struct dentry), GFP_KERNEL);
    if (!child) {
      vfs_free_dentry_chain(curr);
      return NULL;
    }

    for (int i = 0; i <= len; i++)
      child->d_name[i] = buf[i];

    /* Assume success for now (lookup populates child->d_inode) */
    curr->d_inode->i_op->lookup(curr->d_inode, child);

    if (!child->d_inode) {
      vfs_free_dentry_chain(child);
      return NULL;
    }
    curr = child;
  }

  return NULL;
}

/* Redefine vfs_open with lookup */
struct file *vfs_open(const char *path, int flags, mode_t mode) {
  if (!path || path[0] == '\0') {
    return NULL;
  }
  /* Special case for root */
  if (path[0] == '/' && path[1] == '\0') {
    struct file *f = kzalloc(sizeof(struct file), GFP_KERNEL);
    if (!root_dentry || !root_dentry->d_inode) {
      if (f)
        kfree(f);
      return NULL;
    }
    f->f_dentry = root_dentry;
    f->f_op = root_dentry->d_inode->i_fop;
    f->private_data = root_dentry->d_inode->i_private;
    f->f_mode = mode;
    f->f_flags = flags;
    f->f_count.counter = 1;
    return f;
  }

  struct vfsmount *mounted = find_mount_by_target(path);
  if (mounted && mounted->mnt_root &&
      path_compare(mounted->mnt_target, path) == 0) {
    struct file *f = kzalloc(sizeof(struct file), GFP_KERNEL);
    if (!f)
      return NULL;
    f->f_dentry = mounted->mnt_root;
    f->f_op = mounted->mnt_root->d_inode->i_fop;
    f->private_data = mounted->mnt_root->d_inode->i_private;
    f->f_mode = mode;
    f->f_flags = flags;
    f->f_count.counter = 1;
    return f;
  }

  char name[NAME_MAX + 1];
  struct dentry *parent = vfs_lookup_parent(path, name);

  if (!parent) {
    /* Try full lookup (might be exact match on an intermediate node? Unlikely
     * for open) */
    /* Or file exists in root */
    if (root_dentry)
      parent = root_dentry;

    /* Extract name from /name */
    const char *p = path;
    while (*p == '/')
      p++;
    int i = 0;
    while (*p && *p != '/') {
      if (i < NAME_MAX)
        name[i++] = *p;
      p++;
    }
    name[i] = '\0';
    if (*p != '\0')
      return NULL; /* Path had more components but parent lookup failed */
  }

  /* Now look for the file in parent */
  struct dentry *child = kzalloc(sizeof(struct dentry), GFP_KERNEL);
  if (!child) {
    if (parent && parent != root_dentry && !dentry_is_mount_root(parent))
      vfs_free_dentry_chain(parent);
    return NULL;
  }
  for (int i = 0; i < NAME_MAX && name[i]; i++)
    child->d_name[i] = name[i];
  if (name[0] == '\0') {
    if (parent && parent != root_dentry && !dentry_is_mount_root(parent))
      vfs_free_dentry_chain(parent);
    kfree(child);
    return NULL;
  }
  child->d_parent = parent;
  child->d_sb = parent->d_sb;

  if (parent->d_inode && parent->d_inode->i_op &&
      parent->d_inode->i_op->lookup) {
    parent->d_inode->i_op->lookup(parent->d_inode, child);
  }

  if (!child->d_inode) {
    /* Check O_CREAT */
    if (flags & O_CREAT) {
      /* Create it */
      if (parent->d_inode->i_op && parent->d_inode->i_op->create) {
        int ret = parent->d_inode->i_op->create(parent->d_inode, child, mode);
        if (ret != 0) {
          vfs_free_dentry_chain(child);
          return NULL;
        }
      } else {
        vfs_free_dentry_chain(child);
        return NULL;
      }
    } else {
      vfs_free_dentry_chain(child);
      return NULL;
    }
  }

  struct file *f = kzalloc(sizeof(struct file), GFP_KERNEL);
  if (!f) {
    vfs_free_dentry_chain(child);
    return NULL;
  }

  f->f_dentry = child;
  f->f_op = child->d_inode->i_fop;
  f->private_data = child->d_inode->i_private;
  f->f_mode = mode;
  f->f_flags = flags;
  f->f_count.counter = 1;

  if ((flags & O_TRUNC) && child->d_inode && S_ISREG(child->d_inode->i_mode)) {
    child->d_inode->i_size = 0;
    ramfs_truncate_file(f->private_data);
  }

  if ((flags & O_APPEND) && child->d_inode && S_ISREG(child->d_inode->i_mode)) {
    f->f_pos = child->d_inode->i_size;
  }

  if (f->f_op && f->f_op->open) {
    f->f_op->open(child->d_inode, f);
    if ((flags & O_APPEND) && child->d_inode &&
        S_ISREG(child->d_inode->i_mode)) {
      f->f_pos = child->d_inode->i_size;
    }
  }

  if (parent && parent != root_dentry && !dentry_is_mount_root(parent)) {
    child->d_parent = root_dentry;
    vfs_free_dentry_chain(parent);
  }

  return f;
}

int vfs_create(const char *path, mode_t mode) {
  char name[NAME_MAX + 1];
  struct dentry *parent = vfs_lookup_parent(path, name);
  if (!parent)
    return -ENOENT;
  if (name[0] == '\0') {
    if (parent != root_dentry && !dentry_is_mount_root(parent))
      vfs_free_dentry_chain(parent);
    return -EINVAL;
  }

  struct dentry *child = kzalloc(sizeof(struct dentry), GFP_KERNEL);
  if (!child) {
    if (parent != root_dentry && !dentry_is_mount_root(parent))
      vfs_free_dentry_chain(parent);
    return -ENOMEM;
  }
  for (int i = 0; i < NAME_MAX && name[i]; i++)
    child->d_name[i] = name[i];
  child->d_parent = parent;
  child->d_sb = parent->d_sb;

  if (!parent->d_inode || !parent->d_inode->i_op || !parent->d_inode->i_op->create) {
    vfs_free_dentry_chain(child);
    return -EPERM;
  }

  int ret = parent->d_inode->i_op->create(parent->d_inode, child, mode);
  vfs_free_dentry_chain(child);
  return ret;
}

int vfs_mkdir(const char *path, mode_t mode) {
  char name[NAME_MAX + 1];
  struct dentry *parent = vfs_lookup_parent(path, name);
  if (!parent)
    return -ENOENT;
  if (name[0] == '\0') {
    if (parent != root_dentry && !dentry_is_mount_root(parent))
      vfs_free_dentry_chain(parent);
    return -EINVAL;
  }

  struct dentry *child = kzalloc(sizeof(struct dentry), GFP_KERNEL);
  if (!child) {
    if (parent != root_dentry && !dentry_is_mount_root(parent))
      vfs_free_dentry_chain(parent);
    return -ENOMEM;
  }
  for (int i = 0; i < NAME_MAX && name[i]; i++)
    child->d_name[i] = name[i];
  child->d_parent = parent;
  child->d_sb = parent->d_sb;

  if (!parent->d_inode || !parent->d_inode->i_op || !parent->d_inode->i_op->mkdir) {
    vfs_free_dentry_chain(child);
    return -EPERM;
  }

  int ret = parent->d_inode->i_op->mkdir(parent->d_inode, child, mode);
  vfs_free_dentry_chain(child);
  return ret;
}

int vfs_readdir(struct file *file, void *ctx,
                int (*filldir)(void *, const char *, int, loff_t, ino_t,
                               unsigned)) {
  if (!file || !file->f_op || !file->f_op->readdir) {
    return -EINVAL;
  }
  return file->f_op->readdir(file, ctx, filldir);
}

int vfs_close(struct file *file) {
  if (!file)
    return -EBADF;
  if (file->f_op && file->f_op->release && file->f_dentry) {
    file->f_op->release(file->f_dentry->d_inode, file);
  }
  file->f_count.counter--;
  if (file->f_count.counter <= 0) {
    if (file->f_dentry && file->f_dentry != root_dentry &&
        !dentry_is_mount_root(file->f_dentry))
      vfs_free_dentry_chain(file->f_dentry);
    kfree(file);
  }
  return 0;
}

ssize_t vfs_read(struct file *file, char *buf, size_t count) {
  if (!file)
    return -EBADF;
  if (!buf)
    return -EFAULT;
  if (!file->f_op || !file->f_op->read)
    return -EINVAL;
  return file->f_op->read(file, buf, count, &file->f_pos);
}

ssize_t vfs_write(struct file *file, const char *buf, size_t count) {
  if (!file)
    return -EBADF;
  if (!buf)
    return -EFAULT;
  if (!file->f_op || !file->f_op->write)
    return -EINVAL;
  return file->f_op->write(file, buf, count, &file->f_pos);
}

int vfs_save_file(const char *path, const void *data, size_t size,
                  uint32_t flags) {
  struct file *file;
  int open_flags = O_WRONLY | O_CREAT;
  size_t total = 0;

  if (!path || path[0] == '\0' || (!data && size > 0))
    return -EINVAL;
  if (size > 0x7fffffffU)
    return -EFBIG;

  if (flags & VFS_SAVE_CREATE_PARENTS)
    vfs_ensure_parent_dirs(path);

  if (flags & VFS_SAVE_APPEND)
    open_flags |= O_APPEND;
  else
    open_flags |= O_TRUNC;

  file = vfs_open(path, open_flags, 0644);
  if (!file)
    return -ENOENT;

  while (total < size) {
    ssize_t written = vfs_write(file, (const char *)data + total, size - total);
    if (written <= 0) {
      vfs_close(file);
      return -EIO;
    }
    total += (size_t)written;
  }

  vfs_close(file);
  return (int)total;
}

loff_t vfs_lseek(struct file *file, loff_t offset, int whence) {
  if (!file)
    return -EBADF;
  loff_t new_pos;
  struct inode *inode = file->f_dentry ? file->f_dentry->d_inode : NULL;

  switch (whence) {
  case SEEK_SET:
    new_pos = offset;
    break;
  case SEEK_CUR:
    new_pos = file->f_pos + offset;
    break;
  case SEEK_END:
    if (!inode)
      return -EINVAL;
    new_pos = inode->i_size + offset;
    break;
  default:
    return -EINVAL;
  }
  if (new_pos < 0)
    return -EINVAL;
  file->f_pos = new_pos;
  return new_pos;
}

int vfs_rmdir(const char *path) {
  char name[NAME_MAX + 1];
  struct dentry *parent = vfs_lookup_parent(path, name);
  if (!parent)
    return -ENOENT;

  struct dentry *child = kzalloc(sizeof(struct dentry), GFP_KERNEL);
  if (!child) {
    if (parent != root_dentry && !dentry_is_mount_root(parent))
      vfs_free_dentry_chain(parent);
    return -ENOMEM;
  }

  int i;
  for (i = 0; i < NAME_MAX && name[i]; i++)
    child->d_name[i] = name[i];
  child->d_name[i] = '\0';

  /* Lookup the target */
  if (parent->d_inode->i_op && parent->d_inode->i_op->lookup) {
    parent->d_inode->i_op->lookup(parent->d_inode, child);
  }

  if (!child->d_inode) {
    vfs_free_dentry_chain(child);
    return -ENOENT;
  }

  /* Must be a directory */
  if (!S_ISDIR(child->d_inode->i_mode)) {
    vfs_free_dentry_chain(child);
    return -ENOTDIR;
  }

  /* Check if rmdir operation is supported */
  if (!parent->d_inode->i_op || !parent->d_inode->i_op->rmdir) {
    vfs_free_dentry_chain(child);
    return -EPERM;
  }

  int ret = parent->d_inode->i_op->rmdir(parent->d_inode, child);
  vfs_free_dentry_chain(child);
  return ret;
}

int vfs_unlink(const char *path) {
  char name[NAME_MAX + 1];
  struct dentry *parent = vfs_lookup_parent(path, name);
  if (!parent)
    return -ENOENT;

  struct dentry *child = kzalloc(sizeof(struct dentry), GFP_KERNEL);
  if (!child) {
    if (parent != root_dentry && !dentry_is_mount_root(parent))
      vfs_free_dentry_chain(parent);
    return -ENOMEM;
  }

  int i;
  for (i = 0; i < NAME_MAX && name[i]; i++)
    child->d_name[i] = name[i];
  child->d_name[i] = '\0';

  /* Lookup the target */
  if (parent->d_inode->i_op && parent->d_inode->i_op->lookup) {
    parent->d_inode->i_op->lookup(parent->d_inode, child);
  }

  if (!child->d_inode) {
    vfs_free_dentry_chain(child);
    return -ENOENT;
  }

  /* Must not be a directory (use rmdir for that) */
  if (S_ISDIR(child->d_inode->i_mode)) {
    vfs_free_dentry_chain(child);
    return -EISDIR;
  }

  /* Check if unlink operation is supported */
  if (!parent->d_inode->i_op || !parent->d_inode->i_op->unlink) {
    vfs_free_dentry_chain(child);
    return -EPERM;
  }

  int ret = parent->d_inode->i_op->unlink(parent->d_inode, child);
  vfs_free_dentry_chain(child);
  return ret;
}
int vfs_rename(const char *old, const char *new) {
  char old_name_buf[NAME_MAX + 1];
  struct dentry *old_parent = vfs_lookup_parent(old, old_name_buf);
  if (!old_parent)
    return -ENOENT;

  char new_name_buf[NAME_MAX + 1];
  struct dentry *new_parent = vfs_lookup_parent(new, new_name_buf);
  if (!new_parent) {
    if (old_parent != root_dentry && !dentry_is_mount_root(old_parent))
      vfs_free_dentry_chain(old_parent);
    return -ENOENT;
  }

  /* Lookup full old dentry */
  struct dentry *old_child = kzalloc(sizeof(struct dentry), GFP_KERNEL);
  if (!old_child) {
    if (old_parent != root_dentry && !dentry_is_mount_root(old_parent))
      vfs_free_dentry_chain(old_parent);
    if (new_parent != root_dentry && !dentry_is_mount_root(new_parent))
      vfs_free_dentry_chain(new_parent);
    return -ENOMEM;
  }
  int i;
  for (i = 0; i < NAME_MAX && old_name_buf[i]; i++)
    old_child->d_name[i] = old_name_buf[i];
  old_child->d_name[i] = '\0';
  old_child->d_parent = old_parent;
  old_child->d_sb = old_parent->d_sb;

  if (old_parent->d_inode->i_op && old_parent->d_inode->i_op->lookup) {
    old_parent->d_inode->i_op->lookup(old_parent->d_inode, old_child);
  }

  if (!old_child->d_inode) {
    vfs_free_dentry_chain(old_child);
    return -ENOENT;
  }

  /* Construct new dentry pattern */
  struct dentry *new_child = kzalloc(sizeof(struct dentry), GFP_KERNEL);
  if (!new_child) {
    vfs_free_dentry_chain(old_child);
    if (new_parent != root_dentry && !dentry_is_mount_root(new_parent))
      vfs_free_dentry_chain(new_parent);
    return -ENOMEM;
  }
  for (i = 0; i < NAME_MAX && new_name_buf[i]; i++)
    new_child->d_name[i] = new_name_buf[i];
  new_child->d_name[i] = '\0';
  new_child->d_parent = new_parent;
  new_child->d_sb = new_parent->d_sb;

  /* Check if operation supported */
  if (!old_parent->d_inode->i_op || !old_parent->d_inode->i_op->rename) {
    vfs_free_dentry_chain(old_child);
    vfs_free_dentry_chain(new_child);
    return -EPERM; /* Should be ENOSYS/EPERM */
  }

  int ret = old_parent->d_inode->i_op->rename(old_parent->d_inode, old_child,
                                              new_parent->d_inode, new_child);

  vfs_free_dentry_chain(old_child);
  vfs_free_dentry_chain(new_child);
  return ret;
}

/* ===================================================================== */
/* Mount operations */
/* ===================================================================== */

int vfs_mount(const char *source, const char *target, const char *fstype,
              unsigned long flags, const void *data) {
  (void)flags;
  (void)data;

  printk(KERN_INFO "VFS: mount('%s', '%s', '%s')\n", source, target, fstype);

  /* Find filesystem type */
  struct file_system_type *fs = find_filesystem(fstype);
  if (!fs) {
    printk(KERN_ERR "VFS: Unknown filesystem type '%s'\n", fstype);
    return -ENODEV;
  }

  /* Reject conflicting reuse of the same mountpoint, but treat an identical
   * remount request as already satisfied. */
  struct vfsmount *existing = find_mount_by_target(target);
  if (existing) {
    if (path_compare(existing->mnt_devname, source) == 0 &&
        path_compare(existing->mnt_fstype, fstype) == 0) {
      printk(KERN_INFO "VFS: '%s' already mounted on '%s', skipping duplicate\n",
             source, target);
      return 0;
    }
    printk(KERN_WARNING "VFS: Mountpoint '%s' already in use by '%s' (%s)\n",
           target, existing->mnt_devname, existing->mnt_fstype);
    return -EBUSY;
  }

  /* Check mount limit */
  if (mount_count >= MAX_MOUNTS)
    return -ENOMEM;

  /* Call filesystem's mount function */
  if (!fs->mount) {
    return -ENOSYS;
  }

  struct super_block *sb = fs->mount(fs, flags, source, (void *)data);
  if (!sb) {
    return -EIO;
  }

  if (sb->s_disk_index < 0) {
    sb->s_disk_index = resolve_disk_index(source);
  }

  /* Create mount structure */
  int slot = -1;
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (!mount_slot_used[i]) {
      slot = i;
      break;
    }
  }
  if (slot < 0)
    return -ENOMEM;

  struct vfsmount *mnt = &mount_pool[slot];
  mount_slot_used[slot] = 1;

  mnt->mnt_root = sb->s_root;
  mnt->mnt_sb = sb;
  mnt->mnt_mountpoint = NULL; /* TODO: Find target dentry */
  mnt->mnt_parent = root_mount;

  /* Copy device name */
  int i;
  for (i = 0; i < 63 && source[i]; i++) {
    mnt->mnt_devname[i] = source[i];
  }
  mnt->mnt_devname[i] = '\0';
  path_copy(mnt->mnt_target, target, sizeof(mnt->mnt_target));
  path_copy(mnt->mnt_fstype, fstype, sizeof(mnt->mnt_fstype));

  mounts[slot] = mnt;
  mount_count++;

  /* If mounting root, set root_mount */
  if (path_compare(target, "/") == 0) {
    root_mount = mnt;
    root_dentry = sb->s_root;
  }

  printk(KERN_INFO "VFS: Mounted '%s' on '%s'\n", source, target);

  return 0;
}

int vfs_umount(const char *target) {
  printk(KERN_INFO "VFS: umount('%s')\n", target);

  /* Find mount point */
  for (int i = 0; i < MAX_MOUNTS; i++) {
    if (mounts[i] && mounts[i]->mnt_root &&
        path_compare(mounts[i]->mnt_target, target) == 0) {
      int slot = (int)(mounts[i] - mount_pool);
      /* TODO: Tear down the superblock properly. */
      mounts[i]->mnt_root = NULL;
      mounts[i]->mnt_sb = NULL;
      mounts[i]->mnt_mountpoint = NULL;
      mounts[i]->mnt_parent = NULL;
      mounts[i]->mnt_devname[0] = '\0';
      mounts[i]->mnt_target[0] = '\0';
      mounts[i]->mnt_fstype[0] = '\0';
      if (root_mount == mounts[i]) {
        root_mount = NULL;
        root_dentry = NULL;
      }
      mounts[i] = NULL;
      if (slot >= 0 && slot < MAX_MOUNTS)
        mount_slot_used[slot] = 0;
      if (mount_count > 0)
        mount_count--;
      printk(KERN_INFO "VFS: Unmounted '%s'\n", target);
      return 0;
    }
  }

  return -ENOSYS;
}
