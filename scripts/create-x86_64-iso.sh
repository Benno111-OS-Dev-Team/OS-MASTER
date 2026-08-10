#!/bin/bash
# Create a hybrid x86_64 ISO that boots via both BIOS and UEFI.
# The resulting ISO can be attached to VMs directly or written to USB media.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-build/x86_64}"
IMAGE_DIR="${2:-image}"
ISO_NAME="${ISO_NAME:-os8-x86_64.iso}"
ISO_ROOT="${BUILD_DIR}/iso_root"
KERNEL_PATH="${KERNEL_PATH:-${BUILD_DIR}/kernel/os-x86_64.elf}"
BOOT_MANAGER_DIR="${BUILD_DIR}/boot-assets/os-boot-manager"
BOOT_MANAGER_SYNC="${ROOT_DIR}/scripts/update-os-boot-manager.sh"
X86_64_BOOT_ASSET_DIR="${ROOT_DIR}/os-x86_64"
BOOT_FILES_SCRIPT="${ROOT_DIR}/scripts/build-install-boot-files.sh"
SYSTEM_IMAGE_SCRIPT="${ROOT_DIR}/scripts/create-system-image.sh"
BOOT_MANAGER_DIR="$("$BOOT_MANAGER_SYNC" "$BOOT_MANAGER_DIR")"
LIMINE_BIN_DIR="${BOOT_MANAGER_DIR}/bin"
LIMINE_CFG="${LIMINE_CFG:-${X86_64_BOOT_ASSET_DIR}/limine.conf}"
INSTALL_LIMINE_CFG="${INSTALL_LIMINE_CFG:-${X86_64_BOOT_ASSET_DIR}/limine-installed.conf}"
INCLUDE_INSTALLER="${INCLUDE_INSTALLER:-0}"
BOOT_PROFILE="${BOOT_PROFILE:-live}"
INSTALL_ROOT="${ISO_ROOT}/install/system-image"
SYSTEM_IMAGE_ROOT="${SYSTEM_IMAGE_ROOT:-${BUILD_DIR}/system-image}"
SYSTEM_IMAGE_ARCHIVE="${SYSTEM_IMAGE_ARCHIVE:-${BUILD_DIR}/system-image.zip}"
BOOT_IMAGE_PATH="${BOOT_IMAGE_PATH:-${BUILD_DIR}/boot-files.img}"
SYSTEM_DISK_IMAGE="${SYSTEM_DISK_IMAGE:-${IMAGE_DIR}/os8-x86_64-system.img}"

GREEN='\033[0;32m'
NC='\033[0m'

log() {
    echo -e "${GREEN}[X86_64-ISO]${NC} $1"
}

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

compress_installer_7z() {
    local iso_path="$1"
    local archive_path="${iso_path}.7z"

    case "$(basename "$iso_path")" in
        *installer*.iso) ;;
        *) return 0 ;;
    esac

    rm -f "$archive_path"

    if ! command -v 7z >/dev/null 2>&1; then
        echo "[WARN] 7z not available; skipping installer 7z archive" >&2
        return 0
    fi

    log "Creating maximum-compression installer archive: $archive_path"
    7z a \
        -t7z \
        -mx=9 \
        -m0=lzma2 \
        -mfb=273 \
        -md=512m \
        -ms=on \
        "$archive_path" \
        "$iso_path"
}

link_or_copy() {
    local src="$1"
    local dst="$2"
    rm -f "$dst"
    if ! ln "$src" "$dst" 2>/dev/null; then
        cp "$src" "$dst"
    fi
}

require_file() {
    if [ ! -f "$1" ]; then
        echo "[ERROR] Required file not found: $1" >&2
        exit 1
    fi
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "[ERROR] Required command not found: $1" >&2
        exit 1
    fi
}

require_file "$KERNEL_PATH"
require_file "$LIMINE_CFG"
if [ "$INCLUDE_INSTALLER" = "1" ]; then
    require_file "$INSTALL_LIMINE_CFG"
fi
require_file "$LIMINE_BIN_DIR/limine-bios.sys"
require_file "$LIMINE_BIN_DIR/limine-bios-cd.bin"
require_cmd xorriso
require_cmd mformat
require_cmd mmd
require_cmd mcopy

mkdir -p "$IMAGE_DIR"
rm -rf "$ISO_ROOT"

log "Preparing ISO root at $ISO_ROOT"
mkdir -p "$ISO_ROOT/install"

env BOOT_PROFILE="$BOOT_PROFILE" LIMINE_CFG_SOURCE="$LIMINE_CFG" \
    bash "$BOOT_FILES_SCRIPT" "$BUILD_DIR" "$ISO_ROOT"
if [ "$INCLUDE_INSTALLER" = "1" ]; then
    env BOOT_LIMINE_CFG="$INSTALL_LIMINE_CFG" \
        bash "$SYSTEM_IMAGE_SCRIPT" "$BUILD_DIR" "$SYSTEM_IMAGE_ROOT"
    rm -rf "$INSTALL_ROOT"
    cp -R "$SYSTEM_IMAGE_ROOT" "$INSTALL_ROOT"
    cp "$SYSTEM_IMAGE_ARCHIVE" "$ISO_ROOT/install/system-image.zip"
    if [ -f "$BOOT_IMAGE_PATH" ]; then
        cp "$BOOT_IMAGE_PATH" "$ISO_ROOT/install/boot-files.img"
    fi
    if [ -f "$SYSTEM_DISK_IMAGE" ]; then
        cp "$SYSTEM_DISK_IMAGE" "$ISO_ROOT/install/system.img"
    fi
