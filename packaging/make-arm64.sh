#!/usr/bin/env bash
set -euo pipefail
# Run on a native Ubuntu 22.04 AArch64 GitHub runner after installing dependencies.
if [[ "$(uname -m)" != "aarch64" ]]; then echo "native ARM64 runner required" >&2; exit 1; fi
root="$(cd "$(dirname "$0")/.." && pwd)"
stage="${1:?usage: bash packaging/make-arm64.sh BUILD_DIR OUTPUT_DIR}"
dest="${2:?missing output directory}"
mkdir -p "$dest/bin" "$dest/lib" "$dest/share" "$dest/configs" "$dest/docs"
cp -a "$stage/omniocr" "$dest/bin/"
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
cat > "$dest/bin/soffice" <<\'SH\'
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
      /lib/aarch64-linux-gnu/ld-linux*|/lib/aarch64-linux-gnu/libc.so.*|/lib/aarch64-linux-gnu/libm.so.*|/lib/aarch64-linux-gnu/libpthread.so.*|/lib/aarch64-linux-gnu/libdl.so.*|/lib/aarch64-linux-gnu/librt.so.*|/lib/aarch64-linux-gnu/libresolv.so.*) continue;;
    esac
    cp -L -n "$dependency" "$dest/lib/$(basename "$dependency")"
  done < <(ldd "$binary" 2>/dev/null | awk '/=> \/|^\s*\// {for(i=1;i<=NF;i++) if($i ~ /^\//) {print $i; break}}')
done
# Other native plugins (e.g. SSL and graphic filters) may be loaded at runtime.
# Preserve plugin roots commonly used by LibreOffice.
for directory in /usr/lib/aarch64-linux-gnu/gio/modules /usr/lib/aarch64-linux-gnu/gdk-pixbuf-2.0 /usr/lib/aarch64-linux-gnu/libreoffice; do
  if [[ -d "$directory" ]]; then
    mkdir -p "$dest/rootfs$(dirname "$directory")"
    cp -a "$directory" "$dest/rootfs$directory"
  fi
done
cp "$root/packaging/run-arm64.sh" "$dest/run.sh"
cp "$root/packaging/verify-arm64.sh" "$dest/verify.sh"
chmod +x "$dest/run.sh" "$dest/verify.sh"
# Avoid including private local files by generating the checksum manifest in staging.
(cd "$dest" && find bin lib configs docs rootfs -type f -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
