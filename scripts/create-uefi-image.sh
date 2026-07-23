#!/bin/bash
# Create a BIOS+UEFI bootable MBR disk image for x86_64 without loop devices.

set -euo pipefail

BUILD_DIR="${1:-build/x86_64}"
IMAGE_DIR="${2:-image}"
IMAGE_NAME="${IMAGE_NAME:-os-x86_64.img}"
IMAGE_SIZE_MB="${IMAGE_SIZE_MB:-100}"
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
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
SYSTEM_IMAGE_ROOT="${SYSTEM_IMAGE_ROOT:-${BUILD_DIR}/system-image}"
SYSTEM_IMAGE_ARCHIVE="${SYSTEM_IMAGE_ARCHIVE:-${BUILD_DIR}/system-image.zip}"
BOOT_IMAGE_PATH="${BOOT_IMAGE_PATH:-${BUILD_DIR}/boot-files.img}"
BOOT_PROFILE="${BOOT_PROFILE:-installed-system}"
LIMINE_CFG_SOURCE="${LIMINE_CFG_SOURCE:-${ROOT_DIR}/os-x86_64/limine-installed.conf}"
INCLUDE_INSTALL_PAYLOAD="${INCLUDE_INSTALL_PAYLOAD:-}"
INCLUDE_DOS_ENV="${INCLUDE_DOS_ENV:-0}"
DOS_INSTALLER_DIR="${DOS_INSTALLER_DIR:-${BUILD_DIR}/dos-installer}"

GREEN='\033[0;32m'
NC='\033[0m'

log() {
    echo -e "${GREEN}[UEFI-IMAGE]${NC} $1"
}

