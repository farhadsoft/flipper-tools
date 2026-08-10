#!/usr/bin/env python3
"""Decode Flipper 1bpp framebuffer captures to 128x64 PNGs.

The framebuffer from rpc_gui_snapshot_screen() is 1024 bytes, page-tiled
(u8g2/SSD1306 style). For each pixel (x, y):

    byte = data[(y // 8) * 128 + x]
    bit  = (byte >> (y % 8)) & 1

Usage:
    python decode_screens.py logs/*.raw --out logs/screens
"""
import argparse, os, sys
from PIL import Image

WIDTH, HEIGHT = 128, 64


def decode_page_tiled(data):
    if len(data) != 1024:
        raise ValueError(f"expected 1024 bytes, got {len(data)}")
    img = Image.new("1", (WIDTH, HEIGHT), 0)
    pixels = img.load()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            byte = data[(y // 8) * WIDTH + x]
            bit = (byte >> (y % 8)) & 1
            pixels[x, y] = 1 if bit else 0
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+", help="raw 1024-byte framebuffer files")
    ap.add_argument("--out", default="logs/screens", help="output directory")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    for path in args.files:
        with open(path, "rb") as f:
            data = f.read()
        img = decode_page_tiled(data)
        out_name = os.path.splitext(os.path.basename(path))[0] + ".png"
        out_path = os.path.join(args.out, out_name)
        img.save(out_path)
        print(f"{path} -> {out_path}")


if __name__ == "__main__":
    main()
