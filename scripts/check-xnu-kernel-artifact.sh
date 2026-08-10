#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
  echo "usage: $0 <arch> <kernel-artifact>" >&2
  exit 2
fi

arch="$1"
kernel="$2"
root="$(cd "$(dirname "$0")/.." && pwd)"
work_dir="${TMPDIR:-/tmp}/os8-xnu-kernel-artifact.$$"
source_file="$work_dir/check-xnu-kernel-artifact.c"
binary_file="$work_dir/check-xnu-kernel-artifact"

cleanup() {
  rm -rf "$work_dir"
}
trap cleanup EXIT

case "$arch" in
  x86_64) arch_id="OS8_XNU_ARCH_X86_64" ;;
  arm64) arch_id="OS8_XNU_ARCH_ARM64" ;;
  *)
    echo "error: XNU kernel artifact supports ARCH=x86_64 or ARCH=arm64, got: $arch" >&2
    exit 1
    ;;
esac

if [ ! -s "$kernel" ]; then
  echo "error: XNU kernel artifact is missing or empty: $kernel" >&2
  exit 1
fi

cc_bin="${CC:-}"
if [ -z "$cc_bin" ]; then
  cc_bin="$(command -v clang 2>/dev/null || command -v cc 2>/dev/null || true)"
fi
if [ -z "$cc_bin" ]; then
  echo "error: clang or cc is required to validate the XNU kernel artifact" >&2
  exit 1
fi

mkdir -p "$work_dir"
cat > "$source_file" <<C
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "xnu_macho_loader.h"

int main(int argc, char **argv) {
  FILE *file;
  long size_long;
  uint64_t size;
  unsigned char *data;
  os8_xnu_macho64_image_t image;
  os8_xnu_macho64_segment_t segment;

  if (argc != 2) return 2;
  file = fopen(argv[1], "rb");
  if (!file) return 1;
  if (fseek(file, 0, SEEK_END) != 0) return 1;
  size_long = ftell(file);
  if (size_long <= 0) return 1;
  if (fseek(file, 0, SEEK_SET) != 0) return 1;
  size = (uint64_t)size_long;
  data = (unsigned char *)malloc((size_t)size);
  if (!data) return 1;
  if (fread(data, 1, (size_t)size, file) != (size_t)size) return 1;
  fclose(file);

  if (os8_xnu_macho64_inspect(data, size, $arch_id, &image) != 0) return 1;
  if (image.segment_count == 0 || image.entry_vmaddr == 0) return 1;
  for (uint32_t i = 0; i < image.segment_count; i++) {
    if (os8_xnu_macho64_segment_at(data, size, $arch_id, i, &segment) != 0)
      return 1;
    if (segment.vmsize == 0 || segment.filesize > segment.vmsize) return 1;
  }

  free(data);
  return 0;
}
C

"$cc_bin" -std=c11 -Wall -Wextra -I"$root/boot/xnu" \
  "$source_file" -o "$binary_file"
if ! "$binary_file" "$kernel"; then
  echo "error: compiled XNU kernel artifact is not a supported $arch Mach-O payload: $kernel" >&2
  exit 1
fi

echo "[XNU] Kernel artifact verified: $kernel"
