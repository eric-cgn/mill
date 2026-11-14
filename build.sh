#!/bin/sh
# Build Mill with Retro68 (pinned by digest) in a container: build/Mill.bin (MacBinary), Mill.dsk.
# MILL_UPDATE_URL=http://... ./build.sh makes a test copy whose Check for updates reads that
# address instead of the real site; a plain ./build.sh goes back to the real one.
# MILL_MEMREPORT=1 ./build.sh makes a test copy in build-memreport/ that writes a "Mill Memory"
# file of what its memory holds, once the window is up and once the first page is in.
# ./build.sh disk makes the distribution disk (tools/disk.py): dist/Mill-*.dsk, from build/Mill.bin
# (build first) and the Internet Services and Web Fonts it builds itself.
# ./build.sh installer makes Install Mill (build/installer/Install.bin) from those, for a release.
# ./build.sh kit makes the Plug-In SDK instead (tools/kit.py): dist/PlugInSDK-*.dsk and .zip.
set -e
cd "$(dirname "$0")"
IMAGE=ghcr.io/autc04/retro68@sha256:dd923969100f408943f73c9618b3c7628686aae819583eecb0ddd1837da6943c
if [ "${1:-}" = disk ]; then
	plugins/internet/build.sh
	# the fonts need Python packages the Retro68 image doesn't have: their own container
	docker run --rm -v "$PWD":/src -w /src python:3.12-slim sh -c '
		pip install -q freetype-py macresources &&
		python3 tools/extras/build.py --styles --cache build/fontcache --out build/fonts >/dev/null'
	exec docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src "$IMAGE" python3 tools/disk.py
fi
if [ "${1:-}" = installer ]; then
	# Install Mill 2.0 (installer/): the payload from the builds above, then the program
	rm -rf build/installer
	exec docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src "$IMAGE" sh -c '
		set -e
		python3 tools/installer.py
		cd build/installer && cmake ../../installer -DCMAKE_TOOLCHAIN_FILE=/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake >/dev/null && make'
fi
if [ "${1:-}" = kit ]; then
	exec docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src "$IMAGE" python3 tools/kit.py
fi
exec docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src -e MILL_UPDATE_URL="${MILL_UPDATE_URL:-}" \
	-e MILL_MEMREPORT="${MILL_MEMREPORT:-}" -e B="build${MILL_MEMREPORT:+-memreport}" \
	"$IMAGE" \
	sh -c 'mkdir -p $B && cd $B && cmake .. -DCMAKE_TOOLCHAIN_FILE=/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake -DMILL_UPDATE_URL="$MILL_UPDATE_URL" -DMILL_MEMREPORT="$MILL_MEMREPORT" >/dev/null && make'
