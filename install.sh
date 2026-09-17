#!/bin/bash
# install.sh - restore printing for the RICOH SP 150 on macOS 26/27, natively.
#
# What it does:
#   1. compiles pdftoraster_cg (PDF -> CUPS raster via Apple CoreGraphics)
#   2. compiles rastertolhpl (CUPS raster -> RICOH LHPL stream, JBIG1 via jbigkit)
#   3. installs both plus the pdftoricoh wrapper into the CUPS filter directory
#   4. points the printer PPD at the wrapper (original PPD is backed up as .orig),
#      or adds the printer queue with the bundled PPD if it does not exist yet
#   5. reloads CUPS
#
# No Ricoh software, no Rosetta 2 and no Ghostscript are needed.
#
# Usage:  ./install.sh [PRINTER_QUEUE_NAME]        (default: RICOH_SP_150)

set -euo pipefail

PRINTER="${1:-RICOH_SP_150}"
DST="/usr/libexec/cups/filter"
PPD="/etc/cups/ppd/${PRINTER}.ppd"
HERE="$(cd "$(dirname "$0")" && pwd)"

if ! command -v cc >/dev/null 2>&1; then
  echo "ERROR: no C compiler found. Install the Xcode Command Line Tools:"
  echo "  xcode-select --install"
  exit 1
fi

# Re-run as root for the install steps.
if [ "$(id -u)" -ne 0 ]; then
  echo "Elevating with sudo..."
  exec sudo "$0" "$@"
fi

echo "==> Compiling pdftoraster_cg"
cc -O2 -o "$DST/pdftoraster_cg" "$HERE/src/pdftoraster_cg.c" \
   -framework CoreGraphics -framework CoreFoundation -framework ImageIO \
   -lcupsimage -lcups

echo "==> Compiling rastertolhpl (with bundled jbigkit)"
cc -O2 -o "$DST/rastertolhpl" "$HERE/src/rastertolhpl.c" \
   "$HERE/src/jbig/jbig.c" "$HERE/src/jbig/jbig_ar.c" -I"$HERE/src" \
   -lcupsimage -lcups

echo "==> Installing pdftoricoh wrapper"
install -m 0755 -o root -g wheel "$HERE/filters/pdftoricoh" "$DST/pdftoricoh"
chown root:wheel "$DST/pdftoraster_cg" "$DST/rastertolhpl"
chmod 0755 "$DST/pdftoraster_cg" "$DST/rastertolhpl"

echo "==> Ad-hoc signing the Mach-O binaries (required on Apple Silicon)"
codesign -s - --force "$DST/pdftoraster_cg"
codesign -s - --force "$DST/rastertolhpl"

# Leftover from the 1.x install (copy of Ricoh's x86_64 filter): no longer used.
[ -f "$DST/RICOH_SP_150Filter" ] && rm -f "$DST/RICOH_SP_150Filter"

if [ -f "$PPD" ]; then
  echo "==> Backing up PPD (once) to ${PPD}.orig"
  [ -f "${PPD}.orig" ] || cp -p "$PPD" "${PPD}.orig"
  echo "==> Pointing the PPD at pdftoricoh"
  /usr/bin/sed -i '' 's#^\*cupsFilter:.*#*cupsFilter: "application/pdf 0 pdftoricoh"#' "$PPD"
  echo "==> Reloading CUPS"
  launchctl kickstart -k system/org.cups.cupsd 2>/dev/null || killall -HUP cupsd 2>/dev/null || true
else
  echo "==> No queue '$PRINTER' yet, adding it with the bundled PPD"
  URI="$(lpinfo -v 2>/dev/null | awk '/usb:\/\/RICOH\/SP%20150/ {print $2; exit}')"
  if [ -z "$URI" ]; then
    echo "ERROR: printer not found on USB. Connect and switch on the SP 150, then re-run."
    echo "       Or add it manually:  lpadmin -p $PRINTER -E -v 'usb://RICOH/SP%20150?serial=...' -P $HERE/ppd/RICOH_SP_150.ppd"
    exit 1
  fi
  lpadmin -p "$PRINTER" -E -v "$URI" -P "$HERE/ppd/RICOH_SP_150.ppd" -o printer-is-shared=false
  lpadmin -d "$PRINTER" 2>/dev/null || true
fi

echo
echo "Done. Test it by printing any PDF, for example:"
echo "  lp -d $PRINTER -o media=A4 /path/to/some.pdf"
echo "  (or just print from any app via the normal print dialog)"
