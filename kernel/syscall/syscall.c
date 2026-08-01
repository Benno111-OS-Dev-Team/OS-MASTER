/*
 * UnixOS Kernel - System Call Implementation
 */

#include "syscall/syscall.h"
#include "apps/kapi.h"
#include "arch/arch.h"
#include "drivers/uart.h"
#include "fs/vfs.h"
#include "mm/kmalloc.h"
#include "mm/vmm.h"
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
#define AT_SYMLINK_FOLLOW 0x400
#define AT_EACCESS 0x200
#define AT_EMPTY_PATH 0x1000
#define UTIME_NOW 1073741823L
#define UTIME_OMIT 1073741822L
#define MS_RDONLY 1
#define ST_RDONLY 1
#define POLLIN 0x001
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020
#define POLLRDNORM 0x040
#define POLLWRNORM 0x100
#define NFDBITS (sizeof(unsigned long) * 8)
#define FDSET_WORDS ((TASK_MAX_FDS + NFDBITS - 1) / NFDBITS)
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
#define SCHED_OTHER 0
#define SCHED_FIFO 1
#define SCHED_RR 2
#define PR_SET_PDEATHSIG 1
#define PR_GET_PDEATHSIG 2
#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_REALTIME_COARSE 5
#define CLOCK_MONOTONIC_COARSE 6
#define CLOCK_BOOTTIME 7
#define USER_HZ 100
#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD 1
#define RLIMIT_CPU 0
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA 2
#define RLIMIT_STACK 3
#define RLIMIT_CORE 4
#define RLIMIT_RSS 5
#define RLIMIT_NPROC 6
#define RLIMIT_NOFILE 7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS 9
#define RLIMIT_LOCKS 10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE 12
#define RLIMIT_NICE 13
#define RLIMIT_RTPRIO 14
#define RLIMIT_RTTIME 15
#define RLIMIT_NLIMITS 16
#define RLIM_INFINITY UINT64_MAX
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED_NOREPLACE 0x100000
#define USER_HEAP_LIMIT (USER_HEAP_BASE + 0x02000000ULL)
#define USER_MMAP_LIMIT (USER_MMAP_BASE + 0x0100000000ULL)

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

static struct mm_struct *ensure_task_mm(struct task_struct *task) {
  if (!task)
    return NULL;
  if (!task->mm) {
    task->mm = vmm_create_address_space();
    if (!task->mm)
      return NULL;
    task->active_mm = task->mm;
  }
  if (task->mm->start_brk == 0) {
    task->mm->start_brk = USER_HEAP_BASE;
    task->mm->brk = USER_HEAP_BASE;
  }
  if (task->mm->mmap_base == 0) {
    task->mm->mmap_base = USER_MMAP_BASE;
    task->mm->mmap_next = USER_MMAP_BASE;
  }
  return task->mm;
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

struct linux_fsid {
  int __val[2];
};

struct linux_statfs {
  unsigned long f_type;
  unsigned long f_bsize;
  uint64_t f_blocks;
  uint64_t f_bfree;
  uint64_t f_bavail;
  uint64_t f_files;
  uint64_t f_ffree;
  struct linux_fsid f_fsid;
  unsigned long f_namelen;
  unsigned long f_frsize;
  unsigned long f_flags;
  unsigned long f_spare[4];
};

struct linux_rlimit {
  uint64_t rlim_cur;
  uint64_t rlim_max;
};

struct linux_rusage {
  struct timeval ru_utime;
  struct timeval ru_stime;
  long ru_maxrss;
  long ru_ixrss;
  long ru_idrss;
  long ru_isrss;
  long ru_minflt;
  long ru_majflt;
  long ru_nswap;
  long ru_inblock;
  long ru_oublock;
  long ru_msgsnd;
  long ru_msgrcv;
  long ru_nsignals;
  long ru_nvcsw;
  long ru_nivcsw;
  long __reserved[16];
};

struct linux_tms {
  long tms_utime;
  long tms_stime;
  long tms_cutime;
  long tms_cstime;
};

struct linux_sched_param {
  int sched_priority;
  int __reserved1;
  long __reserved2[4];
  int __reserved3;
};

struct linux_pollfd {
  int fd;
  short events;
  short revents;
};

struct linux_sysinfo {
  unsigned long uptime;
  unsigned long loads[3];
  unsigned long totalram;
  unsigned long freeram;
  unsigned long sharedram;
  unsigned long bufferram;
  unsigned long totalswap;
  unsigned long freeswap;
  unsigned short procs;
  unsigned short pad;
  unsigned long totalhigh;
  unsigned long freehigh;
  unsigned int mem_unit;
  char __reserved[256];
};

static struct linux_rlimit resource_limits[RLIMIT_NLIMITS];
static int resource_limits_initialized;

static uint64_t kernel_time_ns(void) {
  uint64_t ticks = arch_timer_get_ticks();
  uint64_t freq = arch_timer_get_frequency();

  if (freq == 0)
    return arch_timer_get_ms() * 1000000ULL;
  return (ticks / freq) * 1000000000ULL +
         ((ticks % freq) * 1000000000ULL) / freq;
}

static long kernel_clock_ticks(void) {
  uint64_t ms = arch_timer_get_ms();
  return (long)((ms * USER_HZ) / 1000);
}

static long task_cpu_ticks(uint64_t runtime) {
  return (long)((runtime * USER_HZ) / 1000000000ULL);
}

static void runtime_to_timeval(uint64_t runtime, struct timeval *tv) {
  if (!tv)
    return;

  tv->tv_sec = (time_t)(runtime / 1000000000ULL);
  tv->tv_usec = (suseconds_t)((runtime % 1000000000ULL) / 1000ULL);
}

static struct timespec current_timespec(void) {
  uint64_t ns = kernel_time_ns();
  struct timespec ts;

