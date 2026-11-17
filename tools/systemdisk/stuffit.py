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
#
# The method-13 tables below (META_CODES, META_LENGTHS, and the five predefined sets), and the
# method-13 decoding steps, follow compcol 0.8.0's sit13 module (https://github.com/KarpelesLab/compcol,
# src/sit13/), whose notice is:
#
#   MIT License
#
#   Copyright (c) 2026 Karpeles Lab Inc.
#
#   Permission is hereby granted, free of charge, to any person obtaining a copy
#   of this software and associated documentation files (the "Software"), to deal
#   in the Software without restriction, including without limitation the rights
#   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
#   copies of the Software, and to permit persons to whom the Software is
#   furnished to do so, subject to the following conditions:
#
#   The above copyright notice and this permission notice shall be included in all
#   copies or substantial portions of the Software.
#
#   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
#   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
#   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
#   SOFTWARE.
#
# (also in compcol-LICENSE beside this file)
"""Classic StuffIt archives ("SIT!", StuffIt 1.5 to 4.5) with forks compressed by method 0
(stored) or method 13 (LZ77 with Huffman codes), in pure Python. Enough to unpack Apple's
System 6 disk sets and MacPPP 2.0.1; nothing else (no encryption, no other methods).

    for entry in read_archive(data): entry.name, entry.type, entry.creator, entry.data, entry.rsrc

entry.path is the folders the file is in, outermost first: () at the top of the archive.

Every header and fork CRC the archive carries is checked; a mismatch raises StuffItError.

Archive layout (all numbers big-endian):
  archive header, 22 bytes: 'SIT!', number of entries at the top level (2), archive length
    (4), 'rLau', version (1), reserved (7)
  then for each entry, a 112-byte header and its compressed resource fork, then data fork:
    0 resource fork method, 1 data fork method (low nibble the method; 0x10 encrypted;
    0x20 and 0x21 start and end a folder), 2 name (Pascal string, 63 characters at most),
    66 type, 70 creator, 74 Finder flags (2), 76 created, 80 modified,
    84 resource fork length, 88 data fork length, 92 and 96 their compressed lengths,
    100 resource fork CRC, 102 data fork CRC (of the uncompressed bytes), 104 reserved (6),
    110 the header's own CRC (of bytes 0 to 109)
A folder is a header with both methods 0x20, then the entries in it, then a header with both
methods 0x21. Neither has forks; their lengths are the folder's totals, and are not used here.
The CRC is CRC-16/ARC: polynomial 0x8005 reflected (0xA001), initial value 0, no final XOR.

Method 13 is LZ77 over a 64K window, with three prefix codes: code A for the first token and
for every token after a literal, code B for every token after a match, and a code for the
number of bits in a match's offset. The bit stream is read least significant bit first. The
first byte says where the codes come from: high nibble 1 to 5, one of five predefined sets;
high nibble 0, sent in the stream (see read_lengths), with bit 3 meaning code B is code A, and
the low three bits plus 10 the number of offset symbols. Token symbols: 0 to 255 a literal
byte; 256 to 317 a match of 3 to 64 bytes; 318 a match of 65 plus the next 10 bits; 319 a
match of 65 plus the next 15 bits; 320 the end of the stream. A match's offset symbol n means
a distance of 1 (n = 0), 2 (n = 1), or 2**(n-1) + 1 plus the next n - 1 bits."""

import struct


class StuffItError(Exception):
    pass


def crc16(data, crc=0):
    """CRC-16/ARC, StuffIt's CRC, of a bytes-like object"""
    for b in data:
        crc = _CRC_TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc


def _crc_table():
    t = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
        t.append(c)
    return t


_CRC_TABLE = _crc_table()


class Entry:
    """One file from an archive: name and Finder info, with both forks uncompressed"""

    def __init__(self, name, type, creator, flags, created, modified, data, rsrc, path=()):
        self.name, self.type, self.creator, self.flags = name, type, creator, flags
        self.created, self.modified, self.data, self.rsrc = created, modified, data, rsrc
        self.path = path


