/*
 * UnixOS Kernel - System Call Implementation
 */

#include "syscall/syscall.h"
#include "apps/kapi.h"
#include "arch/arch.h"
#include "drivers/uart.h"
#include "fs/vfs.h"
#include "mm/kmalloc.h"
#include "printk.h"
#include "sched/sched.h"
#include "sched/signal.h"
#include "string.h"

/* ===================================================================== */
/* File Descriptor Table */
/* ===================================================================== */

#define FD_CLOEXEC 1
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030
#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define AT_EACCESS 0x200
#define AT_EMPTY_PATH 0x1000
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#define TCGETS 0x5401
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define WAIT_WNOHANG 1
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define KERNEL_NSIG 32

static int init_task_files(struct task_struct *task) {
  if (!task)
    return -ESRCH;
  if (task->files_initialized)
    return 0;

  for (int i = 0; i < TASK_MAX_FDS; i++) {
    task->files[i].file = NULL;
    task->files[i].flags = 0;
    task->files[i].path[0] = '\0';
    task->files[i].in_use = 0;
  }

  task->files[0].in_use = 1; /* stdin */
  task->files[1].in_use = 1; /* stdout */
  task->files[1].flags = O_WRONLY;
  task->files[2].in_use = 1; /* stderr */
  task->files[2].flags = O_WRONLY;
  task->files_initialized = 1;
  return 0;
}

static struct task_struct *current_task_with_files(void) {
  struct task_struct *task = get_current();
  if (init_task_files(task) != 0)
    return NULL;
  return task;
}

static int alloc_fd_from(struct task_struct *task, int min_fd) {
  if (init_task_files(task) != 0)
    return -ESRCH;
  if (min_fd < 0 || min_fd >= TASK_MAX_FDS)
    return -EINVAL;

  for (int i = min_fd; i < TASK_MAX_FDS; i++) {
    if (!task->files[i].in_use) {
      task->files[i].in_use = 1;
      return i;
    }
  }
  return -EMFILE;
}

static int alloc_fd(struct task_struct *task) { return alloc_fd_from(task, 3); }

static void free_fd(struct task_struct *task, int fd) {
  if (task && fd >= 0 && fd < TASK_MAX_FDS) {
    task->files[fd].file = NULL;
    task->files[fd].flags = 0;
    task->files[fd].path[0] = '\0';
    task->files[fd].in_use = 0;
  }
}

static int close_fd_entry(struct task_struct *task, int fd) {
  if (!task || fd < 0 || fd >= TASK_MAX_FDS || !task->files[fd].in_use)
    return -EBADF;

  if (task->files[fd].file)
    vfs_close(task->files[fd].file);
  free_fd(task, fd);
  return 0;
}

static struct file *get_file(struct task_struct *task, int fd) {
  if (!task || fd < 0 || fd >= TASK_MAX_FDS || !task->files[fd].in_use) {
    return NULL;
  }
  return task->files[fd].file;
}

static int duplicate_fd(struct task_struct *task, int oldfd, int newfd,
                        int extra_flags) {
  struct file *file;

  if (!task || oldfd < 0 || oldfd >= TASK_MAX_FDS ||
      !task->files[oldfd].in_use)
    return -EBADF;
  if (newfd < 0 || newfd >= TASK_MAX_FDS)
    return -EBADF;

  if (task->files[newfd].in_use)
    close_fd_entry(task, newfd);

  file = task->files[oldfd].file;
  if (file)
    file->f_count.counter++;
  task->files[newfd].file = file;
  task->files[newfd].flags = task->files[oldfd].flags | extra_flags;
  strlcpy(task->files[newfd].path, task->files[oldfd].path,
          sizeof(task->files[newfd].path));
  task->files[newfd].in_use = 1;
  return newfd;
}

static int init_task_cwd(struct task_struct *task) {
  if (!task)
    return -ESRCH;
  if (!task->cwd_initialized || task->cwd[0] == '\0') {
    strlcpy(task->cwd, "/", sizeof(task->cwd));
    task->cwd_initialized = 1;
  }
  return 0;
}

static int path_push_component(char *dst, size_t dst_size, size_t *len,
                               const char *comp, size_t comp_len) {
  if (!dst || !len || !comp)
    return -EINVAL;
  if (comp_len == 0)
    return 0;

  if (comp_len == 1 && comp[0] == '.')
    return 0;

  if (comp_len == 2 && comp[0] == '.' && comp[1] == '.') {
    if (*len > 1) {
      while (*len > 1 && dst[*len - 1] == '/')
        (*len)--;
      while (*len > 1 && dst[*len - 1] != '/')
        (*len)--;
      if (*len > 1)
        (*len)--;
      dst[*len] = '\0';
    }
    return 0;
  }

  if (*len > 1) {
    if (*len + 1 >= dst_size)
      return -ENAMETOOLONG;
    dst[(*len)++] = '/';
  }

  if (*len + comp_len >= dst_size)
    return -ENAMETOOLONG;
  for (size_t i = 0; i < comp_len; i++)
    dst[(*len)++] = comp[i];
  dst[*len] = '\0';
  return 0;
}

static int append_normalized_components(char *dst, size_t dst_size, size_t *len,
                                        const char *path) {
  const char *p = path;

  while (p && *p) {
    const char *start;
    size_t comp_len;

    while (*p == '/')
      p++;
    start = p;
    while (*p && *p != '/')
      p++;
    comp_len = (size_t)(p - start);
    if (comp_len) {
      int ret = path_push_component(dst, dst_size, len, start, comp_len);
      if (ret < 0)
        return ret;
    }
  }

  return 0;
}

static int resolve_task_path(struct task_struct *task, const char *path,
                             char *dst, size_t dst_size) {
  size_t len;

  if (!path || !dst || dst_size < 2)
    return -EINVAL;
  if (path[0] == '\0')
    return -ENOENT;
  if (init_task_cwd(task) != 0)
    return -ESRCH;

  dst[0] = '/';
  dst[1] = '\0';
  len = 1;

  if (path[0] != '/') {
    int ret = append_normalized_components(dst, dst_size, &len, task->cwd);
    if (ret < 0)
      return ret;
  }

  return append_normalized_components(dst, dst_size, &len, path);
}

