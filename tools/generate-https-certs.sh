#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
CERT_DIR="$ROOT_DIR/main/certs"
OPENSSL=${OPENSSL:-openssl}

# Load machine-specific SAN values (git-ignored). See config.sample.env.
# shellcheck disable=SC1091
[ -f "$ROOT_DIR/config.env" ] && . "$ROOT_DIR/config.env"
CERT_DNS=${CERT_DNS:-gps-base.local}
CERT_IP=${CERT_IP:-192.168.8.186}
CERT_AP_IP=${CERT_AP_IP:-192.168.4.1}

mkdir -p "$CERT_DIR"

"$OPENSSL" ecparam -name prime256v1 -genkey \
    -noout -out "$CERT_DIR/ca-key.pem"
"$OPENSSL" req -x509 -new -sha256 -days 3650 \
    -key "$CERT_DIR/ca-key.pem" \
    -out "$CERT_DIR/ca-cert.pem" \
    -subj "/CN=GPS Base Station Local CA/O=GPS Base Station"

"$OPENSSL" ecparam -name prime256v1 -genkey \
    -noout -out "$CERT_DIR/server-key.pem"
"$OPENSSL" req -new -sha256 \
    -key "$CERT_DIR/server-key.pem" \
    -out "$CERT_DIR/server.csr" \
    -subj "/CN=$CERT_DNS/O=GPS Base Station"

EXT_FILE="$CERT_DIR/server-ext.cnf"
printf '%s\n' \
    "basicConstraints=CA:FALSE" \
    "keyUsage=digitalSignature,keyEncipherment" \
    "extendedKeyUsage=serverAuth" \
    "subjectAltName=DNS:$CERT_DNS,IP:$CERT_IP,IP:$CERT_AP_IP" \
    > "$EXT_FILE"

"$OPENSSL" x509 -req -sha256 -days 1825 \
    -in "$CERT_DIR/server.csr" \
    -CA "$CERT_DIR/ca-cert.pem" \
    -CAkey "$CERT_DIR/ca-key.pem" \
    -CAcreateserial \
    -extfile "$EXT_FILE" \
    -out "$CERT_DIR/server-cert.pem"

rm -f "$CERT_DIR/server.csr" "$CERT_DIR/server-ext.cnf" \
    "$CERT_DIR/ca-cert.srl"
chmod 600 "$CERT_DIR/ca-key.pem" "$CERT_DIR/server-key.pem"
printf 'HTTPS certificates generated in %s\n' "$CERT_DIR"
