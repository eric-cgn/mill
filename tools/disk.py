#!/usr/bin/env python3
"""Mill's release disk, "Mill Program Disk": dist/Mill-VERSION.image, a Disk Copy 4.2 image of an
800K HFS disk with Mill ready to run, laid out as Easy Install puts it: Mill, and its Mill Plug-Ins
folder with the Plug-Ins and Web Fonts that installer/mill.ini's Easy Install puts there
(Internet Services, SMB, NFS, News, LDAP, IMAP, Image Formats, Archives, and Web Fonts). Also the
invisible Icon file that gives the disk its windmill icon, and the Desktop file System 6 needs
on a locked disk (Mini vMac mounts the image locked). Nothing of Apple's. The Plug-In SDK is not
on it: it is on its own disk (./build.sh kit) and in Install Mill's Custom Install.
Needs what ./build.sh makes first: build/Mill.bin, build/fonts/ (tools/webfonts/build.py --styles
--out build/fonts), plugins/internet/build/Internet Services.bin, plugins/smb/build/SMB.bin,
plugins/nfs/build/NFS.bin, plugins/news/build/News.bin, plugins/ldap/build/LDAP.bin,
plugins/imap/build/IMAP.bin, plugins/images/build/Image Formats.bin, and
plugins/archive/build/Archives.bin.
./build.sh disk does all of it."""
import os, re, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit
import icons

root = kit.root
VOLUME = 'Mill Program Disk'
CAPACITY = 800 * 1024


def part_number(v, disk='DM02'):
    """A Mill disk's part number, which follows Mill's version: #1E6-DM02-V21 for this disk with
    2.1. disk: DM02, this one (DM01 is the System Disk; the Millennium Pack's disks are mill-extras')"""
    return '#1E6-%s-V%s' % (disk, re.match(r'\d+(\.\d+)*', v).group(0).replace('.', ''))


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


def read_resources(fork):
    """{(type, id): data} from a resource fork"""
    dat, mp, dlen, mlen = struct.unpack('>IIII', fork[:16])
    tl = struct.unpack('>H', fork[mp + 24:mp + 26])[0]
    found = {}
    for i in range(struct.unpack('>H', fork[mp + tl:mp + tl + 2])[0] + 1):
        t, n, off = struct.unpack('>4sHH', fork[mp + tl + 2 + 8 * i:mp + tl + 10 + 8 * i])
        for j in range(n + 1):
            r = mp + tl + off + 12 * j
            rid = struct.unpack('>h', fork[r:r + 2])[0]
            d = dat + int.from_bytes(fork[r + 5:r + 8], 'big')
            found[(t, rid)] = fork[d + 4:d + 4 + struct.unpack('>I', fork[d:d + 4])[0]]
    return found


def desktop_file(path, app, parent=2):
    """MacBinary of the invisible "Desktop" file ('FNDR', 'ERIK') that System 6's Finder writes
    on every disk, and that a locked disk must already have: without it the Finder can't make one
    and ejects the disk. The icons come from the application's bundle (Mill's ICN#, FREF, and
    BNDL, here for the application at the top of the disk), as Finder 6.1.5 copies them: the
    local IDs run from 1 instead of 0, there is no ics#, and every copy has a new ID. The IDs and
    the 4-byte stamp are the ones it wrote for Mill on a disk of its own. The creator's resource
    ('MiLL', 0) is that stamp and the BNDL's ID, and 'APPL' says where the application is: its
    creator, the ID of its folder, and its name."""
    mb = open(app, 'rb').read()
    dlen, rlen = struct.unpack('>II', mb[83:91])
    r0 = 128 + (dlen + 127) // 128 * 128
    res = read_resources(mb[r0:r0 + rlen])
    sig, name = mb[69:73], mb[2:2 + mb[1]]
    bndl_id = 23395
    # (file type, ICN# in the application, new ICN# ID, new FREF ID), in the order of the bundle
    files = ((b'APPL', 128, 21677, 20798), (b'MPlg', 129, 18069, 29828), (b'FFIL', 130, 9702, 10388))
    bndl = sig + struct.pack('>HH', 0, 1)
    bndl += b'ICN#' + struct.pack('>H', len(files) - 1) + b''.join(struct.pack('>HH', i + 1, f[2]) for i, f in enumerate(files))
    bndl += b'FREF' + struct.pack('>H', len(files) - 1) + b''.join(struct.pack('>HH', i + 1, f[3]) for i, f in enumerate(files))
    resources = [(b'STR ', 0, b'\x0aFinder 1.0'), (b'BNDL', bndl_id, bndl)]
    resources += [(b'ICN#', f[2], res[(b'ICN#', f[1])]) for f in files]
    resources += [(b'FREF', f[3], f[0] + struct.pack('>H', i + 1) + b'\x00') for i, f in enumerate(files)]
    pstring = bytes([len(name)]) + name
    resources += [(sig, 0, bytes.fromhex('77231199') + struct.pack('>H', bndl_id)),
                  (b'APPL', 0, sig + struct.pack('>I', parent) + pstring + bytes(len(pstring) % 2))]
    fork = resource_fork(resources)
    h = bytearray(128)
    h[1] = 7
    h[2:9] = b'Desktop'
    h[65:69] = b'FNDR'
    h[69:73] = b'ERIK'
    h[73] = 0x40                    # invisible
    h[87:91] = struct.pack('>I', len(fork))
    h[122] = h[123] = 129
    h[124:126] = crc16(h[:124]).to_bytes(2, 'big')
    open(path, 'wb').write(bytes(h) + fork + bytes(-len(fork) % 128))


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


