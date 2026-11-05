#!/usr/bin/env python3
"""Writes iop/chime_blip.h: the boot sound as SPU ADPCM, 0.2 s of 880 Hz at
24 kHz, fading out. maxsdk's mx_snd_encode (src/base/snd.c) in Python, so the
module carries the sound that maxsdk's tests/ps2/sndapi.c plays as its blip."""
import math

RATE, FREQ, AMP, N = 24000, 880, 10000, 24000 // 5
FILT = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def clamp(v):
    return max(-32768, min(32767, v))


def cdiv(a, b):  # C's division
    return abs(a) // b * (1 if a >= 0 else -1)


def try_block(x, f, sh, h1, h2):
    s1, s2, step, err = h1, h2, 1 << (12 - sh), 0
    b = [f << 4 | sh, 0] + [0] * 14
    for i in range(28):
        pred = (s1 * FILT[f][0] + s2 * FILT[f][1] + 32) >> 6
        r = x[i] - pred
        q = max(-8, min(7, cdiv(r + (-step // 2 if r < 0 else step // 2), step)))
        d = clamp(q * step + pred)
        err += (x[i] - d) ** 2
        s1, s2 = d, s1
        b[2 + i // 2] |= (q & 15) << (4 if i & 1 else 0)
    return err, b, s1, s2


def encode(pcm):
    out, h1, h2 = [], 0, 0
    for k in range((len(pcm) + 27) // 28):
        x = (pcm[k * 28 : k * 28 + 28] + [0] * 28)[:28]
        best = None
        for f in range(5):
            p1, p2, m = h1, h2, 0
            for v in x:
                m = max(m, abs(v - ((p1 * FILT[f][0] + p2 * FILT[f][1] + 32) >> 6)))
                p1, p2 = v, p1
            sh = 12
            while sh > 0 and (7 << (12 - sh)) < m:
                sh -= 1
            for s in (sh, sh - 1):
                if s >= 0:
                    t = try_block(x, f, s, h1, h2)
                    if best is None or t[0] < best[0]:
                        best = t
        _, b, h1, h2 = best
        out += b
    out[-15] |= 1  # the last block: end, the voice stops
    return out


pcm = [int(int(AMP * math.sin(2 * math.pi * FREQ * i / RATE)) * (N - i) / N) for i in range(N)]
data = encode(pcm)
words = [data[i] | data[i + 1] << 8 for i in range(0, len(data), 2)]
words += [0] * (-len(words) % 32)  # whole 64-byte transfers
with open(__file__.replace(".py", ".h"), "w") as h:
    h.write("/* made by chime_blip.py */\nstatic const u16 blip[%d] = {\n" % len(words))
    for i in range(0, len(words), 12):
        h.write("  " + ", ".join("0x%04x" % w for w in words[i : i + 12]) + ",\n")
    h.write("};\n")
