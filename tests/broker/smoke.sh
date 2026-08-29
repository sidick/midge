#!/bin/sh
# broker/smoke.sh — end-to-end check of the host mqtt_pub/mqtt_sub tools
# against a real Mosquitto broker (real MQTT wire protocol, real TCP; just
# not on m68k - see tests/copperline/ for the on-target codec check and
# tests/net/ for the on-target networking check).
#
# Usage: MQTT_PUB=build/mqtt_pub-host MQTT_SUB=build/mqtt_sub-host sh tests/broker/smoke.sh
# (invoked via `make broker-smoke`, which builds the tools first)
#
# Starts its own mosquitto on a scratch port so it doesn't collide with a
# broker already running on 1883; needs `mosquitto` on PATH (apt/brew
# package `mosquitto`, or run the CI job which installs it).
set -eu

MQTT_PUB="${MQTT_PUB:?set MQTT_PUB to the mqtt_pub binary}"
MQTT_SUB="${MQTT_SUB:?set MQTT_SUB to the mqtt_sub binary}"
PORT=18830
HOST=127.0.0.1
OUTDIR=$(mktemp -d)
MOSQ_PID=
trap 'kill "$MOSQ_PID" 2>/dev/null || true; rm -rf "$OUTDIR"' EXIT

command -v mosquitto >/dev/null || { echo "smoke: mosquitto not on PATH" >&2; exit 2; }

cat > "$OUTDIR/mosquitto.conf" <<EOF
listener $PORT $HOST
allow_anonymous true
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
    [ "$i" -ge 50 ] && { echo "smoke: mosquitto did not start"; cat "$OUTDIR/mosquitto.log"; exit 1; }
    sleep 0.1
done

echo "--- QoS 0 publish/subscribe ---"
"$MQTT_SUB" -h "$HOST" -p "$PORT" -t midge/smoke -C 1 > "$OUTDIR/sub.out" &
SUB_PID=$!
sleep 0.3
"$MQTT_PUB" -h "$HOST" -p "$PORT" -t midge/smoke -m "hello from midge"
wait "$SUB_PID"
grep -qF "hello from midge" "$OUTDIR/sub.out" || { echo "smoke: message not received"; cat "$OUTDIR/sub.out"; exit 1; }
echo "ok: received \"$(cat "$OUTDIR/sub.out")\""

echo "--- retained message ---"
"$MQTT_PUB" -h "$HOST" -p "$PORT" -t midge/smoke/retained -m "sticky" -r
sleep 0.2
"$MQTT_SUB" -h "$HOST" -p "$PORT" -t midge/smoke/retained -C 1 > "$OUTDIR/retained.out"
grep -qF "sticky" "$OUTDIR/retained.out" || { echo "smoke: retained message not received"; cat "$OUTDIR/retained.out"; exit 1; }
echo "ok: retained message delivered on subscribe"

echo "--- wildcard subscribe ---"
"$MQTT_SUB" -h "$HOST" -p "$PORT" -t "midge/smoke/#" -C 1 > "$OUTDIR/wild.out" &
SUB_PID=$!
sleep 0.3
"$MQTT_PUB" -h "$HOST" -p "$PORT" -t midge/smoke/wild -m "wildcard-ok"
wait "$SUB_PID"
grep -qF "wildcard-ok" "$OUTDIR/wild.out" || { echo "smoke: wildcard subscribe failed"; cat "$OUTDIR/wild.out"; exit 1; }
echo "ok: wildcard filter matched"

echo "All broker smoke checks passed."
