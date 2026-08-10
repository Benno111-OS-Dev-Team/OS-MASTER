/*
 * UnixOS Kernel - Fork and Exec Implementation
 *
 * Implements process creation (fork) and program loading (exec).
 */

#include "fs/vfs.h"
#include "arch/arch.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "printk.h"
#include "sched/sched.h"
#include "string.h"

/* Forward declaration */
static void fork_entry(void *arg);

/* Simple ELF header definitions */
#define ELF_MAGIC 0x464C457F /* "\x7FELF" */
#define ET_EXEC 2
#define EM_X86_64 62
#define EM_386 3
#define EM_AARCH64 183
#define PT_LOAD 1
#define EXEC_STACK_MAX_ARGS 64

struct elf64_hdr {
  uint32_t e_ident_magic;
  uint8_t e_ident_class;
  uint8_t e_ident_data;
  uint8_t e_ident_version;
  uint8_t e_ident_osabi;
  uint8_t e_ident_pad[8];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
};

struct elf64_phdr {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
};

static int u64_add_overflow(uint64_t a, uint64_t b, uint64_t *out) {
  if (a > UINT64_MAX - b)
    return 1;
  *out = a + b;
  return 0;
}

static int u64_mul_overflow(uint64_t a, uint64_t b, uint64_t *out) {
  if (a != 0 && b > UINT64_MAX / a)
    return 1;
  *out = a * b;
  return 0;
}

static int file_range_invalid(uint64_t offset, uint64_t size,
                              uint64_t file_size) {
  uint64_t end = 0;
  return u64_add_overflow(offset, size, &end) || end > file_size;
}

static uint64_t min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }

static int elf_machine_supported(uint16_t machine) {
#if defined(ARCH_X86_64)
  return machine == EM_X86_64;
#elif defined(ARCH_X86)
  return machine == EM_386;
#elif defined(ARCH_ARM64)
  return machine == EM_AARCH64;
#else
  (void)machine;
  return 0;
#endif
}

static int stack_write(virt_addr_t stack_bottom, const phys_addr_t *pages,
                       size_t page_count, virt_addr_t dst, const void *src,
                       size_t len) {
  const uint8_t *in = (const uint8_t *)src;

  if (!pages || (!src && len > 0) || dst < stack_bottom)
    return -1;
  if (dst - stack_bottom > page_count * PAGE_SIZE ||
      len > page_count * PAGE_SIZE - (size_t)(dst - stack_bottom))
    return -1;

  while (len > 0) {
    size_t page_index = (size_t)((dst - stack_bottom) / PAGE_SIZE);
    size_t page_offset = (size_t)((dst - stack_bottom) % PAGE_SIZE);
    size_t chunk = PAGE_SIZE - page_offset;

    if (page_index >= page_count)
      return -1;
    if (chunk > len)
      chunk = len;

    memcpy((void *)(uintptr_t)(pages[page_index] + page_offset), in, chunk);
    dst += chunk;
    in += chunk;
    len -= chunk;
  }

  return 0;
}

/* ===================================================================== */
/* Fork implementation */
/* ===================================================================== */

static int copy_mm(struct task_struct *child, struct task_struct *parent) {
  if (!parent->mm) {
    child->mm = NULL;
    child->active_mm = parent->active_mm;
    return 0;
  }

  child->mm = vmm_create_address_space();
  if (!child->mm) {
    return -1;
  }

  child->active_mm = child->mm;
  return 0;
}

static void copy_thread(struct task_struct *child, struct task_struct *parent) {
  child->cpu_context = parent->cpu_context;
}

static void fork_entry(void *arg) { (void)arg; }

