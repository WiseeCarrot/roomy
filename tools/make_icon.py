"""Draws res/roomy.ico. Run with: python tools/make_icon.py"""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
S = 1024  # draw big, then scale down for each icon size


def rounded(d, box, r, **kw):
    d.rounded_rectangle(box, radius=r, **kw)


def draw():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Squircle-ish tile with a soft vertical gradient.
    top, bottom = (58, 196, 132), (24, 132, 86)
    tile = Image.new("RGBA", (S, S))
    td = ImageDraw.Draw(tile)
    for y in range(S):
        t = y / (S - 1)
        td.line([(0, y), (S, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    mask = Image.new("L", (S, S), 0)
    rounded(ImageDraw.Draw(mask), (40, 40, S - 40, S - 40), 230, fill=255)
    img.paste(tile, (0, 0), mask)

    # Three storage bars. The faint tail on each is the space Roomy frees.
    x0, x1, h, r = 232, 792, 112, 56
    rows = [(300, 0.82), (456, 0.58), (612, 0.36)]
    for y, used in rows:
        rounded(d, (x0, y, x1, y + h), r, fill=(255, 255, 255, 70))
        rounded(d, (x0, y, x0 + int((x1 - x0) * used), y + h), r, fill=(255, 255, 255, 255))
    return img


def main():
    big = draw()
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    big.save(ROOT / "res" / "roomy.ico", sizes=[(s, s) for s in sizes])
    big.resize((256, 256), Image.LANCZOS).save(ROOT / "docs" / "icon.png")
    print("wrote res/roomy.ico and docs/icon.png")


if __name__ == "__main__":
    main()
