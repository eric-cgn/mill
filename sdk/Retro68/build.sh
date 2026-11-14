#!/bin/sh
# Build the Finger example with Retro68 (the same pinned image as Mill): build/Finger.bin,
# a MacBinary Plug-In file to put in the "Mill Plug-Ins" folder.
set -e
cd "$(dirname "$0")"
exec docker run --rm --platform linux/amd64 -v "$PWD/..":/sdk -w /sdk/Retro68 \
	ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
	set -e
	mkdir -p build
	CC="m68k-apple-macos-gcc -m68000 -Os -Wall -I/sdk/Interfaces -ffunction-sections"
	$CC -c /sdk/Glue/MillGlue.c -o build/glue.o
	$CC -c /sdk/Examples/Finger/Finger.c -o build/finger.o
	m68k-apple-macos-gcc -m68000 build/glue.o build/finger.o -o build/Finger.flt -Wl,--mac-flat -Wl,-eMILLMAIN -Wl,--gc-sections -nostartfiles
	gzip -9nc /sdk/Examples/Finger/License > build/License.gz
	cd build && Rez -I/Retro68-build/toolchain/m68k-apple-macos/RIncludes ../../Examples/Finger/Finger.r -o Finger.bin -t MPlg -c MiLL --cc Finger.bin
	ls -l Finger.flt Finger.bin'
