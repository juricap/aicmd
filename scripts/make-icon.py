"""Generates src/gui/aicmd.ico: two file panels, a full-row cursor and a red mark.

Run: python scripts/make-icon.py  (requires Pillow). Output is tracked; rerun only to change the design.
"""
from pathlib import Path
from PIL import Image, ImageDraw

BG, EDGE = (29, 38, 51), (64, 80, 102)
PANEL, ROW = (236, 241, 246), (168, 180, 196)
CURSOR, MARK = (47, 111, 214), (214, 69, 69)


def draw(size):
    # Small sizes are drawn on a pixel grid; large ones are supersampled.
    scale = 1 if size <= 32 else 4
    s = size * scale
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    u = s / 16.0
    radius = max(1, round(2.6 * u)) if size > 16 else 2
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=radius, fill=BG, outline=EDGE if size >= 32 else None, width=max(1, round(0.25 * u)))
    pad, gap, top = round(1.5 * u), max(1, round(0.75 * u)), round(2.5 * u)
    width = (s - 2 * pad - gap) // 2
    bottom = s - pad
    for i in range(2):
        x0 = pad + i * (width + gap)
        x1 = x0 + width - 1
        d.rectangle([x0, round(1.2 * u), x1, top - max(1, round(0.4 * u))], fill=CURSOR if i == 0 else EDGE)  # path bar
        d.rectangle([x0, top, x1, bottom - 1], fill=PANEL)
        rows = 4 if size <= 20 else 5
        step = (bottom - top) / (rows + 0.5)
        line = max(1, round(step * 0.38))
        for r in range(rows):
            y = round(top + step * (r + 0.55))
            if i == 0 and r == 1:
                d.rectangle([x0, y - line, x1, y + line], fill=CURSOR)  # full-row cursor
                continue
            colour = MARK if (i == 1 and r == 2) else ROW
            inset = max(1, round(0.6 * u))
            length = x1 - inset - (round((r * 7 + i * 3) % 4 * 0.6 * u) if size > 16 else 0)
            d.rectangle([x0 + inset, y - line // 2, length, y + (line - 1) // 2], fill=colour)
    if scale > 1:
        img = img.resize((size, size), Image.LANCZOS)
    return img


root = Path(__file__).resolve().parent.parent
sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
images = [draw(n) for n in sizes]
out = root / 'src' / 'gui' / 'aicmd.ico'
images[-1].save(out, format='ICO', sizes=[(n, n) for n in sizes], append_images=images[:-1])
preview = root / 'artifacts' / 'icon-preview.png'
preview.parent.mkdir(exist_ok=True)
sheet = Image.new('RGBA', (sum(sizes) + 10 * len(sizes), 256), (255, 255, 255, 255))
x = 0
for n, im in zip(sizes, images):
    sheet.paste(im, (x, 0), im)
    x += n + 10
sheet.save(preview)
print(out, preview)
