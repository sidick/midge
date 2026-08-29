#!/bin/sh
# tests/net/net-smoke.sh — on-target de-risk check: the cross-built,
# m68k mqtt_pub AND mqtt_pub-static both connect out over the real Amiga
# bsdsocket transport (src/amiga/transport_bsdsocket.c, either directly for
# the static build, or via mqtt.library's own subprocess for the default
# library-linked build) and reach a real Mosquitto listening on the host.
#
# Uses Copperline's bundled HostSocket board (`[hostsocket] net = "host"`,
# see machine.toml): bsdsocket.library for the guest, backed directly by
# real host OS sockets. No Kickstart ROM or Workbench image is needed (the
# library autoboots from the board's own ROM code on the bundled AROS
# Kickstart replacement, same as tests/copperline/), so unlike an initial
# design sketched against a real guest TCP/IP stack (Roadshow/AmiTCP), this
# needs no machine-specific assets and can run in CI - see
# .github/workflows/ci.yml's net-smoke job.
#
# Stages the boot volume the same way tests/copperline/run.sh does (a host
# directory mounted as an FFS volume, not Copperline's `--run` fast path -
# `--run` was observed to leave a bigger, multi-object binary like the real
# mqtt_pub never actually launched by its generated Startup-Sequence, for
# reasons not fully root-caused; this config-file approach is the one
# that's proven reliable here).
#
# Usage: sh tests/net/net-smoke.sh   (invoked via `make net-smoke`, which
# cross-builds mqtt_pub/mqtt_pub-static, mqtt.library, and the host
# mqtt_sub-host observer first)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

COPPERLINE=${COPPERLINE:-copperline}
BIN=${MQTT_PUB_M68K:-$ROOT/build/mqtt_pub}
BIN_STATIC=${MQTT_PUB_STATIC_M68K:-$ROOT/build/mqtt_pub-static}
LIB=${MQTT_LIBRARY_M68K:-$ROOT/build/mqtt.library}
MQTT_SUB=${MQTT_SUB:-$ROOT/build/mqtt_sub-host}
PORT=18831 # must match sys/S/Startup-Sequence
BENCH=${BENCH:-30}

command -v "$COPPERLINE" >/dev/null || { echo "net-smoke: $COPPERLINE not found" >&2; exit 2; }
command -v mosquitto >/dev/null || { echo "net-smoke: mosquitto not on PATH" >&2; exit 2; }
[ -x "$BIN" ] || { echo "net-smoke: $BIN not built - run 'make m68k' first" >&2; exit 2; }
[ -x "$BIN_STATIC" ] || { echo "net-smoke: $BIN_STATIC not built - run 'make m68k' first" >&2; exit 2; }
[ -e "$LIB" ] || { echo "net-smoke: $LIB not built - run 'make library' first" >&2; exit 2; }
[ -x "$MQTT_SUB" ] || { echo "net-smoke: $MQTT_SUB not built - run 'make cli' first" >&2; exit 2; }

mkdir -p "$HERE/sys/C" "$HERE/sys/Libs"
cp "$BIN" "$HERE/sys/C/mqtt_pub"
cp "$BIN_STATIC" "$HERE/sys/C/mqtt_pub-static"
cp "$LIB" "$HERE/sys/Libs/mqtt.library"

OUTDIR=$(mktemp -d)
MOSQ_PID=
trap 'kill "$MOSQ_PID" 2>/dev/null || true; rm -rf "$OUTDIR"' EXIT

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
    [ "$i" -ge 50 ] && { echo "net-smoke: mosquitto did not start"; cat "$OUTDIR/mosquitto.log"; exit 1; }
    sleep 0.1
done

"$MQTT_SUB" -h 127.0.0.1 -p "$PORT" -t midge/net-smoke -C 2 > "$OUTDIR/sub.out" &
SUB_PID=$!
sleep 0.3

# --benchmark-until bounds the run and makes copperline exit once both m68k
# tools (Startup-Sequence runs mqtt_pub-static then mqtt_pub, each of which
# connects, publishes, disconnects, and returns) have had time to finish.
# cd so `path = "sys"` in machine.toml resolves relative to this directory.
( cd "$HERE" && "$COPPERLINE" --config machine.toml --noaudio --serial stdout --benchmark-until "$BENCH" ) \
    > "$OUTDIR/copperline.log" 2>&1

wait "$SUB_PID" || true
grep -qF "hello-from-copperline-static" "$OUTDIR/sub.out" || {
    echo "net-smoke: static-build message not received"
    echo "--- copperline log ---"; cat "$OUTDIR/copperline.log"
    echo "--- mqtt_sub-host output ---"; cat "$OUTDIR/sub.out"
    exit 1
}
grep -qF "hello-from-copperline-lib" "$OUTDIR/sub.out" || {
    echo "net-smoke: library-linked message not received"
    echo "--- copperline log ---"; cat "$OUTDIR/copperline.log"
    echo "--- mqtt_sub-host output ---"; cat "$OUTDIR/sub.out"
    exit 1
}
echo "ok: on-target mqtt_pub-static and mqtt_pub (bsdsocket.library / mqtt.library, real m68k codepath) both reached the host broker via HostSocket"
