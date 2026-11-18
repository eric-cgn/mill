#!/usr/bin/env python3
'''Makes Mill's release files: dist/Install-Mill-VERSION.bin (MacBinary) and .hqx (BinHex 4.0),
copied from the installer ./build.sh installer makes with EUP. The release's third file, the
"Mill Program Disk" (dist/Mill-VERSION.image, Mill ready to run), is made by ./build.sh disk;
here it is also wrapped as MacBinary (Mill-VERSION.image.bin), which keeps Disk Copy's file type
when a Mac downloads it. The Plug-In SDK (./build.sh kit) is not part of a release. The version
comes from src/version.h.
python3 tools/release.py [build dir] [dist dir]'''

import os, re, shutil, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kit import macbinary

here = os.path.dirname(os.path.abspath(__file__))
build = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', 'build')
dist = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, '..', 'dist')


def version():
    h = open(os.path.join(here, '..', 'src', 'version.h')).read()
    return re.search(r'#define MILL_VERSION "([^"]+)"', h).group(1)


def main():
    v = version()
    os.makedirs(dist, exist_ok=True)
    for ext in ('bin', 'hqx'):
        name = 'Install-Mill-%s.%s' % (v, ext)
        shutil.copyfile(os.path.join(build, 'installer', name), os.path.join(dist, name))
        print('%s: %d bytes' % (os.path.join(dist, name), os.path.getsize(os.path.join(dist, name))))
    image = os.path.join(dist, 'Mill-%s.image' % v)
    if not os.path.exists(image):
        sys.exit('%s is missing: ./build.sh disk' % image)
    print('%s: %d bytes' % (image, os.path.getsize(image)))
    with open(image + '.bin', 'wb') as f:     # Disk Copy image: type dImg, creator ddsk (Disk Copy 6)
        f.write(macbinary(os.path.basename(image), b'dImg', b'ddsk', open(image, 'rb').read(), os.path.getmtime(image)))
    print('%s: %d bytes' % (image + '.bin', os.path.getsize(image + '.bin')))


if __name__ == '__main__':
    main()
