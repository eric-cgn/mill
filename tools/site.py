#!/usr/bin/env python3
'''Mill's download site (mill.1in1e6.com), plain HTML 3.2 for classic Macs, built from
the release files in <site>/download (Mill-VERSION.hqx, .bin, .dsk) and src/changes.html.
python3 tools/site.py site <site dir>     writes <site dir>/index.html
python3 tools/site.py notes VERSION       prints a version's change log entries as text'''

import html, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
KINDS = [('hqx', 'BinHex'), ('bin', 'MacBinary'), ('dsk', 'disk image')]


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
    found = {}
    for f in os.listdir(download):
        m = re.match(r'Mill-([0-9.]+)\.(hqx|bin|dsk)$', f)
        if m:
            found.setdefault(m.group(1), {})[m.group(2)] = os.path.getsize(os.path.join(download, f))
    return sorted(found.items(), key=lambda kv: vkey(kv[0]), reverse=True)


def links(v, files):
    return ['<A HREF="download/Mill-%s.%s">Mill-%s.%s</A> (%s, %dK)' % (v, ext, v, ext, what, (files[ext] + 1023) // 1024)
            for ext, what in KINDS if ext in files]


def site(out):
    rel = releases(os.path.join(out, 'download'))
    p = ['<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 3.2 Final//EN">',
         '<HTML><HEAD><TITLE>Mill</TITLE></HEAD><BODY>',
         '<H1>Mill</H1>',
         '<P>A web browser for classic Macs. Freeware from OneInAMillion Software.',
         '<P>Mill is a capable web browser for monochrome Macs. It is the most feature-complete, '
         'no-nonsense browser available for the Plus and later.',
         '<P><B>System Requirements:</B>',
         '<UL><LI>Mac Plus or later<LI>System 6.0.8 with MacTCP or later<LI>1 MB RAM available</UL>']
    if rel:
        v, files = rel[0]
        p.append('<H2>Download Mill %s</H2>' % v)
        p.append('<UL>' + ''.join('<LI>' + l for l in links(v, files)) + '</UL>')
        new = changes(v)
        if new:
            p.append('<P><B>New in %s:</B>' % v)
            p.append('<UL>' + ''.join('<LI>' + html.escape(c, quote=False) for c in new) + '</UL>')
        if len(rel) > 1:
            p.append('<H2>Earlier versions</H2>')
            p.append('<UL>' + ''.join('<LI>%s: %s' % (v, ', '.join(links(v, f))) for v, f in rel[1:]) + '</UL>')
    p.append('<HR><P>Source code: github.com/eric-cgn/mill')
    p.append('</BODY></HTML>')
    open(os.path.join(out, 'index.html'), 'w', encoding='latin-1').write('\n'.join(p) + '\n')


if __name__ == '__main__':
    if sys.argv[1:2] == ['notes']:
        print('\n'.join('- ' + c for c in changes(sys.argv[2])))
    elif sys.argv[1:2] == ['site']:
        site(sys.argv[2])
    else:
        sys.exit(__doc__)
