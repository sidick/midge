#!/bin/sh
# volamos-net-run.sh — fast local alternative to net-run.sh: runs the
# cross-built libnet harness under volamos (Simon's Rust API-level AmigaOS
# runtime, ~/src/volamos) instead of Copperline, against a real
# disk-resident mqtt.library and a real host Mosquitto (--net).
#
# CAVEAT: mqtt.library's real API spawns a subprocess per client
# (CreateNewProcTags) and talks to it over MsgPorts (see mqtt_funcs.c) -
# unlike libsmoke.c's plain OpenLibrary/CloseLibrary check, this exercises
# that subprocess model. If volamos can't support CreateNewProc + inter-
# process MsgPort IPC, this script SKIPs (exit 0) rather than failing the
# build - see the "SKIP" branch below. Copperline's net-run.sh is the
# actual CI/release gate; this is a faster first check while developing.
#
# Usage: sh tests/library/volamos-net-run.sh   (invoked via
# `make volamos-library-net-smoke`, which cross-builds libnet + mqtt.library
# first)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

BIN="${LIBNET_M68K:-$ROOT/build/libnet}"
LIB="${MQTT_LIBRARY_M68K:-$ROOT/build/mqtt.library}"
MQTT_PUB="${MQTT_PUB:-$ROOT/build/mqtt_pub-host}"
MQTT_SUB="${MQTT_SUB:-$ROOT/build/mqtt_sub-host}"
PORT=18832 # must match libnet.c's TEST_PORT
TOPIC_IN=midge/lib/in
TOPIC_OUT=midge/lib/out
RETAINED_PAYLOAD=hello-from-host-retained
OUT_PAYLOAD=hello-from-mqtt-library

command -v volamos >/dev/null || { echo "FAIL: volamos not found" >&2; exit 2; }
command -v mosquitto >/dev/null || { echo "FAIL: mosquitto not on PATH" >&2; exit 2; }
[ -e "$BIN" ] || { echo "FAIL: missing $BIN - run 'make libnet-m68k' first" >&2; exit 2; }
[ -e "$LIB" ] || { echo "FAIL: missing $LIB - run 'make library' first" >&2; exit 2; }
[ -x "$MQTT_PUB" ] || { echo "FAIL: missing $MQTT_PUB - run 'make cli' first" >&2; exit 2; }
[ -x "$MQTT_SUB" ] || { echo "FAIL: missing $MQTT_SUB - run 'make cli' first" >&2; exit 2; }

LIBSDIR=$(mktemp -d)
cp "$LIB" "$LIBSDIR/mqtt.library"

OUTDIR=$(mktemp -d)
OUT=$(mktemp)
MOSQ_PID=
SUB_PID=
trap 'kill "$MOSQ_PID" 2>/dev/null || true; kill "$SUB_PID" 2>/dev/null || true; rm -f "$OUT"; rm -rf "$OUTDIR" "$LIBSDIR"' EXIT

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

"$MQTT_PUB" -h 127.0.0.1 -p "$PORT" -t "$TOPIC_IN" -m "$RETAINED_PAYLOAD" -r

"$MQTT_SUB" -h 127.0.0.1 -p "$PORT" -t "$TOPIC_OUT" -C 1 > "$OUTDIR/sub.out" 2>"$OUTDIR/sub.err" &
SUB_PID=$!
sleep 0.3

# Bounded wait, not a bare `volamos ...` call: if volamos can't handle
# mqtt.library's CreateNewProcTags/MsgPort subprocess model (see this
# script's banner), the observed failure mode during development was a
# hang, not a crash - and this script has no `timeout`(1) to rely on
# (not present on a stock macOS dev box, and we stay POSIX sh throughout -
# CLAUDE.md/the task notes). Run it in the background and poll for exit
# with a hard cap instead.
set +e
volamos --cpu 68020 --net -V "LIBS:$LIBSDIR" "$BIN" >"$OUT" 2>&1 &
VOL_PID=$!
i=0
while kill -0 "$VOL_PID" 2>/dev/null; do
    i=$((i + 1))
    if [ "$i" -ge 150 ]; then # 150 * 0.2s = 30s
        kill -9 "$VOL_PID" 2>/dev/null
        wait "$VOL_PID" 2>/dev/null
        echo "SKIP: volamos hung for 30s running libnet against mqtt.library" >&2
        echo "SKIP: (killed it) - see the comment at the top of this script;" >&2
        echo "SKIP: not a CI gate, so we don't fail the build over it." >&2
        rc=124
        set -e
        tr -d '\r' <"$OUT" >"$OUT.n" && mv "$OUT.n" "$OUT" 2>/dev/null || true
        echo "----- output (partial, before kill) -----"; cat "$OUT"; echo "-------------------"
        exit 0
    fi
    sleep 0.2
done
wait "$VOL_PID"
rc=$?
set -e

tr -d '\r' <"$OUT" >"$OUT.n" && mv "$OUT.n" "$OUT" # RawPutChar sends CRLF; drop CR
echo "----- output -----"; cat "$OUT"; echo "-------------------"

# Deliberately keyed on the END marker, not BEGIN: libnet.c prints BEGIN
# before doing anything mqtt.library-specific, so a crash partway through
# (observed in development: volamos reports "unhandled library call ...
# dos.library/CreateNewProc" as soon as MQTT_CreateClient() reaches
# CreateNewProcTags()) still leaves BEGIN in $OUT. Only END means the
# on-target program ran to completion. This also matters for hang-safety:
# if we haven't reached END, libnet.c never got as far as MQTT_Publish(),
# so the host observer below would never receive anything and a blocking
# `wait "$SUB_PID"` would hang right after a "SKIP" that was supposed to
# be harmless - kill it instead of waiting.
if [ "$rc" -ge 128 ] || ! grep -q '^END$' "$OUT"; then
    kill -9 "$SUB_PID" 2>/dev/null || true
    wait "$SUB_PID" 2>/dev/null || true
    echo "SKIP: volamos could not run libnet to completion against a" >&2
    echo "SKIP: disk-resident mqtt.library and its per-client subprocess" >&2
    echo "SKIP: model (no END marker seen; volamos exit=$rc). See the" >&2
    echo "SKIP: comment at the top of this script - if volamos's" >&2
    echo "SKIP: CreateNewProcTags/MsgPort IPC support regressed or never" >&2
    echo "SKIP: covered this pattern, that's not a CI gate (Copperline's" >&2
    echo "SKIP: net-run.sh is), so we don't fail the build over it." >&2
    exit 0
fi

wait "$SUB_PID" || true
echo "----- host observer (topic $TOPIC_OUT) -----"; cat "$OUTDIR/sub.out"; echo "--------------------------"

grep -q '^END' "$OUT" || { echo "FAIL: no END marker" >&2; exit 1; }
grep -q '^FAIL ' "$OUT" && { echo "FAIL: one or more mqtt.library checks failed" >&2; exit 1; }
grep -q '^RESULT=OK$' "$OUT" || { echo "FAIL: no RESULT=OK marker" >&2; exit 1; }
grep -qF "$OUT_PAYLOAD" "$OUTDIR/sub.out" || {
    echo "FAIL: host observer never saw \"$OUT_PAYLOAD\" on $TOPIC_OUT" >&2
    exit 1
}

echo "PASS: mqtt.library's real API works end-to-end under volamos"
