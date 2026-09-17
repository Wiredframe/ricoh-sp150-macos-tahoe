#!/bin/bash
# remove-ricoh-driver.sh - remove everything Ricoh's SP 150 driver package
# installed. Only makes sense after ./install.sh: the native chain does not
# need any of it. Switches the queue to the bundled PPD first so that nothing
# references Ricoh's files anymore.
#
# Removes:
#   /Library/Printers/RICOH                       (StatusService, TonerSupplyTool, dylib, icon)
#   /Library/Printers/PPDs/Contents/Resources/RICOH SP 150*.ppd.gz
#   /usr/libexec/cups/filter/RICOH_SP_150Filter.app
#   /usr/libexec/cups/filter/usbtonerlevel        (Ricoh's toner level helper)
#   /Applications/RICOH                           (RICOH Printer.app)
#   /etc/cups/ppd/<queue>.ppd.orig, .ppd.O        (backups of Ricoh's PPD)
#   the four com.RICOH.ricohSp150.* package receipts
#
# Usage:  ./remove-ricoh-driver.sh [PRINTER_QUEUE_NAME]     (default: RICOH_SP_150)

set -euo pipefail

PRINTER="${1:-RICOH_SP_150}"
HERE="$(cd "$(dirname "$0")" && pwd)"
DST="/usr/libexec/cups/filter"

if [ ! -x "$DST/rastertolhpl" ] || [ ! -x "$DST/pdftoricoh" ]; then
  echo "ERROR: native filters not installed. Run ./install.sh first."
  exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
  echo "Elevating with sudo..."
  exec sudo "$0" "$@"
fi

if lpstat -p "$PRINTER" >/dev/null 2>&1; then
  echo "==> Switching queue '$PRINTER' to the bundled PPD"
  lpadmin -p "$PRINTER" -P "$HERE/ppd/RICOH_SP_150.ppd"
fi

echo "==> Removing Ricoh driver files"
rm -rf "/Library/Printers/RICOH"
rm -f  "/Library/Printers/PPDs/Contents/Resources/RICOH SP 150.ppd.gz" \
       "/Library/Printers/PPDs/Contents/Resources/RICOH SP 150w.ppd.gz"
rm -f  "$DST/RICOH_SP_150Filter.app" "$DST/RICOH_SP_150Filter" "$DST/usbtonerlevel"
rm -rf "/Applications/RICOH"
rm -f  "/etc/cups/ppd/${PRINTER}.ppd.orig" "/etc/cups/ppd/${PRINTER}.ppd.O"

echo "==> Forgetting package receipts"
for p in $(pkgutil --pkgs | grep -E '^com\.RICOH\.ricohSp150\.' || true); do
  pkgutil --forget "$p" >/dev/null && echo "    $p"
done

echo "==> Reloading CUPS"
launchctl kickstart -k system/org.cups.cupsd 2>/dev/null || killall -HUP cupsd 2>/dev/null || true

echo
echo "Done. Ricoh's driver is gone; printing runs on the native chain only."