# Where the Finder shows the disk's icons (top, left, in the window) and the window (global rect):
# Mill on the left and its folder beside it
PLACES = {'Mill': (12, 24), 'Mill Plug-Ins': (12, 124)}
WINDOW = (70, 40, 250, 260)
# Folders on the disk whose window and icons are set too: {name: (window, icons a row)}; the
# icons go in name order, 80 apart, rows 64 apart
FOLDERS = {'Mill Plug-Ins': ((100, 40, 300, 470), 5)}


def catalog(d):
    """(parent ID, name, offset of the record's body) of each leaf record of the disk's catalog"""
    blk, start = struct.unpack('>IH', bytes(d[1024 + 20:1024 + 24] + d[1024 + 28:1024 + 30]))
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
            name = bytes(d[n + off + 7:n + off + 7 + d[n + off + 6]]).decode('mac_roman')
            yield parent, name, n + off + 1 + klen + (1 + klen) % 2     # the record starts on an even byte


def place(d, body, at):
    """An item's place in its window, (top, left), and "inited", so the Finder keeps it"""
    loc = body + 14 if d[body] == 2 else body + 32         # fdLocation, or a folder's frLocation
    fl = body + 12 if d[body] == 2 else body + 30          # fdFlags, frFlags
    d[loc:loc + 4] = struct.pack('>2h', *at)
    d[fl:fl + 2] = struct.pack('>H', struct.unpack('>H', d[fl:fl + 2])[0] | 0x0100)


def open_by_icon(d, body, window):
    """A folder's window: its rect, and frView 0x0100, by Icon. The Finder ignores both unless
    the folder is "inited"."""
    d[body + 22:body + 30] = struct.pack('>4h', *window)
    d[body + 30:body + 32] = struct.pack('>H', struct.unpack('>H', d[body + 30:body + 32])[0] | 0x0100)
    d[body + 36:body + 38] = struct.pack('>H', 0x0100)


