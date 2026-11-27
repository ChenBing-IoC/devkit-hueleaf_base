#!/usr/bin/env python3
"""Three 480x720 demo pictures built from the EPD037A02 six-ink palette."""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 480, 720
INK = {
    "black": (0x11, 0x14, 0x12),
    "white": (0xF4, 0xF3, 0xE8),
    "yellow": (0xE7, 0xC8, 0x3F),
    "red": (0xC6, 0x3B, 0x35),
    "blue": (0x31, 0x5E, 0xAE),
    "green": (0x39, 0x84, 0x51),
}
FONT_B = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONT_R = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"


def font(path, size):
    return ImageFont.truetype(path, size)


def mix(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def landscape():
    """Scene-like gradient panel: dusk sky, layered hills, a lake."""
    img = Image.new("RGB", (W, H), INK["white"])
    px = img.load()
    sky_top, sky_mid, sky_low = (0x6E, 0x8F, 0xB8), INK["yellow"], (0xE4, 0x9A, 0x62)
    for y in range(0, 430):
        t = y / 430.0
        c = mix(sky_top, sky_mid, t * 2) if t < 0.5 else mix(sky_mid, sky_low, (t - 0.5) * 2)
        for x in range(W):
            px[x, y] = c
    # sun
    d = ImageDraw.Draw(img)
    d.ellipse((150, 250, 330, 430), fill=INK["red"])
    d.ellipse((170, 270, 310, 410), fill=(0xE8, 0x7A, 0x50))
    # clouds: soft white bands
    cloud = Image.new("L", (W, H), 0)
    cd = ImageDraw.Draw(cloud)
    for i, (cx, cy, w, h) in enumerate([(60, 120, 220, 34), (250, 180, 200, 28), (30, 220, 150, 22)]):
        cd.ellipse((cx, cy, cx + w, cy + h), fill=200 - i * 20)
    cloud = cloud.filter(ImageFilter.GaussianBlur(8))
    white = Image.new("RGB", (W, H), INK["white"])
    img = Image.composite(white, img, cloud.point(lambda v: v // 2))
    # hills
    d = ImageDraw.Draw(img)
    d.polygon([(0, 430), (120, 360), (250, 430), (360, 390), (480, 440), (480, 520), (0, 520)], fill=INK["green"])
    d.polygon([(0, 470), (110, 430), (210, 470), (330, 440), (480, 480), (480, 560), (0, 560)], fill=(0x2B, 0x5F, 0x3C))
    # lake
    d.rectangle((0, 520, W, H), fill=INK["blue"])
    for i in range(18):
        y = 530 + i * 10
        x0 = 40 + (i * 37) % 200
        d.line((x0, y, x0 + 90 + (i * 23) % 160, y), fill=mix(INK["blue"], INK["white"], 0.35), width=3)
    # sun reflection
    for i in range(9):
        y = 528 + i * 12
        wdt = 120 - i * 9
        d.line((240 - wdt // 2, y, 240 + wdt // 2, y), fill=INK["yellow"], width=4)
    # birds
    for bx, by, s in [(352, 150, 10), (392, 168, 8), (330, 186, 7)]:
        d.line((bx - s, by, bx, by - s // 2), fill=INK["black"], width=2)
        d.line((bx, by - s // 2, bx + s, by), fill=INK["black"], width=2)
    d.text((26, 26), "6-INK  /  DUSK", font=font(FONT_B, 26), fill=INK["black"])
    d.text((26, 60), "480 x 720  EPD037A02", font=font(FONT_R, 18), fill=(0x2A, 0x2E, 0x2C))
    return img


def poster():
    """Flat geometric poster: the strongest use of large saturated fields."""
    img = Image.new("RGB", (W, H), INK["white"])
    d = ImageDraw.Draw(img)
    d.rectangle((0, 0, W, 240), fill=INK["yellow"])
    d.rectangle((0, 240, W, 470), fill=INK["red"])
    d.rectangle((0, 470, W, H), fill=INK["blue"])
    d.ellipse((-90, 70, 210, 370), fill=INK["white"])
    d.pieslice((250, 90, 520, 360), 180, 360, fill=INK["green"])
    d.ellipse((300, 520, 430, 650), fill=INK["yellow"])
    d.rectangle((40, 540, 150, 690), fill=INK["black"])
    d.polygon([(0, 470), (W, 470), (W, 500), (0, 500)], fill=INK["black"])
    d.polygon([(210, 470), (300, 470), (255, 560)], fill=INK["white"])
    # thin rules pull the eye to the panel resolution
    for i in range(9):
        d.rectangle((24 + i * 5, 300, 26 + i * 5, 420), fill=INK["white"])
    d.text((28, 210), "SIX INKS", font=font(FONT_B, 54), fill=INK["black"])
    d.text((232, 262), "BLACK  WHITE  YELLOW", font=font(FONT_R, 18), fill=INK["white"])
    d.text((232, 290), "RED  BLUE  GREEN", font=font(FONT_R, 18), fill=INK["white"])
    d.text((232, 690), "NO BACKLIGHT", font=font(FONT_B, 20), fill=INK["white"])
    return img


def test_card():
    """Measurement card: bars, ramps and line pairs for the six-ink panel."""
    img = Image.new("RGB", (W, H), INK["white"])
    d = ImageDraw.Draw(img)
    names = ["BLACK", "WHITE", "YELLOW", "RED", "BLUE", "GREEN"]
    keys = ["black", "white", "yellow", "red", "blue", "green"]
    bar_w = W // 6
    for i, key in enumerate(keys):
        x0 = i * bar_w
        d.rectangle((x0, 60, x0 + bar_w, 240), fill=INK[key])
        d.text((x0 + 8, 210), names[i], font=font(FONT_B, 14),
               fill=INK["black"] if key in ("white", "yellow") else INK["white"])
    # hairline frame so the white patch reads as a patch
    d.rectangle((0, 60, W - 1, 240), outline=INK["black"], width=2)
    for i in range(1, 6):
        d.line((i * bar_w, 60, i * bar_w, 240), fill=INK["black"], width=1)
    d.text((16, 16), "EPD037A02  SIX-INK TEST", font=font(FONT_B, 26), fill=INK["black"])
    d.text((16, 260), "DITHER RAMPS  0 - 100%", font=font(FONT_B, 18), fill=INK["black"])
    # checker ramps for each ink against white
    for row, key in enumerate(["black", "red", "blue", "green", "yellow"]):
        y = 292 + row * 38
        d.text((16, y + 6), key[:3].upper(), font=font(FONT_B, 14), fill=INK["black"])
        for step in range(16):
            x0 = 90 + step * 24
            frac = step / 15.0
            for yy in range(y, y + 30):
                for xx in range(x0, x0 + 24):
                    on = ((xx + yy) % 2 == 0) if False else (((xx * 7 + yy * 3) % 10) / 10.0 < frac)
                    if on:
                        img.putpixel((xx, yy), INK[key])
    # line pairs
    d.text((16, 500), "LINE PAIRS  1px / 2px / 3px", font=font(FONT_B, 16), fill=INK["black"])
    x = 20
    for width in (1, 2, 3):
        for i in range(8):
            d.rectangle((x, 530, x + width - 1, 600), fill=INK["black"])
            x += width * 2
        x += 14
    # solid block mix and text sizes
    d.rectangle((20, 620, 200, 700), fill=INK["blue"])
    d.text((30, 626), "TEXT 14", font=font(FONT_R, 14), fill=INK["white"])
    d.text((30, 646), "TEXT 20", font=font(FONT_R, 20), fill=INK["white"])
    d.text((30, 666), "TEXT 26", font=font(FONT_B, 24), fill=INK["yellow"])
    d.rectangle((230, 620, 460, 700), fill=INK["red"])
    d.text((244, 636), "FULL REFRESH", font=font(FONT_B, 26), fill=INK["white"])
    d.text((244, 674), "BWRYBG  6 INK", font=font(FONT_R, 18), fill=INK["yellow"])
    return img


def main(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for name, fn in [("01_dusk_landscape.png", landscape),
                     ("02_six_ink_poster.png", poster),
                     ("03_panel_test_card.png", test_card)]:
        path = os.path.join(out_dir, name)
        fn().save(path, optimize=True)
        print(path, os.path.getsize(path))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
