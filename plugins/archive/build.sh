#!/bin/sh
# Build Archives with Retro68 (the same pinned image as Mill): build/Archives.bin, a
# MacBinary Plug-In file for the "Mill Plug-Ins" folder. The cores are plain C, tested on a
# host by tests/test_archive.c; the check at the end shows nothing but the host's calls is
# linked (no malloc, no printf).
set -e
cd "$(dirname "$0")"
mkdir -p build
tr '\n' '\r' < ../../LICENSE > build/License.txt     # a Mac TEXT resource has CR line ends
tr '\n' '\r' < about.txt > build/About.txt
cp "Archives.r" build/
exec docker run --rm --platform linux/amd64 -v "$PWD/../..":/mill -w /mill/plugins/archive \
	ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
	set -e
	CC="m68k-apple-macos-gcc -m68000 -Os -Wall -Wextra -Wno-pointer-sign -Wno-unused-parameter -I/mill/sdk/Interfaces -ffunction-sections -fdata-sections"
	$CC -c /mill/sdk/Glue/MillGlue.c -o build/glue.o
	$CC -Werror -c ../gzhost/gzhost.c -o build/gzhost.o
	O="build/glue.o build/gzhost.o"
	for f in Plugin common zip tar gzip macbinary binhex; do $CC -Werror -c $f.c -o build/$f.o; O="$O build/$f.o"; done
	for f in charset macenc filetypes; do $CC -c /mill/src/core/$f.c -o build/core-$f.o; O="$O build/core-$f.o"; done
	m68k-apple-macos-gcc -m68000 $O -o build/Archives.flt -Wl,--mac-flat -Wl,-eMILLMAIN -Wl,--gc-sections -nostartfiles \
		-Wl,-Map,build/archives.map
	sed -n "/^Linker script and memory map/,\$p" build/archives.map > build/linked.map
	if grep -q -E "malloc|printf" build/linked.map; then echo "malloc or printf is linked in"; exit 1; fi
	cd build && Rez -I/Retro68-build/toolchain/m68k-apple-macos/RIncludes -I/mill/sdk/Interfaces Archives.r -o Archives.bin -t MPlg -c MiLL --cc Archives.bin
	ls -l Archives.flt Archives.bin'
