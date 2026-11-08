#!/bin/sh
# Remove the TM-T20III driver + utility installed by the .pkg (or by make install).
#   sudo /Library/Printers/Epson/TMT20III/uninstall.sh [--remove-queues]
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"

queues=""
for q in $(lpstat -e 2>/dev/null); do
    f="/private/etc/cups/ppd/$q.ppd"
    [ -f "$f" ] && grep -q rastertotmt20 "$f" && queues="$queues $q"
done
if [ "$1" = "--remove-queues" ]; then
    for q in $queues; do lpadmin -x "$q" && echo "Removed queue $q"; done
elif [ -n "$queues" ]; then
    echo "Note: these queues still use the driver and will stop printing:$queues"
    echo "      (re-run with --remove-queues to delete them)"
fi

rm -rf "/Applications/TM-T20III Utility.app" \
       /usr/local/bin/tmutil /usr/local/bin/tmqueue \
       /Library/Printers/PPDs/Contents/Resources/EpsonTMT20III.ppd \
       /Library/Printers/Epson/TMT20III
rmdir /Library/Printers/Epson 2>/dev/null
pkgutil --pkgs 2>/dev/null | grep -i 'tmt20iii' | while read -r id; do pkgutil --forget "$id" >/dev/null; done
echo "TM-T20III driver removed."