def read_archive(data):
    """The entries of a classic StuffIt archive, as a list of Entry"""
    if len(data) < 22 or data[:4] != b'SIT!' or data[10:14] != b'rLau':
        raise StuffItError('not a classic StuffIt archive')
    count, length = struct.unpack('>HI', data[4:10])
    if length > len(data):
        raise StuffItError('archive is cut short (%d of %d bytes)' % (len(data), length))
    entries, pos, folders = [], 22, []
    while count or folders:
        h = data[pos:pos + 112]
        if len(h) < 112:
            raise StuffItError('entry header is cut short')
        if crc16(h[:110]) != struct.unpack('>H', h[110:112])[0]:
            raise StuffItError('entry header CRC does not match')
        rmethod, dmethod = h[0], h[1]
        name = h[3:3 + min(h[2], 63)].decode('mac_roman')
        pos += 112
        if rmethod == dmethod == 0x20:                      # a folder starts
            if not folders:
                count -= 1
            folders.append(name)
            continue
        if rmethod == dmethod == 0x21:                      # and ends
            if not folders:
                raise StuffItError('unmatched folder end')
            folders.pop()
            continue
        if (rmethod | dmethod) & 0xF0:
            raise StuffItError('encrypted and other kinds of entries are not supported')
        if not folders:
            count -= 1
        type, creator = h[66:70], h[70:74]
        flags, created, modified = struct.unpack('>HII', h[74:84])
        rlen, dlen, rclen, dclen, rcrc, dcrc = struct.unpack('>IIIIHH', h[84:104])
        rsrc = _fork(data[pos:pos + rclen], rmethod, rlen, rcrc, name + ' (resource fork)')
        pos += rclen
        fork = _fork(data[pos:pos + dclen], dmethod, dlen, dcrc, name + ' (data fork)')
        pos += dclen
        entries.append(Entry(name, type, creator, flags, created, modified, fork, rsrc, tuple(folders)))
    return entries


def _fork(packed, method, length, crc, what):
    if method == 0:
        out = bytes(packed)
    elif method == 13:
        out = decompress13(packed, length)
    else:
        raise StuffItError('%s: compression method %d is not supported' % (what, method))
    if len(out) != length:
        raise StuffItError('%s: %d bytes, not %d' % (what, len(out), length))
    if crc16(out) != crc:
        raise StuffItError('%s: CRC does not match' % what)
    return out


# --- method 13 ---

class _Bits:
    """A bit stream over bytes, least significant bit first"""

    def __init__(self, data):
        self.data, self.pos, self.buf, self.count = data, 0, 0, 0

    def peek(self, n):
        """The next n bits, without using them; past the end, zeros (take() notices)"""
        while self.count < n:
            b = self.data[self.pos] if self.pos < len(self.data) else 0
            self.buf |= b << self.count
            self.pos += 1
            self.count += 8
        return self.buf & ((1 << n) - 1)

    def take(self, n):
        v = self.peek(n)
        self.skip(n)
        return v

    def skip(self, n):
        self.buf >>= n
        self.count -= n
        if self.pos * 8 - self.count > len(self.data) * 8:
            raise StuffItError('method 13: stream is cut short')


class _Code:
    """A prefix code, decoded with a table indexed by the next `fast` bits; longer codes are
    looked up one bit at a time in `slow` by (length, value)"""

    def __init__(self, codes):
        # codes: (symbol, value as read least significant bit first, length) for each symbol
        if not codes:
            raise StuffItError('method 13: an empty code')
        self.fast = min(max(n for _, _, n in codes), 12)
        self.table = [None] * (1 << self.fast)
        self.slow = {}
        for sym, value, n in codes:
            if n <= self.fast:
                for hi in range(1 << (self.fast - n)):     # every way the unused bits can go
                    i = value | (hi << n)
                    if self.table[i] is not None:
                        raise StuffItError('method 13: not a prefix code')
                    self.table[i] = (sym, n)
            else:
                self.slow[(n, value)] = sym
                prefix = value & ((1 << self.fast) - 1)
                if self.table[prefix] is None:
                    self.table[prefix] = (None, self.fast)
                elif self.table[prefix][0] is not None:
                    raise StuffItError('method 13: not a prefix code')
        self.longest = max(n for _, _, n in codes)

    @classmethod
    def from_lengths(cls, lengths):
        """The canonical code for the bit lengths (0: no code): shorter codes first, and in
        symbol order within a length, counting up from 0; each code is then read least
        significant bit first, so its value here is its bits reversed"""
        codes, code, prev = [], 0, 0
        for n, sym in sorted((n, s) for s, n in enumerate(lengths) if n):
            code <<= n - prev
            prev = n
            if code >> n:
                raise StuffItError('method 13: too many codes')
            codes.append((sym, int(format(code, '0%db' % n)[::-1], 2), n))
            code += 1
        return cls(codes)

    def read(self, bits):
        sym, n = self.table[bits.peek(self.fast)]
        if sym is not None:
            bits.skip(n)
            return sym
        v = bits.peek(self.longest)                         # a long code
        for n in range(self.fast + 1, self.longest + 1):
            sym = self.slow.get((n, v & ((1 << n) - 1)))
            if sym is not None:
                bits.skip(n)
                return sym
        raise StuffItError('method 13: bad code')


