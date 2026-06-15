/*
 * UnixOS Kernel - Apple NVMe Driver (ANS)
 *
 * Based on Asahi Linux apple_nvme driver.
 * Supports Apple's proprietary NVMe implementation on M-series chips.
 */

#include "arch/arch.h"
#include "drivers/storage.h"
#include "mm/vmm.h"
#include "printk.h"
#include "types.h"

/* ===================================================================== */
/* ANS (Apple NVMe Storage) Definitions */
/* ===================================================================== */

#define ANS_BASE 0x27BCC0000UL /* M2 NVMe base */
#define ANS_SIZE 0x40000UL     /* 256KB */

#define ANS_BOOT_STATUS 0x1300

#define NVME_CAP 0x0000
#define NVME_CC 0x0014
#define NVME_CSTS 0x001C
#define NVME_AQA 0x0024
#define NVME_ASQ 0x0028
#define NVME_ACQ 0x0030

#define ANS_ADMIN_Q_DEPTH 16
#define ANS_IO_Q_DEPTH 16
#define ANS_PAGE_SIZE 4096
#define ANS_IO_TIMEOUT_MS 1000

typedef struct {
  uint32_t cdw0;
  uint32_t nsid;
  uint32_t cdw2;
  uint32_t cdw3;
  uint64_t mptr;
  uint64_t prp1;
  uint64_t prp2;
  uint32_t cdw10;
  uint32_t cdw11;
  uint32_t cdw12;
  uint32_t cdw13;
  uint32_t cdw14;
  uint32_t cdw15;
} __attribute__((packed)) nvme_sqe_t;

typedef struct {
  uint32_t result;
  uint32_t rsvd;
  uint16_t sq_head;
  uint16_t sq_id;
  uint16_t cid;
  uint16_t status;
} __attribute__((packed)) nvme_cqe_t;

typedef struct {
  volatile uint32_t *regs;
  uint32_t nsid;
  uint32_t sector_size;
  uint64_t sector_count;
  uint16_t admin_sq_tail;
  uint16_t admin_cq_head;
  uint8_t admin_phase;
  uint16_t io_sq_tail;
  uint16_t io_cq_head;
  uint8_t io_phase;
  int active;
  uint8_t admin_sq[ANS_PAGE_SIZE] __attribute__((aligned(ANS_PAGE_SIZE)));
  uint8_t admin_cq[ANS_PAGE_SIZE] __attribute__((aligned(ANS_PAGE_SIZE)));
  uint8_t io_sq[ANS_PAGE_SIZE] __attribute__((aligned(ANS_PAGE_SIZE)));
  uint8_t io_cq[ANS_PAGE_SIZE] __attribute__((aligned(ANS_PAGE_SIZE)));
  uint8_t identify_ns[ANS_PAGE_SIZE] __attribute__((aligned(ANS_PAGE_SIZE)));
  uint8_t io_buffer[ANS_PAGE_SIZE] __attribute__((aligned(ANS_PAGE_SIZE)));
} ans_nvme_ctx_t;

/* ===================================================================== */
/* Driver State */
/* ===================================================================== */

static ans_nvme_ctx_t ans_ctx;
static bool ans_initialized = false;

/* Forward declarations */
int ans_read_blocks(uint64_t lba, uint32_t count, void *buffer, void *ctx);
int ans_write_blocks(uint64_t lba, uint32_t count, const void *buffer, void *ctx);

/* ===================================================================== */
/* MMIO Helpers */
/* ===================================================================== */

static inline uint32_t ans_read32(uint32_t offset) {
  if (!ans_ctx.regs)
    return 0;
  return ans_ctx.regs[offset / 4];
}

static inline void ans_write32(uint32_t offset, uint32_t val) {
  if (!ans_ctx.regs)
    return;
  ans_ctx.regs[offset / 4] = val;
}

static volatile uint32_t *ans_nvme_db_reg(uint16_t qid, int is_cq) {
  uintptr_t base;
  uintptr_t stride;

  if (!ans_ctx.regs)
    return NULL;
  base = (uintptr_t)ans_ctx.regs + 0x1000;
  stride = 4U << ((ans_ctx.regs[0] >> 20) & 0xF);
  return (volatile uint32_t *)(base + ((qid * 2U + (is_cq ? 1U : 0U)) * stride));
}

static uint64_t ans_deadline_ms(void) { return arch_timer_get_ms() + ANS_IO_TIMEOUT_MS; }

static int ans_wait_ready(int ready) {
  uint64_t deadline = ans_deadline_ms();

  if (!ans_ctx.regs)
    return -1;
  while (arch_timer_get_ms() <= deadline) {
    if ((((ans_ctx.regs[NVME_CSTS / 4] & 1U) != 0) ? 1 : 0) == (ready ? 1 : 0))
      return 0;
  }
  return -1;
}

