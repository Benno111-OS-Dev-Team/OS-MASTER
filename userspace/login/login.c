/*
 * OS8 - Login Program
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "password_hash.h"

#define ACCOUNT_CONFIG_PATH "/System/account.cfg"
#define ACCOUNTS_DIR "/System/Accounts"
#define PASSWD_PATH "/etc/passwd"
#define SHADOW_PATH "/etc/shadow"
#define MAX_USER_LEN 32
#define MAX_PASS_LEN 32
#define MAX_HOME_LEN 96
#define MAX_SHELL_LEN 32

struct user_cred {
  char username[MAX_USER_LEN];
  char password_hash[33];
  char partition_label[32];
  char disk_location[32];
  uid_t uid;
  gid_t gid;
  char home[MAX_HOME_LEN];
  char shell[MAX_SHELL_LEN];
  int configured;
};

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

static void build_default_home(const char *username, char *out, size_t len) {
  static const char *persistent_roots[] = {"/Persist", "/persist", "/disk",
                                           "/mnt/disk"};
  char candidate[MAX_HOME_LEN];

  if (!out || len == 0)
    return;

  if (username && strcmp(username, "root") == 0) {
    snprintf(out, len, "/root");
    return;
  }

  for (size_t i = 0; i < sizeof(persistent_roots) / sizeof(persistent_roots[0]);
       i++) {
    int fd;

    snprintf(candidate, sizeof(candidate), "%s/home/%s", persistent_roots[i],
             username ? username : "user");
    fd = open(candidate, O_RDONLY);
    if (fd >= 0) {
      close(fd);
      snprintf(out, len, "%s", candidate);
      return;
    }
  }

  snprintf(out, len, "/home/%s", username ? username : "user");
}

static void build_partition_home(const char *disk_location,
                                 const char *partition_label, char *out,
                                 size_t len) {
  if (!out || len == 0)
    return;
  if (!disk_location || !disk_location[0] || !partition_label ||
      !partition_label[0]) {
    out[0] = '\0';
    return;
  }
  snprintf(out, len, "/Installed/%s/%s", disk_location, partition_label);
}

static int manifest_get_value(const char *manifest, const char *key, char *out,
                              size_t max) {
  size_t key_len = 0;
  size_t i = 0;

  if (!manifest || !key || !out || max == 0)
    return -1;

  while (key[key_len])
    key_len++;

  while (manifest[i]) {
    size_t j = 0;

    while (j < key_len && manifest[i + j] == key[j])
      j++;
    if (j == key_len && manifest[i + j] == '=') {
      size_t out_idx = 0;
      i += key_len + 1;
      while (manifest[i] && manifest[i] != '\n' && manifest[i] != '\r' &&
             out_idx < max - 1) {
        out[out_idx++] = manifest[i++];
      }
      out[out_idx] = '\0';
      return 0;
    }

    while (manifest[i] && manifest[i] != '\n')
      i++;
    if (manifest[i] == '\n')
      i++;
  }

  out[0] = '\0';
  return -1;
}

static int account_manifest_path(const char *username, char *path, size_t len) {
  if (!username || !username[0] || !path || len < 32)
    return -1;
  snprintf(path, len, "%s/%s.cfg", ACCOUNTS_DIR, username);
  return 0;
}

static int read_account_manifest_file(const char *path, char *manifest,
                                      size_t manifest_len) {
  FILE *fp;
  size_t bytes;

  if (!path || !manifest || manifest_len == 0)
    return -1;

  fp = fopen(path, "r");
  if (!fp)
    return -1;
  bytes = fread(manifest, 1, manifest_len - 1, fp);
  fclose(fp);
  if (bytes == 0)
    return -1;
  manifest[bytes] = '\0';
  return 0;
}


static int unix_get_field(const char *line, int field, char *out, size_t max) {
  int current = 0;
  size_t idx = 0;

  if (!line || !out || max == 0 || field < 0)
    return -1;
  out[0] = '\0';
  for (size_t i = 0; line[i] && line[i] != '\n' && line[i] != '\r'; i++) {
    if (line[i] == ':') {
      if (current == field) {
        out[idx] = '\0';
        return 0;
      }
      current++;
      idx = 0;
      continue;
    }
    if (current == field && idx < max - 1)
      out[idx++] = line[i];
  }
  if (current == field) {
    out[idx] = '\0';
    return 0;
  }
  return -1;
}

static int unix_line_matches_user(const char *line, const char *username) {
  size_t i = 0;

  if (!line || !username || !username[0])
    return 0;
  while (username[i] && line[i] == username[i])
    i++;
  return username[i] == '\0' && line[i] == ':';
}

static int find_unix_line(const char *path, const char *username, char *out,
                          size_t out_len) {
  FILE *fp;
  char line[256];

  if (!path || !username || !out || out_len == 0)
    return -1;
  fp = fopen(path, "r");
  if (!fp)
    return -1;
  while (fgets(line, sizeof(line), fp)) {
    if (unix_line_matches_user(line, username)) {
      snprintf(out, out_len, "%s", line);
      trim_newline(out);
      fclose(fp);
      return 0;
    }
  }
  fclose(fp);
  return -1;
}

static int load_unix_account_by_name(const char *requested_user,
                                     struct user_cred *cred) {
  char passwd_line[256];
  char shadow_line[256];
  char field[96];

  if (!requested_user || !requested_user[0] || !cred)
    return -1;
  if (find_unix_line(PASSWD_PATH, requested_user, passwd_line,
                     sizeof(passwd_line)) != 0)
    return -1;

  memset(cred, 0, sizeof(*cred));
  snprintf(cred->shell, sizeof(cred->shell), "/bin/sh");
  snprintf(cred->username, sizeof(cred->username), "%s", requested_user);

  if (unix_get_field(passwd_line, 2, field, sizeof(field)) == 0)
    cred->uid = (uid_t)strtoul(field, NULL, 10);
  if (unix_get_field(passwd_line, 3, field, sizeof(field)) == 0)
    cred->gid = (gid_t)strtoul(field, NULL, 10);
  if (unix_get_field(passwd_line, 5, field, sizeof(field)) == 0 && field[0])
    snprintf(cred->home, sizeof(cred->home), "%s", field);
  if (unix_get_field(passwd_line, 6, field, sizeof(field)) == 0 && field[0])
    snprintf(cred->shell, sizeof(cred->shell), "%s", field);

  if (find_unix_line(SHADOW_PATH, requested_user, shadow_line,
                     sizeof(shadow_line)) == 0 &&
      unix_get_field(shadow_line, 1, field, sizeof(field)) == 0 && field[0]) {
    snprintf(cred->password_hash, sizeof(cred->password_hash), "%s", field);
  } else if (unix_get_field(passwd_line, 1, field, sizeof(field)) == 0 &&
             field[0] && strcmp(field, "x") != 0) {
    snprintf(cred->password_hash, sizeof(cred->password_hash), "%s", field);
  } else {
    return -1;
  }

  if (!cred->home[0])
    build_default_home(cred->username, cred->home, sizeof(cred->home));
  cred->configured = 1;
  return 0;
}

static int load_system_account_by_name(const char *requested_user,
                                       struct user_cred *cred) {
  char manifest[192];
  char account_path[128];
  char legacy_password[MAX_PASS_LEN];

  if (!cred)
    return -1;

  if (load_unix_account_by_name(requested_user, cred) == 0)
    return 0;

  memset(cred, 0, sizeof(*cred));
  snprintf(cred->shell, sizeof(cred->shell), "/bin/sh");

  if (requested_user && requested_user[0] &&
      account_manifest_path(requested_user, account_path,
                            sizeof(account_path)) == 0 &&
      read_account_manifest_file(account_path, manifest, sizeof(manifest)) == 0) {
    /* Loaded from per-user account manifest. */
  } else if (read_account_manifest_file(ACCOUNT_CONFIG_PATH, manifest,
                                        sizeof(manifest)) != 0) {
    return -1;
  }

  if (manifest_get_value(manifest, "username", cred->username,
                         sizeof(cred->username)) != 0 ||
      !cred->username[0]) {
    return -1;
  }

  if (manifest_get_value(manifest, "password_hash", cred->password_hash,
                         sizeof(cred->password_hash)) != 0 ||
      !cred->password_hash[0]) {
    legacy_password[0] = '\0';
    if (manifest_get_value(manifest, "password", legacy_password,
                           sizeof(legacy_password)) != 0 ||
        !legacy_password[0]) {
      return -1;
    }
    vib_password_hash_hex(cred->username, legacy_password, cred->password_hash,
                          sizeof(cred->password_hash));
  }

  cred->partition_label[0] = '\0';
  cred->disk_location[0] = '\0';
  manifest_get_value(manifest, "partition_label", cred->partition_label,
                     sizeof(cred->partition_label));
  manifest_get_value(manifest, "disk_location", cred->disk_location,
                     sizeof(cred->disk_location));
  build_partition_home(cred->disk_location, cred->partition_label, cred->home,
                       sizeof(cred->home));
  if (!cred->home[0])
    build_default_home(cred->username, cred->home, sizeof(cred->home));
  cred->uid = strcmp(cred->username, "root") == 0 ? 0 : 1000;
  cred->gid = cred->uid;
  cred->configured = 1;
  return 0;
}

