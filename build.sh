#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KAS_VERSION="${KAS_VERSION:-4.6}"
export KAS_IMAGE="${KAS_IMAGE:-ghcr.io/siemens/kas/kas:${KAS_VERSION}}"

if [[ ! -x "$HERE/.kas-container" ]]; then
    echo "[build.sh] Fetching kas-container ${KAS_VERSION}..."
    curl -fsSL "https://raw.githubusercontent.com/siemens/kas/${KAS_VERSION}/kas-container" \
        -o "$HERE/.kas-container"
    chmod +x "$HERE/.kas-container"
fi

mkdir -p "$HERE/shared/downloads" "$HERE/shared/sstate-cache" "$HERE/build"
# kas-container runs as UID 30000 (builder) inside the image; host dirs
# must be world-writable so the container can populate them.
chmod 0777 "$HERE/shared" "$HERE/shared/downloads" "$HERE/shared/sstate-cache" "$HERE/build" 2>/dev/null || true

cd "$HERE"
exec ./.kas-container "${@:-build}" kas.yml
