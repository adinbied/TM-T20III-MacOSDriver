#!/usr/bin/env python3
"""Draw the TM-T20III Utility app icon (receipt printer with paper coming out).

    python3 make_icon.py            -> AppIcon.png (1024x1024) + AppIcon.icns

Needs Pillow + numpy. The .icns is written directly (PNG-based entries), so no
macOS tools are needed; both outputs are committed so the Mac build does not
need Python.
"""
import io
import os
import struct

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

S = 4                      # supersampling factor
W = 1024 * S
here = os.path.dirname(os.path.abspath(__file__))


def px(v):
    return int(round(v * S))


def rr(draw, box, r, **kw):
    draw.rounded_rectangle([px(b) for b in box], radius=px(r), **kw)


def vgradient(size, top, bottom):
    h, w = size[1], size[0]
    t = np.linspace(0.0, 1.0, h)[:, None, None]
    a = np.array(top, float)[None, None, :]
    b = np.array(bottom, float)[None, None, :]
    rows = (a + (b - a) * t).astype(np.uint8)
    return Image.fromarray(np.repeat(rows, w, axis=1), "RGBA")


def mask_rr(box, r):
    m = Image.new("L", (W, W), 0)
    rr(ImageDraw.Draw(m), box, r, fill=255)
    return m


def paste_gradient(img, box, r, top, bottom):
    g = vgradient((W, W), top, bottom)
    img.paste(g, (0, 0), mask_rr(box, r))


def shadow(img, box, r, offset, blur, alpha):
    m = Image.new("L", (W, W), 0)
    b = (box[0], box[1] + offset, box[2], box[3] + offset)
    rr(ImageDraw.Draw(m), b, r, fill=alpha)
    m = m.filter(ImageFilter.GaussianBlur(px(blur)))
    black = Image.new("RGBA", (W, W), (0, 0, 0, 255))
    img.paste(black, (0, 0), m)


img = Image.new("RGBA", (W, W), (0, 0, 0, 0))

# --- background tile (macOS icon grid: 824 pt body, ~185 pt corner) --------
tile = (100, 100, 924, 924)
shadow(img, tile, 185, 10, 14, 90)
paste_gradient(img, tile, 185, (92, 170, 255, 255), (28, 92, 200, 255))
# soft highlight on the upper half
hl = Image.new("L", (W, W), 0)
rr(ImageDraw.Draw(hl), (100, 100, 924, 520), 185, fill=40)
hl = hl.filter(ImageFilter.GaussianBlur(px(30)))
img.paste(Image.new("RGBA", (W, W), (255, 255, 255, 255)), (0, 0),
          Image.fromarray(np.minimum(np.array(hl), np.array(mask_rr(tile, 185))).astype(np.uint8)))

# --- printer body --------------------------------------------------------
body = (214, 540, 810, 852)
shadow(img, (230, 560, 794, 872), 70, 14, 22, 150)
paste_gradient(img, body, 70, (78, 82, 90, 255), (30, 32, 37, 255))
d = ImageDraw.Draw(img)
# top cover (slightly lighter) and its front edge
paste_gradient(img, (214, 540, 810, 660), 70, (98, 103, 112, 255), (62, 66, 74, 255))
d.rectangle([px(214), px(620), px(810), px(660)], fill=(62, 66, 74, 255))
d.line([px(250), px(660), px(774), px(660)], fill=(20, 21, 25, 255), width=px(5))
d.line([px(250), px(664), px(774), px(664)], fill=(110, 115, 125, 255), width=px(2))
# paper exit slot
rr(d, (282, 572, 742, 596), 12, fill=(14, 15, 18, 255))
# front panel: LED + feed button
d.ellipse([px(300), px(752), px(330), px(782)], fill=(70, 230, 120, 255))
glow = Image.new("L", (W, W), 0)
ImageDraw.Draw(glow).ellipse([px(290), px(742), px(340), px(792)], fill=120)
glow = glow.filter(ImageFilter.GaussianBlur(px(10)))
img.paste(Image.new("RGBA", (W, W), (90, 255, 140, 255)), (0, 0), glow)
d = ImageDraw.Draw(img)
d.ellipse([px(300), px(752), px(330), px(782)], fill=(80, 240, 130, 255))
d.ellipse([px(308), px(758), px(318), px(768)], fill=(200, 255, 215, 255))
rr(d, (660, 748, 744, 786), 19, fill=(48, 51, 58, 255), outline=(18, 19, 22, 255), width=px(3))
rr(d, (664, 750, 740, 774), 15, fill=(70, 74, 83, 255))

