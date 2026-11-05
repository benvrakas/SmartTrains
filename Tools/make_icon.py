"""Draws the SmartTrains icon: one train at the bottom, tracks fanning out to three platforms, the one in need lit up.

  python Tools/make_icon.py

Writes Docs/Images/icon-1080.png (full resolution), Docs/Images/icon-512.png (mod page) and Resources/Icon128.png
(shown in the game's Mods menu). The design is laid out on a 512-unit grid and rendered at full resolution.
"""
import math
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZE = 512  # Design grid; all coordinates below are in these units.
FULL_SIZE = 1080
SUPERSAMPLE = 2  # Drawn larger than FULL_SIZE and scaled down for smooth edges.
SCALE = FULL_SIZE * SUPERSAMPLE / SIZE

BACKGROUND_TOP = (38, 44, 54)
BACKGROUND_BOTTOM = (22, 26, 32)
RAIL = (120, 126, 136)
BED = (52, 58, 68)
ORANGE = (250, 149, 73)
ORANGE_DARK = (196, 104, 40)
GREY = (92, 98, 108)
GREY_DARK = (64, 69, 77)
WHEEL = (30, 32, 36)
WHITE = (238, 240, 242)


def s(value):
    return int(value * SCALE)


def box(x0, y0, x1, y1):
    return [s(x0), s(y0), s(x1), s(y1)]


def bezier(p0, p1, p2, p3, steps=60):
    points = []
    for i in range(steps + 1):
        t = i / steps
        u = 1 - t
        x = u**3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0]
        y = u**3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1]
        points.append((x, y))
    return points


def draw_track(draw, points, lit):
    scaled = [(s(x), s(y)) for x, y in points]
    draw.line(scaled, fill=ORANGE_DARK if lit else BED, width=s(22), joint="curve")
    draw.line(scaled, fill=ORANGE if lit else RAIL, width=s(14), joint="curve")
    draw.line(scaled, fill=BACKGROUND_BOTTOM, width=s(6), joint="curve")


def draw_platform(draw, cx, top, lit):
    body, trim = (ORANGE, ORANGE_DARK) if lit else (GREY, GREY_DARK)
    draw.rounded_rectangle(box(cx - 52, top, cx + 52, top + 74), radius=s(8), fill=body)
    draw.rectangle(box(cx - 52, top + 58, cx + 52, top + 74), fill=trim)
    for crate in (-30, 4):
        draw.rounded_rectangle(box(cx + crate, top + 14, cx + crate + 26, top + 40), radius=s(3), fill=trim)
        draw.line([s(cx + crate + 13), s(top + 14), s(cx + crate + 13), s(top + 40)], fill=body, width=s(3))


def draw_wheels(draw, x, y, width, trim):
    for wx in (x + width * 0.24, x + width * 0.76):
        draw.ellipse(box(wx - 11, y - 11, wx + 11, y + 11), fill=WHEEL)
        draw.ellipse(box(wx - 4, y - 4, wx + 4, y + 4), fill=trim)


def draw_train(draw, cx, y):
    width, height, gap = 96, 52, 10
    left = cx - (2 * width + gap) / 2
    draw.rounded_rectangle(box(left, y, left + width, y + height), radius=s(7), fill=GREY)
    draw.rectangle(box(left - 3, y + height - 6, left + width + 3, y + height + 2), fill=GREY_DARK)
    draw.rectangle(box(left + width, y + height - 10, left + width + gap, y + height - 4), fill=GREY_DARK)
    draw_wheels(draw, left, y + height + 4, width, GREY_DARK)
    x = left + width + gap
    draw.rounded_rectangle(box(x, y - 14, x + width, y + height), radius=s(9), fill=ORANGE)
    draw.rounded_rectangle(box(x + width * 0.50, y - 14, x + width - 6, y + 18), radius=s(5), fill=BACKGROUND_TOP)
    draw.rectangle(box(x - 3, y + height - 6, x + width + 3, y + height + 2), fill=ORANGE_DARK)
    draw_wheels(draw, x, y + height + 4, width, ORANGE_DARK)


def main():
    big = FULL_SIZE * SUPERSAMPLE
    image = Image.new("RGB", (big, big))
    draw = ImageDraw.Draw(image)
    for row in range(big):
        t = row / big
        color = tuple(int(BACKGROUND_TOP[i] + (BACKGROUND_BOTTOM[i] - BACKGROUND_TOP[i]) * t) for i in range(3))
        draw.line([(0, row), (big, row)], fill=color)

    hub = (256, 330)
    targets = [(112, 150), (256, 150), (400, 150)]
    lit = 2
    for index, (tx, ty) in enumerate(targets):
        if index == lit:
            continue
        draw_track(draw, bezier((hub[0], hub[1]), (hub[0], hub[1] - 80), (tx, ty + 130), (tx, ty + 80)), False)
    draw_track(draw, bezier(hub, (hub[0], hub[1] - 80), (targets[lit][0], targets[lit][1] + 130),
                            (targets[lit][0], targets[lit][1] + 80)), True)
    for index, (tx, ty) in enumerate(targets):
        draw_platform(draw, tx, ty - 6, index == lit)

    draw_track(draw, [(256, 330), (256, 500)], False)
    draw_train(draw, 256, 372)

    full = image.resize((FULL_SIZE, FULL_SIZE), Image.LANCZOS)
    docs = os.path.join(ROOT, "Docs", "Images")
    resources = os.path.join(ROOT, "Resources")
    os.makedirs(docs, exist_ok=True)
    os.makedirs(resources, exist_ok=True)
    full.save(os.path.join(docs, "icon-1080.png"))
    full.resize((512, 512), Image.LANCZOS).save(os.path.join(docs, "icon-512.png"))
    full.resize((128, 128), Image.LANCZOS).save(os.path.join(resources, "Icon128.png"))
    print("Wrote Docs/Images/icon-1080.png, Docs/Images/icon-512.png and Resources/Icon128.png")


if __name__ == "__main__":
    main()
