"""Builds the launcher's images from the approved REFLORA art in launcher/art/ (run once when the art changes).

  python launcher/tools/make_assets.py

Inputs (approved by the project owner, 2026-11-11):
  art/reflora-logo-light.webp   the logo on a white background (source of the transparent logo)
  art/reflora-scene.webp        the floral scene with the logo, used as the home backdrop
Outputs:
  src/assets/reflora-logo.webp      transparent logo, full size (hero/about)
  src/assets/reflora-logo-sm.webp   transparent logo, 560 px wide (sidebar)
  src/assets/reflora-scene.webp     the scene, unchanged
  src-tauri/icons/                  icon.ico (16-256), icon.png, 32x32.png, 128x128.png, 128x128@2x.png

The transparent logo: the background is the near-white region connected to the image border; the anti-aliased
rim (2 px) is un-mixed from white against the logo's dark outline colour. Needs Pillow.
"""
import os
import shutil
from collections import deque

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ART = os.path.join(ROOT, "art")
ASSETS = os.path.join(ROOT, "src", "assets")
ICONS = os.path.join(ROOT, "src-tauri", "icons")
OUTLINE = (24, 24, 58)  # the logo's dark outline


def whiteish(c):
    return min(c) >= 232 and max(c) - min(c) <= 18


def transparent_logo(src):
    im = Image.open(src).convert("RGB")
    w, h = im.size
    px = im.load()
    bg = bytearray(w * h)
    q = deque()
    for x in range(w):
        for y in (0, h - 1):
            q.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            q.append((x, y))
    while q:  # flood fill of the near-white background from the border
        x, y = q.popleft()
        i = y * w + x
        if bg[i] or not whiteish(px[x, y]):
            continue
        bg[i] = 1
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and not bg[ny * w + nx]:
                q.append((nx, ny))
    mask = Image.frombytes("L", (w, h), bytes(255 if b else 0 for b in bg))
    rim = mask.filter(ImageFilter.MaxFilter(5)).load()
    out = Image.new("RGBA", (w, h))
    op = out.load()
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if bg[y * w + x]:
                op[x, y] = (0, 0, 0, 0)
                continue
            if rim[x, y]:
                a = max(0.0, min(1.0, max((255 - c) / (255 - o) for c, o in zip((r, g, b), OUTLINE))))
                if a <= 0:
                    op[x, y] = (0, 0, 0, 0)
                    continue
                r, g, b = (int(max(0, min(255, (c - 255 * (1 - a)) / a))) for c in (r, g, b))
                op[x, y] = (r, g, b, int(round(a * 255)))
            else:
                op[x, y] = (r, g, b, 255)
    return out.crop(out.getbbox())


def only_largest_shape(im):
    """Drops what is not connected to the largest opaque shape (the cut-off sparkles beside the R)."""
    w, h = im.size
    a = im.split()[3].load()
    seen = bytearray(w * h)
    best = []
    for y0 in range(h):
        for x0 in range(w):
            if seen[y0 * w + x0] or a[x0, y0] < 8:
                continue
            comp, q = [], deque([(x0, y0)])
            seen[y0 * w + x0] = 1
            while q:
                x, y = q.popleft()
                comp.append((x, y))
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if 0 <= nx < w and 0 <= ny < h and not seen[ny * w + nx] and a[nx, ny] >= 8:
                        seen[ny * w + nx] = 1
                        q.append((nx, ny))
            if len(comp) > len(best):
                best = comp
    keep = Image.new("L", (w, h), 0)
    kp = keep.load()
    for x, y in best:
        kp[x, y] = 255
    keep = keep.filter(ImageFilter.MaxFilter(3))  # keep the soft rim around it
    out = im.copy()
    out.putalpha(Image.composite(im.split()[3], Image.new("L", (w, h), 0), keep))
    return out.crop(out.getbbox())


def icon_image(logo, size):
    """The logo's R on a rounded navy tile (the colour of the logo's dark art)."""
    s = 1024
    tile = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(tile).rounded_rectangle((24, 24, s - 24, s - 24), radius=220, fill=(10, 16, 64, 255))
    r = only_largest_shape(logo.crop((40, 96, 352, 540)))  # R with its outline, cut inside the one shared with E
    scale = 820 / r.size[1]
    r = r.resize((int(r.size[0] * scale), 820), Image.LANCZOS)
    tile.alpha_composite(r, ((s - r.size[0]) // 2, (s - r.size[1]) // 2 + 10))
    return tile.resize((size, size), Image.LANCZOS)


def main():
    os.makedirs(ASSETS, exist_ok=True)
    os.makedirs(ICONS, exist_ok=True)
    logo = transparent_logo(os.path.join(ART, "reflora-logo-light.webp"))
    logo.save(os.path.join(ASSETS, "reflora-logo.webp"), quality=92, method=6)
    sm = logo.resize((560, round(logo.size[1] * 560 / logo.size[0])), Image.LANCZOS)
    sm.save(os.path.join(ASSETS, "reflora-logo-sm.webp"), quality=92, method=6)
    shutil.copyfile(os.path.join(ART, "reflora-scene.webp"), os.path.join(ASSETS, "reflora-scene.webp"))
    big = icon_image(logo, 512)
    big.save(os.path.join(ICONS, "icon.png"))
    for n, sz in (("32x32.png", 32), ("128x128.png", 128), ("128x128@2x.png", 256)):
        icon_image(logo, sz).save(os.path.join(ICONS, n))
    icon_image(logo, 256).save(os.path.join(ICONS, "icon.ico"),
                               sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("logo", logo.size, "->", ASSETS)


if __name__ == "__main__":
    main()
