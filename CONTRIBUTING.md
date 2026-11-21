# Contributing to Mill

Mill is a web browser for the Macs of its period, and its period is December 25, 1997.

## Period-correct features only

With few exceptions, Mill accepts only features that existed on or before December 25, 1997:
protocols, file formats, HTML, and the way Mac programs behaved. A pull request that adds a
feature says when it appeared and cites a primary source: the RFC, the W3C or vendor
specification, or the release that introduced it.

Anything from after that date belongs in [mill-extras](https://github.com/eric-cgn/mill-extras),
the Millennium Pack: Plug-Ins that work with Mill but are openly out of period, such as
Mastodon, Markdown, Gemtext, and MQTT. A new kind of Plug-In needed only by an out-of-period
feature is discussed there first.

The exceptions:

- Bug fixes, including ones a period browser would have had.
- What it takes to work with today's servers, when a period feature can't otherwise be used:
  for example, a fix for how a server answers HTTP.
- Security fixes.
- Building, testing, and releasing: the tools that make Mill are not part of it.

## The base stays small

Mill runs on a Mac Plus with 1 MB of memory, and every byte of the base program is spent on
every Mac. Keep a change as small as it can be, measure what it adds to the code (`CODE`
resources) and to the other resources, and say so in the pull request. A feature that can be a
Plug-In should be one.

## How changes are made

- Branch from `dev` and open a pull request into `dev`. The checks build Mill and run
  `tests/run.sh`; they must pass.
- `dev` goes into `main` only by pull request, and that makes a release.
- A user-visible change adds a line to `src/changes.html`: short and plain, like the lines
  already there. Changing that file changes the layout checksums in
  `tests/fixtures/layout.sums`: regenerate them with `test_layout sums` (see
  `tests/test_layout.c`), built in the same Retro68 image as `tests/run.sh` uses, as
  `linux/amd64`.
- Say what you tested, and on which System and Mac (or emulator).

## Writing

On-screen text, change log lines, and documents are short and plain. American spelling, and
the serial comma.

## Licenses

Mill is under the MIT License. Code from elsewhere must be under a license compatible with it,
and carries its license text in the file and in the repository.
