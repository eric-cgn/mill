#!/bin/sh
# Build Test Format (TestFormat.c), a Plug-In for testing Mill's formats on a Mac, never
# shipped: build/Test Format.bin, for the "Mill Plug-Ins" folder of a test copy. Its file
# tests/plugin/test.mtest goes beside it.
set -e
cd "$(dirname "$0")"
mkdir -p build
exec docker run --rm --network none --platform linux/amd64 -v "$PWD/../..":/mill -w /mill/tests/plugin \
	ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
	set -e
	CC="m68k-apple-macos-gcc -m68000 -Os -Wall -I/mill/sdk/Interfaces -ffunction-sections"
	$CC -c /mill/sdk/Glue/MillGlue.c -o build/glue.o
	$CC -c TestFormat.c -o build/test.o
	m68k-apple-macos-gcc -m68000 build/glue.o build/test.o -o build/TestFormat.flt -Wl,--mac-flat -Wl,-eMILLMAIN -Wl,--gc-sections -nostartfiles
	cd build && Rez -I/Retro68-build/toolchain/m68k-apple-macos/RIncludes -I/mill/sdk/Interfaces ../TestFormat.r -o "Test Format.bin" -t MPlg -c MiLL --cc "Test Format.bin"
	ls -l TestFormat.flt "Test Format.bin"'