def place_icons(image, places=None, window=None, folders=None):
    """Give the root's items their places, and the disk's window its size, by Icon; and the same
    for the folders in folders and their items: hfsutils can't write these Finder fields. places,
    window, and folders default to this disk's (PLACES, WINDOW, FOLDERS)"""
    places = PLACES if places is None else places
    window = WINDOW if window is None else window
    folders = FOLDERS if folders is None else folders
    d = bytearray(open(image, 'rb').read())
    placed, ids = set(), {}
    for parent, name, body in catalog(d):
        if parent == 1 and d[body] == 1:                    # the root folder: its window
            open_by_icon(d, body, window)
        elif parent == 2 and name in places and d[body] in (1, 2):
            place(d, body, places[name])
            placed.add(name)
            if d[body] == 1 and name in folders:
                open_by_icon(d, body, folders[name][0])
                ids[struct.unpack('>I', d[body + 6:body + 10])[0]] = name
    if placed != set(places):
        sys.exit('the disk image has no %s' % ', '.join(sorted(set(places) - placed)))
    inside = {}
    for parent, name, body in catalog(d):
        if parent in ids and d[body] in (1, 2):
            inside.setdefault(parent, []).append((name.lower(), body))
    for parent, items in inside.items():
        a_row = folders[ids[parent]][1]
        for i, (_, body) in enumerate(sorted(items)):
            place(d, body, (12 + 64 * (i // a_row), 24 + 80 * (i % a_row)))
    open(image, 'wb').write(d)


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


def easy_install_check():
    """Exit unless the disk's Mill Plug-Ins folder holds what installer/mill.ini's Easy Install
    puts there, so the two can't drift apart"""
    ini = open(os.path.join(root, 'installer', 'mill.ini'), encoding='utf-8').read()
    easy = set()
    for block in re.split(r'\n(?=\[)', ini):
        k = dict(re.findall(r'^(\w+)\s*=\s*(.*?)\s*$', block, re.M))
        if block.startswith('[component') and 'easy' in [t.strip() for t in k.get('types', '').split(',')] \
                and k.get('destination') == '%chosen%:Mill Plug-Ins':
            easy.add(k['source'][:-len('.bin')])
    if easy != set(PLUGINS):
        sys.exit('installer/mill.ini\'s Easy Install puts %s in Mill Plug-Ins; this disk has %s'
                 % (', '.join(sorted(easy)), ', '.join(sorted(PLUGINS))))


# The Mill Plug-Ins folder: (name, MacBinary path in the repository, type), Easy Install's
PLUGINS = {
    'Web Fonts': (('build', 'fonts', 'Web Fonts.bin'), b'FFIL'),
    'Internet Services': (('plugins', 'internet', 'build', 'Internet Services.bin'), b'MPlg'),
    'SMB': (('plugins', 'smb', 'build', 'SMB.bin'), b'MPlg'),
    'NFS': (('plugins', 'nfs', 'build', 'NFS.bin'), b'MPlg'),
    'News': (('plugins', 'news', 'build', 'News.bin'), b'MPlg'),
    'LDAP': (('plugins', 'ldap', 'build', 'LDAP.bin'), b'MPlg'),
    'IMAP': (('plugins', 'imap', 'build', 'IMAP.bin'), b'MPlg'),
    'Image Formats': (('plugins', 'images', 'build', 'Image Formats.bin'), b'MPlg'),
    'Archives': (('plugins', 'archive', 'build', 'Archives.bin'), b'MPlg'),
}


def need(path, how):
    if not os.path.exists(path):
        sys.exit('%s is missing: %s' % (os.path.relpath(path, root), how))
    return path


def main():
    v = kit.version()
    mill = need(os.path.join(root, 'build', 'Mill.bin'), './build.sh')
    easy_install_check()
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
        copies = [('Mill', bundled, (b'APPL', b'MiLL'), ':')]
        for name, (parts, ftype) in PLUGINS.items():
            how = 'tools/webfonts/build.py --styles --out build/fonts' if name == 'Web Fonts' else '%s/build.sh' % '/'.join(parts[:2])
            copies.append(('Mill Plug-Ins:' + name, need(os.path.join(root, *parts), how), (ftype, b'MiLL'), ':Mill Plug-Ins:'))
        for name, path, want, where in copies:
            t, c, dlen, rlen = macbinary_fields(path)
            if (t, c) != want:
                sys.exit('%s is type %s, creator %s; expected %s' % (name, t, c, want))
            kit.hfs('hcopy', '-m', path, where)
            items.append((name, dlen + rlen))
        icon = os.path.join(tmp, 'Icon.bin')
        icon_file(icon)
        kit.hfs('hcopy', '-m', icon, ':')
        desktop = os.path.join(tmp, 'Desktop.bin')
        desktop_file(desktop, bundled)
        kit.hfs('hcopy', '-m', desktop, ':')
        free = free_bytes()
    finally:
        kit.hfs('humount')
    custom_icon_flag(out)
    place_icons(out)
    open(image, 'wb').write(disk_copy(open(out, 'rb').read(), VOLUME))
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
