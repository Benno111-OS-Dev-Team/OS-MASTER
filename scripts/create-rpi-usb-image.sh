#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/arm64"
IMAGE_DIR="${ROOT_DIR}/image"
FIRMWARE_DIR="${ROOT_DIR}/build/boot-assets/rpi4-uefi"
IMAGE_NAME="os8-rpi-usb.img"
IMAGE_SIZE_MB=512
USB_DEVICE=""
KEEP_STAGING=0
AUTO_DOWNLOAD_FIRMWARE=1
FIRMWARE_RELEASE_API="https://api.github.com/repos/pftf/RPi4/releases/latest"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() {
    echo -e "${GREEN}[RPI-USB]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[RPI-USB]${NC} $1" >&2
}

usage() {
    cat <<EOF
Usage:
  $(basename "$0") [options]

Build a Raspberry Pi USB boot image for OS8 using Raspberry Pi UEFI firmware.

Optional:
  --build-dir <dir>      ARM64 build output directory
                         Default: ${BUILD_DIR}
  --firmware-dir <dir>   Directory containing Raspberry Pi boot firmware files
                         and RPI_EFI.fd
                         Default: ${FIRMWARE_DIR}
  --no-download-firmware Do not auto-download firmware if the cache is missing
  --image-dir <dir>      Output directory for the generated image
                         Default: ${IMAGE_DIR}
  --image-name <name>    Output image filename
                         Default: ${IMAGE_NAME}
  --image-size-mb <mb>   FAT image size in MiB
                         Default: ${IMAGE_SIZE_MB}
  --device <path>        Optional block device to flash after image creation
                         Example: /dev/sdX
  --keep-staging         Keep the temporary staging tree under the image dir
  --help                 Show this help

Examples:
  $(basename "$0")
  $(basename "$0") --device /dev/sdX
  $(basename "$0") --firmware-dir ~/rpi-uefi

Notes:
  - This targets Raspberry Pi 4/400/CM4-style UEFI boot flows.
  - The ARM64 kernel must already exist at build/arm64/kernel/os-arm64.elf.
  - If --device is provided, the script will overwrite it with dd.
  - By default, firmware is downloaded from the pftf/RPi4 GitHub releases API
    into a repo-local cache if it is not already present.
EOF
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "[ERROR] Required command not found: $1" >&2
        exit 1
    fi
}

extract_zip() {
    local archive="$1"
    local dest="$2"

    if command -v unzip >/dev/null 2>&1; then
        unzip -q "$archive" -d "$dest"
        return
    fi

    if command -v python3 >/dev/null 2>&1; then
        python3 - "$archive" "$dest" <<'PY'
import pathlib
import sys
import zipfile

archive = pathlib.Path(sys.argv[1])
dest = pathlib.Path(sys.argv[2])
with zipfile.ZipFile(archive) as zf:
    zf.extractall(dest)
PY
        return
    fi

    echo "[ERROR] Need either unzip or python3 to extract firmware archive" >&2
    exit 1
}

