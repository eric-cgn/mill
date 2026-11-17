#!/bin/sh
# Build Image Formats with Retro68 (the same pinned image as Mill):
# build/Image Formats.bin, a MacBinary Plug-In file for the "Mill Plug-Ins" folder. PNG's
# inflater is Mill's own, called through the host (plugins/gzhost).
set -e
cd "$(dirname "$0")"
mkdir -p build
tr '\n' '\r' < ../../LICENSE > build/License.txt     # a Mac TEXT resource has CR line ends
tr '\n' '\r' < about.txt > build/About.txt
cp "Image Formats.r" build/
exec docker run --rm --platform linux/amd64 -v "$PWD/../..":/mill -w /mill/plugins/images \
	ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
	set -e
	CC="m68k-apple-macos-gcc -m68000 -Os -Wall -I/mill/sdk/Interfaces -ffunction-sections"
	$CC -c /mill/sdk/Glue/MillGlue.c -o build/glue.o
	$CC -c ../gzhost/gzhost.c -o build/gzhost.o
	O="build/glue.o build/gzhost.o"
	for f in Plugin common macpaint pict xbm bmp pnm png; do $CC -c $f.c -o build/$f.o; O="$O build/$f.o"; done
	m68k-apple-macos-gcc -m68000 $O -o "build/Image Formats.flt" -Wl,--mac-flat -Wl,-eMILLMAIN -Wl,--gc-sections -nostartfiles
	cd build && Rez -I/Retro68-build/toolchain/m68k-apple-macos/RIncludes -I/mill/sdk/Interfaces "Image Formats.r" -o "Image Formats.bin" -t MPlg -c MiLL --cc "Image Formats.bin"
	ls -l "Image Formats.flt" *.bin'
