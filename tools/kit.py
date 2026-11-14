#!/usr/bin/env python3
"""The Plug-In SDK from sdk/, for a release: dist/PlugInSDK-VERSION.dsk, an 800K HFS disk
named "Mill Plug-In SDK" (text as TEXT/ttxt in Mac Roman with CR line ends, MacBinary files
such as the CodeWarrior project unpacked with their forks, type, and creator), and
dist/PlugInSDK-VERSION.zip, the same folders with LF text for modern computers (MacBinary
files stay MacBinary there). The disk needs hfsutils, as in Retro68's image: ./build.sh kit."""
import os, re, shutil, struct, subprocess, sys, tempfile, zipfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sdk = os.path.join(root, 'sdk')
dist = os.path.join(root, 'dist')
VOLUME = 'Mill Plug-In SDK'
SKIP = {'build', '.gitignore', '.DS_Store'}
# What the disk leaves out: the Retro68 folder and the example's Retro68-only resource file, and
# any block of a text file between lines with KIT-BEGIN-REPO-ONLY and KIT-END-REPO-ONLY (the
# markers too). The .zip keeps all of it. The disk is for Macs of the period: its text may not
# mention the tools of today, and the build fails if it does.
DISK_SKIP_DIRS = {'Retro68'}
DISK_SKIP_FILES = {'Finger.r'}
BANNED = re.compile(r'retro68|\bgcc\b|\bgit\b|\bzip\b|\bmodern\b|\bdocker\b', re.I)


def scrub(text, path):
    """text without its repo-only blocks; exits if a marker is unmatched"""
    out, skipping = [], False
    for line in text.split('\n'):
        if 'KIT-BEGIN-REPO-ONLY' in line:
            if skipping:
                sys.exit('%s: KIT-BEGIN-REPO-ONLY inside a block' % path)
            skipping = True
        elif 'KIT-END-REPO-ONLY' in line:
            if not skipping:
                sys.exit('%s: KIT-END-REPO-ONLY without a begin' % path)
            skipping = False
        elif not skipping:
            out.append(line)
    if skipping:
        sys.exit('%s: KIT-BEGIN-REPO-ONLY is never ended' % path)
    text = '\n'.join(out)
    m = BANNED.search(text)
    if m:
        sys.exit('%s: the disk image may not mention "%s" (mark it KIT-BEGIN-REPO-ONLY or reword)' % (path, m.group(0)))
    return text


def version():
    for line in open(os.path.join(root, 'src', 'version.h')):
        if line.startswith('#define MILL_VERSION'):
            return line.split('"')[1]


def files():
    """(folder parts, file name, path) for each file of the kit, folders first-level sorted"""
    for d, dirs, names in os.walk(sdk):
        dirs[:] = sorted(x for x in dirs if x not in SKIP)
        rel = os.path.relpath(d, sdk)
        parts = [] if rel == '.' else rel.split(os.sep)
        for n in sorted(names):
            if n not in SKIP:
                yield parts, n, os.path.join(d, n)


def is_macbinary(path):
    b = open(path, 'rb').read(128)
    return path.endswith('.bin') and len(b) == 128 and b[0] == 0 and 0 < b[1] < 64 and b[74] == 0


def hfs(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)


def copy_kit(into=''):
    """Copy the kit's files onto the mounted HFS volume, under the folder `into` (':Name'; empty
    for the volume's top), as the disk has them: text as TEXT/ttxt in Mac Roman with CR line
    ends, scrubbed of what is for the repository; MacBinary files unpacked with their forks"""
    tmp = tempfile.mkdtemp()
    try:
        made = set()
        if into:
            hfs('hmkdir', into)
        for parts, name, path in files():
            if (parts and parts[0] in DISK_SKIP_DIRS) or name in DISK_SKIP_FILES:
                continue
            for i in range(1, len(parts) + 1):
                p = into + ':' + ':'.join(parts[:i])
                if p not in made:
                    hfs('hmkdir', p)
                    made.add(p)
            where = into + ':' + ''.join(p + ':' for p in parts)
            if is_macbinary(path):
                hfs('hcopy', '-m', path, where)
            else:
                text = scrub(open(path, encoding='utf-8').read(), path)
                t = os.path.join(tmp, 'f')
                with open(t, 'wb') as f:
                    f.write(text.encode('mac_roman').replace(b'\n', b'\r'))
                hfs('hcopy', '-r', t, where + name)
                hfs('hattrib', '-t', 'TEXT', '-c', 'ttxt', where + name)
    finally:
        shutil.rmtree(tmp)


def macbinary(name, ftype, creator, data):
    """A MacBinary file of a data fork alone"""
    n = name.encode('mac_roman')
    h = bytearray(128)
    h[1] = len(n)
    h[2:2 + len(n)] = n
    h[65:69], h[69:73] = ftype, creator
    h[83:87] = struct.pack('>I', len(data))
    h[122] = h[123] = 129
    crc = 0
    for b in h[:124]:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    h[124:126] = crc.to_bytes(2, 'big')
    return bytes(h) + data + bytes(-len(data) % 128)


def entries():
    """(folder parts, name, MacBinary bytes) for each file as the disk has it, for the installer"""
    for parts, name, path in files():
        if (parts and parts[0] in DISK_SKIP_DIRS) or name in DISK_SKIP_FILES:
            continue
        if is_macbinary(path):
            mb = open(path, 'rb').read()
            yield parts, mb[2:2 + mb[1]].decode('mac_roman'), mb
        else:
            text = scrub(open(path, encoding='utf-8').read(), path)
            yield parts, name, macbinary(name, b'TEXT', b'ttxt', text.encode('mac_roman').replace(b'\n', b'\r'))


def disk(out):
    with open(out, 'wb') as f:
        f.write(bytes(800 * 1024))
    hfs('hformat', '-l', VOLUME, out)
    try:
        copy_kit()
    finally:
        hfs('humount')


def archive(out):
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for parts, name, path in files():
            z.write(path, '/'.join([VOLUME] + parts + [name]))


def main():
    v = version()
    os.makedirs(dist, exist_ok=True)
    base = os.path.join(dist, 'PlugInSDK-%s' % v)
    archive(base + '.zip')
    if '--zip-only' not in sys.argv:
        disk(base + '.dsk')
    for ext in ('dsk', 'zip'):
        if os.path.exists(base + '.' + ext):
            print('%s.%s: %d bytes' % (base, ext, os.path.getsize(base + '.' + ext)))


if __name__ == '__main__':
    main()
