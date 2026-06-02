#!/bin/bash
# Create a hybrid x86_64 ISO that boots via both BIOS and UEFI.
# The resulting ISO can be attached to VMs directly or written to USB media.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-build/x86_64}"
IMAGE_DIR="${2:-image}"
ISO_NAME="${ISO_NAME:-os8-x86_64.iso}"
ISO_ROOT="${BUILD_DIR}/iso_root"
KERNEL_PATH="${BUILD_DIR}/kernel/os-x86_64.elf"
BOOT_MANAGER_DIR="${BUILD_DIR}/boot-assets/os-boot-manager"
BOOT_MANAGER_SYNC="${ROOT_DIR}/scripts/update-os-boot-manager.sh"
X86_64_BOOT_ASSET_DIR="${ROOT_DIR}/os-x86_64"
BOOT_FILES_SCRIPT="${ROOT_DIR}/scripts/build-install-boot-files.sh"
SYSTEM_IMAGE_SCRIPT="${ROOT_DIR}/scripts/create-system-image.sh"
BOOT_MANAGER_DIR="$("$BOOT_MANAGER_SYNC" "$BOOT_MANAGER_DIR")"
LIMINE_BIN_DIR="${BOOT_MANAGER_DIR}/bin"
LIMINE_SRC_DIR="${BOOT_MANAGER_DIR}"
LIMINE_TOOL_PATH="${LIMINE_SRC_DIR}/limine"
LIMINE_CFG="${LIMINE_CFG:-${X86_64_BOOT_ASSET_DIR}/limine-installer-text.conf}"
INSTALL_LIMINE_CFG="${INSTALL_LIMINE_CFG:-${X86_64_BOOT_ASSET_DIR}/limine-installed.conf}"
INSTALL_ROOT="${ISO_ROOT}/install/system-image"
SYSTEM_IMAGE_ROOT="${SYSTEM_IMAGE_ROOT:-${BUILD_DIR}/system-image}"
SYSTEM_IMAGE_ARCHIVE="${SYSTEM_IMAGE_ARCHIVE:-${BUILD_DIR}/system-image.zip}"
BOOT_IMAGE_ARCHIVE="${BOOT_IMAGE_ARCHIVE:-${BUILD_DIR}/boot-files.zip}"
SYSTEM_DISK_IMAGE="${SYSTEM_DISK_IMAGE:-${IMAGE_DIR}/os8-x86_64-system.img}"
DOS_INSTALLER_DIR="${DOS_INSTALLER_DIR:-${BUILD_DIR}/dos-installer}"
DOS_ENV_IMAGE="${DOS_ENV_IMAGE:-${IMAGE_DIR}/os8-x86_64-dos-env.img}"

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

ensure_executable() {
    local path="$1"
    if [ ! -x "$path" ]; then
        chmod +x "$path" 2>/dev/null || true
    fi
    if [ ! -x "$path" ]; then
        echo "[ERROR] Required executable is not runnable: $path" >&2
        exit 1
    fi
}

tool_runs() {
    local path="$1"
    if [ ! -x "$path" ]; then
        return 1
    fi
    if "$path" --help >/dev/null 2>&1; then
        return 0
    fi
    case $? in
        126|127)
            return 1
            ;;
        *)
            return 0
            ;;
    esac
}

resolve_limine_tool() {
    local host_tool="${LIMINE_SRC_DIR}/limine-host"

    if tool_runs "$LIMINE_TOOL_PATH"; then
        printf '%s\n' "$LIMINE_TOOL_PATH"
        return 0
    fi

    require_file "${LIMINE_SRC_DIR}/limine.c"
    require_cmd cc

    if ! tool_runs "$host_tool"; then
        log "Bundled Limine host tool is not runnable on this platform; building a native copy" >&2
        cc -g -O2 -pipe -Wall -Wextra -std=c99 "${LIMINE_SRC_DIR}/limine.c" -o "$host_tool"
        ensure_executable "$host_tool"
    fi

    if ! tool_runs "$host_tool"; then
        echo "[ERROR] Failed to build a runnable Limine host tool: $host_tool" >&2
        exit 1
    fi

    printf '%s\n' "$host_tool"
}

require_file "$KERNEL_PATH"
require_file "$LIMINE_CFG"
require_file "$INSTALL_LIMINE_CFG"
require_file "$LIMINE_BIN_DIR/BOOTX64.EFI"
require_file "$LIMINE_BIN_DIR/limine-bios.sys"
require_file "$LIMINE_BIN_DIR/limine-bios-cd.bin"
require_file "$LIMINE_BIN_DIR/limine-uefi-cd.bin"
require_cmd xorriso

mkdir -p "$IMAGE_DIR"
rm -rf "$ISO_ROOT"

log "Preparing ISO root at $ISO_ROOT"
mkdir -p "$ISO_ROOT/install"

env BOOT_PROFILE=installer LIMINE_CFG_SOURCE="$LIMINE_CFG" \
    bash "$BOOT_FILES_SCRIPT" "$BUILD_DIR" "$ISO_ROOT"
