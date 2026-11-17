#!/usr/bin/env python3
"""Install Mill, made with EUP: build/installer/Install-Mill-VERSION.bin and .hqx. Copies
installer/mill.ini (its version lines set from src/version.h), the files it names, and the SDK
into build/installer/stage, fetches EUP's source release (pinned, checked by SHA-256), builds
its eup command, and runs eup check and eup build on the stage. Needs what ./build.sh and
./build.sh disk make, and "Install icon.bin" in the stage (./build.sh installer makes it from
installer/icons.r with Rez). ./build.sh installer runs it."""
import hashlib, os, re, shutil, subprocess, sys, tarfile, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import disk, kit

# EUP 1.3: git archive --prefix=eup-1.3/ of EUP's v1.1 branch at fa047be, as its release.sh makes it.
# Until that release is on GitHub, put the archive in build/ (it is used from there, checked).
EUP_VERSION = '1.3'
EUP_URL = 'https://github.com/eric-cgn/eup/releases/download/v%s/eup-%s.tar.gz' % (EUP_VERSION, EUP_VERSION)
EUP_SHA256 = '43cc377bf82aa8af0e4dfd868c58244c6c24caadaecd659c5ced00a92a59cad9'

root = kit.root
out = os.path.join(root, 'build', 'installer')
stage = os.path.join(out, 'stage')


def eup():
    """EUP's eup command, built from its source release, which is fetched once into build/"""
    top = os.path.join(root, 'build', 'eup-%s' % EUP_VERSION)
    tgz = top + '.tar.gz'
    if not os.path.exists(tgz):
        with urllib.request.urlopen(EUP_URL) as r:
            data = r.read()
        if hashlib.sha256(data).hexdigest() != EUP_SHA256:
            sys.exit('%s: not the expected file (SHA-256)' % EUP_URL)
        open(tgz, 'wb').write(data)
    if hashlib.sha256(open(tgz, 'rb').read()).hexdigest() != EUP_SHA256:
        sys.exit('%s: not the expected file (SHA-256); delete it to fetch it again' % os.path.relpath(tgz, root))
    shutil.rmtree(top, ignore_errors=True)
    with tarfile.open(tgz) as t:
        for m in t.getmembers():
            if not (m.isfile() or m.isdir()) or m.name.startswith('/') or '..' in m.name.split('/'):
                sys.exit('%s: unexpected item %s' % (tgz, m.name))
        t.extractall(os.path.dirname(top))
    subprocess.run(['make', '-s', '-C', top, 'eup'], check=True)
    return os.path.join(top, 'eup')


def ini(v):
    """installer/mill.ini with Mill's version. EUP's version takes 2.0.1 or 2.1b2, so a dev build's
    2.0.1-dev-46a0304 goes in the title, Get Info, and the file names, and 2.0.1 in the version."""
    h = open(os.path.join(root, 'src', 'version.h')).read()
    year = re.search(r'#define MILL_YEAR "([^"]+)"', h).group(1)
    who = re.search(r'#define MILL_PUBLISHER "([^"]+)"', h).group(1)
    num = re.match(r'\d+\.\d(\.\d)?([dab]\d+)?$', v)
    keys = {'version': num.group(0) if num else re.match(r'\d+\.\d(\.\d)?', v).group(0),
            'get_info': 'Install Mill %s, © %s %s. Freeware.' % (v, year, who)}
    if not num:
        keys['title'] = 'Install Mill %s' % v
    lines, section = [], None
    for line in open(os.path.join(root, 'installer', 'mill.ini'), encoding='utf-8'):
        s = re.match(r'\s*\[(\w+)', line)
        k = re.match(r'\s*(\w+)\s*=', line)
        if s:
            section = s.group(1).lower()
        elif section == 'installer' and k and k.group(1).lower() in keys:
            continue
        lines.append(line)
        if s and section == 'installer':
            lines.extend('%s = %s\n' % kv for kv in keys.items())
    open(os.path.join(stage, 'mill.ini'), 'w', encoding='utf-8').write(''.join(lines))


def main():
    files = {
        'Mill.bin': os.path.join(root, 'build', 'Mill.bin'),
        'Internet Services.bin': os.path.join(root, 'plugins', 'internet', 'build', 'Internet Services.bin'),
        'SMB.bin': os.path.join(root, 'plugins', 'smb', 'build', 'SMB.bin'),
        'NFS.bin': os.path.join(root, 'plugins', 'nfs', 'build', 'NFS.bin'),
        'News.bin': os.path.join(root, 'plugins', 'news', 'build', 'News.bin'),
        'LDAP.bin': os.path.join(root, 'plugins', 'ldap', 'build', 'LDAP.bin'),
        'IMAP.bin': os.path.join(root, 'plugins', 'imap', 'build', 'IMAP.bin'),
        'Image Formats.bin': os.path.join(root, 'plugins', 'images', 'build', 'Image Formats.bin'),
        'Archives.bin': os.path.join(root, 'plugins', 'archive', 'build', 'Archives.bin'),
        'Web Fonts.bin': os.path.join(root, 'build', 'fonts', 'Web Fonts.bin'),
        'Easy Install.txt': os.path.join(root, 'installer', 'Easy Install.txt'),
    }
    disk.need(os.path.join(stage, 'Install icon.bin'), './build.sh installer')
    for name, path in files.items():
        shutil.copyfile(disk.need(path, './build.sh and ./build.sh disk'), os.path.join(stage, name))
    # the SDK as its disk has it, each file as MacBinary, which gives its Mac name
    sdk = os.path.join(stage, 'Mill Plug-In SDK')
    shutil.rmtree(sdk, ignore_errors=True)
    for parts, name, mb in kit.entries():
        d = os.path.join(sdk, *parts)
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, name.replace('/', '-') + '.bin'), 'wb').write(mb)
    ini(kit.version())
    tool = eup()
    subprocess.run([tool, 'check', os.path.join(stage, 'mill.ini')], check=True)
    subprocess.run([tool, 'build', os.path.join(stage, 'mill.ini'), '-o', out], check=True)


if __name__ == '__main__':
    main()
