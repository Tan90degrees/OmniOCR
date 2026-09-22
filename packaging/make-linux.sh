#!/usr/bin/env bash
set -euo pipefail
# Run on the native Ubuntu 24.04 runner matching the requested package architecture.
# X64 and ARM64 packages use the same source, manifest, executable layout and checks.
arch="${3:?usage: bash packaging/make-linux.sh BUILD_DIR OUTPUT_DIR x64|arm64}"
case "$arch" in
  x64) expected_machine=x86_64 ;;
  arm64) expected_machine=aarch64 ;;
  *) echo "unsupported package architecture: $arch" >&2; exit 1 ;;
esac
if [[ "$(uname -m)" != "$expected_machine" ]]; then
  echo "native $arch runner required (expected $expected_machine)" >&2; exit 1
fi
multiarch="$(gcc -dumpmachine)"
case "$arch:$multiarch" in
  x64:x86_64-linux-gnu|arm64:aarch64-linux-gnu) ;;
  *) echo "unexpected host toolchain: $multiarch" >&2; exit 1 ;;
esac
root="$(cd "$(dirname "$0")/.." && pwd)"
stage="${1:?usage: bash packaging/make-linux.sh BUILD_DIR OUTPUT_DIR x64|arm64}"
dest="${2:?missing output directory}"
mkdir -p "$dest/bin" "$dest/lib" "$dest/share" "$dest/configs" "$dest/docs"
cp -a "$stage/omniocr" "$dest/bin/"
if [[ -x "$stage/omniocr-server" ]]; then
  cp -a "$stage/omniocr-server" "$dest/bin/"
fi
cp -a "$root/configs/." "$dest/configs/"
cp -a "$root/docs/." "$dest/docs/"
cp -a "$root/README.md" "$dest/"
for name in pdfinfo pdftoppm; do
  path="$(command -v "$name")"
  cp -L "$path" "$dest/bin/$name"
done
# Bundle document conversion and PDF data. The host remains responsible for glibc.
cp -a /usr/lib/libreoffice "$dest/lib/libreoffice"
for directory in /usr/share/libreoffice /usr/share/poppler /usr/share/fonts /etc/libreoffice; do
  if [[ -d "$directory" ]]; then
    mkdir -p "$dest/rootfs$(dirname "$directory")"
    cp -a "$directory" "$dest/rootfs$directory"
  fi
done
# LibreOffice discovers its install tree relative to its own executable.
# Keep all bundled LO libraries together, and use a wrapper for its executable.
cat > "$dest/bin/soffice" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$here/lib/libreoffice/program/soffice" "$@"
SH
chmod +x "$dest/bin/soffice"
# Gather ELF DT_NEEDED dependencies recursively, including dependencies of LO's
# dynamically opened filters (which may not be visible from soffice.bin alone).
mapfile -d '' binaries < <(find "$dest/bin" "$dest/lib/libreoffice" -type f -print0)
for binary in "${binaries[@]}"; do
  [[ "$(head -c4 "$binary" 2>/dev/null || true)" == $'\177ELF' ]] || continue
  while read -r dependency; do
    [[ -f "$dependency" ]] || continue
    case "$dependency" in
      */ld-linux*|*/ld64.so*|*/libc.so.*|*/libm.so.*|*/libpthread.so.*|*/libdl.so.*|*/librt.so.*|*/libresolv.so.*) continue;;
    esac
    cp -L -n "$dependency" "$dest/lib/$(basename "$dependency")"
  done < <(ldd "$binary" 2>/dev/null | awk '/=> \/|^\s*\// {for(i=1;i<=NF;i++) if($i ~ /^\//) {print $i; break}}')
done
# Other native plugins (e.g. SSL and graphic filters) may be loaded at runtime.
# Preserve plugin roots commonly used by LibreOffice.
for directory in "/usr/lib/$multiarch/gio/modules" "/usr/lib/$multiarch/gdk-pixbuf-2.0" "/usr/lib/$multiarch/libreoffice"; do
  if [[ -d "$directory" ]]; then
    mkdir -p "$dest/rootfs$(dirname "$directory")"
    cp -a "$directory" "$dest/rootfs$directory"
  fi
done
cp "$root/packaging/run-linux.sh" "$dest/run.sh"
if [[ -x "$stage/omniocr-server" ]]; then
  cp "$root/packaging/run-linux.sh" "$dest/server.sh"
fi
cp "$root/packaging/verify-linux.sh" "$dest/verify.sh"
chmod +x "$dest/run.sh" "$dest/verify.sh"
[[ ! -f "$dest/server.sh" ]] || chmod +x "$dest/server.sh"
# Avoid including private local files by generating the checksum manifest in staging.
cat > "$dest/PACKAGE-INFO" <<EOF
arch=$arch
machine=$expected_machine
build_ubuntu=24.04
local_backends=HTTP/vLLM,Mock (ACL=OFF,ONNX=OFF)
glibc=host-compatible-required
EOF
(cd "$dest" && find bin lib configs docs rootfs PACKAGE-INFO -type f -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
