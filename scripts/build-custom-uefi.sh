#!/bin/bash
# Build OS8's custom x86_64 UEFI loader chain.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-build/x86_64}"
KERNEL_PATH="${KERNEL_PATH:-${BUILD_DIR}/kernel/os-x86_64.elf}"
KERNEL_FORMAT="${KERNEL_FORMAT:-elf}"
OUT_DIR="${BUILD_DIR}/boot/custom-uefi"
SRC_DIR="${ROOT_DIR}/boot/custom"
CC="${CC:-clang}"
LLD_LINK="${LLD_LINK:-lld-link}"
SKIP_BOOT_CONFIG="${SKIP_BOOT_CONFIG:-0}"

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "[ERROR] Required command not found: $1" >&2
        exit 1
    fi
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        python3 - "$1" <<'PY'
import hashlib, pathlib, sys
print(hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest())
PY
    fi
}

require_cmd "$CC"
require_cmd "$LLD_LINK"
if [ "$SKIP_BOOT_CONFIG" != "1" ] && [ ! -f "$KERNEL_PATH" ]; then
    echo "[ERROR] Required kernel not found: $KERNEL_PATH" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
CFLAGS=(-target x86_64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone -fno-stack-protector -fno-builtin -nostdlib -Wall -Wextra -I"$SRC_DIR" -I"$ROOT_DIR/boot/xnu")
LDFLAGS=(/subsystem:efi_application /entry:EfiMain /nodefaultlib)

"$CC" "${CFLAGS[@]}" -c "$SRC_DIR/uefi.c" -o "$OUT_DIR/uefi.obj"
"$CC" "${CFLAGS[@]}" -c "$SRC_DIR/uefi-entry.S" -o "$OUT_DIR/uefi-entry.obj"
"$CC" "${CFLAGS[@]}" -c "$SRC_DIR/startup-handoff.S" -o "$OUT_DIR/startup-handoff.obj"
"$CC" "${CFLAGS[@]}" -c "$SRC_DIR/loader.c" -o "$OUT_DIR/loader.obj"
"$LLD_LINK" "${LDFLAGS[@]}" "$OUT_DIR/uefi-entry.obj" "$OUT_DIR/uefi.obj" "$OUT_DIR/loader.obj" /out:"$OUT_DIR/BOOTX64.EFI"

"$CC" "${CFLAGS[@]}" -c "$SRC_DIR/startup.c" -o "$OUT_DIR/startup.obj"
"$LLD_LINK" "${LDFLAGS[@]}" "$OUT_DIR/uefi-entry.obj" "$OUT_DIR/uefi.obj" "$OUT_DIR/startup.obj" "$OUT_DIR/startup-handoff.obj" /out:"$OUT_DIR/STARTUPX64.EFI"

if [ "$SKIP_BOOT_CONFIG" != "1" ]; then
    cat > "$OUT_DIR/os8boot.cfg" <<EOF
version=1
input_timeout_ms=1500
startup_path=\\EFI\\OS8\\STARTUPX64.EFI
startup_sha256=$(sha256_file "$OUT_DIR/STARTUPX64.EFI")
kernel_path=\\boot\\main.sys
kernel_format=$KERNEL_FORMAT
kernel_sha256=$(sha256_file "$KERNEL_PATH")
trusted_key=os8-development
recovery_partition=auto
boot_options=normal
EOF
fi

echo "$OUT_DIR"
