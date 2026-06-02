#!/bin/bash
# Create a full DOS boot disk image and place the OS8 installer payload beside it.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-build/x86_64}"
IMAGE_DIR="${2:-image}"
OUT_IMAGE_NAME="${IMAGE_NAME:-os8-x86_64-dos-env.img}"
DOS_INSTALLER_DIR="${DOS_INSTALLER_DIR:-${BUILD_DIR}/dos-installer}"
FREEDOS_VERSION="${FREEDOS_VERSION:-1.4}"
FREEDOS_FULLUSB_URL="${FREEDOS_FULLUSB_URL:-https://www.freedos.org/download/download/FD14-FullUSB.zip}"
FREEDOS_FULLUSB_SHA256="${FREEDOS_FULLUSB_SHA256:-cd440cd165f5a8a184870cb615f525af182660c15f9bcf1e9d198ca19cedcaff}"
FREEDOS_CACHE_DIR="${FREEDOS_CACHE_DIR:-${BUILD_DIR}/freedos-cache}"

GREEN='\033[0;32m'
NC='\033[0m'

log() {
    echo -e "${GREEN}[DOS-ENV]${NC} $1"
}

fail() {
    echo "[ERROR] $1" >&2
    exit 1
}

require_file() {
    if [ ! -f "$1" ]; then
        fail "Required file not found: $1"
    fi
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        fail "Required command not found: $1"
    fi
}

resolve_python() {
    command -v python3 2>/dev/null || command -v python 2>/dev/null || true
}

sha256_file() {
    local path="$1"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$path" | awk '{print $1}'
        return 0
    fi
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$path" | awk '{print $1}'
        return 0
    fi
    fail "sha256sum or shasum is required"
}

download_freedos_zip() {
    local dst="$1"
    if [ -f "$dst" ]; then
        return 0
    fi
    if command -v curl >/dev/null 2>&1; then
        curl -L --fail --retry 3 -o "$dst" "$FREEDOS_FULLUSB_URL"
        return 0
    fi
    if command -v wget >/dev/null 2>&1; then
        wget -O "$dst" "$FREEDOS_FULLUSB_URL"
        return 0
    fi
    fail "curl or wget is required to download FreeDOS"
}

extract_zip_image() {
    local zip_path="$1"
    local out_dir="$2"
    local py
    py="$(resolve_python)"
    if [ -z "$py" ]; then
        fail "python3 or python is required to extract the FreeDOS archive"
    fi
    "$py" - "$zip_path" "$out_dir" <<'PY'
import pathlib
import sys
import zipfile

zip_path = pathlib.Path(sys.argv[1])
out_dir = pathlib.Path(sys.argv[2])
out_dir.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(zip_path) as zf:
    zf.extractall(out_dir)
PY
}

find_extracted_img() {
    local dir="$1"
    local found
    found=$(find "$dir" -type f -iname '*.img' | head -n 1)
    if [ -z "$found" ]; then
        fail "No .img file was found inside the FreeDOS archive"
    fi
    printf '%s\n' "$found"
}

partition_offset_spec() {
    local image_path="$1"
    local py
    local json_path
    py="$(resolve_python)"
    if [ -z "$py" ]; then
        fail "python3 or python is required to inspect the FreeDOS image"
    fi
    json_path="${image_path}.sfdisk.json"
    sfdisk -J "$image_path" > "$json_path"
    "$py" - "$json_path" "$image_path" <<'PY'
import json
import pathlib
import sys

json_path = pathlib.Path(sys.argv[1])
image_path = pathlib.Path(sys.argv[2])
data = json.loads(json_path.read_text(encoding="utf-8"))
parts = data.get("partitiontable", {}).get("partitions", [])
if not parts:
    print(str(image_path))
    raise SystemExit(0)
start = int(parts[0].get("start", 0))
if start <= 0:
    print(str(image_path))
else:
    print(f"{image_path}@@{start * 512}")
PY
    rm -f "$json_path"
}

write_autoexec() {
    local path="$1"
    cat > "$path" <<'EOF'
@ECHO OFF
CLS
ECHO Starting OS8 Setup...
IF EXIST OSINST.COM GOTO RUNSETUP
ECHO OSINST.COM was not found on this DOS boot disk.
GOTO END
:RUNSETUP
OSINST.COM
:END
COMMAND
EOF
}

main() {
    local zip_name="FD14-FullUSB.zip"
    local zip_path
    local zip_hash
    local extract_dir
    local source_img
    local out_image
    local mtools_image
    local temp_dir
    local autoexec_path

    require_file "${DOS_INSTALLER_DIR}/OSINST.COM"
    require_file "${DOS_INSTALLER_DIR}/OSSYS.IMG"
    require_cmd sfdisk
    require_cmd mcopy
    require_cmd mdel

    mkdir -p "$IMAGE_DIR"
    mkdir -p "$FREEDOS_CACHE_DIR"
    temp_dir="$(mktemp -d)"
    trap 'if [ -n "${temp_dir:-}" ] && [ -d "$temp_dir" ]; then rm -rf "$temp_dir"; fi' EXIT

    zip_path="${FREEDOS_CACHE_DIR}/${zip_name}"
    log "Downloading FreeDOS ${FREEDOS_VERSION} FullUSB boot disk"
    download_freedos_zip "$zip_path"

    zip_hash="$(sha256_file "$zip_path")"
    if [ "$zip_hash" != "$FREEDOS_FULLUSB_SHA256" ]; then
        fail "FreeDOS archive hash mismatch for $zip_path"
    fi

    extract_dir="${temp_dir}/freedos"
    extract_zip_image "$zip_path" "$extract_dir"
    source_img="$(find_extracted_img "$extract_dir")"
    out_image="${IMAGE_DIR}/${OUT_IMAGE_NAME}"
    cp "$source_img" "$out_image"

    mtools_image="$(partition_offset_spec "$out_image")"
    log "Injecting OS8 DOS setup payload into $(basename "$out_image")"
    mcopy -o -i "$mtools_image" "${DOS_INSTALLER_DIR}/OSINST.COM" ::
    mcopy -o -i "$mtools_image" "${DOS_INSTALLER_DIR}/OSSYS.IMG" ::
    if [ -f "${DOS_INSTALLER_DIR}/README.TXT" ]; then
      mcopy -o -i "$mtools_image" "${DOS_INSTALLER_DIR}/README.TXT" ::
    fi

    autoexec_path="${temp_dir}/AUTOEXEC.BAT"
    write_autoexec "$autoexec_path"
    mdel -i "$mtools_image" ::AUTOEXEC.BAT 2>/dev/null || true
    mcopy -o -i "$mtools_image" "$autoexec_path" ::AUTOEXEC.BAT

    log "Full DOS boot disk ready: $out_image"
    ls -lh "$out_image"
}

main "$@"
