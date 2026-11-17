#!/usr/bin/env python3
'''Mill's download site (mill.1in1e6.com), plain HTML 3.2 for classic Macs, built from
the release files in <site>/download (Install-Mill-VERSION.hqx and .bin, and Mill-VERSION.image,
the Mill Program Disk; older releases have Mill-VERSION.hqx, .bin, and .dsk) and src/changes.html.
The newest release's list ends with the Mill System Disk, which is not offered: its line links to
how to make one (SYSTEM_DISK). The disks carry their part numbers (tools/disk.py's for the
Program Disk; SYSTEM_PARTS for the System Disk's two editions).
Only the newest few releases are listed (SHOWN).
python3 tools/site.py site <site dir>     writes <site dir>/index.html, changes.html, latest.txt, and icon.gif
python3 tools/site.py notes VERSION       prints a version's change log entries as text'''

import html, os, re, shutil, sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from disk import part_number
# (file name pattern, what it is), in the order they're listed
KINDS = [('Install-Mill-%s.hqx', 'Install Mill, BinHex'), ('Install-Mill-%s.bin', 'Install Mill, MacBinary'),
         ('Mill-%s.image', 'Mill Program Disk, disk image'), ('Mill-%s.hqx', 'BinHex'), ('Mill-%s.bin', 'MacBinary'), ('Mill-%s.dsk', 'disk image')]
SHOWN = 3   # releases on the page: the newest, and the two before it
SITE = 'http://mill.1in1e6.com/'
# How to make the Mill System Disk from your own Apple disks: it holds Apple's System, so it is
# never offered for download
SYSTEM_DISK = 'https://github.com/eric-cgn/mill/blob/main/docs/system-disk.md'
SYSTEM_PARTS = ('#1E6-DM01-N', '#1E6-DM01-P')     # its Network and Personal Editions


def changes(version):
    '''A version's change log entries (src/changes.html), as plain text'''
    log = open(os.path.join(here, '..', 'src', 'changes.html'), encoding='latin-1').read()
    m = re.search(r'<H3>%s</H3>\s*<UL>(.*?)</UL>' % re.escape(version), log, re.S)
    if not m:
        return []
    items = re.findall(r'<LI>(.*?)(?=<LI>|$)', m.group(1), re.S)
    return [html.unescape(re.sub(r'<[^>]+>', '', i)).strip() for i in items]


def vkey(v):
    return [int(p) for p in v.split('.')]


def releases(download):
    """[(version, [(file name, size)])], newest first"""
    found = {}
    for f in os.listdir(download):
        m = re.match(r'(?:Install-)?Mill-([0-9.]+)\.(hqx|bin|dsk|image)$', f)
        if m:
            found.setdefault(m.group(1), {})[f] = os.path.getsize(os.path.join(download, f))
    out = []
    for v, files in found.items():
        out.append((v, [(pat % v, files[pat % v]) for pat, _ in KINDS if pat % v in files]))
    return sorted(out, key=lambda kv: vkey(kv[0]), reverse=True)


def links(v, files):
    what = dict((pat % v, w) for pat, w in KINDS)
    if vkey(v) >= [2, 1]:       # the disks before 2.1 have no part number
        what['Mill-%s.image' % v] = 'Mill Program Disk %s, disk image' % part_number(v)
    return ['<A HREF="download/%s">%s</A> (%s, %dK)' % (f, f, what[f], (size + 1023) // 1024) for f, size in files]


def site(out):
    rel = releases(os.path.join(out, 'download'))[:SHOWN]
    p = ['<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 3.2 Final//EN">',
         '<HTML><HEAD><TITLE>Mill</TITLE></HEAD><BODY>',
         '<H1><IMG SRC="icon.gif" WIDTH=32 HEIGHT=32 ALT=""> Mill</H1>',
         '<P>A web browser for classic Macs. Freeware from OneInAMillion Software.',
         '<P>Mill is a capable web browser for monochrome Macs. It is the most feature-complete, '
         'no-nonsense browser available for the Plus and later.',
         '<P><B>System Requirements:</B>',
         '<UL><LI>Mac Plus or later, 1 MB RAM<LI>System 6.0.3, MacTCP 1.1, or later<LI>700K free RAM'
         '<LI>Recommended: System 6.0.8, 2 MB RAM'
         '<LI>About 800K of disk for everything</UL>']
    if rel:
        v, files = rel[0]
        p.append('<H2>Download Mill %s</H2>' % v)
        system = ('Mill System Disk, Network and Personal Editions (%s and %s) &mdash; Non-Distributable, '
                  'Box Set Only (<A HREF="%%s">Instructions</A>)' % SYSTEM_PARTS) % SYSTEM_DISK
        p.append('<UL>' + ''.join('<LI>' + l for l in links(v, files) + [system]) + '</UL>')
        new = changes(v)
        if new:
            p.append('<P><B>New in %s:</B>' % v)
            p.append('<UL>' + ''.join('<LI>' + html.escape(c, quote=False) for c in new) + '</UL>')
        if len(rel) > 1:
            p.append('<H2>Earlier versions</H2>')
            p.append('<UL>' + ''.join('<LI>%s: %s' % (v, ', '.join(links(v, f))) for v, f in rel[1:]) + '</UL>')
    p.append('<P><A HREF="changes.html">Change log</A>')
    p.append('</BODY></HTML>')
    open(os.path.join(out, 'index.html'), 'w', encoding='latin-1', errors='xmlcharrefreplace').write('\n'.join(p) + '\n')
    if rel:
        latest(out, rel[0][0])
    shutil.copy(os.path.join(here, '..', 'art', 'icon.gif'), out)
    # the change log, which Mill's About page links to: src/changes.html as it is
    shutil.copy(os.path.join(here, '..', 'src', 'changes.html'), os.path.join(out, 'changes.html'))


def latest(out, v):
    '''latest.txt, which Mill's Check for updates reads (src/core/update.h): the newest
    version, the address of its MacBinary file, and what's new in it, a line each. In
    Windows-1252, which Mill reads as Latin-1 with its curly quotes'''
    lines = [v, SITE + 'download/%s-%s.bin' % ('Install-Mill' if int(v.split('.')[0]) >= 2 else 'Mill', v)]
    lines += [' '.join(c.split()) for c in changes(v)]
    open(os.path.join(out, 'latest.txt'), 'w', encoding='cp1252', errors='replace', newline='\n').write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    if sys.argv[1:2] == ['notes']:
        print('\n'.join('- ' + c for c in changes(sys.argv[2])))
    elif sys.argv[1:2] == ['site']:
        site(sys.argv[2])
    else:
        sys.exit(__doc__)
