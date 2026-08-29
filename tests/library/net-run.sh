#!/bin/sh
# net-run.sh — headless Copperline end-to-end network test for mqtt.library
# (Phase 2 slice 2's real API surface). Modeled on ../net/net-smoke.sh
# (broker setup, HostSocket board, readiness polling) and ./run.sh (staging
# a boot volume, serial capture, PASS/FAIL/RESULT/END markers).
#
# Sequence:
#   1. start a scratch Mosquitto, wait for it to be listening
#   2. retained-publish RETAINED_PAYLOAD to TOPIC_IN (host mqtt_pub-host -r)
#      - so the on-target subscriber gets it immediately, no republish loop
#        needed (mqtt_pub-host does support -r; see src/host/args.c)
#   3. start a host mqtt_sub-host observer on TOPIC_OUT
#   4. boot Copperline (HostSocket board) running C:libnet, which
#      OpenLibrary("mqtt.library")s the freshly cross-built library staged
#      into netsys/Libs/, MQTT_CreateClient/Connect/Subscribe(TOPIC_IN)/
#      GetMessage(s)/Publish(TOPIC_OUT)/Disconnect/DeleteClient/CloseLibrary
#   5. assert both the guest's serial PASS/FAIL/RESULT/END markers AND that
#      the host observer actually saw OUT_PAYLOAD on TOPIC_OUT
#
# These constants must match tests/library/libnet.c's #defines exactly.
#
# Usage: sh tests/library/net-run.sh   (invoked via
# `make library-net-smoke`, which cross-builds libnet + mqtt.library first)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

COPPERLINE=${COPPERLINE:-copperline}
BIN=${LIBNET_M68K:-$ROOT/build/libnet}
LIB=${MQTT_LIBRARY_M68K:-$ROOT/build/mqtt.library}
MQTT_PUB=${MQTT_PUB:-$ROOT/build/mqtt_pub-host}
MQTT_SUB=${MQTT_SUB:-$ROOT/build/mqtt_sub-host}
PORT=18832 # must match libnet.c's TEST_PORT
TOPIC_IN=midge/lib/in
TOPIC_OUT=midge/lib/out
RETAINED_PAYLOAD=hello-from-host-retained
OUT_PAYLOAD=hello-from-mqtt-library
BENCH=${BENCH:-60} # emulated seconds: AROS boot (~10s) + up to ~20s poll budget

command -v "$COPPERLINE" >/dev/null || { echo "FAIL: $COPPERLINE not found" >&2; exit 2; }
command -v mosquitto >/dev/null || { echo "FAIL: mosquitto not on PATH" >&2; exit 2; }
[ -e "$BIN" ] || { echo "FAIL: missing $BIN - run 'make libnet-m68k' first" >&2; exit 2; }
[ -e "$LIB" ] || { echo "FAIL: missing $LIB - run 'make library' first" >&2; exit 2; }
[ -x "$MQTT_PUB" ] || { echo "FAIL: missing $MQTT_PUB - run 'make cli' first" >&2; exit 2; }
[ -x "$MQTT_SUB" ] || { echo "FAIL: missing $MQTT_SUB - run 'make cli' first" >&2; exit 2; }

mkdir -p "$HERE/netsys/C" "$HERE/netsys/Libs"
cp "$BIN" "$HERE/netsys/C/libnet"
cp "$LIB" "$HERE/netsys/Libs/mqtt.library"

OUTDIR=$(mktemp -d)
OUT=$(mktemp)
MOSQ_PID=
SUB_PID=
trap 'kill "$MOSQ_PID" 2>/dev/null || true; kill "$SUB_PID" 2>/dev/null || true; rm -f "$OUT"; rm -rf "$OUTDIR"' EXIT

cat > "$OUTDIR/mosquitto.conf" <<EOF
listener $PORT 127.0.0.1
allow_anonymous true
persistence false
EOF
mosquitto -c "$OUTDIR/mosquitto.conf" > "$OUTDIR/mosquitto.log" 2>&1 &
MOSQ_PID=$!

# mosquitto has no readiness signal of its own beyond its log line -
# /dev/tcp is a bashism dash (Ubuntu's /bin/sh, i.e. CI) doesn't support,
# so poll the log instead of a raw socket probe (see ../net/net-smoke.sh).
i=0
while ! grep -q "mosquitto version .* running" "$OUTDIR/mosquitto.log" 2>/dev/null; do
    i=$((i + 1))
    [ "$i" -ge 50 ] && { echo "FAIL: mosquitto did not start" >&2; cat "$OUTDIR/mosquitto.log" >&2; exit 1; }
    sleep 0.1
done

# Retained pre-publish: the guest's MQTT_Subscribe(TOPIC_IN) will get this
# delivered immediately as the broker's stored retained message, no
# republish loop needed.
"$MQTT_PUB" -h 127.0.0.1 -p "$PORT" -t "$TOPIC_IN" -m "$RETAINED_PAYLOAD" -r

"$MQTT_SUB" -h 127.0.0.1 -p "$PORT" -t "$TOPIC_OUT" -C 1 > "$OUTDIR/sub.out" 2>"$OUTDIR/sub.err" &
SUB_PID=$!
sleep 0.3

set -- --config net-machine.toml --noaudio --serial stdout --benchmark-until "$BENCH"
( cd "$HERE" && "$COPPERLINE" "$@" ) >"$OUT" 2>/dev/null \
    || { echo "FAIL: $COPPERLINE exited non-zero" >&2; cat "$OUT" >&2; exit 3; }

tr -d '\r' <"$OUT" >"$OUT.n" && mv "$OUT.n" "$OUT" # serial sends CRLF; drop CR
echo "----- serial capture -----"; cat "$OUT"; echo "--------------------------"

wait "$SUB_PID" || true
echo "----- host observer (topic $TOPIC_OUT) -----"; cat "$OUTDIR/sub.out"; echo "--------------------------"

grep -q '^END' "$OUT" 2>/dev/null || { echo "FAIL: no END marker (raise BENCH?)" >&2; exit 1; }
grep -q '^FAIL ' "$OUT" && { echo "FAIL: one or more mqtt.library checks failed on-target" >&2; exit 1; }
grep -q '^RESULT=OK$' "$OUT" || { echo "FAIL: no RESULT=OK marker" >&2; exit 1; }
grep -qF "$OUT_PAYLOAD" "$OUTDIR/sub.out" || {
    echo "FAIL: host observer never saw \"$OUT_PAYLOAD\" on $TOPIC_OUT" >&2
    exit 1
}

echo "PASS: mqtt.library's real API works end-to-end on 68020 (real bsdsocket.library codepath)"
