#!/usr/bin/env python3
'''Makes Mill's release files: dist/Install-Mill-VERSION.bin (MacBinary, with the Finder's "has
bundle" flag set so its icon shows) and .hqx (BinHex 4.0), from the installer ./build.sh installer
makes. The disk image, dist/Mill-VERSION.image, is made by ./build.sh disk. The version comes from
src/version.h. Inside the .bin and .hqx the application is named "Install Mill VERSION".
python3 tools/release.py [build dir] [dist dir]'''

import os, re, struct, sys

here = os.path.dirname(os.path.abspath(__file__))
build = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', 'build')
dist = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, '..', 'dist')


def version():
    h = open(os.path.join(here, '..', 'src', 'version.h')).read()
    return re.search(r'#define MILL_VERSION "([^"]+)"', h).group(1)


def crc16(data, crc=0):
    '''CRC-CCITT (XMODEM), as MacBinary II and BinHex use'''
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def with_bundle(mb, name):
    '''The MacBinary file named name, with "has bundle" set and "inited" clear, and its
    header CRC redone'''
    b = bytearray(mb)
    n = name.encode('mac_roman')
    assert len(n) <= 63
    b[1] = len(n)
    b[2:65] = n + b'\0' * (63 - len(n))
    b[73] = (b[73] | 0x20) & ~0x01
    crc = crc16(b[:124])
    b[124], b[125] = crc >> 8, crc & 0xFF
    return bytes(b)


def forks(mb):
    name = mb[2:2 + mb[1]]
    dlen, rlen = struct.unpack('>II', mb[83:91])
    data = mb[128:128 + dlen]
    rsrc_at = 128 + (dlen + 127) // 128 * 128
    rsrc = mb[rsrc_at:rsrc_at + rlen]
    flags = (mb[73] << 8) | mb[101]
    return name, mb[65:69], mb[69:73], flags, data, rsrc


def rle90(data):
    '''BinHex's run-length step: a run is the byte, 0x90, and its count; 0x90 itself is 0x90 0'''
    out, i = bytearray(), 0
    while i < len(data):
        b, n = data[i], 1
        while i + n < len(data) and data[i + n] == b and n < 255:
            n += 1
        out += b'\x90\x00' if b == 0x90 else bytes([b])
        if n >= 3:
            out += bytes([0x90, n])
        elif n == 2:
            out += b'\x90\x00' if b == 0x90 else bytes([b])
        i += n
    return bytes(out)


ALPHABET = b'!"#$%&\'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr'


def binhex(mb):
    '''BinHex 4.0 text for a MacBinary file'''
    name, ftype, creator, flags, data, rsrc = forks(mb)
    head = bytes([len(name)]) + name + b'\0' + ftype + creator + struct.pack('>HII', flags, len(data), len(rsrc))
    raw = bytearray()
    for part in (head, data, rsrc):
        # (BinHex describes it over the bytes and two zeros, bit by bit; this direct
        # form gives the same number from the bytes alone)
        raw += part + struct.pack('>H', crc16(part))
    packed = rle90(bytes(raw))
    chars = bytearray()
    for i in range(0, len(packed), 3):
        group = packed[i:i + 3]
        v = int.from_bytes(group + b'\0' * (3 - len(group)), 'big')
        n = len(group) + 1      # 1 byte -> 2 chars, 2 -> 3, 3 -> 4
        for k in range(n):
            chars.append(ALPHABET[(v >> (18 - 6 * k)) & 63])
    text = b':' + bytes(chars) + b':'
    lines = [text[i:i + 64] for i in range(0, len(text), 64)]
    return b'(This file must be converted with BinHex 4.0)\r\r' + b'\r'.join(lines) + b'\r'


def main():
    v = version()
    os.makedirs(dist, exist_ok=True)
    mb = with_bundle(open(os.path.join(build, 'installer', 'Install.bin'), 'rb').read(), 'Install Mill %s' % v)
    base = os.path.join(dist, 'Install-Mill-%s' % v)
    open(base + '.bin', 'wb').write(mb)
    open(base + '.hqx', 'wb').write(binhex(mb))
    for ext in ('bin', 'hqx'):
        print('%s.%s: %d bytes' % (base, ext, os.path.getsize(base + '.' + ext)))


if __name__ == '__main__':
    main()
