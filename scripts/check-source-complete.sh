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
  libc/src
  os-x86_64
  scripts
  .github
)

exclude_re='(^|/)(build|image|diagnostics|os-x86_64/limine|os-x86_64/limine-bin|os-x86_64/limine-src|scripts/\.lf-run)(/|$)'
ext_re='\.(c|h|S|asm|sh|ps1|py|md|yml|yaml|cfg|conf|mk|txt)$'

combined_pattern="$(IFS='|'; echo "${bad_patterns[*]}")"
files=()
while IFS= read -r -d '' file; do
  rel="${file#./}"
  if [[ "$rel" =~ $exclude_re ]]; then
    continue
  fi
  if [[ "$rel" != "AGENTS.md" && "$rel" != "Makefile.multiarch" && ! "$rel" =~ $ext_re ]]; then
    continue
  fi
  files+=("$rel")
done < <(git ls-files -z -- "${scan_dirs[@]}")

if [ "${#files[@]}" -ne 0 ] &&
   LC_ALL=C grep -nEI "$combined_pattern" "${files[@]}" >/tmp/os8-source-hits.$$ 2>/dev/null; then
  cat /tmp/os8-source-hits.$$
  rm -f /tmp/os8-source-hits.$$
  echo "error: project-owned source contains prohibited incomplete implementation markers" >&2
  exit 1
fi

rm -f /tmp/os8-source-hits.$$