static void ans_clear_queue(uint8_t *queue, size_t size) {
  for (size_t i = 0; i < size; i++)
    queue[i] = 0;
}

static uint64_t ans_read_le64(const uint8_t *src) {
  return (uint64_t)src[0] | ((uint64_t)src[1] << 8) |
         ((uint64_t)src[2] << 16) | ((uint64_t)src[3] << 24) |
         ((uint64_t)src[4] << 32) | ((uint64_t)src[5] << 40) |
         ((uint64_t)src[6] << 48) | ((uint64_t)src[7] << 56);
}

static int ans_submit_admin(nvme_sqe_t *cmd) {
  nvme_sqe_t *sq;
  nvme_cqe_t *cq;
  uint16_t cid;
  uint64_t deadline;

  if (!ans_ctx.regs || !cmd)
    return -1;

  sq = (nvme_sqe_t *)ans_ctx.admin_sq;
  cq = (nvme_cqe_t *)ans_ctx.admin_cq;
  cid = ans_ctx.admin_sq_tail;
  cmd->cdw0 = (cmd->cdw0 & 0x0000FFFFU) | ((uint32_t)cid << 16);
  sq[ans_ctx.admin_sq_tail] = *cmd;
  ans_ctx.admin_sq_tail = (uint16_t)((ans_ctx.admin_sq_tail + 1) % ANS_ADMIN_Q_DEPTH);
  *ans_nvme_db_reg(0, 0) = ans_ctx.admin_sq_tail;

  deadline = ans_deadline_ms();
  while (arch_timer_get_ms() <= deadline) {
    nvme_cqe_t *entry = &cq[ans_ctx.admin_cq_head];
    if (((entry->status >> 15) & 1U) == ans_ctx.admin_phase) {
      if (entry->cid != cid || ((entry->status >> 1) & 0x7FF) != 0)
        return -1;
      ans_ctx.admin_cq_head =
          (uint16_t)((ans_ctx.admin_cq_head + 1) % ANS_ADMIN_Q_DEPTH);
      if (ans_ctx.admin_cq_head == 0)
        ans_ctx.admin_phase ^= 1U;
      *ans_nvme_db_reg(0, 1) = ans_ctx.admin_cq_head;
      return 0;
    }
  }
  return -1;
}

static int ans_submit_io(nvme_sqe_t *cmd) {
  nvme_sqe_t *sq;
  nvme_cqe_t *cq;
  uint16_t cid;
  uint64_t deadline;

  if (!ans_ctx.regs || !cmd)
    return -1;

  sq = (nvme_sqe_t *)ans_ctx.io_sq;
  cq = (nvme_cqe_t *)ans_ctx.io_cq;
  cid = ans_ctx.io_sq_tail;
  cmd->cdw0 = (cmd->cdw0 & 0x0000FFFFU) | ((uint32_t)cid << 16);
  sq[ans_ctx.io_sq_tail] = *cmd;
  ans_ctx.io_sq_tail = (uint16_t)((ans_ctx.io_sq_tail + 1) % ANS_IO_Q_DEPTH);
  *ans_nvme_db_reg(1, 0) = ans_ctx.io_sq_tail;

  deadline = ans_deadline_ms();
  while (arch_timer_get_ms() <= deadline) {
    nvme_cqe_t *entry = &cq[ans_ctx.io_cq_head];
    if (((entry->status >> 15) & 1U) == ans_ctx.io_phase) {
      if (entry->cid != cid || ((entry->status >> 1) & 0x7FF) != 0)
        return -1;
      ans_ctx.io_cq_head = (uint16_t)((ans_ctx.io_cq_head + 1) % ANS_IO_Q_DEPTH);
      if (ans_ctx.io_cq_head == 0)
        ans_ctx.io_phase ^= 1U;
      *ans_nvme_db_reg(1, 1) = ans_ctx.io_cq_head;
      return 0;
    }
  }
  return -1;
}

