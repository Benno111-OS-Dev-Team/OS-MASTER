#!/bin/bash
# Compatibility wrapper for the retired Limine-specific x86_64 build path.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== OS8 custom UEFI boot build ==="
echo "[INFO] The Limine UEFI loader path has been retired."
echo "[INFO] Building x86_64 media through OS8's custom EFI loader chain."

cd "$ROOT_DIR"
exec make -f Makefile.multiarch ARCH=x86_64 image
