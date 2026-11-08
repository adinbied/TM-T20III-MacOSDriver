#!/usr/bin/env python3
"""End-to-end test of the real CUPS filter (needs libcups; runs on macOS or Linux).

Builds raster jobs with build/mkraster, runs build/rastertotmt20 the way cupsd
does (PPD in $PPD, options in argv[5]), decodes the ESC/POS output back into
paper with escpos_decode.Decoder and compares it with the expected image.

    python3 tests/filter_test.py          (make filter-test)
"""
import math
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from escpos_decode import Decoder  # noqa: E402

FILTER = os.path.join(ROOT, "build", "rastertotmt20")
MKRASTER = os.path.join(ROOT, "build", "mkraster")
BASE_PPD = os.path.join(ROOT, "ppd", "EpsonTMT20III.ppd")
TMP = tempfile.mkdtemp(prefix="tmt20-")
DPI = 203


def lround(v):
    return int(math.floor(v + 0.5))


def ink(x, y, w, h):
    if x in (0, w - 1) or y in (0, h - 1):
        return True
    if 20 <= x < 36 and 10 <= y < 26:
        return True
    return y % 100 == 50


def run(page, options="", ppd=BASE_PPD, pages=1):
    """page = (w_pt, h_pt, llx, urx).  Returns (decoder, (src_w, src_h), stderr)."""
    ras = os.path.join(TMP, "in.ras")
    out = subprocess.run([MKRASTER, ras, *map(str, page), str(pages)], check=True,
                         capture_output=True, text=True).stdout.split()
    sw, sh = int(out[0]), int(out[1])
    env = dict(os.environ, PPD=ppd)
    p = subprocess.run([FILTER, "1", "user", "title", "1", options, ras], env=env,
                       capture_output=True)
    err = p.stderr.decode()
    if p.returncode != 0:
        raise AssertionError(f"filter failed ({p.returncode}):\n{err}")
    dec = Decoder(p.stdout).run()
    assert dec.stats["unknown"] == 0, dec.events
    return dec, (sw, sh), err


def expected(src, page, head=576, left=0, scale=1.0, upside=False):
    """Paper image the filter should produce for one page (list of bit lists)."""
    sw, sh = src
    w_pt, _, llx, urx = page
    xoff = lround(llx * DPI / 72)
    lim = xoff + lround((urx - llx) * DPI / 72)
    xmap = []
    for x in range(head):
        sx = -1
        if x >= left:
            sx = xoff + math.floor((x - left) / scale)
            if sx >= lim or sx >= sw:
                sx = -1
        xmap.append(sx)
    rows = [[1 if sx >= 0 and ink(sx, y, sw, sh) else 0 for sx in xmap] for y in range(sh)]
    if upside:
        rows = [list(reversed(r)) for r in reversed(rows)]
    out, done = [], 0
    for i, r in enumerate(rows):                 # encoder's vertical resample
        target = lround((i + 1) * scale)
        out.extend([r] * (target - done))
        done = target
    return out


def bits(row_bytes, width):
    return [(row_bytes[x >> 3] >> (7 - (x & 7))) & 1 for x in range(width)]


def check_image(dec, exp, label, start=0):
    width = len(exp[0])
    assert dec.width == width, f"{label}: width {dec.width} != {width}"
    got = [bits(r, width) for r in dec.rows[start:start + len(exp)]]
    assert len(got) == len(exp), f"{label}: {len(got)} rows, expected {len(exp)}"
    bad = sum(1 for a, b in zip(got, exp) if a != b)
    assert bad == 0, f"{label}: {bad} rows differ"


def cuts(dec):
    return [m for _, _, m in dec.events if m.startswith("CUT")]


def events(dec, prefix):
    return [(row, m) for _, row, m in dec.events if m.startswith(prefix)]


def ppd_with(lines, defaults=None):
    """Queue PPD as TMUtility.app writes it: base + *TmSetting lines + defaults."""
    text = open(BASE_PPD, encoding="latin-1").read()
    text = text.replace("*%@@TMSETTINGS", "\n".join(lines))
    for k, v in (defaults or {}).items():
        text, n = re.subn(rf"^\*Default{k}: .*$", f"*Default{k}: {v}", text, flags=re.M)
        assert n == 1, k
    path = os.path.join(TMP, f"q{abs(hash(text))}.ppd")
    open(path, "w", encoding="latin-1").write(text)
    return path


RP80 = (226.77, 841.89, 10.80, 215.13)
RP58 = (164.41, 841.89, 7.09, 156.06)
A4 = (595.28, 841.89, 0, 595.28)
TRUSS = (228.96, 864, 0, 228.96)