static int ans_nvme_rw_one(uint64_t lba, void *buffer, int write) {
  nvme_sqe_t cmd;
  uint8_t *dst = (uint8_t *)buffer;
  const uint8_t *src = (const uint8_t *)buffer;

  if (!ans_ctx.active || !buffer || ans_ctx.sector_size == 0)
    return -1;
  if (ans_ctx.sector_size > ANS_PAGE_SIZE)
    return -1;

  ans_clear_queue(ans_ctx.io_buffer, sizeof(ans_ctx.io_buffer));
  if (write) {
    for (uint32_t i = 0; i < ans_ctx.sector_size; i++)
      ans_ctx.io_buffer[i] = src[i];
  }

  for (int i = 0; i < (int)sizeof(cmd); i++)
    ((uint8_t *)&cmd)[i] = 0;
  cmd.cdw0 = write ? 0x01 : 0x02;
  cmd.nsid = ans_ctx.nsid;
  cmd.prp1 = (uint64_t)(uintptr_t)ans_ctx.io_buffer;
  cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFFULL);
  cmd.cdw11 = (uint32_t)(lba >> 32);
  cmd.cdw12 = 0;

  if (ans_submit_io(&cmd) != 0)
    return -1;

  if (!write) {
    for (uint32_t i = 0; i < ans_ctx.sector_size; i++)
      dst[i] = ans_ctx.io_buffer[i];
  }
  return 0;
}

/* ===================================================================== */
/* Initialization */
/* ===================================================================== */

int ans_nvme_init(void) {
  printk(KERN_INFO "ANS: Initializing Apple NVMe controller\n");

#ifdef __QEMU__
  printk(KERN_INFO "ANS: Running in QEMU, using virtio-blk instead\n");
  return 0;
#endif

  vmm_map_range(ANS_BASE, ANS_BASE, ANS_SIZE, VM_DEVICE);
  ans_ctx.regs = (volatile uint32_t *)ANS_BASE;
  ans_ctx.nsid = 1;
  ans_ctx.sector_size = 512;
  ans_ctx.sector_count = 0;
  ans_ctx.admin_sq_tail = 0;
  ans_ctx.admin_cq_head = 0;
  ans_ctx.admin_phase = 1;
  ans_ctx.io_sq_tail = 0;
  ans_ctx.io_cq_head = 0;
  ans_ctx.io_phase = 1;
  ans_ctx.active = 0;

  printk(KERN_INFO "ANS: Boot status: 0x%x\n", ans_read32(ANS_BOOT_STATUS));
  if ((ans_read32(ANS_BOOT_STATUS) & 0x1) == 0) {
    int timeout = 1000;
    while (!(ans_read32(ANS_BOOT_STATUS) & 0x1) && timeout > 0)
      timeout--;
    if (timeout == 0) {
      printk(KERN_ERR "ANS: Timeout waiting for controller\n");
      return -1;
    }
  }

  {
    uint32_t cc = ans_read32(NVME_CC);
    if (cc & 1U) {
      ans_write32(NVME_CC, cc & ~1U);
      if (ans_wait_ready(0) != 0) {
        printk(KERN_ERR "ANS: Controller did not quiesce\n");
        return -1;
      }
    }
  }

  ans_clear_queue(ans_ctx.admin_sq, sizeof(ans_ctx.admin_sq));
  ans_clear_queue(ans_ctx.admin_cq, sizeof(ans_ctx.admin_cq));
  ans_clear_queue(ans_ctx.io_sq, sizeof(ans_ctx.io_sq));
  ans_clear_queue(ans_ctx.io_cq, sizeof(ans_ctx.io_cq));

  ans_write32(NVME_AQA, ((ANS_ADMIN_Q_DEPTH - 1) << 16) | (ANS_ADMIN_Q_DEPTH - 1));
  ans_write32(NVME_ASQ, (uint32_t)(uintptr_t)ans_ctx.admin_sq);
  ans_write32(NVME_ASQ + 4,
              (uint32_t)(((uint64_t)(uintptr_t)ans_ctx.admin_sq) >> 32));
  ans_write32(NVME_ACQ, (uint32_t)(uintptr_t)ans_ctx.admin_cq);
  ans_write32(NVME_ACQ + 4,
              (uint32_t)(((uint64_t)(uintptr_t)ans_ctx.admin_cq) >> 32));
  ans_write32(NVME_CC, (6U << 20) | (4U << 16) | 1U);

  if (ans_wait_ready(1) != 0) {
    printk(KERN_ERR "ANS: Controller did not become ready\n");
    return -1;
  }

  {
    nvme_sqe_t cmd;

    for (int i = 0; i < (int)sizeof(cmd); i++)
      ((uint8_t *)&cmd)[i] = 0;
    cmd.cdw0 = 0x05;
    cmd.prp1 = (uint64_t)(uintptr_t)ans_ctx.io_cq;
    cmd.cdw10 = ((ANS_IO_Q_DEPTH - 1) << 16) | 1U;
    cmd.cdw11 = 1U;
    if (ans_submit_admin(&cmd) != 0) {
      printk(KERN_ERR "ANS: Failed to create I/O completion queue\n");
      return -1;
    }

    for (int i = 0; i < (int)sizeof(cmd); i++)
      ((uint8_t *)&cmd)[i] = 0;
    cmd.cdw0 = 0x01;
    cmd.prp1 = (uint64_t)(uintptr_t)ans_ctx.io_sq;
    cmd.cdw10 = ((ANS_IO_Q_DEPTH - 1) << 16) | 1U;
    cmd.cdw11 = 1U | (1U << 16);
    if (ans_submit_admin(&cmd) != 0) {
      printk(KERN_ERR "ANS: Failed to create I/O submission queue\n");
      return -1;
    }

    for (int i = 0; i < (int)sizeof(cmd); i++)
      ((uint8_t *)&cmd)[i] = 0;
    for (int i = 0; i < (int)sizeof(ans_ctx.identify_ns); i++)
      ans_ctx.identify_ns[i] = 0;
    cmd.cdw0 = 0x06;
    cmd.nsid = 1;
    cmd.prp1 = (uint64_t)(uintptr_t)ans_ctx.identify_ns;
    cmd.cdw10 = 0;
    if (ans_submit_admin(&cmd) != 0) {
      printk(KERN_ERR "ANS: Failed to identify namespace\n");
      return -1;
    }
  }

  ans_ctx.sector_count = ans_read_le64(&ans_ctx.identify_ns[0]);
  {
    uint8_t flbas = ans_ctx.identify_ns[26];
    uint8_t lbaf_index = flbas & 0x0F;
    uint8_t lbads = ans_ctx.identify_ns[128 + lbaf_index * 4 + 2];
    if (lbads >= 9 && lbads < 17)
      ans_ctx.sector_size = 1U << lbads;
  }

  if (ans_ctx.sector_size != 512) {
    printk(KERN_ERR "ANS: Unsupported sector size %u\n", ans_ctx.sector_size);
    return -1;
  }

  ans_ctx.active = 1;

  printk(KERN_INFO "ANS: NVMe CAP: 0x%llx\n",
         (unsigned long long)(((uint64_t)ans_read32(NVME_CAP + 4) << 32) |
                              ans_read32(NVME_CAP)));

  ans_initialized = true;
  storage_register_disk_device("Apple NVMe Disk", STORAGE_KIND_APPLE_ANS,
                               "nvme0");
  if (storage_register_disk_backend("nvme0", ans_read_blocks, ans_write_blocks,
                                    &ans_ctx) != 0) {
    printk(KERN_ERR "ANS: Failed to register disk backend\n");
    return -1;
  }

  printk(KERN_INFO "ANS: NVMe controller initialized\n");
  return 0;
}

