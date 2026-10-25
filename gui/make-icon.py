# Makes gui/dd.ico: a giraffe (head and neck, facing right) looking at an
# SD card.  It is drawn on a 256 grid at S times that size, then scaled
# down; 16 to 24 pixels show a close-up of the head instead, with the card
# as a small badge drawn at the final size, since scaled it would smudge.  Needs Python with Pillow
# and numpy; it is not part of the build.
#   python gui/make-icon.py [out.ico] [--preview sheet.png]
import os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageChops

S = 4
N = 256 * S

COAT = (238, 178, 72, 255)       # golden coat
PATCH = (150, 82, 34, 255)       # brown patches
LINE = (74, 40, 18, 255)         # outline
MUZZLE = (250, 222, 170, 255)    # pale muzzle
EAR_IN = (214, 140, 64, 255)
DARK = (50, 28, 14, 255)

def P(pts):
    return [(x * S, y * S) for x, y in pts]

def smooth(pts, steps=12):
    """Closed Catmull-Rom curve through pts."""
    out = []
    n = len(pts)
    for i in range(n):
        p0, p1, p2, p3 = pts[i - 1], pts[i], pts[(i + 1) % n], pts[(i + 2) % n]
        for k in range(steps):
            t = k / steps
            t2, t3 = t * t, t * t * t
            out.append(tuple(0.5 * ((2 * p1[j]) + (-p0[j] + p2[j]) * t +
                                    (2 * p0[j] - 5 * p1[j] + 4 * p2[j] - p3[j]) * t2 +
                                    (-p0[j] + 3 * p1[j] - 3 * p2[j] + p3[j]) * t3) for j in range(2)))
    return out

def ellipse(d, cx, cy, rx, ry, fill):
    d.ellipse([(cx - rx) * S, (cy - ry) * S, (cx + rx) * S, (cy + ry) * S], fill=fill)

# --- shapes, on the 256 grid ---------------------------------------------------
neck = smooth([(58, 262), (78, 200), (100, 140), (116, 96), (150, 84), (164, 112),
               (150, 160), (148, 210), (156, 262)])
head = smooth([(112, 74), (134, 52), (166, 50), (200, 70), (228, 96), (236, 120),
               (224, 140), (196, 140), (164, 122), (128, 108), (108, 94)])
muzzle = smooth([(198, 94), (226, 98), (238, 118), (228, 138), (204, 140), (190, 120)])
ear = smooth([(120, 70), (96, 54), (70, 52), (84, 70), (112, 84)])
ear_in = smooth([(114, 72), (96, 60), (82, 59), (92, 70), (110, 80)])
horns = [((132, 56), (124, 22)), ((156, 52), (158, 18))]          # ossicones
knob_r = 9
# small spots on the head, behind and below the eye, never above it
head_spots = [
    [(132, 66), (144, 60), (150, 70), (138, 76)],
    [(124, 88), (136, 84), (138, 96), (126, 98)],
    [(150, 100), (160, 98), (160, 108), (150, 110)],
]

# --- the silhouette, for the outline ----------------------------------------------
mask = Image.new('L', (N, N), 0)
m = ImageDraw.Draw(mask)
for shape in (neck, head, muzzle, ear):
    m.polygon(P(shape), fill=255)
for a, b in horns:
    m.line(P([a, b]), fill=255, width=12 * S)
    ellipse(m, b[0], b[1], knob_r, knob_r, 255)
outline = mask.filter(ImageFilter.MaxFilter(2 * 4 * S + 1))      # 4 px on the 256 grid

img = Image.new('RGBA', (N, N), (0, 0, 0, 0))
img.paste(LINE, (0, 0), outline)

# coat
body = Image.new('RGBA', (N, N), (0, 0, 0, 0))
d = ImageDraw.Draw(body)
for shape in (neck, head, ear):
    d.polygon(P(shape), fill=COAT)
for a, b in horns:
    d.line(P([a, b]), fill=COAT, width=12 * S)
# reticulated patches on the neck: Voronoi cells around jittered seeds,
# with a narrow coat-coloured gap where two cells meet
rng = np.random.default_rng(7)
seeds = []
for row, y in enumerate(range(96, 290, 26)):
    for x in range(40, 190, 30):
        seeds.append((x + (15 if row % 2 else 0) + rng.uniform(-6, 6), y + rng.uniform(-5, 5)))
seeds = np.array(seeds) * S
ys, xs = np.mgrid[0:N, 0:N]
d1 = np.full((N, N), np.inf); d2 = np.full((N, N), np.inf)
for sx, sy in seeds:
    dist = np.hypot(xs - sx, ys - sy)
    d2 = np.where(dist < d1, d1, np.minimum(d2, dist))
    d1 = np.minimum(d1, dist)
