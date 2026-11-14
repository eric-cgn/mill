#!/bin/sh
# Build Internet Services with Retro68 (the same pinned image as Mill):
# build/Internet Services.bin, a MacBinary Plug-In file for the "Mill Plug-Ins" folder.
set -e
cd "$(dirname "$0")"
mkdir -p build
tr '\n' '\r' < ../../LICENSE > build/License.txt     # a Mac TEXT resource has CR line ends
cp Internet.r build/
exec docker run --rm --platform linux/amd64 -v "$PWD/../..":/mill -w /mill/plugins/internet \
	ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
	set -e
	CC="m68k-apple-macos-gcc -m68000 -Os -Wall -I/mill/sdk/Interfaces -ffunction-sections"
	$CC -c /mill/sdk/Glue/MillGlue.c -o build/glue.o
	$CC -c Internet.c -o build/internet.o
	$CC -c ftp.c -o build/ftp.o
	$CC -c gopher.c -o build/gopher.o
	m68k-apple-macos-gcc -m68000 build/glue.o build/internet.o build/ftp.o build/gopher.o -o build/Internet.flt -Wl,--mac-flat -Wl,-eMILLMAIN -Wl,--gc-sections -nostartfiles
	cd build && Rez -I/Retro68-build/toolchain/m68k-apple-macos/RIncludes Internet.r -o "Internet Services.bin" -t MPlg -c MiLL --cc "Internet Services.bin"
	ls -l Internet.flt *.bin'
