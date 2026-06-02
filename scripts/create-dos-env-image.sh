#!/bin/bash
# Create a hybrid BIOS/UEFI text-setup environment image with DOS payload files.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-build/x86_64}"
IMAGE_DIR="${2:-image}"

env \
  IMAGE_NAME="${IMAGE_NAME:-os8-x86_64-dos-env.img}" \
  BOOT_PROFILE=installer \
  LIMINE_CFG_SOURCE="${LIMINE_CFG_SOURCE:-${ROOT_DIR}/os-x86_64/limine-installer-text.conf}" \
  INCLUDE_INSTALL_PAYLOAD=1 \
  INCLUDE_DOS_ENV=1 \
  DOS_INSTALLER_DIR="${DOS_INSTALLER_DIR:-${BUILD_DIR}/dos-installer}" \
  bash "${ROOT_DIR}/scripts/create-uefi-image.sh" "${BUILD_DIR}" "${IMAGE_DIR}"
