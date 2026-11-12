# Mill

A web browser for classic Macs.

Mill is a capable web browser for classic monochrome Macs. It is the most feature-complete,
no-nonsense browser available for the Plus and later. It is designed to fill a software gap
that left the 68000-based machines without a browser capable of rich layouts, including tables
and inline images, which is achievable for them.

Freeware from OneInAMillion Software.

![The splash screen while Mill starts](screenshots/splash.png)
![The Millionth's front page](screenshots/front.png)
![Mill's settings page](screenshots/settings.png)

On a Mac Plus running System 6.0.8.

## System requirements

- Mac Plus or later
- System 6.0.3 with MacTCP or later
- 700K free

## Capabilities

- HTML 3.2: text, lists, tables, images (incl. maps), and forms
- Non-standard common extensions: bgcolor, font face
- Character sets: UTF-8, ISO 8859-1, and Mac Roman
- Image formats: GIF (static) and JPEG (baseline), dithered for one-bit screens
- Cookies (session only)
- Basic and Digest logins
- Downloads, optional MacBinary decoding
- HTTP proxy support
- URL schemes: http:// (https:// w/ proxy), file://, and mill:// (built-ins)

## Download

From the [releases](https://github.com/eric-cgn/mill/releases), or at
http://mill.1in1e6.com/

## Building

`./build.sh` builds `build/Mill.bin` (MacBinary) and `build/Mill.dsk` with
[Retro68](https://github.com/autc04/Retro68) in a container (image pinned by digest).
The code is plain 68000. `tests/run.sh` runs the host tests for the portable core.

## License

MIT. Mill also includes ChaN's TJpgDec (`src/third_party/tjpgd`), and the built app parts of
newlib, Multiversal Interfaces, and the Retro68 and GCC runtimes; their notices are in
`src/license.html`, shown as mill://license.

The splash screen is George A. Harker's drawing of Don Quixote and the windmills, from James
Baldwin's *Stories of Don Quixote* (1910), in the public domain (see `art/README`).