def main():
    if not (os.path.exists(FILTER) and os.path.exists(MKRASTER)):
        sys.exit("build/rastertotmt20 and build/mkraster required (make filter-test)")

    # 1. Defaults: 80 mm roll, Document[Feed, Cut], exact length, pixel exact.
    dec, src, _ = run(RP80)
    check_image(dec, expected(src, RP80), "default")
    assert len(dec.rows) == src[1] and cuts(dec) == ["CUT GS V 41 (feed to cutter)"]
    print("default 80 mm page: ok")

    # 2. Truss tape page: origin at the paper edge, right 8.8 mm cut off, not scaled.
    dec, src, _ = run(TRUSS)
    check_image(dec, expected(src, TRUSS), "truss")
    assert len(dec.rows) == src[1] == 2436                  # 12.00 in
    print("truss tape page: ok")

    # 3. Upside-down.
    dec, src, _ = run(RP80, "TmUpsideDown=True")
    check_image(dec, expected(src, RP80, upside=True), "upside-down")
    print("upside-down: ok")

    # 4. Left margin 5 mm (APD Utility Layout), top margin 3 mm.
    ppd = ppd_with(['*TmSetting tm-margin-left-enable: "1"', '*TmSetting tm-margin-left: "5000"',
                    '*TmSetting tm-margin-top-enable: "1"', '*TmSetting tm-margin-top: "3000"'])
    dec, src, _ = run(RP80, ppd=ppd)
    top = lround(3000 * 406 / 25400) // 2                      # 48 units = 24 rows
    check_image(dec, expected(src, RP80, left=40), "margins", start=top)
    assert len(dec.rows) == src[1] + top
    print("margins: ok")

    # 5. A4 shrinks to the 80 mm head ("Unsupported Paper: automatically adjust").
    dec, src, _ = run(A4)
    s = 576 / src[0]
    check_image(dec, expected(src, A4, scale=s), "A4 auto-fit")
    dec2, _, _ = run(A4, "tm-unsupported-auto=0")
    assert len(dec2.rows) == src[1]                            # cropped, not scaled
    print(f"A4 auto-fit (scale {s:.3f}): ok")

    # 6. 58 mm roll paper width, and Reduce 80 -> 58 mm, and 70 % reduced print.
    dec, src, _ = run(RP58, "TmPaperWidth=58mm")
    check_image(dec, expected(src, RP58, head=420), "58 mm")
    dec, src, _ = run(RP80, "TmPaperWidth=58mm tm-reduce-80-58=1")
    check_image(dec, expected(src, RP80, head=420, scale=420 / 576), "80->58")
    dec, src, _ = run(RP80, "TmScaling=70")
    check_image(dec, expected(src, RP80, scale=0.7), "70 %")
    print("paper width / reduction: ok")

    # 7. Feed and Cut Custom from Printing Preferences, overridden per job.
    ppd = ppd_with(['*TmSetting tm-cut-pageend: "1"', '*TmSetting tm-cut-docend: "17"',
                    '*TmSetting tm-feed-docend: "10000"',
                    '*TmSetting tm-dk-docend: "1"', '*TmSetting tm-dk-pagebegin: "513"',
                    '*TmSetting tm-dk-count-pagebegin: "2"',
                    '*TmSetting tm-cmd-docbegin: "48454C4C4F 0A"'],
                   {"TmFeedCut": "Detail"})
    dec, src, _ = run(RP80, ppd=ppd, pages=2)
    assert cuts(dec) == ["CUT GS V 00", "CUT GS V 00", "CUT GS V 41 (feed to cutter)"], cuts(dec)
    assert len(events(dec, "DRAWER pin2")) == 1
    assert len(events(dec, "ESC ( A 61 02 02")) == 2           # buzzer B x2, each page
    assert events(dec, "TEXT 'HELLO'")
    assert len(dec.rows) == 2 * src[1] + 80                    # + 10 mm feed (80 rows)
    dec, _, _ = run(RP80, "TmFeedCut=DocNoFeedNoCut", ppd=ppd, pages=2)
    assert cuts(dec) == []
    dec, _, _ = run(RP80, "TmPeripherals=None", ppd=ppd)
    assert not events(dec, "DRAWER") and not events(dec, "ESC ( A")
    print("feed and cut / peripherals / send command: ok")

    # 8. Paper Source = Bottom (Epson conservation) drops trailing blank feed.
    ppd = ppd_with([], {"TmPaperSource": "Bottom"})
    dec, src, err = run(RP80, ppd=ppd)
    assert "trim=2" in err
    print("paper source: ok")
    print("ALL FILTER TESTS OK")


if __name__ == "__main__":
    main()