static int resolve_at_path(struct task_struct *task, int dirfd,
                           const char *path, char *dst, size_t dst_size) {
  size_t len;

  if (!path || !dst)
    return -EINVAL;
  if (path[0] == '/' || dirfd == AT_FDCWD)
    return resolve_task_path(task, path, dst, dst_size);
  if (path[0] == '\0')
    return -ENOENT;
  if (!task || dirfd < 0 || dirfd >= TASK_MAX_FDS ||
      !task->files[dirfd].in_use)
    return -EBADF;
  if (!task->files[dirfd].file || !task->files[dirfd].file->f_dentry ||
      !task->files[dirfd].file->f_dentry->d_inode ||
      !S_ISDIR(task->files[dirfd].file->f_dentry->d_inode->i_mode))
    return -ENOTDIR;
  if (task->files[dirfd].path[0] == '\0')
    return -EINVAL;

  dst[0] = '/';
  dst[1] = '\0';
  len = 1;

  int ret = append_normalized_components(dst, dst_size, &len,
                                         task->files[dirfd].path);
  if (ret < 0)
    return ret;
  return append_normalized_components(dst, dst_size, &len, path);
}

/* ===================================================================== */
/* User Pointer Validation */
/* ===================================================================== */

/* Valid memory regions for user processes */
#define KERNEL_START 0x40000000UL
#define KERNEL_END 0x50000000UL

/* Check if pointer is in valid user-accessible memory range */
static int is_valid_user_ptr(uint64_t ptr, size_t len) {
  if (ptr == 0)
    return 0;

  /* Prevent overflow */
  if (len > 0 && ptr > UINT64_MAX - len)
    return 0;
  uint64_t end = ptr + len;

  /* Allow user heap region (0x10000000 - 0x14000000) */
  if (ptr >= 0x10000000UL && end <= 0x14000000UL) {
    return 1;
  }

  /* Allow program load region (0x44000000+) */
  if (ptr >= 0x44000000UL && end < 0x50000000UL) {
    return 1;
  }

  /* Block access to kernel memory */
  if (ptr >= KERNEL_START && ptr < KERNEL_END)
    return 0;

  /* Allow other reasonable addresses (stack, etc.) */
  return 1;
}

static int copy_user_string(uint64_t user_ptr, char *dst, size_t dst_size) {
  const char *src = (const char *)(uintptr_t)user_ptr;

  if (!dst || dst_size == 0 || !is_valid_user_ptr(user_ptr, 1))
    return -EFAULT;

  for (size_t i = 0; i < dst_size; i++) {
    if (!is_valid_user_ptr(user_ptr + i, 1))
      return -EFAULT;
    dst[i] = src[i];
    if (dst[i] == '\0')
      return 0;
  }

  dst[dst_size - 1] = '\0';
  return -ENAMETOOLONG;
}

/* ===================================================================== */
/* File metadata helpers */
/* ===================================================================== */

struct linux_stat {
  dev_t st_dev;
  ino_t st_ino;
  mode_t st_mode;
  nlink_t st_nlink;
  uid_t st_uid;
  gid_t st_gid;
  dev_t st_rdev;
  unsigned long __pad;
  loff_t st_size;
  blksize_t st_blksize;
  int __pad2;
  blkcnt_t st_blocks;
  struct timespec st_atim;
  struct timespec st_mtim;
  struct timespec st_ctim;
  unsigned __stat_unused[2];
};

static long copy_inode_stat(const struct inode *inode, uint64_t statbuf) {
  struct linux_stat *st;
  blksize_t blksize = 512;

  if (!inode)
    return -ENOENT;
  if (!is_valid_user_ptr(statbuf, sizeof(struct linux_stat)))
    return -EFAULT;

  if (inode->i_blksize > 0)
    blksize = inode->i_blksize;
  else if (inode->i_sb && inode->i_sb->s_blocksize > 0)
    blksize = inode->i_sb->s_blocksize;

  st = (struct linux_stat *)(uintptr_t)statbuf;
  memset(st, 0, sizeof(*st));
  st->st_dev = inode->i_sb ? inode->i_sb->s_dev : 0;
  st->st_ino = inode->i_ino;
  st->st_mode = inode->i_mode;
  st->st_nlink = inode->i_nlink;
  st->st_uid = inode->i_uid;
  st->st_gid = inode->i_gid;
  st->st_rdev = inode->i_rdev;
  st->st_size = inode->i_size;
  st->st_blksize = blksize;
  st->st_blocks = inode->i_blocks;
  st->st_atim = inode->i_atime;
  st->st_mtim = inode->i_mtime;
  st->st_ctim = inode->i_ctime;
  return 0;
}

static long copy_file_stat(const struct file *file, uint64_t statbuf) {
  if (!file || !file->f_dentry)
    return -EBADF;
  return copy_inode_stat(file->f_dentry->d_inode, statbuf);
}