ceil_div() {
    local value="$1"
    local divisor="$2"
    echo $(((value + divisor - 1) / divisor))
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

mkdir -p "$IMAGE_DIR"

IMAGE_PATH="$IMAGE_DIR/$IMAGE_NAME"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

require_file "$KERNEL_PATH"
require_file "$LIMINE_BIN_DIR/limine-bios.sys"
require_file "$LIMINE_BIN_DIR/limine-bios-cd.bin"
require_file "$LIMINE_CFG_SOURCE"
require_cmd mkfs.fat
require_cmd mmd
require_cmd mcopy
require_cmd sfdisk

if [ -z "$INCLUDE_INSTALL_PAYLOAD" ]; then
    if [ "$BOOT_PROFILE" = "installer" ]; then
        INCLUDE_INSTALL_PAYLOAD=1
    else
        INCLUDE_INSTALL_PAYLOAD=0
    fi
fi

seed_dos_environment() {
    local root="$1"
    local startup_path="${root}/startup.nsh"
    local notes_path="${root}/DOSENV.TXT"

    if [ -f "${DOS_INSTALLER_DIR}/OSINST.COM" ]; then
        cp "${DOS_INSTALLER_DIR}/OSINST.COM" "${root}/OSINST.COM"
    fi
    if [ -f "${DOS_INSTALLER_DIR}/OSSYS.IMG" ]; then
        cp "${DOS_INSTALLER_DIR}/OSSYS.IMG" "${root}/OSSYS.IMG"
    fi
    if [ -f "${DOS_INSTALLER_DIR}/README.TXT" ]; then
        cp "${DOS_INSTALLER_DIR}/README.TXT" "${root}/README.TXT"
    fi

    cat > "${startup_path}" <<'EOF'
echo -off
cls
echo OS8 DOS Environment
echo ===================
echo This hybrid BIOS+UEFI image boots directly into OS8 text setup.
echo DOS payload files are available at:
echo   \OSINST.COM
echo   \OSSYS.IMG
echo   \README.TXT
echo.
echo If you are in the UEFI shell, run:
echo   fs0:\EFI\BOOT\BOOTX64.EFI
echo.
EOF

    cat > "${notes_path}" <<'EOF'
OS8 DOS Environment

This image is compatible with:
- BIOS / MBR boot through Limine BIOS stages
- UEFI boot through the OS8 custom loader at /EFI/BOOT/BOOTX64.EFI

Included DOS payload:
- /OSINST.COM
- /OSSYS.IMG
- /README.TXT

Usage:
1. Boot this disk image directly on BIOS or UEFI hardware.
2. OSINST.COM and OSSYS.IMG stay together in the root directory.
3. Or copy OSINST.COM and OSSYS.IMG onto an existing DOS system and run OSINST.COM.
4. In a UEFI shell, run fs0:\EFI\BOOT\BOOTX64.EFI to start text setup manually.
EOF
}

STAGING_ROOT="${TMP_DIR}/staging-root"
rm -rf "$STAGING_ROOT"
mkdir -p "$STAGING_ROOT"
if [ "$INCLUDE_INSTALL_PAYLOAD" = "1" ]; then
    mkdir -p "$STAGING_ROOT/install"
fi
env BOOT_PROFILE="$BOOT_PROFILE" LIMINE_CFG_SOURCE="$LIMINE_CFG_SOURCE" \
    bash "$BOOT_FILES_SCRIPT" "$BUILD_DIR" "$STAGING_ROOT"
if [ "$INCLUDE_INSTALL_PAYLOAD" = "1" ]; then
    env BOOT_LIMINE_CFG="${ROOT_DIR}/os-x86_64/limine-installed.conf" \
        bash "$SYSTEM_IMAGE_SCRIPT" "$BUILD_DIR" "$SYSTEM_IMAGE_ROOT"
    cp "$SYSTEM_IMAGE_ARCHIVE" "$STAGING_ROOT/install/system-image.zip"
    if [ -f "$BOOT_IMAGE_PATH" ]; then
        cp "$BOOT_IMAGE_PATH" "$STAGING_ROOT/install/boot-files.img"
    fi
fi
if [ "$INCLUDE_DOS_ENV" = "1" ]; then
    seed_dos_environment "$STAGING_ROOT"
fi

STAGING_KIB=$(du -sk "$STAGING_ROOT" | awk '{print $1}')
PAYLOAD_MB=$(ceil_div "$STAGING_KIB" 1024)
REQUIRED_IMAGE_MB=$((PAYLOAD_MB + 64))
if [ "$REQUIRED_IMAGE_MB" -lt 100 ]; then
    REQUIRED_IMAGE_MB=100
fi
if [ "$IMAGE_SIZE_MB" -lt "$REQUIRED_IMAGE_MB" ]; then
    log "Growing image from ${IMAGE_SIZE_MB}M to ${REQUIRED_IMAGE_MB}M to fit staged payload"
    IMAGE_SIZE_MB="$REQUIRED_IMAGE_MB"
fi

log "Creating UEFI disk image: $IMAGE_PATH (${IMAGE_SIZE_MB}M, profile=${BOOT_PROFILE})"
dd if=/dev/zero of="$IMAGE_PATH" bs=1M count="$IMAGE_SIZE_MB" status=none

PART_START_SECTORS=2048
TOTAL_SECTORS=$((IMAGE_SIZE_MB * 1024 * 1024 / 512))
PART_SIZE_SECTORS=$((TOTAL_SECTORS - PART_START_SECTORS))
FAT_OFFSET_BYTES=$((PART_START_SECTORS * 512))
MTOOLS_IMAGE="${IMAGE_PATH}@@${FAT_OFFSET_BYTES}"

printf 'label: dos\nlabel-id: 0x4f534e58\nunit: sectors\n\n%s,%s,0x0c,*\n' \
    "$PART_START_SECTORS" "$PART_SIZE_SECTORS" | sfdisk "$IMAGE_PATH" >/dev/null

mkfs.fat -F 32 --offset "$PART_START_SECTORS" -n OSNEXT64 "$IMAGE_PATH" >/dev/null

LIMINE_TOOL="$(resolve_limine_tool)"

log "Seeding UEFI boot files into FAT image"
mmd -i "$MTOOLS_IMAGE" ::/EFI
mmd -i "$MTOOLS_IMAGE" ::/EFI/BOOT
mmd -i "$MTOOLS_IMAGE" ::/EFI/OS8
mmd -i "$MTOOLS_IMAGE" ::/boot
mmd -i "$MTOOLS_IMAGE" ::/limine
mmd -i "$MTOOLS_IMAGE" ::/System
mmd -i "$MTOOLS_IMAGE" ::/install

mcopy -i "$MTOOLS_IMAGE" -s "$STAGING_ROOT"/* ::

"$LIMINE_TOOL" bios-install "$IMAGE_PATH" || {
    echo "[ERROR] Failed to install Limine BIOS stages into $IMAGE_PATH" >&2
    exit 1
}

log "BIOS+UEFI boot image created successfully: $IMAGE_PATH"
ls -lh "$IMAGE_PATH"
echo ""
log "To write to USB: sudo dd if=$IMAGE_PATH of=/dev/sdX bs=4M status=progress"
