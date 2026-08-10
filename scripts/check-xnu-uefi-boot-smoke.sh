#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work_dir="${TMPDIR:-/tmp}/os8-xnu-uefi-smoke.$$"
build_dir="$work_dir/build/x86_64"
image_dir="$work_dir/image"
kernel="$build_dir/kernel/xnu-x86_64.kernel"
image="$image_dir/xnu-x86_64-uefi.img"
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
    /usr/share/edk2/ovmf/OVMF_CODE_4M.fd; do
    if [ -f "$candidate" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

require_cmd clang
require_cmd lld-link
require_cmd mformat
require_cmd mmd
require_cmd mcopy
require_cmd qemu-system-x86_64
require_cmd timeout

ovmf="$(resolve_ovmf || true)"
if [ -z "$ovmf" ]; then
  echo "error: OVMF firmware not found; install ovmf or set OVMF_CODE" >&2
  exit 1
fi

mkdir -p "$build_dir/kernel" "$image_dir"
printf 'synthetic-xnu-kernel-for-uefi-smoke\n' > "$kernel"

KERNEL_PATH="$kernel" KERNEL_FORMAT=xnu \
  bash "$root/scripts/build-custom-uefi.sh" "$build_dir" >/dev/null
bash "$root/scripts/create-xnu-uefi-boot-image.sh" \
  "$build_dir" "$image_dir" "$kernel" >/dev/null

set +e
timeout --signal=TERM "${XNU_UEFI_SMOKE_SECONDS:-20}s" \
  qemu-system-x86_64 \
    -M q35 \
    -cpu qemu64 \
    -m 512M \
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
  echo "error: XNU UEFI smoke QEMU exited unexpectedly: $qemu_status" >&2
  exit 1
fi

if ! grep -q 'OS8 Startup Executable' "$serial_log"; then
  cat "$serial_log" >&2
  echo "error: XNU UEFI smoke did not reach the startup executable" >&2
  exit 1
fi

if ! grep -q 'KERNEL-0003' "$serial_log" ||
   ! grep -q 'supported x86_64 Mach-O image' "$serial_log"; then
  cat "$serial_log" >&2
  echo "error: XNU UEFI smoke did not exercise the XNU Mach-O validation path" >&2
  exit 1
fi

echo "[XNU] x86_64 UEFI boot smoke reached XNU startup validation"