long do_fork(unsigned long flags) {
  struct task_struct *current_task = get_current();
  struct task_struct *child;

  child = create_task(fork_entry, NULL, (uint32_t)flags);
  if (!child) {
    return -1;
  }

  if (copy_mm(child, current_task) < 0) {
    return -1;
  }

  copy_thread(child, current_task);
  child->parent = current_task;
  child->uid = current_task->uid;
  child->euid = current_task->euid;
  child->suid = current_task->suid;
  child->gid = current_task->gid;
<<<<<<< HEAD
  child->umask = current_task->umask;
=======
  child->egid = current_task->egid;
  child->sgid = current_task->sgid;
  child->group_count = current_task->group_count;
  for (int group = 0; group < TASK_MAX_GROUPS; group++) {
    child->groups[group] = current_task->groups[group];
  }
  child->pgrp = current_task->pgrp;
  child->sid = current_task->sid;
  child->umask = current_task->umask;
  if (current_task->root_initialized) {
    strlcpy(child->root, current_task->root, sizeof(child->root));
    child->root_initialized = 1;
  } else {
    strlcpy(child->root, "/", sizeof(child->root));
    child->root_initialized = 1;
  }
  child->pdeath_signal = current_task->pdeath_signal;
  child->no_new_privs = current_task->no_new_privs;
  child->seccomp_mode = current_task->seccomp_mode;
  child->membarrier_registered = current_task->membarrier_registered;
  child->personality = current_task->personality;
  for (int cap = 0; cap < 2; cap++) {
    child->cap_effective[cap] = current_task->cap_effective[cap];
    child->cap_permitted[cap] = current_task->cap_permitted[cap];
    child->cap_inheritable[cap] = current_task->cap_inheritable[cap];
  }
  child->cap_initialized = current_task->cap_initialized;
>>>>>>> 8c9572f4cc7ca61e4a09950ae47b17008999ca1e
  child->state = TASK_RUNNING;

  return child->pid;
}

/* ===================================================================== */
/* Exec implementation */
/* ===================================================================== */

static int load_elf_binary(struct mm_struct *mm, const char *path,
                           uint64_t *entry_point) {
  struct file *file;
  struct elf64_hdr ehdr;
  uint64_t file_size = 0;
  uint64_t ph_table_size = 0;
  uint64_t min_vaddr = UINT64_MAX;
  uint64_t max_vaddr = 0;
  ssize_t bytes_read;

  if (!mm || !path || !entry_point) {
    return -1;
  }

  file = vfs_open(path, O_RDONLY, 0);
  if (!file) {
    printk(KERN_ERR "exec: cannot open '%s'\n", path);
    return -1;
  }

  bytes_read = vfs_read(file, (char *)&ehdr, sizeof(ehdr));
  if (bytes_read < (ssize_t)sizeof(ehdr)) {
    vfs_close(file);
    return -1;
  }

  if (file->f_dentry && file->f_dentry->d_inode) {
    file_size = (uint64_t)file->f_dentry->d_inode->i_size;
  }

  if (file_size < sizeof(ehdr)) {
    vfs_close(file);
    return -1;
  }

  if (ehdr.e_ident_magic != ELF_MAGIC ||
      !elf_machine_supported(ehdr.e_machine) || ehdr.e_type != ET_EXEC ||
      ehdr.e_phentsize != sizeof(struct elf64_phdr) || ehdr.e_phnum == 0 ||
      u64_mul_overflow((uint64_t)ehdr.e_phnum, sizeof(struct elf64_phdr),
                       &ph_table_size) ||
      file_range_invalid(ehdr.e_phoff, ph_table_size, file_size)) {
    vfs_close(file);
    return -1;
  }

  for (int i = 0; i < ehdr.e_phnum; i++) {
    struct elf64_phdr phdr;
    uint64_t phoff = 0;

    if (u64_mul_overflow((uint64_t)i, ehdr.e_phentsize, &phoff) ||
        u64_add_overflow(ehdr.e_phoff, phoff, &phoff)) {
      vfs_close(file);
      return -1;
    }
    file->f_pos = (loff_t)phoff;
    bytes_read = vfs_read(file, (char *)&phdr, sizeof(phdr));
    if (bytes_read < (ssize_t)sizeof(phdr) || phdr.p_type != PT_LOAD) {
      continue;
    }

    if (phdr.p_filesz > phdr.p_memsz ||
        file_range_invalid(phdr.p_offset, phdr.p_filesz, file_size)) {
      vfs_close(file);
      return -1;
    }

    virt_addr_t vaddr = phdr.p_vaddr & ~(PAGE_SIZE - 1);
    uint64_t page_delta = phdr.p_vaddr - vaddr;
    uint64_t segment_size = 0;
    if (u64_add_overflow(phdr.p_memsz, page_delta, &segment_size) ||
        segment_size > (uint64_t)((size_t)-1) - (PAGE_SIZE - 1)) {
      vfs_close(file);
      return -1;
    }
    size_t size = PAGE_ALIGN((size_t)segment_size);
    uint32_t vm_flags = VM_READ | VM_USER;
    if (phdr.p_flags & 0x1)
      vm_flags |= VM_EXEC;
    if (phdr.p_flags & 0x2)
      vm_flags |= VM_WRITE;

    if (vmm_add_vma(mm, vaddr, vaddr + size, vm_flags) != 0) {
      vfs_close(file);
      return -1;
    }

    for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
      virt_addr_t page_vaddr = vaddr + offset;
      phys_addr_t paddr = pmm_alloc_page();
      if (!paddr) {
        vfs_close(file);
        return -1;
      }

      memset((void *)(uintptr_t)paddr, 0, PAGE_SIZE);

      if (vmm_map_user_page(mm, page_vaddr, paddr, vm_flags) != 0) {
        pmm_free_page(paddr);
        vfs_close(file);
        return -1;
      }

      uint64_t page_start = page_vaddr;
      uint64_t page_end = page_start + PAGE_SIZE;
      uint64_t file_start = phdr.p_vaddr;
      uint64_t file_end = phdr.p_vaddr + phdr.p_filesz;

      if (page_end > file_start && page_start < file_end) {
        uint64_t copy_start = page_start > file_start ? page_start : file_start;
        uint64_t copy_end = min_u64(page_end, file_end);
        uint64_t dst_off = copy_start - page_start;
        uint64_t src_off = phdr.p_offset + (copy_start - phdr.p_vaddr);
        uint64_t copy_len = copy_end - copy_start;

        file->f_pos = (loff_t)src_off;
        bytes_read = vfs_read(file, (char *)(uintptr_t)(paddr + dst_off),
                              (size_t)copy_len);
        if (bytes_read != (ssize_t)copy_len) {
          vfs_close(file);
          return -1;
        }
      }
    }

    if (vaddr < min_vaddr)
      min_vaddr = vaddr;
    if (vaddr + size > max_vaddr)
      max_vaddr = vaddr + size;
  }

  vfs_close(file);
  *entry_point = ehdr.e_entry;
  if (min_vaddr != UINT64_MAX) {
    mm->start_code = min_vaddr;
    mm->end_code = max_vaddr;
    mm->start_data = min_vaddr;
    mm->end_data = max_vaddr;
    mm->start_brk = max_vaddr;
    mm->brk = max_vaddr;
  }
  return 0;
}