static void get_password(char *buf, size_t len) {
  printf("(visible) ");
  if (!fgets(buf, (int)len, stdin)) {
    if (len > 0)
      buf[0] = '\0';
    return;
  }
  trim_newline(buf);
}

static int authenticate(const struct user_cred *cred, const char *user,
                        const char *pass) {
  char password_hash[33];

  if (!cred || !cred->configured || !user || !pass)
    return 0;
  if (strcmp(user, cred->username) != 0)
    return 0;

  vib_password_hash_hex(user, pass, password_hash, sizeof(password_hash));
  return vib_secure_string_eq(password_hash, cred->password_hash);
}

int main(int argc, char *argv[]) {
  char username[MAX_USER_LEN];
  char password[MAX_PASS_LEN];
  struct user_cred cred;

  (void)argc;
  (void)argv;

  printf("\033[2J\033[H");
  printf("Welcome to OS8 v8.0.0\n");
  printf("Kernel 8.0.0-arm64 on an aarch64\n\n");

  while (1) {
    printf("OS8 login: ");
    fflush(stdout);

    if (fgets(username, sizeof(username), stdin) == NULL)
      continue;
    trim_newline(username);

    if (strlen(username) == 0)
      continue;

    printf("Password: ");
    fflush(stdout);
    get_password(password, sizeof(password));

    if (load_system_account_by_name(username, &cred) != 0) {
      printf("\nLogin incorrect\n\n");
      sleep(2);
      continue;
    }

    if (authenticate(&cred, username, password)) {
      char home_env[MAX_HOME_LEN + 5];
      char shell_env[MAX_SHELL_LEN + 7];
      char *shell_argv[] = {cred.shell, "-l", NULL};
      char *envp[] = {home_env, "PATH=/bin:/sbin:/usr/bin:/usr/sbin",
                      shell_env, "TERM=linux", NULL};

      snprintf(home_env, sizeof(home_env), "HOME=%s", cred.home);
      snprintf(shell_env, sizeof(shell_env), "SHELL=%s", cred.shell);

      printf("\nLogin successful.\n\n");
      chdir(cred.home);
      execve(cred.shell, shell_argv, envp);

      printf("Failed to execute shell: %s\n", cred.shell);
      _exit(1);
    }

    printf("\nLogin incorrect\n\n");
    sleep(2);
  }

  return 0;
}
