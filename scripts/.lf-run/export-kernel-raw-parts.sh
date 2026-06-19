#!/bin/bash
# Export raw x86_64 kernel sections for staging alongside the boot payload.

set -euo pipefail

KERNEL_PATH="${1:-}"
OUTPUT_DIR="${2:-}"

if [ -z "$KERNEL_PATH" ] || [ -z "$OUTPUT_DIR" ]; then
    echo "usage: $0 <kernel-elf> <output-dir>" >&2
    exit 1
fi

if [ ! -f "$KERNEL_PATH" ]; then
    echo "[ERROR] Kernel ELF not found: $KERNEL_PATH" >&2
    exit 1
fi

resolve_objcopy() {
    if [ -n "${OBJCOPY:-}" ] && command -v "${OBJCOPY}" >/dev/null 2>&1; then
        printf '%s\n' "${OBJCOPY}"
        return 0
    fi
    if command -v llvm-objcopy >/dev/null 2>&1; then
        printf '%s\n' "llvm-objcopy"
        return 0
    fi
    if command -v objcopy >/dev/null 2>&1; then
        printf '%s\n' "objcopy"
        return 0
    fi
    return 1
}

OBJCOPY_BIN="$(resolve_objcopy || true)"
if [ -z "$OBJCOPY_BIN" ]; then
    echo "[ERROR] llvm-objcopy/objcopy is required to export raw kernel parts" >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
rm -f "$OUTPUT_DIR"/*

"$OBJCOPY_BIN" --dump-section .limine_requests="$OUTPUT_DIR/00-limine-requests.bin" "$KERNEL_PATH"
"$OBJCOPY_BIN" --dump-section .text="$OUTPUT_DIR/10-text.bin" "$KERNEL_PATH"
"$OBJCOPY_BIN" --dump-section .rodata="$OUTPUT_DIR/20-rodata.bin" "$KERNEL_PATH"
"$OBJCOPY_BIN" --dump-section .data="$OUTPUT_DIR/30-data.bin" "$KERNEL_PATH"

cat > "$OUTPUT_DIR/manifest.txt" <<'EOF'
OS8 x86_64 raw kernel parts

These files are direct binary dumps of the bootable kernel ELF sections
that Limine loads for the x86_64 image:

- 00-limine-requests.bin
- 10-text.bin
- 20-rodata.bin
- 30-data.bin

bootloader.sys is the first-stage boot entry configured in Limine.
main.sys remains the compatibility copy of the full kernel ELF payload.
EOF
