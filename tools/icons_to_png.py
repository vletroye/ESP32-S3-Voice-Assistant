"""
The 20 x 21 icons of the screen (main/app/images/ui_img_reply_*.c, LV_IMG_CF_TRUE_COLOR_ALPHA: 16-bit colour with
its bytes swapped, then an alpha byte) written as PNG files in docs/icons/, shown by the README.

  python tools/icons_to_png.py
"""

import glob
import os
import re
import struct
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def png(path, width, height, rgba):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    rows = b"".join(b"\x00" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(rows, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    out_dir = os.path.join(ROOT, "docs", "icons")
    os.makedirs(out_dir, exist_ok=True)
    for path in sorted(glob.glob(os.path.join(ROOT, "main", "app", "images", "ui_img_reply_*.c"))):
        text = open(path, encoding="utf-8").read()
        width = int(re.search(r"\.header\.w\s*=\s*(\d+)", text).group(1))
        height = int(re.search(r"\.header\.h\s*=\s*(\d+)", text).group(1))
        data = bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", text.split("_data[] = {")[1].split("};")[0]))
        rgba = bytearray()
        for i in range(width * height):
            value = (data[3 * i] << 8) | data[3 * i + 1]     # bytes swapped
            r, g, b = (value >> 11) & 0x1F, (value >> 5) & 0x3F, value & 0x1F
            rgba += bytes((r * 255 // 31, g * 255 // 63, b * 255 // 31, data[3 * i + 2]))
        name = os.path.basename(path)[len("ui_img_reply_"):-2]
        png(os.path.join(out_dir, name + ".png"), width, height, bytes(rgba))
        print(name)


if __name__ == "__main__":
    main()
