#!/usr/bin/env bash
set -euo pipefail

if [ -f "${IDF_PATH:-/opt/esp/idf}/export.sh" ]; then
  # shellcheck disable=SC1091
  source "${IDF_PATH:-/opt/esp/idf}/export.sh" >/dev/null
fi

git config --global --add safe.directory /work >/dev/null 2>&1 || true

exec "$@"
