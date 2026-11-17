#!/bin/sh
# Build News with Retro68 (the same pinned image as Mill):
# build/News.bin, a MacBinary Plug-In file for the "Mill Plug-Ins" folder. mime.c is shared
# with IMAP (plugins/mime).
set -e
cd "$(dirname "$0")"
mkdir -p build
tr '\n' '\r' < ../../LICENSE > build/License.txt     # a Mac TEXT resource has CR line ends
cp News.r build/
exec docker run --rm --platform linux/amd64 -v "$PWD/../..":/mill -w /mill/plugins/news \
	ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
	set -e
	CC="m68k-apple-macos-gcc -m68000 -Os -Wall -I/mill/sdk/Interfaces -I../mime -ffunction-sections"
	$CC -c /mill/sdk/Glue/MillGlue.c -o build/glue.o
	$CC -c Plugin.c -o build/plugin.o
	$CC -c news.c -o build/news.o
	$CC -c newsutil.c -o build/newsutil.o
	$CC -c ../mime/mime.c -o build/mime.o
	m68k-apple-macos-gcc -m68000 build/glue.o build/plugin.o build/news.o build/newsutil.o build/mime.o -o build/News.flt -Wl,--mac-flat -Wl,-eMILLMAIN -Wl,--gc-sections -nostartfiles
	cd build && Rez -I/Retro68-build/toolchain/m68k-apple-macos/RIncludes -I/mill/sdk/Interfaces News.r -o News.bin -t MPlg -c MiLL --cc News.bin
	ls -l News.flt *.bin'
