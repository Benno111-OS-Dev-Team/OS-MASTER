#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:?missing build dir}"
IMAGE_DIR="${2:?missing image dir}"

KERNEL_ELF="${KERNEL_PATH:-${BUILD_DIR}/kernel/os-arm64.elf}"
ISO_NAME="${ISO_NAME:-os8-arm64.iso}"
ISO_LABEL="${ISO_LABEL:-OS_ARM64}"
KERNEL_NAME="${KERNEL_NAME:-os-arm64.elf}"
ISO_PATH="${IMAGE_DIR}/${ISO_NAME}"

STAGING_DIR="${BUILD_DIR}/arm64-iso"
EFI_DIR="${STAGING_DIR}/EFI/BOOT"
BOOTAA64_EFI="${EFI_DIR}/BOOTAA64.EFI"

compress_iso() {
    local iso_path="$1"
    local compressed_path="${iso_path}.xz"

    rm -f "$compressed_path"

    if command -v xz >/dev/null 2>&1; then
        xz -T0 -9e -k -f "$iso_path"
        return 0
    fi

    local python_cmd=""
    python_cmd="$(command -v python3 2>/dev/null || command -v python 2>/dev/null || true)"
    if [ -n "$python_cmd" ]; then
        "$python_cmd" - "$iso_path" "$compressed_path" <<'PY'
import lzma
import shutil
import sys

src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f_in, lzma.open(dst, "wb", preset=(lzma.PRESET_EXTREME | 9)) as f_out:
    shutil.copyfileobj(f_in, f_out)
PY
        return 0
    fi

    echo "[WARN] xz/python3 not available; skipping ISO compression" >&2
    return 0
}

mkdir -p "${IMAGE_DIR}"
rm -rf "${STAGING_DIR}"
mkdir -p "${EFI_DIR}"

if [ -d "${BUILD_DIR}/assets" ]; then
    mkdir -p "${STAGING_DIR}/assets"
    cp -R "${BUILD_DIR}/assets"/. "${STAGING_DIR}/assets/"
fi

if [ ! -f "${KERNEL_ELF}" ]; then
    echo "[ERROR] ARM64 kernel not found: ${KERNEL_ELF}"
    exit 1
fi

if command -v aarch64-linux-gnu-objcopy >/dev/null 2>&1; then
    OBJCOPY="aarch64-linux-gnu-objcopy"
elif command -v objcopy >/dev/null 2>&1; then
    OBJCOPY="objcopy"
else
    echo "[ERROR] no suitable objcopy found"
    exit 1
fi

if ! command -v xorriso >/dev/null 2>&1; then
    echo "[ERROR] xorriso not found"
    exit 1
fi

echo "[IMAGE] Staging ARM64 UEFI ISO tree..."
cp "${KERNEL_ELF}" "${STAGING_DIR}/${KERNEL_NAME}"

echo "[IMAGE] Converting kernel ELF to BOOTAA64.EFI using ${OBJCOPY}..."
"${OBJCOPY}" \
    -O pei-aarch64-little \
    "${KERNEL_ELF}" \
    "${BOOTAA64_EFI}"

echo "[IMAGE] Creating ARM64 ISO: ${ISO_PATH}"
xorriso -as mkisofs \
    -R -r -J \
    -V "${ISO_LABEL}" \
    -o "${ISO_PATH}" \
    -eltorito-alt-boot \
    -e EFI/BOOT/BOOTAA64.EFI \
    -no-emul-boot \
    "${STAGING_DIR}"

compress_iso "${ISO_PATH}"

echo "[IMAGE] ARM64 ISO created: ${ISO_PATH}"
ls -lh "${ISO_PATH}"
if [ -f "${ISO_PATH}.xz" ]; then
    ls -lh "${ISO_PATH}.xz"
fi
