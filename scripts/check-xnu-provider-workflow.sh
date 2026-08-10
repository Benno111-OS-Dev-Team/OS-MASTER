#!/usr/bin/env bash
set -euo pipefail

workflow="${1:-.github/workflows/xnu-provider-build.yml}"

if [ ! -s "$workflow" ]; then
  echo "error: XNU provider workflow is missing: $workflow" >&2
  exit 1
fi

require_text() {
  pattern="$1"
  label="$2"
  if ! grep -Eq "$pattern" "$workflow"; then
    echo "error: XNU provider workflow is missing $label" >&2
    exit 1
  fi
}

require_text 'workflow_dispatch:' "manual dispatch entry point"
require_text 'runs-on:[[:space:]]+macos-latest' "macOS runner"
require_text 'SDKROOT:[[:space:]]+\$\{\{[[:space:]]*inputs\.sdkroot[[:space:]]*\}\}' "SDKROOT input propagation"
require_text 'KERNEL_CONFIGS:[[:space:]]+\$\{\{[[:space:]]*inputs\.kernel_configs[[:space:]]*\}\}' "KERNEL_CONFIGS input propagation"
require_text 'KDKROOT:[[:space:]]+\$\{\{[[:space:]]*inputs\.kdkroot[[:space:]]*\}\}' "KDKROOT input propagation"
require_text 'XNU_REF:[[:space:]]+\$\{\{[[:space:]]*inputs\.xnu_ref[[:space:]]*\}\}' "XNU_REF input propagation"
require_text 'git clone --depth=1 https://github\.com/apple-oss-distributions/xnu\.git External/xnu' "official external XNU fetch"
require_text 'git -C External/xnu fetch --depth=1 origin "\$XNU_REF"' "pinned external XNU ref fetch"
require_text 'git -C External/xnu checkout --detach FETCH_HEAD' "pinned external XNU ref checkout"
require_text 'chmod -R a-w External/xnu' "read-only external XNU checkout"
require_text 'brew install .*coreutils.*mtools.*qemu' "macOS boot smoke tool installation"
require_text 'KERNEL_PROVIDER=xnu[[:space:]]*\\' "XNU provider Makefile path"
require_text 'check-xnu-build-env' "real XNU build environment gate"
require_text 'make -f Makefile\.multiarch KERNEL_PROVIDER=xnu ARCH="\$\{\{[[:space:]]*inputs\.arch[[:space:]]*\}\}" kernel' "compiled XNU provider build"
require_text 'make -f Makefile\.multiarch KERNEL_PROVIDER=xnu ARCH="\$\{\{[[:space:]]*inputs\.arch[[:space:]]*\}\}" xnu-image' "XNU provider media packaging"
require_text 'grep -q '\''\^mode=compiled\$'\''' "compiled provider manifest assertion"
require_text 'scripts/check-xnu-kernel-artifact\.sh "\$\{\{[[:space:]]*inputs\.arch[[:space:]]*\}\}"' "compiled Mach-O artifact verifier"
require_text 'scripts/check-xnu-provider-media\.sh "\$\{\{[[:space:]]*inputs\.arch[[:space:]]*\}\}" "\$archive" compiled' "compiled provider media verifier"
require_text 'scripts/create-xnu-provider-proof\.sh' "compiled provider proof manifest generation"
require_text 'scripts/check-xnu-provider-proof\.sh' "compiled provider proof manifest verification"
require_text 'XNU_COMPILED_UEFI_SMOKE_LOG=build/x86_64/xnu-smoke/qemu-serial\.log' "compiled smoke log capture"
require_text 'check-xnu-compiled-uefi-boot-smoke' "compiled x86_64 UEFI handoff smoke"
require_text 'Entered ExitBootServices; jumping to XNU kernel' "post-ExitBootServices handoff assertion"
require_text 'git -C External/xnu status --porcelain=v1 --untracked-files=normal' "clean external checkout assertion"
require_text 'actions/upload-artifact@v4' "provider artifact upload"
require_text 'build/\$\{\{[[:space:]]*inputs\.arch[[:space:]]*\}\}/xnu-proof/xnu-provider-proof\.manifest' "provider proof manifest upload"
require_text 'build/\$\{\{[[:space:]]*inputs\.arch[[:space:]]*\}\}/xnu-smoke/qemu-serial\.log' "compiled smoke log upload"

if grep -Eq 'XNU_SOURCE_VALIDATION_ONLY=1' "$workflow"; then
  echo "error: real XNU provider workflow must not use source-validation mode" >&2
  exit 1
fi

echo "[XNU] Provider workflow contract verified: $workflow"
