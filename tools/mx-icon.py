#!/usr/bin/env python3
"""Memory card icons for the mx OS folders (MXBOOT, MXOS), in the Penumbra look.

Writes a PS2 browser icon (.icn) and its icon.sys: a bevelled "MX" in 3D on a
plinth whose front carries a word (BOOT, OS), lit by one warm lamp.

  tools/mx-icon.py --font ../mxshell/font/src/InterTight-Variable.ttf --word OS \
      --word2 KERNEL --title "MX OS" --title2 kernel --icn MXOS.ICN --out icon
  tools/mx-icon.py --font ../mxshell/font/src/InterTight-Variable.ttf --word BOOT \
      --word2 MAIN --title "MX BOOT" --title2 bootloader --icn MXBOOT.ICN \
      --out ../mxboot/icon/MXBOOT
  (and for each update folder, e.g. --word2 EUROPE --title2 "update Europe"
   --out ../mxboot/icon/BEEXEC-SYSTEM)
  tools/mx-icon.py --preview icon/MXOS.ICN icon/icon.sys out.png

The font is the shell's (apps/mxshell/font/src/InterTight-Variable.ttf).

Formats (SDK docs: "PlayStation 2 File Format: icon.sys"; the .icn layout as
read by HDLGameInstaller's IconLoader and checked against ps2link's icon):
- .icn: u32 0x00010000; u32 shapes; u32 attrib (1 gouraud, 2 AA, 4 texture,
  8 RLE); f32 bface (1.0); u32 vertices (a multiple of 3). Per vertex and
  shape s16 x, y, z, w; then s16 normal x, y, z, w (4096 = 1.0); s16 u, v
  (4096 = 1.0); u8 r, g, b, a (0x80 = 1.0). Then u32 sequences (1); u32
  frames, f32 speed, u32 offset, u32 shapes used; per shape u32 id, u32 keys,
  keys as f32 frame, f32 weight. Then the 128x128 texture, 16-bit
  (R5G5B5, bit 15 alpha), uncompressed.
- The model: y points down and the model stands on y = 0 (up is -y); the
  viewer looks from -z; x to the right; texture row 0 is v = 0. Sizes like
  ps2link's icon: about 16000 units high, inside x and z +-10240.
- At most 600 triangles with one shape.
"""
import argparse
import math
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# Penumbra (apps/mxshell/src/pn.h)
SLAB = (0x1C, 0x1F, 0x24)
WHITE = (0xEC, 0xEA, 0xE6)
LAMP = (0xE9, 0xC9, 0x8F)

# Texture colours: the browser lights them with icon.sys's lights only
# (diffuse, no specular), so they sit above the shell's material colours.
TEX_LETTER = (0x78, 0x7C, 0x84)
TEX_SLAB = (0x3A, 0x3D, 0x44)
TEX_EDGE = LAMP  # the bevels: the lit edge
TEX_SLAB2 = (0x2C, 0x2F, 0x35)  # the lower layer: a step darker
DIM = (0xA3, 0xA6, 0xAB)  # the lower layer's word

TEX = 128
SWATCH = {"letter": (8, 104), "slab": (40, 104), "edge": (72, 104), "slab2": (104, 104)}  # 16x16 at (x, y)

# The model in shell units (cap height 1), as the mockup
CAP = 1.0
DEPTH = 0.30
BEVEL = 0.035
PLINTH_BEVEL = 0.03
GAP = 0.08
SCALE = 7800.0  # icon units per shell unit: 2.6 wide -> 20280 (+-10140)

# The plinth's layers, top first: width, height, depth, slab swatch, word colour,
# and the texture rows of the word (the strip keeps the face's proportions)
LAYERS = [
    dict(w=2.4, h=0.34, d=0.9, slab="slab", ink=WHITE, bg=TEX_SLAB, row=0, rows=17),
    dict(w=2.6, h=0.28, d=1.04, slab="slab2", ink=DIM, bg=TEX_SLAB2, row=24, rows=13),
]


