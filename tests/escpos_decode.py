#!/usr/bin/env python3
"""Decode a TM-T20III ESC/POS stream back into the paper it would produce.

Works on output of rastertotmt20 *and* on captures of Epson's Windows driver
(print to a FILE: port), so the two can be compared byte-for-byte and
row-for-row.

    escpos_decode.py stream.bin [--pbm out.pbm] [--units-per-row 2]
                                [--expect-rows N] [-v]
"""
import argparse
import sys
import zlib


class Decoder:
    def __init__(self, data, units_per_row=2, verbose=False):
        self.d = data
        self.i = 0
        self.upr = units_per_row
        self.v = verbose
        self.width = None
        self.rows = []            # paper rows (bytes)
        self.stored = []          # rows held by GS 8 L until GS ( L fn50
        self.feed_units = 0       # ESC J units not yet converted to rows
        self.events = []
        self.stats = dict(bands=0, zbands=0, rawbands=0, feeds=0, cuts=0,
                          realtime_in_raw=0, unknown=0)

    # -- helpers -----------------------------------------------------------
    def need(self, n):
        if self.i + n > len(self.d):
            raise ValueError(f"truncated command at 0x{self.i:x}")
        return self.d[self.i:self.i + n]

    def log(self, msg):
        self.events.append((self.i, len(self.rows), msg))
        if self.v:
            print(f"0x{self.i:06x} row {len(self.rows):6d}: {msg}")

    def flush_feed(self):
        if self.feed_units:
            n, rem = divmod(self.feed_units, self.upr)
            blank = bytes(((self.width or 576) + 7) // 8)
            self.rows.extend([blank] * n)
            self.feed_units = rem

    # -- commands ----------------------------------------------------------
    def run(self):
        d = self.d
        while self.i < len(d):
            b = d[self.i]
            if b == 0x1B:
                self.esc()
            elif b == 0x1D:
                self.gs()
            elif b == 0x10:
                self.log(f"DLE {d[self.i + 1]:02x} (real-time command!)")
                self.i += 3
            elif b == 0x0A:
                self.log("LF")
                self.i += 1
            elif 0x20 <= b < 0x7F:                      # text (Send Command)
                j = self.i
                while j < len(d) and 0x20 <= d[j] < 0x7F:
                    j += 1
                self.log(f"TEXT {d[self.i:j].decode()!r}")
                self.i = j
            else:
                self.stats["unknown"] += 1
                self.log(f"unknown byte {b:02x}")
                self.i += 1
        self.flush_feed()
        return self

    def esc(self):
        c = self.need(2)[1]
        simple = {0x3D: 3, 0x52: 3, 0x74: 3, 0x32: 2, 0x40: 2, 0x55: 3, 0x72: 3}
        if c == 0x4A:                                   # ESC J n
            n = self.need(3)[2]
            self.feed_units += n
            self.stats["feeds"] += 1
            self.i += 3
        elif c == 0x63:                                 # ESC c x n
            self.log(f"ESC c {self.need(4)[2]:02x} {self.d[self.i + 3]:02x}")
            self.i += 4
        elif c == 0x70:                                 # ESC p m t1 t2
            p = self.need(5)
            self.log(f"DRAWER pin{'2' if p[2] in (0, 48) else '5'} on={p[3]*2}ms off={p[4]*2}ms")
            self.i += 5
        elif c == 0x28:                                 # ESC ( x pL pH ...
            p = self.need(5)
            ln = p[3] | p[4] << 8
            body = self.need(5 + ln)[5:]
            self.log(f"ESC ( {chr(p[2])} {body.hex(' ')}")
            self.i += 5 + ln
        elif c in simple:
            self.i += simple[c]
        else:
            self.stats["unknown"] += 1
            self.log(f"unknown ESC {c:02x}")
            self.i += 2

    def gs(self):
        c = self.need(2)[1]
        if c == 0x38 and self.need(3)[2] == 0x4C:       # GS 8 L
            self.gs8l()
        elif c == 0x28:                                 # GS ( x pL pH
            p = self.need(5)
            ln = p[3] | p[4] << 8
            body = self.need(5 + ln)[5:]
            if p[2] == 0x4C and body[:2] == b"\x30\x32":  # GS ( L fn50 print
                self.flush_feed()
                self.rows.extend(self.stored)
                self.stored = []
            else:
                self.log(f"GS ( {chr(p[2])} {body.hex(' ')}")
            self.i += 5 + ln
        elif c == 0x56:                                 # GS V
            m = self.need(3)[2]
            n = 4 if m in (0x41, 0x42, 0x61, 0x62, 0x67, 0x68) else 3
            self.flush_feed()
            self.stats["cuts"] += 1
            self.log(f"CUT GS V {m:02x}" + (" (feed to cutter)" if n == 4 else ""))
            self.i += n
        elif c == 0x62:                                 # GS b n
            self.i += 3
        elif c == 0x50:                                 # GS P x y
            self.log(f"GS P {self.need(4)[2]} {self.d[self.i + 3]}")
            self.i += 4
        else:
            self.stats["unknown"] += 1
            self.log(f"unknown GS {c:02x}")
            self.i += 2

    def gs8l(self):
        h = self.need(17)
        p = int.from_bytes(h[3:7], "little")
        m, fn, a, bx, by, col = h[7:13]
        x = h[13] | h[14] << 8
        y = h[15] | h[16] << 8
        data = self.need(7 + p)[17:]
        assert m == 0x30 and fn == 0x70, f"GS 8 L fn {fn:#x} not raster store"
        assert bx == 1 and by == 1 and col == 0x31, "unexpected scale/colour"
        rb = (x + 7) // 8
        if a in (0x31, 0x35):
            raw = zlib.decompress(data)
            self.stats["zbands"] += 1
        elif a in (0x30, 0x34):
            raw = data
            self.stats["rawbands"] += 1
            for k in range(len(raw) - 2):   # driver leaves the last 2 bytes
                if raw[k] == 0x10 and raw[k + 1] in (0x04, 0x05, 0x14):
                    self.stats["realtime_in_raw"] += 1
        else:
            raise ValueError(f"unknown graphics a={a:#x}")
        if len(raw) != rb * y:
            raise ValueError(f"band size {len(raw)} != {rb}*{y}")
        if self.width is None:
            self.width = x
        elif self.width != x:
            self.log(f"width change {self.width} -> {x}")
        self.stored.extend(raw[r * rb:(r + 1) * rb] for r in range(y))
        self.stats["bands"] += 1
        self.i += 7 + p


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stream")
    ap.add_argument("--pbm", help="write reconstructed paper as PBM image")
    ap.add_argument("--units-per-row", type=int, default=2,
                    help="ESC J units per 203-dpi row (TM-T20III: 2)")
    ap.add_argument("--expect-rows", type=int)
    ap.add_argument("--compare", help="expected PBM; report differing rows/bytes")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    dec = Decoder(open(a.stream, "rb").read(), a.units_per_row, a.verbose).run()
    w = dec.width or 576
    n = len(dec.rows)
    print(f"width {w} dots, {n} rows = {n * 25.4 / 203:.1f} mm "
          f"({n / 203:.3f} in) of paper before final cut feed")
    print("stats:", ", ".join(f"{k}={v}" for k, v in dec.stats.items()))
    for off, row, msg in dec.events:
        if not a.verbose and not msg.startswith(("unknown", "DLE")):
            print(f"  @row {row}: {msg}")
    if a.pbm:
        with open(a.pbm, "wb") as f:
            f.write(b"P4\n%d %d\n" % (w, n))
            for r in dec.rows:
                f.write(r)
        print("wrote", a.pbm)
    bad = dec.stats["unknown"] or dec.stats["realtime_in_raw"]
    if a.compare:
        exp = open(a.compare, "rb").read()
        hdr_end = exp.index(b"\n", exp.index(b"\n") + 1) + 1
        ew, eh = map(int, exp[3:hdr_end].split())
        erb = (ew + 7) // 8
        erows = [exp[hdr_end + r * erb: hdr_end + (r + 1) * erb] for r in range(eh)]
        diff_rows = diff_bytes = sanitised = 0
        for got, want in zip(dec.rows, erows):
            if got != want:
                diff_rows += 1
                for k, (g, w_) in enumerate(zip(got, want)):
                    if g != w_:
                        diff_bytes += 1
                        # only allowed difference: DLE/ESC lead byte -> 0x30
                        if g == 0x30 and w_ in (0x10, 0x1B):
                            sanitised += 1
        print(f"compare: {len(erows)} expected rows, {diff_rows} rows differ, "
              f"{diff_bytes} bytes differ ({sanitised} are real-time sanitising)")
        if len(erows) != len(dec.rows) or diff_bytes != sanitised:
            print("FAIL: image mismatch")
            return 1
    if a.expect_rows is not None and n != a.expect_rows:
        print(f"FAIL: expected {a.expect_rows} rows, got {n}")
        return 1
    if bad:
        print("FAIL: unknown bytes or unsanitised real-time commands")
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
