#!/bin/sh

BOOTSPLASH_HELPER="/usr/local/share/os-master-desktop/session/bootsplash-common.sh"
if [ -r "${BOOTSPLASH_HELPER}" ]; then
  . "${BOOTSPLASH_HELPER}"
  os_master_bootsplash_start installer
fi

exec /usr/local/bin/os-master-installer-session
