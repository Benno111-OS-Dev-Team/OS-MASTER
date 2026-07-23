#!/bin/bash
# Build script for UEFI Demo OS
# Builds OS8's custom UEFI startup chain before creating media.

set -e

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# Configuration
BUILD_DIR="build"
ISO_ROOT="iso_root"
ISO_NAME="uefi-demo.iso"
BOOT_MANAGER_DIR="${BUILD_DIR}/boot-assets/os-boot-manager"
BOOT_MANAGER_SYNC="${ROOT_DIR}/scripts/update-os-boot-manager.sh"
CUSTOM_UEFI_SCRIPT="${ROOT_DIR}/scripts/build-custom-uefi.sh"

echo "=== UEFI Demo OS Build Script ==="
echo ""

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

# Create build directories
mkdir -p "$BUILD_DIR"/{boot,lib,drivers,gui}

# Compile kernel
echo "[1/5] Compiling kernel..."

CC="clang"
CFLAGS="-target x86_64-unknown-none-elf -ffreestanding -fno-stack-protector \
        -fno-stack-check -fno-lto -fno-PIC -m64 -march=x86-64 \
        -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
        -mcmodel=kernel -Ikernel/include -Wall -O2"

$CC $CFLAGS -c kernel/boot/limine_boot.c -o $BUILD_DIR/boot/limine_boot.o 2>/dev/null
$CC $CFLAGS -c kernel/lib/string.c -o $BUILD_DIR/lib/string.o 2>/dev/null
$CC $CFLAGS -c kernel/drivers/framebuffer.c -o $BUILD_DIR/drivers/framebuffer.o 2>/dev/null
$CC $CFLAGS -c kernel/gui/font.c -o $BUILD_DIR/gui/font.o 2>/dev/null
$CC $CFLAGS -c kernel/gui/desktop.c -o $BUILD_DIR/gui/desktop.o 2>/dev/null
$CC $CFLAGS -c kernel/gui/window.c -o $BUILD_DIR/gui/window.o 2>/dev/null
$CC $CFLAGS -c kernel/gui/compositor.c -o $BUILD_DIR/gui/compositor.o 2>/dev/null

echo "[2/5] Linking kernel..."
ld.lld -nostdlib -static -z max-page-size=0x1000 -T kernel/linker.ld \
    $BUILD_DIR/boot/limine_boot.o \
    $BUILD_DIR/lib/string.o \
    $BUILD_DIR/drivers/framebuffer.o \
    $BUILD_DIR/gui/font.o \
    $BUILD_DIR/gui/desktop.o \
    $BUILD_DIR/gui/window.o \
    $BUILD_DIR/gui/compositor.o \
    -o $BUILD_DIR/main.sys

echo "   Kernel: $BUILD_DIR/main.sys ($(ls -lh $BUILD_DIR/main.sys | awk '{print $5}'))"

# Refresh the boot manager assets before building the image.
echo "[3/5] Getting OS-BOOT-MANAGER boot assets..."
BOOT_MANAGER_DIR="$("$BOOT_MANAGER_SYNC" "$BOOT_MANAGER_DIR")"
LIMINE_BIN_DIR="${BOOT_MANAGER_DIR}/bin"
LIMINE_SRC_DIR="${BOOT_MANAGER_DIR}"

if [ ! -f "${LIMINE_BIN_DIR}/limine-bios-cd.bin" ]; then
    echo "   WARNING: Latest BIOS boot assets were not refreshed, falling back to the generated cache if available."
fi

if [ ! -f "${LIMINE_BIN_DIR}/limine-bios-cd.bin" ]; then
    echo ""
    echo "WARNING: Could not obtain legacy BIOS boot assets."
    echo "You may need network access or a cached boot asset directory."
    echo ""
    echo "For now, creating a QEMU-compatible disk image..."

    mkdir -p "$ISO_ROOT"/boot
    cp $BUILD_DIR/main.sys "$ISO_ROOT"/boot/bootloader.sys
    cp $BUILD_DIR/main.sys "$ISO_ROOT"/boot/main.sys
    cp limine.conf "$ISO_ROOT"/boot/limine.conf

    echo ""
    echo "To test in QEMU with direct kernel boot:"
    echo "  qemu-system-x86_64 -M q35 -m 512M -kernel $BUILD_DIR/main.sys -serial stdio"
    echo ""
    exit 0
fi

echo "   Legacy BIOS boot assets ready!"

# Create ISO structure
echo "[4/5] Creating ISO structure..."
rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT"/boot
mkdir -p "$ISO_ROOT"/EFI/BOOT
EFI_BOOT_IMAGE="$ISO_ROOT/EFI/efiboot.img"

