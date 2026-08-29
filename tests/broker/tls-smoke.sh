#!/bin/sh
# broker/tls-smoke.sh — end-to-end check of the host mqtt_pub/mqtt_sub tools'
# TLS support (-s verify-on, -S skip-verify) against a real Mosquitto broker
# with a TLS listener (real MQTT wire protocol, real TLS handshake; just not
# on m68k - see tests/broker/smoke.sh for the plaintext equivalent).
#
# Usage: MQTT_PUB=build/mqtt_pub-host MQTT_SUB=build/mqtt_sub-host sh tests/broker/tls-smoke.sh
# (invoked via `make broker-tls-smoke`, which builds the tools first)
#
# Starts its own mosquitto on a scratch port so it doesn't collide with a
# broker already running on 1883/8883; needs `mosquitto` on PATH (apt/brew
# package `mosquitto`, or run the CI job which installs it) and `openssl` on
# PATH to generate a throwaway self-signed cert/key pair.
set -eu

MQTT_PUB="${MQTT_PUB:?set MQTT_PUB to the mqtt_pub binary}"
MQTT_SUB="${MQTT_SUB:?set MQTT_SUB to the mqtt_sub binary}"
PORT=18883
OUTDIR=$(mktemp -d)
MOSQ_PID=
trap 'kill "$MOSQ_PID" 2>/dev/null || true; rm -rf "$OUTDIR"' EXIT

command -v mosquitto >/dev/null || { echo "tls-smoke: mosquitto not on PATH" >&2; exit 2; }
command -v openssl >/dev/null || { echo "tls-smoke: openssl not on PATH" >&2; exit 2; }

# Throwaway self-signed cert/key pair. The SAN matters - the client verifies
# the hostname it connected with (SSL_set1_host), so the cert must cover
# "localhost", which round 1 below connects to.
openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$OUTDIR/server.key" -out "$OUTDIR/server.crt" -days 2 \
    -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
    2>/dev/null

cat > "$OUTDIR/mosquitto.conf" <<EOF
listener $PORT 127.0.0.1
allow_anonymous true
certfile $OUTDIR/server.crt
keyfile $OUTDIR/server.key
EOF

mosquitto -c "$OUTDIR/mosquitto.conf" > "$OUTDIR/mosquitto.log" 2>&1 &
MOSQ_PID=$!

# Wait for the listener to come up (mosquitto has no readiness signal of
# its own beyond its log line - "/dev/tcp/..." is a bash extension dash
# (Ubuntu's /bin/sh) doesn't support, so poll the log instead of a raw
# socket probe; portable everywhere this script's own shebang runs).
i=0
while ! grep -q "mosquitto version .* running" "$OUTDIR/mosquitto.log" 2>/dev/null; do
    i=$((i + 1))
    [ "$i" -ge 50 ] && { echo "tls-smoke: mosquitto did not start"; cat "$OUTDIR/mosquitto.log"; exit 1; }
    sleep 0.1
done

echo "--- TLS verify-on (-s), trusting the self-signed cert via SSL_CERT_FILE ---"
SSL_CERT_FILE="$OUTDIR/server.crt" "$MQTT_SUB" -h localhost -p "$PORT" -s -t midge/tls-smoke -C 1 > "$OUTDIR/sub-verify.out" &
SUB_PID=$!
sleep 0.3
SSL_CERT_FILE="$OUTDIR/server.crt" "$MQTT_PUB" -h localhost -p "$PORT" -s -t midge/tls-smoke -m "hello over tls"
wait "$SUB_PID"
grep -qF "hello over tls" "$OUTDIR/sub-verify.out" || { echo "tls-smoke: message not received over verified TLS"; cat "$OUTDIR/sub-verify.out"; exit 1; }
echo "ok: received \"$(cat "$OUTDIR/sub-verify.out")\""

echo "--- TLS skip-verify (-S), untrusted cert, no SSL_CERT_FILE ---"
"$MQTT_SUB" -h 127.0.0.1 -p "$PORT" -S -t midge/tls-smoke -C 1 > "$OUTDIR/sub-insecure.out" &
SUB_PID=$!
sleep 0.3
"$MQTT_PUB" -h 127.0.0.1 -p "$PORT" -S -t midge/tls-smoke -m "hello insecure"
wait "$SUB_PID"
grep -qF "hello insecure" "$OUTDIR/sub-insecure.out" || { echo "tls-smoke: message not received over insecure TLS"; cat "$OUTDIR/sub-insecure.out"; exit 1; }
echo "ok: received \"$(cat "$OUTDIR/sub-insecure.out")\""

echo "--- TLS verify-on (-s) with no trust anchor must fail ---"
if "$MQTT_PUB" -h localhost -p "$PORT" -s -t midge/tls-smoke -m "should not connect" 2>"$OUTDIR/pub-fail.out"; then
    echo "tls-smoke: mqtt_pub connected with -s and no SSL_CERT_FILE - verification is not actually on"; cat "$OUTDIR/pub-fail.out"; exit 1
fi
echo "ok: verify-on connect correctly rejected the untrusted cert"

echo "All broker TLS smoke checks passed."
