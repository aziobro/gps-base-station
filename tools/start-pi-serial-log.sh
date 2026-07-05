#!/bin/sh
set -eu

# Foreground serial capture, run under systemd (see esp32-log.service) so
# systemd owns the process lifecycle directly -- auto-start on Pi boot,
# Restart=always if it dies. Do NOT background this with nohup/&: an
# earlier version did that plus its own "while true" respawn loop, and a
# stray pkill that only killed `cat` (not the loop) left the old loop
# alive to spawn a second reader on the same port -- two processes both
# reading /dev/ttyACM0 split the serial byte stream and corrupt both logs
# (hit this 2026-07-05 while adding systemd supervision).
#
# To manually bounce the logger: `sudo systemctl restart esp32-log.service`.
# To free the port for esptool (USB recovery flash): `sudo systemctl stop
# esp32-log.service` ... flash ... `sudo systemctl start esp32-log.service`.
# See docs/agent-memory/project_deploy_test.md and tools/flash-esp32.sh.

PORT="${1:-/dev/ttyACM0}"
BAUD="${BAUD:-115200}"
LOG_DIR="${LOG_DIR:-/home/aziobro/logs}"
PREFIX="${PREFIX:-esp32p4}"

mkdir -p "$LOG_DIR"

STAMP="$(date +%Y-%m-%d-%H%M%S)"
LOG_FILE="$LOG_DIR/$PREFIX-$STAMP.log"
CURRENT_LINK="$LOG_DIR/$PREFIX-current.log"

{
    echo "=== $PREFIX serial capture started $STAMP ==="
    echo "port=$PORT baud=$BAUD"
    echo
} > "$LOG_FILE"

ln -sfn "$LOG_FILE" "$CURRENT_LINK"

stty -F "$PORT" "$BAUD" raw -echo -crtscts

exec cat "$PORT" >> "$LOG_FILE"
