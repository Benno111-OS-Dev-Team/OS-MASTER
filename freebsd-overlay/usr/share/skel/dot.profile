#!/bin/sh

case ":${PATH:-}:" in
  *:/usr/local/bin:*) ;;
  *) PATH="/usr/local/bin:/usr/local/sbin:${PATH:-/bin:/sbin:/usr/bin:/usr/sbin}" ;;
esac

export PATH

if [ -z "${DISPLAY:-}" ] && [ -z "${SSH_TTY:-}" ] && [ -t 0 ] && [ -x /usr/local/bin/startx ] && [ ! -f /var/db/os-master/firstboot-setup-complete ]; then
  if [ ! -f /tmp/os-master-autostartx.lock ]; then
    : > /tmp/os-master-autostartx.lock
    exec /usr/local/bin/startx
  fi
fi
