#!/bin/sh
# tls-run.sh — on-target Copperline end-to-end TLS test for mqtt.library's
# mco_TLS/mco_TLSInsecure (issue #3 Phase 3). Modeled on net-run.sh (broker
# setup, staging, serial capture, PASS/FAIL/RESULT/END markers) but, unlike
# every other tests/library/*-run.sh, this one is LOCAL-ONLY and NOT a
# CI/release gate - see tests/library/README.md's "TLS smoke test" section
# for the full explanation. In short: AmiSSL needs a real AmigaOS
# Devs:/Libs: install (its own recipe assigns AmiSSL: to SYS:Devs/AmiSSL),
# which the bundled-AROS/HostSocket-only boot every other library-*-smoke
# target uses doesn't have - so this test boots a real AmigaOS 3.2.2 +
# AmiSSL image built by amibake (github.com/sidick/amibake) instead, via a
# real Kickstart ROM, both of which are local developer assets, not
# something CI can be given.
#
# Sequence (matching libtls.c's PORT/topic/payload constants exactly):
#   1. start a scratch TLS Mosquitto (self-signed cert), wait for it
#   2. retained-publish RETAINED_PAYLOAD to TOPIC_IN over TLS (host
#      mqtt_pub-host -s -S -r)
#   3. boot Copperline running C:libtls (staged into the amibake image,
#      alongside a fresh S:User-Startup that does the AmiSSL: assign a
#      real OS boot doesn't inject on its own - see amissl-spike/run.sh's
#      own comment on this same trap) - libtls OpenLibrary("mqtt.library"),
#      MQTT_CreateClient(mco_TLS=TRUE, mco_TLSInsecure=TRUE)/Connect/
#      Subscribe(TOPIC_IN)/GetMessage/Publish(TOPIC_OUT)/Disconnect/
#      DeleteClient/CloseLibrary
#   4. assert the guest's serial PASS/FAIL/RESULT/END markers AND that a
#      host TLS observer actually saw OUT_PAYLOAD on TOPIC_OUT
#
# Usage: sh tests/library/tls-run.sh   (invoked via `make library-tls-smoke`,
# which cross-builds libtls + mqtt.library - with AmiSSL support, see
# `make fetch-amissl-sdk` - first)
#
# Environment:
#   MIDGE_TLS_AMIGA_IMAGE  amibake-built image dir (default
#                          $HOME/src/amibake/manifests/AmiSSLSpike)
#   MIDGE_TLS_KICKSTART    Kickstart ROM path (default
#                          $HOME/src/amibake/assets/roms/kickstart-47.7.rom)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

COPPERLINE=${COPPERLINE:-copperline}
BIN=${LIBTLS_M68K:-$ROOT/build/libtls}
LIB=${MQTT_LIBRARY_M68K:-$ROOT/build/mqtt.library}
MQTT_PUB=${MQTT_PUB:-$ROOT/build/mqtt_pub-host}
IMAGE=${MIDGE_TLS_AMIGA_IMAGE:-$HOME/src/amibake/manifests/AmiSSLSpike}
ROM=${MIDGE_TLS_KICKSTART:-$HOME/src/amibake/assets/roms/kickstart-47.7.rom}
PORT=18884 # must match libtls.c's TEST_PORT
TOPIC_IN=midge/lib/tls/in
TOPIC_OUT=midge/lib/tls/out
RETAINED_PAYLOAD=hello-from-host-retained-tls
OUT_PAYLOAD=hello-from-mqtt-library-tls
BENCH=${BENCH:-300} # a real OS3.2.2 boot costs much more than AROS's ~10s

command -v "$COPPERLINE" >/dev/null || { echo "FAIL: $COPPERLINE not found" >&2; exit 2; }
command -v mosquitto >/dev/null || { echo "FAIL: mosquitto not on PATH" >&2; exit 2; }
command -v openssl >/dev/null || { echo "FAIL: openssl not on PATH" >&2; exit 2; }
[ -e "$BIN" ] || { echo "FAIL: missing $BIN - run 'make libtls-m68k' first" >&2; exit 2; }
[ -e "$LIB" ] || { echo "FAIL: missing $LIB - run 'make fetch-amissl-sdk && make library' first" >&2; exit 2; }
[ -x "$MQTT_PUB" ] || { echo "FAIL: missing $MQTT_PUB - run 'make cli' first" >&2; exit 2; }
[ -d "$IMAGE" ] || {
    echo "FAIL: MIDGE_TLS_AMIGA_IMAGE=$IMAGE not found - see" >&2
    echo "  tests/library/README.md's \"TLS smoke test\" section to build one" >&2
    exit 2
}
[ -f "$ROM" ] || { echo "FAIL: MIDGE_TLS_KICKSTART=$ROM not found" >&2; exit 2; }

