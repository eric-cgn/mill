#!/bin/sh
# Build IMAP with Retro68 (the same pinned image as Mill):
# build/IMAP.bin, a MacBinary Plug-In file for the "Mill Plug-Ins" folder. mime.c is shared
# with News (plugins/mime).
set -e
cd "$(dirname "$0")"
mkdir -p build
tr '\n' '\r' < ../../LICENSE > build/License.txt     # a Mac TEXT resource has CR line ends
cp IMAP.r build/
exec docker run --rm --platform linux/amd64 -v "$PWD/../..":/mill -w /mill/plugins/imap \
	ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
	set -e
	CC="m68k-apple-macos-gcc -m68000 -Os -Wall -I/mill/sdk/Interfaces -I../mime -ffunction-sections"
	$CC -c /mill/sdk/Glue/MillGlue.c -o build/glue.o
	$CC -c Plugin.c -o build/plugin.o
	$CC -c imap.c -o build/imap.o
	$CC -c imaputil.c -o build/imaputil.o
	$CC -c ../mime/mime.c -o build/mime.o
	m68k-apple-macos-gcc -m68000 build/glue.o build/plugin.o build/imap.o build/imaputil.o build/mime.o -o build/IMAP.flt -Wl,--mac-flat -Wl,-eMILLMAIN -Wl,--gc-sections -nostartfiles
	cd build && Rez -I/Retro68-build/toolchain/m68k-apple-macos/RIncludes -I/mill/sdk/Interfaces IMAP.r -o IMAP.bin -t MPlg -c MiLL --cc IMAP.bin
	ls -l IMAP.flt *.bin'
