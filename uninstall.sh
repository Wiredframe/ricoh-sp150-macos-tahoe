#!/bin/bash
# uninstall.sh - revert the RICOH SP 150 filter-chain fix.
#
# Restores the original PPD and removes the files this project installed.
# It does NOT touch Ricoh's own RICOH_SP_150Filter.app.
#
# Usage:  ./uninstall.sh [PRINTER_QUEUE_NAME]      (default: RICOH_SP_150)

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
else
  echo "==> No ${PPD}.orig backup found, leaving PPD as is"
fi

echo "==> Removing installed filters"
rm -f "$DST/pdftoraster_cg" "$DST/pdftoricoh" "$DST/RICOH_SP_150Filter"

echo "==> Reloading CUPS"
launchctl kickstart -k system/org.cups.cupsd 2>/dev/null || killall -HUP cupsd 2>/dev/null || true

echo "Done."
