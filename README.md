# Ricoh SP 150 on macOS 26 / 27 (Apple Silicon): print again, natively

Get the **Ricoh SP 150** (USB, host based, GDI) printing on **macOS 26 "Tahoe"
and macOS 27**, on **Apple Silicon and Intel**, with no Ricoh software, no
Rosetta 2, no Ghostscript, no print server. Two small native CUPS filters built
from Apple system frameworks plus a bundled JBIG1 encoder do the whole job.

## Symptoms

- macOS 26: the printer is listed and "idle", jobs vanish or stop with
  `Filter failed`; `/var/log/cups/error_log` says `Filter "cgpdftoraster" not found`.
- macOS 27 (or any Apple Silicon Mac without Rosetta 2): jobs stop with
  `"Data" failed` and the log says
  `RICOH_SP_150Filter: Bad CPU type in executable`.

## Why it happens

The SP 150 has no PostScript, PCL or AirPrint. The host must render every page
and send a proprietary bitmap stream ("LHPL": PJL framing around JBIG1 data).
Ricoh's macOS driver did this with two pieces, and Apple has since removed the
ground under both:

1. `cgpdftoraster`, Apple's PDF to CUPS raster filter, was dropped from macOS 26.
2. `RICOH_SP_150Filter.app`, Ricoh's raster to LHPL filter, is an x86_64 binary
   from 2016. It only runs through Rosetta 2, and every macOS major update
   removes Rosetta until you reinstall it. Apple has announced that Rosetta goes
   away for good after macOS 27.

## The fix

Both steps are replaced by native code:

```
PDF  ->  pdftoraster_cg (CoreGraphics)  ->  CUPS raster
     ->  rastertolhpl (dither + JBIG1 + PJL)  ->  RICOH SP 150
```

- `src/pdftoraster_cg.c` renders each PDF page with CoreGraphics into 8 bit
  gray at 600 dpi and writes CUPS raster (all nine paper sizes of the PPD, copies
  passed on via `NumCopies`).
- `src/rastertolhpl.c` turns the gray raster into the printer's LHPL stream. Its
  output is **byte for byte identical** to what Ricoh's own filter produces for the
  same page (verified against the original driver on test pages including a
  256 step gray ramp), because it reuses the driver's exact 32x32 halftone matrix
  (`src/lhpl_screen.h`) and the same JBIG1 parameters.
- `filters/pdftoricoh` chains the two; the PPD points at it.
- `ppd/RICOH_SP_150.ppd` is a minimal PPD for fresh installs where Ricoh's driver
  was never installed. If the queue already exists, its PPD is kept and only the
  filter line is changed.

Because only system frameworks and libcups are used, everything runs inside the
CUPS filter sandbox (which is why a Homebrew Ghostscript approach fails: the
sandbox blocks executing binaries from `/opt/homebrew`).

## Requirements

- macOS 26 or 27, Apple Silicon or Intel.
- Xcode Command Line Tools (`xcode-select --install`) for the C compiler.
- The SP 150 connected via USB. Ricoh's driver is **not** needed anymore.

## Install

```bash
git clone https://github.com/Wiredframe/ricoh-sp150-macos-tahoe.git
cd ricoh-sp150-macos-tahoe
./install.sh                 # or: ./install.sh YOUR_PRINTER_QUEUE_NAME
```

The script asks for your password (via `sudo`), compiles and signs the two
filters, installs the wrapper, repoints the PPD (backed up once as
`RICOH_SP_150.ppd.orig`) or adds the queue with the bundled PPD, and reloads
CUPS. Then print any PDF.

The default queue name is `RICOH_SP_150`. If yours differs, find it with
`lpstat -p` and pass it as the first argument.

## Uninstall

```bash
./uninstall.sh               # or: ./uninstall.sh YOUR_PRINTER_QUEUE_NAME
```

Restores the original PPD and removes the installed files. It does not touch
Ricoh's own `RICOH_SP_150Filter.app` if you still have it.

## The LHPL stream, documented

Reverse engineered from the vendor filter's output; may help with sibling
models (the vendor filter is a Fuji Xerox derived "rastertoFX..." build).

```
ESC%-12345X@PJL JOB NAME=PRINTER
@PJL SET JOBATTR=HST:<host>      USR:<user>  DOC:<title>
@PJL SET JOBATTR=DATE:MM/DD/YYYY TIME:HH:MM:SS
@PJL SET DUPLEX=OFF / MEDIASOURCE=0 / RENDERMODE=GRAYSCALE
@PJL SET RESOLUTION=600 / BITSPERPIXEL=1 / COPIES=<n>
@PJL ENTER LANGUAGE=LHPL
ESC LH@sj  01 00 01 00 + 53 x 00                     + xor   (job start)
per page:
  ESC LH@sp  u8 paper  u8 01  u32 width  u32 height  u32 bytes
             u32 jbiglen  u32 jbiglen  16 x 00
             u16 dpi  u16 width/0.1mm  u16 height/0.1mm  13 x 00 + xor
  <JBIG1 BIE: 1 plane, L0=128 lines/stripe, LRLTWO, SDRST, mx=0>
  ESC LH@ep  57 x 00 + xor                         (vendor: uninitialised memory)
ESC%-12345X@PJL EOJ
```

- Every block is 64 bytes: 6 byte prefix, 57 byte body, trailing XOR of the
  preceding 63 bytes. All integers little endian.
- Paper codes: Letter 1, A4 2, A5 3, A6 4, B5 5, B6 6, Executive 7, 16K 8, Legal 9.
- Width is padded up to a multiple of 512 px (A4 4958 -> 5120). Height is the
  page height in pixels. Millimetre fields are truncated from points.
- 1 = black. Gray to 1 bit: pixel is black when `gray < screen[y % 32][x % 32]`.

Tips for developers: `pdftoraster_cg` honours `PDFTORICOH_DUMP_PNG=/path/out.png`
to dump page 1 as PNG. To inspect a stream, cut the JBIG payload out of the `sp`
block and decode it with jbigkit's `jbgtopbm`.

## Limitations

- Covers `application/pdf` jobs, which is what macOS apps send through the normal
  print dialog. Pure text or raw image jobs would need Apple's other filters.
- 600 dpi only, grayscale only, single sided (the SP 150 has no duplex unit).
  The vendor driver's "1200 dpi" and toner save modes are not reproduced.

## Licenses

Own code (`src/pdftoraster_cg.c`, `src/rastertolhpl.c`, scripts, PPD): MIT, see
`LICENSE`. `src/jbig/` is jbigkit 2.1 by Markus Kuhn, GPL 2.0 or later, see
`src/jbig/COPYING`; the resulting `rastertolhpl` binary is therefore GPL.
