#!/bin/sh
# Add the TM-T20III as a CUPS queue using the installed PPD.
# Usage: ./scripts/add-printer.sh [queue-name] [device-uri]
set -e
NAME="${1:-TM_T20III}"
URI="${2:-}"
PPD=/Library/Printers/PPDs/Contents/Resources/EpsonTMT20III.ppd

if [ -z "$URI" ]; then
    URI=$(lpinfo -v 2>/dev/null | awk '/usb:\/\/EPSON\/TM-T20/ {print $2; exit}')
fi
if [ -z "$URI" ]; then
    echo "No TM-T20 found on USB. Connected devices:" >&2
    lpinfo -v >&2
    echo "Pass the URI as the 2nd argument (e.g. socket://192.168.1.50:9100)." >&2
    echo "If the printer is in USB *Vendor Class* mode it will not appear here;" >&2
    echo "switch it to USB Printer Class in the TM-T20III utility." >&2
    exit 1
fi

# All other options start at the PPD defaults (Paper Source None, Document[Feed, Cut]);
# change them in TM-T20III Utility > Printing Preferences.
lpadmin -p "$NAME" -E -v "$URI" -P "$PPD" -o printer-is-shared=false -o PageSize=RP80x297
echo "Added queue '$NAME' -> $URI"
echo "Truss tapes:  lp -d $NAME -o PageSize=TrussTape file.pdf"