def read_lengths(bits, meta, count):
    """`count` code lengths sent in the stream, each a meta-code symbol: 0 to 30 set the
    length to 1 to 31; 31 means no code (length 0); 32 and 33 add 1 to or take 1 from the last
    length; 34 repeats it once more if the next bit is 1; 35 repeats it 2 more times plus the
    next 3 bits; 36 repeats it 10 more times plus the next 6 bits. Every symbol also gives the
    current symbol that length."""
    lengths, n = [], 0
    while len(lengths) < count:
        m = meta.read(bits)
        more = 0
        if m <= 30:
            n = m + 1
        elif m == 31:
            n = -1
        elif m == 32:
            n += 1
        elif m == 33:
            n -= 1
        elif m == 34:
            more = bits.take(1)
        elif m == 35:
            more = bits.take(3) + 2
        else:
            more = bits.take(6) + 10
        lengths += [max(n, 0)] * min(more + 1, count - len(lengths))
    return lengths


def decompress13(packed, length):
    """A fork compressed with method 13, `length` bytes once uncompressed"""
    if length == 0:
        return b''
    bits = _Bits(packed)
    control = bits.take(8)
    sets = control >> 4
    if sets == 0:
        meta = _Code([(s, v, n) for s, (v, n) in enumerate(zip(META_CODES, META_LENGTHS))])
        a = read_lengths(bits, meta, 321)
        code_a = _Code.from_lengths(a)
        code_b = code_a if control & 8 else _Code.from_lengths(read_lengths(bits, meta, 321))
        offsets = _Code.from_lengths(read_lengths(bits, meta, (control & 7) + 10))
    elif sets <= 5:
        a, b, off = PREDEFINED[sets - 1]
        code_a, code_b, offsets = _Code.from_lengths(a), _Code.from_lengths(b), _Code.from_lengths(off)
    else:
        raise StuffItError('method 13: unknown code set %d' % sets)
    out = bytearray()
    code = code_a
    while len(out) < length:
        sym = code.read(bits)
        if sym < 256:
            out.append(sym)
            code = code_a
            continue
        if sym == 320:
            break
        if sym < 318:
            n = sym - 256 + 3
        elif sym == 318:
            n = bits.take(10) + 65
        else:
            n = bits.take(15) + 65
        k = offsets.read(bits)
        dist = k + 1 if k < 2 else (1 << (k - 1)) + bits.take(k - 1) + 1
        if dist > len(out) or dist > 0x10000:
            raise StuffItError('method 13: a match reaches back too far')
        start = len(out) - dist
        if dist >= n:
            out += out[start:start + n]
        else:                                               # overlapping: repeats itself
            while n > 0:
                piece = out[start:start + min(n, dist)]
                out += piece
                n -= len(piece)
        code = code_b
    if len(out) != length:
        raise StuffItError('method 13: %d bytes, not %d' % (len(out), length))
    return bytes(out)


# --- method 13's fixed tables (from compcol 0.8.0; see the notice at the top) ---

