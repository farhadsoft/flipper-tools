"""Generate icon.png (10x10, 1-bit) for the RFID Multi-Reader FAP.

Design: an emitter dot on the left with three RFID waves radiating right.
Black pixels are drawn, white is background.

Run:  python make_icon.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).with_name("icon.png")

img = Image.new("1", (10, 10), 1)  # 1 = white background
d = ImageDraw.Draw(img)

# Emitter: a 2x2 block on the left edge, vertically centred.
d.rectangle([0, 4, 1, 5], fill=0)

# Three waves radiating right, arcs centred on the emitter.
d.arc([-1, 2, 4, 7], -70, 70, fill=0)
d.arc([-2, 0, 7, 9], -60, 60, fill=0)
d.arc([-3, -2, 10, 11], -50, 50, fill=0)

img = img.convert("1")
img.save(OUT, bits=1, optimize=True)
print(f"wrote {OUT} ({img.size[0]}x{img.size[1]}, mode={img.mode})")
