#!/bin/sh

set -eu

os_master_bootsplash_start() {
  mode="$1"
  runtime_dir="/tmp/os-master-bootsplash-${mode}"

  rm -rf "${runtime_dir}"
  mkdir -p "${runtime_dir}"
  printf '0\n' > "${runtime_dir}/progress"

  if [ -x /usr/local/bin/os-master-bootsplashd ]; then
    /usr/local/bin/os-master-bootsplashd "${mode}" "${runtime_dir}" >/dev/null 2>&1 &
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
