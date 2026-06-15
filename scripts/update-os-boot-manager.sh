#!/bin/bash
# Fetch the latest OS-BOOT-MANAGER release assets into a build-local cache.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR="${1:-${ROOT_DIR}/build/boot-assets/os-boot-manager}"
LATEST_URL="https://github.com/Benno111-OS-Dev-Team/OS-BOOT-MANAGER/releases/latest/download/os-boot-manager-binary.tar.xz"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() {
    echo -e "${GREEN}[OS-BOOT-MANAGER]${NC} $1" >&2
}

warn() {
    echo -e "${YELLOW}[OS-BOOT-MANAGER]${NC} $1" >&2
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "[ERROR] Required command not found: $1" >&2
        exit 1
    fi
}

sync_from_archive() {
    local archive="$1"
    local extract_dir="$2"
    local source_dir="$extract_dir/os-boot-manager-binary"
    local root_files=(
        "BOOTAA64.EFI"
        "BOOTIA32.EFI"
        "BOOTLOONGARCH64.EFI"
        "BOOTRISCV64.EFI"
        "BOOTX64.EFI"
        "LICENSE"
        "Makefile"
        "limine-bios-cd.bin"
        "limine-bios-hdd.h"
        "limine-bios-pxe.bin"
        "limine-bios.sys"
        "limine.c"
        "limine-uefi-cd.bin"
    )
    local bin_files=(
        "BOOTX64.EFI"
        "limine-bios-cd.bin"
        "limine-bios.sys"
        "limine-uefi-cd.bin"
    )

    rm -rf "$OUTPUT_DIR"
    mkdir -p "$OUTPUT_DIR/bin"

    tar -xf "$archive" -C "$extract_dir"

    for file in "${root_files[@]}"; do
        cp "$source_dir/$file" "$OUTPUT_DIR/$file"
    done

    for file in "${bin_files[@]}"; do
        cp "$source_dir/$file" "$OUTPUT_DIR/bin/$file"
    done
}

require_cmd curl
require_cmd tar
require_cmd cp
require_cmd mkdir
require_cmd rm

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

ARCHIVE="$TMP_DIR/os-boot-manager-binary.tar.xz"
EXTRACT_DIR="$TMP_DIR/extract"
mkdir -p "$EXTRACT_DIR"

log "Fetching latest release from upstream"
if curl -fL --retry 3 --retry-delay 2 "$LATEST_URL" -o "$ARCHIVE"; then
    log "Syncing assets into $OUTPUT_DIR"
    sync_from_archive "$ARCHIVE" "$EXTRACT_DIR"
    log "Updated boot assets"
else
    if [ -f "$OUTPUT_DIR/bin/BOOTX64.EFI" ] && \
       [ -f "$OUTPUT_DIR/bin/limine-bios.sys" ] && \
       [ -f "$OUTPUT_DIR/bin/limine-bios-cd.bin" ] && \
       [ -f "$OUTPUT_DIR/bin/limine-uefi-cd.bin" ] && \
       [ -f "$OUTPUT_DIR/limine.c" ]; then
        warn "Failed to refresh upstream release; reusing cached boot assets in $OUTPUT_DIR"
        printf '%s\n' "$OUTPUT_DIR"
        exit 0
    fi

    echo "[ERROR] Failed to download OS-BOOT-MANAGER latest release and no cached copy is available." >&2
    exit 1
fi

printf '%s\n' "$OUTPUT_DIR"
