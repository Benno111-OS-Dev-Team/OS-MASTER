#!/usr/bin/env bash

set -euo pipefail

PRODUCT_NAME="${PRODUCT_NAME:-os-master-freebsd}"
FREEBSD_RELEASE="${FREEBSD_RELEASE:-14.4-RELEASE}"
FREEBSD_ARCH="${FREEBSD_ARCH:-amd64}"
FREEBSD_IMAGE_BASENAME="${FREEBSD_IMAGE_BASENAME:-dvd1.iso}"
OUTPUT_DIR="${OUTPUT_DIR:-image}"

image_path="${OUTPUT_DIR}/${PRODUCT_NAME}-${FREEBSD_RELEASE}-${FREEBSD_ARCH}-${FREEBSD_IMAGE_BASENAME}"

if [ ! -f "${image_path}" ]; then
  echo "[ERROR] Missing staged image: ${image_path}" >&2
  echo "[INFO] Run 'make image' first." >&2
  exit 1
fi

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
  echo "[ERROR] qemu-system-x86_64 is required" >&2
  exit 1
fi

exec qemu-system-x86_64 \
  -m 4096 \
  -smp 2 \
  -boot d \
  -cdrom "${image_path}" \
  -serial mon:stdio
