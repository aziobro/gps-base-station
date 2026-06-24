#!/bin/sh
set -eu

PORT="${1:-/dev/ttyACM0}"
BAUD="${BAUD:-115200}"
LOG_DIR="${LOG_DIR:-/home/aziobro/logs}"
PREFIX="${PREFIX:-esp32p4}"

mkdir -p "$LOG_DIR"

# Stop any previous capture using this serial port.
pkill -f "[c]at $PORT" 2>/dev/null || true
sleep 1

STAMP="$(date +%Y-%m-%d-%H%M%S)"
LOG_FILE="$LOG_DIR/$PREFIX-$STAMP.log"
CURRENT_LINK="$LOG_DIR/$PREFIX-current.log"

{
    echo "=== $PREFIX serial capture started $STAMP ==="
    echo "port=$PORT baud=$BAUD"
    echo
} > "$LOG_FILE"

ln -sfn "$LOG_FILE" "$CURRENT_LINK"

stty -F "$PORT" "$BAUD" raw -echo -crtscts 2>/dev/null || true
nohup sh -c "
while true; do
    stty -F '$PORT' '$BAUD' raw -echo -crtscts 2>/dev/null || true
    cat '$PORT' >> '$LOG_FILE' 2>/dev/null
    sleep 1
done
" >/dev/null 2>&1 &

echo "started $LOG_FILE"
echo "current $CURRENT_LINK"
