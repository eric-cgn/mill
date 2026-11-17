# The Mill System Disk

Mill's release has a Mill Program Disk: Mill, ready to run, on an 800K floppy. Its partner, the
"Mill System Disk", is an 800K System 6.0.5 startup disk that leaves a 1 MB Mac Plus enough
memory for Mill. It holds Apple's System software and MacTCP, which Apple's license does not
let us redistribute, so it is never offered for download. These are the instructions for making it
yourself from Apple's own free downloads.

With the two disks, a Mac with no hard disk starts from the Mill System Disk in one drive and
runs Mill from the Mill Program Disk in the other.

## Two editions

| Part number | Edition | For | Has |
|---|---|---|---|
| #1E6-DM01-N | Network Edition | LocalTalk or Ethernet | AppleShare |
| #1E6-DM01-P | Personal Edition | a modem | MacPPP 2.0.1 (PPP and Config PPP), no AppleShare |

Everything else is the same, and both are named "Mill System Disk". If the Mac is on
LocalTalk or Ethernet, make the Network Edition; if it dials in with a modem, make the
Personal Edition.

## Making it

[`tools/systemdisk/`](../tools/systemdisk/) has a script that makes it in one command:
`tools/systemdisk/make_system_disk.py`. It sets up its own Python environment, downloads
Apple's System 6.0.5 disks and one disk of System 7.5 Update 2.0 from the Internet Archive,
unpacks them itself, and writes the "Mill System Disk" image: the Network Edition, or with
`--personal` the Personal Edition, for which it also downloads MacPPP. Its Read Me there has
the details.

MacTCP isn't on Apple's 6.0.5 disks. In the 1990s MacTCP came with site licenses and
Internet starter kits; Apple later included it in the free System 7.5.3 release, which is
where the tool takes it from. System 7.5 Update 2.0, the free update to 7.5.3, has MacTCP
2.0.6 on its third disk, inside the Installer's archive; the script unpacks it from there.
MacTCP 2.0.6 runs on 6.0.5. `--mactcp` uses your own copy instead, and `--no-mactcp` leaves
it off.

The script is licensed separately from Mill, under the GNU LGPL 2.1 or later (Mill itself is
MIT). It is a standalone tool: Mill doesn't use its code, and it isn't part of any Mill binary
or disk image.

You can also make the disk by hand, as below. Either way you need:

- Apple's System 6.0.5 disks: System Tools, Utilities 1, Utilities 2, and Printing Tools.
- MacTCP 2.0.6 (the script takes it from System 7.5 Update 2.0). MacTCP's control panel
  calls Gestalt, so it requires 6.0.4 or later; 6.0.5 is fine.
- By hand: a blank 800K disk, or an 800K disk image, and a way to edit resources (ResEdit).

Work on copies. Keep the originals as they are.

## What goes on it

One System Folder, named "System Folder", holding:

| File | From |
|---|---|
| System (trimmed, see below) | System Tools |
| Finder (6.1.5) | System Tools |
| General | System Tools |
| Clipboard File | System Tools |
| Keyboard, Mouse, Sound | Utilities 1 |
| AppleShare | Utilities 1 |
| Map | Utilities 2 |
| LaserWriter, Laser Prep, ImageWriter | Printing Tools |
| MacTCP | MacTCP 2.0.6 |

The Personal Edition leaves out AppleShare and has PPP and Config PPP from MacPPP 2.0.1 in
its place (see below).

Nothing else: no MultiFinder (a 1 MB Plus runs Mill from the Finder), no other desk accessories
or control panels, and only the fonts the 6.0.5 System has in it. The disk keeps System Tools'
boot blocks, so the easiest start is a copy of the System Tools disk, renamed "Mill System
Disk", with everything not in the table deleted and the rest copied in.

### Trimming the System

Five resources are removed from the System to make room. A Plus never uses them:

| Resource | What it is |
|---|---|
| `PTCH` 376 | the ROM patch for the original Mac II |
| `DRVR` 120 | the Mac II video driver |
| `snd ` 2, 3, 4 | the alert sounds Clink-Klank, Boing, and Monkey |

Nothing refers to the sounds by number. The Sound control panel lists the System's `snd `
resources in order and keeps the choice in parameter RAM, and if the chosen sound is gone the
beep is Simple Beep (`snd ` 1), which stays.

### AppleShare

On the Network Edition, AppleShare (the Chooser device) also needs its drivers in the System: `DRVR` 40 (.XPP) and
`DRVR` 41 (.AFPTranslator), with `ALRT`, `DITL`, and `STR#` -15072. Apple's Installer adds these
when it installs AppleShare from the 6.0.5 disks. Install AppleShare onto the disk with it,
or copy the five resources from a System where AppleShare is installed.

There is no AppleShare Prep file to copy: AppleShare makes its own when you set a server
volume to open at startup.

If you would rather print to an ImageWriter on AppleTalk than use file servers, put
AppleTalk ImageWriter (Printing Tools) on the disk in place of AppleShare (`--atimagewriter`).
There is not room for both (see below).

### MacPPP

The Personal Edition has MacPPP 2.0.1 from Merit Network and the University of Michigan: the
PPP connection for MacTCP and the Config PPP control panel. The script downloads it from the
Info-Mac archive at FUNET (`mac-ppp-201.hqx`). Its license:

> MacPPP was developed at, and is copyrighted by, the Merit Network, Inc. and the University
> of Michigan. Merit and the University of Michigan grant an unlimited license for use and
> redistribution of the executable program provided that it is not sold for profit, either
> as is or as part of another product. Charges to recover the cost of duplication and
> distribution are permitted. MacPPP is offered "as is"-neither Merit nor the University of
> Michigan make any guarantees about the performance or reliability of the software.

It goes on the disk unchanged. It adds about 48K. Config PPP reserves 128K of system heap,
so on a 1 MB Plus Mill gets about 30K less memory than with the Network Edition. Choose PPP
in MacTCP, and set up the modem and number in Config PPP.

Mill opens MacTCP only when it needs the network, so it doesn't dial when it starts, unless
Connection check is turned on from the Settings page.

### MacTCP

Copy MacTCP into the System Folder and start up from the disk. MacTCP makes MacTCP DNR next
to it at the first startup. Then set MacTCP up for your own network in its control panel.
That writes a MacTCP Prep file next to them.

## Leave 20K free

Leave about 20K free on the disk. On a nearly full disk, MacTCP DNR loses its resolver code
(its `dnrp` resource, 16K) at startup, and names no longer resolve. It did with both
AppleShare and AppleTalk ImageWriter on the disk, which left 16K free. That is why the disk has
one or the other.

As listed, the Network Edition has about 78K free when it is made, and about 52K after its
first startup (MacTCP DNR and the Finder's Desktop file take the rest; MacTCP Prep, once
MacTCP is set up, takes a little more). The Personal Edition has about 80K free when made,
and also about 52K after its first startup.

## What it gives you

Started from the Network Edition only, a Mac Plus with 1 MB RAM has the full 700K for Mill; from
the Personal Edition, about 30K less. Put the Mill Program Disk in the other drive and open
Mill from it.