static long check_inode_access(const struct inode *inode, int mode) {
  mode_t allowed;
  struct task_struct *task;

  if (!inode)
    return -ENOENT;
  if (mode == F_OK)
    return 0;
  if (mode & ~(R_OK | W_OK | X_OK))
    return -EINVAL;

  task = get_current();
  if (task && task->uid == 0) {
    if ((mode & X_OK) && !(inode->i_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
      return -EACCES;
    return 0;
  }

  if (task && task->uid == inode->i_uid)
    allowed = (inode->i_mode & S_IRWXU) >> 6;
  else if (task && task->gid == inode->i_gid)
    allowed = (inode->i_mode & S_IRWXG) >> 3;
  else
    allowed = inode->i_mode & S_IRWXO;

  if ((mode & R_OK) && !(allowed & 4))
    return -EACCES;
  if ((mode & W_OK) && !(allowed & 2))
    return -EACCES;
  if ((mode & X_OK) && !(allowed & 1))
    return -EACCES;
  return 0;
}

struct linux_dirent64 {
  ino_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[];
};

struct getdents64_ctx {
  char *buf;
  size_t buflen;
  size_t bytes;
  loff_t start_pos;
  loff_t seen;
  loff_t next_pos;
  int error;
  int full;
};

static int getdents64_fill(void *ctx, const char *name, int len, loff_t offset,
                           ino_t ino, unsigned type) {
  (void)offset;

  struct getdents64_ctx *state = (struct getdents64_ctx *)ctx;
  loff_t entry_pos;
  size_t name_len;
  size_t reclen;
  struct linux_dirent64 *dirent;

  if (!state || !name || len < 0)
    return -EINVAL;

  entry_pos = state->seen++;
  if (entry_pos < state->start_pos)
    return 0;
  if (state->full)
    return 0;

  name_len = (size_t)len;
  if (name_len > NAME_MAX) {
    state->error = -ENAMETOOLONG;
    state->full = 1;
    return state->error;
  }

  reclen = ALIGN(offsetof(struct linux_dirent64, d_name) + name_len + 1,
                 sizeof(uint64_t));
  if (reclen > (size_t)UINT16_MAX || reclen > state->buflen - state->bytes) {
    if (state->bytes == 0)
      state->error = -EINVAL;
    state->full = 1;
    return state->error;
  }

  dirent = (struct linux_dirent64 *)(state->buf + state->bytes);
  memset(dirent, 0, reclen);
  dirent->d_ino = ino;
  dirent->d_off = entry_pos + 1;
  dirent->d_reclen = (unsigned short)reclen;
  dirent->d_type = (unsigned char)type;
  for (size_t i = 0; i < name_len; i++)
    dirent->d_name[i] = name[i];
  dirent->d_name[name_len] = '\0';

  state->bytes += reclen;
  state->next_pos = entry_pos + 1;
  return 0;
}

/* ===================================================================== */
/* System call table */
/* ===================================================================== */

typedef long (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t);

static syscall_fn_t syscall_table[NR_syscalls];

/* ===================================================================== */
/* System call implementations */
/* ===================================================================== */

static long sys_read(uint64_t fd, uint64_t buf, uint64_t count, uint64_t a3,
                     uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;
  if (fd >= TASK_MAX_FDS)
    return -EBADF;

  /* Validate user buffer */
  if (!is_valid_user_ptr(buf, count)) {
    return -EFAULT;
  }

  /* Handle stdin specially */
  if (fd < TASK_MAX_FDS && task->files[fd].in_use &&
      !task->files[fd].file &&
      (task->files[fd].flags & O_ACCMODE) != O_WRONLY) {
    kapi_t *api = kapi_get();
    char *p = (char *)buf;
    size_t n = 0;

    /* Block until we get at least one character */
    while (n < count) {
      /* Poll for input */
      int c = api->getc();

      if (c >= 0) {
        /* Got a character */
        p[n++] = (char)c;

        /* Return immediately on newline for line-buffering behavior */
        if (c == '\n' || c == '\r') {
          /* Normalize \r to \n */
          if (c == '\r')
            p[n - 1] = '\n';
          return n;
        }
      } else {
        /* No input available */
        if (n > 0) {
          /* We already read something, return it */
          return n;
        }

        /* Nothing read yet, yield and wait */
        extern void process_yield(void);
        process_yield();
      }
    }
    return n;
  }

  struct file *f = get_file(task, (int)fd);
  if (!f) {
    return -EBADF;
  }

  return vfs_read(f, (char *)buf, count);
}

static long sys_write(uint64_t fd, uint64_t buf, uint64_t count, uint64_t a3,
                      uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;
  if (fd >= TASK_MAX_FDS)
    return -EBADF;

  if (count > 0 && !is_valid_user_ptr(buf, count)) {
    return -EFAULT;
  }

  /* Special case: stdout/stderr (fd 1 and 2) go to console */
  if (fd < TASK_MAX_FDS && task->files[fd].in_use &&
      !task->files[fd].file &&
      (task->files[fd].flags & O_ACCMODE) != O_RDONLY) {
    const char *str = (const char *)buf;
    for (size_t i = 0; i < count; i++) {
      uart_putc(str[i]);
    }
    return count;
  }

  struct file *f = get_file(task, (int)fd);
  if (!f) {
    return -EBADF;
  }

  return vfs_write(f, (const char *)buf, count);
}

static long sys_openat(uint64_t dirfd, uint64_t pathname, uint64_t flags,
                       uint64_t mode, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;
  char user_path[PATH_MAX];
  char path[PATH_MAX];
  long ret = copy_user_string(pathname, user_path, sizeof(user_path));
  if (ret < 0) {
    return ret;
  }
  ret = resolve_at_path(task, (int)dirfd, user_path, path, sizeof(path));
  if (ret < 0)
    return ret;

  /* Allocate file descriptor */
  int fd = alloc_fd(task);
  if (fd < 0) {
    return fd;
  }

  /* Open the file */
  struct file *f = vfs_open(path, (int)flags, (mode_t)mode);
  if (!f) {
    free_fd(task, fd);
    return -ENOENT;
  }

  task->files[fd].file = f;
  task->files[fd].flags = (int)flags;
  strlcpy(task->files[fd].path, path, sizeof(task->files[fd].path));

  return fd;
}

static long sys_close(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;
  if (fd >= TASK_MAX_FDS)
    return -EBADF;

  return close_fd_entry(task, (int)fd);
}

extern int do_pipe(struct file **read_file, struct file **write_file);

static long sys_pipe2(uint64_t pipefd, uint64_t flags, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  struct file *read_file = NULL;
  struct file *write_file = NULL;
  int read_fd;
  int write_fd;
  int ret;

  if (!task)
    return -ESRCH;
  if (flags & ~(uint64_t)(O_CLOEXEC | O_NONBLOCK))
    return -EINVAL;
  if (!is_valid_user_ptr(pipefd, sizeof(int) * 2))
    return -EFAULT;

  ret = do_pipe(&read_file, &write_file);
  if (ret < 0)
    return ret;

  read_fd = alloc_fd_from(task, 0);
  if (read_fd < 0) {
    vfs_close(read_file);
    vfs_close(write_file);
    return read_fd;
  }
  write_fd = alloc_fd_from(task, 0);
  if (write_fd < 0) {
    free_fd(task, read_fd);
    vfs_close(read_file);
    vfs_close(write_file);
    return write_fd;
  }

  read_file->f_flags |= (int)(flags & O_NONBLOCK);
  write_file->f_flags |= (int)(flags & O_NONBLOCK);

  task->files[read_fd].file = read_file;
  task->files[read_fd].flags = O_RDONLY | (int)flags;
  task->files[write_fd].file = write_file;
  task->files[write_fd].flags = O_WRONLY | (int)flags;

  int *user_pipefd = (int *)(uintptr_t)pipefd;
  user_pipefd[0] = read_fd;
  user_pipefd[1] = write_fd;
  return 0;
}

static long sys_dup(uint64_t oldfd, uint64_t a1, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;
  if (oldfd >= TASK_MAX_FDS)
    return -EBADF;

  int newfd = alloc_fd_from(task, 0);
  if (newfd < 0)
    return newfd;

  int ret = duplicate_fd(task, (int)oldfd, newfd, 0);
  if (ret < 0) {
    free_fd(task, newfd);
    return ret;
  }
  return ret;
}

static long sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags,
                     uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;
  if (oldfd >= TASK_MAX_FDS || newfd >= TASK_MAX_FDS)
    return -EBADF;
  if (oldfd == newfd)
    return -EINVAL;
  if (flags & ~(uint64_t)O_CLOEXEC)
    return -EINVAL;

  return duplicate_fd(task, (int)oldfd, (int)newfd,
                      (flags & O_CLOEXEC) ? O_CLOEXEC : 0);
}

static long sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t a3,
                      uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  int fd_i = (int)fd;

  if (!task)
    return -ESRCH;
  if (fd >= TASK_MAX_FDS)
    return -EBADF;
  if (fd_i < 0 || fd_i >= TASK_MAX_FDS || !task->files[fd_i].in_use)
    return -EBADF;

  switch (cmd) {
  case F_DUPFD: {
    int newfd = alloc_fd_from(task, (int)arg);
    if (newfd < 0)
      return newfd;
    int ret = duplicate_fd(task, fd_i, newfd, 0);
    if (ret < 0)
      free_fd(task, newfd);
    return ret;
  }
  case F_DUPFD_CLOEXEC: {
    int newfd = alloc_fd_from(task, (int)arg);
    if (newfd < 0)
      return newfd;
    int ret = duplicate_fd(task, fd_i, newfd, O_CLOEXEC);
    if (ret < 0)
      free_fd(task, newfd);
    return ret;
  }
  case F_GETFD:
    return (task->files[fd_i].flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
  case F_SETFD:
    if (arg & FD_CLOEXEC)
      task->files[fd_i].flags |= O_CLOEXEC;
    else
      task->files[fd_i].flags &= ~O_CLOEXEC;
    return 0;
  case F_GETFL:
    if (task->files[fd_i].file)
      return task->files[fd_i].file->f_flags;
    return task->files[fd_i].flags & ~O_CLOEXEC;
  case F_SETFL:
    task->files[fd_i].flags =
        (task->files[fd_i].flags & O_CLOEXEC) | ((int)arg & ~O_CLOEXEC);
    if (task->files[fd_i].file) {
      int access_mode = task->files[fd_i].file->f_flags & O_ACCMODE;
      task->files[fd_i].file->f_flags = access_mode | ((int)arg & ~O_CLOEXEC);
    }
    return 0;
  default:
    return -EINVAL;
  }
}

static long sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg, uint64_t a3,
                      uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
  };

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;
  if (fd >= TASK_MAX_FDS)
    return -EBADF;
  if (!task->files[(int)fd].in_use)
    return -EBADF;
  if (task->files[(int)fd].file)
    return -ENOTTY;

  switch (request) {
  case TCGETS:
    if (!is_valid_user_ptr(arg, 60))
      return -EFAULT;
    memset((void *)(uintptr_t)arg, 0, 60);
    return 0;
  case TIOCGWINSZ: {
    if (!is_valid_user_ptr(arg, sizeof(struct winsize)))
      return -EFAULT;
    struct winsize *ws = (struct winsize *)(uintptr_t)arg;
    ws->ws_row = 25;
    ws->ws_col = 80;
    ws->ws_xpixel = 0;
    ws->ws_ypixel = 0;
    return 0;
  }
  case TIOCSWINSZ:
    if (!is_valid_user_ptr(arg, sizeof(struct winsize)))
      return -EFAULT;
    return 0;
  default:
    return -ENOTTY;
  }
}

