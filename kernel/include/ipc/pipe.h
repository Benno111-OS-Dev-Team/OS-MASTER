/*
 * UnixOS Kernel - Pipe IPC Header
 */

#ifndef _IPC_PIPE_H
#define _IPC_PIPE_H

#include "fs/vfs.h"

int do_pipe(struct file **read_file, struct file **write_file);

#endif /* _IPC_PIPE_H */