def letter_m():
    return [(0, 0), (0.21, 0), (0.21, 0.64), (0.47, 0.27), (0.73, 0.64), (0.73, 0), (0.94, 0),
            (0.94, 1), (0.75, 1), (0.47, 0.58), (0.19, 1), (0, 1)]


def letter_x():
    return [(0, 0), (0.245, 0), (0.45, 0.315), (0.655, 0), (0.9, 0), (0.575, 0.5), (0.9, 1),
            (0.655, 1), (0.45, 0.685), (0.245, 1), (0, 1), (0.325, 0.5)]


def area(p):
    return 0.5 * sum(p[i][0] * p[(i + 1) % len(p)][1] - p[(i + 1) % len(p)][0] * p[i][1]
                     for i in range(len(p)))


def ccw(p):
    return p if area(p) > 0 else p[::-1]


def inset(p, d):
    """Move each edge of a CCW polygon inwards by d (mitred corners)."""
    n = len(p)
    out = []
    for i in range(n):
        a, b, c = np.array(p[i - 1]), np.array(p[i]), np.array(p[(i + 1) % n])
        e1, e2 = b - a, c - b
        n1 = np.array([-e1[1], e1[0]]) / np.linalg.norm(e1)
        n2 = np.array([-e2[1], e2[0]]) / np.linalg.norm(e2)
        m = n1 + n2
        k = 1.0 + float(n1 @ n2)
        off = m / k * d if k > 1e-3 else n1 * d
        lim = 3 * d
        if np.linalg.norm(off) > lim:
            off = off / np.linalg.norm(off) * lim
        out.append(tuple(b + off))
    return out


def ear_clip(p):
    """Triangles (index triples, CCW) of a simple CCW polygon."""
    idx = list(range(len(p)))
    tris = []

    def inside(pt, a, b, c):
        def s(p1, p2, p3):
            return (p1[0] - p3[0]) * (p2[1] - p3[1]) - (p2[0] - p3[0]) * (p1[1] - p3[1])
        d1, d2, d3 = s(pt, a, b), s(pt, b, c), s(pt, c, a)
        return not ((d1 < 0 or d2 < 0 or d3 < 0) and (d1 > 0 or d2 > 0 or d3 > 0))

    guard = 0
    while len(idx) > 3 and guard < 1000:
        guard += 1
        for k in range(len(idx)):
            i0, i1, i2 = idx[k - 1], idx[k], idx[(k + 1) % len(idx)]
            a, b, c = p[i0], p[i1], p[i2]
            if (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) <= 1e-9:
                continue
            if any(inside(p[j], a, b, c) for j in idx if j not in (i0, i1, i2)):
                continue
            tris.append((i0, i1, i2))
            idx.pop(k)
            break
    tris.append(tuple(idx))
    return tris


class Mesh:
    def __init__(self):
        self.tris = []  # (p0, p1, p2, swatch or ("label", u0, u1)), points in shell units, outward CCW

    def tri(self, a, b, c, mat):
        self.tris.append((np.array(a, float), np.array(b, float), np.array(c, float), mat))

    def quad(self, a, b, c, d, mat):
        self.tri(a, b, c, mat)
        self.tri(a, c, d, mat)


def extrude_bevel(mesh, outline, z0, z1, b, mat_face, mat_edge, mat_side):
    """A prism from z0 (back) to z1 (front) with chamfered front and back edges."""
    outer = ccw(outline)
    inner = inset(outer, b)
    n = len(outer)
    front = [(x, y, z1) for x, y in inner]
    front_o = [(x, y, z1 - b) for x, y in outer]
    back_o = [(x, y, z0 + b) for x, y in outer]
    back = [(x, y, z0) for x, y in inner]
    for t in ear_clip(inner):
        mesh.tri(front[t[0]], front[t[1]], front[t[2]], mat_face)
        mesh.tri(back[t[0]], back[t[2]], back[t[1]], mat_face)
    for i in range(n):
        j = (i + 1) % n
        mesh.quad(front_o[i], front_o[j], front[j], front[i], mat_edge)
        mesh.quad(back_o[i], back_o[j], front_o[j], front_o[i], mat_side)
        mesh.quad(back[i], back[j], back_o[j], back_o[i], mat_edge)