static long sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                      uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;
  if (fd >= TASK_MAX_FDS)
    return -EBADF;

  struct file *f = get_file(task, (int)fd);
  if (!f) {
    return -EBADF;
  }

  return vfs_lseek(f, (loff_t)offset, (int)whence);
}

static long sys_getdents64(uint64_t fd, uint64_t dirp, uint64_t count,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  struct getdents64_ctx ctx;
  int ret;

  if (!task)
    return -ESRCH;
  if (fd >= TASK_MAX_FDS)
    return -EBADF;
  if (count == 0 || count > SSIZE_MAX)
    return -EINVAL;
  if (!is_valid_user_ptr(dirp, (size_t)count))
    return -EFAULT;

  struct file *f = get_file(task, (int)fd);
  if (!f)
    return -EBADF;
  if (!f->f_dentry || !f->f_dentry->d_inode ||
      !S_ISDIR(f->f_dentry->d_inode->i_mode))
    return -ENOTDIR;

  memset(&ctx, 0, sizeof(ctx));
  ctx.buf = (char *)(uintptr_t)dirp;
  ctx.buflen = (size_t)count;
  ctx.start_pos = f->f_pos;
  ctx.next_pos = f->f_pos;

  ret = vfs_readdir(f, &ctx, getdents64_fill);
  if (ret < 0)
    return ret;
  if (ctx.bytes == 0 && ctx.error < 0)
    return ctx.error;
  if (ctx.bytes > 0)
    f->f_pos = ctx.next_pos;

  return (long)ctx.bytes;
}

static long sys_fstat(uint64_t fd, uint64_t statbuf, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;
  if (fd >= TASK_MAX_FDS)
    return -EBADF;

  struct file *f = get_file(task, (int)fd);
  if (!f)
    return -EBADF;

  return copy_file_stat(f, statbuf);
}

static long sys_newfstatat(uint64_t dirfd, uint64_t pathname, uint64_t statbuf,
                           uint64_t flags, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  char user_path[PATH_MAX];
  char path[PATH_MAX];
  long ret;

  if (!task)
    return -ESRCH;
  if (flags & ~(uint64_t)(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
    return -EINVAL;

  ret = copy_user_string(pathname, user_path, sizeof(user_path));
  if (ret < 0)
    return ret;

  if ((flags & AT_EMPTY_PATH) && user_path[0] == '\0') {
    if ((int64_t)dirfd == AT_FDCWD) {
      ret = init_task_cwd(task);
      if (ret < 0)
        return ret;

      struct file *cwd = vfs_open(task->cwd, O_RDONLY | O_DIRECTORY, 0);
      if (!cwd)
        return -ENOENT;
      ret = copy_file_stat(cwd, statbuf);
      vfs_close(cwd);
      return ret;
    }

    struct file *f = get_file(task, (int)dirfd);
    if (!f)
      return -EBADF;
    return copy_file_stat(f, statbuf);
  }

  ret = resolve_at_path(task, (int)dirfd, user_path, path, sizeof(path));
  if (ret < 0)
    return ret;

  struct file *f = vfs_open(path, O_RDONLY, 0);
  if (!f)
    return -ENOENT;

  ret = copy_file_stat(f, statbuf);
  vfs_close(f);
  return ret;
}

static long sys_exit(uint64_t error_code, uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  printk(KERN_INFO "sys_exit: code=%llu\n", (unsigned long long)error_code);
  exit_task((int)error_code);

  /* Never reached */
  return 0;
}

static long sys_exit_group(uint64_t error_code, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  return sys_exit(error_code, a1, a2, a3, a4, a5);
}

static long sys_getpid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  return current ? current->pid : -1;
}

static long sys_getppid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  if (current && current->parent) {
    return current->parent->pid;
  }
  return 0;
}

