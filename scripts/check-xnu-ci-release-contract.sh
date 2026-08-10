#!/usr/bin/env bash
set -euo pipefail

workflow="${1:-.github/workflows/ci.yml}"

if [ ! -s "$workflow" ]; then
  echo "error: CI workflow is missing: $workflow" >&2
  exit 1
fi

require_text() {
  pattern="$1"
  label="$2"
  if ! grep -Eq "$pattern" "$workflow"; then
    echo "error: CI release contract is missing $label" >&2
    exit 1
  fi
}

forbid_text() {
  pattern="$1"
  label="$2"
  if grep -Eq "$pattern" "$workflow"; then
    echo "error: CI release contract has misleading $label" >&2
    exit 1
  fi
}

require_text 'XNU_SOURCE_VALIDATION_ONLY=1 make ARCH=x86_64 image' "x86_64 source-validation provider build"
require_text 'XNU_SOURCE_VALIDATION_ONLY=1 make ARCH=arm64 image' "arm64 source-validation provider build"
require_text 'scripts/check-xnu-provider-media\.sh x86_64 image/xnu-x86_64-provider\.tar\.gz source-validation' "x86_64 source-validation media verifier"
require_text 'scripts/check-xnu-provider-media\.sh arm64 image/xnu-arm64-provider\.tar\.gz source-validation' "arm64 source-validation media verifier"
require_text 'XNU source-validation provider media: success' "truthful XNU status wording"
require_text '## Provider Media' "provider-media release notes section"
require_text 'Source-validation XNU provider archives are not bootable kernel replacements' "source-validation non-bootable disclaimer"
forbid_text 'XNU x86_64 provider media: success' "plain x86_64 XNU success wording"
forbid_text 'XNU ARM64 provider media: success' "plain ARM64 XNU success wording"

bootable_section="$(
  awk '
    /^          ## Bootable Media$/ { in_section=1; next }
    /^          ## / && in_section { exit }
    in_section { print }
  ' "$workflow"
)"

if printf '%s\n' "$bootable_section" | grep -Eq 'xnu-[^` ]+-provider\.tar\.gz'; then
  echo "error: source-validation XNU provider archives must not be listed as bootable media" >&2
  exit 1
fi

echo "[XNU] CI release contract verified: $workflow"
