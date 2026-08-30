#!/bin/sh
# cafile-run.sh — on-target Copperline end-to-end test for mqtt.library's
# mco_CAFile (issue #13). Same local-only/amibake-image requirement as
# tls-run.sh (see tests/library/README.md's "TLS smoke test" section) plus
# one more: it seeds Copperline's RTC to the host's real wall-clock time
# via --rtc-time.
#
# THAT MATTERS: Copperline's guest clock otherwise starts unseeded, and
# certificate-date verification (mco_CAFile, unlike mco_TLSInsecure, does
# real X.509 chain verification including validity dates) checks the
# broker cert's notBefore/notAfter against the Amiga's own idea of "now" -
# an unseeded/wrong clock makes a perfectly good just-issued certificate
# look not-yet-valid (SSL_get_verify_result() == X509_V_ERR_CERT_NOT_YET_
# VALID == 9) and the handshake fails for a reason that has nothing to do
# with mco_CAFile itself. This is also true on real hardware with a dead
# or unset battery-backed RTC - see userdocs/CLI-Reference.md's "A note on
# TLS and the system clock".
#
# Even seeded, a few hours of skew (timezone/DST handling somewhere in the
# AmigaOS/AmiSSL boot-to-clock path - not pinned down, not worth chasing
# further for a test harness) was enough to make a cert issued moments
# before the guest even booted look not-yet-valid. The certs below are
# backdated a day for comfortable margin against that, rather than issued
# at literally "now".
#
# Sequence:
#   1. generate a private CA + a broker cert signed by it (openssl)
#   2. start a scratch Mosquitto using that cert
#   3. boot Copperline (RTC seeded to `date +%s`) running C:libcafile,
#      staged into the amibake image alongside a fresh S:User-Startup
#      that does the AmiSSL: assign and copies the CA cert into
#      Devs:AmiSSL/Certs/ - libcafile.c connects twice: once with no CA
#      file (must fail - untrusted issuer) and once with mco_CAFile
#      pointing at it (must succeed)
#   4. assert the guest's serial PASS/FAIL/RESULT/END markers
#
# Usage: sh tests/library/cafile-run.sh   (invoked via
# `make library-cafile-smoke`, which cross-builds libcafile + mqtt.library
# - with AmiSSL support, see `make fetch-amissl-sdk` - first)
#
# Environment: same as tls-run.sh (MIDGE_TLS_AMIGA_IMAGE, MIDGE_TLS_KICKSTART).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

COPPERLINE=${COPPERLINE:-copperline}
BIN=${LIBCAFILE_M68K:-$ROOT/build/libcafile}
LIB=${MQTT_LIBRARY_M68K:-$ROOT/build/mqtt.library}
IMAGE=${MIDGE_TLS_AMIGA_IMAGE:-$HOME/src/amibake/manifests/AmiSSLSpike}
ROM=${MIDGE_TLS_KICKSTART:-$HOME/src/amibake/assets/roms/kickstart-47.7.rom}
PORT=18886 # must match libcafile.c's TEST_PORT
CA_FILENAME=midge-test-ca.pem # must match libcafile.c's CA_FILE basename
BENCH=${BENCH:-300}

command -v "$COPPERLINE" >/dev/null || { echo "FAIL: $COPPERLINE not found" >&2; exit 2; }
command -v mosquitto >/dev/null || { echo "FAIL: mosquitto not on PATH" >&2; exit 2; }
command -v openssl >/dev/null || { echo "FAIL: openssl not on PATH" >&2; exit 2; }
[ -e "$BIN" ] || { echo "FAIL: missing $BIN - run 'make libcafile-m68k' first" >&2; exit 2; }
[ -e "$LIB" ] || { echo "FAIL: missing $LIB - run 'make fetch-amissl-sdk && make library' first" >&2; exit 2; }
[ -d "$IMAGE" ] || {
    echo "FAIL: MIDGE_TLS_AMIGA_IMAGE=$IMAGE not found - see" >&2
    echo "  tests/library/README.md's \"TLS smoke test\" section to build one" >&2
    exit 2
}
[ -f "$ROM" ] || { echo "FAIL: MIDGE_TLS_KICKSTART=$ROM not found" >&2; exit 2; }

