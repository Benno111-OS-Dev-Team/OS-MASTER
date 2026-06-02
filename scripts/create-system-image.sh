#!/bin/bash
# Create an installable OS system image tree and archive for the installer.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-build/x86_64}"
OUTPUT_ROOT="${2:-${BUILD_DIR}/system-image}"
OUTPUT_ARCHIVE="${OUTPUT_ARCHIVE:-${BUILD_DIR}/system-image.zip}"
BOOT_FILES_SCRIPT="${ROOT_DIR}/scripts/build-install-boot-files.sh"
BOOT_LIMINE_CFG="${BOOT_LIMINE_CFG:-${ROOT_DIR}/os-x86_64/limine-installed.conf}"

GREEN='\033[0;32m'
NC='\033[0m'

log() {
    echo -e "${GREEN}[SYSTEM-IMAGE]${NC} $1"
}

require_file() {
    if [ ! -f "$1" ]; then
        echo "[ERROR] Required file not found: $1" >&2
        exit 1
    fi
}

resolve_python() {
    command -v python3 2>/dev/null || command -v python 2>/dev/null || true
}

extract_c_array() {
    local source_file="$1"
    local symbol_name="$2"
    local output_path="$3"
    local python_cmd="$4"

    "$python_cmd" - "$source_file" "$symbol_name" "$output_path" <<'PY'
import pathlib
import re
import sys

source_path = pathlib.Path(sys.argv[1])
symbol_name = sys.argv[2]
output_path = pathlib.Path(sys.argv[3])
text = source_path.read_text(encoding="utf-8")
pattern = re.compile(
    r"(?:const\s+)?(?:unsigned\s+char|uint8_t)\s+%s\[\]\s*=\s*\{(.*?)\};"
    % re.escape(symbol_name),
    re.S,
)
match = pattern.search(text)
if not match:
    raise SystemExit(f"missing array: {symbol_name}")
body = match.group(1)
values = []
for token in body.replace("\n", " ").split(","):
    token = token.strip()
    if not token:
        continue
    values.append(int(token, 0))
output_path.parent.mkdir(parents=True, exist_ok=True)
output_path.write_bytes(bytes(values))
PY
}

write_zip_archive() {
    local root_dir="$1"
    local archive_path="$2"
    local python_cmd="$3"

    "$python_cmd" - "$root_dir" "$archive_path" <<'PY'
import pathlib
import sys
import zipfile

root = pathlib.Path(sys.argv[1]).resolve()
archive = pathlib.Path(sys.argv[2]).resolve()
archive.parent.mkdir(parents=True, exist_ok=True)
if archive.exists():
    archive.unlink()
with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED) as zf:
    for path in sorted(root.rglob("*")):
        if path.is_dir():
            continue
        zf.write(path, path.relative_to(root).as_posix())
PY
}

PYTHON_CMD="$(resolve_python)"
if [ -z "$PYTHON_CMD" ]; then
    echo "[ERROR] python3 or python is required to build the staged system image" >&2
    exit 1
fi

require_file "$ROOT_DIR/kernel/apps/embedded_apps.c"
require_file "$ROOT_DIR/kernel/media/seed_mp3.inc"
require_file "$ROOT_DIR/assets/logo.png"
require_file "$ROOT_DIR/kernel/media/bootstrap_images/landscape.png"
require_file "$ROOT_DIR/kernel/media/bootstrap_images/nature.jpg"
require_file "$ROOT_DIR/kernel/media/bootstrap_images/city.jpg"
require_file "$ROOT_DIR/kernel/media/bootstrap_images/portrait.jpg"
require_file "$ROOT_DIR/kernel/media/bootstrap_images/square.jpg"
require_file "$ROOT_DIR/kernel/media/bootstrap_images/default.jpg"
require_file "$ROOT_DIR/kernel/media/bootstrap_images/test_png.png"

rm -rf "$OUTPUT_ROOT"
mkdir -p "$OUTPUT_ROOT"