download_firmware_if_needed() {
    if [ -f "${FIRMWARE_DIR}/RPI_EFI.fd" ] &&
       compgen -G "${FIRMWARE_DIR}/start*.elf" >/dev/null &&
       compgen -G "${FIRMWARE_DIR}/fixup*.dat" >/dev/null; then
        log "Using cached Raspberry Pi firmware in ${FIRMWARE_DIR}"
        return
    fi

    if [ "${AUTO_DOWNLOAD_FIRMWARE}" -ne 1 ]; then
        echo "[ERROR] Firmware cache missing and auto-download is disabled" >&2
        exit 1
    fi

    require_cmd curl
    if command -v python3 >/dev/null 2>&1; then
        PYTHON_BIN="python3"
    elif command -v python >/dev/null 2>&1; then
        PYTHON_BIN="python"
    else
        echo "[ERROR] Need python3 or python to parse GitHub release metadata" >&2
        exit 1
    fi

    local tmp_dir json_path archive_url archive_name archive_path extract_dir
    tmp_dir="$(mktemp -d)"
    trap 'rm -rf "${tmp_dir}"' RETURN
    json_path="${tmp_dir}/release.json"

    log "Downloading latest Raspberry Pi UEFI firmware metadata"
    curl -fsSL "${FIRMWARE_RELEASE_API}" -o "${json_path}"

    archive_url="$("${PYTHON_BIN}" - "${json_path}" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fh:
    data = json.load(fh)

for asset in data.get("assets", []):
    name = asset.get("name", "")
    if name.lower().endswith(".zip"):
        print(asset.get("browser_download_url", ""))
        break
PY
)"

    if [ -z "${archive_url}" ]; then
        echo "[ERROR] Could not find a firmware zip asset in latest pftf/RPi4 release" >&2
        exit 1
    fi

    archive_name="$(basename "${archive_url}")"
    archive_path="${tmp_dir}/${archive_name}"
    extract_dir="${tmp_dir}/extract"

    log "Downloading Raspberry Pi UEFI firmware archive ${archive_name}"
    curl -fL --retry 3 --retry-delay 2 "${archive_url}" -o "${archive_path}"

    mkdir -p "${extract_dir}"
    extract_zip "${archive_path}" "${extract_dir}"

    rm -rf "${FIRMWARE_DIR}"
    mkdir -p "${FIRMWARE_DIR}"
    cp -R "${extract_dir}"/. "${FIRMWARE_DIR}/"

    if [ ! -f "${FIRMWARE_DIR}/RPI_EFI.fd" ]; then
        echo "[ERROR] Downloaded firmware archive did not contain RPI_EFI.fd" >&2
        exit 1
    fi

    log "Cached Raspberry Pi firmware in ${FIRMWARE_DIR}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --firmware-dir)
            FIRMWARE_DIR="${2:?missing value for --firmware-dir}"
            shift 2
            ;;
        --no-download-firmware)
            AUTO_DOWNLOAD_FIRMWARE=0
            shift
            ;;
        --build-dir)
            BUILD_DIR="${2:?missing value for --build-dir}"
            shift 2
            ;;
        --image-dir)
            IMAGE_DIR="${2:?missing value for --image-dir}"
            shift 2
            ;;
        --image-name)
            IMAGE_NAME="${2:?missing value for --image-name}"
            shift 2
            ;;
        --image-size-mb)
            IMAGE_SIZE_MB="${2:?missing value for --image-size-mb}"
            shift 2
            ;;
        --device)
            USB_DEVICE="${2:?missing value for --device}"
            shift 2
            ;;
        --keep-staging)
            KEEP_STAGING=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "[ERROR] Unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
done

case "${IMAGE_SIZE_MB}" in
    ''|*[!0-9]*)
        echo "[ERROR] --image-size-mb must be a positive integer" >&2
        exit 1
        ;;
esac

if [ "${IMAGE_SIZE_MB}" -lt 128 ]; then
    echo "[ERROR] --image-size-mb must be at least 128" >&2
    exit 1
fi

KERNEL_ELF="${BUILD_DIR}/kernel/os-arm64.elf"
IMAGE_PATH="${IMAGE_DIR}/${IMAGE_NAME}"
STAGING_DIR="${IMAGE_DIR}/rpi-usb-staging"
EFI_DIR="${STAGING_DIR}/EFI/BOOT"
PART_START_SECTORS=2048
TOTAL_SECTORS=$((IMAGE_SIZE_MB * 1024 * 1024 / 512))
PART_SIZE_SECTORS=$((TOTAL_SECTORS - PART_START_SECTORS))
FAT_OFFSET_BYTES=$((PART_START_SECTORS * 512))
MTOOLS_IMAGE="${IMAGE_PATH}@@${FAT_OFFSET_BYTES}"

require_cmd sfdisk
require_cmd mkfs.fat
require_cmd mcopy
require_cmd mmd
require_cmd truncate
require_cmd dd
require_cmd sync

if command -v aarch64-linux-gnu-objcopy >/dev/null 2>&1; then
    OBJCOPY="aarch64-linux-gnu-objcopy"
elif command -v llvm-objcopy >/dev/null 2>&1; then
    OBJCOPY="llvm-objcopy"
elif command -v objcopy >/dev/null 2>&1; then
    OBJCOPY="objcopy"
else
    echo "[ERROR] No suitable objcopy found" >&2
    exit 1
fi

if [ ! -f "${KERNEL_ELF}" ]; then
    echo "[ERROR] ARM64 kernel not found: ${KERNEL_ELF}" >&2
    echo "[ERROR] Build it first with: make -f Makefile.multiarch ARCH=arm64 kernel" >&2
    exit 1
fi

if [ ! -d "${FIRMWARE_DIR}" ]; then
    mkdir -p "${FIRMWARE_DIR}"
fi

download_firmware_if_needed

