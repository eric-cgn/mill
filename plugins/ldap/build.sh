#!/bin/sh
# Build LDAP with Retro68 (the same pinned image as Mill):
# build/LDAP.bin, a MacBinary Plug-In file for the "Mill Plug-Ins" folder.
set -e
cd "$(dirname "$0")"
mkdir -p build
tr '\n' '\r' < ../../LICENSE > build/License.txt     # a Mac TEXT resource has CR line ends
cp LDAP.r build/
exec docker run --rm --platform linux/amd64 -v "$PWD/../..":/mill -w /mill/plugins/ldap \
	ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c sh -c '
	set -e
	CC="m68k-apple-macos-gcc -m68000 -Os -Wall -I/mill/sdk/Interfaces -ffunction-sections"
	$CC -c /mill/sdk/Glue/MillGlue.c -o build/glue.o
	$CC -c Plugin.c -o build/plugin.o
	$CC -c ldap.c -o build/ldap.o
	$CC -c ldaputil.c -o build/ldaputil.o
	m68k-apple-macos-gcc -m68000 build/glue.o build/plugin.o build/ldap.o build/ldaputil.o -o build/LDAP.flt -Wl,--mac-flat -Wl,-eMILLMAIN -Wl,--gc-sections -nostartfiles
	cd build && Rez -I/Retro68-build/toolchain/m68k-apple-macos/RIncludes -I/mill/sdk/Interfaces LDAP.r -o LDAP.bin -t MPlg -c MiLL --cc LDAP.bin
	ls -l LDAP.flt *.bin'