def plinth_layer(mesh, k, L, y1):
    """One layer of the plinth: a box from y1 - h to y1, its top edges chamfered
    (lit), the word on its front (z = +d)."""
    w, h, d, c = L["w"] / 2, L["h"], L["d"] / 2, PLINTH_BEVEL
    y0, yc = y1 - h, y1 - c
    L["top"] = y1
    tl = [(-w + c, y1, -d + c), (w - c, y1, -d + c), (w - c, y1, d - c), (-w + c, y1, d - c)]
    mesh.quad(tl[3], tl[2], tl[1], tl[0], L["slab"])
    ring = [(-w, yc, -d), (w, yc, -d), (w, yc, d), (-w, yc, d)]
    bot = [(-w, y0, -d), (w, y0, -d), (w, y0, d), (-w, y0, d)]
    for i in range(4):
        j = (i + 1) % 4
        # the corners run clockwise seen from above: j -> i keeps the faces outward
        mesh.quad(ring[j], ring[i], tl[i], tl[j], "edge")
        side_mat = ("label", k) if i == 2 else L["slab"]  # edge 2 -> 3 is the front
        mesh.quad(bot[j], bot[i], ring[i], ring[j], side_mat)
    mesh.quad(bot[0], bot[1], bot[2], bot[3], L["slab"])
    return y0


def build(layers):
    mesh = Mesh()
    z0, z1 = -DEPTH / 2, DEPTH / 2
    mx_w = 0.94 + GAP + 0.9
    x0 = -mx_w / 2
    m = [(x + x0, y) for x, y in letter_m()]
    x = [(xx + x0 + 0.94 + GAP, y) for xx, y in letter_x()]
    for outline in (m, x):
        extrude_bevel(mesh, outline, z0, z1, BEVEL, "letter", "edge", "letter")
    y = 0.0
    for k, L in enumerate(layers):
        y = plinth_layer(mesh, k, L, y)
    return mesh, -y


def draw_word(font_path, word, L):
    """The word, centred, at the size of the strip, on the layer's colour."""
    ss = 8
    rows = L["rows"]
    strip = Image.new("RGB", (TEX * ss, rows * ss), L["bg"])
    if not word:
        return strip.resize((TEX, rows), Image.LANCZOS)
    dr = ImageDraw.Draw(strip)
    size = int(17 * ss * 0.82 * L["h"] / LAYERS[0]["h"] * LAYERS[0]["w"] / L["w"])
    font = ImageFont.truetype(font_path, size)
    try:
        font.set_variation_by_axes([600])
    except Exception:
        pass
    track = int(size * 0.22)
    widths = [dr.textlength(ch, font=font) for ch in word]
    total = sum(widths) + track * (len(word) - 1)
    if total > TEX * ss * 0.92:  # a long word: squeeze the tracking, then the size
        track = max(0, int((TEX * ss * 0.92 - sum(widths)) / max(1, len(word) - 1)))
        total = sum(widths) + track * (len(word) - 1)
    bbox = dr.textbbox((0, 0), "H", font=font)
    cap_h = bbox[3] - bbox[1]
    y = (rows * ss - cap_h) / 2 - bbox[1]
    xx = (TEX * ss - total) / 2
    for ch, wd in zip(word, widths):
        dr.text((xx, y), ch, font=font, fill=L["ink"])
        xx += wd + track
    return strip.resize((TEX, rows), Image.LANCZOS)


def label_texture(font_path, words, layers):
    """The 128x128 texture: a strip per layer for its word, colour swatches below."""
    img = Image.new("RGB", (TEX, TEX), TEX_SLAB)
    for word, L in zip(words, layers):
        img.paste(draw_word(font_path, word, L), (0, L["row"]))
    for name, col in (("letter", TEX_LETTER), ("slab", TEX_SLAB), ("edge", TEX_EDGE), ("slab2", TEX_SLAB2)):
        sx, sy = SWATCH[name]
        img.paste(Image.new("RGB", (16, 16), col), (sx, sy))
    return img