static int setup_user_stack(struct mm_struct *mm, char *const argv[],
                            char *const envp[], uint64_t *stack_pointer,
                            uint64_t *argc_out) {
  virt_addr_t stack_top = 0x7FFFFFFFE000UL;
  size_t stack_size = 1024 * 1024;
  virt_addr_t stack_bottom = stack_top - stack_size;
  size_t page_count = stack_size / PAGE_SIZE;
  phys_addr_t stack_pages[256];
  uint64_t argv_user[EXEC_STACK_MAX_ARGS];
  uint64_t envp_user[EXEC_STACK_MAX_ARGS];
  uint64_t zero = 0;
  int argc = 0;
  int envc = 0;
  virt_addr_t sp = stack_top;
  uint64_t argv_addr = 0;
  uint64_t envp_addr = 0;

  if (!mm || !stack_pointer)
    return -1;
  if (page_count > sizeof(stack_pages) / sizeof(stack_pages[0]))
    return -1;
  if (vmm_add_vma(mm, stack_bottom, stack_top, VM_READ | VM_WRITE | VM_USER) != 0)
    return -1;

  if (argv) {
    while (argv[argc]) {
      if (argc >= EXEC_STACK_MAX_ARGS)
        return -1;
      argc++;
    }
  }
  if (envp) {
    while (envp[envc]) {
      if (envc >= EXEC_STACK_MAX_ARGS)
        return -1;
      envc++;
    }
  }

  for (size_t offset = 0; offset < stack_size; offset += PAGE_SIZE) {
    virt_addr_t page_vaddr = stack_bottom + offset;
    phys_addr_t paddr = pmm_alloc_page();
    if (!paddr)
      return -1;
    memset((void *)(uintptr_t)paddr, 0, PAGE_SIZE);
    if (vmm_map_user_page(mm, page_vaddr, paddr, VM_READ | VM_WRITE | VM_USER) != 0) {
      pmm_free_page(paddr);
      return -1;
    }
    stack_pages[offset / PAGE_SIZE] = paddr;
  }

  for (int i = envc - 1; i >= 0; i--) {
    size_t len = strlen(envp[i]) + 1;
    if (len > (size_t)(sp - stack_bottom))
      return -1;
    sp -= len;
    if (stack_write(stack_bottom, stack_pages, page_count, sp, envp[i], len) != 0)
      return -1;
    envp_user[i] = sp;
  }

  for (int i = argc - 1; i >= 0; i--) {
    size_t len = strlen(argv[i]) + 1;
    if (len > (size_t)(sp - stack_bottom))
      return -1;
    sp -= len;
    if (stack_write(stack_bottom, stack_pages, page_count, sp, argv[i], len) != 0)
      return -1;
    argv_user[i] = sp;
  }

  sp &= ~0xFULL;

  sp -= sizeof(uint64_t);
  if (stack_write(stack_bottom, stack_pages, page_count, sp, &zero,
                  sizeof(zero)) != 0)
    return -1;
  for (int i = envc - 1; i >= 0; i--) {
    sp -= sizeof(uint64_t);
    if (stack_write(stack_bottom, stack_pages, page_count, sp, &envp_user[i],
                    sizeof(envp_user[i])) != 0)
      return -1;
  }
  envp_addr = sp;

  sp -= sizeof(uint64_t);
  if (stack_write(stack_bottom, stack_pages, page_count, sp, &zero,
                  sizeof(zero)) != 0)
    return -1;
  for (int i = argc - 1; i >= 0; i--) {
    sp -= sizeof(uint64_t);
    if (stack_write(stack_bottom, stack_pages, page_count, sp, &argv_user[i],
                    sizeof(argv_user[i])) != 0)
      return -1;
  }
  argv_addr = sp;

  sp -= sizeof(uint64_t);
  uint64_t argc_word = (uint64_t)argc;
  if (stack_write(stack_bottom, stack_pages, page_count, sp, &argc_word,
                  sizeof(argc_word)) != 0)
    return -1;

  *stack_pointer = sp;
  if (argc_out)
    *argc_out = (uint64_t)argc;
  mm->start_stack = stack_top;
  mm->arg_start = argv_addr;
  mm->arg_end = envp_addr ? envp_addr : argv_addr;
  mm->env_start = envp_addr;
  mm->env_end = stack_top;
  return 0;
}

