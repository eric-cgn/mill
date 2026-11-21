#!/bin/sh
# Build the code of Web Fonts with Retro68 (the same pinned image as Mill): build/Web Fonts.flt,
# a flat code resource. tools/webfonts/build.py --code puts it in the Web Fonts file as MPlg 128,
# with its MIfc (./build.sh disk does both).
set -e
cd "$(dirname "$0")"
mkdir -p build
exec docker run --rm --network none --platform linux/amd64 -v "$PWD/../..":/mill -w /mill/plugins/fonts \
	ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
	set -e
	CC="m68k-apple-macos-gcc -m68000 -Os -Wall -Wno-pointer-sign -I/mill/sdk/Interfaces -I. -DTT_DROPOUT -ffunction-sections"
	$CC -c /mill/sdk/Glue/MillGlue.c -o build/glue.o
	O=build/glue.o
	for f in Plugin parse raster glyph strike; do $CC -c $f.c -o build/$f.o; O="$O build/$f.o"; done
	m68k-apple-macos-gcc -m68000 $O -o "build/Web Fonts.flt" -Wl,--mac-flat -Wl,-eMILLMAIN -Wl,--gc-sections -nostartfiles
	m68k-apple-macos-size -t $O
	ls -l "build/Web Fonts.flt"'
