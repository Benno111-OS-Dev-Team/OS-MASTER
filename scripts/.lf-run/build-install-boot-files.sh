#!/bin/bash
# Build the bootable OS install layout inside an existing target root.

set -euo pipefail

ROOT_DIR="/mnt/c/Users/Benno111/Documents/GitHub/OS-MASTER"
BUILD_DIR="${1:-build/x86_64}"
INSTALL_ROOT="${2:-${BUILD_DIR}/system-image}"
BOOT_IMAGE_ARCHIVE="${BOOT_IMAGE_ARCHIVE:-${BUILD_DIR}/boot-files.zip}"
LIMINE_CFG_SOURCE="${LIMINE_CFG_SOURCE:-${ROOT_DIR}/os-x86_64/limine.conf}"
BOOT_PROFILE="${BOOT_PROFILE:-installed-system}"
KERNEL_PATH="${BUILD_DIR}/kernel/os-x86_64.elf"
RAW_PARTS_DIR="${BUILD_DIR}/kernel/raw"
RAW_PARTS_SCRIPT="${ROOT_DIR}/scripts/.lf-run/export-kernel-raw-parts.sh"
BOOT_MANAGER_DIR="${BUILD_DIR}/boot-assets/os-boot-manager"
BOOT_MANAGER_SYNC="${ROOT_DIR}/scripts/.lf-run/update-os-boot-manager.sh"

BOOT_MANAGER_DIR="$("$BOOT_MANAGER_SYNC" "$BOOT_MANAGER_DIR")"
LIMINE_BIN_DIR="${BOOT_MANAGER_DIR}/bin"

GREEN='\033[0;32m'
NC='\033[0m'

INSTALLER_STATE_SOURCE=""
BOOTABLE_SOURCE=""
BIOS_BOOTABLE_SOURCE=""
FIRST_BOOT_SETUP=""
PYTHON_CMD=""

log() {
    echo -e "${GREEN}[BOOT-FILES]${NC} $1"
}

fail() {
    echo "[ERROR] $1" >&2
    exit 1
}

require_file() {
    if [ ! -f "$1" ]; then
        fail "Required file not found: $1"
    fi
}

resolve_python() {
    command -v python3 2>/dev/null || command -v python 2>/dev/null || true
}

write_zip_archive() {
    local root_dir="$1"
    local archive_path="$2"

    "$PYTHON_CMD" - "$root_dir" "$archive_path" <<'PY'
import pathlib
import sys
import zipfile

root = pathlib.Path(sys.argv[1]).resolve()
archive = pathlib.Path(sys.argv[2]).resolve()
archive.parent.mkdir(parents=True, exist_ok=True)
if archive.exists():
    archive.unlink()
with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED) as zf:
    for path in sorted(root.rglob("*")):
        if path.is_dir():
            continue
        zf.write(path, path.relative_to(root).as_posix())
PY
}

configure_profile() {
    case "$BOOT_PROFILE" in
        installed-system)
            INSTALLER_STATE_SOURCE="installed-system"
            BOOTABLE_SOURCE="installed-system"
            BIOS_BOOTABLE_SOURCE="installed-system"
            FIRST_BOOT_SETUP="1"
            ;;
        installer)
            INSTALLER_STATE_SOURCE="installer-iso"
            BOOTABLE_SOURCE="installer"
            BIOS_BOOTABLE_SOURCE="installer"
            FIRST_BOOT_SETUP="0"
            ;;
        live)
            INSTALLER_STATE_SOURCE="live-iso"
            BOOTABLE_SOURCE="live"
            BIOS_BOOTABLE_SOURCE="live"
            FIRST_BOOT_SETUP="0"
            ;;
        *)
            fail "Unsupported BOOT_PROFILE: $BOOT_PROFILE"
            ;;
    esac
}

resolve_dependencies() {
    require_file "$KERNEL_PATH"
    require_file "$RAW_PARTS_SCRIPT"
    require_file "$LIMINE_CFG_SOURCE"
    require_file "$LIMINE_BIN_DIR/BOOTX64.EFI"
    require_file "$LIMINE_BIN_DIR/limine-bios.sys"
    require_file "$LIMINE_BIN_DIR/limine-bios-cd.bin"
    require_file "$LIMINE_BIN_DIR/limine-uefi-cd.bin"

    PYTHON_CMD="$(resolve_python)"
    if [ -z "$PYTHON_CMD" ]; then
        fail "python3 or python is required to package boot files"
    fi
}

