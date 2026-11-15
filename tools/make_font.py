#!/usr/bin/env python3
"""Rasterises a free font (Barlow Condensed, SIL OFL 1.1) into Eden Duo's MFNT metrics + a PNG atlas.

MFNT (the format Eden Duo's core parses as a fallback, engine_mercury.cpp ParseMfnt):
  "MFNT" magic, u32 line_height at 0x1C, u32 glyph count at 0x20, u32 first-record offset at
  0x28; 14-byte records {u16 x, u16 y, u16 w, u16 h, s16 bearing_x, s16 bearing_y, u16 advance}.
  Record 0 is notdef, record 1 is U+0020 and the rest follow in codepoint order.
The runtime draws text 'scale * 5' pixels tall, mapping that to line_height, with the baseline
at y + scale*5 and glyph tops bearing_y above it. So line_height = the cap height here, and a
text_scale of 6 gives 30-pixel capitals. Coverage lives in the atlas alpha; the widget colour is
painted through it.
Usage: make_font.py font.ttf variation out_prefix   (writes out_prefix.mfnt and out_prefix.png)
"""
import struct
import sys
from PIL import Image, ImageDraw, ImageFont

FIRST, LAST = 0x20, 0xFF  # ASCII + Latin-1 (á é í ó ú ñ ü ¿ ¡ ...)
SIZE = 72                 # render size in px; capitals come out ~50 px


def main(ttf, variation, out):
    font = ImageFont.truetype(ttf, SIZE)
    if variation:
        font.set_variation_by_name(variation)
    cap = -font.getbbox("H", anchor="ls")[1]
    cells = []
    for cp in range(FIRST, LAST + 1):
        ch = chr(cp)
        adv = int(round(font.getlength(ch)))
        if cp < 0xA0 and cp > 0x7E or cp == 0xAD:  # C1 controls / soft hyphen: blank
            cells.append((None, 0, 0, adv if cp == 0xA0 else 0))
            continue
        x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
        if x1 <= x0 or y1 <= y0:
            cells.append((None, 0, 0, adv))
            continue
        img = Image.new("L", (x1 - x0 + 2, y1 - y0 + 2), 0)
        ImageDraw.Draw(img).text((1 - x0, 1 - y0), ch, font=font, fill=255, anchor="ls")
        cells.append((img, x0 - 1, -(y0 - 1), adv))

    # Shelf-pack into a 1024-wide atlas.
    W, x, y, row_h, placed = 1024, 0, 0, 0, []
    for img, bx, by, adv in cells:
        if img is None:
            placed.append((0, 0, 0, 0, bx, by, adv))
            continue
        w, h = img.size
        if x + w > W:
            x, y, row_h = 0, y + row_h + 1, 0
        placed.append((x, y, w, h, bx, by, adv))
        x += w + 1
        row_h = max(row_h, h)
    H = y + row_h + 1
    atlas = Image.new("RGBA", (W, H), (255, 255, 255, 0))
    for (img, *_), (px, py, w, h, *_rest) in zip(cells, placed):
        if img is not None:
            atlas.paste(Image.merge("RGBA", (img.point(lambda _: 255),) * 3 + (img,)), (px, py))
    atlas.save(out + ".png", optimize=True)

    count = 1 + len(placed)
    first = 0x30
    hdr = bytearray(first)
    hdr[0:4] = b"MFNT"
    struct.pack_into("<I", hdr, 0x1C, cap)
    struct.pack_into("<I", hdr, 0x20, count)
    struct.pack_into("<I", hdr, 0x28, first)
    recs = bytearray(14)  # notdef
    for px, py, w, h, bx, by, adv in placed:
        recs += struct.pack("<HHHHhhH", px, py, w, h, bx, by, adv)
    data = bytes(hdr) + bytes(recs)
    data += b"\0" * max(0, 0x60 - len(data))
    open(out + ".mfnt", "wb").write(data)
    print(f"{out}: {count} glyphs, cap {cap}px, atlas {W}x{H}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "", sys.argv[3] if len(sys.argv) > 3 else "font")
