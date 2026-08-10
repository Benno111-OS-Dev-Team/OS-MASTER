/*
 * UnixOS Kernel - Virtual Filesystem Implementation
 */

#include "fs/vfs.h"
#include "fs/fat32.h"
#include "drivers/storage.h"
#include "mm/kmalloc.h"
#include "printk.h"
#include "string.h"

extern int ramfs_truncate_file(void *inode_private);
extern int ramfs_resize_file(void *inode_private, size_t size);

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

static void file_set_path(struct file *file, const char *path) {
  if (!file)
    return;
  path_copy(file->f_path, path ? path : "", sizeof(file->f_path));
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

static int inode_is_ramfs(const struct inode *inode) {
  return inode && inode->i_sb && inode->i_sb->s_type &&
         path_compare(inode->i_sb->s_type->name, "ramfs") == 0;
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

static long vfs_fs_type_magic(const char *name) {
  if (!name)
    return 0;
  if (path_compare(name, "ramfs") == 0)
    return 0x858458f6;
  if (path_compare(name, "fat32") == 0)
    return 0x4d44;
  if (path_compare(name, "iso9660") == 0)
    return 0x9660;
  if (path_compare(name, "ext4") == 0)
    return 0xef53;
  if (path_compare(name, "apfs") == 0)
    return 0x4253584e;
  return 0;
}

static int vfs_statfs_super(struct super_block *sb, struct vfs_statfs *stat) {
  uint64_t dev = 0;

  if (!sb || !stat)
    return -EINVAL;
  if (sb->s_op && sb->s_op->statfs) {
    int ret = sb->s_op->statfs(sb, stat);
    if (ret < 0)
      return ret;
  } else {
    memset(stat, 0, sizeof(*stat));
  }

  if (!stat->type)
    stat->type = vfs_fs_type_magic(sb->s_type ? sb->s_type->name : NULL);
  if (!stat->bsize)
    stat->bsize = sb->s_blocksize ? (long)sb->s_blocksize : 4096;
  if (!stat->frsize)
    stat->frsize = stat->bsize;
  if (!stat->namelen)
    stat->namelen = NAME_MAX;
  if (sb->s_dev)
    dev = sb->s_dev;
  else if (sb->s_disk_index >= 0)
    dev = (uint64_t)sb->s_disk_index + 1;
  stat->fsid[0] = dev;
  stat->fsid[1] = (uint64_t)(uintptr_t)sb;
  return 0;
}

static void vfs_release_superblock(struct file_system_type *fs,
                                   struct super_block *sb) {
  if (!sb)
    return;
  if (!fs && sb->s_type)
    fs = sb->s_type;
  if (fs && fs->kill_sb) {
    fs->kill_sb(sb);
    return;
  }
  if (sb->s_op && sb->s_op->put_super)
    sb->s_op->put_super(sb);
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
    file_set_path(f, "/");
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
    file_set_path(f, path);
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
  file_set_path(f, path);

  if ((flags & O_TRUNC) && child->d_inode && S_ISREG(child->d_inode->i_mode)) {
    if (!inode_is_ramfs(child->d_inode)) {
      vfs_close(f);
      return NULL;
    }
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

static int vfs_stat_inode(struct inode *inode, struct vfs_stat *stat) {
  if (!inode || !stat)
    return -EINVAL;

  stat->dev = inode->i_sb ? inode->i_sb->s_dev : 0;
  stat->ino = inode->i_ino;
  stat->mode = inode->i_mode;
  stat->nlink = inode->i_nlink ? inode->i_nlink : 1;
  stat->uid = inode->i_uid;
  stat->gid = inode->i_gid;
  stat->rdev = inode->i_rdev;
  stat->size = inode->i_size;
  stat->blksize = inode->i_blksize ? inode->i_blksize : 4096;
  stat->blocks = inode->i_blocks;
  if (stat->blocks == 0 && stat->size > 0) {
    stat->blocks = (stat->size + 511) / 512;
  }
  stat->atime = inode->i_atime;
  stat->mtime = inode->i_mtime;
  stat->ctime = inode->i_ctime;
  return 0;
}

int vfs_stat_path(const char *path, struct vfs_stat *stat) {
  struct dentry *dentry;
  int ret;

  if (!path || !stat)
    return -EINVAL;

  dentry = vfs_lookup_path(path, NULL);
  if (!dentry || !dentry->d_inode) {
    if (dentry)
      vfs_free_dentry_chain(dentry);
    return -ENOENT;
  }

  ret = vfs_stat_inode(dentry->d_inode, stat);
  if (dentry != root_dentry && !dentry_is_mount_root(dentry))
    vfs_free_dentry_chain(dentry);
  return ret;
}

int vfs_statfs_path(const char *path, struct vfs_statfs *stat) {
  struct dentry *dentry;
  struct super_block *sb;
  int ret;

  if (!path || !stat)
    return -EINVAL;

  dentry = vfs_lookup_path(path, NULL);
  if (!dentry || !dentry->d_inode) {
    if (dentry)
      vfs_free_dentry_chain(dentry);
    return -ENOENT;
  }

  sb = dentry->d_sb ? dentry->d_sb : dentry->d_inode->i_sb;
  ret = vfs_statfs_super(sb, stat);
  if (dentry != root_dentry && !dentry_is_mount_root(dentry))
    vfs_free_dentry_chain(dentry);
  return ret;
}

int vfs_statfs_file(struct file *file, struct vfs_statfs *stat) {
  struct super_block *sb;

  if (!file || !file->f_dentry || !file->f_dentry->d_inode || !stat)
    return -EBADF;
  sb = file->f_dentry->d_sb ? file->f_dentry->d_sb :
                              file->f_dentry->d_inode->i_sb;
  return vfs_statfs_super(sb, stat);
}

int vfs_file_path(struct file *file, char *buf, size_t size) {
  if (!file || !buf || size == 0)
    return -EINVAL;
  if (file->f_path[0] == '\0')
    return -ENOSYS;
  if (strlcpy(buf, file->f_path, size) >= size)
    return -ERANGE;
  return 0;
}

int vfs_close(struct file *file) {
  if (!file)
    return -EBADF;
  file->f_count.counter--;
  if (file->f_count.counter <= 0) {
    if (file->f_op && file->f_op->release) {
      struct inode *inode = file->f_dentry ? file->f_dentry->d_inode : NULL;
      file->f_op->release(inode, file);
    }
    if (file->f_dentry && file->f_dentry != root_dentry &&
        !dentry_is_mount_root(file->f_dentry))
      vfs_free_dentry_chain(file->f_dentry);
    kfree(file);
  }
  return 0;
}

int vfs_truncate_file(struct file *file, loff_t length) {
  struct inode *inode;
  int ret;

  if (!file)
    return -EBADF;
  if (length < 0)
    return -EINVAL;

  inode = file->f_dentry ? file->f_dentry->d_inode : NULL;
  if (!inode)
    return -ENOENT;
  if (!S_ISREG(inode->i_mode))
    return -EINVAL;
  if (!inode_is_ramfs(inode))
    return -ENOSYS;

  ret = ramfs_resize_file(file->private_data, (size_t)length);
  if (ret == 0) {
    inode->i_size = length;
    if (file->f_pos > length)
      file->f_pos = length;
  }
  return ret;
}

int vfs_poll_file(struct file *file, short events) {
  short ready = 0;

  if (!file)
    return -EBADF;
  if (file->f_op && file->f_op->poll)
    return file->f_op->poll(file, events);

  if ((events & 0x001) && file->f_op && file->f_op->read)
    ready |= 0x001;
  if ((events & 0x004) && file->f_op && file->f_op->write)
    ready |= 0x004;
  if ((events & 0x040) && file->f_op && file->f_op->read)
    ready |= 0x040;
  if ((events & 0x100) && file->f_op && file->f_op->write)
    ready |= 0x100;
  return ready;
}

int vfs_sync_file(struct file *file) {
  struct inode *inode;
  struct super_block *sb;

  if (!file)
    return -EBADF;
  inode = file->f_dentry ? file->f_dentry->d_inode : NULL;
  sb = inode ? inode->i_sb : NULL;
  if (sb && sb->s_op && sb->s_op->sync_fs)
    return sb->s_op->sync_fs(sb, 0);
  return 0;
}

int vfs_sync_all(void) {
  int ret = 0;

  for (int i = 0; i < MAX_MOUNTS; i++) {
    struct super_block *sb = mounts[i] ? mounts[i]->mnt_sb : NULL;
    if (sb && sb->s_op && sb->s_op->sync_fs) {
      int fs_ret = sb->s_op->sync_fs(sb, 0);
      if (fs_ret < 0 && ret == 0)
        ret = fs_ret;
    }
  }

  return ret;
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

int vfs_symlink(const char *target, const char *linkpath) {
  char name[NAME_MAX + 1];
  struct dentry *parent;
  struct dentry *child;
  int ret;

  if (!target || !linkpath || target[0] == '\0' || linkpath[0] == '\0')
    return -EINVAL;

  parent = vfs_lookup_parent(linkpath, name);
  if (!parent)
    return -ENOENT;
  if (name[0] == '\0') {
    if (parent != root_dentry && !dentry_is_mount_root(parent))
      vfs_free_dentry_chain(parent);
    return -EINVAL;
  }

  child = kzalloc(sizeof(struct dentry), GFP_KERNEL);
  if (!child) {
    if (parent != root_dentry && !dentry_is_mount_root(parent))
      vfs_free_dentry_chain(parent);
    return -ENOMEM;
  }
  for (int i = 0; i < NAME_MAX && name[i]; i++)
    child->d_name[i] = name[i];
  child->d_parent = parent;
  child->d_sb = parent->d_sb;

  if (!parent->d_inode || !parent->d_inode->i_op ||
      !parent->d_inode->i_op->symlink) {
    vfs_free_dentry_chain(child);
    return -ENOSYS;
  }

  ret = parent->d_inode->i_op->symlink(parent->d_inode, child, target);
  vfs_free_dentry_chain(child);
  return ret;
}

int vfs_link(const char *oldpath, const char *newpath) {
  char name[NAME_MAX + 1];
  struct dentry *old_dentry;
  struct dentry *new_parent;
  struct dentry *new_dentry;
  int ret;

  if (!oldpath || !newpath || oldpath[0] == '\0' || newpath[0] == '\0')
    return -EINVAL;

  old_dentry = vfs_lookup_path(oldpath, NULL);
  if (!old_dentry)
    return -ENOENT;
  new_parent = vfs_lookup_parent(newpath, name);
  if (!new_parent) {
    vfs_free_dentry_chain(old_dentry);
    return -ENOENT;
  }

  new_dentry = kzalloc(sizeof(struct dentry), GFP_KERNEL);
  if (!new_dentry) {
    vfs_free_dentry_chain(old_dentry);
    if (new_parent != root_dentry && !dentry_is_mount_root(new_parent))
      vfs_free_dentry_chain(new_parent);
    return -ENOMEM;
  }
  for (int i = 0; i < NAME_MAX && name[i]; i++)
    new_dentry->d_name[i] = name[i];
  new_dentry->d_parent = new_parent;
  new_dentry->d_sb = new_parent->d_sb;

  if (!new_parent->d_inode || !new_parent->d_inode->i_op ||
      !new_parent->d_inode->i_op->link) {
    vfs_free_dentry_chain(old_dentry);
    vfs_free_dentry_chain(new_dentry);
    return -ENOSYS;
  }

  ret = new_parent->d_inode->i_op->link(old_dentry, new_parent->d_inode,
                                        new_dentry);
  vfs_free_dentry_chain(old_dentry);
  vfs_free_dentry_chain(new_dentry);
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
    if (new_parent != root_dentry && !dentry_is_mount_root(new_parent))
      vfs_free_dentry_chain(new_parent);
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
    return -ENOSYS;
  }

  int ret = old_parent->d_inode->i_op->rename(old_parent->d_inode, old_child,
                                              new_parent->d_inode, new_child);

  vfs_free_dentry_chain(old_child);
  vfs_free_dentry_chain(new_child);
  return ret;
}

int vfs_symlink(const char *target, const char *linkpath) {
  char name[NAME_MAX + 1];
  struct dentry *parent = vfs_lookup_parent(linkpath, name);
  struct dentry *child;
  int ret;

  if (!target || !linkpath)
    return -EINVAL;
  if (!parent)
    return -ENOENT;
  if (name[0] == '\0') {
    if (parent != root_dentry && !dentry_is_mount_root(parent))
      vfs_free_dentry_chain(parent);
    return -EINVAL;
  }

  child = kzalloc(sizeof(struct dentry), GFP_KERNEL);
  if (!child) {
    if (parent != root_dentry && !dentry_is_mount_root(parent))
      vfs_free_dentry_chain(parent);
    return -ENOMEM;
  }

  int i;
  for (i = 0; i < NAME_MAX && name[i]; i++)
    child->d_name[i] = name[i];
  child->d_name[i] = '\0';
  child->d_parent = parent;
  child->d_sb = parent->d_sb;

  if (parent->d_inode->i_op && parent->d_inode->i_op->lookup)
    parent->d_inode->i_op->lookup(parent->d_inode, child);
  if (child->d_inode) {
    vfs_free_dentry_chain(child);
    return -EEXIST;
  }

  if (!parent->d_inode->i_op || !parent->d_inode->i_op->symlink) {
    vfs_free_dentry_chain(child);
    return -ENOSYS;
  }

  ret = parent->d_inode->i_op->symlink(parent->d_inode, child, target);
  vfs_free_dentry_chain(child);
  return ret;
}

ssize_t vfs_readlink(const char *path, char *buf, size_t bufsiz) {
  struct dentry *dentry;
  int ret;

  if (!path || !buf)
    return -EINVAL;
  if (bufsiz > (size_t)INT32_MAX)
    return -EINVAL;

  dentry = vfs_lookup_path(path, NULL);
  if (!dentry || !dentry->d_inode) {
    if (dentry)
      vfs_free_dentry_chain(dentry);
    return -ENOENT;
  }

  if (!S_ISLNK(dentry->d_inode->i_mode)) {
    if (dentry != root_dentry && !dentry_is_mount_root(dentry))
      vfs_free_dentry_chain(dentry);
    return -EINVAL;
  }
  if (!dentry->d_inode->i_op || !dentry->d_inode->i_op->readlink) {
    if (dentry != root_dentry && !dentry_is_mount_root(dentry))
      vfs_free_dentry_chain(dentry);
    return -ENOSYS;
  }

  ret = dentry->d_inode->i_op->readlink(dentry, buf, (int)bufsiz);
  if (dentry != root_dentry && !dentry_is_mount_root(dentry))
    vfs_free_dentry_chain(dentry);
  return ret;
}

static int vfs_chmod_dentry(struct dentry *dentry, mode_t mode) {
  struct vfs_iattr attr;

  if (!dentry || !dentry->d_inode)
    return -EINVAL;
  if (!dentry->d_inode->i_op || !dentry->d_inode->i_op->setattr)
    return -ENOSYS;

  attr.valid = VFS_ATTR_MODE;
  attr.mode = mode & 07777;
  return dentry->d_inode->i_op->setattr(dentry, &attr);
}

static int vfs_chown_dentry(struct dentry *dentry, uid_t uid, gid_t gid) {
  struct vfs_iattr attr;

  if (!dentry || !dentry->d_inode)
    return -EINVAL;
  if (!dentry->d_inode->i_op || !dentry->d_inode->i_op->setattr)
    return -ENOSYS;

  memset(&attr, 0, sizeof(attr));
  if (uid != (uid_t)-1) {
    attr.valid |= VFS_ATTR_UID;
    attr.uid = uid;
  }
  if (gid != (gid_t)-1) {
    attr.valid |= VFS_ATTR_GID;
    attr.gid = gid;
  }
  if (!attr.valid)
    return 0;
  return dentry->d_inode->i_op->setattr(dentry, &attr);
}

int vfs_chmod_path(const char *path, mode_t mode) {
  struct dentry *dentry;
  int ret;

  if (!path)
    return -EINVAL;

  dentry = vfs_lookup_path(path, NULL);
  if (!dentry || !dentry->d_inode) {
    if (dentry)
      vfs_free_dentry_chain(dentry);
    return -ENOENT;
  }

  ret = vfs_chmod_dentry(dentry, mode);
  if (dentry != root_dentry && !dentry_is_mount_root(dentry))
    vfs_free_dentry_chain(dentry);
  return ret;
}

int vfs_chmod_file(struct file *file, mode_t mode) {
  if (!file || !file->f_dentry)
    return -EBADF;
  return vfs_chmod_dentry(file->f_dentry, mode);
}

int vfs_chown_path(const char *path, uid_t uid, gid_t gid) {
  struct dentry *dentry;
  int ret;

  if (!path)
    return -EINVAL;

  dentry = vfs_lookup_path(path, NULL);
  if (!dentry || !dentry->d_inode) {
    if (dentry)
      vfs_free_dentry_chain(dentry);
    return -ENOENT;
  }

  ret = vfs_chown_dentry(dentry, uid, gid);
  if (dentry != root_dentry && !dentry_is_mount_root(dentry))
    vfs_free_dentry_chain(dentry);
  return ret;
}

int vfs_chown_file(struct file *file, uid_t uid, gid_t gid) {
  if (!file || !file->f_dentry)
    return -EBADF;
  return vfs_chown_dentry(file->f_dentry, uid, gid);
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

  struct dentry *mountpoint = NULL;
  if (path_compare(target, "/") != 0) {
    mountpoint = vfs_lookup_path(target, NULL);
    if (!mountpoint || !mountpoint->d_inode) {
      if (mountpoint)
        vfs_free_dentry_chain(mountpoint);
      return -ENOENT;
    }
    if (!S_ISDIR(mountpoint->d_inode->i_mode)) {
      vfs_free_dentry_chain(mountpoint);
      return -ENOTDIR;
    }
  }

  /* Call filesystem's mount function */
  if (!fs->mount) {
    if (mountpoint)
      vfs_free_dentry_chain(mountpoint);
    return -ENOSYS;
  }

  struct super_block *sb = fs->mount(fs, flags, source, (void *)data);
  if (!sb) {
    if (mountpoint)
      vfs_free_dentry_chain(mountpoint);
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
  if (slot < 0) {
    if (mountpoint)
      vfs_free_dentry_chain(mountpoint);
    vfs_release_superblock(fs, sb);
    return -ENOMEM;
  }

  struct vfsmount *mnt = &mount_pool[slot];
  mount_slot_used[slot] = 1;

  mnt->mnt_root = sb->s_root;
  mnt->mnt_sb = sb;
  mnt->mnt_mountpoint = mountpoint;
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
      struct vfsmount *mnt = mounts[i];
      struct super_block *sb = mnt->mnt_sb;
      struct file_system_type *fs = sb ? sb->s_type : NULL;
      struct dentry *mountpoint = mnt->mnt_mountpoint;
      int slot = (int)(mounts[i] - mount_pool);

      if (root_mount == mnt) {
        root_mount = NULL;
        root_dentry = NULL;
      }
      mnt->mnt_root = NULL;
      mnt->mnt_sb = NULL;
      mnt->mnt_mountpoint = NULL;
      mnt->mnt_parent = NULL;
      mnt->mnt_devname[0] = '\0';
      mnt->mnt_target[0] = '\0';
      mnt->mnt_fstype[0] = '\0';
      mounts[i] = NULL;
      if (slot >= 0 && slot < MAX_MOUNTS)
        mount_slot_used[slot] = 0;
      if (mount_count > 0)
        mount_count--;
      if (mountpoint)
        vfs_free_dentry_chain(mountpoint);
      vfs_release_superblock(fs, sb);
      printk(KERN_INFO "VFS: Unmounted '%s'\n", target);
      return 0;
    }
  }

  return -ENOSYS;
}
