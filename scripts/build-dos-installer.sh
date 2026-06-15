#!/bin/bash
# Build the DOS textmode installer utility and package its companion files.

set -euo pipefail

BUILD_DIR="${1:-build/x86_64}"
IMAGE_DIR="${2:-image}"
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${ROOT_DIR}/boot/bios/dos_installer_com.S"
OUT_DIR="${BUILD_DIR}/dos-installer"
OBJ_PATH="${OUT_DIR}/OSINST.o"
COM_PATH="${OUT_DIR}/OSINST.COM"
README_PATH="${OUT_DIR}/README.TXT"
SYSTEM_IMG_CANDIDATES=(
  "${IMAGE_DIR}/os8-x86_64-system.img"
  "${IMAGE_DIR}/os-x86_64-system.img"
  "${IMAGE_DIR}/os-x86_64.img"
)

CC_BIN="${CC_BIN:-clang}"
LD_BIN="${LD_BIN:-ld.lld}"

mkdir -p "${OUT_DIR}"

if ! command -v "${CC_BIN}" >/dev/null 2>&1; then
  echo "[DOS-INSTALLER] clang not found; skipping DOS installer build" >&2
  exit 0
fi
if ! command -v "${LD_BIN}" >/dev/null 2>&1; then
  echo "[DOS-INSTALLER] ld.lld not found; skipping DOS installer build" >&2
  exit 0
fi

echo "[DOS-INSTALLER] Assembling ${SRC}"
"${CC_BIN}" --target=i386-unknown-none-elf -m16 -ffreestanding -nostdlib \
  -x assembler-with-cpp -c "${SRC}" -o "${OBJ_PATH}"

echo "[DOS-INSTALLER] Linking ${COM_PATH}"
"${LD_BIN}" -m elf_i386 -Ttext 0x100 --oformat=binary -nostdlib -e _start \
  -o "${COM_PATH}" "${OBJ_PATH}"

cat > "${README_PATH}" <<'EOF'
OS8 Text Setup for MS-DOS

Files:
- OSINST.COM  DOS textmode setup utility
- OSSYS.IMG   Raw OS8 system image written by Setup

Usage:
1. Copy OSINST.COM and OSSYS.IMG into the same DOS directory.
2. Boot MS-DOS or compatible DOS.
3. Change to that directory and run OSINST.COM.
4. Press C to choose the target BIOS disk.
5. Press ENTER to install OS8, or R to repair BIOS boot code only.
6. Press F3 to exit Setup.

Notes:
- Setup tries BIOS disks 0x81-0x87 before 0x80 for safety.
- The selected target disk is overwritten sector-by-sector during install.
- This utility is intended to resemble classic Windows NT text setup.
EOF

for candidate in "${SYSTEM_IMG_CANDIDATES[@]}"; do
  if [ -f "${candidate}" ]; then
    cp "${candidate}" "${OUT_DIR}/OSSYS.IMG"
    echo "[DOS-INSTALLER] Packaged companion image from ${candidate}"
    break
  fi
done

echo "[DOS-INSTALLER] Ready: ${COM_PATH}"
