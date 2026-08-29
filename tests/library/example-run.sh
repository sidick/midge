#!/bin/sh
# example-run.sh — headless Copperline end-to-end check that the shipped
# mqtt.library example program (examples/pubexample.c) actually works: a
# real third-party-style caller, built only against the sfdc-generated
# caller-side headers (see the Makefile's `examples` target), publishing to
# a real broker over Copperline's HostSocket board. Modeled on
# ../net/net-smoke.sh and ./net-run.sh (broker setup, HostSocket board,
# readiness polling, host observer).
#
# Sequence:
#   1. start a scratch Mosquitto, wait for it to be listening
#   2. start a host mqtt_sub-host observer on TOPIC
#   3. boot Copperline (HostSocket board) running C:pubexample against the
#      freshly cross-built mqtt.library staged into examplesys/Libs/ (see
#      examplesys/S/Startup-Sequence for the exact args)
#   4. assert the host observer actually saw MESSAGE on TOPIC
#
# pubexample is a normal stdio CLI program (not one of the project's own
# RawPutChar-based on-target tests), so there is no serial PASS/FAIL/
# RESULT/END contract to assert here beyond "did the broker see the
# publish" - Copperline exiting non-zero or the message never arriving are
# both treated as failure.
#
# These constants must match examplesys/S/Startup-Sequence's own args.
#
# Usage: sh tests/library/example-run.sh   (invoked via `make
# example-smoke`, which cross-builds pubexample + mqtt.library first)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

COPPERLINE=${COPPERLINE:-copperline}
BIN=${PUBEXAMPLE_M68K:-$ROOT/build/pubexample}
LIB=${MQTT_LIBRARY_M68K:-$ROOT/build/mqtt.library}
MQTT_SUB=${MQTT_SUB:-$ROOT/build/mqtt_sub-host}
PORT=18833 # must match examplesys/S/Startup-Sequence
TOPIC=midge/example/pub
MESSAGE=hello-from-pubexample
BENCH=${BENCH:-40} # emulated seconds: AROS boot (~10s) + connect/publish

command -v "$COPPERLINE" >/dev/null || { echo "FAIL: $COPPERLINE not found" >&2; exit 2; }
command -v mosquitto >/dev/null || { echo "FAIL: mosquitto not on PATH" >&2; exit 2; }
[ -e "$BIN" ] || { echo "FAIL: missing $BIN - run 'make examples' first" >&2; exit 2; }
[ -e "$LIB" ] || { echo "FAIL: missing $LIB - run 'make library' first" >&2; exit 2; }
[ -x "$MQTT_SUB" ] || { echo "FAIL: missing $MQTT_SUB - run 'make cli' first" >&2; exit 2; }

mkdir -p "$HERE/examplesys/C" "$HERE/examplesys/Libs"
cp "$BIN" "$HERE/examplesys/C/pubexample"
cp "$LIB" "$HERE/examplesys/Libs/mqtt.library"

OUTDIR=$(mktemp -d)
MOSQ_PID=
SUB_PID=
trap 'kill "$MOSQ_PID" 2>/dev/null || true; kill "$SUB_PID" 2>/dev/null || true; rm -rf "$OUTDIR"' EXIT

cat > "$OUTDIR/mosquitto.conf" <<EOF
listener $PORT 127.0.0.1
allow_anonymous true
persistence false
EOF
mosquitto -c "$OUTDIR/mosquitto.conf" > "$OUTDIR/mosquitto.log" 2>&1 &
MOSQ_PID=$!

i=0
while ! grep -q "mosquitto version .* running" "$OUTDIR/mosquitto.log" 2>/dev/null; do
    i=$((i + 1))
    [ "$i" -ge 50 ] && { echo "FAIL: mosquitto did not start" >&2; cat "$OUTDIR/mosquitto.log" >&2; exit 1; }
    sleep 0.1
done

"$MQTT_SUB" -h 127.0.0.1 -p "$PORT" -t "$TOPIC" -C 1 > "$OUTDIR/sub.out" 2>"$OUTDIR/sub.err" &
SUB_PID=$!
sleep 0.3

( cd "$HERE" && "$COPPERLINE" --config example-machine.toml --noaudio --serial stdout --benchmark-until "$BENCH" ) \
    > "$OUTDIR/copperline.log" 2>&1 \
    || { echo "FAIL: $COPPERLINE exited non-zero" >&2; cat "$OUTDIR/copperline.log" >&2; exit 3; }

wait "$SUB_PID" || true
echo "----- host observer (topic $TOPIC) -----"; cat "$OUTDIR/sub.out"; echo "--------------------------"

grep -qF "$MESSAGE" "$OUTDIR/sub.out" || {
    echo "FAIL: host observer never saw \"$MESSAGE\" on $TOPIC" >&2
    echo "--- copperline log ---" >&2; cat "$OUTDIR/copperline.log" >&2
    exit 1
}

echo "PASS: examples/pubexample.c (real third-party-style mqtt.library caller) publishes end-to-end on 68020"
