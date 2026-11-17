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
"""Apple Installer "tome" archives (type 'idcp', creator 'kakc'), as on the System 7.5 Update
2.0 disks, in pure Python. Enough to take one file out of one; nothing else.

    for entry in read_catalog(data): entry.name, entry.type, entry.creator, ...
    rsrc = read_fork(data, entry.rsrc, 'MacTCP')

The archive is in the tome's data fork (all numbers big-endian):
  header: 'kc', version 1 (2), ..., 24 offset of the catalog (2), 26 number of entries (2)
  catalog: 128 bytes for each file: 4 file number (2), 6 name (Pascal string, 31 characters
    at most), 38 type, 42 creator, 46 created, 50 modified, 54 version (2), 56 (2),
    58 Finder flags (2), then the data fork and the resource fork, 16 bytes each: length,
    offset of its compressed bytes in the archive, compressed length, checksum
  a compressed fork: blocks, each a 4-byte header (the most it unpacks to, 0x10000 so far)
    then a bit stream that ends on a byte boundary. Each block goes on from the last: one
    window, one output.

The compression is Apple's InstaCompOne, LZ77 with fixed prefix codes. The bit stream is read
most significant bit first. Tokens alternate loosely between literal runs and matches:
  - a match-length code; 0, when the last token was a match (or at the start),
    means a literal run follows: a run-length code, then that many bytes, 8 bits
    each. A run shorter than 63 must be followed by a match.
  - otherwise a match of the code's value plus 2 bytes, plus 3 after a literal run; then its
    distance (distance()), which depends on how much has been unpacked so far.
The codes come from macresources 1.2 (MIT licensed; Maxim Poliakovski and Elliot Nunn), whose
instacomp.py decodes the same compression in System 7.5's resources, and so do the distances
from 161 to 10752. The smaller and larger distances, the 43008-byte window, and the block
layout were worked out from Apple's MacTCP 2.0.6 in Tome 3. The distances are not all right
yet: about half the other files in that tome stop with an error partway. So the caller
checks what it takes out against a pinned SHA-256. The fork checksums in the catalog are
not checked: their algorithm is not known."""

import struct


class TomeError(Exception):
    pass


class Entry:
    def __init__(self, **kw):
        self.__dict__.update(kw)


def read_catalog(data):
    """The files in a tome's data fork"""
    if len(data) < 28 or data[:4] != b'kc\x00\x01':
        raise TomeError('not an Installer tome')
    at, count = struct.unpack_from('>HH', data, 24)
    if at + 128 * count > len(data):
        raise TomeError('the catalog is cut short')
    entries = []
    for i in range(count):
        e = data[at + 128 * i:at + 128 * (i + 1)]
        n = min(e[6], 31)
        crdate, mddate, version, _, flags = struct.unpack_from('>IIHHH', e, 46)
        entries.append(Entry(name=e[7:7 + n].decode('mac_roman'), type=e[38:42], creator=e[42:46],
                             created=crdate, modified=mddate, flags=flags,
                             data=struct.unpack_from('>IIII', e, 60),
                             rsrc=struct.unpack_from('>IIII', e, 76)))
    return entries


def read_fork(data, fork, what):
    """One fork, unpacked: `fork` is (length, offset, compressed length, checksum) from the
    catalog"""
    length, at, packed, _ = fork
    if length == 0:
        return b''
    if at + packed > len(data):
        raise TomeError('%s: cut short' % what)
    out = bytearray()
    pos, end, goal = at, at + packed, 0
    while len(out) < length:
        if pos + 4 > end:
            raise TomeError('%s: cut short' % what)
        block, = struct.unpack_from('>I', data, pos)
        if block == 0:
            raise TomeError('%s: an empty block' % what)
        goal = min(goal + block, length)
        pos = _block(data, pos + 4, end, out, goal, what)
    if pos != end:
        raise TomeError('%s: %d bytes left over' % (what, end - pos))
    del out[length:]                            # the last match can run past the end
    return bytes(out)


class _Bits:
    def __init__(self, data, pos, end):
        self.data, self.bit, self.end = data, pos * 8, end * 8

    def peek(self, n):
        b, d = self.bit, self.data
        if b + n > self.end:
            raise TomeError('the bit stream is cut short')
        i = b >> 3
        word = int.from_bytes(d[i:i + 4].ljust(4, b'\0'), 'big')
        return (word >> (32 - (b & 7) - n)) & ((1 << n) - 1)

    def get(self, n):
        if n == 0:
            return 0
        v = self.peek(n)
        self.bit += n
        return v

    def code(self, table, shortest, longest):
        for n in range(shortest, longest + 1):
            if self.bit + n > self.end:
                break
            v = table.get((n, self.peek(n)))
            if v is not None:
                self.bit += n
                if isinstance(v, tuple):        # a prefix, then extra bits
                    bits, base = v
                    return self.get(bits) + base
                return v
        raise TomeError('a bad code')


def _tables():
    """macresources' two code tables, keyed here on (length, code): theirs are keyed on the
    code's value alone. Every code starts with a 1 bit but the match codes 00 and 01 and the
    run code 0."""
    from macresources.instacomp import lenHuffTab, litHuffTab
    match = {(2 if code < 2 else code.bit_length(), code): v for code, v in lenHuffTab.items()}
    run = {(1 if code == 0 else code.bit_length(), code): v for code, v in litHuffTab.items()}
    return match, run


_MATCH = _RUN = None

# Distances: for each limit on how much is unpacked so far (capped at 43008, the window),
# the bits of a near distance ('0', then that many bits, plus 1), the bits of a middle one
# ('10', then that many bits, plus MID), and where a far one starts ('11', then enough bits
# for the rest of the window, plus FAR).
DISTANCES = (
    # limit, near bits, middle bits, MID, FAR
    (10, 0, 2, 2, 6),
    (20, 1, 3, 3, 11),
    (40, 2, 4, 5, 21),
    (80, 3, 5, 9, 41),
    (160, 4, 6, 17, 81),
    (672, 5, 7, 33, 161),
    (1000, 6, 8, 65, 321),
    (2688, 7, 9, 129, 641),
    (5376, 8, 10, 257, 1281),
    (10752, 9, 11, 513, 2561),
    (21504, 10, 12, 1025, 5121),
    (43008, 11, 13, 2049, 10241),
)
WINDOW = 43008


def distance(bits, done):
    done = min(done, WINDOW)
    for limit, near, middle, mid, far in DISTANCES:
        if done <= limit:
            if not bits.get(1):
                return bits.get(near) + 1
            if not bits.get(1):
                return bits.get(middle) + mid
            rest = done - far + 1
            return bits.get(1 if rest < 2 else (rest - 1).bit_length()) + far
    raise TomeError('no distance code')         # done is 0: nothing to match


def _block(data, pos, end, out, goal, what):
    global _MATCH, _RUN
    if _MATCH is None:
        _MATCH, _RUN = _tables()
    bits = _Bits(data, pos, end)
    after_run = False
    while len(out) < goal:
        n = bits.code(_MATCH, 2, 11)
        if n or after_run:
            n += 3 if after_run else 2
            d = distance(bits, len(out))
            if d > len(out):
                raise TomeError('%s: a match reaches back too far' % what)
            for _ in range(n):
                out.append(out[-d])
            after_run = False
        else:
            n = bits.code(_RUN, 1, 7)
            for _ in range(n):
                out.append(bits.get(8))
            after_run = n < 63
    return (bits.bit + 7) // 8
