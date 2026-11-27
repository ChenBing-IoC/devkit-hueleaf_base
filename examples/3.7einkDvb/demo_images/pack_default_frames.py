#!/usr/bin/env python3
"""Pack the demo PNGs into otp6-v1 frames for the firmware defaults."""
import os
import sys

from PIL import Image

W, H = 480, 720
INF = 1e9
# ink code -> sRGB
INKS = [
    (0, (0x11, 0x14, 0x12)),  # black
    (1, (0xF4, 0xF3, 0xE8)),  # white
    (2, (0xE7, 0xC8, 0x3F)),  # yellow
    (3, (0xC6, 0x3B, 0x35)),  # red
    (5, (0x31, 0x5E, 0xAE)),  # blue
    (6, (0x39, 0x84, 0x51)),  # green
]


def srgb_to_linear(v):
    v /= 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def oklab(rgb):
    r, g, b = (srgb_to_linear(c) for c in rgb)
    l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b
    m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b
    s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b
    l, m, s = (v ** (1.0 / 3.0) if v >= 0 else -((-v) ** (1.0 / 3.0)) for v in (l, m, s))
    return (
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
    )


PAL = [(code, oklab(rgb)) for code, rgb in INKS]


def nearest(lab):
    best, best_d = 0, INF
    for code, pl in PAL:
        d = (lab[0] - pl[0]) ** 2 + (lab[1] - pl[1]) ** 2 + (lab[2] - pl[2]) ** 2
        if d < best_d:
            best, best_d = code, d
    return best


def quantize(img):
    rgb = img.convert("RGB")
    px = rgb.load()
    labs = [[None] * W for _ in range(H)]
    buf = [0.0] * 3 * W
    out = [[0] * W for _ in range(H)]
    for y in range(H):
        row = labs[y]
        for x in range(W):
            r, g, b = px[x, y]
            row[x] = list(oklab((r, g, b)))
    for y in range(H):
        rng = range(W) if y % 2 == 0 else range(W - 1, -1, -1)
        step = 1 if y % 2 == 0 else -1
        for x in rng:
            cur = labs[y][x]
            code = nearest(cur)
            out[y][x] = code
            # error in OKLab
            ref = dict(PAL)[code]
            er = (cur[0] - ref[0], cur[1] - ref[1], cur[2] - ref[2])
            for dx, dy, wgt in ((step, 0, 7), (-step, 1, 3), (0, 1, 5), (step, 1, 1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < W and 0 <= ny < H:
                    t = labs[ny][nx]
                    for i in range(3):
                        t[i] += er[i] * wgt / 16.0
    return out


def pack(codes):
    data = bytearray(W * H // 2)
    for px in range(W):
        gate = W - 1 - px
        base = gate * (H // 2)
        for py in range(0, H, 2):
            hi = codes[py][px]
            lo = codes[py + 1][px]
            data[base + py // 2] = (hi << 4) | lo
    return bytes(data)


def preview(codes, path):
    im = Image.new("RGB", (W, H))
    im.putdata([dict(INKS)[codes[y][x]] for y in range(H) for x in range(W)])
    im.save(path, optimize=True)


def main(src_dir, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    names = ["01_dusk_landscape.png", "02_six_ink_poster.png", "03_panel_test_card.png"]
    for i, name in enumerate(names):
        codes = quantize(Image.open(os.path.join(src_dir, name)))
        blob = pack(codes)
        assert len(blob) == 172800, len(blob)
        with open(os.path.join(out_dir, f"pic{i}.bin"), "wb") as fh:
            fh.write(blob)
        preview(codes, os.path.join(out_dir, f"preview{i}.png"))
        print(name, "->", f"pic{i}.bin", len(blob))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