cp "$BIN" "$IMAGE/C/libtls"
cp "$LIB" "$IMAGE/Libs/mqtt.library"
cat > "$IMAGE/S/User-Startup" <<'EOF'
; Written by tests/library/tls-run.sh on every run - see that script.
Assign AmiSSL: SYS:Devs/AmiSSL
libtls
echo "LIBTLS: ALL-DONE"
EOF

OUTDIR=$(mktemp -d)
MACHINE_TOML="$OUTDIR/tls-machine.toml"
OUT=$(mktemp)
MOSQ_PID=
trap 'kill "$MOSQ_PID" 2>/dev/null || true; rm -f "$OUT"; rm -rf "$OUTDIR"' EXIT

cat > "$MACHINE_TOML" <<EOF
rom = "$ROM"

[cpu]
model = "68020"

[memory]
chip = "1M"
fast = "8M"

[floppy]
drives = 1

[[filesys]]
path = "$IMAGE"
volume = "$(basename "$IMAGE")"
bootpri = 10

[hostsocket]
net = "host"

[emulation]
power_on = true
warp_speed = "max"
EOF

openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$OUTDIR/server.key" -out "$OUTDIR/server.crt" -days 2 \
    -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
    2>/dev/null

cat > "$OUTDIR/mosquitto.conf" <<EOF
listener $PORT 127.0.0.1
allow_anonymous true
persistence false
certfile $OUTDIR/server.crt
keyfile $OUTDIR/server.key
EOF
mosquitto -c "$OUTDIR/mosquitto.conf" > "$OUTDIR/mosquitto.log" 2>&1 &
MOSQ_PID=$!

i=0
while ! grep -q "mosquitto version .* running" "$OUTDIR/mosquitto.log" 2>/dev/null; do
    i=$((i + 1))
    [ "$i" -ge 50 ] && { echo "FAIL: mosquitto did not start" >&2; cat "$OUTDIR/mosquitto.log" >&2; exit 1; }
    sleep 0.1
done

"$MQTT_PUB" -h 127.0.0.1 -p "$PORT" -S -t "$TOPIC_IN" -m "$RETAINED_PAYLOAD" -r

mosquitto_sub -h 127.0.0.1 -p "$PORT" --cafile "$OUTDIR/server.crt" \
    -t "$TOPIC_OUT" -C 1 -W "$BENCH" > "$OUTDIR/sub.out" 2>"$OUTDIR/sub.err" &
SUB_PID=$!

set -- --config "$MACHINE_TOML" --noaudio --serial stdout --benchmark-until "$BENCH"
( cd "$IMAGE" && "$COPPERLINE" "$@" ) >"$OUT" 2>/dev/null \
    || { echo "FAIL: $COPPERLINE exited non-zero" >&2; cat "$OUT" >&2; exit 3; }

tr -d '\r' <"$OUT" >"$OUT.n" && mv "$OUT.n" "$OUT"
echo "----- serial capture -----"; cat "$OUT"; echo "--------------------------"

wait "$SUB_PID" || true
echo "----- host TLS observer (topic $TOPIC_OUT) -----"; cat "$OUTDIR/sub.out"; echo "--------------------------"

grep -q '^END' "$OUT" 2>/dev/null || { echo "FAIL: no END marker (raise BENCH?)" >&2; exit 1; }
grep -q '^FAIL ' "$OUT" && { echo "FAIL: one or more mqtt.library TLS checks failed on-target" >&2; exit 1; }
grep -q '^RESULT=OK$' "$OUT" || { echo "FAIL: no RESULT=OK marker" >&2; exit 1; }
grep -qF "$OUT_PAYLOAD" "$OUTDIR/sub.out" || {
    echo "FAIL: host TLS observer never saw \"$OUT_PAYLOAD\" on $TOPIC_OUT" >&2
    exit 1
}

echo "PASS: mqtt.library's mco_TLS works end-to-end on 68020 (real AmiSSL codepath)"
