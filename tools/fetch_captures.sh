#!/usr/bin/env bash
# Download the dysentery reference captures used by the N-T2 replay tests.
# Pinned by commit and verified by SHA-256 (tests/captures/MANIFEST.tsv).
# The captures are not committed to this repository (see LEGAL.md).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="${1:-$here/tests/captures}"
commit=7f61b1c1276c27c80643632c24ee57d13b0db572
base="https://raw.githubusercontent.com/Deep-Symmetry/dysentery/$commit"
mkdir -p "$dest"
grep -v '^#' "$here/tests/captures/MANIFEST.tsv" | while IFS=$'\t' read -r file sha src _; do
  [ -n "$file" ] || continue
  out="$dest/$file"
  if [ -f "$out" ] && echo "$sha  $out" | sha256sum -c --status; then continue; fi
  tmp="$out.part"
  curl -fsSL --retry 3 -o "$tmp" "$base/$src"
  if ! echo "$sha  $tmp" | sha256sum -c --status; then
    echo "checksum mismatch for $file" >&2; rm -f "$tmp"; exit 1
  fi
  mv "$tmp" "$out"
done
echo "captures ready in $dest"
