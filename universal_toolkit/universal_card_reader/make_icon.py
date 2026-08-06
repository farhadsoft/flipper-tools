"""Generate icon.png (10x10, 1-bit) for the Universal Card Reader FAP.

Design: a small card silhouette on the left with three RFID/NFC waves
radiating to the right. Black pixels are drawn, white is background.

Run:  python make_icon.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).with_name("icon.png")

img = Image.new("1", (10, 10), 1)  # 1 = white background
d = ImageDraw.Draw(img)

# Card body with a chip, occupying the left half.
d.rectangle([0, 2, 4, 9], outline=0)
d.rectangle([2, 4, 3, 5], fill=0)

# Three waves radiating right, drawn as arcs centred on the card edge.
d.arc([3, 3, 7, 8], -60, 60, fill=0)
d.arc([2, 1, 9, 10], -55, 55, fill=0)
d.arc([1, -1, 11, 12], -45, 45, fill=0)

img = img.convert("1")
img.save(OUT, bits=1, optimize=True)
print(f"wrote {OUT} ({img.size[0]}x{img.size[1]}, mode={img.mode})")
