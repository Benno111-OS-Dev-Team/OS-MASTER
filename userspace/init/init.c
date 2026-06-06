/*
 * OS8 - Mini Init Process (PID 1)
 *
 * Tiny bootstrap that prepares the console and launches the user session.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define HOSTNAME "OS8"
#define LOGIN_PATH "/bin/login"
#define LOGIN_PATH_FALLBACK "/usr/bin/login"
#define SHELL_PATH "/bin/sh"
#define SHELL_PATH_FALLBACK "/usr/bin/sh"
#define CONSOLE_DEV "/dev/console"

static void setup_console(void) {
  int fd;

  close(0);
  close(1);
  close(2);

  fd = open(CONSOLE_DEV, O_RDWR);
  if (fd < 0)
    fd = open("/dev/ttyS0", O_RDWR);
  if (fd < 0)
    fd = open("/dev/tty1", O_RDWR);

  if (fd >= 0) {
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2)
      close(fd);
  }
}

static void prepare_runtime_dirs(void) {
  if (mkdir("/dev", 0755) < 0 && errno != EEXIST) {
  }
  if (mkdir("/tmp", 01777) < 0 && errno != EEXIST) {
  }
  if (mkdir("/var", 0755) < 0 && errno != EEXIST) {
  }
  if (mkdir("/var/run", 0755) < 0 && errno != EEXIST) {
  }
  if (mkdir("/var/tmp", 01777) < 0 && errno != EEXIST) {
  }
  if (mkdir("/usr", 0755) < 0 && errno != EEXIST) {
  }
  if (mkdir("/usr/bin", 0755) < 0 && errno != EEXIST) {
  }
  if (mkdir("/usr/sbin", 0755) < 0 && errno != EEXIST) {
  }

  if (sethostname(HOSTNAME, strlen(HOSTNAME)) < 0) {
    perror("sethostname");
  }
}

static int start_session(void) {
  pid_t pid = fork();

  if (pid < 0) {
    perror("fork");
    return -1;
  }

  if (pid == 0) {
    char *argv[] = {"/bin/login", NULL};
    char *envp[] = {"HOME=/root",
                    "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:/usr/local/sbin",
                    "SHELL=/bin/sh", "TERM=xterm", "USER=root", NULL};

    setsid();
    execve(LOGIN_PATH, argv, envp);
    execve(LOGIN_PATH_FALLBACK, argv, envp);

    char *shell_argv[] = {"/bin/sh", "-l", NULL};
    execve(SHELL_PATH, shell_argv, envp);
    shell_argv[0] = "/usr/bin/sh";
    execve(SHELL_PATH_FALLBACK, shell_argv, envp);
    perror("execve");
    _exit(127);
  }

  return (int)pid;
}

int main(int argc, char *argv[]) {
  int status;
  int pid;

  (void)argc;
  (void)argv;

  if (getpid() != 1) {
    fprintf(stderr, "init: must be PID 1\n");
    return 1;
  }

  setup_console();

  printf("\n[mini-init] OS8 PID 1 bootstrap\n");
  printf("[mini-init] Preparing basic runtime directories\n");
  prepare_runtime_dirs();

  for (;;) {
    printf("[mini-init] Starting user session\n");
    pid = start_session();
    if (pid < 0) {
      sleep(1);
      continue;
    }

    while (waitpid(pid, &status, 0) < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    printf("[mini-init] Session exited, restarting\n");
    sleep(1);
  }

  return 0;
}
