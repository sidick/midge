#!/bin/sh
# run.sh — headless Copperline on-target smoke test for mqtt.library.
#
# Boots a 68020 from ./sys (a throwaway RAM volume built from this host
# directory, same as tests/copperline/run.sh). The Startup-Sequence runs
# C:libsmoke, which OpenLibrary("mqtt.library", 0)s the freshly cross-built
# library staged into sys/Libs/, checks its lib_Node.ln_Name/lib_Version,
# CloseLibrary()s it, and emits PASS/FAIL lines over serial via RawPutChar.
# Copperline forwards serial to its stdout (`--serial stdout`); we assert
# the result.
#
# LIBS: is auto-assigned by the OS boot process to the boot volume's Libs
# directory before Startup-Sequence even runs (same as DEVS:/FONTS:/S:/C:/
# L:), so staging mqtt.library into sys/Libs/ is all that's needed for
# OpenLibrary to find it as a real disk library - no explicit Assign, no
# Mount.
#
# Prereqs:
#   - copperline on PATH (brew install copperline), or COPPERLINE= pointing
#     at another build. Such a build won't have the bundled AROS assets
#     installed next to it, so pair COPPERLINE= with KICK= (a real ROM)
#     when using one.
#   - the cross-built harness  LIBSMOKE_M68K=   (default build/libsmoke)
#   - the cross-built library  MQTT_LIBRARY_M68K= (default build/mqtt.library)
#   - KICK= (optional): a Kickstart ROM. If unset, boots Copperline's
#     bundled AROS Kickstart replacement - redistributable, so CI needs no ROM.
#
# For a much faster local loop, `make volamos-library-smoke` runs the same
# check under volamos instead - see volamos-run.sh. Not a CI substitute:
# this Copperline run is the one that exercises real 68020 codegen and a
# real disk-library load through a booted Kickstart.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

COPPERLINE=${COPPERLINE:-copperline}
KICK=${KICK:-}   # empty => bundled AROS (no licensed ROM needed)
BIN=${LIBSMOKE_M68K:-$ROOT/build/libsmoke}
LIB=${MQTT_LIBRARY_M68K:-$ROOT/build/mqtt.library}
BENCH=${BENCH:-40} # emulated seconds to run; enough for the slower AROS boot

[ -e "$BIN" ] || { echo "FAIL: missing $BIN" >&2; exit 2; }
[ -e "$LIB" ] || { echo "FAIL: missing $LIB" >&2; exit 2; }
[ -z "$KICK" ] || [ -e "$KICK" ] || { echo "FAIL: KICK set but missing: $KICK" >&2; exit 2; }
command -v "$COPPERLINE" >/dev/null || { echo "FAIL: $COPPERLINE not found" >&2; exit 2; }
[ -n "$KICK" ] && echo "ROM: $KICK" || echo "ROM: bundled AROS"

# --- stage the boot volume (harness binary + the library it opens) ---------
mkdir -p "$HERE/sys/C" "$HERE/sys/Libs"
cp "$BIN" "$HERE/sys/C/libsmoke"
cp "$LIB" "$HERE/sys/Libs/mqtt.library"

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
grep -q '^FAIL ' "$OUT" && { echo "FAIL: one or more mqtt.library checks failed on-target" >&2; exit 1; }
grep -q '^RESULT=OK$' "$OUT" || { echo "FAIL: no RESULT=OK marker" >&2; exit 1; }

echo "PASS: mqtt.library opens/closes correctly on 68020 (via serial)"