BASE = 0.0  # the plinth's height, set by main: the model stands on y = 0


def to_icon(p):
    """Shell units (y up, viewer at +z) -> icon units (y down, viewer at -z)."""
    return np.array([p[0] * SCALE, -(p[1] + BASE) * SCALE - 16, -p[2] * SCALE])


def to_icon_dir(n):
    return np.array([n[0], -n[1], -n[2]])


def uv_of(mat, p, layers):
    if isinstance(mat, tuple):
        L = layers[mat[1]]
        u = (p[0] + L["w"] / 2) / L["w"]
        v = (L["top"] - PLINTH_BEVEL - p[1]) / (L["h"] - PLINTH_BEVEL)
        u = 0.5 / TEX + u * (TEX - 1.0) / TEX
        v = (L["row"] + 0.5 + v * (L["rows"] - 1.0)) / TEX
        return u, v
    sx, sy = SWATCH[mat]
    return (sx + 8) / TEX, (sy + 8) / TEX


def write_icn(path, mesh, tex, layers):
    verts = []
    for a, b, c, mat in mesh.tris:
        nrm = np.cross(b - a, c - a)
        ln = np.linalg.norm(nrm)
        if ln < 1e-12:
            continue
        nrm = to_icon_dir(nrm / ln)
        for p in (a, b, c):
            q = to_icon(p)
            u, v = uv_of(mat, p, layers)
            verts.append((q, nrm, u, v))
    ntri = len(verts) // 3
    if ntri > 600:
        sys.exit(f"{ntri} triangles: more than 600")
    out = bytearray()
    out += struct.pack("<IIIfI", 0x00010000, 1, 0x07, 1.0, len(verts))
    for q, n, u, v in verts:
        qi = [int(round(t)) for t in q]
        for t in qi:
            if not -32768 <= t <= 32767:
                sys.exit("vertex out of range")
        out += struct.pack("<hhhh", qi[0], qi[1], qi[2], 1024)
        out += struct.pack("<hhhh", *[int(round(t * 4096)) for t in n], 1024)
        out += struct.pack("<hh", int(round(u * 4096)), int(round(v * 4096)))
        out += bytes((0x80, 0x80, 0x80, 0x80))
    out += struct.pack("<I", 1)  # sequences
    out += struct.pack("<IfII", 1, 0.0, 0, 1)  # frames, speed, offset, shapes used
    out += struct.pack("<II", 0, 1) + struct.pack("<ff", 0.0, 1.0)  # shape 0: one key, weight 1
    px = np.asarray(tex.convert("RGB"), dtype=np.uint16)
    r, g, b = px[..., 0] >> 3, px[..., 1] >> 3, px[..., 2] >> 3
    out += (r | (g << 5) | (b << 10) | 0x8000).astype("<u2").tobytes()
    with open(path, "wb") as f:
        f.write(out)
    return ntri


def sjis_full(s):
    full = "".join("　" if ch == " " else chr(ord(ch) + 0xFEE0) if 0x21 <= ord(ch) <= 0x7E else ch
                   for ch in s)
    return full.encode("shift_jis")


def unit(v):
    v = np.array(v, float)
    return v / np.linalg.norm(v)


