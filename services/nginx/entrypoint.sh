#!/bin/sh
# Generates a self-signed TLS certificate on first start, then starts nginx.
# The cert is stored in a named Docker volume and reused on subsequent starts.
# Set CERT_HOSTNAME env var to change the hostname (default: vulcan.local).
set -e

CERT=/etc/nginx/certs/selfsigned.crt
KEY=/etc/nginx/certs/selfsigned.key
HOSTNAME=${CERT_HOSTNAME:-vulcan.local}

if [ ! -f "$CERT" ] || [ ! -f "$KEY" ]; then
    echo "Generating self-signed TLS certificate for $HOSTNAME..."
    mkdir -p /etc/nginx/certs
    # Use a config file for SAN — LibreSSL (Alpine) doesn't support -addext
    cat > /tmp/openssl.cnf << EOF
[req]
distinguished_name = req_distinguished_name
x509_extensions = v3_ca
prompt = no
[req_distinguished_name]
CN = $HOSTNAME
[v3_ca]
subjectAltName = DNS:$HOSTNAME
EOF
    openssl req -x509 -nodes -days 3650 -newkey rsa:2048 \
        -keyout "$KEY" \
        -out    "$CERT" \
        -config /tmp/openssl.cnf
    echo "TLS certificate generated."
fi

exec "$@"
