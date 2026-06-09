#!/usr/bin/env bash
#
# release.sh — one command to ship a firmware update and prove it landed.
#
# Workflow:
#   1. Bump the version (writes version.txt — the single source of truth).
#   2. Build the firmware (./idf.sh build).
#   3. Verify the freshly built binary embeds the target version.
#   4. Push it to the device: over USB (idf.sh flash) or over the air
#      (HTTPS POST /update).
#   5. Verify the running device reports the target version at /status.
#
# Usage:
#   tools/release.sh build [VERSION]
#       Bump + build + verify the binary. No device required. Great for CI.
#
#   tools/release.sh usb [VERSION]
#       Bump + build + flash over USB + verify the binary.
#       Set DEVICE_HOST to also confirm the live /status version after reboot.
#       Honors PORT (serial device, passed through to idf.sh).
#
#   tools/release.sh ota HOST [VERSION]
#       Bump + build + upload over the air to HOST + verify /status reports it.
#       Needs the admin password (ADMIN_PASSWORD env, or you'll be prompted).
#
# VERSION is optional. When omitted, the trailing integer of the current
# version is incremented (e.g. 2026.06.09-ota1 -> 2026.06.09-ota2).
#
# Environment:
#   ADMIN_PASSWORD   Admin password for OTA upload / live verification.
#   DEVICE_HOST      Hostname/IP to verify after a USB flash (optional).
#   PORT             Serial port for USB flashing (passed to idf.sh).
#   ADMIN_USER       Defaults to "admin".

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

VERSION_FILE="$REPO_ROOT/version.txt"
APP_BIN="$REPO_ROOT/build/gps_base_station.bin"
ADMIN_USER="${ADMIN_USER:-admin}"

die() { echo "ERROR: $*" >&2; exit 1; }
step() { echo; echo "==> $*"; }

usage() {
    sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

# --- argument parsing -------------------------------------------------------
MODE="${1:-}"
[ -n "$MODE" ] || usage 1
shift || true

OTA_HOST=""
if [ "$MODE" = "ota" ]; then
    OTA_HOST="${1:-}"
    [ -n "$OTA_HOST" ] || die "ota mode requires a HOST argument"
    shift || true
fi

EXPLICIT_VERSION="${1:-}"

case "$MODE" in
    build|usb|ota) ;;
    -h|--help|help) usage 0 ;;
    *) die "unknown mode '$MODE' (expected build, usb, or ota)" ;;
esac

# --- determine the target version ------------------------------------------
current_version() {
    [ -f "$VERSION_FILE" ] && head -n1 "$VERSION_FILE" | tr -d '[:space:]'
}

bump_version() {
    local cur="$1"
    if [[ "$cur" =~ ^(.*[^0-9])([0-9]+)$ ]]; then
        printf '%s%d' "${BASH_REMATCH[1]}" "$(( BASH_REMATCH[2] + 1 ))"
    elif [[ "$cur" =~ ^([0-9]+)$ ]]; then
        printf '%d' "$(( cur + 1 ))"
    else
        printf '%s-1' "$cur"
    fi
}

CUR_VERSION="$(current_version || true)"
if [ -n "$EXPLICIT_VERSION" ]; then
    TARGET_VERSION="$EXPLICIT_VERSION"
elif [ -n "$CUR_VERSION" ]; then
    TARGET_VERSION="$(bump_version "$CUR_VERSION")"
else
    die "no version.txt and no explicit VERSION given"
fi

# The app descriptor caps version at 31 chars (+ NUL).
[ "${#TARGET_VERSION}" -le 31 ] || die "version '$TARGET_VERSION' exceeds 31 chars"

step "Version: ${CUR_VERSION:-<none>} -> $TARGET_VERSION"
printf '%s\n' "$TARGET_VERSION" > "$VERSION_FILE"

# --- build ------------------------------------------------------------------
step "Building firmware"
./idf.sh build

[ -f "$APP_BIN" ] || die "build did not produce $APP_BIN"

# --- verify the binary embeds the target version ---------------------------
step "Verifying built binary"
BIN_VERSION="$(python3 "$REPO_ROOT/tools/fw_version.py" "$APP_BIN")"
echo "    binary reports: $BIN_VERSION"
[ "$BIN_VERSION" = "$TARGET_VERSION" ] \
    || die "binary version '$BIN_VERSION' != target '$TARGET_VERSION'"
echo "    OK — binary matches target version"

# --- helpers for live (on-device) verification -----------------------------
require_password() {
    if [ -z "${ADMIN_PASSWORD:-}" ]; then
        read -rsp "Admin password for $ADMIN_USER: " ADMIN_PASSWORD; echo
    fi
    [ -n "${ADMIN_PASSWORD:-}" ] || die "admin password required"
}

device_version() {
    # Prints the version field from /status, or nothing on failure.
    local host="$1"
    curl -k -s -m 8 -u "$ADMIN_USER:$ADMIN_PASSWORD" \
        "https://$host/status" 2>/dev/null \
        | python3 -c 'import sys,json
try: print(json.load(sys.stdin).get("version",""))
except Exception: pass'
}

verify_live() {
    local host="$1"
    step "Verifying live device at $host (waiting for reboot)"
    local deadline=$(( SECONDS + 90 )) seen=""
    while [ "$SECONDS" -lt "$deadline" ]; do
        seen="$(device_version "$host" || true)"
        if [ "$seen" = "$TARGET_VERSION" ]; then
            echo "    OK — device reports $seen"
            return 0
        fi
        [ -n "$seen" ] && echo "    device reports '$seen' (want '$TARGET_VERSION'), retrying…"
        sleep 5
    done
    die "device never reported '$TARGET_VERSION' (last saw: '${seen:-<unreachable>}')"
}

# --- push -------------------------------------------------------------------
case "$MODE" in
    build)
        step "Done (build-only). version.txt = $TARGET_VERSION"
        ;;

    usb)
        step "Flashing over USB"
        ./idf.sh flash
        if [ -n "${DEVICE_HOST:-}" ]; then
            require_password
            verify_live "$DEVICE_HOST"
        else
            echo
            echo "    Flashed. Set DEVICE_HOST=<ip-or-host> to auto-verify /status,"
            echo "    or open https://<device>/ and confirm the version reads"
            echo "    '$TARGET_VERSION'."
        fi
        ;;

    ota)
        require_password
        # The device suspends its RTCM streams and reboots on a successful
        # upload, so the HTTP response can be dropped as the socket closes —
        # and a busy base station may reject the first attempt under load.
        # Retry, but check /status first so we never re-upload a build the
        # device already took.
        for attempt in 1 2 3; do
            step "Uploading over the air to $OTA_HOST (attempt $attempt)"
            RESPONSE="$(curl -k -s -m 600 \
                -u "$ADMIN_USER:$ADMIN_PASSWORD" \
                -H 'Content-Type: application/octet-stream' \
                --data-binary "@$APP_BIN" \
                "https://$OTA_HOST/update" || true)"
            if printf '%s' "$RESPONSE" | grep -q "Update accepted"; then
                echo "    device: $RESPONSE"
                break
            fi
            echo "    upload did not confirm (response: '${RESPONSE:-<none>}')"
            sleep 6
            if [ "$(device_version "$OTA_HOST" || true)" = "$TARGET_VERSION" ]; then
                echo "    device already reports $TARGET_VERSION — upload took"
                break
            fi
            [ "$attempt" -lt 3 ] && echo "    retrying…"
        done
        # verify_live is authoritative: it polls /status and fails if the
        # device never reports the target version.
        verify_live "$OTA_HOST"
        ;;
esac

step "Release $TARGET_VERSION complete"
