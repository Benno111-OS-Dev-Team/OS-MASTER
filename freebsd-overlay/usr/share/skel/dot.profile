#!/bin/sh

case ":${PATH:-}:" in
  *:/usr/local/bin:*) ;;
  *) PATH="/usr/local/bin:/usr/local/sbin:${PATH:-/bin:/sbin:/usr/bin:/usr/sbin}" ;;
esac

export PATH
