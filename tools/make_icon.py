"""Draws the rng-lens icon (a lens with an aperture ring) at any size."""
import sys
from PIL import Image, ImageDraw, ImageFilter

def icon(size):
    s = size * 4  # supersample
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = s // 5
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=r, fill=(14, 14, 18, 255))
    c = s / 2
    def ring(rad, width, color):
        d.ellipse([c - rad, c - rad, c + rad, c + rad], outline=color, width=width)
    def disc(rad, color):
        d.ellipse([c - rad, c - rad, c + rad, c + rad], fill=color)
    disc(s * 0.36, (30, 30, 40, 255))
    ring(s * 0.36, int(s * 0.035), (255, 96, 64, 255))      # outer accent ring
    disc(s * 0.25, (8, 8, 12, 255))
    ring(s * 0.25, int(s * 0.02), (70, 70, 90, 255))
    disc(s * 0.13, (40, 120, 255, 255))                      # iris
    disc(s * 0.07, (5, 5, 8, 255))                           # pupil
    hl = s * 0.035
    hx, hy = c - s * 0.085, c - s * 0.085
    d.ellipse([hx - hl, hy - hl, hx + hl, hy + hl], fill=(255, 255, 255, 220))
    return img.resize((size, size), Image.LANCZOS)

if __name__ == "__main__":
    out = sys.argv[1]
    if out.endswith(".ico"):
        icon(256).save(out, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    else:
        icon(int(sys.argv[2]) if len(sys.argv) > 2 else 192).save(out)