long do_execve(const char *filename, char *const argv[], char *const envp[]) {
  struct task_struct *current_task = get_current();
  struct mm_struct *old_mm;
  struct mm_struct *new_mm;
  uint64_t entry_point;
  uint64_t user_sp;
  uint64_t user_argc = 0;

  if (!current_task || !filename || filename[0] == '\0') {
    return -1;
  }

  printk(KERN_INFO "execve: loading '%s'\n", filename);

  new_mm = vmm_create_address_space();
  if (!new_mm) {
    return -1;
  }

  if (load_elf_binary(new_mm, filename, &entry_point) < 0) {
    vmm_destroy_address_space(new_mm);
    return -1;
  }

  if (setup_user_stack(new_mm, argv, envp, &user_sp, &user_argc) < 0) {
    vmm_destroy_address_space(new_mm);
    return -1;
  }

  old_mm = current_task->mm;
  current_task->mm = new_mm;
  current_task->active_mm = new_mm;
  vmm_switch_address_space(new_mm);

  const char *name = filename;
  while (*filename) {
    if (*filename == '/')
      name = filename + 1;
    filename++;
  }
  strlcpy(current_task->comm, name, sizeof(current_task->comm));

  current_task->cpu_context.pc = entry_point;
  current_task->cpu_context.sp = user_sp;

  if (old_mm && old_mm != new_mm) {
    if (old_mm->users.counter <= 1) {
      vmm_destroy_address_space(old_mm);
    } else {
      old_mm->users.counter--;
    }
  }

  printk(KERN_INFO "execve: ready\n");
#if defined(ARCH_X86_64)
  arch_enter_userspace(entry_point, user_sp, user_argc, current_task->mm->arg_start);
  __builtin_unreachable();
#endif
  return 0;
}
