#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="$here/bin:$PATH"
export LD_LIBRARY_PATH="$here/lib:$here/lib/libreoffice/program${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export FONTCONFIG_PATH="$here/rootfs/etc/fonts${FONTCONFIG_PATH:+:$FONTCONFIG_PATH}"
export FONTCONFIG_FILE="${FONTCONFIG_FILE:-/etc/fonts/fonts.conf}"
export XDG_DATA_DIRS="$here/rootfs/usr/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
# The C++ document reader launches 'soffice' via PATH; this wrapper also handles
# the Office launcher by placing the bundled LibreOffice program/ directory first.
if [[ "$(basename "$0")" == "server.sh" ]]; then
  exec "$here/bin/omniocr-server" "$@"
fi
exec "$here/bin/omniocr" "$@"
