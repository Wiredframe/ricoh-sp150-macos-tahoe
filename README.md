# Ricoh SP 150 on macOS 26 (Tahoe): print again

Get the **Ricoh SP 150** (USB, host based) printing again on **macOS 26 "Tahoe"**,
including **Apple Silicon**, using only Apple system frameworks. No Ghostscript,
no print server, no third party daemon.

The printer shows as online and ready, but every job silently fails or is
rejected. This project restores the one piece Apple removed and gets you back to
normal printing.

## Symptoms

- The printer is listed, shows "idle" / "ready", accepts jobs, but nothing prints.
- Jobs vanish, or you see `Stopped - "Filter failed"`.
- `/var/log/cups/error_log` is full of lines like:

  ```
  Filter "cgpdftoraster" not found.
  Filter "cgtexttopdf" not found.
  Returning IPP client-error-document-format-not-supported for Send-Document
  ```

## Why it happens

Apple and OpenPrinting deprecated classic printer drivers over many years
(PPD files since 2008, raw queues 2018, drivers since CUPS 2.3 in 2019). In
macOS 26 the classic CUPS raster filters were removed from the system, including
`cgpdftoraster`, which used to convert the incoming PDF into CUPS raster.

The Ricoh SP 150 is a USB only, host based (GDI) printer with no AirPrint. Its
driver can only be driven through the old chain:

```
PDF  ->  cgpdftoraster  ->  CUPS raster  ->  RICOH_SP_150Filter  ->  printer
```

The vendor filter `RICOH_SP_150Filter.app` is still installed and still works.
It just never receives any data, because the step that feeds it (`cgpdftoraster`)
is gone. So the printer looks perfectly healthy while nothing ever reaches it.

## The fix

We put the missing step back with a tiny native filter, `pdftoraster_cg`, that
renders the PDF to 8 bit grayscale CUPS raster using Apple's own **CoreGraphics**
(the same technology Apple's `cgpdftoraster` used internally). A small wrapper,
`pdftoricoh`, chains it into the existing Ricoh vendor filter:

```
PDF  ->  pdftoraster_cg (CoreGraphics)  ->  CUPS raster  ->  RICOH_SP_150Filter  ->  printer
```

Because it uses only system frameworks, it runs inside the CUPS filter sandbox,
which is why a Homebrew Ghostscript based approach does not work here: the sandbox
blocks executing binaries from `/opt/homebrew`.

### Two Apple Silicon gotchas this handles for you

1. **Unsigned binary gets killed.** The Ricoh vendor filter is unsigned. On Apple
   Silicon the kernel (AMFI) kills unsigned executables with SIGKILL. The installer
   ad-hoc signs a copy (`codesign -s -`).
2. **The `.app` suffix gets killed too.** macOS refuses to exec a plain binary whose
   name ends in `.app` (it is treated as an app bundle). The installer runs a copy
   named `RICOH_SP_150Filter` without the suffix.

The x86_64 slice of the vendor filter runs through **Rosetta 2**, so Rosetta must be
installed (`softwareupdate --install-rosetta --agree-to-license`).

## Requirements

- macOS 26 (Tahoe), Intel or Apple Silicon.
- Xcode Command Line Tools (`xcode-select --install`) for the C compiler.
- Rosetta 2 on Apple Silicon (see above).
- **The Ricoh SP 150 driver already installed and the printer already added.**
  This project does not ship Ricoh's driver. It reuses the `RICOH_SP_150Filter.app`
  and the PPD that the Ricoh driver installed on your machine.

## Install

```bash
git clone https://github.com/<your-user>/ricoh-sp150-macos-tahoe.git
cd ricoh-sp150-macos-tahoe
./install.sh                 # or: ./install.sh YOUR_PRINTER_QUEUE_NAME
```

The script asks for your password (via `sudo`), backs up the PPD to
`RICOH_SP_150.ppd.orig`, compiles and installs the filter, signs the binaries,
repoints the PPD, and reloads CUPS. Then print any PDF.

The default printer queue name is `RICOH_SP_150`. If yours differs, find it with
`lpstat -p` and pass it as the first argument.

## Uninstall

```bash
./uninstall.sh               # or: ./uninstall.sh YOUR_PRINTER_QUEUE_NAME
```

Restores the original PPD and removes the installed files. It does not touch
Ricoh's own `RICOH_SP_150Filter.app`.

## How it works, in detail

- `src/pdftoraster_cg.c`: a CUPS filter. Opens the PDF (file argument or stdin),
  renders each page with CoreGraphics into an 8 bit `DeviceGray` bitmap at 600 dpi,
  and writes CUPS raster with libcups (`cupsRasterWriteHeader2` / `cupsRasterWritePixels`).
  The raster parameters match what the Ricoh PPD declares: `cupsColorSpace W`,
  8 bits per color, 600 dpi. Paper size defaults to A4 and honors Letter from the
  job options.
- `filters/pdftoricoh`: pipes `pdftoraster_cg` into the signed, suffix free vendor
  filter, passing the CUPS filter arguments through.
- `install.sh` / `uninstall.sh`: wire it into CUPS and back out again.

Tip for developers: `pdftoraster_cg` supports `PDFTORICOH_DUMP_PNG=/path/out.png`
to dump page 1 as a PNG, which is what the exact output looks like before it goes
to the printer. Handy for checking orientation and tone without wasting paper.

## Limitations

- This covers `application/pdf` jobs, which is what macOS apps send through the
  normal print dialog. Pure `text/plain` or raw image jobs would need Apple's other
  removed filters (`cgtexttopdf`, `cgimagetopdf`) and are not handled here.
- macOS updates can overwrite files under `/usr/libexec/cups/filter`. If printing
  breaks again after an update, just run `./install.sh` again.
- Only the Ricoh SP 150 is targeted. Other host based Ricoh models may need
  different PPD raster parameters.

## Legal

This repository contains only original code (MIT licensed). It does not include,
bundle, or redistribute any Ricoh driver, filter, or firmware. You must obtain and
install the Ricoh SP 150 driver yourself. Ad-hoc signing and renaming a local copy
of a filter you already installed, for interoperability on your own machine, is
what the installer does on your system. Ricoh and SP 150 are trademarks of their
respective owners, used here for identification only.

## Credits

Diagnosis and implementation worked out interactively with Claude Code. The
`.app` suffix pitfall echoes long standing community reports about Ricoh filters
on macOS.
