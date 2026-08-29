#!/bin/sh
# tests/copperline/volamos-run.sh — fast local alternative to run.sh: runs
# the cross-built codec self-test under volamos (Simon's Rust API-level
# AmigaOS runtime, ~/src/volamos) instead of Copperline. volamos traps
# library calls (including exec.library/RawPutChar, the ROM debug path this
# test uses for its PASS/FAIL output) directly at the API boundary rather
# than booting a full emulated machine, so this finishes in milliseconds
# instead of Copperline's ~10s AROS boot.
#
# Not a CI substitute for run.sh: volamos doesn't run real 68020
# instruction-level codegen through a booted Kickstart the way Copperline
# does, so it doesn't carry the same on-target confidence - keep using
# `make test-target` (Copperline) as the CI/release gate. This script is a
# faster first check while developing.
#
# Usage: sh tests/copperline/volamos-run.sh   (invoked via
# `make volamos-test-target`, which cross-builds codec_selftest first)
set -eu

BIN="${CODEC_SELFTEST_M68K:-build/codec_selftest}"

command -v volamos >/dev/null || { echo "FAIL: volamos not found" >&2; exit 2; }
[ -e "$BIN" ] || { echo "FAIL: missing $BIN" >&2; exit 2; }

OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT

volamos --cpu 68020 "$BIN" >"$OUT" 2>&1 \
    || { echo "FAIL: volamos exited non-zero" >&2; cat "$OUT" >&2; exit 3; }

tr -d '\r' <"$OUT" >"$OUT.n" && mv "$OUT.n" "$OUT" # RawPutChar sends CRLF; drop CR
echo "----- output -----"; cat "$OUT"; echo "-------------------"
grep -q '^END' "$OUT" || { echo "FAIL: no END marker" >&2; exit 1; }
grep -q '^FAIL ' "$OUT" && { echo "FAIL: one or more codec checks failed" >&2; exit 1; }
grep -q '^RESULT=OK$' "$OUT" || { echo "FAIL: no RESULT=OK marker" >&2; exit 1; }

echo "PASS: MQTT codec checks correct under volamos"
