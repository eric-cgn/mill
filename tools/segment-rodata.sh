#!/bin/sh
# segment-rodata.sh SEGMAP COMPILER ARGS...: compile, then, for an object that SEGMAP
# puts in a segment of its own (not Mill), rename its .rodata sections to .text.rodata,
# so its constants (strings, tables) go into that segment instead of Mill's always-loaded
# globals. Only that object's own code may use them: a constant another segment needs
# must be in a file of the Mill segment. CMake runs this for each compile.
segmap=$1; shift
"$@" || exit $?
obj=
while [ $# -gt 0 ]; do [ "$1" = -o ] && obj=$2; shift; done
[ -n "$obj" ] || exit 0
base=$(basename "$obj")
# the objects named in SEGMAP before its catch-all Mill segment
awk '/^SEGMENT Mill/ { exit } /^\*/ { sub(/^\*/, ""); print }' "$segmap" | grep -qx "$base" || exit 0
renames=$(m68k-apple-macos-objdump -h "$obj" | awk '$2 ~ /^\.rodata/ { printf " --rename-section %s=.text%s", $2, $2 }')
[ -n "$renames" ] && m68k-apple-macos-objcopy $renames "$obj"
exit 0
