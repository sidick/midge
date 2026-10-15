#!/bin/sh
# run.sh — headless Copperline on-target smoke test for the MQTT codec.
#
# Boots straight into codec_selftest via `--run`/`--exit-on-return`
# (Copperline's generated RunBoot:/RunProg: volumes - no staged boot
# volume needed). codec_selftest exercises the same vectors as
# tests/test_codec.c, emits PASS/FAIL lines over serial via RawPutChar for
# diagnostics, and returns its failure count as its AmigaDOS return code;
# Copperline relays that as its own exit status, which is what we assert.
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

[ -e "$BIN" ] || { echo "FAIL: missing $BIN" >&2; exit 2; }
[ -z "$KICK" ] || [ -e "$KICK" ] || { echo "FAIL: KICK set but missing: $KICK" >&2; exit 2; }
command -v "$COPPERLINE" >/dev/null || { echo "FAIL: $COPPERLINE not found" >&2; exit 2; }
[ -n "$KICK" ] && echo "ROM: $KICK" || echo "ROM: bundled AROS"

OUT=$(mktemp)
cleanup() { rm -f "$OUT"; }
trap cleanup EXIT INT TERM

# --exit-on-return (which requires --run) ends the session the moment
# codec_selftest's return code lands, instead of running unthrottled for a
# fixed --benchmark-until window - Copperline's own exit status becomes
# that return code (0 = all checks passed, clamped 0-255; 4 = the run
# ended some other way before the program returned). cd so machine.toml's
# relative paths resolve from this directory. A ROM arg overrides the
# config's (absent) rom; with none, Copperline boots its bundled AROS.
set -- --config machine.toml --noaudio --serial stdout \
    --run "$BIN" --exit-on-return
[ -n "$KICK" ] && set -- "$@" "$KICK"
STATUS=0
( cd "$HERE" && "$COPPERLINE" "$@" ) >"$OUT" 2>/dev/null || STATUS=$?

tr -d '\r' <"$OUT" >"$OUT.n" && mv "$OUT.n" "$OUT" # serial sends CRLF; drop CR
echo "----- serial capture -----"; cat "$OUT"; echo "--------------------------"

# Status 4 means --exit-on-return never saw a return code (Copperline
# ended for some other reason); any other nonzero status is codec_selftest's
# own AmigaDOS return code (its failing-check count) relayed verbatim -
# see docs/guide/headless.md#exit-statuses upstream.
[ "$STATUS" -ne 4 ] || { echo "FAIL: $COPPERLINE ended before codec_selftest returned" >&2; exit 1; }
[ "$STATUS" -eq 0 ] || { echo "FAIL: codec_selftest reported $STATUS failing check(s) on-target (or $COPPERLINE itself failed)" >&2; exit 1; }

echo "PASS: MQTT codec checks correct on 68020 (via --exit-on-return)"
