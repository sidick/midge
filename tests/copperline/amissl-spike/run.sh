#!/bin/sh
# run.sh - drives the AmiSSL de-risking spike (issue #3, Milestone 2):
# boots an amibake-built AmigaOS 3.2.2 + AmiSSL 5.27 image
# (~/src/amibake, manifests/AmiSSLSpike.toml - amisslmaster.library +
# the 68020-40 amissl_v362.library + AmiSSL's bundled cert store under
# Devs/AmiSSL, per the amissl recipe's own install layout) under
# Copperline's HostSocket board, against a real TLS Mosquitto listener on
# the host, and runs the compiled amisslspike binary repeatedly (20x, via
# ./User-Startup staged as S:User-Startup) to check SSL_set_fd +
# WaitSelect-gated handshake reliability - NOT a pass/fail CI check, a
# one-off investigation. Throwaway/investigative (CLAUDE.md); not run by
# `make`, does not touch src/.
#
# FINDING (2026-08-30): at real 68020/14MHz cycle-accurate timing, the
# handshake always succeeds but the immediately-following connect-write
# fails intermittently with SSL_ERROR_SYSCALL (~2/7-9 runs passed clean).
# With --jit alone (same nominal 14MHz), it's 7/7 clean; with --jit
# --cpu-clock 200 it's been 9/9+ clean. So this reproduces only under
# real cycle-accurate 68020 timing, not under Copperline's JIT (explicitly
# "not cycle-exact, like an accelerator card" - see `copperline --help`),
# and not under a raised clock. That means this is very likely a genuine
# timing-sensitive race in the WaitSelect-gated write-retry loop, NOT an
# artifact of slow-emulator throughput - and midge's baseline target is a
# real, unaccelerated 68020, which is exactly the condition that
# reproduces it. Cranking --jit/--cpu-clock here makes the spike itself
# pass but does NOT validate transport_amissl.c against midge's actual
# target hardware - keep it off (or run both ways) when using this spike
# to judge real-hardware readiness. Root cause not yet identified.
#
# Matches the upstream AmiSSL maintainer's own diagnosis of a near-
# identical symptom (broken pipe after a good handshake, fixed by
# enabling JIT): github.com/jens-maus/amissl/issues/111, closed as "the
# Amiga just can't keep up with modern SSL" - a CPU-speed limit, not an
# AmiSSL bug to fix upstream.
#
# THRESHOLD SWEEP (2026-08-30, real cycle-accurate --cpu-clock, no --jit,
# 150s bench, this machine): 14MHz pass=2/6 fail=4/6; 16MHz pass=4/4
# fail=0; 18MHz pass=4/4 fail=0; 20MHz pass=5/5 fail=0; 25/33/50/75/100MHz
# all clean. So the threshold sits between 14 and 16MHz - i.e. a genuinely
# stock, unaccelerated 68020 (~14MHz, e.g. an A1200's 68EC020) is right at
# the edge of this race, while even a modest bump (16MHz+) clears it
# reliably in every run tried so far. Not yet tested on real hardware or
# against a broker other than local Mosquitto.
#
# Usage: tests/copperline/amissl-spike/run.sh [BENCH_SECONDS]
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
COPPERLINE=${COPPERLINE:-copperline}
AMIBAKE_IMAGE=${AMIBAKE_IMAGE:-$HOME/src/amibake/manifests/AmiSSLSpike}
PORT=18883
# A real OS3.2.2 boot (SetPatch, assigns, IPrefs, LoadWB) costs much more
# emulated time than the old minimal-AROS boot did - 300s comfortably
# covers boot + all 20 amisslspike iterations at this machine's ~1 MIPS.
BENCH=${1:-300}
# Default is cycle-accurate stock 68020/14MHz - the FINDING above means
# turning this on hides the very bug the spike exists to catch. Set
# SPIKE_FAST=1 only for a quick "does the wiring work at all" smoke check.
# SPIKE_CPU_CLOCK sweeps a real (non-JIT, still cycle-accurate) clock
# speed instead, to find the minimum-spec threshold that clears the race.
CPU_FLAGS=""
if [ "${SPIKE_FAST:-0}" = "1" ]; then
    CPU_FLAGS="--jit --cpu-clock 200"
elif [ -n "${SPIKE_CPU_CLOCK:-}" ]; then
    CPU_FLAGS="--cpu-clock $SPIKE_CPU_CLOCK"
fi

command -v "$COPPERLINE" >/dev/null || { echo "run.sh: $COPPERLINE not found" >&2; exit 2; }
command -v mosquitto >/dev/null || { echo "run.sh: mosquitto not on PATH" >&2; exit 2; }
command -v openssl >/dev/null || { echo "run.sh: openssl not on PATH" >&2; exit 2; }
[ -x "$HERE/build/amisslspike" ] || { echo "run.sh: build/amisslspike missing - run build.sh first" >&2; exit 2; }
[ -d "$AMIBAKE_IMAGE" ] || {
    echo "run.sh: $AMIBAKE_IMAGE missing - build it first:" >&2
    echo "  (cd ~/src/amibake && .venv/bin/amibake build manifests/AmiSSLSpike.toml --assets assets)" >&2
    exit 2
}

cp "$HERE/build/amisslspike" "$AMIBAKE_IMAGE/C/amisslspike"
cp "$HERE/User-Startup" "$AMIBAKE_IMAGE/S/User-Startup"

OUTDIR=$(mktemp -d)
MOSQ_PID=
trap 'kill "$MOSQ_PID" 2>/dev/null || true; rm -rf "$OUTDIR"' EXIT INT TERM

openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$OUTDIR/server.key" -out "$OUTDIR/server.crt" -days 2 \
    -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
    2>/dev/null

cat > "$OUTDIR/mosquitto.conf" <<EOF
listener $PORT 127.0.0.1
allow_anonymous true
certfile $OUTDIR/server.crt
keyfile $OUTDIR/server.key
EOF

mosquitto -c "$OUTDIR/mosquitto.conf" > "$OUTDIR/mosquitto.log" 2>&1 &
MOSQ_PID=$!

i=0
while ! grep -q "mosquitto version .* running" "$OUTDIR/mosquitto.log" 2>/dev/null; do
    i=$((i + 1))
    [ "$i" -ge 50 ] && { echo "run.sh: mosquitto did not start"; cat "$OUTDIR/mosquitto.log"; exit 1; }
    sleep 0.1
done
echo "run.sh: TLS mosquitto listening on 127.0.0.1:$PORT"

( cd "$HERE" && "$COPPERLINE" --config machine.toml $CPU_FLAGS --noaudio --serial stdout --benchmark-until "$BENCH" ) \
    > "$OUTDIR/copperline.log" 2>&1 || true

echo "--- copperline serial output (SPIKE: lines) ---"
tr -d '\r' < "$OUTDIR/copperline.log" | grep '^SPIKE:' || echo "(none - see full log below)"

pass=$(tr -d '\r' < "$OUTDIR/copperline.log" | grep -c '^SPIKE: PASS$' || true)
fail=$(tr -d '\r' < "$OUTDIR/copperline.log" | grep -c '^SPIKE: FAIL' || true)
total=$((pass + fail))

echo
echo "=== SPIKE RESULT: $pass/$total passed, $fail failed ==="

if ! tr -d '\r' < "$OUTDIR/copperline.log" | grep -q '^SPIKE: ALL-DONE$'; then
    echo "run.sh: WARNING - Startup-Sequence did not reach ALL-DONE (boot/timeout issue?) - raising BENCH_SECONDS may help"
    echo "--- full copperline log ---"
    cat "$OUTDIR/copperline.log"
fi
