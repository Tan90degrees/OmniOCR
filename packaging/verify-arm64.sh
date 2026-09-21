#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
[[ "$(uname -m)" == aarch64 ]] || { echo "AArch64 host required" >&2; exit 1; }
file bin/omniocr | grep -Eq 'ELF 64-bit.*ARM aarch64|ELF 64-bit.*ARM64'
sha256sum -c SHA256SUMS --quiet
./run.sh --help
if [[ -f bin/omniocr-server ]]; then
  file bin/omniocr-server | grep -Eq 'ELF 64-bit.*ARM aarch64|ELF 64-bit.*ARM64'
  ./server.sh --help
fi
