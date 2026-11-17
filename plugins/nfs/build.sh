#!/bin/sh
# Build NFS with Retro68 (the same pinned image as Mill):
# build/NFS.bin, a MacBinary Plug-In file for the "Mill Plug-Ins" folder.
set -e
cd "$(dirname "$0")"
mkdir -p build
tr '\n' '\r' < ../../LICENSE > build/License.txt     # a Mac TEXT resource has CR line ends
cp NFS.r build/
exec docker run --rm --platform linux/amd64 -v "$PWD/../..":/mill -w /mill/plugins/nfs \
	ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
	set -e
	CC="m68k-apple-macos-gcc -m68000 -Os -Wall -I/mill/sdk/Interfaces -ffunction-sections"
	$CC -c /mill/sdk/Glue/MillGlue.c -o build/glue.o
	$CC -c Plugin.c -o build/plugin.o
	$CC -c nfs.c -o build/nfs.o
	$CC -c nfsutil.c -o build/nfsutil.o
	m68k-apple-macos-gcc -m68000 build/glue.o build/plugin.o build/nfs.o build/nfsutil.o -o build/NFS.flt -Wl,--mac-flat -Wl,-eMILLMAIN -Wl,--gc-sections -nostartfiles
	cd build && Rez -I/Retro68-build/toolchain/m68k-apple-macos/RIncludes -I/mill/sdk/Interfaces NFS.r -o NFS.bin -t MPlg -c MiLL --cc NFS.bin
	ls -l NFS.flt *.bin'
