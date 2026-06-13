#!/bin/sh

set -eu

os_master_bootsplash_start() {
  mode="$1"
  runtime_dir="/tmp/os-master-bootsplash-${mode}"
  pid_file="${runtime_dir}/pid"

  mkdir -p "${runtime_dir}"
  rm -f "${runtime_dir}/complete"
  printf '0\n' > "${runtime_dir}/progress"

  if [ -f "${pid_file}" ]; then
    old_pid="$(cat "${pid_file}" 2>/dev/null || true)"
    if [ -n "${old_pid}" ] && kill -0 "${old_pid}" >/dev/null 2>&1; then
      return 0
    fi
    rm -f "${pid_file}"
  fi

  if [ -x /usr/local/bin/os-master-bootsplashd ]; then
    /usr/local/bin/os-master-bootsplashd "${mode}" "${runtime_dir}" >/dev/null 2>&1 &
    printf '%s\n' "$!" > "${pid_file}"
  fi
}

os_master_bootsplash_progress() {
  mode="$1"
  progress="$2"
  runtime_dir="/tmp/os-master-bootsplash-${mode}"

  if [ ! -d "${runtime_dir}" ]; then
    return 0
  fi

  printf '%s\n' "${progress}" > "${runtime_dir}/progress"
}

os_master_bootsplash_complete() {
  mode="$1"
  runtime_dir="/tmp/os-master-bootsplash-${mode}"

  if [ ! -d "${runtime_dir}" ]; then
    return 0
  fi

  : > "${runtime_dir}/complete"
}
