"""Generate icon.png (10x10, 1-bit) for the Universal Toolkit launcher FAP.

Design: a small toolbox (body + handle), since this app is a launcher shell
for multiple tool modules, not any one radio or protocol.

Run:  python make_icon.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).with_name("icon.png")

img = Image.new("1", (10, 10), 1)  # 1 = white background
d = ImageDraw.Draw(img)

# Toolbox body.
d.rectangle([0, 4, 9, 9], outline=0)
# Handle.
d.arc([2, 0, 7, 6], 180, 360, fill=0)
# Clasp on the front face.
d.line([4, 4, 4, 6], fill=0)
d.line([5, 4, 5, 6], fill=0)

img = img.convert("1")
img.save(OUT, bits=1, optimize=True)
print(f"wrote {OUT} ({img.size[0]}x{img.size[1]}, mode={img.mode})")
