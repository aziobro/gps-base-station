#!/bin/sh
set -eu

# USB recovery flash wrapper, run on the monitoring Pi: stops the serial
# logger (esp32-log.service) so esptool has exclusive access to
# /dev/ttyACM0, flashes, then restarts the logger -- even if the flash
# fails -- so a bad flash doesn't also leave the Pi silently not logging.
# See docs/agent-memory/project_deploy_test.md.

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <ota_data_initial.bin> <app.bin>" >&2
    echo "(minimal recovery flash -- app0 + otadata only; bootloader/partition-table untouched)" >&2
    exit 1
fi

OTA_DATA="$1"
APP="$2"
PORT="${PORT:-/dev/ttyACM0}"

restart_logger() {
    sudo systemctl start esp32-log.service
}
trap restart_logger EXIT

sudo systemctl stop esp32-log.service

~/esptool-venv/bin/esptool --chip esp32p4 --port "$PORT" -b 460800 \
    --before default-reset --after hard-reset write-flash \
    0xe000 "$OTA_DATA" 0x10000 "$APP"