# Copy kernel
cp $BUILD_DIR/main.sys "$ISO_ROOT"/boot/bootloader.sys
cp $BUILD_DIR/main.sys "$ISO_ROOT"/boot/main.sys

# Copy Limine config to multiple locations (Limine searches several paths)
cp limine.conf "$ISO_ROOT"/boot/limine.conf
cp limine.conf "$ISO_ROOT"/limine.conf
mkdir -p "$ISO_ROOT"/limine
cp limine.conf "$ISO_ROOT"/limine/limine.conf

# Copy OS8 custom UEFI chain
CUSTOM_UEFI_DIR="$(KERNEL_PATH="$BUILD_DIR/main.sys" bash "$CUSTOM_UEFI_SCRIPT" "$BUILD_DIR")"
mkdir -p "$ISO_ROOT"/EFI/OS8
cp "$CUSTOM_UEFI_DIR/BOOTX64.EFI" "$ISO_ROOT"/EFI/BOOT/BOOTX64.EFI
cp "$CUSTOM_UEFI_DIR/STARTUPX64.EFI" "$ISO_ROOT"/EFI/OS8/STARTUPX64.EFI
cp "$CUSTOM_UEFI_DIR/os8boot.cfg" "$ISO_ROOT"/EFI/OS8/os8boot.cfg

# Build a real FAT ESP image for UEFI El Torito boot.
for tool in dd mformat mmd mcopy; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "error: required tool '$tool' not found" >&2
        exit 1
    }
done
rm -f "$EFI_BOOT_IMAGE"
dd if=/dev/zero of="$EFI_BOOT_IMAGE" bs=1M count=16 status=none
mformat -i "$EFI_BOOT_IMAGE" -v OS8EFI ::
mmd -i "$EFI_BOOT_IMAGE" ::/EFI ::/EFI/BOOT ::/EFI/OS8 ::/boot
mcopy -i "$EFI_BOOT_IMAGE" "$ISO_ROOT/EFI/BOOT/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$EFI_BOOT_IMAGE" "$ISO_ROOT/EFI/OS8/STARTUPX64.EFI" ::/EFI/OS8/STARTUPX64.EFI
mcopy -i "$EFI_BOOT_IMAGE" "$ISO_ROOT/EFI/OS8/os8boot.cfg" ::/EFI/OS8/os8boot.cfg
mcopy -i "$EFI_BOOT_IMAGE" "$ISO_ROOT/boot/main.sys" ::/boot/main.sys
mcopy -i "$EFI_BOOT_IMAGE" "$ISO_ROOT/boot/bootloader.sys" ::/boot/bootloader.sys

# Copy legacy BIOS CD boot file if available
[ -f "$LIMINE_BIN_DIR/limine-bios-cd.bin" ] && cp "$LIMINE_BIN_DIR/limine-bios-cd.bin" "$ISO_ROOT"/boot/

# Create ISO
echo "[5/5] Creating bootable ISO..."

if [ -f "$ISO_ROOT/boot/limine-bios-cd.bin" ]; then
    xorriso -as mkisofs \
        -b boot/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        -eltorito-alt-boot \
        -e EFI/efiboot.img \
        -no-emul-boot \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        "$ISO_ROOT" -o "$ISO_NAME" 2>/dev/null
else
    # UEFI-only ISO
    xorriso -as mkisofs \
        -e EFI/efiboot.img \
        -no-emul-boot \
        -isohybrid-gpt-basdat \
        "$ISO_ROOT" -o "$ISO_NAME" 2>/dev/null || \
    xorriso -as mkisofs \
        "$ISO_ROOT" -o "$ISO_NAME"
fi

compress_iso "$ISO_NAME"

echo ""
echo "============================================"
echo "  SUCCESS! Created: $ISO_NAME"
echo "  Size: $(ls -lh $ISO_NAME | awk '{print $5}')"
if [ -f "${ISO_NAME}.xz" ]; then
    echo "  Compressed: $(ls -lh "${ISO_NAME}.xz" | awk '{print $5}')"
fi
echo "============================================"
echo ""
echo "To test in QEMU (UEFI):"
echo "  qemu-system-x86_64 -M q35 -m 512M -cdrom $ISO_NAME \\"
echo "    -bios /opt/homebrew/share/qemu/edk2-x86_64-code.fd -serial stdio"
echo ""
echo "To flash to USB (BE CAREFUL!):"
echo "  sudo dd if=$ISO_NAME of=/dev/rdiskN bs=4m status=progress"