/* ===================================================================== */
/* Block Operations */
/* ===================================================================== */

int ans_read_blocks(uint64_t lba, uint32_t count, void *buffer, void *ctx) {
  uint8_t *dst = (uint8_t *)buffer;
  ans_nvme_ctx_t *use_ctx = (ans_nvme_ctx_t *)ctx;

  if (!use_ctx)
    use_ctx = &ans_ctx;
  if (!ans_initialized || !use_ctx->active || !buffer || count == 0)
    return -1;
  if (use_ctx->sector_count > 0 && lba + (uint64_t)count > use_ctx->sector_count)
    return -1;

  for (uint32_t i = 0; i < count; i++) {
    if (ans_nvme_rw_one(lba + i, dst + i * use_ctx->sector_size, 0) != 0)
      return -1;
  }
  return 0;
}

int ans_write_blocks(uint64_t lba, uint32_t count, const void *buffer, void *ctx) {
  const uint8_t *src = (const uint8_t *)buffer;
  ans_nvme_ctx_t *use_ctx = (ans_nvme_ctx_t *)ctx;

  if (!use_ctx)
    use_ctx = &ans_ctx;
  if (!ans_initialized || !use_ctx->active || !buffer || count == 0)
    return -1;
  if (use_ctx->sector_count > 0 && lba + (uint64_t)count > use_ctx->sector_count)
    return -1;

  for (uint32_t i = 0; i < count; i++) {
    if (ans_nvme_rw_one(lba + i, (void *)(uintptr_t)(src + i * use_ctx->sector_size),
                        1) != 0)
      return -1;
  }
  return 0;
}

/* ===================================================================== */
/* Power Management */
/* ===================================================================== */

int ans_suspend(void) {
  if (!ans_initialized)
    return 0;

  printk(KERN_INFO "ANS: Suspending NVMe controller\n");
  return 0;
}

int ans_resume(void) {
  if (!ans_initialized)
    return 0;

  printk(KERN_INFO "ANS: Resuming NVMe controller\n");
  return 0;
}