META_CODES = (
    1496, 88, 64, 192, 0, 120, 43, 20, 12, 28, 27, 11, 16, 32, 56, 24, 216, 3032, 384, 1664,
    896, 3968, 1920, 1152, 128, 640, 984, 4056, 2008, 2520, 472, 4, 1, 2, 7, 3, 8,
)
META_LENGTHS = (
    11, 8, 8, 8, 8, 7, 6, 5, 5, 5, 5, 6, 5, 6, 7, 7, 9, 12, 10, 11, 11, 12, 12, 11, 11, 11, 12,
    12, 12, 12, 12, 5, 2, 2, 3, 4, 5,
)
_SET1_A = (
    4, 5, 7, 8, 8, 9, 9, 9, 9, 7, 9, 9, 9, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 9, 9, 10, 10, 9,
    10, 9, 9, 5, 9, 9, 9, 9, 10, 9, 9, 9, 9, 9, 9, 9, 9, 7, 9, 9, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 8, 9, 9, 8, 8, 9, 9, 9, 9, 9, 9, 9, 7, 8, 9, 7, 9, 9, 7, 7, 9, 9, 9, 9,
    10, 9, 10, 10, 10, 9, 9, 9, 5, 9, 8, 7, 5, 9, 8, 8, 7, 9, 9, 8, 8, 5, 5, 7, 10, 5, 8, 5, 8,
    9, 9, 9, 9, 9, 10, 9, 9, 10, 9, 9, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10,
    10, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10,
    10, 10, 9, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10,
    10, 10, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    9, 9, 10, 10, 9, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 9, 10, 9, 5, 6, 5, 5, 8, 9, 9,
    9, 9, 9, 9, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 9, 10, 9, 9, 9, 10, 9, 10, 9, 10, 9, 10, 9, 10, 10, 10, 9, 10, 9, 10,
    10, 9, 9, 9, 6, 9, 9, 10, 9, 5,
)
_SET1_B = (
    4, 5, 6, 6, 7, 7, 6, 7, 7, 7, 6, 8, 7, 8, 8, 8, 8, 9, 6, 9, 8, 9, 8, 9, 9, 9, 8, 10, 5, 9,
    7, 9, 6, 9, 8, 10, 9, 10, 8, 8, 9, 9, 7, 9, 8, 9, 8, 9, 8, 8, 6, 9, 9, 8, 8, 9, 9, 10, 8, 9,
    9, 10, 8, 10, 8, 8, 8, 8, 8, 9, 7, 10, 6, 9, 9, 11, 7, 8, 8, 9, 8, 10, 7, 8, 6, 9, 10, 9, 9,
    10, 8, 11, 9, 11, 9, 10, 9, 8, 9, 8, 8, 8, 8, 10, 9, 9, 10, 10, 8, 9, 8, 8, 8, 11, 9, 8, 8,
    9, 9, 10, 8, 11, 10, 10, 8, 10, 9, 10, 8, 9, 9, 11, 9, 11, 9, 10, 10, 11, 10, 12, 9, 12, 10,
    11, 10, 11, 9, 10, 10, 11, 10, 11, 10, 11, 10, 11, 10, 10, 10, 9, 9, 9, 8, 7, 6, 8, 11, 11,
    9, 12, 10, 12, 9, 11, 11, 11, 10, 12, 11, 11, 10, 12, 10, 11, 10, 10, 10, 11, 10, 11, 11,
    11, 9, 12, 10, 12, 11, 12, 10, 11, 10, 12, 11, 12, 11, 12, 11, 12, 10, 12, 11, 12, 11, 11,
    10, 12, 10, 11, 10, 12, 10, 12, 10, 12, 10, 11, 11, 11, 10, 11, 11, 11, 10, 12, 11, 12, 10,
    10, 11, 11, 9, 12, 11, 12, 10, 11, 10, 12, 10, 11, 10, 12, 10, 11, 10, 7, 5, 4, 6, 6, 7, 7,
    7, 8, 8, 7, 7, 6, 8, 6, 7, 7, 9, 8, 9, 9, 10, 11, 11, 11, 12, 11, 10, 11, 12, 11, 12, 11,
    12, 12, 12, 12, 11, 12, 12, 11, 12, 11, 12, 11, 13, 11, 12, 10, 13, 10, 14, 14, 13, 14, 15,
    14, 16, 15, 15, 18, 18, 18, 9, 18, 8,
)
_SET1_OFF = (
    5, 6, 3, 3, 3, 3, 3, 3, 3, 4, 6,
)
_SET2_A = (
    4, 7, 7, 8, 7, 8, 8, 8, 8, 7, 8, 7, 8, 7, 9, 8, 8, 8, 9, 9, 9, 9, 10, 10, 9, 10, 10, 10, 10,
    10, 9, 9, 5, 9, 8, 9, 9, 11, 10, 9, 8, 9, 9, 9, 8, 9, 7, 8, 8, 8, 9, 9, 9, 9, 9, 10, 9, 9,
    9, 10, 9, 9, 10, 9, 8, 8, 7, 7, 7, 8, 8, 9, 8, 8, 9, 9, 8, 8, 7, 8, 7, 10, 8, 7, 7, 9, 9, 9,
    9, 10, 10, 11, 11, 11, 10, 9, 8, 6, 8, 7, 7, 5, 7, 7, 7, 6, 9, 8, 6, 7, 6, 6, 7, 9, 6, 6, 6,
    7, 8, 8, 8, 8, 9, 10, 9, 10, 9, 9, 8, 9, 10, 10, 9, 10, 10, 9, 9, 10, 10, 10, 10, 10, 10,
    10, 9, 10, 10, 11, 10, 10, 10, 10, 10, 10, 10, 11, 10, 11, 10, 10, 9, 11, 10, 10, 10, 10,
    10, 10, 9, 9, 10, 11, 10, 11, 10, 11, 10, 12, 10, 11, 10, 12, 11, 12, 10, 12, 10, 11, 10,
    11, 11, 11, 9, 10, 11, 11, 11, 12, 12, 10, 10, 10, 11, 11, 10, 11, 10, 10, 9, 11, 10, 11,
    10, 11, 11, 11, 10, 11, 11, 12, 11, 11, 10, 10, 10, 11, 10, 10, 11, 11, 12, 10, 10, 11, 11,
    12, 11, 11, 10, 11, 9, 12, 10, 11, 11, 11, 10, 11, 10, 11, 10, 11, 9, 10, 9, 7, 3, 5, 6, 6,
    7, 7, 8, 8, 8, 9, 9, 9, 11, 10, 10, 10, 12, 13, 11, 12, 12, 11, 13, 12, 12, 11, 12, 12, 13,
    12, 14, 13, 14, 13, 15, 13, 14, 15, 15, 14, 13, 15, 15, 14, 15, 14, 15, 15, 14, 15, 13, 13,
    14, 15, 15, 14, 14, 16, 16, 15, 15, 15, 12, 15, 10,
)
_SET2_B = (
    5, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 8, 7, 8, 7, 7, 7, 8, 8, 8, 8, 9, 8, 9, 8, 9, 9, 9, 7, 9, 8,
    8, 6, 9, 8, 9, 8, 9, 8, 9, 8, 9, 8, 9, 8, 9, 8, 8, 8, 8, 8, 9, 8, 9, 8, 9, 9, 10, 8, 10, 8,
    9, 9, 8, 8, 8, 7, 8, 8, 9, 8, 9, 7, 9, 8, 10, 8, 9, 8, 9, 8, 9, 8, 8, 8, 9, 9, 9, 9, 10, 9,
    11, 9, 10, 9, 10, 8, 8, 8, 9, 8, 8, 8, 9, 9, 8, 9, 10, 8, 9, 8, 8, 8, 11, 8, 7, 8, 9, 9, 9,
    9, 10, 9, 10, 9, 10, 9, 8, 8, 9, 9, 10, 9, 10, 9, 10, 8, 10, 9, 10, 9, 11, 10, 11, 9, 11,
    10, 10, 10, 11, 9, 11, 9, 10, 9, 11, 9, 11, 10, 10, 9, 10, 9, 9, 8, 10, 9, 11, 9, 9, 9, 11,
    10, 11, 9, 11, 9, 11, 9, 11, 10, 11, 10, 11, 10, 11, 9, 10, 10, 11, 10, 10, 8, 10, 9, 10,
    10, 11, 9, 11, 9, 10, 10, 11, 9, 10, 10, 9, 9, 10, 9, 10, 9, 10, 9, 10, 9, 11, 9, 11, 10,
    10, 9, 10, 9, 11, 9, 11, 9, 11, 9, 10, 9, 11, 9, 11, 9, 11, 9, 10, 8, 11, 9, 10, 9, 10, 9,
    10, 8, 10, 8, 9, 8, 9, 8, 7, 4, 4, 5, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 7, 8, 8, 9, 9, 10, 10,
    10, 10, 10, 10, 11, 11, 10, 10, 12, 11, 11, 12, 12, 11, 12, 12, 11, 12, 12, 12, 12, 12, 12,
    11, 12, 11, 13, 12, 13, 12, 13, 14, 14, 14, 15, 13, 14, 13, 14, 18, 18, 17, 7, 16, 9,
)
_SET2_OFF = (
    5, 6, 4, 4, 3, 3, 3, 3, 3, 4, 4, 4, 6,
)
_SET3_A = (
    6, 6, 6, 6, 6, 9, 8, 8, 4, 9, 8, 9, 8, 9, 9, 9, 8, 9, 9, 10, 8, 10, 10, 10, 9, 10, 10, 10,
    9, 10, 10, 9, 9, 9, 8, 10, 9, 10, 9, 10, 9, 10, 9, 10, 9, 9, 8, 9, 8, 9, 9, 9, 10, 10, 10,
    10, 9, 9, 9, 10, 9, 10, 9, 9, 7, 8, 8, 9, 8, 9, 9, 9, 8, 9, 9, 10, 9, 9, 8, 9, 8, 9, 8, 8,
    8, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 9, 8, 8, 9, 8, 9, 7, 8, 8, 9, 8, 10, 10, 8, 9, 8, 8,
    8, 10, 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 10, 9, 7, 9, 9, 10, 10, 10, 10, 10, 9, 10,
    10, 10, 10, 10, 10, 9, 9, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10, 9, 10,
    10, 10, 10, 10, 10, 10, 9, 9, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    9, 10, 10, 10, 10, 9, 8, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 9, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 9, 10, 10, 10, 10, 10, 10, 9, 10, 10,
    10, 10, 10, 10, 9, 9, 9, 10, 10, 10, 10, 10, 10, 9, 9, 10, 9, 9, 8, 9, 8, 9, 4, 6, 6, 6, 7,
    8, 8, 9, 9, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 7, 10, 10, 10, 7, 10, 10, 7, 7, 7, 7, 7, 6, 7, 10, 7, 7, 10, 7, 7, 7, 6, 7, 6, 6, 7, 7,
    6, 6, 9, 6, 9, 10, 6, 10,
)
_SET3_B = (
    5, 6, 6, 6, 6, 7, 7, 7, 6, 8, 7, 8, 7, 9, 8, 8, 7, 7, 8, 9, 9, 9, 9, 10, 8, 9, 9, 10, 8, 10,
    9, 8, 6, 10, 8, 10, 8, 10, 9, 9, 9, 9, 9, 10, 9, 9, 8, 9, 8, 9, 8, 9, 9, 10, 9, 10, 9, 9, 8,
    10, 9, 11, 10, 8, 8, 8, 8, 9, 7, 9, 9, 10, 8, 9, 8, 11, 9, 10, 9, 10, 8, 9, 9, 9, 9, 8, 9,
    9, 10, 10, 10, 12, 10, 11, 10, 10, 8, 9, 9, 9, 8, 9, 8, 8, 10, 9, 10, 11, 8, 10, 9, 9, 8,
    12, 8, 9, 9, 9, 9, 8, 9, 10, 9, 12, 10, 10, 10, 8, 7, 11, 10, 9, 10, 11, 9, 11, 7, 11, 10,
    12, 10, 12, 10, 11, 9, 11, 9, 12, 10, 12, 10, 12, 10, 9, 11, 12, 10, 12, 10, 11, 9, 10, 9,
    10, 9, 11, 11, 12, 9, 10, 8, 12, 11, 12, 9, 12, 10, 12, 10, 13, 10, 12, 10, 12, 10, 12, 10,
    9, 10, 12, 10, 9, 8, 11, 10, 12, 10, 12, 10, 12, 10, 11, 10, 12, 8, 12, 10, 11, 10, 10, 10,
    12, 9, 11, 10, 12, 10, 12, 11, 12, 10, 9, 10, 12, 9, 10, 10, 12, 10, 11, 10, 11, 10, 12, 8,
    12, 9, 12, 8, 12, 8, 11, 10, 11, 10, 11, 9, 10, 8, 10, 9, 9, 8, 9, 8, 7, 4, 3, 5, 5, 6, 5,
    6, 6, 7, 7, 8, 8, 8, 7, 7, 7, 9, 8, 9, 9, 11, 9, 11, 9, 8, 9, 9, 11, 12, 11, 12, 12, 13, 13,
    12, 13, 14, 13, 14, 13, 14, 13, 13, 13, 12, 13, 13, 12, 13, 13, 14, 14, 13, 13, 14, 14, 14,
    14, 15, 18, 17, 18, 8, 16, 10,
)
_SET3_OFF = (
    6, 7, 4, 4, 3, 3, 3, 3, 3, 4, 4, 4, 5, 7,
)
_SET4_A = (
    2, 6, 6, 7, 7, 8, 7, 8, 7, 8, 8, 9, 8, 9, 9, 9, 8, 8, 9, 9, 9, 10, 10, 9, 8, 10, 9, 10, 9,
    10, 9, 9, 6, 9, 8, 9, 9, 10, 9, 9, 9, 10, 9, 9, 9, 9, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 10, 10, 9, 7, 7, 8, 8, 8, 8, 9, 9, 7, 8, 9, 10, 8, 8, 7, 8, 8, 10, 8, 8, 8, 9, 8, 9,
    9, 10, 9, 11, 10, 11, 9, 9, 8, 7, 9, 8, 8, 6, 8, 8, 8, 7, 10, 9, 7, 8, 7, 7, 8, 10, 7, 7, 7,
    8, 9, 9, 9, 9, 10, 11, 9, 11, 10, 9, 7, 9, 10, 10, 10, 11, 11, 10, 10, 11, 10, 10, 10, 11,
    11, 10, 9, 10, 10, 11, 10, 11, 10, 11, 10, 10, 10, 11, 10, 11, 10, 10, 9, 10, 10, 11, 10,
    10, 10, 10, 9, 10, 10, 10, 10, 11, 10, 11, 10, 11, 10, 11, 11, 11, 10, 12, 10, 11, 10, 11,
    10, 11, 11, 10, 8, 10, 10, 11, 10, 11, 11, 11, 10, 11, 10, 11, 10, 11, 11, 11, 9, 10, 11,
    11, 10, 11, 11, 11, 10, 11, 11, 11, 10, 10, 10, 10, 10, 11, 10, 10, 11, 11, 10, 10, 9, 11,
    10, 10, 11, 11, 10, 10, 10, 11, 10, 10, 10, 10, 10, 10, 9, 11, 10, 10, 8, 10, 8, 6, 5, 6, 6,
    7, 7, 8, 8, 8, 9, 10, 11, 10, 10, 11, 11, 12, 12, 10, 11, 12, 12, 12, 12, 13, 13, 13, 13,
    13, 12, 13, 13, 15, 14, 12, 14, 15, 16, 12, 12, 13, 15, 14, 16, 15, 17, 18, 15, 17, 16, 15,
    15, 15, 15, 13, 13, 10, 14, 12, 13, 17, 17, 18, 10, 17, 4,
)
_SET4_B = (
    4, 5, 6, 6, 6, 6, 7, 7, 6, 7, 7, 9, 6, 8, 8, 7, 7, 8, 8, 8, 6, 9, 8, 8, 7, 9, 8, 9, 8, 9, 8,
    9, 6, 9, 8, 9, 8, 10, 9, 9, 8, 10, 8, 10, 8, 9, 8, 9, 8, 8, 7, 9, 9, 9, 9, 9, 8, 10, 9, 10,
    9, 10, 9, 8, 7, 8, 9, 9, 8, 9, 9, 9, 7, 10, 9, 10, 9, 9, 8, 9, 8, 9, 8, 8, 8, 9, 9, 10, 9,
    9, 8, 11, 9, 11, 10, 10, 8, 8, 10, 8, 8, 9, 9, 9, 10, 9, 10, 11, 9, 9, 9, 9, 8, 9, 8, 8, 8,
    10, 10, 9, 9, 8, 10, 11, 10, 11, 11, 9, 8, 9, 10, 11, 9, 10, 11, 11, 9, 12, 10, 10, 10, 12,
    11, 11, 9, 11, 11, 12, 9, 11, 9, 10, 10, 10, 10, 12, 9, 11, 10, 11, 9, 11, 11, 11, 10, 11,
    11, 12, 9, 10, 10, 12, 11, 11, 10, 11, 9, 11, 10, 11, 10, 11, 9, 11, 11, 9, 8, 11, 10, 11,
    11, 10, 7, 12, 11, 11, 11, 11, 11, 12, 10, 12, 11, 13, 11, 10, 12, 11, 10, 11, 10, 11, 10,
    11, 11, 11, 10, 12, 11, 11, 10, 11, 10, 10, 10, 11, 10, 12, 11, 12, 10, 11, 9, 11, 10, 11,
    10, 11, 10, 12, 9, 11, 11, 11, 9, 11, 10, 10, 9, 11, 10, 10, 9, 10, 9, 7, 4, 5, 5, 5, 6, 6,
    7, 6, 8, 7, 8, 9, 9, 7, 8, 8, 10, 9, 10, 10, 12, 10, 11, 11, 11, 11, 10, 11, 12, 11, 11, 11,
    11, 11, 13, 12, 11, 12, 13, 12, 12, 12, 13, 11, 9, 12, 13, 7, 13, 11, 13, 11, 10, 11, 13,
    15, 15, 12, 14, 15, 15, 15, 6, 15, 5,
)
_SET4_OFF = (
    3, 6, 5, 4, 2, 3, 3, 3, 4, 4, 6,
)
_SET5_A = (
    7, 9, 9, 9, 9, 9, 9, 9, 9, 8, 9, 9, 9, 7, 9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 9, 10, 9, 10, 9,
    10, 9, 9, 5, 9, 7, 9, 9, 9, 9, 9, 7, 7, 7, 9, 7, 7, 8, 7, 8, 8, 7, 7, 9, 9, 9, 9, 7, 7, 7,
    9, 9, 9, 9, 9, 9, 7, 9, 7, 7, 7, 7, 9, 9, 7, 9, 9, 7, 7, 7, 7, 7, 9, 7, 8, 7, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 7, 8, 7, 7, 7, 8, 8, 6, 7, 9, 7, 7, 8, 7, 5, 6, 9, 5, 7, 5, 6, 7, 7, 9,
    8, 9, 9, 9, 9, 9, 9, 9, 9, 10, 9, 10, 10, 10, 9, 9, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 9, 10, 10, 10, 9, 9, 10, 9, 9, 9, 9, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10,
    10, 9, 10, 10, 10, 9, 9, 9, 10, 10, 10, 10, 10, 9, 10, 9, 10, 10, 9, 10, 10, 9, 10, 10, 10,
    10, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10,
    10, 10, 10, 10, 10, 9, 10, 9, 10, 9, 10, 10, 9, 5, 6, 8, 8, 7, 7, 7, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 5, 10, 8, 9, 8, 9,
)
_SET5_B = (
    8, 10, 11, 11, 11, 12, 11, 11, 12, 6, 11, 12, 10, 5, 12, 12, 12, 12, 12, 12, 12, 13, 13, 14,
    13, 13, 12, 13, 12, 13, 12, 15, 4, 10, 7, 9, 11, 11, 10, 9, 6, 7, 8, 9, 6, 7, 6, 7, 8, 7, 7,
    8, 8, 8, 8, 8, 8, 9, 8, 7, 10, 9, 10, 10, 11, 7, 8, 6, 7, 8, 8, 9, 8, 7, 10, 10, 8, 7, 8, 8,
    7, 10, 7, 6, 7, 9, 9, 8, 11, 11, 11, 10, 11, 11, 11, 8, 11, 6, 7, 6, 6, 6, 6, 8, 7, 6, 10,
    9, 6, 7, 6, 6, 7, 10, 6, 5, 6, 7, 7, 7, 10, 8, 11, 9, 13, 7, 14, 16, 12, 14, 14, 15, 15, 16,
    16, 14, 15, 15, 15, 15, 15, 15, 15, 15, 14, 15, 13, 14, 14, 16, 15, 17, 14, 17, 15, 17, 12,
    14, 13, 16, 12, 17, 13, 17, 14, 13, 13, 14, 14, 12, 13, 15, 15, 14, 15, 17, 14, 17, 15, 14,
    15, 16, 12, 16, 15, 14, 15, 16, 15, 16, 17, 17, 15, 15, 17, 17, 13, 14, 15, 15, 13, 12, 16,
    16, 17, 14, 15, 16, 15, 15, 13, 13, 15, 13, 16, 17, 15, 17, 17, 17, 16, 17, 14, 17, 14, 16,
    15, 17, 15, 15, 14, 17, 15, 17, 15, 16, 15, 15, 16, 16, 14, 17, 17, 15, 15, 16, 15, 17, 15,
    14, 16, 16, 16, 16, 16, 12, 4, 4, 5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9, 9, 10, 10,
    10, 11, 10, 11, 11, 11, 11, 11, 12, 12, 12, 13, 13, 12, 13, 12, 14, 14, 12, 13, 13, 13, 13,
    14, 12, 13, 13, 14, 14, 14, 13, 14, 14, 15, 15, 13, 15, 13, 17, 17, 17, 9, 17, 7,
)
_SET5_OFF = (
    6, 7, 7, 6, 4, 3, 2, 2, 3, 3, 6,
)

# (code A lengths, code B lengths, offset code lengths) for control byte high nibbles 1 to 5
PREDEFINED = (
    (_SET1_A, _SET1_B, _SET1_OFF),
    (_SET2_A, _SET2_B, _SET2_OFF),
    (_SET3_A, _SET3_B, _SET3_OFF),
    (_SET4_A, _SET4_B, _SET4_OFF),
    (_SET5_A, _SET5_B, _SET5_OFF),
)
