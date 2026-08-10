#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <xnu-x86_64-uefi.img>" >&2
  exit 2
fi

image="$1"
work_dir="${TMPDIR:-/tmp}/os8-xnu-compiled-uefi-smoke.$$"
serial_log="$work_dir/qemu-serial.log"

cleanup() {
  rm -rf "$work_dir"
}
trap cleanup EXIT

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: required command is missing: $1" >&2
    exit 1
  fi
}

resolve_ovmf() {
  if [ -n "${OVMF_CODE:-}" ] && [ -f "$OVMF_CODE" ]; then
    printf '%s\n' "$OVMF_CODE"
    return 0
  fi
  for candidate in \
    /usr/share/qemu/OVMF.fd \
    /usr/share/ovmf/OVMF.fd \
    /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/OVMF/OVMF_CODE_4M.fd \
    /usr/share/edk2/ovmf/OVMF_CODE.fd \
    /usr/share/edk2/ovmf/OVMF_CODE_4M.fd \
    /opt/homebrew/share/qemu/edk2-x86_64-code.fd \
    /usr/local/share/qemu/edk2-x86_64-code.fd \
    /opt/homebrew/share/qemu/OVMF.fd \
    /usr/local/share/qemu/OVMF.fd; do
    if [ -f "$candidate" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

require_cmd qemu-system-x86_64
timeout_bin="$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)"
if [ -z "$timeout_bin" ]; then
  echo "error: required command is missing: timeout or gtimeout" >&2
  exit 1
fi

if [ ! -s "$image" ]; then
  echo "error: compiled XNU UEFI image is missing or empty: $image" >&2
  exit 1
fi

ovmf="$(resolve_ovmf || true)"
if [ -z "$ovmf" ]; then
  echo "error: OVMF firmware not found; install OVMF or set OVMF_CODE" >&2
  exit 1
fi

mkdir -p "$work_dir"

set +e
"$timeout_bin" --signal=TERM "${XNU_COMPILED_UEFI_SMOKE_SECONDS:-30}s" \
  qemu-system-x86_64 \
    -M q35 \
    -cpu qemu64 \
    -m 1024M \
    -nographic \
    -monitor none \
    -no-reboot \
    -no-shutdown \
    -bios "$ovmf" \
    -drive "format=raw,file=$image,if=ide" \
    >"$serial_log" 2>&1
qemu_status=$?
set -e

if [ "$qemu_status" -ne 0 ] && [ "$qemu_status" -ne 124 ] && [ "$qemu_status" -ne 143 ]; then
  cat "$serial_log" >&2
  echo "error: compiled XNU UEFI smoke QEMU exited unexpectedly: $qemu_status" >&2
  exit 1
fi

if ! grep -q 'OS8 Startup Executable' "$serial_log"; then
  cat "$serial_log" >&2
  echo "error: compiled XNU UEFI smoke did not reach the startup executable" >&2
  exit 1
fi

if grep -q 'OS8 startup error' "$serial_log"; then
  cat "$serial_log" >&2
  echo "error: compiled XNU UEFI smoke hit a startup error before handoff" >&2
  exit 1
fi

if ! grep -q 'Kernel verified and loaded. Exiting boot services' "$serial_log" ||
   ! grep -q 'Entering XNU kernel after ExitBootServices' "$serial_log" ||
   ! grep -q 'Entered ExitBootServices; jumping to XNU kernel' "$serial_log"; then
  cat "$serial_log" >&2
  echo "error: compiled XNU UEFI smoke did not reach the XNU handoff point" >&2
  exit 1
fi

echo "[XNU] compiled x86_64 UEFI boot smoke reached XNU handoff"