cells = Image.fromarray(((d2 - d1) > 5 * S).astype(np.uint8) * 255)
neck_mask = Image.new('L', (N, N), 0)
ImageDraw.Draw(neck_mask).polygon(P(neck), fill=255)
neck_mask = neck_mask.filter(ImageFilter.MinFilter(2 * 3 * S + 1))   # coat shows at the edges
spots = ImageChops.multiply(cells.filter(ImageFilter.GaussianBlur(1.5 * S)).point(lambda v: 255 if v > 128 else 0), neck_mask)
hs = ImageDraw.Draw(spots)
for p in head_spots:
    hs.polygon(P(smooth(p, 8)), fill=255)
body.paste(PATCH, (0, 0), spots)
d = ImageDraw.Draw(body)
# mane along the back of the neck
d.line(P([(62, 258), (80, 200), (100, 146), (114, 104)]), fill=PATCH, width=7 * S, joint='curve')
d.polygon(P(ear_in), fill=EAR_IN)
d.polygon(P(muzzle), fill=MUZZLE)
for a, b in horns:
    ellipse(d, b[0], b[1], knob_r, knob_r, DARK)
# face
ellipse(d, 184, 104, 9, 4, (246, 150, 120, 255))  # blush
ellipse(d, 172, 86, 10, 11, DARK)                # eye
ellipse(d, 175, 81, 4, 4, (255, 255, 255, 255))  # its highlight
ellipse(d, 168, 91, 2, 2, (255, 255, 255, 255))
ellipse(d, 222, 110, 4, 3, DARK)                 # nostril
d.arc([204 * S, 116 * S, 230 * S, 134 * S], 20, 120, fill=LINE, width=3 * S)  # a slight smile

img.alpha_composite(body)

# an SD card, which the giraffe is looking at
def add_card(img):
    c = ImageDraw.Draw(img)
    c.rounded_rectangle([174 * S, 166 * S, 244 * S, 252 * S], radius=8 * S, fill=LINE)
    c.polygon(P([(178, 170), (222, 170), (240, 188), (240, 248), (178, 248)]),
              fill=(64, 84, 112, 255), outline=LINE, width=3 * S)
    c.rounded_rectangle([186 * S, 206 * S, 232 * S, 240 * S], radius=4 * S, fill=(228, 236, 244, 255))  # label
    for i in range(4):                                                  # contacts
        x = 186 + i * 11
        c.rectangle([x * S, 176 * S, (x + 6) * S, 194 * S], fill=(236, 196, 92, 255))
    # the clipped corner, cut out of the rounded outline and edged again
    c.polygon(P([(226, 162), (248, 162), (248, 184)]), fill=(0, 0, 0, 0))
    c.line(P([(222, 170), (240, 188)]), fill=LINE, width=7 * S)
    return img

# the card again, as a badge in the corner of the close-ups: drawn pixel by
# pixel at the final size, since a scaled-down one would blur away
def add_badge(im):
    n = im.width
    w, h = round(n * 0.38), round(n * 0.46)
    x0, y0, x1, y1 = n - w, n - h, n - 1, n - 1
    c = ImageDraw.Draw(im)
    c.rectangle([x0, y0, x1, y1], fill=LINE)
    c.rectangle([x0 + 1, y0 + 1, x1 - 1, y1 - 1], fill=(64, 84, 112, 255))
    label_top = y0 + 1 + (h - 2) // 2
    c.rectangle([x0 + 2, label_top, x1 - 2, y1 - 2], fill=(228, 236, 244, 255))
    if n >= 20:                                                         # contacts
        for x in range(x0 + 2, x1 - 2, 2):
            c.rectangle([x, y0 + 2, x, label_top - 2], fill=(236, 196, 92, 255))
    # the clipped corner
    cut = max(1, n // 12)
    for i in range(cut):
        c.line([(x1 - cut + 1 + i, y0), (x1, y0 + i)], fill=(0, 0, 0, 0))
    c.line([(x1 - cut, y0), (x1, y0 + cut)], fill=LINE)
    return im

full = add_card(img.copy())
head = img.crop((56 * S, 6 * S, 248 * S, 198 * S))
images = [add_badge(head.resize((n, n), Image.LANCZOS)) if n <= 24 else full.resize((n, n), Image.LANCZOS)
          for n in (16, 20, 24, 32, 40, 48, 64, 256)]

args = sys.argv[1:]
preview = args[args.index('--preview') + 1] if '--preview' in args else None
args = [a for a in args if a != '--preview' and a != preview]
out = args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dd.ico')
images[-1].save(out, format='ICO', sizes=[im.size for im in images], append_images=images[:-1])
print(out)

if preview:   # every size, on white, and the small ones on dark too
    sheet = Image.new('RGBA', (sum(im.width + 12 for im in images) + 12, 320), (255, 255, 255, 255))
    sheet.paste((32, 32, 32, 255), (0, 276, sheet.width, 320))
    x = 12
    for im in reversed(images):
        sheet.alpha_composite(im, (x, 12))
        if im.width <= 40:
            sheet.alpha_composite(im, (x, 278))
        x += im.width + 12
    sheet.save(preview)