def write_iconsys(path, title, title2, icn):
    t1, t2 = sjis_full(title), sjis_full(title2)
    if len(t2) > 32 or len(t1) + len(t2) > 66:
        sys.exit("title too long")
    out = bytearray(964)
    out[0:4] = b"PS2D"
    struct.pack_into("<HHII", out, 4, 0, len(t1), 0, 0x60)
    top, bottom = (0x16, 0x18, 0x1C), (0x06, 0x06, 0x07)  # graphite above, ink below
    for i, c in enumerate((top, top, bottom, bottom)):
        struct.pack_into("<4i", out, 16 + 16 * i, *c, 0)
    # direction the light travels, in icon units (y down, viewer at -z)
    lamp = unit((0.30, 0.80, 0.52))  # from above, in front, a little left
    rim = unit((0.65, 0.25, -0.72))  # from behind, left
    fill = unit((-0.30, -0.55, 0.78))  # low, from the front right
    for i, d in enumerate((lamp, rim, fill)):
        struct.pack_into("<4f", out, 80 + 16 * i, *d, 0.0)
    cols = ([c / 255 * 0.95 for c in LAMP], (0.22, 0.24, 0.28), (0.07, 0.07, 0.08))
    for i, c in enumerate(cols):
        struct.pack_into("<4f", out, 128 + 16 * i, *c, 0.0)
    struct.pack_into("<4f", out, 176, 0.17, 0.17, 0.18, 0.0)
    out[192:192 + len(t1) + len(t2)] = t1 + t2
    name = icn.encode()
    for off in (260, 324, 388):
        out[off:off + len(name)] = name
    with open(path, "wb") as f:
        f.write(out)


# --- preview: reads the files back and draws them as the browser should ---

def read_icn(path):
    d = open(path, "rb").read()
    ver, nsp, attr, bface, nv = struct.unpack_from("<IIIfI", d, 0)
    assert ver == 0x10000 and nsp == 1 and nv % 3 == 0
    off = 20
    V = []
    for _ in range(nv):
        x, y, z, _w = struct.unpack_from("<hhhh", d, off)
        nx, ny, nz, _ = struct.unpack_from("<hhhh", d, off + 8)
        u, v = struct.unpack_from("<hh", d, off + 16)
        V.append(((x, y, z), (nx / 4096, ny / 4096, nz / 4096), (u / 4096, v / 4096)))
        off += 24
    nseq = struct.unpack_from("<I", d, off)[0]
    off += 4
    for _ in range(nseq):
        _fr, _sp, _of, nks = struct.unpack_from("<IfII", d, off)
        off += 16
        for _ in range(nks):
            _id, nk = struct.unpack_from("<II", d, off)
            off += 8 + 8 * nk
    tex = np.frombuffer(d[off:off + 32768], dtype="<u2").reshape(128, 128)
    rgb = np.stack([(tex & 31) << 3, ((tex >> 5) & 31) << 3, ((tex >> 10) & 31) << 3], -1).astype(float) / 255
    assert len(d) == off + 32768, (len(d), off)
    return V, rgb, attr


