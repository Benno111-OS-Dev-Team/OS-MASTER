#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

bad_impl_word="st""ub"
bad_plural_word="${bad_impl_word}s"
gap_word="place""holder"
empty_action_word="no""-op"
bad_patterns=(
  "\\b${bad_impl_word}\\b"
  "\\b${bad_plural_word}\\b"
  "\\b${gap_word}\\b"
  "\\b${empty_action_word}\\b"
)

scan_dirs=(
  Makefile.multiarch
  boot
  drivers
  kernel
  os-x86_64
  scripts
  .github
)

exclude_re='(^|/)(build|image|diagnostics|libc|os-x86_64/limine|os-x86_64/limine-bin|os-x86_64/limine-src|scripts/\.lf-run)(/|$)'
ext_re='\.(c|h|S|asm|sh|ps1|py|md|yml|yaml|cfg|conf|mk|txt)$'

found=0
while IFS= read -r -d '' file; do
  rel="${file#./}"
  if [[ "$rel" =~ $exclude_re ]]; then
    continue
  fi
  if [[ "$rel" != "AGENTS.md" && "$rel" != "Makefile.multiarch" && ! "$rel" =~ $ext_re ]]; then
    continue
  fi
  for pattern in "${bad_patterns[@]}"; do
    if LC_ALL=C grep -nEI "$pattern" "$rel" >/tmp/os8-source-hits.$$ 2>/dev/null; then
      while IFS= read -r hit; do
        printf '%s:%s\n' "$rel" "$hit"
      done </tmp/os8-source-hits.$$
      found=1
    fi
  done
done < <(git ls-files -z -- "${scan_dirs[@]}")

rm -f /tmp/os8-source-hits.$$

if [ "$found" -ne 0 ]; then
  echo "error: project-owned source contains prohibited incomplete implementation markers" >&2
  exit 1
fi
