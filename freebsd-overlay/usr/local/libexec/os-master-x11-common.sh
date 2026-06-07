#!/bin/sh

set -eu

OS_MASTER_X11_LOG="${OS_MASTER_X11_LOG:-/var/log/os-master-x11.log}"
OS_MASTER_X11_MARKER="${OS_MASTER_X11_MARKER:-/var/db/os-master/x11-installed}"

os_master_log() {
  mkdir -p "$(dirname "${OS_MASTER_X11_LOG}")"
  printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "${OS_MASTER_X11_LOG}"
}

os_master_mark_x11_installed() {
  marker_path="${1:-${OS_MASTER_X11_MARKER}}"
  mkdir -p "$(dirname "${marker_path}")"
  : > "${marker_path}"
}

os_master_x11_is_installed() {
  rootdir="${1:-}"

  if [ -n "${rootdir}" ]; then
    [ -x "${rootdir}/usr/local/bin/startx" ] && [ -x "${rootdir}/usr/local/bin/Xorg" ]
    return
  fi

  command -v startx >/dev/null 2>&1 && command -v Xorg >/dev/null 2>&1
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
    dbus \
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

os_master_seed_xsession() {
  target_root="$1"

  cat > "${target_root}/root/.xsession" <<'EOF'
#!/bin/sh
exec /usr/local/bin/os-master-xsession
EOF
  chmod 0644 "${target_root}/root/.xsession"

  if [ -d "${target_root}/usr/share/skel" ]; then
    cat > "${target_root}/usr/share/skel/dot.xsession" <<'EOF'
#!/bin/sh
exec /usr/local/bin/os-master-xsession
EOF
    chmod 0644 "${target_root}/usr/share/skel/dot.xsession"
  fi
}

os_master_enable_rc_conf_flag() {
  target_root="$1"
  flag_name="$2"
  flag_value="$3"
  rc_conf_path="${target_root}/etc/rc.conf"
  temp_path="${rc_conf_path}.tmp"

  mkdir -p "$(dirname "${rc_conf_path}")"
  if [ ! -f "${rc_conf_path}" ]; then
    : > "${rc_conf_path}"
  fi

  awk -v name="${flag_name}" '$0 !~ ("^" name "=")' "${rc_conf_path}" > "${temp_path}"
  printf '%s="%s"\n' "${flag_name}" "${flag_value}" >> "${temp_path}"
  mv "${temp_path}" "${rc_conf_path}"
}

os_master_enable_dbus() {
  target_root="$1"
  os_master_enable_rc_conf_flag "${target_root}" "dbus_enable" "YES"
}

os_master_enable_graphical_login() {
  target_root="$1"
  ttys_path="${target_root}/etc/ttys"
  temp_path="${ttys_path}.tmp"
  xdm_line='ttyv8   "/usr/local/bin/xdm -nodaemon"  xterm   on  secure'

  if [ ! -f "${ttys_path}" ]; then
    return 0
  fi

  awk -v xdm_line="${xdm_line}" '
    BEGIN {
      replaced = 0
    }
    $1 == "ttyv8" {
      print xdm_line
      replaced = 1
      next
    }
    {
      print
    }
    END {
      if (!replaced) {
        print xdm_line
      }
    }
  ' "${ttys_path}" > "${temp_path}"
  mv "${temp_path}" "${ttys_path}"
}

os_master_activate_graphical_login() {
  if command -v service >/dev/null 2>&1; then
    service dbus onestart >/dev/null 2>&1 || true
  fi

  kill -HUP 1 >/dev/null 2>&1 || true
}

os_master_seed_desktop_login() {
  target_root="$1"

  os_master_seed_xinitrc "${target_root}"
  os_master_seed_xsession "${target_root}"
  os_master_enable_dbus "${target_root}"
  os_master_enable_graphical_login "${target_root}"
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

os_master_seed_firstboot_setup() {
  target_root="$1"

  if [ -z "${target_root}" ] || [ ! -d "${target_root}" ]; then
    return 0
  fi

  mkdir -p "${target_root}/var/db/os-master"
  rm -f "${target_root}/var/db/os-master/firstboot-setup-complete"
}
