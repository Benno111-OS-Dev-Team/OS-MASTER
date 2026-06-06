#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#define CMD_MAX 256

static char cmd_buf[CMD_MAX];

static void trim_newline(char *buf) {
  size_t len;

  if (!buf)
    return;

  len = strlen(buf);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
    buf[len - 1] = '\0';
    len--;
  }
}

static void cmd_help(void) {
  puts("");
  puts("OS8 sh commands:");
  puts("  help     - Show this help");
  puts("  uname    - Show system information");
  puts("  hostname - Show current hostname");
  puts("  whoami   - Show current user");
  puts("  pid      - Show shell PID");
  puts("  echo     - Print text");
  puts("  clear    - Clear screen");
  puts("  exit     - Exit shell");
  puts("");
}

static void cmd_uname(void) {
  struct utsname uts;

  if (uname(&uts) == 0) {
    printf("%s %s %s %s %s\n", uts.sysname, uts.nodename, uts.release,
           uts.version, uts.machine);
  } else {
    puts("uname failed");
  }
}

static void cmd_hostname(void) {
  char host[65];

  if (gethostname(host, sizeof(host)) == 0) {
    puts(host);
  } else {
    puts("hostname unavailable");
  }
}

static void cmd_pid(void) { printf("PID: %d\n", (int)getpid()); }

static void cmd_whoami(void) {
  char *user = getenv("USER");
  puts(user && user[0] ? user : "root");
}

static void cmd_echo(const char *args) { puts(args ? args : ""); }

static void cmd_clear(void) { printf("\033[2J\033[H"); }

static void process_command(void) {
  char *args;

  args = cmd_buf;
  while (*args && *args != ' ')
    args++;
  if (*args == ' ') {
    *args++ = '\0';
    while (*args == ' ')
      args++;
  } else {
    args = NULL;
  }

  if (cmd_buf[0] == '\0') {
    return;
  } else if (strcmp(cmd_buf, "help") == 0 || strcmp(cmd_buf, "?") == 0) {
    cmd_help();
  } else if (strcmp(cmd_buf, "uname") == 0) {
    cmd_uname();
  } else if (strcmp(cmd_buf, "hostname") == 0) {
    cmd_hostname();
  } else if (strcmp(cmd_buf, "whoami") == 0) {
    cmd_whoami();
  } else if (strcmp(cmd_buf, "pid") == 0) {
    cmd_pid();
  } else if (strcmp(cmd_buf, "echo") == 0) {
    cmd_echo(args ? args : "");
  } else if (strcmp(cmd_buf, "clear") == 0) {
    cmd_clear();
  } else if (strcmp(cmd_buf, "exit") == 0) {
    puts("Goodbye!");
    exit(0);
  } else {
    printf("sh: %s: command not found\n", cmd_buf);
  }
}

int main(void) {
  puts("");
  puts("OS8 /bin/sh");
  puts("Type 'help' for available commands.");
  puts("");

  for (;;) {
    printf("sh# ");
    fflush(stdout);
    if (!fgets(cmd_buf, sizeof(cmd_buf), stdin)) {
      putchar('\n');
      continue;
    }
    trim_newline(cmd_buf);
    process_command();
  }
}