def read_iconsys(path):
    d = open(path, "rb").read()
    assert d[:4] == b"PS2D" and len(d) == 964
    lf, trans = struct.unpack_from("<H", d, 6)[0], struct.unpack_from("<I", d, 12)[0]
    bg = [struct.unpack_from("<4i", d, 16 + 16 * i)[:3] for i in range(4)]
    ld = [np.array(struct.unpack_from("<4f", d, 80 + 16 * i)[:3]) for i in range(3)]
    lc = [np.array(struct.unpack_from("<4f", d, 128 + 16 * i)[:3]) for i in range(3)]
    amb = np.array(struct.unpack_from("<4f", d, 176)[:3])
    title = d[192:260].split(b"\0")[0].decode("shift_jis")
    return dict(lf=lf, bg=bg, ld=ld, lc=lc, amb=amb, title=title[:lf // 2] + " / " + title[lf // 2:])


def preview(icn, iconsys, png, angles=(0, -25, 25, 90, 180)):
    V, tex, _ = read_icn(icn)
    sysd = read_iconsys(iconsys)
    W = H = 256
    tiles = []
    for ang in angles:
        a = math.radians(ang)
        ca, sa = math.cos(a), math.sin(a)
        top, bot = np.array(sysd["bg"][0]) / 255, np.array(sysd["bg"][2]) / 255
        img = np.zeros((H, W, 3))
        for yy in range(H):
            img[yy] = top + (bot - top) * yy / (H - 1)
        zb = np.full((H, W), np.inf)
        s = W / 26000
        for t in range(0, len(V), 3):
            pts, cols = [], None
            tri = V[t:t + 3]
            P = []
            for (x, y, z), n, uv in tri:
                xr, zr = x * ca + z * sa, -x * sa + z * ca  # turn about y
                P.append((W / 2 + xr * s, H * 0.78 + (y + 0) * s, zr))
            n = np.array(tri[0][1])
            n = np.array([n[0] * ca + n[2] * sa, n[1], -n[0] * sa + n[2] * ca])
            if n[2] > 0.02:  # faces away from the viewer at -z
                continue
            light = sysd["amb"].copy()
            for d, c in zip(sysd["ld"], sysd["lc"]):
                light += c * max(0.0, float(-(n @ d)))
            (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = P
            den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
            if abs(den) < 1e-9:
                continue
            bx0, bx1 = int(max(0, math.floor(min(x0, x1, x2)))), int(min(W - 1, math.ceil(max(x0, x1, x2))))
            by0, by1 = int(max(0, math.floor(min(y0, y1, y2)))), int(min(H - 1, math.ceil(max(y0, y1, y2))))
            if bx0 > bx1 or by0 > by1:
                continue
            gx, gy = np.meshgrid(np.arange(bx0, bx1 + 1) + 0.5, np.arange(by0, by1 + 1) + 0.5)
            l0 = ((y1 - y2) * (gx - x2) + (x2 - x1) * (gy - y2)) / den
            l1 = ((y2 - y0) * (gx - x2) + (x0 - x2) * (gy - y2)) / den
            l2 = 1 - l0 - l1
            m = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
            zz = l0 * z0 + l1 * z1 + l2 * z2
            uvs = np.array([v[2] for v in tri])
            u = l0 * uvs[0, 0] + l1 * uvs[1, 0] + l2 * uvs[2, 0]
            v = l0 * uvs[0, 1] + l1 * uvs[1, 1] + l2 * uvs[2, 1]
            tx = np.clip((u * 128).astype(int), 0, 127)
            ty = np.clip((v * 128).astype(int), 0, 127)
            sub = zb[by0:by1 + 1, bx0:bx1 + 1]
            m &= zz < sub
            col = np.clip(tex[ty, tx] * light, 0, 1)
            sub[m] = zz[m]
            img[by0:by1 + 1, bx0:bx1 + 1][m] = col[m]
        tiles.append(img)
    strip = np.concatenate(tiles, axis=1)
    Image.fromarray((strip * 255).astype(np.uint8)).resize((W * len(angles) * 2 // 2, H)).save(png)
    print(f"{icn}: {len(V) // 3} triangles; title {sysd['title']!r}; preview {png}")


def main():
    global BASE
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--font")
    ap.add_argument("--word")
    ap.add_argument("--word2", help="the word on a second, lower layer of the plinth")
    ap.add_argument("--title")
    ap.add_argument("--title2", default="")
    ap.add_argument("--icn")
    ap.add_argument("--out")
    ap.add_argument("--preview", nargs=3, metavar=("ICN", "ICONSYS", "PNG"))
    a = ap.parse_args()
    if a.preview:
        preview(*a.preview)
        return
    if not (a.font and a.word and a.title and a.icn and a.out):
        ap.error("--font, --word, --title, --icn and --out are needed")
    os.makedirs(a.out, exist_ok=True)
    words = [a.word] + ([a.word2] if a.word2 is not None else [])
    layers = [dict(L) for L in LAYERS[:len(words)]]
    mesh, base = build(layers)
    BASE = base
    tex = label_texture(a.font, words, layers)
    n = write_icn(os.path.join(a.out, a.icn), mesh, tex, layers)
    write_iconsys(os.path.join(a.out, "icon.sys"), a.title, a.title2, a.icn)
    print(f"{a.out}: {a.icn} ({n} triangles), icon.sys")


if __name__ == "__main__":
    main()
