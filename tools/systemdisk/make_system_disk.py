#!/usr/bin/env python3
# Copyright (c) 2026 OneInAMillion Software
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is part of tools/systemdisk, which is licensed separately from the rest of Mill.
# It is free software: you can redistribute it and/or modify it under the terms of the GNU
# Lesser General Public License as published by the Free Software Foundation, either version
# 2.1 of the License, or (at your option) any later version. It is distributed in the hope
# that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
# License, in LICENSE beside this file, for more details.
"""Make the "Mill System Disk": an 800K startup floppy of System 6.0.5, trimmed for a 1 MB Mac Plus,
from Apple's own System 6.0.5 disk set, with MacTCP 2.0.6 from Apple's System 7.5 Update 2.0.
See "Read Me" beside this script.

    python3 tools/systemdisk/make_system_disk.py OUTDIR [--personal | --atimagewriter]
                                                        [--mactcp PATH | --no-mactcp]
                                                        [--all-disks] [--cache DIR]

There are two editions. The Network Edition (#1E6-DM01-N, the default) has AppleShare; the
Personal Edition (#1E6-DM01-P, --personal) has MacPPP 2.0.1 in its place, for a modem. It
writes OUTDIR/Mill-System-Disk-Network.image (Disk Copy 4.2) and
OUTDIR/Mill-System-Disk-Network.dsk (the same disk, raw), or the same with -Personal. Steps:

1. Makes a virtual environment in the cache folder (tools/systemdisk/cache unless --cache) and
   installs pinned, hash-checked copies of machfs and macresources into it, then runs itself
   again from there. Nothing else is needed but Python 3.
2. Downloads the four SSW_6.0.5_DiskNof4.sea.bin files from the Internet Archive's copies of
   Apple's old download site, into the cache, and refuses any whose SHA-256 is not the one
   below.
3. Unpacks each: MacBinary, holding a StuffIt self-extracting archive, holding one Disk Copy
   4.2 image (stuffit.py, beside this script, does the StuffIt part), and checks every CRC
   and checksum along the way.
4. Downloads Sys_7.5_Update_2.0-03of14.sea.bin, the third of the fourteen disks of System 7.5
   Update 2.0, the same way, and unpacks it the same way to a 1440K disk. Its Installer tome,
   "System 7.5 Update 2.0 Tome 3", holds MacTCP 2.0.6, compressed; tome.py, beside this
   script, takes it out, and its SHA-256 must be the one below.
5. Builds the disk from the System Tools, Utilities 1, Utilities 2, and Printing Tools disks:
   - System Folder: 6.0.5's System, less the Mac II's ROM patch (PTCH 376), the Mac II video
     driver (DRVR 120), and the alert sounds Clink-Klank, Boing, and Monkey ('snd ' 2 to 4);
     plus AppleShare's .XPP and .AFPTranslator drivers, which its Installer adds to the
     System (taken from the System on Utilities 1, which has them);
   - Finder, General, Clipboard File, Keyboard, Mouse, Sound, Map, MacTCP, AppleShare,
     LaserWriter, Laser Prep, and ImageWriter;
   - 6.0.5's boot blocks. No MultiFinder, and the fonts 6.0.5 ships with.
   --atimagewriter puts AppleTalk ImageWriter in place of AppleShare (and its drivers).
6. For the Personal Edition, leaves out AppleShare and its drivers, and adds PPP and Config
   PPP from MacPPP 2.0.1. It downloads mac-ppp-201.hqx from the Info-Mac archive at FUNET and
   refuses it unless its SHA-256 is the one below, decodes the BinHex (checking its CRCs),
   unpacks the StuffIt archive in it, and checks the SHA-256s of both files. They go on the
   disk as they are: MacPPP's license does not allow changing it.
   Nothing refers to the dropped sounds by number: the Sound control panel lists the System's
   'snd ' resources by index, and the system beep falls back to Simple Beep.

MacTCP makes MacTCP DNR itself at the first startup. --mactcp PATH uses your own copy of
MacTCP in place of the download: a MacBinary file of it, a folder of MacBinary files with it,
or a disk image (raw or Disk Copy 4.2) with it on. A MacTCP DNR beside it comes along too.
--no-mactcp leaves MacTCP off.

--all-disks also writes Apple's four disks to OUTDIR as raw images.
"""

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))

