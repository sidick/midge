#!/bin/sh
# build.sh — cross-compile the AmiSSL handshake SPIKE (amisslspike.c) for
# m68k/AmigaOS3. Uses the local m68k-amigaos-gcc toolchain at /opt/amiga/bin
# directly (no Docker) - same flags amiauth's docker-wrapped
# amissl-bench.sh uses, just invoked natively; Docker Desktop was wedged for
# `docker run` on this machine (confirmed: docker ps -a never showed a
# container from repeated attempts) and there's no need for it when the
# cross-compiler is already installed locally. Throwaway/investigative code
# (see /Users/simond/src/midge/CLAUDE.md) — not part of the shipped midge
# build, not run by `make`, does not touch src/. This script only builds; it
# does not run the emulator (see /Users/simond/src/midge/tests/copperline/
# for the harness that does).
#
# Usage:
#   tests/copperline/amissl-spike/build.sh
#
# Output: tests/copperline/amissl-spike/build/amisslspike
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
M68K_CC=${M68K_CC:-/opt/amiga/bin/m68k-amigaos-gcc}

mkdir -p "$HERE/build"

# Fetch (or reuse the cached) AmiSSL v5.27 SDK - only the Developer headers +
# libamisslstubs.a are needed to link this binary.
sdk_out=$("$HERE/fetch-amissl-sdk.sh")
SDK_DEV=$(echo "$sdk_out" | sed -n 1p)
[ -d "$SDK_DEV" ] || { echo "FAIL: fetch-amissl-sdk.sh did not return a valid SDK path" >&2; exit 1; }

"$M68K_CC" -std=c99 -O2 -Wall -m68020 -noixemul \
  -I"$SDK_DEV/include" \
  "$HERE/amisslspike.c" \
  -L"$SDK_DEV/lib/AmigaOS3" -lamisslstubs \
  -o "$HERE/build/amisslspike"

echo "build.sh: OK -> $HERE/build/amisslspike"
