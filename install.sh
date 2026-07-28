#!/bin/bash
# install.sh - restore printing for the RICOH SP 150 on macOS 26 (Tahoe).
#
# What it does:
#   1. compiles pdftoraster_cg (PDF to CUPS raster via Apple CoreGraphics)
#   2. installs it plus the pdftoricoh wrapper into the CUPS filter directory
#   3. makes a signed, .app-suffix-free copy of your locally installed Ricoh filter
#   4. points the printer PPD at the wrapper (original PPD is backed up as .orig)
#   5. reloads CUPS
#
# It does NOT ship or download Ricoh's proprietary driver. You must install the
# Ricoh SP 150 driver and add the printer yourself first, so that the vendor
# filter and the PPD already exist on this machine.
#
# Usage:  ./install.sh [PRINTER_QUEUE_NAME]        (default: RICOH_SP_150)

set -euo pipefail

PRINTER="${1:-RICOH_SP_150}"
DST="/usr/libexec/cups/filter"
PPD="/etc/cups/ppd/${PRINTER}.ppd"
VENDOR_APP="$DST/RICOH_SP_150Filter.app"
HERE="$(cd "$(dirname "$0")" && pwd)"

# Pre-flight checks (run before elevating so messages are clear).
if [ ! -f "$PPD" ]; then
  echo "ERROR: $PPD not found."
  echo "Install the Ricoh SP 150 driver and add the printer (queue name '$PRINTER') first."
  exit 1
fi
if [ ! -f "$VENDOR_APP" ]; then
  echo "ERROR: vendor filter $VENDOR_APP not found."
  echo "Install the Ricoh SP 150 driver first (it provides RICOH_SP_150Filter.app)."
  exit 1
fi
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

echo "==> Backing up PPD (once) to ${PPD}.orig"
[ -f "${PPD}.orig" ] || cp -p "$PPD" "${PPD}.orig"

echo "==> Compiling pdftoraster_cg"
cc -O2 -o "$DST/pdftoraster_cg" "$HERE/src/pdftoraster_cg.c" \
   -framework CoreGraphics -framework CoreFoundation -framework ImageIO \
   -lcupsimage -lcups

echo "==> Installing pdftoricoh wrapper"
install -m 0755 -o root -g wheel "$HERE/filters/pdftoricoh" "$DST/pdftoricoh"

echo "==> Creating signed, .app-suffix-free copy of the vendor filter"
cp "$VENDOR_APP" "$DST/RICOH_SP_150Filter"
chown root:wheel "$DST/RICOH_SP_150Filter" "$DST/pdftoraster_cg"
chmod 0755 "$DST/RICOH_SP_150Filter" "$DST/pdftoraster_cg"

echo "==> Ad-hoc signing the Mach-O binaries (required on Apple Silicon)"
codesign -s - --force "$DST/pdftoraster_cg"
codesign -s - --force "$DST/RICOH_SP_150Filter"

echo "==> Pointing the PPD at pdftoricoh"
/usr/bin/sed -i '' 's#^\*cupsFilter:.*#*cupsFilter: "application/pdf 0 pdftoricoh"#' "$PPD"

echo "==> Reloading CUPS"
launchctl kickstart -k system/org.cups.cupsd 2>/dev/null || killall -HUP cupsd 2>/dev/null || true

echo
echo "Done. Test it by printing any PDF, for example:"
echo "  lp -d $PRINTER -o media=A4 /path/to/some.pdf"
echo "  (or just print from any app via the normal print dialog)"