static long sys_getuid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  return current ? current->uid : 0;
}

static long sys_getgid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  return current ? current->gid : 0;
}

static long sys_gettid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  return current ? current->pid : -1;
}

static long send_signal_to_pid(pid_t pid, int sig) {
  struct task_struct *task;

  if (sig < 0 || sig >= KERNEL_NSIG)
    return -EINVAL;
  if (pid == 0) {
    task = get_current();
  } else if (pid > 0) {
    task = get_task_by_pid(pid);
  } else {
    return -ENOSYS;
  }

  if (!task)
    return -ESRCH;
  return kill_task(task, sig) == 0 ? 0 : -EINVAL;
}

static long sys_kill(uint64_t pid, uint64_t sig, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  return send_signal_to_pid((pid_t)(int64_t)pid, (int)sig);
}

static long sys_tkill(uint64_t tid, uint64_t sig, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if ((pid_t)tid <= 0)
    return -EINVAL;
  return send_signal_to_pid((pid_t)tid, (int)sig);
}

static long sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  if ((pid_t)tgid <= 0 || (pid_t)tid <= 0)
    return -EINVAL;

  struct task_struct *task = get_task_by_pid((pid_t)tid);
  if (!task)
    return -ESRCH;
  if (task->tgid != (pid_t)tgid)
    return -ESRCH;
  if (sig >= KERNEL_NSIG)
    return -EINVAL;

  return kill_task(task, (int)sig) == 0 ? 0 : -EINVAL;
}

static long sys_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t oldset,
                               uint64_t sigsetsize, uint64_t a4,
                               uint64_t a5) {
  (void)a4;
  (void)a5;

  ksigset_t new_mask = 0;
  ksigset_t old_mask = 0;
  const ksigset_t *new_mask_ptr = NULL;

  if (sigsetsize < sizeof(ksigset_t))
    return -EINVAL;
  if (set) {
    if (!is_valid_user_ptr(set, sizeof(ksigset_t)))
      return -EFAULT;
    new_mask = *(const ksigset_t *)(uintptr_t)set;
    new_mask_ptr = &new_mask;
  }
  if (oldset && !is_valid_user_ptr(oldset, sizeof(ksigset_t)))
    return -EFAULT;
  if (how > SIG_SETMASK)
    return -EINVAL;

  int ret = sigprocmask((int)how, new_mask_ptr, oldset ? &old_mask : NULL);
  if (ret < 0)
    return -EINVAL;
  if (oldset)
    *(ksigset_t *)(uintptr_t)oldset = old_mask;
  return 0;
}

static long sys_rt_sigpending(uint64_t set, uint64_t sigsetsize, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (sigsetsize < sizeof(ksigset_t))
    return -EINVAL;
  if (!is_valid_user_ptr(set, sizeof(ksigset_t)))
    return -EFAULT;

  struct task_struct *task = get_current();
  if (!task)
    return -ESRCH;
  *(ksigset_t *)(uintptr_t)set = signal_pending_mask(task);
  return 0;
}

static long sys_rt_sigaction(uint64_t sig, uint64_t act, uint64_t oldact,
                             uint64_t sigsetsize, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;

  struct k_sigaction new_action;
  struct k_sigaction old_action;
  const struct k_sigaction *new_action_ptr = NULL;

  if (sigsetsize < sizeof(ksigset_t))
    return -EINVAL;
  if (act) {
    if (!is_valid_user_ptr(act, sizeof(new_action)))
      return -EFAULT;
    new_action = *(const struct k_sigaction *)(uintptr_t)act;
    new_action_ptr = &new_action;
  }
  if (oldact && !is_valid_user_ptr(oldact, sizeof(old_action)))
    return -EFAULT;

  int ret = sigaction_syscall((int)sig, new_action_ptr,
                              oldact ? &old_action : NULL);
  if (ret < 0)
    return -EINVAL;
  if (oldact)
    *(struct k_sigaction *)(uintptr_t)oldact = old_action;
  return 0;
}

static long sys_getcwd(uint64_t buf, uint64_t size, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = get_current();
  if (init_task_cwd(task) != 0)
    return -ESRCH;
  if (size == 0)
    return -EINVAL;
  if (!is_valid_user_ptr(buf, (size_t)size))
    return -EFAULT;

  size_t len = strlen(task->cwd) + 1;
  if (len > (size_t)size)
    return -ENAMETOOLONG;

  strlcpy((char *)(uintptr_t)buf, task->cwd, (size_t)size);
  return (long)buf;
}

static long sys_chdir(uint64_t pathname, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = get_current();
  char user_path[PATH_MAX];
  char path[PATH_MAX];
  long ret = copy_user_string(pathname, user_path, sizeof(user_path));
  if (ret < 0)
    return ret;
  ret = resolve_task_path(task, user_path, path, sizeof(path));
  if (ret < 0)
    return ret;

  struct file *dir = vfs_open(path, O_RDONLY | O_DIRECTORY, 0);
  if (!dir)
    return -ENOENT;

  if (!dir->f_dentry || !dir->f_dentry->d_inode ||
      !S_ISDIR(dir->f_dentry->d_inode->i_mode)) {
    vfs_close(dir);
    return -ENOTDIR;
  }

  vfs_close(dir);
  strlcpy(task->cwd, path, sizeof(task->cwd));
  task->cwd_initialized = 1;
  return 0;
}

static long sys_fchdir(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;
  if (fd >= TASK_MAX_FDS)
    return -EBADF;

  struct file *dir = get_file(task, (int)fd);
  if (!dir)
    return -EBADF;
  if (!dir->f_dentry || !dir->f_dentry->d_inode ||
      !S_ISDIR(dir->f_dentry->d_inode->i_mode))
    return -ENOTDIR;
  if (task->files[(int)fd].path[0] == '\0')
    return -EINVAL;

  strlcpy(task->cwd, task->files[(int)fd].path, sizeof(task->cwd));
  task->cwd_initialized = 1;
  return 0;
}