# What the virtual environment holds: both MIT-licensed, pure Python, from PyPI
REQUIREMENTS = '''\
machfs==1.3 \\
    --hash=sha256:17a87350c46d4542c74f1d7e2045201ff90d73906e2021b3864a4f9a7f8e8ff7 \\
    --hash=sha256:89201818ca16c2385be234b5bfe90946cf80e8141dfa4ed42a54dfa164690389
macresources==1.2 \\
    --hash=sha256:6aac6ee6c8170ad0d7e2b3ee810b97eede3bd6ff421097dfb59fc7b88c53f9fb \\
    --hash=sha256:f5a1465e02b11e6bc2cb9f2221e0cfac90d57d294b6baa73c28035b000c3dec4
'''

APPLE = ('http://download.info.apple.com/Apple_Support_Area/Apple_Software_Updates/'
         'English-North_American/Macintosh/System/Older_System/System_6.0.x/')

# Apple's System 6.0.5 disk set: (file, Wayback Machine capture, size, SHA-256, the disk in it)
DISKS = (
    ('SSW_6.0.5_Disk1of4.sea.bin', '20150608125422', 465152,
     '48e4dd29955bd52d5ccbd092d9909b60eeeb1fa989a7b027267095fac857eb6d', 'System Tools'),
    ('SSW_6.0.5_Disk2of4.sea.bin', '20150608125553', 477696,
     '37bb2dc319fb6bb4267fee0fa2e62cb7212188cfc0e2091edc034bcc28e37626', 'Utilities 1'),
    ('SSW_6.0.5_Disk3of4.sea.bin', '20150608125347', 414208,
     'e64011db59ffb9f0b7cc1d30579ab9dc2efe1234da57be51dd395dd9c7a3ea54', 'Utilities 2'),
    ('SSW_6.0.5_Disk4of4.sea.bin', '20150608125627', 437760,
     'bb97d7b198b77456699368c1da6e6924f7aefcefc7686d2d6824d7d16563418f', 'Printing Tools'),
)

# MacTCP 2.0.6, from the third disk of Apple's System 7.5 Update 2.0 (Apple's free update to
# System 7.5.3): (file, Wayback Machine capture, size, SHA-256, the disk in it), the tome on
# that disk, and the SHA-256 of MacTCP's resource fork once unpacked (it has no data fork)
UPDATE = ('http://download.info.apple.com/Apple_Support_Area/Apple_Software_Updates/'
          'English-North_American/Macintosh/System/System_7.5_Update_2.0/')
MACTCP_DISK = ('Sys_7.5_Update_2.0-03of14.sea.bin', '20150608125113', 1206272,
               '2cfe6f32a287947da266f676ee3c6a8208707a7b98c8ded659bcd0ca954b9c5f',
               'System 7.5 Update 2.0 - 3')
MACTCP_TOME = 'System 7.5 Update 2.0 Tome 3'
MACTCP_SHA256 = '37fa3b9435ad792da3d49abdfcbd42f9d298e90e20a6e6aa0b40c8c5a70433c3'

# MacPPP 2.0.1, from Merit Network and the University of Michigan, as the Info-Mac archive
# keeps it (BinHex, holding a StuffIt archive with one folder): its URL, size, and SHA-256, the
# folder, and for PPP and Config PPP their type, creator, and the SHA-256 of their resource
# forks once unpacked (neither has a data fork)
MACPPP_URL = 'ftp://ftp.funet.fi/pub/mac/info-mac/comm/inet/conn/ppp/mac-ppp-201.hqx'
MACPPP_SIZE = 56140
MACPPP_SHA256 = 'dca3a36474b4faba2d82087dbf917fe1f64b5d8738c2dcd1b0eb7a3d4854a40c'
MACPPP_FOLDER = 'MacPPP 2.0.1'
MACPPP_FILES = (
    ('PPP', b'mdev', b'PPP ', 'c5255d0291632555c235cdaf5c80b8938f125449dfc69b52afa087cc9d1d20f6'),
    ('Config PPP', b'cdev', b'Mppp', 'e04a5fe2e56e9ca1012caa02869265e348bcb6e54e8df481697cd54af8211bc4'),
)