OUTDIR=$(mktemp -d)
MACHINE_TOML="$OUTDIR/cafile-machine.toml"
OUT=$(mktemp)
MOSQ_PID=
trap 'kill "$MOSQ_PID" 2>/dev/null || true; rm -f "$OUT"; rm -rf "$OUTDIR"' EXIT

# Backdated notBefore, generous notAfter - see the clock-skew note above.
# BSD date (macOS) uses -v-1d; GNU date (Linux/CI) uses -d '-1 day'.
NOT_BEFORE=$(date -u -v-1d +%y%m%d%H%M%SZ 2>/dev/null || date -u -d '-1 day' +%y%m%d%H%M%SZ)
NOT_AFTER=$(date -u -v+30d +%y%m%d%H%M%SZ 2>/dev/null || date -u -d '+30 days' +%y%m%d%H%M%SZ)

openssl genrsa -out "$OUTDIR/ca.key" 2048 2>/dev/null
openssl req -x509 -new -nodes -key "$OUTDIR/ca.key" -sha256 \
    -not_before "$NOT_BEFORE" -not_after "$NOT_AFTER" \
    -out "$OUTDIR/ca.crt" -subj "/CN=midge test CA" 2>/dev/null
openssl genrsa -out "$OUTDIR/server.key" 2048 2>/dev/null
openssl req -new -key "$OUTDIR/server.key" -out "$OUTDIR/server.csr" \
    -subj "/CN=localhost" 2>/dev/null
echo "subjectAltName=DNS:localhost,IP:127.0.0.1" > "$OUTDIR/ext.cnf"
openssl x509 -req -in "$OUTDIR/server.csr" -CA "$OUTDIR/ca.crt" \
    -CAkey "$OUTDIR/ca.key" -CAcreateserial -out "$OUTDIR/server.crt" \
    -not_before "$NOT_BEFORE" -not_after "$NOT_AFTER" \
    -sha256 -extfile "$OUTDIR/ext.cnf" 2>/dev/null

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

cp "$BIN" "$IMAGE/C/libcafile"
cp "$LIB" "$IMAGE/Libs/mqtt.library"
cp "$OUTDIR/ca.crt" "$IMAGE/Devs/AmiSSL/Certs/$CA_FILENAME"
cat > "$IMAGE/S/User-Startup" <<'EOF'
; Written by tests/library/cafile-run.sh on every run - see that script.
Assign AmiSSL: SYS:Devs/AmiSSL
libcafile
echo "LIBCAFILE: ALL-DONE"
EOF

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

set -- --config "$MACHINE_TOML" --rtc-time "$(date +%s)" --noaudio --serial stdout --benchmark-until "$BENCH"
( cd "$IMAGE" && "$COPPERLINE" "$@" ) >"$OUT" 2>/dev/null \
    || { echo "FAIL: $COPPERLINE exited non-zero" >&2; cat "$OUT" >&2; exit 3; }

tr -d '\r' <"$OUT" >"$OUT.n" && mv "$OUT.n" "$OUT"
echo "----- serial capture -----"; cat "$OUT"; echo "--------------------------"

grep -q '^END' "$OUT" 2>/dev/null || { echo "FAIL: no END marker (raise BENCH?)" >&2; exit 1; }
grep -q '^FAIL ' "$OUT" && { echo "FAIL: one or more mqtt.library CA-file checks failed on-target" >&2; exit 1; }
grep -q '^RESULT=OK$' "$OUT" || { echo "FAIL: no RESULT=OK marker" >&2; exit 1; }

echo "PASS: mqtt.library's mco_CAFile works end-to-end on 68020 (private CA trusted, untrusted CA rejected)"
