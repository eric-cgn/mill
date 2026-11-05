# Mill

A web browser for classic Macs.

Mill is a capable web browser for monochrome Macs. It is the most feature-complete,
no-nonsense browser available for the Plus and later.

Freeware from OneInAMillion Software.

![The splash screen while Mill starts](screenshots/splash.png)
![The Millionth's front page](screenshots/front.png)
![Mill's settings page](screenshots/settings.png)

On a Mac Plus (512×342) running System 6.0.8.

## System requirements

- Mac Plus or later
- System 6.0.8 with MacTCP or later
- 1 MB RAM available

## What it reads

- HTML 3.2: text, lists, tables, images (and maps), and forms, plus a few extras like
  bgcolor and font face
- URL schemes: http://, file://, and mill:// (built-ins)
- Character sets: UTF-8, ISO 8859-1, and Mac Roman
- Image formats: GIF (static) and JPEG (baseline), dithered for one-bit screens
- Cookies, kept until you quit
- Basic and Digest logins
- Downloads, with MacBinary decoding

## Download

From the [releases](https://github.com/eric-cgn/mill/releases), or at
http://mill.1in1e6.com/

## Building

`./build.sh` builds `build/Mill.bin` (MacBinary) and `build/Mill.dsk` with
[Retro68](https://github.com/autc04/Retro68) in a container (image pinned by digest).
The code is plain 68000. `tests/run.sh` runs the host tests for the portable core.

## Releases

A change that lands on main with a new `MILL_VERSION` (`src/version.h`) is released: the
Release workflow builds Mill, tags the commit `vX.Y.Z`, publishes `Mill-X.Y.Z.hqx`, `.bin`,
and `.dsk` (`tools/release.py`), and rebuilds the download site (`tools/site.py`). Changes
that keep the version aren't released.

## License

MIT. Mill also includes ChaN's TJpgDec (`src/third_party/tjpgd`), and the built app parts of
newlib, Multiversal Interfaces, and the Retro68 and GCC runtimes; their notices are in
`src/license.html`, shown as mill://license.

The splash screen is George A. Harker's drawing of Don Quixote and the windmills, from James
Baldwin's *Stories of Don Quixote* (1910), in the public domain (see `art/README`).
