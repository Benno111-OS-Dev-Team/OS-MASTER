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
#include "string.h"

/* ===================================================================== */
/* File Descriptor Table */
/* ===================================================================== */

static int init_task_files(struct task_struct *task) {
  if (!task)
    return -ESRCH;
  if (task->files_initialized)
    return 0;

  for (int i = 0; i < TASK_MAX_FDS; i++) {
    task->files[i].file = NULL;
    task->files[i].flags = 0;
    task->files[i].in_use = 0;
  }

  task->files[0].in_use = 1; /* stdin */
  task->files[1].in_use = 1; /* stdout */
  task->files[2].in_use = 1; /* stderr */
  task->files_initialized = 1;
  return 0;
}

static struct task_struct *current_task_with_files(void) {
  struct task_struct *task = get_current();
  if (init_task_files(task) != 0)
    return NULL;
  return task;
}

static int alloc_fd(struct task_struct *task) {
  if (init_task_files(task) != 0)
    return -ESRCH;
  for (int i = 3; i < TASK_MAX_FDS; i++) {
    if (!task->files[i].in_use) {
      task->files[i].in_use = 1;
      return i;
    }
  }
  return -EMFILE;
}

static void free_fd(struct task_struct *task, int fd) {
  if (task && fd >= 0 && fd < TASK_MAX_FDS) {
    task->files[fd].file = NULL;
    task->files[fd].flags = 0;
    task->files[fd].in_use = 0;
  }
}

static struct file *get_file(struct task_struct *task, int fd) {
  if (!task || fd < 0 || fd >= TASK_MAX_FDS || !task->files[fd].in_use) {
    return NULL;
  }
  return task->files[fd].file;
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

  /* Validate user buffer */
  if (!is_valid_user_ptr(buf, count)) {
    return -EFAULT;
  }

  /* Handle stdin specially */
  if (fd == 0) {
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

  if (count > 0 && !is_valid_user_ptr(buf, count)) {
    return -EFAULT;
  }

  /* Special case: stdout/stderr (fd 1 and 2) go to console */
  if (fd == 1 || fd == 2) {
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
  (void)dirfd; /* dirfd ignored - always use absolute paths */

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;
  char path[256];
  long ret = copy_user_string(pathname, path, sizeof(path));
  if (ret < 0) {
    return ret;
  }

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

  /* Don't close stdin/stdout/stderr */
  if (fd < 3) {
    return 0;
  }

  struct file *f = get_file(task, (int)fd);
  if (!f) {
    return -EBADF;
  }

  vfs_close(f);
  free_fd(task, (int)fd);

  return 0;
}

static long sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                      uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct task_struct *task = current_task_with_files();
  if (!task)
    return -ESRCH;

  struct file *f = get_file(task, (int)fd);
  if (!f) {
    return -EBADF;
  }

  return vfs_lseek(f, (loff_t)offset, (int)whence);
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

static long sys_clone(uint64_t flags, uint64_t stack, uint64_t ptid,
                      uint64_t tls, uint64_t ctid, uint64_t a5) {
  (void)flags;
  (void)stack;
  (void)ptid;
  (void)tls;
  (void)ctid;
  (void)a5;

  return -ENOSYS;
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

  char path[256];
  long path_ret = copy_user_string(filename, path, sizeof(path));
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
  syscall_table[SYS_read] = sys_read;
  syscall_table[SYS_write] = sys_write;
  syscall_table[SYS_openat] = sys_openat;
  syscall_table[SYS_close] = sys_close;
  syscall_table[SYS_lseek] = sys_lseek;
  syscall_table[SYS_exit] = sys_exit;
  syscall_table[SYS_exit_group] = sys_exit_group;
  syscall_table[SYS_getpid] = sys_getpid;
  syscall_table[SYS_getppid] = sys_getppid;
  syscall_table[SYS_getuid] = sys_getuid;
  syscall_table[SYS_geteuid] = sys_getuid;
  syscall_table[SYS_getgid] = sys_getgid;
  syscall_table[SYS_getegid] = sys_getgid;
  syscall_table[SYS_gettid] = sys_gettid;
  syscall_table[SYS_brk] = sys_brk;
  syscall_table[SYS_mmap] = sys_mmap;
  syscall_table[SYS_munmap] = sys_munmap;
  syscall_table[SYS_clone] = sys_clone;
  syscall_table[SYS_execve] = sys_execve;
  syscall_table[SYS_uname] = sys_uname;
  syscall_table[SYS_sched_yield] = sys_sched_yield;
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
