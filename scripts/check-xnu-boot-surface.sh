#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: $0 <arch> [xnu-source-dir]" >&2
  exit 2
fi

arch="$1"
xnu_dir="${2:-${XNU_SOURCE_DIR:-External/xnu}}"

require_file() {
  rel="$1"
  if [ ! -f "$xnu_dir/$rel" ]; then
    echo "error: XNU boot surface is missing required file: $rel" >&2
    exit 1
  fi
}

require_text() {
  rel="$1"
  pattern="$2"
  label="$3"
  require_file "$rel"
  if ! grep -Eq "$pattern" "$xnu_dir/$rel"; then
    echo "error: XNU boot surface missing $label in $rel" >&2
    exit 1
  fi
}

if [ ! -d "$xnu_dir" ]; then
  echo "error: XNU source tree is missing at $xnu_dir" >&2
  exit 1
fi

require_text "pexpert/pexpert/boot.h" \
  '#include[[:space:]]+<pexpert/machine/boot.h>' \
  "machine boot header selector"
require_text "pexpert/pexpert/pexpert.h" \
  'extern[[:space:]]+PE_state_t[[:space:]]+PE_state;' \
  "PE_state export"
require_text "osfmk/kern/startup.c" \
  'kernel_bootstrap[[:space:]]*\([[:space:]]*void[[:space:]]*\)' \
  "kernel bootstrap entry"

case "$arch" in
  x86_64)
    require_text "pexpert/pexpert/i386/boot.h" \
      'typedef[[:space:]]+struct[[:space:]]+boot_args' \
      "x86 boot_args structure"
    require_text "pexpert/pexpert/i386/boot.h" \
      '#define[[:space:]]+BOOT_LINE_LENGTH[[:space:]]+1024' \
      "x86 boot_args command-line length"
    require_text "pexpert/pexpert/i386/boot.h" \
      '#define[[:space:]]+kBootArgsRevision[[:space:]]+0' \
      "x86 boot_args revision constant"
    require_text "pexpert/pexpert/i386/boot.h" \
      '#define[[:space:]]+kBootArgsVersion[[:space:]]+2' \
      "x86 boot_args version constant"
    require_text "pexpert/pexpert/i386/boot.h" \
      'kBootArgsEfiMode64' \
      "64-bit EFI boot mode"
    require_text "pexpert/pexpert/i386/boot.h" \
      'struct[[:space:]]+Boot_VideoV1' \
      "legacy x86 boot video structure"
    require_text "pexpert/pexpert/i386/boot.h" \
      'struct[[:space:]]+Boot_Video' \
      "x86 boot video structure"
    require_text "pexpert/pexpert/i386/boot.h" \
      'MemoryMap(Size)?|MemoryMapDescriptorSize' \
      "EFI memory map fields"
    require_text "pexpert/pexpert/i386/boot.h" \
      'uint32_t[[:space:]]+kaddr;[^;]*kernel text' \
      "x86 boot_args kernel base field"
    require_text "pexpert/pexpert/i386/boot.h" \
      'uint32_t[[:space:]]+ksize;[^;]*kernel text' \
      "x86 boot_args kernel size field"
    require_text "pexpert/pexpert/i386/boot.h" \
      'uint32_t[[:space:]]+efiSystemTable;' \
      "x86 boot_args EFI system table field"
    require_text "pexpert/pexpert/i386/boot.h" \
      'uint64_t[[:space:]]+PhysicalMemorySize;' \
      "x86 boot_args physical memory field"
    require_text "pexpert/pexpert/i386/boot.h" \
      'CommandLine|Boot_Video|Video' \
      "command line and video boot fields"
    require_text "pexpert/pexpert/i386/boot.h" \
      'sizeof\(boot_args\)[[:space:]]*==[[:space:]]*4096' \
      "x86 boot_args size invariant"
    require_text "pexpert/pexpert/i386/boot.h" \
      'uint32_t[[:space:]]+__reserved4\[692\];' \
      "x86 boot_args reserved tail"
    require_text "osfmk/x86_64/start.s" \
      'boot_args_start' \
      "x86_64 boot_args entry register handoff"
    require_text "osfmk/i386/i386_init.c" \
      'PE_init_platform[[:space:]]*\([[:space:]]*TRUE[[:space:]]*,[[:space:]]*kernelBootArgs[[:space:]]*\)' \
      "x86 platform initialization from boot_args"
    require_text "osfmk/i386/i386_init.c" \
      'machine_startup[[:space:]]*\([[:space:]]*\)' \
      "x86 machine startup handoff"
    ;;
  arm64)
    require_text "pexpert/pexpert/arm64/boot.h" \
      'typedef[[:space:]]+struct[[:space:]]+boot_args' \
      "arm64 boot_args structure"
    require_text "pexpert/pexpert/arm64/boot.h" \
      'deviceTreeP|deviceTreeLength' \
      "device-tree boot fields"
    require_text "pexpert/pexpert/arm64/boot.h" \
      'Boot_Video|Video' \
      "video boot fields"
    require_text "pexpert/pexpert/arm64/boot.h" \
      'bootFlags|memSizeActual' \
      "extended arm64 boot fields"
    require_text "osfmk/arm64/start.s" \
      'arm_init' \
      "arm64 assembly to C startup handoff"
    require_text "osfmk/arm/arm_init.c" \
      'arm_init[[:space:]]*\([[:space:]]*boot_args[[:space:]]*\*[[:space:]]*args[[:space:]]*\)' \
      "arm64 boot_args entry"
    require_text "osfmk/arm/arm_init.c" \
      'PE_init_platform[[:space:]]*\([^)]*args[^)]*\)' \
      "arm64 platform initialization from boot_args"
    require_text "osfmk/arm/arm_init.c" \
      'machine_startup[[:space:]]*\([[:space:]]*args[[:space:]]*\)' \
      "arm64 machine startup handoff"
    ;;
  *)
    echo "error: XNU boot surface supports ARCH=x86_64 or ARCH=arm64, got: $arch" >&2
    exit 1
    ;;
esac

echo "[XNU] Boot surface verified for $arch at $xnu_dir"