# --- receipt paper -------------------------------------------------------
L, R, TOP, BOT = 318, 706, 128, 586          # paper enters the slot at y~584
teeth = 14
tw = (R - L) / teeth
poly = [(L, BOT), (L, TOP + 16)]
for i in range(teeth):
    x0 = L + i * tw
    poly += [(x0 + tw / 2, TOP), (x0 + tw, TOP + 16)]
poly += [(R, BOT)]
# shadow behind paper
sm = Image.new("L", (W, W), 0)
ImageDraw.Draw(sm).polygon([(px(x + 6), px(y + 10)) for x, y in poly], fill=110)
sm = sm.filter(ImageFilter.GaussianBlur(px(14)))
img.paste(Image.new("RGBA", (W, W), (0, 0, 0, 255)), (0, 0), sm)
# paper with a slight top-to-bottom shade
pm = Image.new("L", (W, W), 0)
ImageDraw.Draw(pm).polygon([(px(x), px(y)) for x, y in poly], fill=255)
img.paste(vgradient((W, W), (255, 255, 255, 255), (226, 231, 238, 255)), (0, 0), pm)
d = ImageDraw.Draw(img)

# printed content
ink = (60, 66, 78, 255)
light = (150, 158, 170, 255)
cx = (L + R) / 2
rr(d, (cx - 92, 186, cx + 92, 210), 6, fill=ink)              # store name
rr(d, (cx - 60, 222, cx + 60, 236), 5, fill=light)           # address
y = 268
for wl, wr in ((150, 60), (120, 60), (176, 60), (100, 60)):  # item lines
    rr(d, (L + 40, y, L + 40 + wl, y + 14), 5, fill=light)
    rr(d, (R - 40 - wr, y, R - 40, y + 14), 5, fill=light)
    y += 36
# dashed rule
x = L + 40
while x < R - 40:
    d.rectangle([px(x), px(y + 4), px(min(x + 16, R - 40)), px(y + 8)], fill=light)
    x += 26
y += 32
rr(d, (L + 40, y, L + 150, y + 22), 6, fill=ink)              # TOTAL
rr(d, (R - 130, y, R - 40, y + 22), 6, fill=ink)
y += 54
# barcode
x = cx - 110
widths = [4, 2, 2, 6, 2, 4, 2, 2, 4, 6, 2, 2, 4, 2, 6, 2, 4, 2, 2, 4, 2, 6, 4, 2, 2, 4]
for i, bw in enumerate(widths):
    if i % 2 == 0:
        d.rectangle([px(x), px(y), px(x + bw), px(y + 46)], fill=ink)
    x += bw + 3
    if x > cx + 110:
        break
# paper darkens where it enters the slot
fade = vgradient((W, px(40)), (0, 0, 0, 0), (0, 0, 0, 90))
fm = Image.new("L", (W, px(40)), 0)
ImageDraw.Draw(fm).rectangle([px(L), 0, px(R), px(40)], fill=255)
img.paste(fade, (0, px(BOT - 40)), Image.fromarray(np.minimum(np.array(fade)[:, :, 3], np.array(fm))))
# slot lip over the paper's bottom edge
d = ImageDraw.Draw(img)
d.rectangle([px(282), px(586), px(742), px(596)], fill=(14, 15, 18, 255))
d.line([px(296), px(597), px(728), px(597)], fill=(120, 126, 136, 255), width=px(2))

out = img.resize((1024, 1024), Image.LANCZOS)
out.save(os.path.join(here, "AppIcon.png"))

# --- .icns (PNG payloads) -------------------------------------------------
entries = [(b"icp4", 16), (b"icp5", 32), (b"ic11", 32), (b"ic07", 128), (b"ic12", 64),
           (b"ic08", 256), (b"ic13", 256), (b"ic09", 512), (b"ic14", 512), (b"ic10", 1024)]
chunks = b""
for tag, size in entries:
    buf = io.BytesIO()
    out.resize((size, size), Image.LANCZOS).save(buf, "PNG")
    data = buf.getvalue()
    chunks += tag + struct.pack(">I", len(data) + 8) + data
with open(os.path.join(here, "AppIcon.icns"), "wb") as f:
    f.write(b"icns" + struct.pack(">I", len(chunks) + 8) + chunks)
print("wrote AppIcon.png and AppIcon.icns")