VOLUME = 'Mill System Disk'
OUTNAME = 'Mill-System-Disk'
EDITIONS = {'Network': '#1E6-DM01-N', 'Personal': '#1E6-DM01-P'}    # their part numbers
DISK_BYTES = 819200
HD_BYTES = 1474560              # a 1440K disk
ROOM_FOR_DNR = 20 * 1024        # MacTCP DNR needs about this much free to make itself at startup


def main():
    p = argparse.ArgumentParser(description='Make the Mill System Disk from Apple\'s own downloads.')
    p.add_argument('outdir', help='folder for the .image and .dsk files')
    e = p.add_mutually_exclusive_group()
    e.add_argument('--personal', action='store_true',
                   help='make the Personal Edition (MacPPP in place of AppleShare)')
    e.add_argument('--atimagewriter', action='store_true',
                   help='AppleTalk ImageWriter in place of AppleShare')
    m = p.add_mutually_exclusive_group()
    m.add_argument('--mactcp', metavar='PATH',
                   help='use your own MacTCP instead of downloading it: a MacBinary file, a folder '
                        'of them, or a disk image')
    m.add_argument('--no-mactcp', action='store_true', help='leave MacTCP off')
    p.add_argument('--all-disks', action='store_true', help="also write Apple's four disks as raw images")
    p.add_argument('--cache', metavar='DIR', default=os.path.join(HERE, 'cache'),
                   help='where the virtual environment and downloads go (default: %(default)s)')
    args = p.parse_args()
    if args.personal and args.no_mactcp:
        p.error('the Personal Edition requires MacTCP: MacPPP works through it')
    cache = os.path.abspath(args.cache)
    edition = 'Personal' if args.personal else 'Network'

    use_venv(os.path.join(cache, 'venv'))       # from here on, inside it

    sys.path.insert(0, HERE)
    import stuffit

    downloads = os.path.join(cache, 'downloads')
    if args.no_mactcp:
        mactcp = None
    elif args.mactcp:
        mactcp = find_mactcp(args.mactcp)
    else:
        mactcp = apple_mactcp(downloads, stuffit)
    ppp = macppp(downloads, stuffit) if args.personal else None
    os.makedirs(args.outdir, exist_ok=True)
    disks = {}
    for name, stamp, size, sha, label in DISKS:
        sea = download(wayback(stamp, APPLE, name), name, size, sha, downloads)
        disks[label] = unpack(sea, name, label, DISK_BYTES, stuffit)
        if args.all_disks:
            write(os.path.join(args.outdir, label + '.dsk'), disks[label])
    if args.personal:
        chooser = None
    else:
        chooser = 'AppleTalk ImageWriter' if args.atimagewriter else 'AppleShare'
    raw = build(disks, mactcp, chooser, ppp)
    outname = '%s-%s' % (OUTNAME, edition)
    write(os.path.join(args.outdir, outname + '.dsk'), raw)
    write(os.path.join(args.outdir, outname + '.image'), disk_copy(raw, VOLUME))
    free = free_bytes(raw)
    print('%s, %s Edition (%s): %dK free' % (VOLUME, edition, EDITIONS[edition], free // 1024))
    if mactcp is None:
        print('No MacTCP (--no-mactcp): see "MacTCP" in tools/systemdisk/Read Me.')
    elif 'MacTCP DNR' not in mactcp and free < ROOM_FOR_DNR:
        print('Warning: MacTCP requires about %dK free to make MacTCP DNR at startup.' % (ROOM_FOR_DNR // 1024))


# --- the virtual environment ---

def use_venv(where):
    """Carry on inside a virtual environment at `where` that has REQUIREMENTS, making it first
    if need be. Returns only when this process is already running inside it."""
    python = os.path.join(where, 'Scripts', 'python.exe') if os.name == 'nt' else os.path.join(where, 'bin', 'python')
    stamp = os.path.join(where, 'mill-requirements.txt')
    ready = os.path.exists(python) and os.path.exists(stamp) and open(stamp).read() == REQUIREMENTS
    if ready and os.path.realpath(sys.prefix) == os.path.realpath(where):
        return
    if os.environ.get('MILL_SYSTEMDISK_VENV') == where:
        sys.exit('The virtual environment at %s does not work; delete it and try again.' % where)
    if not ready:
        import venv
        print('Making a virtual environment in %s' % where, flush=True)
        venv.EnvBuilder(with_pip=True, clear=True).create(where)
        req = os.path.join(where, 'requirements.txt')
        with open(req, 'w') as f:
            f.write(REQUIREMENTS)
        subprocess.run([python, '-m', 'pip', 'install', '--quiet', '--disable-pip-version-check',
                        '--require-hashes', '--only-binary', ':all:', '-r', req], check=True)
        with open(stamp, 'w') as f:
            f.write(REQUIREMENTS)
    os.environ['MILL_SYSTEMDISK_VENV'] = where
    sys.stdout.flush()
    argv = [python, '-I', os.path.abspath(__file__)] + sys.argv[1:]
    if os.name == 'nt':                         # no real exec on Windows
        sys.exit(subprocess.run(argv).returncode)
    os.execv(python, argv)


# --- Apple's disks ---

def wayback(stamp, base, name):
    """The Internet Archive's copy of base + name, as it was at `stamp`"""
    return 'https://web.archive.org/web/%sid_/%s%s' % (stamp, base, name)


def download(url, name, size, sha, folder):
    """The path of `name` in the downloads folder, fetched from `url` unless it is there
    already; either way its SHA-256 must be `sha`"""
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, name)
    if os.path.exists(path):
        if sha256(path) == sha:
            return path
        print('%s in the cache is not the right file; fetching it again' % name)
        os.remove(path)
    print('Fetching %s' % url)
    part = path + '.part'
    req = urllib.request.Request(url, headers={'User-Agent': 'make_system_disk.py (Mill System Disk builder)'})
    try:
        with urllib.request.urlopen(req, timeout=120) as r, open(part, 'wb') as f:
            while True:
                chunk = r.read(65536)
                if not chunk:
                    break
                f.write(chunk)
                if f.tell() > size:
                    break
    except OSError as e:                        # urllib's errors included
        if os.path.exists(part):
            os.remove(part)
        sys.exit('Could not fetch %s: %s' % (url, e))
    if os.path.getsize(part) != size or sha256(part) != sha:
        os.remove(part)
        sys.exit('%s is not the expected file (size or SHA-256 differs); refusing it.' % url)
    os.replace(part, path)
    return path


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(65536), b''):
            h.update(chunk)
    return h.hexdigest()


def unpack(sea, name, label, disk_bytes, stuffit):
    """The raw disk (800K or 1440K) inside one of Apple's .sea.bin files"""
    b = open(sea, 'rb').read()
    info = macbinary(b)
    if info is None:
        sys.exit('%s is not a MacBinary file' % name)
    try:
        entries = stuffit.read_archive(info['data'])
    except stuffit.StuffItError as e:
        sys.exit('%s: %s' % (name, e))
    if len(entries) != 1 or entries[0].type != b'dImg':
        sys.exit('%s: expected one Disk Copy image' % name)
    raw, volname = disk_copy_data(entries[0].data, name)
    if volname != label or len(raw) != disk_bytes:
        sys.exit('%s: expected the %dK disk "%s", found "%s"' % (name, disk_bytes // 1024, label, volname))
    print('%s: %s, %d bytes, CRCs and checksums correct' % (name, entries[0].name, len(raw)))
    return raw


def macbinary(b):
    """A MacBinary file's name, Finder info, and forks, as a dict; None if `b` isn't one"""
    if len(b) < 128 or b[0] != 0 or b[74] != 0 or not 1 <= b[1] <= 63:
        return None
    dlen, rlen = struct.unpack('>II', b[83:91])
    r0 = 128 + (dlen + 127) // 128 * 128
    if 128 + dlen > len(b) or (rlen and r0 + rlen > len(b)):
        return None
    if b[82] == 0 and b[122] >= 129 and crc16_xmodem(b[:124]) != struct.unpack('>H', b[124:126])[0]:
        return None                             # MacBinary II or III, header CRC wrong
    created, modified = struct.unpack('>II', b[91:99])
    return {'name': b[2:2 + b[1]].decode('mac_roman'), 'type': b[65:69], 'creator': b[69:73],
            'flags': (b[73] << 8) | b[101], 'created': created, 'modified': modified,
            'data': b[128:128 + dlen], 'rsrc': b[r0:r0 + rlen]}


BINHEX = {c: i for i, c in enumerate(
    b'!"#$%&\'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr')}


def binhex(text, what):
    """A BinHex 4.0 file's name, Finder info, and forks, as a dict like macbinary()'s; checks
    its three CRCs. After the line "(This file must be converted with BinHex 4.0)", the file is
    between two colons, six bits to a character (BINHEX), with line breaks to ignore. Those
    bytes are run-length coded: 0x90 then n repeats the byte before to n in all, 0x90 then 0
    is 0x90 itself. What they code is a header (the name as a Pascal string, a 0, type,
    creator, Finder flags (2), data fork length (4), resource fork length (4)), then the data
    fork, then the resource fork, each followed by its CRC-16/XMODEM (2)."""
    i = text.find(b'(This file must be converted with BinHex 4.0)')
    start = text.find(b':', i) + 1 if i >= 0 else 0
    end = text.find(b':', start) if start else -1
    if end < 0:
        sys.exit('%s: not a BinHex 4.0 file' % what)
    six, acc, n = bytearray(), 0, 0
    for c in text[start:end]:
        if c in b'\r\n\t ':
            continue
        if c not in BINHEX:
            sys.exit('%s: invalid BinHex character' % what)
        acc, n = (acc << 6) | BINHEX[c], n + 6
        if n >= 8:
            n -= 8
            six.append((acc >> n) & 0xFF)
    b, k = bytearray(), 0
    while k < len(six):
        x = six[k]
        k += 1
        if x != 0x90:
            b.append(x)
        elif k == len(six) or (six[k] and not b):
            sys.exit('%s: BinHex run-length code is cut short' % what)
        else:
            b += b'\x90' if six[k] == 0 else bytes([b[-1]]) * (six[k] - 1)
            k += 1
    try:
        p = 1 + b[0]
        if b[p] != 0:
            sys.exit('%s: not a BinHex 4.0 file' % what)
        flags, dlen, rlen = struct.unpack('>HII', b[p + 9:p + 19])
        info = {'name': b[1:p].decode('mac_roman'), 'type': bytes(b[p + 1:p + 5]),
                'creator': bytes(b[p + 5:p + 9]), 'flags': flags}
        p += 19
        for key, start, length in ((None, 0, p), ('data', p + 2, dlen), ('rsrc', p + 4 + dlen, rlen)):
            part = bytes(b[start:start + length])
            crc, = struct.unpack('>H', b[start + length:start + length + 2])
            if len(part) != length or crc16_xmodem(part) != crc:
                sys.exit('%s: BinHex %s CRC does not match' % (what, key + ' fork' if key else 'header'))
            if key:
                info[key] = part
    except (IndexError, struct.error):
        sys.exit('%s: BinHex file is cut short' % what)
    return info


def crc16_xmodem(data):
    """CRC-16/XMODEM, the MacBinary II header CRC and BinHex's CRC"""
    crc = 0
    for x in data:
        crc ^= x << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


# --- Disk Copy 4.2 ---
# An 84-byte header: name (Pascal string, 63 characters at most), data size (4), tag size (4),
# data checksum (4), tag checksum (4), disk format (1: 0 400K, 1 800K, 2 720K, 3 1440K),
# format byte (1), 0x0100; then the disk's sectors, then 12 bytes of tags for each sector.

def dc42_checksum(b):
    """Disk Copy 4.2's checksum: add each big-endian 16-bit word, then rotate right one bit"""
    s = 0
    for (w,) in struct.iter_unpack('>H', b):
        s = (s + w) & 0xFFFFFFFF
        s = (s >> 1) | ((s & 1) << 31)
    return s


def disk_copy_data(img, what):
    """The raw disk in a Disk Copy 4.2 image, and the image's name; checks both checksums"""
    if len(img) < 84 or img[82:84] != b'\x01\x00':
        sys.exit('%s: not a Disk Copy 4.2 image' % what)
    dsize, tsize, dsum, tsum = struct.unpack('>IIII', img[64:80])
    if len(img) != 84 + dsize + tsize:
        sys.exit('%s: Disk Copy image is the wrong size' % what)
    data, tags = img[84:84 + dsize], img[84 + dsize:]
    if dc42_checksum(data) != dsum:
        sys.exit('%s: Disk Copy data checksum does not match' % what)
    if tsize and dc42_checksum(tags[12:]) != tsum:      # the first sector's tags are left out
        sys.exit('%s: Disk Copy tag checksum does not match' % what)
    return data, img[1:1 + min(img[0], 63)].decode('mac_roman')


def disk_copy(raw, name):
    """A Disk Copy 4.2 image of an 800K raw disk, with no tags"""
    n = name.encode('mac_roman')[:63]
    h = bytearray(84)
    h[0] = len(n)
    h[1:1 + len(n)] = n
    h[64:68] = struct.pack('>I', len(raw))
    h[72:76] = struct.pack('>I', dc42_checksum(raw))
    h[80] = 1                                   # 800K
    h[81] = 0x22                                # double-sided Macintosh format
    h[82:84] = b'\x01\x00'
    return bytes(h) + raw


# --- the disk ---

def volume(raw):
    import machfs
    v = machfs.Volume()
    v.read(raw)
    return v


def build(disks, mactcp, chooser, ppp):
    """The Mill System Disk, raw. `chooser` is 'AppleShare', 'AppleTalk ImageWriter', or None,
    and `ppp` MacPPP's {name: File}, or None."""
    import machfs
    import macresources
    st = volume(disks['System Tools'])
    u1 = volume(disks['Utilities 1'])
    u2 = volume(disks['Utilities 2'])
    pt = volume(disks['Printing Tools'])
    SF, U1, U2 = st['System Folder'], u1['System Folder'], u2['System Folder Additions']

    # The System, less what a Mac Plus never uses
    drop = {(b'PTCH', 376), (b'DRVR', 120), (b'snd ', 2), (b'snd ', 3), (b'snd ', 4)}
    rs = list(macresources.parse_file(SF['System'].rsrc))
    have = {(r.type, r.id) for r in rs}
    if not drop <= have:
        sys.exit('System Tools: the System is not the expected one')
    keep = [r for r in rs if (r.type, r.id) not in drop]
    if chooser == 'AppleShare':
        # AppleShare's drivers, as its Installer puts them in the System. The System on
        # Utilities 1 has them, set up just as the Installer leaves them.
        u1sys = {(r.type, r.id): r for r in macresources.parse_file(U1['System'].rsrc)}
        for key in ((b'DRVR', 40), (b'DRVR', 41), (b'ALRT', -15072), (b'DITL', -15072), (b'STR#', -15072)):
            if key in have or key not in u1sys:
                sys.exit('Utilities 1: the System is not the expected one')
            keep.append(u1sys[key])
    system = machfs.File()
    system.type, system.creator, system.flags = SF['System'].type, SF['System'].creator, SF['System'].flags
    system.data = SF['System'].data
    system.rsrc = macresources.make_file(keep)

    out = machfs.Volume()
    out.name = VOLUME
    sf = machfs.Folder()
    out['System Folder'] = sf
    sf['System'] = system
    for k, where in (('Finder', SF), ('General', SF), ('Clipboard File', SF), ('Keyboard', U1),
                     ('Mouse', U1), ('Sound', U1), ('Map', U2)):
        sf[k] = where[k]
    for k in ('MacTCP', 'MacTCP DNR'):
        if k in (mactcp or {}):
            sf[k] = mactcp[k]
    if chooser:
        sf[chooser] = (U1 if chooser == 'AppleShare' else pt)[chooser]
    for k, f in (ppp or {}).items():
        sf[k] = f
    for k in ('LaserWriter', 'Laser Prep', 'ImageWriter'):
        sf[k] = pt[k]
    # Each file keeps its place in the window it came from, so many would sit on top of
    # one another: put them in rows of five instead, in the order above, and mark them all
    # placed ("inited"), or the Finder finds its own place for them. (machfs's x and y are
    # the Finder's v and h, in that order.)
    for i, f in enumerate(sf.values()):
        f.x, f.y = 10 + 64 * (i // 5), 16 + 76 * (i % 5)
        f.flags |= 0x0100
    try:
        raw = bytearray(out.write(DISK_BYTES))
    except machfs.main.OutOfSpaceError:
        sys.exit('It does not fit on an 800K disk.')
    raw[:1024] = disks['System Tools'][:1024]   # 6.0.5's boot blocks
    return bytes(raw)


def free_bytes(raw):
    """Free space on a raw HFS disk, from its Master Directory Block"""
    alloc_size, = struct.unpack('>I', raw[1024 + 20:1024 + 24])
    free_blocks, = struct.unpack('>H', raw[1024 + 34:1024 + 36])
    return alloc_size * free_blocks


def apple_mactcp(downloads, stuffit):
    """{'MacTCP': File}, MacTCP 2.0.6 from Apple's System 7.5 Update 2.0"""
    import machfs
    import tome
    name, stamp, size, sha, label = MACTCP_DISK
    sea = download(wayback(stamp, UPDATE, name), name, size, sha, downloads)
    v = volume(unpack(sea, name, label, HD_BYTES, stuffit))
    t = v.get(MACTCP_TOME)
    if not isinstance(t, machfs.File) or (t.type, t.creator) != (b'idcp', b'kakc'):
        sys.exit('%s: no Installer tome "%s"' % (name, MACTCP_TOME))
    try:
        e = [e for e in tome.read_catalog(t.data) if e.name == 'MacTCP']
        if len(e) != 1 or (e[0].type, e[0].creator) != (b'cdev', b'ztcp') or e[0].data[0]:
            sys.exit('%s: the tome does not have the expected MacTCP' % MACTCP_TOME)
        e = e[0]
        rsrc = tome.read_fork(t.data, e.rsrc, 'MacTCP')
    except tome.TomeError as x:
        sys.exit('%s: %s' % (MACTCP_TOME, x))
    if hashlib.sha256(rsrc).hexdigest() != MACTCP_SHA256:
        sys.exit('%s: MacTCP did not unpack to the expected file (SHA-256 differs)' % MACTCP_TOME)
    f = machfs.File()
    f.type, f.creator, f.flags = e.type, e.creator, e.flags
    f.crdate, f.mddate = e.created, e.modified
    f.rsrc = rsrc
    print('%s: MacTCP %s, %d bytes, SHA-256 correct' % (MACTCP_TOME, mactcp_version(rsrc), len(rsrc)))
    return {'MacTCP': f}


def macppp(downloads, stuffit):
    """{'PPP': File, 'Config PPP': File}, from MacPPP 2.0.1 in the Info-Mac archive"""
    import machfs
    name = MACPPP_URL.rsplit('/', 1)[1]
    hqx = download(MACPPP_URL, name, MACPPP_SIZE, MACPPP_SHA256, downloads)
    info = binhex(open(hqx, 'rb').read(), name)
    try:
        entries = stuffit.read_archive(info['data'])
    except stuffit.StuffItError as e:
        sys.exit('%s: %s' % (name, e))
    found = {}
    for k, type, creator, sha in MACPPP_FILES:
        e = [e for e in entries if (e.path, e.name) == ((MACPPP_FOLDER,), k)]
        if (len(e) != 1 or (e[0].type, e[0].creator) != (type, creator) or e[0].data
                or hashlib.sha256(e[0].rsrc).hexdigest() != sha):
            sys.exit('%s: %s did not unpack to the expected file (SHA-256 differs)' % (name, k))
        e = e[0]
        f = machfs.File()
        f.type, f.creator, f.flags = e.type, e.creator, e.flags
        f.crdate, f.mddate = e.created, e.modified
        f.rsrc = e.rsrc
        found[k] = f
    print('%s: %s, CRCs and SHA-256s correct' % (name, ' and '.join(found)))
    return found


def mactcp_version(rsrc):
    """The version in a resource fork's 'vers' 1, as text"""
    import macresources
    for r in macresources.parse_file(rsrc):
        if (r.type, r.id) == (b'vers', 1):
            return r.data[7:7 + r.data[6]].decode('mac_roman')
    return '(no version)'


def find_mactcp(path):
    """{'MacTCP': File} from the user's copy, with 'MacTCP DNR' too if it is beside it"""
    import machfs
    found = {}

    def from_macbinary(info):
        f = machfs.File()
        f.type, f.creator = info['type'], info['creator']
        f.flags = info['flags']
        f.crdate, f.mddate = info['created'], info['modified']
        f.data, f.rsrc = info['data'], info['rsrc']
        return f

    def from_folder(folder):                    # a folder on a disk image: the first with MacTCP
        if isinstance(folder.get('MacTCP'), machfs.File):
            found['MacTCP'] = folder['MacTCP']
            if isinstance(folder.get('MacTCP DNR'), machfs.File):
                found['MacTCP DNR'] = folder['MacTCP DNR']
            return True
        return any(from_folder(v) for k, v in folder.items() if isinstance(v, machfs.Folder))

    def from_file(p, data):
        info = macbinary(data)
        if info is not None:
            if info['name'] in ('MacTCP', 'MacTCP DNR'):
                found.setdefault(info['name'], from_macbinary(info))
            return
        if len(data) >= 84 and data[82:84] == b'\x01\x00' and data[0] < 64:
            data, _ = disk_copy_data(data, p)
        if any(data[i + 1024:i + 1026] == b'BD' for i in range(0, min(len(data), 1 << 20), 512)):
            try:
                v = volume(data)
            except Exception as e:
                sys.exit('%s: could not read the disk image (%s)' % (p, e))
            from_folder(v)

    if os.path.isdir(path):
        for dirpath, dirs, files in os.walk(path):
            dirs.sort()
            for name in sorted(files):
                p = os.path.join(dirpath, name)
                if os.path.getsize(p) <= 64 * 1024 * 1024:
                    from_file(p, open(p, 'rb').read())
    elif os.path.isfile(path):
        from_file(path, open(path, 'rb').read())
    else:
        sys.exit('--mactcp: %s is not there' % path)
    if 'MacTCP' not in found:
        sys.exit('--mactcp: no MacTCP in %s (a MacBinary file named MacTCP, or a disk image containing it)' % path)
    if found['MacTCP'].type != b'cdev':
        sys.exit('--mactcp: the MacTCP in %s is not a control panel' % path)
    return found


def write(path, b):
    with open(path, 'wb') as f:
        f.write(b)
    print('Wrote %s' % path)


if __name__ == '__main__':
    main()
