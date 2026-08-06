"""Generate icon.png (10x10, 1-bit) for the iButton Brute Force FAP.

Design: a small iButton key (round head + short shaft).
Run:  python make_icon.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).with_name("icon.png")

img = Image.new("1", (10, 10), 1)  # white background
d = ImageDraw.Draw(img)

# Key head: small circle near the top.
d.ellipse([1, 0, 5, 4], outline=0)
# Shaft.
d.line([3, 4, 3, 9], fill=0)
# Bit on the shaft.
d.line([3, 7, 6, 7], fill=0)
d.line([3, 9, 5, 9], fill=0)

img = img.convert("1")
img.save(OUT, bits=1, optimize=True)
print(f"wrote {OUT} ({img.size[0]}x{img.size[1]}, mode={img.mode})")