if [ ! -f "${FIRMWARE_DIR}/RPI_EFI.fd" ]; then
    echo "[ERROR] ${FIRMWARE_DIR}/RPI_EFI.fd is missing" >&2
    echo "[ERROR] This script expects Raspberry Pi UEFI firmware files." >&2
    exit 1
fi

if ! compgen -G "${FIRMWARE_DIR}/start*.elf" >/dev/null; then
    echo "[ERROR] No start*.elf files found in ${FIRMWARE_DIR}" >&2
    exit 1
fi

if ! compgen -G "${FIRMWARE_DIR}/fixup*.dat" >/dev/null; then
    echo "[ERROR] No fixup*.dat files found in ${FIRMWARE_DIR}" >&2
    exit 1
fi

mkdir -p "${IMAGE_DIR}"
rm -rf "${STAGING_DIR}"
mkdir -p "${EFI_DIR}"

cleanup() {
    if [ "${KEEP_STAGING}" -eq 0 ]; then
        rm -rf "${STAGING_DIR}"
    fi
}
trap cleanup EXIT

log "Staging Raspberry Pi firmware"

shopt -s nullglob
for pattern in \
    "bootcode.bin" \
    "fixup*.dat" \
    "start*.elf" \
    "*.dtb" \
    "RPI_EFI.fd"; do
    for path in "${FIRMWARE_DIR}"/${pattern}; do
        [ -e "${path}" ] || continue
        cp "${path}" "${STAGING_DIR}/"
    done
done
shopt -u nullglob

if [ -d "${FIRMWARE_DIR}/overlays" ]; then
    cp -R "${FIRMWARE_DIR}/overlays" "${STAGING_DIR}/overlays"
fi

log "Converting OS8 ARM64 kernel to BOOTAA64.EFI"
"${OBJCOPY}" -O pei-aarch64-little "${KERNEL_ELF}" "${EFI_DIR}/BOOTAA64.EFI"
cp "${KERNEL_ELF}" "${STAGING_DIR}/os-arm64.elf"

cat > "${STAGING_DIR}/config.txt" <<'EOF'
arm_64bit=1
enable_uart=1
enable_gic=1
uart_2ndstage=1
kernel=RPI_EFI.fd
EOF

cat > "${STAGING_DIR}/startup.nsh" <<'EOF'
\EFI\BOOT\BOOTAA64.EFI
EOF

cat > "${STAGING_DIR}/README-OS8-RPI.txt" <<'EOF'
OS8 Raspberry Pi boot media

This image boots through Raspberry Pi UEFI firmware (RPI_EFI.fd), then
hands off to EFI/BOOT/BOOTAA64.EFI, which is the OS8 ARM64 kernel.
EOF

log "Creating FAT32 USB image ${IMAGE_PATH}"
rm -f "${IMAGE_PATH}"
truncate -s "${IMAGE_SIZE_MB}M" "${IMAGE_PATH}"

printf 'label: dos\nlabel-id: 0x4f533852\nunit: sectors\n\n%s,%s,0x0c,*\n' \
    "${PART_START_SECTORS}" "${PART_SIZE_SECTORS}" | sfdisk "${IMAGE_PATH}" >/dev/null

mkfs.fat -F 32 --offset "${PART_START_SECTORS}" -n OS8RPI "${IMAGE_PATH}" >/dev/null

log "Seeding firmware and OS8 boot files"
mmd -i "${MTOOLS_IMAGE}" ::/EFI
mmd -i "${MTOOLS_IMAGE}" ::/EFI/BOOT

if [ -d "${STAGING_DIR}/overlays" ]; then
    mmd -i "${MTOOLS_IMAGE}" ::/overlays
fi

mcopy -i "${MTOOLS_IMAGE}" -s "${STAGING_DIR}"/* ::

log "Raspberry Pi USB image created: ${IMAGE_PATH}"
ls -lh "${IMAGE_PATH}"

if [ -n "${USB_DEVICE}" ]; then
    if [ ! -b "${USB_DEVICE}" ]; then
        echo "[ERROR] --device must point to a block device: ${USB_DEVICE}" >&2
        exit 1
    fi

    warn "About to overwrite ${USB_DEVICE}"
    log "Writing image to ${USB_DEVICE}"
    dd if="${IMAGE_PATH}" of="${USB_DEVICE}" bs=4M conv=fsync status=progress
    sync
    log "USB device written successfully"
fi

echo ""
log "Next step: plug the USB device into a Raspberry Pi with USB boot enabled."
