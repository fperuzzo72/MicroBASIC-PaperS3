#!/usr/bin/env bash
# Pick which app the M5PaperS3 dev unit boots, without touching either app image.
#
#   editor/boot-slot.sh            # report the current selection
#   editor/boot-slot.sh 0          # boot app0 (CrossPoint reader)
#   editor/boot-slot.sh 1          # boot app1 (MicroBASIC, this repo)
#
# The unit carries both firmwares at once (see docs/DUAL_BOOT.md); only the 8KB
# otadata partition decides which one runs, so switching is instant and neither
# slot is rewritten. This is a thin wrapper over ESP-IDF's own otatool.py -- it
# exists so the otadata entry (a sequence number plus its CRC) is written by the
# vendor's implementation rather than a hand-rolled one.
#
# Override the port with PORT=/dev/cu.usbmodemXXXX. Power-cycle with the
# physical button afterwards.
set -euo pipefail

SLOT="${1:-}"
IDF="${IDF:-$HOME/.platformio/packages/framework-espidf}"
PY="${PY:-$HOME/.platformio/penv/bin/python}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ ! -f "$IDF/components/app_update/otatool.py" ]]; then
  echo "otatool.py not found under $IDF -- build once with pio, or set IDF=" >&2
  exit 1
fi

if [[ -z "${PORT:-}" ]]; then
  PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)"
  [[ -n "$PORT" ]] || { echo "no /dev/cu.usbmodem* found -- set PORT=" >&2; exit 1; }
fi

run() {
  # otatool.py imports parttool from components/partition_table, and parttool
  # in turn locates esptool through a literal '$IDF_PATH' it expands itself --
  # so both have to be set, not just the import path.
  PYTHONPATH="$IDF/components/partition_table" IDF_PATH="$IDF" \
    "$PY" "$IDF/components/app_update/otatool.py" \
    --port "$PORT" --partition-table-file "$HERE/partitions.csv" "$@"
}

if [[ -z "$SLOT" ]]; then
  run read_otadata
  exit 0
fi

case "$SLOT" in
  0) echo "-> app0 (CrossPoint)" ;;
  1) echo "-> app1 (MicroBASIC)" ;;
  *) echo "slot must be 0 or 1" >&2; exit 1 ;;
esac
run switch_ota_partition --slot "$SLOT"
echo "Done. Power-cycle with the physical button."
