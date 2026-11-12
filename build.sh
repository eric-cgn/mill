#!/bin/sh
# Build Mill with Retro68 (pinned by digest) in a container: build/Mill.bin (MacBinary), Mill.dsk.
# MILL_UPDATE_URL=http://... ./build.sh makes a test copy whose Check for updates reads that
# address instead of the real site; a plain ./build.sh goes back to the real one.
set -e
cd "$(dirname "$0")"
exec docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src -e MILL_UPDATE_URL="${MILL_UPDATE_URL:-}" \
	ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c \
	sh -c 'mkdir -p build && cd build && cmake .. -DCMAKE_TOOLCHAIN_FILE=/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake -DMILL_UPDATE_URL="$MILL_UPDATE_URL" >/dev/null && make'
