#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="${TMPDIR:-/tmp}/os8-storage-driver-check"
mkdir -p "$out_dir"

common_flags=(
  -Wall
  -Wextra
  -Wno-unused-function
  -ffreestanding
  -fno-stack-protector
  -fno-pic
  -O2
  -g
  -Ikernel/include
  -Ikernel
  -Inewwindows/include
  -I.
  -Ishared-api
  -fno-builtin
  -nostdlib
  -nostdinc
  -DWINDOW_SKIN_USE_KERNEL_TYPES
)

cd "$root"

clang "${common_flags[@]}" \
  --target=x86_64-unknown-none-elf \
  -mcmodel=kernel \
  -mno-red-zone \
  -mno-mmx \
  -mno-sse \
  -mno-sse2 \
  -Ibuild/x86_64/generated \
  -DARCH_X86_64 \
  -c kernel/drivers/storage.c \
  -o "$out_dir/storage-x86_64.o"

clang "${common_flags[@]}" \
  --target=i686-unknown-none-elf \
  -m32 \
  -march=i686 \
  -Ibuild/x86/generated \
  -DARCH_X86 \
  -c kernel/drivers/storage.c \
  -o "$out_dir/storage-x86.o"

clang "${common_flags[@]}" \
  --target=aarch64-unknown-none-elf \
  -mcpu=cortex-a72 \
  -mgeneral-regs-only \
  -Ibuild/arm64/generated \
  -DARCH_ARM64 \
  -c kernel/drivers/storage.c \
  -o "$out_dir/storage-arm64.o"

echo "storage driver compile check: x86_64, x86, and arm64 passed"
