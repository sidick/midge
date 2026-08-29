#!/bin/sh
# reconn-run.sh — headless Copperline on-target test for mco_AutoReconnect
# (Phase 2 "reconnect/backoff + auto-resubscribe" milestone). Modeled on
# net-run.sh (broker setup, HostSocket board, readiness polling, PASS/FAIL/
# RESULT/END assertions) with one addition: this script kills and restarts
# the scratch Mosquitto broker mid-run, on the same port, to prove the
# guest's mqtt.library child subprocess notices the drop, backs off,
# reconnects, and automatically re-subscribes on its own.
#
# Sequence:
#   1. start a scratch Mosquitto ("phase1"), wait for it to be listening
#   2. retained-publish PHASE1_PAYLOAD to TOPIC_IN (host mqtt_pub-host -r)
#   3. boot Copperline (HostSocket board) IN THE BACKGROUND running
#      C:libreconn, which OpenLibrary("mqtt.library")s the freshly
#      cross-built library staged into reconnsys/Libs/, CreateClients with
#      mco_AutoReconnect=TRUE, Connects, Subscribes(TOPIC_IN), receives the
#      phase1 retained message, then prints a "MARK ready-for-outage" line
#      and starts polling MQTT_GetMessage() for a second message
#   4. poll the guest's serial capture for that marker
#   5. kill the phase1 Mosquitto, wait for it to actually die, start a
#      *fresh* Mosquitto ("phase2") on the same port, wait for it to be
#      listening, then retained-publish PHASE2_PAYLOAD to TOPIC_IN - the
#      guest can only see this if it reconnected and resubscribed after the
#      outage (the whole point: the broker holds no memory of phase1's
#      retained message or the guest's earlier subscription, and phase2's
#      retained publish only lands with a subscriber already attached if
#      the guest's own resubscribe won the race - which is exactly what
#      proves auto-resubscribe worked, whether it landed just before or
#      just after this retained publish)
#   6. wait for the Copperline background process to exit, then assert the
#      guest's serial PASS/FAIL/RESULT/END markers
#
# These constants must match tests/library/libreconn.c's #defines exactly.
#
# Note: the guest may attempt (and fail) reconnects while the broker is
# down between steps 5's kill and fresh start - that is the backoff path
# working as designed; a connection-refused there just schedules the next
# retry (see mqtt_funcs.c's child_reconnect_loop()), it is not a test
# failure.
#
# Strictly POSIX sh (no bashisms) - CI runs this under dash.
#
# Usage: sh tests/library/reconn-run.sh   (invoked via
# `make library-reconnect-smoke`, which cross-builds libreconn + mqtt.library
# first)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

COPPERLINE=${COPPERLINE:-copperline}
BIN=${LIBRECONN_M68K:-$ROOT/build/libreconn}
LIB=${MQTT_LIBRARY_M68K:-$ROOT/build/mqtt.library}
MQTT_PUB=${MQTT_PUB:-$ROOT/build/mqtt_pub-host}
PORT=18833 # must match libreconn.c's TEST_PORT (distinct from net-run.sh's 18832)
TOPIC_IN=midge/lib/reconn
PHASE1_PAYLOAD=reconn-phase1-before-outage
PHASE2_PAYLOAD=reconn-phase2-after-outage
# Emulated seconds: AROS boot (~10s) + phase1 poll (near-instant) + up to
# ~120s phase2 poll budget (libreconn.c's POLL2_TRIES/POLL2_TICKS) + margin
# for this script's own broker-restart choreography.
BENCH=${BENCH:-180}

command -v "$COPPERLINE" >/dev/null || { echo "FAIL: $COPPERLINE not found" >&2; exit 2; }
command -v mosquitto >/dev/null || { echo "FAIL: mosquitto not on PATH" >&2; exit 2; }
[ -e "$BIN" ] || { echo "FAIL: missing $BIN - run 'make libreconn-m68k' first" >&2; exit 2; }
[ -e "$LIB" ] || { echo "FAIL: missing $LIB - run 'make library' first" >&2; exit 2; }
[ -x "$MQTT_PUB" ] || { echo "FAIL: missing $MQTT_PUB - run 'make cli' first" >&2; exit 2; }

mkdir -p "$HERE/reconnsys/C" "$HERE/reconnsys/Libs"
cp "$BIN" "$HERE/reconnsys/C/libreconn"
cp "$LIB" "$HERE/reconnsys/Libs/mqtt.library"

