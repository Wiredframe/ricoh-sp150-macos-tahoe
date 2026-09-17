#!/bin/bash
# uninstall.sh - remove the native RICOH SP 150 filter chain and restore the PPD.
#
# Usage:  ./uninstall.sh [PRINTER_QUEUE_NAME]        (default: RICOH_SP_150)

set -euo pipefail

PRINTER="${1:-RICOH_SP_150}"
DST="/usr/libexec/cups/filter"
PPD="/etc/cups/ppd/${PRINTER}.ppd"

if [ "$(id -u)" -ne 0 ]; then
  echo "Elevating with sudo..."
  exec sudo "$0" "$@"
fi

if [ -f "${PPD}.orig" ]; then
  echo "==> Restoring original PPD"
  cp -p "${PPD}.orig" "$PPD"
  rm -f "${PPD}.orig"
fi

echo "==> Removing filters"
rm -f "$DST/pdftoricoh" "$DST/pdftoraster_cg" "$DST/rastertolhpl" "$DST/RICOH_SP_150Filter"

echo "==> Reloading CUPS"
launchctl kickstart -k system/org.cups.cupsd 2>/dev/null || killall -HUP cupsd 2>/dev/null || true

echo "Done. Ricoh's own RICOH_SP_150Filter.app (if installed) was left untouched."
