#!/bin/sh
# run.sh — headless Copperline on-target smoke test for the MQTT codec.
#
# Boots a 68020 from ./sys (a throwaway RAM volume built from this host
# directory). The Startup-Sequence runs C:codec_selftest, which exercises
# the same vectors as tests/test_codec.c and emits PASS/FAIL lines over
# serial via RawPutChar. Copperline forwards serial to its stdout
# (`--serial stdout`); we assert the result.
#
# No Workbench files, handlers, or Mounts are needed - RawPutChar is the ROM
# debug path straight to the Paula serial registers. No networking either -
# see tests/net/ for that.
#
# Prereqs:
#   - copperline on PATH (brew install copperline), or COPPERLINE= pointing
#     at another build. Such a build won't have the bundled AROS assets
#     installed next to it, so pair COPPERLINE= with KICK= (a real ROM)
#     when using one.
#   - the cross-built harness    CODEC_SELFTEST_M68K= (default build/codec_selftest)
#   - KICK= (optional): a Kickstart ROM. If unset, boots Copperline's
#     bundled AROS Kickstart replacement - redistributable, so CI needs no ROM.
#
# For a much faster local loop (milliseconds instead of a ~10s AROS boot),
# `make volamos-test-target` runs the same self-test under volamos instead -
# see volamos-run.sh. Not a CI substitute: this Copperline run is the one
# that exercises real 68020 codegen through a booted Kickstart.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

COPPERLINE=${COPPERLINE:-copperline}
KICK=${KICK:-}   # empty => bundled AROS (no licensed ROM needed)
BIN=${CODEC_SELFTEST_M68K:-$ROOT/build/codec_selftest}
BENCH=${BENCH:-40} # emulated seconds to run; enough for the slower AROS boot

[ -e "$BIN" ] || { echo "FAIL: missing $BIN" >&2; exit 2; }
[ -z "$KICK" ] || [ -e "$KICK" ] || { echo "FAIL: KICK set but missing: $KICK" >&2; exit 2; }
command -v "$COPPERLINE" >/dev/null || { echo "FAIL: $COPPERLINE not found" >&2; exit 2; }
[ -n "$KICK" ] && echo "ROM: $KICK" || echo "ROM: bundled AROS"

# --- stage the boot volume (just the harness binary) -------------------------
mkdir -p "$HERE/sys/C"
cp "$BIN" "$HERE/sys/C/codec_selftest"

OUT=$(mktemp)
cleanup() { rm -f "$OUT"; }
trap cleanup EXIT INT TERM

# --benchmark-until runs with no window until the given emulated time, then
# exits; boot + self-test finish well before then. cd so `path = "sys"` in
# machine.toml resolves relative to this directory. A ROM arg overrides the
# config's (absent) rom; with none, Copperline boots its bundled AROS.
set -- --config machine.toml --noaudio --serial stdout --benchmark-until "$BENCH"
[ -n "$KICK" ] && set -- "$@" "$KICK"
( cd "$HERE" && "$COPPERLINE" "$@" ) >"$OUT" 2>/dev/null \
    || { echo "FAIL: $COPPERLINE exited non-zero" >&2; cat "$OUT" >&2; exit 3; }

tr -d '\r' <"$OUT" >"$OUT.n" && mv "$OUT.n" "$OUT" # serial sends CRLF; drop CR
echo "----- serial capture -----"; cat "$OUT"; echo "--------------------------"
grep -q '^END' "$OUT" 2>/dev/null || { echo "FAIL: no END marker (raise BENCH?)" >&2; exit 1; }
grep -q '^FAIL ' "$OUT" && { echo "FAIL: one or more codec checks failed on-target" >&2; exit 1; }
grep -q '^RESULT=OK$' "$OUT" || { echo "FAIL: no RESULT=OK marker" >&2; exit 1; }

echo "PASS: MQTT codec checks correct on 68020 (via serial)"