OUTDIR=$(mktemp -d)
OUT="$OUTDIR/serial.out"
MOSQ_PID=
COP_PID=
trap 'kill "$MOSQ_PID" 2>/dev/null || true; kill "$COP_PID" 2>/dev/null || true; rm -rf "$OUTDIR"' EXIT

start_mosquitto() {
    # $1 = conf path, $2 = log path
    mosquitto -c "$1" > "$2" 2>&1 &
    MOSQ_PID=$!
    i=0
    while ! grep -q "mosquitto version .* running" "$2" 2>/dev/null; do
        i=$((i + 1))
        [ "$i" -ge 50 ] && { echo "FAIL: mosquitto did not start" >&2; cat "$2" >&2; exit 1; }
        sleep 0.1
    done
}

cat > "$OUTDIR/mosquitto1.conf" <<EOF
listener $PORT 127.0.0.1
allow_anonymous true
persistence false
EOF
start_mosquitto "$OUTDIR/mosquitto1.conf" "$OUTDIR/mosquitto1.log"

# Retained pre-publish: the guest's MQTT_Subscribe(TOPIC_IN) will get this
# delivered immediately as the broker's stored retained message, no
# republish loop needed - same as net-run.sh's own phase1.
"$MQTT_PUB" -h 127.0.0.1 -p "$PORT" -t "$TOPIC_IN" -m "$PHASE1_PAYLOAD" -r

set -- --config reconn-machine.toml --noaudio --serial stdout --benchmark-until "$BENCH"
( cd "$HERE" && exec "$COPPERLINE" "$@" ) >"$OUT" 2>"$OUTDIR/copperline.err" &
COP_PID=$!

# Poll the serial capture for the guest's "ready for outage" marker -
# printed only after it has subscribed and drained the phase1 retained
# message (see libreconn.c). Bounded ~60 tries at 0.5s = ~30s, generous
# next to the AROS boot + near-instant phase1 delivery.
i=0
while ! grep -q "MARK ready-for-outage" "$OUT" 2>/dev/null; do
    i=$((i + 1))
    if [ "$i" -ge 60 ]; then
        echo "FAIL: guest never reached ready-for-outage marker" >&2
        kill "$COP_PID" 2>/dev/null || true
        cat "$OUT" >&2
        exit 1
    fi
    if ! kill -0 "$COP_PID" 2>/dev/null; then
        echo "FAIL: $COPPERLINE exited before reaching ready-for-outage marker" >&2
        cat "$OUT" >&2
        cat "$OUTDIR/copperline.err" >&2
        exit 3
    fi
    sleep 0.5
done

# --- the outage: kill the phase1 broker, wait for it to actually die -----
kill "$MOSQ_PID" 2>/dev/null || true
i=0
while kill -0 "$MOSQ_PID" 2>/dev/null; do
    i=$((i + 1))
    [ "$i" -ge 50 ] && { echo "FAIL: phase1 mosquitto would not die" >&2; exit 1; }
    sleep 0.1
done
MOSQ_PID=

# --- fresh broker, same port: the guest's auto-reconnect must find this
# one on its own (no memory of the phase1 broker's state) --------------
cat > "$OUTDIR/mosquitto2.conf" <<EOF
listener $PORT 127.0.0.1
allow_anonymous true
persistence false
EOF
start_mosquitto "$OUTDIR/mosquitto2.conf" "$OUTDIR/mosquitto2.log"

"$MQTT_PUB" -h 127.0.0.1 -p "$PORT" -t "$TOPIC_IN" -m "$PHASE2_PAYLOAD" -r

wait "$COP_PID" || { echo "FAIL: $COPPERLINE exited non-zero" >&2; cat "$OUT" >&2; cat "$OUTDIR/copperline.err" >&2; exit 3; }
COP_PID=

tr -d '\r' <"$OUT" >"$OUT.n" && mv "$OUT.n" "$OUT" # serial sends CRLF; drop CR
echo "----- serial capture -----"; cat "$OUT"; echo "--------------------------"

grep -q '^END' "$OUT" 2>/dev/null || { echo "FAIL: no END marker (raise BENCH?)" >&2; exit 1; }
grep -q '^FAIL ' "$OUT" && { echo "FAIL: one or more mqtt.library checks failed on-target" >&2; exit 1; }
grep -q '^RESULT=OK$' "$OUT" || { echo "FAIL: no RESULT=OK marker" >&2; exit 1; }

echo "PASS: mqtt.library's mco_AutoReconnect works end-to-end on 68020 (backoff + auto-resubscribe across a real broker outage)"
