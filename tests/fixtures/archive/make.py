#!/usr/bin/env python3
"""The test archives the standard tools don't make, written from the formats' published
specifications with Python's standard library: MacBinary I, II, and III (the MacBinary II
and III proposals), BinHex 4.0 (Lempereur's 1985 description), a gzip header with every
field (RFC 1952), a ustar member with a long path (POSIX.1-1988), and tar and zip archives
with names that try to leave the archive. Run by make.sh."""
import binascii, io, struct, tarfile, zipfile, zlib

DATE_UNIX = 883008000                   # 1997-12-25 00:00 UTC
DATE_MAC = DATE_UNIX + 2082844800       # the Mac's seconds since 1904

HELLO = open("src/hello.txt", "rb").read()
README = open("src/docs/readme.txt", "rb").read()
# a resource fork's worth of bytes, with runs and 0x90s for BinHex's run-length coding
RSRC = bytes(range(256)) + b"\x90" * 7 + b"\x00" * 40 + b"\x90\x00\x90" + b"rsrc end"
DATA = README[:1000] + b"\x90" + b"=" * 300 + b"\x90\x90\x90\x90" + b"data end\n"
open("src/mac-data", "wb").write(DATA)
open("src/mac-rsrc", "wb").write(RSRC)


# ---- MacBinary ----------------------------------------------------------------------

def macbinary(name, data, rsrc, version):
    h = bytearray(128)
    h[1] = len(name)
    h[2:2 + len(name)] = name
    h[65:69] = b"TEXT"
    h[69:73] = b"ttxt"
    h[73] = 0x01                        # Finder flags, high byte (bundle bit off; 0x0100 is "inited")
    struct.pack_into(">II", h, 83, len(data), len(rsrc))
    struct.pack_into(">II", h, 91, DATE_MAC - 86400, DATE_MAC)
    if version >= 2:
        h[101] = 0x20                   # Finder flags, low byte
        h[122] = 129 if version == 2 else 130
        h[123] = 129
        if version == 3:
            h[102:106] = b"mBIN"
        struct.pack_into(">H", h, 124, binascii.crc_hqx(bytes(h[:124]), 0))
    pad = lambda b: b + bytes(-len(b) % 128)
    return bytes(h) + pad(data) + pad(rsrc)


open("mb1.bin", "wb").write(macbinary(b"Read Me", DATA, RSRC, 1))
open("mb2.bin", "wb").write(macbinary(b"Read Me/Now", DATA, RSRC, 2))
open("mb3.bin", "wb").write(macbinary(b"Read Me", DATA, b"", 3))


# ---- BinHex 4.0 ---------------------------------------------------------------------

HQX = b"!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr"


def rle(b):
    out, i = bytearray(), 0
    lit = lambda c: out.extend(b"\x90\x00" if c == 0x90 else bytes([c]))
    while i < len(b):
        n = 1
        while i + n < len(b) and b[i + n] == b[i] and n < 255:
            n += 1
        lit(b[i])
        if n >= 3:
            out.extend(bytes([0x90, n]))
            i += n
        else:
            i += 1
    return bytes(out)


def binhex(name, data, rsrc, before=b""):
    head = bytes([len(name)]) + name + b"\0" + b"TEXT" + b"ttxt" + struct.pack(">HII", 0x0100, len(data), len(rsrc))
    crc = lambda b: struct.pack(">H", binascii.crc_hqx(b, 0))
    raw = rle(head + crc(head) + data + crc(data) + rsrc + crc(rsrc))
    bits, nbits, text = 0, 0, bytearray()
    for c in raw:
        bits, nbits = (bits << 8) | c, nbits + 8
        while nbits >= 6:
            nbits -= 6
            text.append(HQX[(bits >> nbits) & 63])
    if nbits:
        text.append(HQX[(bits << (6 - nbits)) & 63])
    body = b":" + bytes(text) + b":"
    lines = [body[i:i + 64] for i in range(0, len(body), 64)]
    return before + b"(This file must be converted with BinHex 4.0)\r\n" + b"\r\n".join(lines) + b"\r\n"


MAIL = b"From: mill@example.com\r\nSubject: Read Me\r\n\r\nHere it is (in BinHex form).\r\n\r\n"
open("readme.hqx", "wb").write(binhex(b"Read Me", DATA, RSRC, MAIL))
open("empty.hqx", "wb").write(binhex(b"Nothing", b"", b""))


# ---- gzip with every header field ---------------------------------------------------

c = zlib.compressobj(9, zlib.DEFLATED, -15)
body = c.compress(HELLO) + c.flush()
head = struct.pack("<BBBBIBB", 31, 139, 8, 1 | 2 | 4 | 8 | 16, DATE_UNIX, 2, 3)
head += struct.pack("<H", 6) + b"AB" + struct.pack("<H", 2) + b"hi"
head += b"full.txt\0" + b"Merry Christmas\0"
head += struct.pack("<H", zlib.crc32(head) & 0xFFFF)
open("full.gz", "wb").write(head + body + struct.pack("<II", zlib.crc32(HELLO), len(HELLO)))


# ---- tar: a long path, and hostile names --------------------------------------------

def add(t, name, data, kind=tarfile.REGTYPE):
    i = tarfile.TarInfo(name)
    i.size, i.mtime, i.mode, i.type = len(data), DATE_UNIX, 0o644, kind
    t.addfile(i, io.BytesIO(data))


DEEP = "a" * 60 + "/" + "b" * 60 + "/deep.txt"
with tarfile.open("deep.tar", "w", format=tarfile.USTAR_FORMAT) as t:
    add(t, DEEP, HELLO)
with tarfile.open("evil.tar", "w", format=tarfile.USTAR_FORMAT) as t:
    for n in ("../evil.txt", "/abs.txt", "a/../../b.txt", "./ok/./fine.txt", "ok/.."):
        add(t, n, b"evil\n")
    add(t, "ok.txt", HELLO)


# ---- zip: hostile names, a DOS name with backslashes, a member comment --------------

with zipfile.ZipFile("evil.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for n in ("../evil.txt", "/abs.txt", "a/../../b.txt"):
        z.writestr(zipfile.ZipInfo(n, (1997, 12, 25, 0, 0, 0)), b"evil\n")
    i = zipfile.ZipInfo("dos\\path.txt", (1997, 12, 25, 0, 0, 0))
    i.create_system = 0
    i.comment = b"From a PC."
    z.writestr(i, HELLO)