ensure_layout() {
    mkdir -p "$INSTALL_ROOT/boot"
    mkdir -p "$INSTALL_ROOT/boot/raw"
    mkdir -p "$INSTALL_ROOT/EFI/BOOT"
    mkdir -p "$INSTALL_ROOT/limine"
    mkdir -p "$INSTALL_ROOT/System"
}

copy_boot_payload() {
    cp "$KERNEL_PATH" "$INSTALL_ROOT/boot/bootloader.sys"
    cp "$KERNEL_PATH" "$INSTALL_ROOT/boot/main.sys"

    bash "$RAW_PARTS_SCRIPT" "$KERNEL_PATH" "$RAW_PARTS_DIR"
    cp "$RAW_PARTS_DIR"/* "$INSTALL_ROOT/boot/raw/"

    cp "$LIMINE_CFG_SOURCE" "$INSTALL_ROOT/limine.conf"
    cp "$LIMINE_CFG_SOURCE" "$INSTALL_ROOT/boot/limine.conf"
    cp "$LIMINE_CFG_SOURCE" "$INSTALL_ROOT/limine/limine.conf"
    cp "$LIMINE_CFG_SOURCE" "$INSTALL_ROOT/EFI/BOOT/limine.conf"

    cp "$LIMINE_BIN_DIR/limine-bios.sys" "$INSTALL_ROOT/boot/"
    cp "$LIMINE_BIN_DIR/limine-bios-cd.bin" "$INSTALL_ROOT/boot/"
    cp "$LIMINE_BIN_DIR/limine-uefi-cd.bin" "$INSTALL_ROOT/boot/"
    cp "$LIMINE_BIN_DIR/BOOTX64.EFI" "$INSTALL_ROOT/EFI/BOOT/"
}

write_boot_metadata() {
    cat > "$INSTALL_ROOT/INSTALLERS.TXT" <<'EOF'
OS8 Installer Types

1. DOS Text Setup
   Primary artifacts: DOSENV.IMG, OSINST.COM, and OSSYS.IMG.
   DOSENV.IMG is built from the bundled DOS boot disk image, with a FreeDOS download fallback.
   The DOS boot disk keeps OSINST.COM and OSSYS.IMG together in the root folder.
   The ISO output is only a carrier for that same DOS boot disk and payload.
EOF

    cat > "$INSTALL_ROOT/BOOTABLE.CFG" <<EOF
bootable=1
loader=limine
source=${BOOTABLE_SOURCE}
EOF

    cat > "$INSTALL_ROOT/EFI/BOOT/BOOTABLE.CFG" <<EOF
bootable=1
loader=limine
source=${BOOTABLE_SOURCE}
EOF

    cat > "$INSTALL_ROOT/boot/BOOTABLE.CFG" <<EOF
bootable=1
scheme=mbr
active_partition=System
loader=limine
source=${BIOS_BOOTABLE_SOURCE}
EOF

    cat > "$INSTALL_ROOT/System/installer-state.txt" <<EOF
installed=1
profile=system-image
source=${INSTALLER_STATE_SOURCE}
first_boot_setup=${FIRST_BOOT_SETUP}
EOF

    cat > "$INSTALL_ROOT/System/efi-boot.cfg" <<EOF
bootable=1
loader=limine
source=${BOOTABLE_SOURCE}
EOF

    cat > "$INSTALL_ROOT/System/mbr-boot.cfg" <<EOF
bootable=1
scheme=mbr
active_partition=System
loader=limine
source=${BIOS_BOOTABLE_SOURCE}
EOF

    cat > "$INSTALL_ROOT/IMAGE_INFO.txt" <<'EOF'
OS8 System Image

This image contains:
- a staged OS install tree
- Limine BIOS and UEFI boot files

Primary payload files:
- /boot/bootloader.sys
- /boot/main.sys
- /boot/raw/manifest.txt
- /boot/raw/00-limine-requests.bin
- /boot/raw/10-text.bin
- /boot/raw/20-rodata.bin
- /boot/raw/30-data.bin
- /limine.conf
- /boot/limine.conf
- /limine/limine.conf
- /EFI/BOOT/limine.conf
- /boot/limine-bios.sys
- /boot/limine-bios-cd.bin
- /boot/limine-uefi-cd.bin
- /EFI/BOOT/BOOTX64.EFI
EOF
}

main() {
    configure_profile
    resolve_dependencies

    log "Preparing boot payload at $INSTALL_ROOT"
    ensure_layout
    copy_boot_payload
    write_boot_metadata
    write_zip_archive "$INSTALL_ROOT" "$BOOT_IMAGE_ARCHIVE"

    log "Boot files staged into $INSTALL_ROOT"
    log "Boot file archive: $BOOT_IMAGE_ARCHIVE"
}

main "$@"
