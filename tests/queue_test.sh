#!/bin/sh
# Printing Preferences end to end: tmqueue writes the queue PPD, CUPS accepts
# it, and the filter honours it.  Run from macos/: sh tests/queue_test.sh
# (needs build/rastertotmt20, build/mkraster, utility/build/tmqueue).
set -e
Q=utility/build/tmqueue
T=ppd/EpsonTMT20III.ppd
D=$(mktemp -d)

$Q -t $T -o $D/q1.ppd set $T TmFeedCut=Detail tm-cut-pageend=1 \
    'tm-cmd-docbegin="HI" 0A' tm-dk-docend=2 TmPaperSource=Bottom
$Q -t $T -o $D/q2.ppd paper-add $D/q1.ppd "TRUSS TAPE" 3.18 12 --inch
$Q -t $T -o $D/q3.ppd set $D/q2.ppd PageSize=UDP1
$Q show $D/q3.ppd | grep -E '\*$|^paper'

if command -v cupstestppd >/dev/null; then
    # the only expected failure off-Mac is the missing filter path
    cupstestppd -W translations $D/q3.ppd | grep -E 'FAIL|ERROR' | grep -v 'cupsFilter\|: FAIL' && exit 1
    echo "cupstestppd: ok"
fi

build/mkraster $D/t.ras 228.96 1728 0 228.96 2 >/dev/null      # 2 pages of 24 in
PPD=$D/q3.ppd build/rastertotmt20 1 u t 1 "" $D/t.ras > $D/t.bin 2> $D/err
grep 'DEBUG: head' $D/err
python3 tests/escpos_decode.py $D/t.bin > $D/dec.txt
cat $D/dec.txt | grep -E 'CUT|DRAWER|TEXT|rows'
[ "$(grep -c 'CUT GS V 00' $D/dec.txt)" = 2 ]           # Custom: cut after each page
grep -q "DRAWER pin5" $D/dec.txt                          # tm-dk-docend=2
grep -q "TEXT 'HI'" $D/dec.txt                            # Send Command
grep -q "trim=2" $D/err                                   # Paper Source = Bottom
rm -rf $D
echo "QUEUE TEST OK"
