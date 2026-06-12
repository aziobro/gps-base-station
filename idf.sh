#!/bin/bash
# Activates the ESP-IDF v6.0.1 environment and runs any idf.py command.
# Usage:
#   source idf.sh          — just activate the environment in your current shell
#   ./idf.sh build         — build
#   ./idf.sh flash monitor — flash and open serial monitor
#   ./idf.sh fullclean     — clean build artifacts

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXPORT="$SCRIPT_DIR/.tools/esp-idf-v6.0.1/export.sh"
IDF_PY="$SCRIPT_DIR/.tools/esp-idf-v6.0.1/tools/idf.py"

# Load machine-specific config (git-ignored). A real environment variable set
# by the caller takes precedence, so capture those before sourcing config.env
# and re-apply them afterwards. See config.sample.env for documentation.
_PORT_OVERRIDE="${PORT:-}"
_RISCV_OVERRIDE="${RISCV_TOOLCHAIN_BIN:-}"
[ -f "$SCRIPT_DIR/config.env" ] && source "$SCRIPT_DIR/config.env"
[ -n "$_PORT_OVERRIDE" ] && PORT="$_PORT_OVERRIDE"
[ -n "$_RISCV_OVERRIDE" ] && RISCV_TOOLCHAIN_BIN="$_RISCV_OVERRIDE"

# Built-in fallbacks keep the repo working even without a config.env.
RISCV_BIN="${RISCV_TOOLCHAIN_BIN:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin}"
PORT="${PORT:-/dev/tty.usbmodem5B140747221}"

if [ ! -f "$EXPORT" ]; then
    echo "ERROR: export.sh not found at $EXPORT"
    return 1 2>/dev/null || exit 1
fi

source "$EXPORT"

# Ensure the RISC-V toolchain is on PATH (needed when running as a subshell)
export PATH="$RISCV_BIN:$PATH"

# If arguments were passed, run idf.py with them
if [ $# -gt 0 ]; then
    python3 "$IDF_PY" -p "$PORT" "$@"
fi