static long sys_mkdirat(uint64_t dirfd, uint64_t pathname, uint64_t mode,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  char user_path[PATH_MAX];
  char path[PATH_MAX];
  long ret;

  if (!task)
    return -ESRCH;
  ret = copy_user_string(pathname, user_path, sizeof(user_path));
  if (ret < 0)
    return ret;
  ret = resolve_at_path(task, (int)dirfd, user_path, path, sizeof(path));
  if (ret < 0)
    return ret;
  return vfs_mkdir(path, (mode_t)mode);
}

static long sys_unlinkat(uint64_t dirfd, uint64_t pathname, uint64_t flags,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  char user_path[PATH_MAX];
  char path[PATH_MAX];
  long ret;

  if (!task)
    return -ESRCH;
  if (flags & ~(uint64_t)AT_REMOVEDIR)
    return -EINVAL;
  ret = copy_user_string(pathname, user_path, sizeof(user_path));
  if (ret < 0)
    return ret;
  ret = resolve_at_path(task, (int)dirfd, user_path, path, sizeof(path));
  if (ret < 0)
    return ret;
  return (flags & AT_REMOVEDIR) ? vfs_rmdir(path) : vfs_unlink(path);
}

static long sys_renameat(uint64_t olddirfd, uint64_t oldpath_ptr,
                         uint64_t newdirfd, uint64_t newpath_ptr, uint64_t a4,
                         uint64_t a5) {
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  char old_user_path[PATH_MAX];
  char new_user_path[PATH_MAX];
  char old_path[PATH_MAX];
  char new_path[PATH_MAX];
  long ret;

  if (!task)
    return -ESRCH;
  ret = copy_user_string(oldpath_ptr, old_user_path, sizeof(old_user_path));
  if (ret < 0)
    return ret;
  ret = copy_user_string(newpath_ptr, new_user_path, sizeof(new_user_path));
  if (ret < 0)
    return ret;
  ret = resolve_at_path(task, (int)olddirfd, old_user_path, old_path,
                        sizeof(old_path));
  if (ret < 0)
    return ret;
  ret = resolve_at_path(task, (int)newdirfd, new_user_path, new_path,
                        sizeof(new_path));
  if (ret < 0)
    return ret;
  return vfs_rename(old_path, new_path);
}

static long sys_faccessat(uint64_t dirfd, uint64_t pathname, uint64_t mode,
                          uint64_t flags, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  char user_path[PATH_MAX];
  char path[PATH_MAX];
  struct file *file;
  long ret;

  if (!task)
    return -ESRCH;
  if (flags & ~(uint64_t)AT_EACCESS)
    return -EINVAL;
  if (mode & ~(uint64_t)(R_OK | W_OK | X_OK))
    return -EINVAL;

  ret = copy_user_string(pathname, user_path, sizeof(user_path));
  if (ret < 0)
    return ret;
  ret = resolve_at_path(task, (int)dirfd, user_path, path, sizeof(path));
  if (ret < 0)
    return ret;

  file = vfs_open(path, O_RDONLY, 0);
  if (!file)
    return -ENOENT;
  ret = file->f_dentry ? check_inode_access(file->f_dentry->d_inode, (int)mode)
                       : -ENOENT;
  vfs_close(file);
  return ret;
}

static long sys_readlinkat(uint64_t dirfd, uint64_t pathname, uint64_t buf,
                           uint64_t bufsiz, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  char user_path[PATH_MAX];
  char path[PATH_MAX];
  struct file *file;
  long ret;

  if (!task)
    return -ESRCH;
  if (bufsiz == 0)
    return -EINVAL;
  if (!is_valid_user_ptr(buf, (size_t)bufsiz))
    return -EFAULT;

  ret = copy_user_string(pathname, user_path, sizeof(user_path));
  if (ret < 0)
    return ret;
  ret = resolve_at_path(task, (int)dirfd, user_path, path, sizeof(path));
  if (ret < 0)
    return ret;

  file = vfs_open(path, O_RDONLY, 0);
  if (!file)
    return -ENOENT;
  if (!file->f_dentry || !file->f_dentry->d_inode ||
      !S_ISLNK(file->f_dentry->d_inode->i_mode) ||
      !file->f_dentry->d_inode->i_op ||
      !file->f_dentry->d_inode->i_op->readlink) {
    vfs_close(file);
    return -EINVAL;
  }

  ret = file->f_dentry->d_inode->i_op->readlink(
      file->f_dentry, (char *)(uintptr_t)buf, (int)bufsiz);
  vfs_close(file);
  return ret;
}

/* Userspace heap management - dedicated region for userspace processes */
#define USER_HEAP_START 0x10000000UL /* 256MB mark */
#define USER_HEAP_SIZE 0x04000000UL  /* 64MB heap */
static uint64_t user_brk_current = USER_HEAP_START;
static uint64_t user_mmap_current =
    USER_HEAP_START + USER_HEAP_SIZE / 2; /* mmap from middle */

static long sys_brk(uint64_t brk, uint64_t a1, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  /* If brk is 0 or less than start, return current brk */
  if (brk == 0 || brk < USER_HEAP_START) {
    return user_brk_current;
  }

  /* Check bounds */
  if (brk > USER_HEAP_START + USER_HEAP_SIZE / 2) {
    /* Would overlap with mmap region */
    return user_brk_current;
  }

  /* Extend brk */
  user_brk_current = brk;
  return user_brk_current;
}

static long sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags,
                     uint64_t fd, uint64_t offset) {
  (void)addr;
  (void)prot;
  (void)offset;

/* Only support anonymous mappings for now */
#define MAP_ANONYMOUS 0x20
  if (!(flags & MAP_ANONYMOUS) || (int64_t)fd != -1) {
    printk(KERN_DEBUG "sys_mmap: only anonymous mappings supported\n");
    return -ENOSYS;
  }

  if (len == 0 || len > UINT64_MAX - (PAGE_SIZE - 1))
    return -EINVAL;

/* Align len to page size */
  len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  /* Check bounds */
  if (len > (USER_HEAP_START + USER_HEAP_SIZE) - user_mmap_current) {
    printk(KERN_WARNING "sys_mmap: out of memory\n");
    return -ENOMEM;
  }

  /* Allocate from mmap region */
  uint64_t result = user_mmap_current;
  user_mmap_current += len;

  /* Zero the memory */
  uint8_t *p = (uint8_t *)result;
  for (size_t i = 0; i < len; i++)
    p[i] = 0;

  return result;
}

