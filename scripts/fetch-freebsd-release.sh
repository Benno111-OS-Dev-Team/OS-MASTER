#!/usr/bin/env bash

set -euo pipefail

PRODUCT_NAME="${PRODUCT_NAME:-os-master-freebsd}"
FREEBSD_RELEASE="${FREEBSD_RELEASE:-14.4-RELEASE}"
FREEBSD_ARCH="${FREEBSD_ARCH:-amd64}"
FREEBSD_IMAGE_BASENAME="${FREEBSD_IMAGE_BASENAME:-dvd1.iso}"
BUILD_DIR="${BUILD_DIR:-build/freebsd}"
OUTPUT_DIR="${OUTPUT_DIR:-image}"
FREEBSD_SERIES="${FREEBSD_SERIES:-${FREEBSD_RELEASE%%-RELEASE}}"
FREEBSD_BASE_URL="${FREEBSD_BASE_URL:-https://download.freebsd.org/releases/ISO-IMAGES/${FREEBSD_SERIES}}"
PYTHON_BIN="${PYTHON_BIN:-python3}"

artifact_xz="FreeBSD-${FREEBSD_RELEASE}-${FREEBSD_ARCH}-${FREEBSD_IMAGE_BASENAME}.xz"
artifact_raw="${artifact_xz%.xz}"
checksum_file="CHECKSUM.SHA256-FreeBSD-${FREEBSD_RELEASE}-${FREEBSD_ARCH}"
artifact_url="${FREEBSD_BASE_URL}/${artifact_xz}"
checksum_url="${FREEBSD_BASE_URL}/${checksum_file}"
output_prefix="${PRODUCT_NAME}-${FREEBSD_RELEASE}-${FREEBSD_ARCH}-${FREEBSD_IMAGE_BASENAME}"
output_iso="${OUTPUT_DIR}/${output_prefix}"
output_xz="${OUTPUT_DIR}/${output_prefix}.xz"
output_manifest="${OUTPUT_DIR}/${output_prefix}.txt"

mkdir -p "${BUILD_DIR}" "${OUTPUT_DIR}"

if ! command -v "${PYTHON_BIN}" >/dev/null 2>&1; then
  echo "[ERROR] ${PYTHON_BIN} is required" >&2
  exit 1
fi

if ! "${PYTHON_BIN}" -c "import pycdlib" >/dev/null 2>&1; then
  echo "[ERROR] pycdlib is required for ISO customization" >&2
  echo "[ERROR] Install it with: ${PYTHON_BIN} -m pip install pycdlib" >&2
  exit 1
fi

echo "[FETCH] ${artifact_url}"
curl -fL --retry 3 --retry-delay 2 -o "${BUILD_DIR}/${artifact_xz}" "${artifact_url}"

echo "[FETCH] ${checksum_url}"
curl -fL --retry 3 --retry-delay 2 -o "${BUILD_DIR}/${checksum_file}" "${checksum_url}"

expected_sha256="$(
  grep -F "(${artifact_xz})" "${BUILD_DIR}/${checksum_file}" | awk '{print $4}'
)"

if [ -z "${expected_sha256}" ]; then
  echo "[ERROR] Could not find SHA256 for ${artifact_xz} in ${checksum_file}" >&2
  exit 1
fi

actual_sha256="$(sha256sum "${BUILD_DIR}/${artifact_xz}" | awk '{print $1}')"

if [ "${actual_sha256}" != "${expected_sha256}" ]; then
  echo "[ERROR] SHA256 mismatch for ${artifact_xz}" >&2
  echo "[ERROR] Expected: ${expected_sha256}" >&2
  echo "[ERROR] Actual:   ${actual_sha256}" >&2
  exit 1
fi

echo "[VERIFY] SHA256 OK"

echo "[EXPAND] ${artifact_xz}"
xz -dc "${BUILD_DIR}/${artifact_xz}" > "${BUILD_DIR}/${artifact_raw}"

echo "[CUSTOMIZE] ${artifact_raw}"
"${PYTHON_BIN}" ./scripts/customize-freebsd-media.py \
  --input "${BUILD_DIR}/${artifact_raw}" \
  --output "${output_iso}"

echo "[COMPRESS] ${output_iso}"
xz -T0 -zc "${output_iso}" > "${output_xz}"

custom_iso_sha256="$(sha256sum "${output_iso}" | awk '{print $1}')"
custom_xz_sha256="$(sha256sum "${output_xz}" | awk '{print $1}')"

cat > "${output_manifest}" <<EOF
OS-MASTER FreeBSD X11 Installer Manifest

Product: ${PRODUCT_NAME}
FreeBSD release: ${FREEBSD_RELEASE}
Architecture: ${FREEBSD_ARCH}
Image basename: ${FREEBSD_IMAGE_BASENAME}
Series: ${FREEBSD_SERIES}
Source image URL: ${artifact_url}
Source checksum URL: ${checksum_url}
Source SHA256 (${artifact_xz}): ${actual_sha256}
Customized SHA256 (${output_prefix}): ${custom_iso_sha256}
Customized SHA256 (${output_prefix}.xz): ${custom_xz_sha256}
Customization:
  - installer-side X11 hook
  - installed-system X11 hook
  - seeded .xinitrc launching twm
EOF

echo "[DONE] Staged ${output_iso}"
echo "[DONE] Staged ${output_xz}"
echo "[DONE] Wrote ${output_manifest}"
