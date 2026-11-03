#!/bin/sh
# Publish build/Mill.bin to a folder in Basilisk II's Unix-volume layout (the format
# the lab's AppleShare server serves), with the Finder's "has bundle" flag set so the
# windmill icon shows. Usage: tools/publish.sh <dir> [name]   (e.g. a staging dir to
# copy up; name is the Mac file name, Mill unless given)
set -e
cd "$(dirname "$0")/.."
out=${1:?usage: tools/publish.sh <dir>}
python3 -I - build/Mill.bin "$out" "${2:-}" <<'PY'
import os, struct, sys
b = open(sys.argv[1], 'rb').read()
name = sys.argv[3] or b[2:2 + b[1]].decode('mac_roman')
dlen, rlen = struct.unpack('>II', b[83:91])
rsrc_at = 128 + (dlen + 127) // 128 * 128
out = sys.argv[2]
for d in (out, os.path.join(out, '.rsrc'), os.path.join(out, '.finf')):
    os.makedirs(d, exist_ok=True)
open(os.path.join(out, name), 'wb').write(b[128:128 + dlen])
open(os.path.join(out, '.rsrc', name), 'wb').write(b[rsrc_at:rsrc_at + rlen])
flags = ((b[73] << 8) | b[101] | 0x2000) & ~0x0100   # has bundle; not yet "inited"
open(os.path.join(out, '.finf', name), 'wb').write(b[65:73] + struct.pack('>H', flags) + b'\0' * 22)
print('%s: %d bytes of resources, type %s, flags %04x' % (name, rlen, b[65:73].decode('mac_roman'), flags))
PY
