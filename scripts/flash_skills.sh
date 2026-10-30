#!/usr/bin/env bash
# Build and flash the skills SPIFFS image to the ESP32.
# Usage: ./scripts/flash_skills.sh [serial-port] [project-dir]
set -euo pipefail
PORT="${1:-/dev/cu.usbserial-0001}"
PROJ="${2:-$(dirname "$0")/../examples/esp32}"
IDF_PATH="${IDF_PATH:-$HOME/esp-idf}"
. "$IDF_PATH/export.sh" >/dev/null

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp -r "$PROJ/skills" "$STAGE/"

IMG="$PROJ/build/skills.img"
python "$IDF_PATH/components/spiffs/spiffsgen.py" --obj-name-len 64 1572864 \
    "$STAGE" "$IMG" >/dev/null
echo "spiffs image: $IMG"

STORAGE_OFFSET=0x10000
python -m esptool --chip esp32 -p "$PORT" -b 460800 \
    --before default_reset --after hard_reset write_flash "$STORAGE_OFFSET" "$IMG"
echo "flashed to $PORT at $STORAGE_OFFSET"
