#!/bin/sh

set -eu

OS_MASTER_X11_LOG="${OS_MASTER_X11_LOG:-/var/log/os-master-x11.log}"

os_master_log() {
  mkdir -p "$(dirname "${OS_MASTER_X11_LOG}")"
  printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "${OS_MASTER_X11_LOG}"
}

os_master_find_local_repo() {
  for candidate in \
    /packages \
    /dist/packages \
    /usr/freebsd-packages \
    /usr/freebsd-packages/offline \
    /cdrom/packages
  do
    if [ -d "${candidate}" ] && find "${candidate}" -maxdepth 2 \( -name '*.pkg' -o -name 'meta.conf' -o -name 'packagesite.pkg' -o -name 'packagesite.txz' \) | grep -q .; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done
  return 1
}

os_master_repo_has_x11() {
  repo_dir="$1"
  find "${repo_dir}" -maxdepth 2 -name 'xorg-*.pkg' | grep -q .
}

os_master_write_repo_conf() {
  repo_root="$1"
  repo_dir="$2"

  mkdir -p "${repo_root}"
  cat > "${repo_root}/os-master-local.conf" <<EOF
OS_MASTER_LOCAL: {
  enabled: yes,
  url: "file://${repo_dir}",
  mirror_type: "none",
  signature_type: "none",
  fingerprints: "",
  priority: 100
}
FreeBSD: { enabled: no }
EOF
}

os_master_pkg() {
  rootdir="$1"
  shift

  if [ -n "${rootdir}" ]; then
    env ASSUME_ALWAYS_YES=yes pkg -r "${rootdir}" "$@"
  else
    env ASSUME_ALWAYS_YES=yes pkg "$@"
  fi
}

os_master_install_x11() {
  rootdir="${1:-}"
  repo_conf_root="/tmp/os-master-pkg-repos"

  rm -rf "${repo_conf_root}"

  if ! pkg -N >/dev/null 2>&1; then
    env ASSUME_ALWAYS_YES=yes pkg bootstrap -f
  fi

  if repo_dir="$(os_master_find_local_repo)" && os_master_repo_has_x11 "${repo_dir}"; then
    os_master_write_repo_conf "${repo_conf_root}" "${repo_dir}"
    export REPOS_DIR="${repo_conf_root}"
    os_master_log "Using local package repository at ${repo_dir}"
  else
    unset REPOS_DIR || true
    os_master_log "No local X11 package repository found; falling back to default pkg repositories"
  fi

  os_master_pkg "${rootdir}" update -f
  os_master_pkg "${rootdir}" install \
    xorg \
    xinit \
    openbox \
    tint2 \
    pcmanfm \
    xterm \
    feh \
    dejavu \
    liberation-fonts-ttf
}

os_master_seed_xinitrc() {
  target_root="$1"

  cat > "${target_root}/root/.xinitrc" <<'EOF'
#!/bin/sh
exec /usr/local/bin/os-master-session
EOF
  chmod 0644 "${target_root}/root/.xinitrc"

  if [ -d "${target_root}/usr/share/skel" ]; then
    cat > "${target_root}/usr/share/skel/dot.xinitrc" <<'EOF'
#!/bin/sh
exec /usr/local/bin/os-master-session
EOF
    chmod 0644 "${target_root}/usr/share/skel/dot.xinitrc"
  fi
}

os_master_add_video_users() {
  target_root="$1"

  if [ ! -f "${target_root}/etc/passwd" ]; then
    return 0
  fi

  awk -F: '$3 >= 1000 && $7 !~ /(false|nologin)$/ { print $1 }' "${target_root}/etc/passwd" | while IFS= read -r user_name; do
    [ -n "${user_name}" ] || continue
    chroot "${target_root}" pw groupmod video -m "${user_name}" || true
  done
}
