#!/bin/sh
# Build Mill with Retro68 (pinned by digest) in a container: build/Mill.bin (MacBinary), Mill.dsk.
# MILL_UPDATE_URL=http://... ./build.sh makes a test copy whose Check for updates reads that
# address instead of the real site; a plain ./build.sh goes back to the real one.
# MILL_MEMREPORT=1 ./build.sh makes a test copy in build-memreport/ that writes a "Mill Memory"
# file of what its memory holds, once the window is up and once the first page is in.
# ./build.sh disk makes the "Mill Program Disk" (tools/disk.py): dist/Mill-*.image, Mill ready to
# run, from build/Mill.bin (build first) and the Internet Services, SMB, NFS, News, LDAP,
# IMAP, Image Formats, Archives, and Web Fonts (its fonts, and its code from plugins/fonts)
# it builds itself.
# ./build.sh installer makes Install Mill with EUP (build/installer/Install-Mill-*.bin and .hqx)
# from those, for a release.
# ./build.sh kit makes the Plug-In SDK instead (tools/kit.py): dist/PlugInSDK-*.dsk and .zip.
set -e
cd "$(dirname "$0")"
IMAGE=ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c
PYIMAGE=python:3.12-slim@sha256:5e2bdb1b0d542d5039cffb8181c65930093949194a65c365dfb1b0fe13a4fa5b
if [ "${1:-}" = disk ]; then
	plugins/internet/build.sh
	plugins/smb/build.sh
	plugins/nfs/build.sh
	plugins/news/build.sh
	plugins/ldap/build.sh
	plugins/imap/build.sh
	plugins/images/build.sh
	plugins/archive/build.sh
	plugins/fonts/build.sh
	# the fonts need Python packages the Retro68 image doesn't have: their own container.
	# fontTools makes the outlines, checked by their SHA-256: the image and the packages are pinned
	docker run --rm -v "$PWD":/src -w /src "$PYIMAGE" sh -c '
		pip install -q freetype-py==2.5.1 macresources==1.2 fonttools==4.68.0 &&
		python3 tools/extras/build.py --styles --code "plugins/fonts/build/Web Fonts.flt" --cache build/fontcache --out build/fonts >/dev/null'
	exec docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src "$IMAGE" python3 tools/disk.py
fi
if [ "${1:-}" = installer ]; then
	# Install Mill (installer/mill.ini), made with EUP: its icon from installer/icons.r, then
	# tools/installer.py, which fetches EUP 1.3 and builds the installer from the builds above
	rm -rf build/installer
	exec docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src "$IMAGE" sh -c '
		set -e
		mkdir -p build/installer/stage
		Rez installer/icons.r -t rsrc -c RSED -o "build/installer/stage/Install icon.bin"
		python3 tools/installer.py'
fi
if [ "${1:-}" = kit ]; then
	exec docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src "$IMAGE" python3 tools/kit.py
fi
exec docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src -e MILL_UPDATE_URL="${MILL_UPDATE_URL:-}" \
	-e MILL_MEMREPORT="${MILL_MEMREPORT:-}" -e B="build${MILL_MEMREPORT:+-memreport}" \
	"$IMAGE" \
	sh -c 'mkdir -p $B && cd $B && cmake .. -DCMAKE_TOOLCHAIN_FILE=/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake -DMILL_UPDATE_URL="$MILL_UPDATE_URL" -DMILL_MEMREPORT="$MILL_MEMREPORT" >/dev/null && make'