fi
if [ -d "${BUILD_DIR}/assets" ]; then
    mkdir -p "$ISO_ROOT/assets"
    cp -R "${BUILD_DIR}/assets"/. "$ISO_ROOT/assets/"
fi

EFI_BOOT_IMAGE="${ISO_ROOT}/EFI/efiboot.img"
log "Creating UEFI El Torito boot image: $EFI_BOOT_IMAGE"
dd if=/dev/zero of="$EFI_BOOT_IMAGE" bs=1M count=16 status=none
mformat -i "$EFI_BOOT_IMAGE" -v OS8EFI ::
mmd -i "$EFI_BOOT_IMAGE" ::/EFI
mmd -i "$EFI_BOOT_IMAGE" ::/EFI/BOOT
mmd -i "$EFI_BOOT_IMAGE" ::/EFI/OS8
mmd -i "$EFI_BOOT_IMAGE" ::/boot
mcopy -i "$EFI_BOOT_IMAGE" "$ISO_ROOT/EFI/BOOT/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$EFI_BOOT_IMAGE" "$ISO_ROOT/EFI/OS8/STARTUPX64.EFI" ::/EFI/OS8/STARTUPX64.EFI
mcopy -i "$EFI_BOOT_IMAGE" "$ISO_ROOT/EFI/OS8/os8boot.cfg" ::/EFI/OS8/os8boot.cfg
mcopy -i "$EFI_BOOT_IMAGE" "$ISO_ROOT/boot/main.sys" ::/boot/main.sys
mcopy -i "$EFI_BOOT_IMAGE" "$ISO_ROOT/boot/bootloader.sys" ::/boot/bootloader.sys

ISO_PATH="${IMAGE_DIR}/${ISO_NAME}"
rm -f "$ISO_PATH"

log "Creating hybrid BIOS+UEFI ISO: $ISO_PATH"
xorriso -as mkisofs \
    -J \
    -joliet-long \
    -r \
    -b boot/limine-bios-cd.bin \
    -no-emul-boot \
    -boot-load-size 4 \
    -boot-info-table \
    -eltorito-alt-boot \
    -e EFI/efiboot.img \
    -no-emul-boot \
    -efi-boot-part \
    --efi-boot-image \
    --protective-msdos-label \
    "$ISO_ROOT" \
    -o "$ISO_PATH"

compress_iso "$ISO_PATH"
if [ "$INCLUDE_INSTALLER" = "1" ]; then
    compress_installer_7z "$ISO_PATH"
fi

log "Validating ISO contents..."
ISO_CONTENTS_FILE="${ISO_ROOT}/iso-contents.txt"
xorriso -indev "$ISO_PATH" -find / -type f -exec lsdl > "$ISO_CONTENTS_FILE"

require_iso_path() {
    local iso_path="$1"
    local output
    output=$(xorriso -indev "$ISO_PATH" -find "$iso_path" -exec lsdl 2>/dev/null || true)
    if [ -z "$output" ]; then
        echo "[ERROR] Missing required ISO path: $iso_path" >&2
        exit 1
    fi
}

require_iso_path "/boot/bootloader.sys"
require_iso_path "/boot/main.sys"
require_iso_path "/boot/raw/manifest.txt"
require_iso_path "/boot/limine-bios-cd.bin"
require_iso_path "/boot/limine-bios.sys"
require_iso_path "/EFI/BOOT/BOOTX64.EFI"
require_iso_path "/EFI/OS8/STARTUPX64.EFI"
require_iso_path "/EFI/OS8/os8boot.cfg"
require_iso_path "/EFI/efiboot.img"
require_iso_path "/limine.conf"
require_iso_path "/boot/limine.conf"
if [ "$INCLUDE_INSTALLER" = "1" ]; then
    require_iso_path "/INSTALLERS.TXT"
    require_iso_path "/install/system-image.zip"
    if [ -f "$BOOT_IMAGE_PATH" ]; then
        require_iso_path "/install/boot-files.img"
    fi
    if [ -f "$SYSTEM_DISK_IMAGE" ]; then
        require_iso_path "/install/system.img"
    fi
    require_iso_path "/install/system-image/INSTALLERS.TXT"
    require_iso_path "/install/system-image/boot/bootloader.sys"
    require_iso_path "/install/system-image/boot/main.sys"
    require_iso_path "/install/system-image/boot/raw/manifest.txt"
    require_iso_path "/install/system-image/limine.conf"
    require_iso_path "/install/system-image/boot/limine.conf"
    require_iso_path "/install/system-image/limine/limine.conf"
    require_iso_path "/install/system-image/boot/limine-bios.sys"
    require_iso_path "/install/system-image/boot/limine-bios-cd.bin"
    require_iso_path "/install/system-image/EFI/BOOT/BOOTX64.EFI"
    require_iso_path "/install/system-image/EFI/OS8/STARTUPX64.EFI"
    require_iso_path "/install/system-image/EFI/OS8/os8boot.cfg"
    require_iso_path "/install/system-image/IMAGE_INFO.txt"
fi
log "ISO created successfully: $ISO_PATH"
ls -lh "$ISO_PATH"
if [ -f "${ISO_PATH}.xz" ]; then
    ls -lh "${ISO_PATH}.xz"
fi
if [ -f "${ISO_PATH}.7z" ]; then
    ls -lh "${ISO_PATH}.7z"
fi
echo ""
log "BIOS VM test:  qemu-system-x86_64 -M q35 -m 512M -cdrom $ISO_PATH -serial stdio"
log "UEFI VM test:  qemu-system-x86_64 -M q35 -m 512M -cdrom $ISO_PATH -bios /usr/share/OVMF/OVMF_CODE.fd -serial stdio"
log "Rufus: select $ISO_PATH and write it in ISO mode to a USB drive"