log "Staging install tree at $OUTPUT_ROOT"
mkdir -p "$OUTPUT_ROOT/Documents"
mkdir -p "$OUTPUT_ROOT/Downloads"
mkdir -p "$OUTPUT_ROOT/Pictures"
mkdir -p "$OUTPUT_ROOT/assets/wallpapers"
mkdir -p "$OUTPUT_ROOT/System/Apps"
mkdir -p "$OUTPUT_ROOT/Desktop/System Apps"
mkdir -p "$OUTPUT_ROOT/Desktop/Projects"
mkdir -p "$OUTPUT_ROOT/bin"
mkdir -p "$OUTPUT_ROOT/sbin"
mkdir -p "$OUTPUT_ROOT/usr/bin"
mkdir -p "$OUTPUT_ROOT/examples"

cat > "$OUTPUT_ROOT/Desktop/notes.txt" <<'EOF'
Welcome to OS8!

This is your desktop - right-click for options!
EOF

cat > "$OUTPUT_ROOT/Desktop/readme.txt" <<'EOF'
OS8 Desktop Manager

- Double-click to open files
- Right-click for context menu
EOF

cat > "$OUTPUT_ROOT/readme.txt" <<'EOF'
Welcome to OS8!
This is a real file in RamFS.
EOF

cat > "$OUTPUT_ROOT/todo.txt" <<'EOF'
- Implement Browser
- Fix Bugs
- Sleep
EOF

cp "$ROOT_DIR/assets/logo.png" "$OUTPUT_ROOT/assets/logo.png"
cp "$ROOT_DIR/kernel/media/bootstrap_images/landscape.png" "$OUTPUT_ROOT/assets/wallpapers/landscape.png"
cp "$ROOT_DIR/kernel/media/bootstrap_images/nature.jpg" "$OUTPUT_ROOT/assets/wallpapers/nature.jpg"
cp "$ROOT_DIR/kernel/media/bootstrap_images/city.jpg" "$OUTPUT_ROOT/assets/wallpapers/city.jpg"
cp "$ROOT_DIR/kernel/media/bootstrap_images/portrait.jpg" "$OUTPUT_ROOT/assets/wallpapers/portrait.jpg"
cp "$ROOT_DIR/kernel/media/bootstrap_images/square.jpg" "$OUTPUT_ROOT/assets/wallpapers/square.jpg"
cp "$ROOT_DIR/kernel/media/bootstrap_images/default.jpg" "$OUTPUT_ROOT/assets/wallpapers/default.jpg"
cp "$ROOT_DIR/kernel/media/bootstrap_images/test_png.png" "$OUTPUT_ROOT/Pictures/test.png"

cp "$ROOT_DIR/userspace/examples/hello.py" "$OUTPUT_ROOT/examples/hello.py"
cp "$ROOT_DIR/userspace/examples/fibonacci.py" "$OUTPUT_ROOT/examples/fibonacci.py"
cp "$ROOT_DIR/userspace/examples/hello.nano" "$OUTPUT_ROOT/examples/hello.nano"
cp "$ROOT_DIR/userspace/examples/calculator.nano" "$OUTPUT_ROOT/examples/calculator.nano"

extract_c_array "$ROOT_DIR/kernel/apps/embedded_apps.c" "init_bin" "$OUTPUT_ROOT/sbin/init" "$PYTHON_CMD"
extract_c_array "$ROOT_DIR/kernel/apps/embedded_apps.c" "login_bin" "$OUTPUT_ROOT/bin/login" "$PYTHON_CMD"
extract_c_array "$ROOT_DIR/kernel/apps/embedded_apps.c" "shell_bin" "$OUTPUT_ROOT/bin/sh" "$PYTHON_CMD"
extract_c_array "$ROOT_DIR/kernel/media/seed_mp3.inc" "_tmp_os_seed_mp3" "$OUTPUT_ROOT/sample.mp3" "$PYTHON_CMD"
chmod 755 "$OUTPUT_ROOT/sbin/init" "$OUTPUT_ROOT/bin/login" "$OUTPUT_ROOT/bin/sh"

env BOOT_PROFILE=installed-system LIMINE_CFG_SOURCE="$BOOT_LIMINE_CFG" \
    bash "$BOOT_FILES_SCRIPT" "$BUILD_DIR" "$OUTPUT_ROOT"

log "Creating image archive $OUTPUT_ARCHIVE"
write_zip_archive "$OUTPUT_ROOT" "$OUTPUT_ARCHIVE" "$PYTHON_CMD"

log "System image root: $OUTPUT_ROOT"
log "System image archive: $OUTPUT_ARCHIVE"