static long sys_munmap(uint64_t addr, uint64_t len, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)addr;
  (void)len;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  /* Current heap allocator does not reclaim individual mappings yet. */
  return 0;
}

extern long do_fork(unsigned long flags);

static long sys_clone(uint64_t flags, uint64_t stack, uint64_t ptid,
                      uint64_t tls, uint64_t ctid, uint64_t a5) {
  (void)stack;
  (void)ptid;
  (void)tls;
  (void)ctid;
  (void)a5;

  return do_fork((unsigned long)flags);
}

static long sys_wait4(uint64_t pid, uint64_t status, uint64_t options,
                      uint64_t rusage, uint64_t a4, uint64_t a5) {
  (void)rusage;
  (void)a4;
  (void)a5;

  if (status && !is_valid_user_ptr(status, sizeof(int)))
    return -EFAULT;
  if (options & ~(uint64_t)WAIT_WNOHANG)
    return -EINVAL;

  return sched_wait4((pid_t)(int64_t)pid, status ? (int *)(uintptr_t)status : NULL,
                     (int)options);
}

/* Forward declarations for ELF loader */
extern int elf_validate(const void *data, size_t size);
extern uint64_t elf_calc_size(const void *data, size_t size);
extern int elf_load_at(const void *data, size_t size, uint64_t load_base,
                       void *info);

static long sys_execve(uint64_t filename, uint64_t argv, uint64_t envp,
                       uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)argv;
  (void)envp;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = get_current();
  char user_path[PATH_MAX];
  char path[PATH_MAX];
  long path_ret = copy_user_string(filename, user_path, sizeof(user_path));
  if (path_ret < 0)
    return path_ret;
  path_ret = resolve_task_path(task, user_path, path, sizeof(path));
  if (path_ret < 0)
    return path_ret;

  printk(KERN_INFO "sys_execve: loading '%s'\n", path);

  /* Open the file */
  struct file *f = vfs_open(path, O_RDONLY, 0);
  if (!f) {
    printk(KERN_ERR "sys_execve: cannot open '%s'\n", path);
    return -ENOENT;
  }

  /* Get file size via dentry->inode */
  size_t file_size = 0;
  if (f->f_dentry && f->f_dentry->d_inode) {
    file_size = f->f_dentry->d_inode->i_size;
  }
  if (file_size == 0 || file_size > 64 * 1024 * 1024) {
    vfs_close(f);
    return -ENOEXEC;
  }

  /* Allocate buffer and read file */
  uint8_t *buf = kmalloc(file_size);
  if (!buf) {
    vfs_close(f);
    return -ENOMEM;
  }

  ssize_t bytes_read = vfs_read(f, (char *)buf, file_size);
  vfs_close(f);

  if (bytes_read != (ssize_t)file_size) {
    kfree(buf);
    return -EIO;
  }

  /* Validate ELF */
  int ret = elf_validate(buf, file_size);
  if (ret != 0) {
    printk(KERN_ERR "sys_execve: invalid ELF (error %d)\n", ret);
    kfree(buf);
    return -ENOEXEC;
  }

  /* Calculate memory needed */
  uint64_t mem_size = elf_calc_size(buf, file_size);
  if (mem_size == 0) {
    kfree(buf);
    return -ENOEXEC;
  }

  /* Load at user code base */
  typedef struct {
    uint64_t entry;
    uint64_t load_base;
    uint64_t load_size;
  } elf_load_info_t;

  elf_load_info_t info;
  ret = elf_load_at(buf, file_size, USER_CODE_BASE, &info);
  kfree(buf);

  if (ret != 0) {
    printk(KERN_ERR "sys_execve: ELF load failed\n");
    return -ENOEXEC;
  }

  printk(KERN_INFO "sys_execve: loaded at 0x%llx, entry 0x%llx\n",
         (unsigned long long)info.load_base, (unsigned long long)info.entry);

  printk(KERN_ERR "sys_execve: userspace transfer is unsupported\n");
  return -ENOSYS;
}

static long sys_uname(uint64_t buf, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
  };

  struct utsname *uts = (struct utsname *)buf;
  if (!is_valid_user_ptr(buf, sizeof(*uts))) {
    return -EFAULT;
  }

  memset(uts, 0, sizeof(*uts));

  /* Copy strings (simple implementation) */
  const char *sysname = "OS8";
  const char *nodename = "localhost";
  const char *release = "0.1.0";
  const char *version = "0.1.0-arm64";
  const char *machine = "aarch64";
  const char *domain = "";

  strlcpy(uts->sysname, sysname, sizeof(uts->sysname));
  strlcpy(uts->nodename, nodename, sizeof(uts->nodename));
  strlcpy(uts->release, release, sizeof(uts->release));
  strlcpy(uts->version, version, sizeof(uts->version));
  strlcpy(uts->machine, machine, sizeof(uts->machine));
  strlcpy(uts->domainname, domain, sizeof(uts->domainname));

  return 0;
}

static long sys_sched_yield(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  schedule();
  return 0;
}

static long sys_nanosleep(uint64_t req, uint64_t rem, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)rem;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  (void)req;

  return 0;
}

static long sys_not_implemented(uint64_t a0, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return -ENOSYS;
}

/* Sound System Call */
#include "drivers/intel_hda.h"

static long sys_sound_play(uint64_t data, uint64_t samples, uint64_t channels,
                           uint64_t rate, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  uint64_t frame_bytes;
  uint64_t byte_count;

  if (!samples || !channels || !rate)
    return -EINVAL;
  if (samples > UINT32_MAX || channels > UINT8_MAX || rate > UINT32_MAX)
    return -EINVAL;
  if (channels > 16)
    return -EINVAL;
  if (channels > UINT64_MAX / 2)
    return -EINVAL;
  frame_bytes = channels * 2;
  if (samples > UINT64_MAX / frame_bytes)
    return -EINVAL;
  byte_count = samples * frame_bytes;
  if (!is_valid_user_ptr(data, (size_t)byte_count))
    return -EFAULT;

  /* Call HDA driver */
  /* Note: data is a user virtual address. HDA DMA needs physical.
     However, our kernel mapping is currently flat/identity or we mapped
     userspace? The syscall receives arguments. pointer is userspace VA. Current
     'kmalloc' logic in HDA uses a kernel buffer and copies data. So we need to
     access user memory. For now, assuming shared address space or we can read
     it.
  */

  return intel_hda_play_pcm((const void *)data, (uint32_t)samples,
                            (uint8_t)channels, (uint32_t)rate);
}

