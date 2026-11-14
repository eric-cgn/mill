#!/usr/bin/env python3
"""Mill's distribution disk: dist/Mill-VERSION.dsk, an 800K HFS disk named "Mill" with Mill,
its Plug-Ins folder (Web Fonts, Internet Services), and the Plug-In SDK folder (the
kit's own disk contents, made by tools/kit.py's code). Nothing of Apple's. Needs what
./build.sh makes first: build/Mill.bin, build/fonts/ (tools/extras/build.py --styles --out
build/fonts), and plugins/internet/build/Internet Services.bin. ./build.sh disk does all of it."""
import os, re, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit

root = kit.root
VOLUME = 'Mill'
CAPACITY = 800 * 1024


def macbinary_fields(path):
    b = open(path, 'rb').read(128)
    dlen, rlen = struct.unpack('>II', b[83:91])
    return b[65:69], b[69:73], dlen, rlen


def with_bundle_bit(src, dst):
    """A copy of a MacBinary file with the Finder's bundle bit set, and the header's CRC redone"""
    d = bytearray(open(src, 'rb').read())
    d[73] |= 0x20
    crc = 0
    for b in d[:124]:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    d[124:126] = crc.to_bytes(2, 'big')
    open(dst, 'wb').write(d)


def need(path, how):
    if not os.path.exists(path):
        sys.exit('%s is missing: %s' % (os.path.relpath(path, root), how))
    return path


def main():
    v = kit.version()
    mill = need(os.path.join(root, 'build', 'Mill.bin'), './build.sh')
    fonts = need(os.path.join(root, 'build', 'fonts', 'Web Fonts.bin'), 'tools/extras/build.py --styles --out build/fonts')
    inet = need(os.path.join(root, 'plugins', 'internet', 'build', 'Internet Services.bin'), 'plugins/internet/build.sh')
    out = os.path.join(kit.dist, 'Mill-%s.dsk' % v)
    os.makedirs(kit.dist, exist_ok=True)
    tmp = tempfile.mkdtemp()
    items = []          # (name, bytes) for the report
    with open(out, 'wb') as f:
        f.write(bytes(CAPACITY))
    kit.hfs('hformat', '-l', VOLUME, out)
    try:
        kit.hfs('hmkdir', ':Mill Plug-Ins')
        bundled = os.path.join(tmp, 'Mill.bin')
        with_bundle_bit(mill, bundled)
        for name, path, want, where in (
                ('Mill', bundled, (b'APPL', b'MiLL'), ':'),
                ('Mill Plug-Ins:Web Fonts', fonts, (b'FFIL', b'MiLL'), ':Mill Plug-Ins:'),
                ('Mill Plug-Ins:Internet Services', inet, (b'MPlg', b'MiLL'), ':Mill Plug-Ins:')):
            t, c, dlen, rlen = macbinary_fields(path)
            if (t, c) != want:
                sys.exit('%s is type %s, creator %s; expected %s' % (name, t, c, want))
            kit.hfs('hcopy', '-m', path, where)
            items.append((name, dlen + rlen))
        before = free_bytes()
        kit.copy_kit(':Mill Plug-In SDK')
        items.append(('Mill Plug-In SDK (the SDK disk\'s files)', before - free_bytes()))
        free = free_bytes()
    finally:
        kit.hfs('humount')
    print('%s' % out)
    print('%-42s %9s' % ('item', 'bytes'))
    for n, b in items:
        print('%-42s %9d' % (n, b))
    print('%-42s %9d of %d' % ('free', free, CAPACITY))


def free_bytes():
    """The mounted volume's free bytes, from hvol"""
    r = subprocess.run(['hvol'], capture_output=True, text=True, check=True).stdout
    m = re.search(r'(\d+)\s+bytes free', r)
    if not m:
        sys.exit('hvol said:\n' + r)
    return int(m.group(1))


if __name__ == '__main__':
    main()
