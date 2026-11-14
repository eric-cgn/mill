#!/usr/bin/env python3
"""Mill's distribution disk: dist/Mill-VERSION.image, a Disk Copy 4.2 image of an 800K HFS disk named "Mill" with Mill,
its Plug-Ins folder (Web Fonts, Internet Services), and the Plug-In SDK folder (the
kit's own disk contents, made by tools/kit.py's code). Nothing of Apple's. Needs what
./build.sh makes first: build/Mill.bin, build/fonts/ (tools/extras/build.py --styles --out
build/fonts), and plugins/internet/build/Internet Services.bin. ./build.sh disk does all of it."""
import os, re, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit
import icons

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


def crc16(b):
    crc = 0
    for x in b:
        crc ^= x << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def resource_fork(resources):
    """A resource fork of [(type, id, data)], with no names"""
    data = b''
    refs = {}
    for t, i, d in resources:
        refs.setdefault(t, []).append((i, len(data)))
        data += struct.pack('>I', len(d)) + d
    types = b''
    reflist = b''
    first = 2 + 8 * len(refs)
    for t, rs in refs.items():
        types += t + struct.pack('>HH', len(rs) - 1, first + len(reflist))
        for i, off in rs:
            reflist += struct.pack('>hH', i, 0xFFFF) + bytes([0]) + off.to_bytes(3, 'big') + bytes(4)
    tlist = struct.pack('>H', len(refs) - 1) + types + reflist
    amap = bytes(16) + bytes(4) + bytes(2) + bytes(2) + struct.pack('>HH', 28, 28 + len(tlist)) + tlist
    head = struct.pack('>IIII', 256, 256 + len(data), len(data), len(amap))
    amap = head + amap[16:]
    return head + bytes(240) + data + amap


def icon_file(path):
    """MacBinary of the invisible "Icon" file ('icon', 'MACS') that makes the disk's custom icon:
    the floppy with the windmill, ICN# and ics# -16455"""
    def icn(art, size, mask):
        return b''.join(int(r.replace('#', '1').replace('.', '0'), 2).to_bytes(size // 8, 'big') for r in icons.rows(art) + icons.rows(mask))
    fork = resource_fork([(b'ICN#', icons.DISK_ID, icn(icons.DISK_32, 32, icons.silhouette(icons.DISK_32))),
                          (b'ics#', icons.DISK_ID, icn(icons.DISK_16, 16, icons.silhouette(icons.DISK_16)))])
    name = b'Icon\r'
    h = bytearray(128)
    h[1] = len(name)
    h[2:2 + len(name)] = name
    h[65:69] = b'icon'
    h[69:73] = b'MACS'
    h[73] = 0x40                    # invisible
    h[87:91] = struct.pack('>I', len(fork))
    h[122] = h[123] = 129
    h[124:126] = crc16(h[:124]).to_bytes(2, 'big')
    pad = lambda b: b + bytes(-len(b) % 128)
    open(path, 'wb').write(bytes(h) + pad(fork))


def custom_icon_flag(image):
    """Set the root folder's "has a custom icon" Finder flag (0x0400) in the disk image: its record
    is in the catalog, which hfsutils can't write that flag to"""
    d = bytearray(open(image, 'rb').read())
    blk, start = struct.unpack('>IH', d[1024 + 20:1024 + 24] + d[1024 + 28:1024 + 30])
    first, count = struct.unpack('>HH', d[1024 + 150:1024 + 154])
    cat = start * 512 + first * blk
    for node in range(count * blk // 512):
        n = cat + node * 512
        if d[n + 8] != 0xFF:        # leaf nodes only
            continue
        for rec in range(struct.unpack('>H', d[n + 10:n + 12])[0]):
            off = struct.unpack('>H', d[n + 510 - 2 * rec:n + 512 - 2 * rec])[0]
            klen = d[n + off]
            parent = struct.unpack('>I', d[n + off + 2:n + off + 6])[0]
            body = n + off + 1 + klen + (1 + klen) % 2     # the record starts on an even byte
            if parent == 1 and d[body] == 1 and struct.unpack('>I', d[body + 6:body + 10])[0] == 2:
                flags = struct.unpack('>H', d[body + 30:body + 32])[0] | 0x0400
                d[body + 30:body + 32] = struct.pack('>H', flags)
                open(image, 'wb').write(d)
                return
    sys.exit('the disk image has no root folder record')


def disk_copy(raw, name):
    """A Disk Copy 4.2 image of an 800K raw disk: an 84-byte header (name, sizes, and the data
    and tag checksums), then the disk's 819200 bytes; no tags (their size and checksum are 0)"""
    def checksum(b):
        s = 0
        for i in range(0, len(b), 2):
            s = (s + ((b[i] << 8) | b[i + 1])) & 0xFFFFFFFF
            s = ((s >> 1) | ((s & 1) << 31)) & 0xFFFFFFFF
        return s
    n = name.encode('mac_roman')[:63]
    h = bytearray(84)
    h[0] = len(n)
    h[1:1 + len(n)] = n
    h[64:68] = struct.pack('>I', len(raw))
    h[72:76] = struct.pack('>I', checksum(raw))
    h[80] = 1               # 800K
    h[81] = 0x22            # double-sided Macintosh format
    h[82:84] = b'\x01\x00'
    return bytes(h) + raw


def need(path, how):
    if not os.path.exists(path):
        sys.exit('%s is missing: %s' % (os.path.relpath(path, root), how))
    return path


def main():
    v = kit.version()
    mill = need(os.path.join(root, 'build', 'Mill.bin'), './build.sh')
    fonts = need(os.path.join(root, 'build', 'fonts', 'Web Fonts.bin'), 'tools/extras/build.py --styles --out build/fonts')
    inet = need(os.path.join(root, 'plugins', 'internet', 'build', 'Internet Services.bin'), 'plugins/internet/build.sh')
    out = os.path.join(root, 'build', 'Mill-disk.dsk')       # raw, for hfsutils; dist gets the Disk Copy image
    image = os.path.join(kit.dist, 'Mill-%s.image' % v)
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
        icon = os.path.join(tmp, 'Icon.bin')
        icon_file(icon)
        kit.hfs('hcopy', '-m', icon, ':')
        before = free_bytes()
        kit.copy_kit(':Mill Plug-In SDK')
        items.append(('Mill Plug-In SDK (the SDK disk\'s files)', before - free_bytes()))
        free = free_bytes()
    finally:
        kit.hfs('humount')
    custom_icon_flag(out)
    open(image, 'wb').write(disk_copy(open(out, 'rb').read(), 'Mill'))
    print('%s' % image)
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