/* ===================================================================== */
/* Syscall initialization */
/* ===================================================================== */

void syscall_init(void) {
  printk(KERN_INFO "SYSCALL: Initializing system call table\n");

  /* Initialize all to not implemented */
  for (int i = 0; i < NR_syscalls; i++) {
    syscall_table[i] = sys_not_implemented;
  }

  /* Register implemented syscalls */
  syscall_table[SYS_getcwd] = sys_getcwd;
  syscall_table[SYS_dup] = sys_dup;
  syscall_table[SYS_dup3] = sys_dup3;
  syscall_table[SYS_fcntl] = sys_fcntl;
  syscall_table[SYS_ioctl] = sys_ioctl;
  syscall_table[SYS_mkdirat] = sys_mkdirat;
  syscall_table[SYS_unlinkat] = sys_unlinkat;
  syscall_table[SYS_renameat] = sys_renameat;
  syscall_table[SYS_faccessat] = sys_faccessat;
  syscall_table[SYS_fchdir] = sys_fchdir;
  syscall_table[SYS_pipe2] = sys_pipe2;
  syscall_table[SYS_read] = sys_read;
  syscall_table[SYS_write] = sys_write;
  syscall_table[SYS_openat] = sys_openat;
  syscall_table[SYS_close] = sys_close;
  syscall_table[SYS_getdents64] = sys_getdents64;
  syscall_table[SYS_lseek] = sys_lseek;
  syscall_table[SYS_readlinkat] = sys_readlinkat;
  syscall_table[SYS_newfstatat] = sys_newfstatat;
  syscall_table[SYS_fstat] = sys_fstat;
  syscall_table[SYS_exit] = sys_exit;
  syscall_table[SYS_exit_group] = sys_exit_group;
  syscall_table[SYS_wait4] = sys_wait4;
  syscall_table[SYS_getpid] = sys_getpid;
  syscall_table[SYS_getppid] = sys_getppid;
  syscall_table[SYS_getuid] = sys_getuid;
  syscall_table[SYS_geteuid] = sys_getuid;
  syscall_table[SYS_getgid] = sys_getgid;
  syscall_table[SYS_getegid] = sys_getgid;
  syscall_table[SYS_gettid] = sys_gettid;
  syscall_table[SYS_chdir] = sys_chdir;
  syscall_table[SYS_brk] = sys_brk;
  syscall_table[SYS_mmap] = sys_mmap;
  syscall_table[SYS_munmap] = sys_munmap;
  syscall_table[SYS_clone] = sys_clone;
  syscall_table[SYS_execve] = sys_execve;
  syscall_table[SYS_uname] = sys_uname;
  syscall_table[SYS_sched_yield] = sys_sched_yield;
  syscall_table[SYS_kill] = sys_kill;
  syscall_table[SYS_tkill] = sys_tkill;
  syscall_table[SYS_tgkill] = sys_tgkill;
  syscall_table[SYS_rt_sigaction] = sys_rt_sigaction;
  syscall_table[SYS_rt_sigprocmask] = sys_rt_sigprocmask;
  syscall_table[SYS_rt_sigpending] = sys_rt_sigpending;
  syscall_table[SYS_nanosleep] = sys_nanosleep;

  printk(KERN_INFO "SYSCALL: System call table initialized\n");
}

/* ===================================================================== */
/* Syscall dispatcher */
/* ===================================================================== */

long handle_syscall(struct pt_regs *regs) {
  /* ARM64 syscall convention:
   * x8 = syscall number
   * x0-x5 = arguments
   * x0 = return value
   */

  uint64_t nr = regs->regs[8];

  if (nr >= NR_syscalls) {
    printk(KERN_WARNING "SYSCALL: Invalid syscall number %llu\n",
           (unsigned long long)nr);
    return -ENOSYS;
  }

  syscall_fn_t fn = syscall_table[nr];

  return fn(regs->regs[0], regs->regs[1], regs->regs[2], regs->regs[3],
            regs->regs[4], regs->regs[5]);
}

/* ===================================================================== */
/* Exception handler */
/* ===================================================================== */

void handle_sync_exception(struct pt_regs *regs) {
  /* Read exception syndrome register - architecture specific */
  uint32_t ec, iss;

#ifdef ARCH_ARM64
  uint64_t esr;
  asm volatile("mrs %0, esr_el1" : "=r"(esr));

  ec = (esr >> 26) & 0x3F; /* Exception class */
  iss = esr & 0x1FFFFFF;   /* Instruction specific syndrome */
#elif defined(ARCH_X86_64) || defined(ARCH_X86)
  /* x86 uses interrupt numbers instead of ESR */
  ec = 0; /* Not used on x86 */
  iss = 0;
#endif

  switch (ec) {
  case 0x15: /* SVC instruction from AArch64 */
    /* System call - handled separately */
    break;

  case 0x20: /* Instruction abort from lower EL */
  case 0x21: /* Instruction abort from same EL */
    printk(KERN_EMERG "Instruction abort at PC=0x%llx\n",
           (unsigned long long)regs->pc);
    panic("Instruction abort");
    break;

  case 0x24: /* Data abort from lower EL */
  case 0x25: /* Data abort from same EL */
  {
#ifdef ARCH_ARM64
    uint64_t far;
    asm volatile("mrs %0, far_el1" : "=r"(far));
    printk(KERN_EMERG "Data abort at PC=0x%llx, FAR=0x%llx\n",
           (unsigned long long)regs->pc,
           (unsigned long long)far);
#elif defined(ARCH_X86_64) || defined(ARCH_X86)
    uint64_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    printk(KERN_EMERG "Page fault at PC=0x%llx, CR2=0x%llx\n",
           (unsigned long long)regs->pc,
           (unsigned long long)cr2);
#endif
    panic("Data abort");
  } break;

  case 0x00: /* Unknown reason */
    printk(KERN_EMERG "Unknown exception at PC=0x%llx\n",
           (unsigned long long)regs->pc);
    panic("Unknown exception");
    break;

  default:
    printk(KERN_EMERG "Unhandled exception class 0x%x, ISS=0x%x\n", ec, iss);
    printk(KERN_EMERG "PC=0x%llx\n", (unsigned long long)regs->pc);
    panic("Unhandled exception");
    break;
  }
}
