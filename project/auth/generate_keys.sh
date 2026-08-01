#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AUTH_DIR="$SCRIPT_DIR"

printf "=== tetriSH PKI generation ===\n"
echo "Output directory: $AUTH_DIR"

printf "\n[1/4] Generating CA private key (RSA-1024)...\n"
openssl genrsa -out "$AUTH_DIR/ca.key" 1024

printf "\n[2/4] Generating self-signed CA certificate (valid 10 years)...\n"
openssl req -new -x509 -key "$AUTH_DIR/ca.key" -out "$AUTH_DIR/cacsertificate.crt" -days 3650 -subj "/C=SG/O=SUTD/CN=50005-CA"
echo "CA cert: $AUTH_DIR/cacsertificate.crt"
echo "CA key: $AUTH_DIR/ca.key"

printf "\n[3/4] Generating server private key (RSA-1024) and CSR...\n"
openssl genrsa -out "$AUTH_DIR/server.key" 1024
openssl req -new -key "$AUTH_DIR/server.key" -out "$AUTH_DIR/server.csr" -subj "/C=SG/O=SUTD/CN=tetrish.local"
echo "Server key: $AUTH_DIR/server.key"
echo "Server CSR: $AUTH_DIR/server.csr"

printf "\n[4/4] CA signing the server CSR to produce server.crt...\n"
SERIAL_FILE="$AUTH_DIR/ca.srl"
if [ ! -f "$SERIAL_FILE" ]; then
    echo "01" > "$SERIAL_FILE"
fi

openssl x509 -req -in "$AUTH_DIR/server.csr" -CA "$AUTH_DIR/cacsertificate.crt" -CAkey "$AUTH_DIR/ca.key" -CAserial "$SERIAL_FILE" -out "$AUTH_DIR/server.crt" -days 365 -sha256
echo "Server cert: $AUTH_DIR/server.crt"

printf "\n=== Verifying chain ===\n"
openssl verify -CAfile "$AUTH_DIR/cacsertificate.crt" "$AUTH_DIR/server.crt"

printf "\n=== Certificate subject / issuer ===\n"
openssl x509 -in "$AUTH_DIR/server.crt" -noout -subject -issuer -dates -serial

echo ""
echo "=== Done. Files created ==="
ls -lh "$AUTH_DIR/"*.crt "$AUTH_DIR/"*.key "$AUTH_DIR/"*.csr 2>/dev/null || true