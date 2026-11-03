#!/bin/sh
# Build Mill with Retro68 (pinned by digest) in a container: build/Mill.bin (MacBinary), Mill.dsk.
set -e
cd "$(dirname "$0")"
exec docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src \
	ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c \
	sh -c 'mkdir -p build && cd build && cmake .. -DCMAKE_TOOLCHAIN_FILE=/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake >/dev/null && make'