  ts.tv_sec = (time_t)(ns / 1000000000ULL);
  ts.tv_nsec = (long)(ns % 1000000000ULL);
  return ts;
}

static void init_resource_limits_once(void) {
  if (resource_limits_initialized)
    return;

  for (int i = 0; i < RLIMIT_NLIMITS; i++) {
    resource_limits[i].rlim_cur = RLIM_INFINITY;
    resource_limits[i].rlim_max = RLIM_INFINITY;
  }

  resource_limits[RLIMIT_DATA].rlim_cur = 64ULL * 1024ULL * 1024ULL;
  resource_limits[RLIMIT_DATA].rlim_max = 64ULL * 1024ULL * 1024ULL;
  resource_limits[RLIMIT_STACK].rlim_cur = USER_STACK_SIZE;
  resource_limits[RLIMIT_STACK].rlim_max = USER_STACK_SIZE;
  resource_limits[RLIMIT_NPROC].rlim_cur = 256;
  resource_limits[RLIMIT_NPROC].rlim_max = 256;
  resource_limits[RLIMIT_NOFILE].rlim_cur = TASK_MAX_FDS;
  resource_limits[RLIMIT_NOFILE].rlim_max = TASK_MAX_FDS;
  resource_limits[RLIMIT_SIGPENDING].rlim_cur = KERNEL_NSIG;
  resource_limits[RLIMIT_SIGPENDING].rlim_max = KERNEL_NSIG;
  resource_limits_initialized = 1;
}

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

static unsigned long fs_magic_from_name(const char *name) {
  if (!name)
    return 0;
  if (strcmp(name, "fat32") == 0)
    return 0x4D44UL;
  if (strcmp(name, "iso9660") == 0)
    return 0x9660UL;
  if (strcmp(name, "ramfs") == 0)
    return 0x858458F6UL;
  if (strcmp(name, "ext4") == 0)
    return 0xEF53UL;
  if (strcmp(name, "apfs") == 0)
    return 0x4253584EUL;
  return 0x0F5008UL;
}

static long copy_file_statfs(const struct file *file, uint64_t statbuf) {
  const struct inode *inode;
  const struct super_block *sb;
  struct linux_statfs *st;
  unsigned long block_size = 512;
  unsigned long flags = 0;
  uint64_t blocks = 0;

  if (!file || !file->f_dentry)
    return -EBADF;
  if (!is_valid_user_ptr(statbuf, sizeof(struct linux_statfs)))
    return -EFAULT;

  inode = file->f_dentry->d_inode;
  sb = inode ? inode->i_sb : NULL;
  if (sb && sb->s_blocksize > 0)
    block_size = sb->s_blocksize;
  else if (inode && inode->i_blksize > 0)
    block_size = (unsigned long)inode->i_blksize;

  if (inode && inode->i_size > 0)
    blocks = ((uint64_t)inode->i_size + block_size - 1) / block_size;
  if (file->f_op && !file->f_op->write)
    flags |= ST_RDONLY;

  st = (struct linux_statfs *)(uintptr_t)statbuf;
  memset(st, 0, sizeof(*st));
  st->f_type = fs_magic_from_name(sb && sb->s_type ? sb->s_type->name : NULL);
  st->f_bsize = block_size;
  st->f_blocks = blocks;
  st->f_bfree = 0;
  st->f_bavail = 0;
  st->f_files = 1;
  st->f_ffree = 0;
  st->f_fsid.__val[0] = sb ? (int)sb->s_dev : 0;
  st->f_fsid.__val[1] = sb ? sb->s_disk_index : -1;
  st->f_namelen = NAME_MAX;
  st->f_frsize = block_size;
  st->f_flags = flags;
  return 0;
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
  if (task && task->euid == 0) {
    if ((mode & X_OK) && !(inode->i_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
      return -EACCES;
    return 0;
  }

  if (task && task->euid == inode->i_uid) {
    allowed = (inode->i_mode & S_IRWXU) >> 6;
  } else if (task && task->egid == inode->i_gid) {
    allowed = (inode->i_mode & S_IRWXG) >> 3;
  } else if (task) {
    int in_group = 0;
    for (int i = 0; i < task->group_count; i++) {
      if (task->groups[i] == inode->i_gid) {
        in_group = 1;
        break;
      }
    }
    allowed = in_group ? ((inode->i_mode & S_IRWXG) >> 3)
                       : (inode->i_mode & S_IRWXO);
  } else {
    allowed = inode->i_mode & S_IRWXO;
  }

  if ((mode & R_OK) && !(allowed & 4))
    return -EACCES;
  if ((mode & W_OK) && !(allowed & 2))
    return -EACCES;
  if ((mode & X_OK) && !(allowed & 1))
    return -EACCES;
  return 0;
}

static long chmod_inode(struct inode *inode, mode_t mode) {
  struct task_struct *task = get_current();

  if (!inode)
    return -ENOENT;
  if (!task)
    return -ESRCH;
  if (task->euid != 0 && task->euid != inode->i_uid)
    return -EPERM;

  inode->i_mode = (inode->i_mode & S_IFMT) | (mode & 07777);
  inode->i_ctime = current_timespec();
  return 0;
}

static long chown_inode(struct inode *inode, uint64_t owner, uint64_t group) {
  struct task_struct *task = get_current();

  if (!inode)
    return -ENOENT;
  if (!task)
    return -ESRCH;
  if (task->euid != 0)
    return -EPERM;

  if ((int64_t)owner != -1) {
    if (owner > UINT32_MAX)
      return -EINVAL;
    inode->i_uid = (uid_t)owner;
  }
  if ((int64_t)group != -1) {
    if (group > UINT32_MAX)
      return -EINVAL;
    inode->i_gid = (gid_t)group;
  }
  inode->i_ctime = current_timespec();
  return 0;
}

static long utimens_inode(struct inode *inode, const struct timespec *times) {
  struct task_struct *task = get_current();
  struct timespec now;

  if (!inode)
    return -ENOENT;
  if (!task)
    return -ESRCH;
  if (task->euid != 0 && task->euid != inode->i_uid)
    return -EPERM;

  now = current_timespec();
  if (!times) {
    inode->i_atime = now;
    inode->i_mtime = now;
  } else {
    if ((times[0].tv_nsec < 0 || times[0].tv_nsec >= 1000000000L) &&
        times[0].tv_nsec != UTIME_NOW && times[0].tv_nsec != UTIME_OMIT)
      return -EINVAL;
    if ((times[1].tv_nsec < 0 || times[1].tv_nsec >= 1000000000L) &&
        times[1].tv_nsec != UTIME_NOW && times[1].tv_nsec != UTIME_OMIT)
      return -EINVAL;

    if (times[0].tv_nsec == UTIME_NOW)
      inode->i_atime = now;
    else if (times[0].tv_nsec != UTIME_OMIT)
      inode->i_atime = times[0];

    if (times[1].tv_nsec == UTIME_NOW)
      inode->i_mtime = now;
    else if (times[1].tv_nsec != UTIME_OMIT)
      inode->i_mtime = times[1];
  }
  inode->i_ctime = now;
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

static int fd_readiness(struct task_struct *task, int fd, short events) {
  short ready = 0;

  if (!task || fd < 0 || fd >= TASK_MAX_FDS || !task->files[fd].in_use)
    return POLLNVAL;

  if (!task->files[fd].file) {
    if ((events & (POLLIN | POLLRDNORM)) && fd == 0)
      ready |= events & (POLLIN | POLLRDNORM);
    if ((events & (POLLOUT | POLLWRNORM)) && (fd == 1 || fd == 2))
      ready |= events & (POLLOUT | POLLWRNORM);
    return ready;
  }

  return vfs_poll_file(task->files[fd].file, events);
}

static int fdset_isset(const unsigned long *set, int fd) {
  return (set[fd / (int)NFDBITS] & (1UL << (fd % (int)NFDBITS))) != 0;
}

static void fdset_set(unsigned long *set, int fd) {
  set[fd / (int)NFDBITS] |= 1UL << (fd % (int)NFDBITS);
}

static size_t fdset_bytes(int nfds) {
  return ((size_t)nfds + NFDBITS - 1) / NFDBITS * sizeof(unsigned long);
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

static long sys_fsync(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;

  struct file *f = get_file(task, (int)fd);
  if (!f)
    return -EBADF;
  return vfs_sync_file(f);
}

static long sys_fdatasync(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  return sys_fsync(fd, a1, a2, a3, a4, a5);
}

static long sys_sync(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  vfs_sync_all();
  return 0;
}

static long sys_syncfs(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  return sys_fsync(fd, a1, a2, a3, a4, a5);
}

static long sys_ppoll(uint64_t fds, uint64_t nfds, uint64_t timeout_ts,
                      uint64_t sigmask, uint64_t sigsetsize, uint64_t a5) {
  (void)sigmask;
  (void)sigsetsize;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  uint64_t deadline = 0;
  int infinite = 1;

  if (!task)
    return -ESRCH;
  if (nfds > TASK_MAX_FDS)
    return -EINVAL;
  if (nfds &&
      !is_valid_user_ptr(fds, sizeof(struct linux_pollfd) * (size_t)nfds))
    return -EFAULT;
  if (timeout_ts) {
    if (!is_valid_user_ptr(timeout_ts, sizeof(struct timespec)))
      return -EFAULT;
    const struct timespec *ts = (const struct timespec *)(uintptr_t)timeout_ts;
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L)
      return -EINVAL;
    deadline = arch_timer_get_ms() + (uint64_t)ts->tv_sec * 1000ULL +
               ((uint64_t)ts->tv_nsec + 999999ULL) / 1000000ULL;
    infinite = 0;
  }

  struct linux_pollfd *pollfds = (struct linux_pollfd *)(uintptr_t)fds;
  for (;;) {
    long ready_count = 0;
    for (size_t i = 0; i < (size_t)nfds; i++) {
      short revents = 0;
      if (pollfds[i].fd >= 0)
        revents = (short)fd_readiness(task, pollfds[i].fd, pollfds[i].events);
      pollfds[i].revents = revents;
      if (revents)
        ready_count++;
    }
    if (ready_count || (!infinite && arch_timer_get_ms() >= deadline))
      return ready_count;

    extern void process_yield(void);
    process_yield();
  }
}

static long sys_pselect6(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                         uint64_t exceptfds, uint64_t timeout_ts,
                         uint64_t sigmask_data) {
  (void)sigmask_data;

  struct task_struct *task = current_task_with_files();
  size_t bytes;
  uint64_t deadline = 0;
  int infinite = 1;

  if (!task)
    return -ESRCH;
  if ((int64_t)nfds < 0 || nfds > TASK_MAX_FDS)
    return -EINVAL;
  bytes = fdset_bytes((int)nfds);
  if (readfds && !is_valid_user_ptr(readfds, bytes))
    return -EFAULT;
  if (writefds && !is_valid_user_ptr(writefds, bytes))
    return -EFAULT;
  if (exceptfds && !is_valid_user_ptr(exceptfds, bytes))
    return -EFAULT;
  if (timeout_ts) {
    if (!is_valid_user_ptr(timeout_ts, sizeof(struct timespec)))
      return -EFAULT;
    const struct timespec *ts = (const struct timespec *)(uintptr_t)timeout_ts;
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L)
      return -EINVAL;
    deadline = arch_timer_get_ms() + (uint64_t)ts->tv_sec * 1000ULL +
               ((uint64_t)ts->tv_nsec + 999999ULL) / 1000000ULL;
    infinite = 0;
  }

  unsigned long *rset = (unsigned long *)(uintptr_t)readfds;
  unsigned long *wset = (unsigned long *)(uintptr_t)writefds;
  unsigned long *eset = (unsigned long *)(uintptr_t)exceptfds;
  unsigned long ready_r[FDSET_WORDS];
  unsigned long ready_w[FDSET_WORDS];
  unsigned long ready_e[FDSET_WORDS];

  for (;;) {
    long ready_count = 0;
    memset(ready_r, 0, sizeof(ready_r));
    memset(ready_w, 0, sizeof(ready_w));
    memset(ready_e, 0, sizeof(ready_e));

    for (int fd = 0; fd < (int)nfds; fd++) {
      short events = 0;
      if (rset && fdset_isset(rset, fd))
        events |= POLLIN | POLLRDNORM;
      if (wset && fdset_isset(wset, fd))
        events |= POLLOUT | POLLWRNORM;
      if (eset && fdset_isset(eset, fd))
        events |= POLLERR;
      if (!events)
        continue;

      int ready = fd_readiness(task, fd, events);
      if (ready & POLLNVAL)
        return -EBADF;
      int fd_is_ready = 0;
      if (rset && (ready & (POLLIN | POLLRDNORM | POLLHUP))) {
        fdset_set(ready_r, fd);
        fd_is_ready = 1;
      }
      if (wset && (ready & (POLLOUT | POLLWRNORM))) {
        fdset_set(ready_w, fd);
        fd_is_ready = 1;
      }
      if (eset && (ready & POLLERR)) {
        fdset_set(ready_e, fd);
        fd_is_ready = 1;
      }
      if (fd_is_ready) {
        ready_count++;
      }
    }

    if (ready_count || (!infinite && arch_timer_get_ms() >= deadline)) {
      if (rset)
        memcpy(rset, ready_r, bytes);
      if (wset)
        memcpy(wset, ready_w, bytes);
      if (eset)
        memcpy(eset, ready_e, bytes);
      return ready_count;
    }

    extern void process_yield(void);
    process_yield();
  }
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
  mode_t create_mode = (mode_t)mode;
  if (flags & O_CREAT)
    create_mode &= ~(task->umask);
  struct file *f = vfs_open(path, (int)flags, create_mode);
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

static long sys_statfs(uint64_t pathname, uint64_t statbuf, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  char user_path[TASK_CWD_MAX];
  char path[TASK_CWD_MAX];
  struct task_struct *task = get_current();
  long ret = copy_user_string(pathname, user_path, sizeof(user_path));
  if (ret < 0)
    return ret;
  ret = resolve_task_path(task, user_path, path, sizeof(path));
  if (ret < 0)
    return ret;

  struct file *f = vfs_open(path, O_RDONLY, 0);
  if (!f)
    return -ENOENT;

  ret = copy_file_statfs(f, statbuf);
  vfs_close(f);
  return ret;
}

static long sys_fstatfs(uint64_t fd, uint64_t statbuf, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;

  struct file *f = get_file(task, (int)fd);
  if (!f)
    return -EBADF;
  return copy_file_statfs(f, statbuf);
}

static long sys_ftruncate(uint64_t fd, uint64_t length, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;
  if ((int64_t)length < 0)
    return -EINVAL;

  struct file *f = get_file(task, (int)fd);
  if (!f)
    return -EBADF;

  long ret = vfs_truncate_file(f, (loff_t)length);
  if (ret == 0 && f->f_dentry && f->f_dentry->d_inode) {
    struct timespec now = current_timespec();
    f->f_dentry->d_inode->i_mtime = now;
    f->f_dentry->d_inode->i_ctime = now;
  }
  return ret;
}

static long sys_truncate(uint64_t pathname, uint64_t length, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  char user_path[PATH_MAX];
  char path[PATH_MAX];
  long ret;

  if (!task)
    return -ESRCH;
  if ((int64_t)length < 0)
    return -EINVAL;

  ret = copy_user_string(pathname, user_path, sizeof(user_path));
  if (ret < 0)
    return ret;
  ret = resolve_task_path(task, user_path, path, sizeof(path));
  if (ret < 0)
    return ret;

  struct file *f = vfs_open(path, O_WRONLY, 0);
  if (!f)
    return -ENOENT;
  ret = vfs_truncate_file(f, (loff_t)length);
  if (ret == 0 && f->f_dentry && f->f_dentry->d_inode) {
    struct timespec now = current_timespec();
    f->f_dentry->d_inode->i_mtime = now;
    f->f_dentry->d_inode->i_ctime = now;
  }
  vfs_close(f);
  return ret;
}

static long sys_fchmod(uint64_t fd, uint64_t mode, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;

  struct file *f = get_file(task, (int)fd);
  if (!f || !f->f_dentry)
    return -EBADF;
  return chmod_inode(f->f_dentry->d_inode, (mode_t)mode);
}

static long sys_fchmodat(uint64_t dirfd, uint64_t pathname, uint64_t mode,
                         uint64_t flags, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  char user_path[PATH_MAX];
  char path[PATH_MAX];
  long ret;

  if (!task)
    return -ESRCH;
  if (flags & ~(uint64_t)AT_SYMLINK_NOFOLLOW)
    return -EINVAL;
  ret = copy_user_string(pathname, user_path, sizeof(user_path));
  if (ret < 0)
    return ret;
  ret = resolve_at_path(task, (int)dirfd, user_path, path, sizeof(path));
  if (ret < 0)
    return ret;

  struct file *f = vfs_open(path, O_RDONLY, 0);
  if (!f)
    return -ENOENT;
  ret = f->f_dentry ? chmod_inode(f->f_dentry->d_inode, (mode_t)mode) : -ENOENT;
  vfs_close(f);
  return ret;
}

static long sys_fchown(uint64_t fd, uint64_t owner, uint64_t group, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;

  struct file *f = get_file(task, (int)fd);
  if (!f || !f->f_dentry)
    return -EBADF;
  return chown_inode(f->f_dentry->d_inode, owner, group);
}

static long sys_fchownat(uint64_t dirfd, uint64_t pathname, uint64_t owner,
                         uint64_t group, uint64_t flags, uint64_t a5) {
  (void)a5;

  struct task_struct *task = current_task_with_files();
  char user_path[PATH_MAX];
  char path[PATH_MAX];
  long ret;

  if (!task)
    return -ESRCH;
  if (flags & ~(uint64_t)AT_SYMLINK_NOFOLLOW)
    return -EINVAL;
  ret = copy_user_string(pathname, user_path, sizeof(user_path));
  if (ret < 0)
    return ret;
  ret = resolve_at_path(task, (int)dirfd, user_path, path, sizeof(path));
  if (ret < 0)
    return ret;

  struct file *f = vfs_open(path, O_RDONLY, 0);
  if (!f)
    return -ENOENT;
  ret = f->f_dentry ? chown_inode(f->f_dentry->d_inode, owner, group) : -ENOENT;
  vfs_close(f);
  return ret;
}

static long sys_utimensat(uint64_t dirfd, uint64_t pathname, uint64_t times,
                          uint64_t flags, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  char user_path[PATH_MAX];
  char path[PATH_MAX];
  struct timespec kernel_times[2];
  const struct timespec *times_ptr = NULL;
  long ret;

  if (!task)
    return -ESRCH;
  if (flags & ~(uint64_t)AT_SYMLINK_NOFOLLOW)
    return -EINVAL;
  if (times) {
    if (!is_valid_user_ptr(times, sizeof(kernel_times)))
      return -EFAULT;
    kernel_times[0] = ((const struct timespec *)(uintptr_t)times)[0];
    kernel_times[1] = ((const struct timespec *)(uintptr_t)times)[1];
    times_ptr = kernel_times;
  }

  ret = copy_user_string(pathname, user_path, sizeof(user_path));
  if (ret < 0)
    return ret;
  ret = resolve_at_path(task, (int)dirfd, user_path, path, sizeof(path));
  if (ret < 0)
    return ret;

  struct file *f = vfs_open(path, O_RDONLY, 0);
  if (!f)
    return -ENOENT;
  ret = f->f_dentry ? utimens_inode(f->f_dentry->d_inode, times_ptr) : -ENOENT;
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

static long sys_geteuid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  return current ? current->euid : 0;
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

static long sys_getegid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  return current ? current->egid : 0;
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

static long sys_setuid(uint64_t uid, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  if (!current || uid > UINT32_MAX)
    return -EINVAL;
  current->uid = (uid_t)uid;
  current->euid = (uid_t)uid;
  current->suid = (uid_t)uid;
  return 0;
}

static long sys_setgid(uint64_t gid, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  if (!current || gid > UINT32_MAX)
    return -EINVAL;
  current->gid = (gid_t)gid;
  current->egid = (gid_t)gid;
  current->sgid = (gid_t)gid;
  return 0;
}

static long sys_setreuid(uint64_t ruid, uint64_t euid, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  if (!current)
    return -ESRCH;
  if ((int64_t)ruid != -1) {
    if (ruid > UINT32_MAX)
      return -EINVAL;
    current->uid = (uid_t)ruid;
  }
  if ((int64_t)euid != -1) {
    if (euid > UINT32_MAX)
      return -EINVAL;
    current->euid = (uid_t)euid;
  }
  current->suid = current->euid;
  return 0;
}

static long sys_setregid(uint64_t rgid, uint64_t egid, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  if (!current)
    return -ESRCH;
  if ((int64_t)rgid != -1) {
    if (rgid > UINT32_MAX)
      return -EINVAL;
    current->gid = (gid_t)rgid;
  }
  if ((int64_t)egid != -1) {
    if (egid > UINT32_MAX)
      return -EINVAL;
    current->egid = (gid_t)egid;
  }
  current->sgid = current->egid;
  return 0;
}

static long sys_setresuid(uint64_t ruid, uint64_t euid, uint64_t suid,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  if (!current)
    return -ESRCH;
  if ((int64_t)ruid != -1) {
    if (ruid > UINT32_MAX)
      return -EINVAL;
    current->uid = (uid_t)ruid;
  }
  if ((int64_t)euid != -1) {
    if (euid > UINT32_MAX)
      return -EINVAL;
    current->euid = (uid_t)euid;
  }
  if ((int64_t)suid != -1) {
    if (suid > UINT32_MAX)
      return -EINVAL;
    current->suid = (uid_t)suid;
  }
  return 0;
}

static long sys_getresuid(uint64_t ruid, uint64_t euid, uint64_t suid,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  if (!current)
    return -ESRCH;
  if (!is_valid_user_ptr(ruid, sizeof(uid_t)) ||
      !is_valid_user_ptr(euid, sizeof(uid_t)) ||
      !is_valid_user_ptr(suid, sizeof(uid_t)))
    return -EFAULT;
  *(uid_t *)(uintptr_t)ruid = current->uid;
  *(uid_t *)(uintptr_t)euid = current->euid;
  *(uid_t *)(uintptr_t)suid = current->suid;
  return 0;
}

static long sys_setresgid(uint64_t rgid, uint64_t egid, uint64_t sgid,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  if (!current)
    return -ESRCH;
  if ((int64_t)rgid != -1) {
    if (rgid > UINT32_MAX)
      return -EINVAL;
    current->gid = (gid_t)rgid;
  }
  if ((int64_t)egid != -1) {
    if (egid > UINT32_MAX)
      return -EINVAL;
    current->egid = (gid_t)egid;
  }
  if ((int64_t)sgid != -1) {
    if (sgid > UINT32_MAX)
      return -EINVAL;
    current->sgid = (gid_t)sgid;
  }
  return 0;
}

static long sys_getresgid(uint64_t rgid, uint64_t egid, uint64_t sgid,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  if (!current)
    return -ESRCH;
  if (!is_valid_user_ptr(rgid, sizeof(gid_t)) ||
      !is_valid_user_ptr(egid, sizeof(gid_t)) ||
      !is_valid_user_ptr(sgid, sizeof(gid_t)))
    return -EFAULT;
  *(gid_t *)(uintptr_t)rgid = current->gid;
  *(gid_t *)(uintptr_t)egid = current->egid;
  *(gid_t *)(uintptr_t)sgid = current->sgid;
  return 0;
}

static long sys_setpgid(uint64_t pid, uint64_t pgid, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  if (!current)
    return -ESRCH;
  if ((int64_t)pid < 0 || (int64_t)pgid < 0)
    return -EINVAL;

  struct task_struct *task = pid ? get_task_by_pid((pid_t)pid) : current;
  if (!task)
    return -ESRCH;

  pid_t new_pgrp = pgid ? (pid_t)pgid : task->pid;
  task->pgrp = new_pgrp;
  return 0;
}

static long sys_getpgid(uint64_t pid, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  struct task_struct *task = pid ? get_task_by_pid((pid_t)pid) : current;
  return task ? task->pgrp : -ESRCH;
}

static long sys_setsid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  if (!current)
    return -ESRCH;
  current->sid = current->pid;
  current->pgrp = current->pid;
  return current->sid;
}

static long sys_getsid(uint64_t pid, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  struct task_struct *task = pid ? get_task_by_pid((pid_t)pid) : current;
  return task ? task->sid : -ESRCH;
}

static long sys_getgroups(uint64_t size, uint64_t list, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  if (!current)
    return -ESRCH;
  if (size == 0)
    return current->group_count;
  if (size > TASK_MAX_GROUPS || (int)size < current->group_count)
    return -EINVAL;
  if (!is_valid_user_ptr(list, sizeof(gid_t) * (size_t)size))
    return -EFAULT;

  gid_t *groups = (gid_t *)(uintptr_t)list;
  for (int i = 0; i < current->group_count; i++)
    groups[i] = current->groups[i];
  return current->group_count;
}

static long sys_setgroups(uint64_t size, uint64_t list, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  if (!current)
    return -ESRCH;
  if (size > TASK_MAX_GROUPS)
    return -EINVAL;
  if (size && !is_valid_user_ptr(list, sizeof(gid_t) * (size_t)size))
    return -EFAULT;

  const gid_t *groups = (const gid_t *)(uintptr_t)list;
  for (int i = 0; i < (int)size; i++)
    current->groups[i] = groups[i];
  for (int i = (int)size; i < TASK_MAX_GROUPS; i++)
    current->groups[i] = 0;
  current->group_count = (int)size;
  return 0;
}

static long sys_umask(uint64_t mask, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *current = get_current();
  if (!current)
    return -ESRCH;
  mode_t old = current->umask;
  current->umask = (mode_t)(mask & 0777);
  return old;
}

static long sys_prctl(uint64_t option, uint64_t arg2, uint64_t arg3,
                      uint64_t arg4, uint64_t arg5, uint64_t a5) {
  (void)arg4;
  (void)arg5;
  (void)a5;

  struct task_struct *current = get_current();
  if (!current)
    return -ESRCH;

  switch (option) {
  case PR_SET_NAME:
    if (!is_valid_user_ptr(arg2, 1))
      return -EFAULT;
    return copy_user_string(arg2, current->comm, sizeof(current->comm)) == 0
               ? 0
               : -EFAULT;
  case PR_GET_NAME:
    if (!is_valid_user_ptr(arg2, TASK_COMM_LEN))
      return -EFAULT;
    strlcpy((char *)(uintptr_t)arg2, current->comm, TASK_COMM_LEN);
    return 0;
  case PR_SET_NO_NEW_PRIVS:
    if (arg2 != 1 || arg3 || arg4 || arg5)
      return -EINVAL;
    current->no_new_privs = 1;
    return 0;
  case PR_GET_NO_NEW_PRIVS:
    return current->no_new_privs ? 1 : 0;
  case PR_SET_PDEATHSIG:
    if (arg2 >= KERNEL_NSIG)
      return -EINVAL;
    current->pdeath_signal = (int)arg2;
    return 0;
  case PR_GET_PDEATHSIG:
    if (!is_valid_user_ptr(arg2, sizeof(int)))
      return -EFAULT;
    *(int *)(uintptr_t)arg2 = current->pdeath_signal;
    return 0;
  default:
    return -ENOSYS;
  }
}

static long sys_getcpu(uint64_t cpu, uint64_t node, uint64_t cache,
                       uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)cache;
  (void)a3;
  (void)a4;
  (void)a5;

  if (cpu) {
    if (!is_valid_user_ptr(cpu, sizeof(uint32_t)))
      return -EFAULT;
    *(uint32_t *)(uintptr_t)cpu = arch_cpu_id();
  }
  if (node) {
    if (!is_valid_user_ptr(node, sizeof(uint32_t)))
      return -EFAULT;
    *(uint32_t *)(uintptr_t)node = 0;
  }
  return 0;
}

static long sys_sched_getscheduler(uint64_t pid, uint64_t a1, uint64_t a2,
                                   uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (pid && !get_task_by_pid((pid_t)pid))
    return -ESRCH;
  return SCHED_OTHER;
}

static long sys_sched_getparam(uint64_t pid, uint64_t param, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (pid && !get_task_by_pid((pid_t)pid))
    return -ESRCH;
  if (!is_valid_user_ptr(param, sizeof(struct linux_sched_param)))
    return -EFAULT;
  struct linux_sched_param *sp = (struct linux_sched_param *)(uintptr_t)param;
  memset(sp, 0, sizeof(*sp));
  return 0;
}

static long sys_sched_setparam(uint64_t pid, uint64_t param, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (pid && !get_task_by_pid((pid_t)pid))
    return -ESRCH;
  if (!is_valid_user_ptr(param, sizeof(struct linux_sched_param)))
    return -EFAULT;
  const struct linux_sched_param *sp =
      (const struct linux_sched_param *)(uintptr_t)param;
  return sp->sched_priority == 0 ? 0 : -EINVAL;
}

static long sys_sched_setscheduler(uint64_t pid, uint64_t policy,
                                   uint64_t param, uint64_t a3, uint64_t a4,
                                   uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  if (policy != SCHED_OTHER)
    return -EINVAL;
  return sys_sched_setparam(pid, param, 0, 0, 0, 0);
}

static long sys_sched_get_priority_max(uint64_t policy, uint64_t a1,
                                       uint64_t a2, uint64_t a3, uint64_t a4,
                                       uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (policy != SCHED_OTHER && policy != SCHED_FIFO && policy != SCHED_RR)
    return -EINVAL;
  return policy == SCHED_OTHER ? 0 : PRIO_MAX;
}

static long sys_sched_get_priority_min(uint64_t policy, uint64_t a1,
                                       uint64_t a2, uint64_t a3, uint64_t a4,
                                       uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (policy != SCHED_OTHER && policy != SCHED_FIFO && policy != SCHED_RR)
    return -EINVAL;
  return policy == SCHED_OTHER ? 0 : PRIO_MIN;
}

static long sys_sched_rr_get_interval(uint64_t pid, uint64_t interval,
                                      uint64_t a2, uint64_t a3, uint64_t a4,
                                      uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (pid && !get_task_by_pid((pid_t)pid))
    return -ESRCH;
  if (!is_valid_user_ptr(interval, sizeof(struct timespec)))
    return -EFAULT;
  struct timespec *ts = (struct timespec *)(uintptr_t)interval;
  ts->tv_sec = 0;
  ts->tv_nsec = 20000000L;
  return 0;
}

static long sys_sched_getaffinity(uint64_t pid, uint64_t cpusetsize,
                                  uint64_t mask, uint64_t a3, uint64_t a4,
                                  uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  if (pid && !get_task_by_pid((pid_t)pid))
    return -ESRCH;
  if (cpusetsize < sizeof(unsigned long))
    return -EINVAL;
  if (!is_valid_user_ptr(mask, (size_t)cpusetsize))
    return -EFAULT;

  memset((void *)(uintptr_t)mask, 0, (size_t)cpusetsize);
  *(unsigned long *)(uintptr_t)mask = 1UL;
  return sizeof(unsigned long);
}

static long sys_sched_setaffinity(uint64_t pid, uint64_t cpusetsize,
                                  uint64_t mask, uint64_t a3, uint64_t a4,
                                  uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  if (pid && !get_task_by_pid((pid_t)pid))
    return -ESRCH;
  if (cpusetsize < sizeof(unsigned long))
    return -EINVAL;
  if (!is_valid_user_ptr(mask, (size_t)cpusetsize))
    return -EFAULT;

  const unsigned long *bits = (const unsigned long *)(uintptr_t)mask;
  return (bits[0] & 1UL) ? 0 : -EINVAL;
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

static long sys_mount(uint64_t source, uint64_t target, uint64_t filesystemtype,
                      uint64_t mountflags, uint64_t data, uint64_t a5) {
  (void)a5;

  struct task_struct *task = get_current();
  char source_buf[TASK_CWD_MAX];
  char target_buf[TASK_CWD_MAX];
  char fstype_buf[32];
  char target_path[TASK_CWD_MAX];
  long ret;

  ret = copy_user_string(source, source_buf, sizeof(source_buf));
  if (ret < 0)
    return ret;
  ret = copy_user_string(target, target_buf, sizeof(target_buf));
  if (ret < 0)
    return ret;
  ret = copy_user_string(filesystemtype, fstype_buf, sizeof(fstype_buf));
  if (ret < 0)
    return ret;
  ret = resolve_task_path(task, target_buf, target_path, sizeof(target_path));
  if (ret < 0)
    return ret;
  if (mountflags & ~((uint64_t)MS_RDONLY))
    return -EINVAL;

  return vfs_mount(source_buf, target_path, fstype_buf,
                   (unsigned long)mountflags, (const void *)(uintptr_t)data);
}

static long sys_umount2(uint64_t target, uint64_t flags, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = get_current();
  char target_buf[TASK_CWD_MAX];
  char target_path[TASK_CWD_MAX];
  long ret;

  if (flags)
    return -EINVAL;

  ret = copy_user_string(target, target_buf, sizeof(target_buf));
  if (ret < 0)
    return ret;
  ret = resolve_task_path(task, target_buf, target_path, sizeof(target_path));
  if (ret < 0)
    return ret;
  return vfs_umount(target_path);
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
  return vfs_mkdir(path, (mode_t)mode & ~(task->umask));
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

static long sys_symlinkat(uint64_t target, uint64_t newdirfd,
                          uint64_t linkpath, uint64_t a3, uint64_t a4,
                          uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  char target_buf[PATH_MAX];
  char link_user_path[PATH_MAX];
  char link_resolved[PATH_MAX];
  long ret;

  if (!task)
    return -ESRCH;
  ret = copy_user_string(target, target_buf, sizeof(target_buf));
  if (ret < 0)
    return ret;
  ret = copy_user_string(linkpath, link_user_path, sizeof(link_user_path));
  if (ret < 0)
    return ret;
  ret = resolve_at_path(task, (int)newdirfd, link_user_path, link_resolved,
                        sizeof(link_resolved));
  if (ret < 0)
    return ret;

  return vfs_symlink(target_buf, link_resolved);
}

static long sys_linkat(uint64_t olddirfd, uint64_t oldpath_ptr,
                       uint64_t newdirfd, uint64_t newpath_ptr,
                       uint64_t flags, uint64_t a5) {
  (void)a5;

  struct task_struct *task = current_task_with_files();
  char old_user_path[PATH_MAX];
  char new_user_path[PATH_MAX];
  char old_path[PATH_MAX];
  char new_path[PATH_MAX];
  long ret;

  if (!task)
    return -ESRCH;
  if (flags & ~(uint64_t)AT_SYMLINK_FOLLOW)
    return -EINVAL;
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

  return vfs_link(old_path, new_path);
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

static uint32_t mmap_prot_to_vm_flags(uint64_t prot) {
  uint32_t vm_flags = VM_USER;

  if (prot & PROT_READ)
    vm_flags |= VM_READ;
  if (prot & PROT_WRITE)
    vm_flags |= VM_WRITE;
  if (prot & PROT_EXEC)
    vm_flags |= VM_EXEC;
  return vm_flags;
}

static long sys_brk(uint64_t brk, uint64_t a1, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = get_current();
  struct mm_struct *mm = ensure_task_mm(task);
  if (!mm)
    return -ENOMEM;

  if (brk == 0)
    return mm->brk;
  if (brk < mm->start_brk)
    return mm->brk;

  if (brk > USER_HEAP_LIMIT)
    return mm->brk;

  if (brk > mm->brk) {
    uint64_t map_start = (mm->brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t map_end = (brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (map_end > map_start &&
        vmm_map_user_range(mm, map_start, (size_t)(map_end - map_start),
                           VM_USER | VM_READ | VM_WRITE) != 0) {
      return mm->brk;
    }
  }

  mm->brk = brk;
  return mm->brk;
}

static long sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags,
                     uint64_t fd, uint64_t offset) {
  (void)offset;

  if (len == 0 || len > UINT64_MAX - (PAGE_SIZE - 1))
    return -EINVAL;
  if (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC))
    return -EINVAL;
  if ((flags & (MAP_PRIVATE | MAP_SHARED)) == 0 ||
      (flags & (MAP_PRIVATE | MAP_SHARED)) == (MAP_PRIVATE | MAP_SHARED))
    return -EINVAL;
  if (!(flags & MAP_ANONYMOUS) || (int64_t)fd != -1) {
    printk(KERN_DEBUG "sys_mmap: file-backed mappings unsupported\n");
    return -ENOSYS;
  }

  struct task_struct *task = get_current();
  struct mm_struct *mm = ensure_task_mm(task);
  if (!mm)
    return -ENOMEM;

  len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  if (len > USER_MMAP_LIMIT - mm->mmap_base)
    return -ENOMEM;

  uint64_t result;
  if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
    if ((addr & (PAGE_SIZE - 1)) != 0 || addr < USER_MMAP_BASE ||
        len > USER_MMAP_LIMIT - addr)
      return -EINVAL;
    result = addr;
  } else {
    result = (mm->mmap_next + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (len > USER_MMAP_LIMIT - result)
      return -ENOMEM;
  }

  uint32_t vm_flags = mmap_prot_to_vm_flags(prot);
  if (flags & MAP_SHARED)
    vm_flags |= VM_SHARED;
  if (vmm_map_user_range(mm, result, (size_t)len, vm_flags) != 0)
    return (flags & MAP_FIXED_NOREPLACE) ? -EEXIST : -ENOMEM;
  if (!(flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)))
    mm->mmap_next = result + len;

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

  struct task_struct *task = get_current();
  struct mm_struct *mm = ensure_task_mm(task);
  if (!mm)
    return -ENOMEM;
  if ((addr & (PAGE_SIZE - 1)) != 0 || len == 0)
    return -EINVAL;
  if (len > UINT64_MAX - (PAGE_SIZE - 1))
    return -EINVAL;
  len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  return vmm_unmap_user_range(mm, addr, (size_t)len) == 0 ? 0 : -EINVAL;
}

static long sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  if ((addr & (PAGE_SIZE - 1)) != 0 || len == 0)
    return -EINVAL;
  if (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC))
    return -EINVAL;
  if (len > UINT64_MAX - (PAGE_SIZE - 1))
    return -EINVAL;
  len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  struct task_struct *task = get_current();
  struct mm_struct *mm = ensure_task_mm(task);
  if (!mm)
    return -ENOMEM;
  return vmm_protect_user_range(mm, addr, (size_t)len,
                                mmap_prot_to_vm_flags(prot)) == 0
             ? 0
             : -ENOMEM;
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

static long sys_clock_gettime(uint64_t clockid, uint64_t tp, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (!is_valid_user_ptr(tp, sizeof(struct timespec)))
    return -EFAULT;

  if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC &&
      clockid != CLOCK_PROCESS_CPUTIME_ID &&
      clockid != CLOCK_THREAD_CPUTIME_ID &&
      clockid != CLOCK_MONOTONIC_RAW && clockid != CLOCK_REALTIME_COARSE &&
      clockid != CLOCK_MONOTONIC_COARSE && clockid != CLOCK_BOOTTIME)
    return -EINVAL;

  uint64_t ns = kernel_time_ns();
  struct timespec *ts = (struct timespec *)(uintptr_t)tp;
  ts->tv_sec = (time_t)(ns / 1000000000ULL);
  ts->tv_nsec = (long)(ns % 1000000000ULL);
  return 0;
}

static long sys_clock_getres(uint64_t clockid, uint64_t res, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC &&
      clockid != CLOCK_PROCESS_CPUTIME_ID &&
      clockid != CLOCK_THREAD_CPUTIME_ID &&
      clockid != CLOCK_MONOTONIC_RAW && clockid != CLOCK_REALTIME_COARSE &&
      clockid != CLOCK_MONOTONIC_COARSE && clockid != CLOCK_BOOTTIME)
    return -EINVAL;
  if (!res)
    return 0;
  if (!is_valid_user_ptr(res, sizeof(struct timespec)))
    return -EFAULT;

  struct timespec *ts = (struct timespec *)(uintptr_t)res;
  ts->tv_sec = 0;
  ts->tv_nsec = 10000000L;
  return 0;
}

static long sys_gettimeofday(uint64_t tvp, uint64_t tzp, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (tvp) {
    if (!is_valid_user_ptr(tvp, sizeof(struct timeval)))
      return -EFAULT;
    uint64_t ns = kernel_time_ns();
    struct timeval *tv = (struct timeval *)(uintptr_t)tvp;
    tv->tv_sec = (time_t)(ns / 1000000000ULL);
    tv->tv_usec = (suseconds_t)((ns % 1000000000ULL) / 1000ULL);
  }
  if (tzp) {
    if (!is_valid_user_ptr(tzp, sizeof(int) * 2))
      return -EFAULT;
    memset((void *)(uintptr_t)tzp, 0, sizeof(int) * 2);
  }

  return 0;
}

static long sys_times(uint64_t buf, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (buf) {
    if (!is_valid_user_ptr(buf, sizeof(struct linux_tms)))
      return -EFAULT;
    struct task_struct *task = get_current();
    struct linux_tms *tms = (struct linux_tms *)(uintptr_t)buf;
    memset(tms, 0, sizeof(*tms));
    if (task) {
      tms->tms_utime = task_cpu_ticks(task->utime);
      tms->tms_stime = task_cpu_ticks(task->stime);
    }
  }

  return kernel_clock_ticks();
}

static long sys_getrlimit(uint64_t resource, uint64_t rlim, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (resource >= RLIMIT_NLIMITS)
    return -EINVAL;
  if (!is_valid_user_ptr(rlim, sizeof(struct linux_rlimit)))
    return -EFAULT;

  init_resource_limits_once();
  *(struct linux_rlimit *)(uintptr_t)rlim = resource_limits[resource];
  return 0;
}

static long sys_setrlimit(uint64_t resource, uint64_t rlim, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (resource >= RLIMIT_NLIMITS)
    return -EINVAL;
  if (!is_valid_user_ptr(rlim, sizeof(struct linux_rlimit)))
    return -EFAULT;

  struct linux_rlimit new_limit = *(struct linux_rlimit *)(uintptr_t)rlim;
  if (new_limit.rlim_cur > new_limit.rlim_max)
    return -EINVAL;
  if (resource == RLIMIT_NOFILE && new_limit.rlim_max > TASK_MAX_FDS)
    return -EINVAL;

  init_resource_limits_once();
  resource_limits[resource] = new_limit;
  return 0;
}

static long sys_getrusage(uint64_t who, uint64_t usage, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if ((int64_t)who != RUSAGE_SELF && (int64_t)who != RUSAGE_CHILDREN &&
      (int64_t)who != RUSAGE_THREAD)
    return -EINVAL;
  if (!is_valid_user_ptr(usage, sizeof(struct linux_rusage)))
    return -EFAULT;

  struct linux_rusage *ru = (struct linux_rusage *)(uintptr_t)usage;
  memset(ru, 0, sizeof(*ru));
  if ((int64_t)who == RUSAGE_SELF || (int64_t)who == RUSAGE_THREAD) {
    struct task_struct *task = get_current();
    if (task) {
      runtime_to_timeval(task->utime, &ru->ru_utime);
      runtime_to_timeval(task->stime, &ru->ru_stime);
    }
  }

  return 0;
}

static long sys_sysinfo(uint64_t info, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (!is_valid_user_ptr(info, sizeof(struct linux_sysinfo)))
    return -EFAULT;

  size_t heap_total = 0;
  size_t heap_used = 0;
  size_t heap_free = 0;
  kmalloc_get_stats(&heap_total, &heap_used, &heap_free);

  struct linux_sysinfo *si = (struct linux_sysinfo *)(uintptr_t)info;
  memset(si, 0, sizeof(*si));
  si->uptime = (unsigned long)(arch_timer_get_ms() / 1000);
  si->totalram = (unsigned long)heap_total;
  si->freeram = (unsigned long)heap_free;
  si->bufferram = (unsigned long)heap_used;
  si->procs = (unsigned short)CLAMP(sched_count_live_tasks(), 0, UINT16_MAX);
  si->mem_unit = 1;
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
  syscall_table[SYS_umount2] = sys_umount2;
  syscall_table[SYS_mount] = sys_mount;
  syscall_table[SYS_statfs] = sys_statfs;
  syscall_table[SYS_fstatfs] = sys_fstatfs;
  syscall_table[SYS_truncate] = sys_truncate;
  syscall_table[SYS_ftruncate] = sys_ftruncate;
  syscall_table[SYS_fchmod] = sys_fchmod;
  syscall_table[SYS_fchmodat] = sys_fchmodat;
  syscall_table[SYS_fchownat] = sys_fchownat;
  syscall_table[SYS_fchown] = sys_fchown;
  syscall_table[SYS_mkdirat] = sys_mkdirat;
  syscall_table[SYS_unlinkat] = sys_unlinkat;
  syscall_table[SYS_symlinkat] = sys_symlinkat;
  syscall_table[SYS_linkat] = sys_linkat;
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
  syscall_table[SYS_pselect6] = sys_pselect6;
  syscall_table[SYS_ppoll] = sys_ppoll;
  syscall_table[SYS_readlinkat] = sys_readlinkat;
  syscall_table[SYS_newfstatat] = sys_newfstatat;
  syscall_table[SYS_fstat] = sys_fstat;
  syscall_table[SYS_utimensat] = sys_utimensat;
  syscall_table[SYS_sync] = sys_sync;
  syscall_table[SYS_fsync] = sys_fsync;
  syscall_table[SYS_fdatasync] = sys_fdatasync;
  syscall_table[SYS_exit] = sys_exit;
  syscall_table[SYS_exit_group] = sys_exit_group;
  syscall_table[SYS_wait4] = sys_wait4;
  syscall_table[SYS_getpid] = sys_getpid;
  syscall_table[SYS_getppid] = sys_getppid;
  syscall_table[SYS_getuid] = sys_getuid;
  syscall_table[SYS_geteuid] = sys_geteuid;
  syscall_table[SYS_getgid] = sys_getgid;
  syscall_table[SYS_getegid] = sys_getegid;
  syscall_table[SYS_gettid] = sys_gettid;
  syscall_table[SYS_setregid] = sys_setregid;
  syscall_table[SYS_setgid] = sys_setgid;
  syscall_table[SYS_setreuid] = sys_setreuid;
  syscall_table[SYS_setuid] = sys_setuid;
  syscall_table[SYS_setresuid] = sys_setresuid;
  syscall_table[SYS_getresuid] = sys_getresuid;
  syscall_table[SYS_setresgid] = sys_setresgid;
  syscall_table[SYS_getresgid] = sys_getresgid;
  syscall_table[SYS_setpgid] = sys_setpgid;
  syscall_table[SYS_getpgid] = sys_getpgid;
  syscall_table[SYS_getsid] = sys_getsid;
  syscall_table[SYS_setsid] = sys_setsid;
  syscall_table[SYS_getgroups] = sys_getgroups;
  syscall_table[SYS_setgroups] = sys_setgroups;
  syscall_table[SYS_chdir] = sys_chdir;
  syscall_table[SYS_brk] = sys_brk;
  syscall_table[SYS_mmap] = sys_mmap;
  syscall_table[SYS_munmap] = sys_munmap;
  syscall_table[SYS_mprotect] = sys_mprotect;
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
  syscall_table[SYS_clock_gettime] = sys_clock_gettime;
  syscall_table[SYS_clock_getres] = sys_clock_getres;
  syscall_table[SYS_times] = sys_times;
  syscall_table[SYS_getrlimit] = sys_getrlimit;
  syscall_table[SYS_setrlimit] = sys_setrlimit;
  syscall_table[SYS_getrusage] = sys_getrusage;
  syscall_table[SYS_umask] = sys_umask;
  syscall_table[SYS_prctl] = sys_prctl;
  syscall_table[SYS_getcpu] = sys_getcpu;
  syscall_table[SYS_gettimeofday] = sys_gettimeofday;
  syscall_table[SYS_sysinfo] = sys_sysinfo;
  syscall_table[SYS_sched_setparam] = sys_sched_setparam;
  syscall_table[SYS_sched_setscheduler] = sys_sched_setscheduler;
  syscall_table[SYS_sched_getscheduler] = sys_sched_getscheduler;
  syscall_table[SYS_sched_getparam] = sys_sched_getparam;
  syscall_table[SYS_sched_setaffinity] = sys_sched_setaffinity;
  syscall_table[SYS_sched_getaffinity] = sys_sched_getaffinity;
  syscall_table[SYS_sched_get_priority_max] = sys_sched_get_priority_max;
  syscall_table[SYS_sched_get_priority_min] = sys_sched_get_priority_min;
  syscall_table[SYS_sched_rr_get_interval] = sys_sched_rr_get_interval;
  syscall_table[SYS_syncfs] = sys_syncfs;

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
