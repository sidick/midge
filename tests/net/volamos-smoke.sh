#!/bin/sh
# tests/net/volamos-smoke.sh — fast local alternative to net-smoke.sh: runs
# the cross-built mqtt_pub under volamos (Simon's Rust API-level AmigaOS
# runtime, ~/src/volamos) instead of Copperline. volamos traps library
# calls directly at the API boundary rather than booting a full emulated
# machine, so this finishes in about a second instead of Copperline's
# ~10s AROS boot - a much tighter loop while iterating locally.
#
# Not a CI substitute for net-smoke.sh: volamos doesn't run real 68020
# instruction-level codegen through a booted Kickstart the way Copperline
# does, so it doesn't carry the same on-target confidence - keep using
# `make net-smoke` (Copperline) as the CI/release gate. This script is a
# faster first check while developing, or bisecting a networking bug.
#
# Usage: sh tests/net/volamos-smoke.sh   (invoked via `make volamos-smoke`,
# which cross-builds mqtt_pub and the host mqtt_sub-host observer first)
set -eu

BIN="${MQTT_PUB_M68K:-build/mqtt_pub}"
MQTT_SUB="${MQTT_SUB:-build/mqtt_sub-host}"
PORT=18832
OUTDIR=$(mktemp -d)
MOSQ_PID=
trap 'kill "$MOSQ_PID" 2>/dev/null || true; rm -rf "$OUTDIR"' EXIT

command -v volamos >/dev/null || { echo "volamos-smoke: volamos not on PATH" >&2; exit 2; }
command -v mosquitto >/dev/null || { echo "volamos-smoke: mosquitto not on PATH" >&2; exit 2; }
[ -x "$BIN" ] || { echo "volamos-smoke: $BIN not built - run 'make m68k' first" >&2; exit 2; }
[ -x "$MQTT_SUB" ] || { echo "volamos-smoke: $MQTT_SUB not built - run 'make cli' first" >&2; exit 2; }

cat > "$OUTDIR/mosquitto.conf" <<EOF
listener $PORT 127.0.0.1
allow_anonymous true
EOF
mosquitto -c "$OUTDIR/mosquitto.conf" > "$OUTDIR/mosquitto.log" 2>&1 &
MOSQ_PID=$!

# Wait for the listener (mosquitto has no readiness signal of its own
# beyond its log line - "/dev/tcp/..." is a bash extension dash (Ubuntu's
# /bin/sh) doesn't support, so poll the log instead of a raw socket probe).
i=0
while ! grep -q "mosquitto version .* running" "$OUTDIR/mosquitto.log" 2>/dev/null; do
    i=$((i + 1))
    [ "$i" -ge 50 ] && { echo "volamos-smoke: mosquitto did not start"; cat "$OUTDIR/mosquitto.log"; exit 1; }
    sleep 0.1
done

"$MQTT_SUB" -h 127.0.0.1 -p "$PORT" -t midge/volamos -C 1 > "$OUTDIR/sub.out" &
SUB_PID=$!
sleep 0.3

volamos --cpu 68020 --net "$BIN" \
    HOST 127.0.0.1 PORT "$PORT" TOPIC midge/volamos MESSAGE hello-from-volamos \
    > "$OUTDIR/volamos.log" 2>&1 || {
    echo "volamos-smoke: mqtt_pub exited non-zero" >&2
    cat "$OUTDIR/volamos.log" >&2
    exit 1
}

wait "$SUB_PID" || true
grep -qF "hello-from-volamos" "$OUTDIR/sub.out" || {
    echo "volamos-smoke: message not received"
    echo "--- mqtt_pub output ---"; cat "$OUTDIR/volamos.log"
    echo "--- mqtt_sub-host output ---"; cat "$OUTDIR/sub.out"
    exit 1
}
echo "ok: mqtt_pub reached the host broker under volamos"
