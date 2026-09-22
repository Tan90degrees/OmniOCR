#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
arch="$(sed -n 's/^arch=//p' PACKAGE-INFO)"
case "$arch" in
  x64) expected_machine=x86_64; elf_pattern='Advanced Micro Devices X86-64' ;;
  arm64) expected_machine=aarch64; elf_pattern='AArch64' ;;
  *) echo "invalid package architecture: $arch" >&2; exit 1 ;;
esac
[[ "$(uname -m)" == "$expected_machine" ]] || {
  echo "$arch package cannot run on $(uname -m) (expected $expected_machine)" >&2; exit 1;
}
for binary in bin/omniocr bin/omniocr-server; do
  if [[ -e "$binary" ]]; then
    readelf -h "$binary" | grep -F "Machine:" | grep -F "$elf_pattern" >/dev/null || {
      echo "unexpected ELF machine in $binary" >&2; exit 1;
    }
  fi
done
sha256sum -c SHA256SUMS --quiet
./run.sh --help
if [[ -f bin/omniocr-server ]]; then
  ./server.sh --help
fi