env BOOT_LIMINE_CFG="$INSTALL_LIMINE_CFG" \
    bash "$SYSTEM_IMAGE_SCRIPT" "$BUILD_DIR" "$SYSTEM_IMAGE_ROOT"
rm -rf "$INSTALL_ROOT"
cp -R "$SYSTEM_IMAGE_ROOT" "$INSTALL_ROOT"
cp "$SYSTEM_IMAGE_ARCHIVE" "$ISO_ROOT/install/system-image.zip"
if [ -f "$BOOT_IMAGE_ARCHIVE" ]; then
    cp "$BOOT_IMAGE_ARCHIVE" "$ISO_ROOT/install/boot-files.zip"
fi
if [ -f "$SYSTEM_DISK_IMAGE" ]; then
    cp "$SYSTEM_DISK_IMAGE" "$ISO_ROOT/install/system.img"
fi
if [ -f "${DOS_INSTALLER_DIR}/OSINST.COM" ]; then
    cp "${DOS_INSTALLER_DIR}/OSINST.COM" "$ISO_ROOT/OSINST.COM"
fi
if [ -f "${DOS_INSTALLER_DIR}/OSSYS.IMG" ]; then
    cp "${DOS_INSTALLER_DIR}/OSSYS.IMG" "$ISO_ROOT/OSSYS.IMG"
fi
if [ -f "${DOS_INSTALLER_DIR}/README.TXT" ]; then
    cp "${DOS_INSTALLER_DIR}/README.TXT" "$ISO_ROOT/README.TXT"
fi
if [ -f "${DOS_ENV_IMAGE}" ]; then
    cp "${DOS_ENV_IMAGE}" "$ISO_ROOT/DOSENV.IMG"
fi
LIMINE_TOOL="$(resolve_limine_tool)"

if [ -d "${BUILD_DIR}/assets" ]; then
    mkdir -p "$ISO_ROOT/assets"
    cp -R "${BUILD_DIR}/assets"/. "$ISO_ROOT/assets/"
fi

ISO_PATH="${IMAGE_DIR}/${ISO_NAME}"
rm -f "$ISO_PATH"

log "Creating hybrid BIOS+UEFI ISO: $ISO_PATH"
xorriso -as mkisofs \
    -b boot/limine-bios-cd.bin \
    -no-emul-boot \
    -boot-load-size 4 \
    -boot-info-table \
    --efi-boot boot/limine-uefi-cd.bin \
    -efi-boot-part \
    --efi-boot-image \
    --protective-msdos-label \
    "$ISO_ROOT" \
    -o "$ISO_PATH"

"$LIMINE_TOOL" bios-install "$ISO_PATH" >/dev/null 2>&1 || true
compress_iso "$ISO_PATH"
compress_installer_7z "$ISO_PATH"

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

require_iso_path "/boot/main.sys"
require_iso_path "/boot/limine-bios-cd.bin"
require_iso_path "/boot/limine-uefi-cd.bin"
require_iso_path "/boot/limine-bios.sys"
require_iso_path "/EFI/BOOT/BOOTX64.EFI"
require_iso_path "/limine.conf"
require_iso_path "/boot/limine.conf"
require_iso_path "/EFI/BOOT/limine.conf"
require_iso_path "/INSTALLERS.TXT"
require_iso_path "/install/system-image.zip"
if [ -f "$BOOT_IMAGE_ARCHIVE" ]; then
    require_iso_path "/install/boot-files.zip"
fi
if [ -f "$SYSTEM_DISK_IMAGE" ]; then
    require_iso_path "/install/system.img"
fi
if [ -f "${DOS_INSTALLER_DIR}/OSINST.COM" ]; then
    require_iso_path "/OSINST.COM"
fi
if [ -f "${DOS_INSTALLER_DIR}/OSSYS.IMG" ]; then
    require_iso_path "/OSSYS.IMG"
fi
if [ -f "${DOS_INSTALLER_DIR}/README.TXT" ]; then
    require_iso_path "/README.TXT"
fi
if [ -f "${DOS_ENV_IMAGE}" ]; then
    require_iso_path "/DOSENV.IMG"
fi
require_iso_path "/install/system-image/INSTALLERS.TXT"
require_iso_path "/install/system-image/boot/main.sys"
require_iso_path "/install/system-image/boot/bootloader.sys"
require_iso_path "/install/system-image/limine.conf"
require_iso_path "/install/system-image/boot/limine.conf"
require_iso_path "/install/system-image/limine/limine.conf"
require_iso_path "/install/system-image/EFI/BOOT/limine.conf"
require_iso_path "/install/system-image/boot/limine-bios.sys"
require_iso_path "/install/system-image/boot/limine-bios-cd.bin"
require_iso_path "/install/system-image/boot/limine-uefi-cd.bin"
require_iso_path "/install/system-image/EFI/BOOT/BOOTX64.EFI"
require_iso_path "/install/system-image/IMAGE_INFO.txt"

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
