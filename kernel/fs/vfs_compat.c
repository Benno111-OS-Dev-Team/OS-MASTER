/*
 * OS8 VFS Compatibility Layer Implementation
 *
 * Provides simple VibeOS-compatible VFS functions.
 */

#include "../include/fs/vfs_compat.h"
#include "../include/printk.h"
#include "mm/kmalloc.h"

/* Current working directory */
static char cwd[256] = "/";

/* Static node pool for simplicity */
#define MAX_VFS_NODES 32
static vfs_node_t node_pool[MAX_VFS_NODES];
static int node_used[MAX_VFS_NODES] = {0};

/* Simple string compare */
/* Allocate a node from pool */
static vfs_node_t *alloc_node(void) {
  for (int i = 0; i < MAX_VFS_NODES; i++) {
    if (!node_used[i]) {
      node_used[i] = 1;
      node_pool[i].name[0] = '\0';
      node_pool[i].size = 0;
      node_pool[i].is_dir = 0;
      node_pool[i].internal = NULL;
      return &node_pool[i];
    }
  }
  return NULL;
}

/* Free a node back to pool */
static void free_node(vfs_node_t *node) {
  for (int i = 0; i < MAX_VFS_NODES; i++) {
    if (&node_pool[i] == node) {
      node_used[i] = 0;
      return;
    }
  }
}

/* Simple string copy */
static void strcpy_safe(char *dst, const char *src, size_t max) {
  size_t i = 0;
  while (src[i] && i < max - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

/* Look up a file by path */
vfs_node_t *vfs_lookup(const char *path) {
  if (!path || path[0] == '\0')
    return NULL;
  vfs_node_t *node = alloc_node();
  if (!node)
    return NULL;

  strcpy_safe(node->name, path, sizeof(node->name));
  node->size = 0;
  node->is_dir = 0;
  node->internal = NULL;

  /* Check for known paths */
  if (path[0] == '/' && path[1] == '\0') {
    node->is_dir = 1;
    return node;
  }
  /* Fallback: Check RAMFS for the file */
  extern int ramfs_lookup_path_info(const char *path, size_t *out_size,
                                    int *out_is_dir, void **out_data);
  size_t rsize = 0;
  int ris_dir = 0;
  void *rdata = NULL;
  if (ramfs_lookup_path_info(path, &rsize, &ris_dir, &rdata) == 0) {
    node->size = rsize;
    node->is_dir = ris_dir;
    node->internal = rdata; /* Store pointer to ramfs inode */
    return node;
  }

  /* File not found */
  free_node(node);
  return NULL;
}

/* Open a file handle */
vfs_node_t *vfs_open_handle(const char *path) { return vfs_lookup(path); }

/* Close a handle */
void vfs_close_handle(vfs_node_t *node) {
  if (node) {
    free_node(node);
  }
}

/* Read from file */
int vfs_read_compat(vfs_node_t *node, char *buf, size_t size, size_t offset) {
  if (!node || !buf)
    return -1;

  /* Check if this is a RAMFS node */
  if (node->internal && node->size > 0) {
    /* It's a RAMFS inode */
    struct ramfs_inode {
      unsigned long ino;
      unsigned int mode;
      unsigned int uid;
      unsigned int gid;
      size_t size;
      unsigned char *data;
      size_t data_capacity;
      /* ... other fields we don't need */
    };
    struct ramfs_inode *rnode = (struct ramfs_inode *)node->internal;

    if (rnode->data) {
      size_t avail = rnode->size > offset ? rnode->size - offset : 0;
      size_t to_read = size < avail ? size : avail;

      for (size_t i = 0; i < to_read; i++) {
        buf[i] = rnode->data[offset + i];
      }
      return (int)to_read;
    }
  }

  /* No data for other files */
  return 0;
}

/* Write to file */
int vfs_write_compat(vfs_node_t *node, const char *buf, size_t size) {
  (void)node;
  (void)buf;
  (void)size;
  return 0;
}

/* Check if directory */
int vfs_is_dir(vfs_node_t *node) { return node ? node->is_dir : 0; }

/* Create file */
vfs_node_t *vfs_create_compat(const char *path) {
  if (!path || path[0] == '\0')
    return NULL;
  extern int ramfs_create_file(const char *path, mode_t mode, const char *content);
  if (ramfs_create_file(path, 0644, NULL) != 0) {
    return NULL;
  }
  vfs_node_t *node = alloc_node();
  if (!node)
    return NULL;
  strcpy_safe(node->name, path, sizeof(node->name));
  node->size = 0;
  node->is_dir = 0;
  node->internal = NULL;
  return node;
}

/* Create directory */
vfs_node_t *vfs_mkdir_compat(const char *path) {
  if (!path || path[0] == '\0')
    return NULL;
  extern int ramfs_create_dir(const char *path, mode_t mode);
  if (ramfs_create_dir(path, 0755) != 0) {
    return NULL;
  }
  vfs_node_t *node = alloc_node();
  if (!node)
    return NULL;
  strcpy_safe(node->name, path, sizeof(node->name));
  node->size = 0;
  node->is_dir = 1;
  node->internal = NULL;
  return node;
}

/* Delete file */
int vfs_delete(const char *path) {
  extern int vfs_unlink(const char *path);
  return vfs_unlink(path);
}

/* Delete directory */
int vfs_delete_dir(const char *path) {
  extern int vfs_rmdir(const char *path);
  return vfs_rmdir(path);
}

/* Delete recursive */
int vfs_delete_recursive(const char *path) {
  (void)path;
  return -1;
}

/* Rename */
int vfs_rename_compat(const char *oldpath, const char *newname) {
  extern int vfs_rename(const char *oldpath, const char *newpath);
  return vfs_rename(oldpath, newname);
}

/* Read directory */
int vfs_readdir_compat(vfs_node_t *dir, int index, char *name, size_t name_size,
                       uint8_t *type) {
  (void)dir;
  (void)index;
  (void)name;
  (void)name_size;
  (void)type;
  return -1; /* End of directory */
}

/* Set CWD */
int vfs_set_cwd(const char *path) {
  strcpy_safe(cwd, path, sizeof(cwd));
  return 0;
}

/* Get CWD */
int vfs_get_cwd_path(char *buf, size_t size) {
  strcpy_safe(buf, cwd, size);
  return 0;
}
