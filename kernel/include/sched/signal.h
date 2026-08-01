/*
 * OS8 Kernel - Signal Handling API
 */

#ifndef _SCHED_SIGNAL_H
#define _SCHED_SIGNAL_H

#include "sched/sched.h"
#include "types.h"

typedef uint64_t ksigset_t;

struct k_sigaction {
  void (*sa_handler)(int);
  ksigset_t sa_mask;
  int sa_flags;
  void (*sa_restorer)(void);
};

void signal_init(struct task_struct *task);
void signal_copy_state(struct task_struct *child, struct task_struct *parent);
int kill_task(struct task_struct *task, int sig);
void do_signal(struct task_struct *task);
ksigset_t signal_pending_mask(struct task_struct *task);
int sigprocmask(int how, const ksigset_t *set, ksigset_t *oldset);
int sigaction_syscall(int sig, const struct k_sigaction *act,
                      struct k_sigaction *oldact);

#endif /* _SCHED_SIGNAL_H */
