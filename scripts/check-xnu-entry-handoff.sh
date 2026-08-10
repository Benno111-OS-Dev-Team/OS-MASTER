#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work_dir="${TMPDIR:-/tmp}/os8-xnu-entry-check.$$"
object_file="$work_dir/startup-handoff.obj"
source_file="$root/boot/custom/startup-handoff.S"

cleanup() {
  rm -rf "$work_dir"
}
trap cleanup EXIT

cc_bin="${CC:-}"
if [ -z "$cc_bin" ]; then
  cc_bin="$(command -v clang 2>/dev/null || true)"
fi
if [ -z "$cc_bin" ]; then
  echo "error: clang is required to compile-check the XNU entry handoff" >&2
  exit 1
fi

nm_bin="$(command -v llvm-nm 2>/dev/null || command -v nm 2>/dev/null || true)"
if [ -z "$nm_bin" ]; then
  echo "error: llvm-nm or nm is required to inspect the XNU entry handoff" >&2
  exit 1
fi

if [ ! -s "$source_file" ]; then
  echo "error: startup handoff assembly is missing: $source_file" >&2
  exit 1
fi

mkdir -p "$work_dir"
"$cc_bin" \
  -target x86_64-unknown-windows \
  -ffreestanding \
  -fshort-wchar \
  -mno-red-zone \
  -fno-stack-protector \
  -fno-builtin \
  -nostdlib \
  -Wall \
  -Wextra \
  -c "$source_file" \
  -o "$object_file"

if ! "$nm_bin" "$object_file" | grep -q 'startup_enter_xnu_kernel'; then
  echo "error: startup handoff object does not export startup_enter_xnu_kernel" >&2
  exit 1
fi

grep -Eq 'mov[[:space:]]+%r8d,[[:space:]]*%edi' "$source_file"
grep -Eq 'jmp[[:space:]]+\*%rdx' "$source_file"

echo "[XNU] x86_64 entry handoff shim compile-checked"
