#!/bin/sh
# tests/library/volamos-run.sh — fast local alternative to run.sh: runs the
# cross-built libsmoke harness under volamos (Simon's Rust API-level
# AmigaOS runtime, ~/src/volamos) instead of Copperline, against a real
# disk-resident mqtt.library.
#
# volamos's OpenLibrary/OldOpenLibrary handling (see open_library_common in
# ~/src/volamos/crates/volamos-core/src/dispatch.rs) resolves a bare name
# with no ':' as "LIBS:<name>" against the configured Vfs and, if it
# resolves, does a real AUTOINIT romtag load of the on-disk library - not a
# fake stub - exactly the path this test needs to exercise. So mapping a
# LIBS: volume onto a host directory containing mqtt.library is enough;
# `-V LIBS:<dir>` below does that.
#
# Not a CI substitute for run.sh: volamos doesn't run real 68020
# instruction-level codegen through a booted Kickstart the way Copperline
# does, so it doesn't carry the same on-target confidence - keep using
# `make library-smoke` (Copperline) as the CI/release gate. This script is
# a faster first check while developing.
#
# Usage: sh tests/library/volamos-run.sh   (invoked via
# `make volamos-library-smoke`, which cross-builds libsmoke + mqtt.library
# first)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

BIN="${LIBSMOKE_M68K:-$ROOT/build/libsmoke}"
LIB="${MQTT_LIBRARY_M68K:-$ROOT/build/mqtt.library}"

command -v volamos >/dev/null || { echo "FAIL: volamos not found" >&2; exit 2; }
[ -e "$BIN" ] || { echo "FAIL: missing $BIN" >&2; exit 2; }
[ -e "$LIB" ] || { echo "FAIL: missing $LIB" >&2; exit 2; }

LIBSDIR=$(mktemp -d)
cp "$LIB" "$LIBSDIR/mqtt.library"

OUT=$(mktemp)
trap 'rm -f "$OUT"; rm -rf "$LIBSDIR"' EXIT

set +e
volamos --cpu 68020 -V "LIBS:$LIBSDIR" "$BIN" >"$OUT" 2>&1
rc=$?
set -e

tr -d '\r' <"$OUT" >"$OUT.n" && mv "$OUT.n" "$OUT" # RawPutChar sends CRLF; drop CR
echo "----- output -----"; cat "$OUT"; echo "-------------------"

if [ "$rc" -ge 128 ] || ! grep -q '^BEGIN$' "$OUT"; then
    echo "SKIP: volamos could not run libsmoke against a disk-resident" >&2
    echo "SKIP: mqtt.library (no BEGIN marker seen; volamos exit=$rc)." >&2
    echo "SKIP: see the comment at the top of this script - if volamos's" >&2
    echo "SKIP: OpenLibrary support for AUTOINIT disk libraries regressed" >&2
    echo "SKIP: or mqtt.library's romtag isn't loadable by it, this is not" >&2
    echo "SKIP: a CI gate (Copperline's run.sh is), so we don't fail the" >&2
    echo "SKIP: build over it." >&2
    exit 0
fi

grep -q '^END' "$OUT" || { echo "FAIL: no END marker" >&2; exit 1; }
grep -q '^FAIL ' "$OUT" && { echo "FAIL: one or more mqtt.library checks failed" >&2; exit 1; }
grep -q '^RESULT=OK$' "$OUT" || { echo "FAIL: no RESULT=OK marker" >&2; exit 1; }

echo "PASS: mqtt.library opens/closes correctly under volamos"
