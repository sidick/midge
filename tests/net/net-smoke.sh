#!/bin/sh
# tests/net/net-smoke.sh — on-target de-risk check: the cross-built,
# m68k mqtt_pub connects out over the real Amiga bsdsocket transport
# (src/amiga/transport_bsdsocket.c) and reaches a real Mosquitto listening
# on the host.
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
# cross-builds mqtt_pub and the host mqtt_sub-host observer first)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

COPPERLINE=${COPPERLINE:-copperline}
BIN=${MQTT_PUB_M68K:-$ROOT/build/mqtt_pub}
MQTT_SUB=${MQTT_SUB:-$ROOT/build/mqtt_sub-host}
PORT=18831 # must match sys/S/Startup-Sequence
BENCH=${BENCH:-30}

command -v "$COPPERLINE" >/dev/null || { echo "net-smoke: $COPPERLINE not found" >&2; exit 2; }
command -v mosquitto >/dev/null || { echo "net-smoke: mosquitto not on PATH" >&2; exit 2; }
[ -x "$BIN" ] || { echo "net-smoke: $BIN not built - run 'make m68k' first" >&2; exit 2; }
[ -x "$MQTT_SUB" ] || { echo "net-smoke: $MQTT_SUB not built - run 'make cli' first" >&2; exit 2; }

mkdir -p "$HERE/sys/C"
cp "$BIN" "$HERE/sys/C/mqtt_pub"

OUTDIR=$(mktemp -d)
MOSQ_PID=
trap 'kill "$MOSQ_PID" 2>/dev/null || true; rm -rf "$OUTDIR"' EXIT

cat > "$OUTDIR/mosquitto.conf" <<EOF
listener $PORT 127.0.0.1
allow_anonymous true
EOF
mosquitto -c "$OUTDIR/mosquitto.conf" > "$OUTDIR/mosquitto.log" 2>&1 &
MOSQ_PID=$!

i=0
while ! (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; do
    i=$((i + 1))
    [ "$i" -ge 50 ] && { echo "net-smoke: mosquitto did not start"; cat "$OUTDIR/mosquitto.log"; exit 1; }
    sleep 0.1
done
exec 3<&- 3>&-

"$MQTT_SUB" -h 127.0.0.1 -p "$PORT" -t midge/net-smoke -C 1 > "$OUTDIR/sub.out" &
SUB_PID=$!
sleep 0.3

# --benchmark-until bounds the run and makes copperline exit once the m68k
# mqtt_pub (which itself connects, publishes, disconnects, and returns) has
# had time to finish. cd so `path = "sys"` in machine.toml resolves
# relative to this directory.
( cd "$HERE" && "$COPPERLINE" --config machine.toml --noaudio --serial stdout --benchmark-until "$BENCH" ) \
    > "$OUTDIR/copperline.log" 2>&1

wait "$SUB_PID" || true
grep -qF "hello-from-copperline" "$OUTDIR/sub.out" || {
    echo "net-smoke: message not received"
    echo "--- copperline log ---"; cat "$OUTDIR/copperline.log"
    echo "--- mqtt_sub-host output ---"; cat "$OUTDIR/sub.out"
    exit 1
}
echo "ok: on-target mqtt_pub (bsdsocket.library, real m68k codepath) reached the host broker via HostSocket"
