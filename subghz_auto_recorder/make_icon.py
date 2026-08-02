"""Generate icon.png (10x10, 1-bit) for the SubGHz Auto Recorder FAP.

Design: a vertical antenna mast on the left with three waves radiating right,
i.e. universal_card_reader's card silhouette replaced by a mast (this app
listens on the air rather than reading a chip). Black pixels are drawn, white
is background.

Run:  python make_icon.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).with_name("icon.png")

img = Image.new("1", (10, 10), 1)  # 1 = white background
d = ImageDraw.Draw(img)

# Antenna mast: a 2px-wide vertical pole with a small base, on the left half.
d.rectangle([1, 1, 2, 8], fill=0)
d.rectangle([0, 8, 3, 9], fill=0)

# Three waves radiating right, drawn as arcs centred on the mast top.
d.arc([2, 0, 6, 5], -60, 60, fill=0)
d.arc([1, -2, 8, 7], -55, 55, fill=0)
d.arc([0, -4, 10, 9], -45, 45, fill=0)

img = img.convert("1")
img.save(OUT, bits=1, optimize=True)
print(f"wrote {OUT} ({img.size[0]}x{img.size[1]}, mode={img.mode})")
