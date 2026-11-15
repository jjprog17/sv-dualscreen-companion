#!/usr/bin/env python3
"""Approximate preview of a companion page. Draws manifest.json the way Eden Duo's runtime does
(images with src / src_names / src_format, bars, rects, labels and values with the package's
MFNT font and outline, repeat templates, need_bind / hide_bind gates) over a snapshot dumped by
native/test_host (SV_DUMP). Not the real renderer: good enough to check layout and text fit.
Usage: preview.py <dualscreen dir> <page id> <snapshot.tsv> <user dir> <out.png>
"""
import json
import os
import struct
import sys
from PIL import Image, ImageDraw


def col(s):
    v = int(str(s).lstrip("#"), 16)
    if len(str(s).lstrip("#")) <= 6:
        v |= 0xFF000000
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255, (v >> 24) & 255)


class Font:
    def __init__(self, mfnt, atlas):
        d = open(mfnt, "rb").read()
        self.line = struct.unpack_from("<I", d, 0x1C)[0]
        n, first = struct.unpack_from("<I", d, 0x20)[0], struct.unpack_from("<I", d, 0x28)[0]
        self.g = {}
        for i in range(1, n):
            self.g[0x1F + i] = struct.unpack_from("<HHHHhhH", d, first + i * 14)
        self.atlas = Image.open(atlas).convert("RGBA")
        self.cache = {}

    def width(self, s, scale):
        r = scale * 5 / self.line
        return sum((self.g[ord(c)][6] if ord(c) in self.g else self.line * 0.5) * r for c in s)

    def draw(self, img, s, x, y, scale, color, outline=None, opx=0):
        wanted = scale * 5
        r = wanted / self.line
        base = y + wanted
        for pass_ in ([0, 1] if outline and opx else [1]):
            pen = x
            for c in s:
                g = self.g.get(ord(c))
                if not g:
                    pen += wanted * 0.5
                    continue
                gx_, gy_, w, h, bx, by, adv = g
                if w and h:
                    gw, gh = max(1, int(w * r)), max(1, int(h * r))
                    key = (ord(c), gw, gh)
                    if key not in self.cache:
                        self.cache[key] = self.atlas.crop((gx_, gy_, gx_ + w, gy_ + h)).resize((gw, gh), Image.LANCZOS).getchannel("A")
                    mask = self.cache[key]
                    px, py = int(pen + bx * r), int(base - by * r)
                    if pass_ == 0:
                        solid = Image.new("RGBA", (gw, gh), col(outline))
                        for k in range(16):
                            import math
                            a = k * math.pi / 8
                            img.paste(solid, (px + round(opx * math.cos(a)), py + round(opx * math.sin(a))), mask)
                    else:
                        img.paste(Image.new("RGBA", (gw, gh), col(color)), (px, py), mask)
                pen += adv * r


def main(ds, page_id, snap_path, user_dir, out):
    m = json.load(open(os.path.join(ds, "manifest.json")))
    snap = {}
    for line in open(snap_path, encoding="utf-8"):
        kind, k, v = line.rstrip("\n").split("\t", 2)
        snap[k] = v if kind == "T" else int(v)
    for k, v in m.get("flags", {}).items():
        snap.setdefault("@flag:" + k, v)
    for k, v in os.environ.items():  # PREVIEW_FLAG_sel=2 sets @flag:sel
        if k.startswith("PREVIEW_FLAG_"):
            snap["@flag:" + k[len("PREVIEW_FLAG_"):]] = int(v)
    for d in m.get("derived", []):  # the forms this package uses, in manifest order
        if "cmp" in d and d["cmp"] == "eq":
            a, b = d["a"], d["b"]
            av = snap.get(a, 0) if isinstance(a, str) else a
            bv = snap.get(b, 0) if isinstance(b, str) else b
            snap[d["name"]] = int(av == bv)
        elif "expr" in d:  # the subset this package uses: names, numbers, == != && || !
            import re
            e = d["expr"].replace("&&", " and ").replace("||", " or ")
            e = re.sub(r"!(?!=)", " not ", e)
            num_of = lambda k: snap.get(k, 0) if isinstance(snap.get(k, 0), int) else 0
            e = re.sub(r"'([^']*)'|[A-Za-z_@][A-Za-z0-9_.:@]*", lambda mm: repr(num_of(mm.group(1))) if mm.group(1)
                       is not None else mm.group(0) if mm.group(0) in ("and", "or", "not") else repr(num_of(mm.group(0))), e)
            snap[d["name"]] = int(bool(eval(e)))
        elif "all_nonzero" in d:
            snap[d["name"]] = int(all(snap.get(n, 0) for n in d["all_nonzero"]))
    font = Font(os.path.join(ds, m["font"][5:]), os.path.join(ds, m["font_atlas"][5:]))
    img = Image.new("RGBA", (m["canvas_w"], m["canvas_h"]), col(m.get("background", "#FF000000")))
    d = ImageDraw.Draw(img)
    page = next(p for p in m["pages"] if p["id"] == page_id)
    cache, problems = {}, []

    def load(src):
        if src in cache:
            return cache[src]
        path = None
        if src.startswith("file:"):
            path = os.path.join(ds, src[5:])
        elif src.startswith("user:"):
            path = os.path.join(user_dir, src[5:])
        im = Image.open(path).convert("RGBA") if path and os.path.exists(path) else None
        if src.startswith("module:item/"):  # the module decodes the game's icon at run time: stand-in
            im = Image.open(os.path.join(ds, "art/pball.png")).convert("RGBA")
        if src.startswith("module:map/") and os.environ.get("PREVIEW_MAP"):  # a stand-in map picture
            im = Image.open(os.environ["PREVIEW_MAP"]).convert("RGBA")
            code = int(src[11:]); zl = (code >> 2) & 3
            if (code >> 23) & 1:  # heading up: round picture turned like map_view
                import math
                z = 1 << zl; cells = 16 * z; tx, ty = (code >> 4) & 255, (code >> 12) & 255
                hb = ((code >> 24) & 0x7F) * 5
                cx, cy = tx * im.width / cells, ty * im.height / cells; r = im.width / (2 * z)
                O = 768; half = O / 2; sc = r / half
                c, sn = math.cos(math.radians(hb)), math.sin(math.radians(hb))
                out = Image.new("RGBA", (O, O), (0, 0, 0, 0)); px = out.load(); src_px = im.load()
                for oy in range(0, O):
                    for ox in range(0, O):
                        dx, dy = ox - half + .5, oy - half + .5
                        if dx * dx + dy * dy > half * half: continue
                        mx, my = cx + (dx * c - dy * sn) * sc, cy + (dx * sn + dy * c) * sc
                        px[ox, oy] = src_px[int(mx), int(my)] if 0 <= mx < im.width and 0 <= my < im.height else (60, 120, 200, 255)
                im = out
                zl = 0
            if zl:  # the module's crop for the zoom (same grid maths)
                z = 1 << zl; cells = 8 * z; tx, ty = (code >> 4) & 255, (code >> 12) & 255
                cw, ch = im.width // z, im.height // z
                x0 = min(max(tx * im.width // cells - cw // 2, 0), im.width - cw)
                y0 = min(max(ty * im.height // cells - ch // 2, 0), im.height - ch)
                im = im.crop((x0, y0, x0 + cw, y0 + ch))
        cache[src] = im
        return im

    for w in page["widgets"]:
        for i in range(w.get("repeat", 1)):
            sub = (lambda s: s.replace("{i}", str(i))) if "repeat" in w else (lambda s: s)
            num = lambda k: snap.get(sub(k), 0) if isinstance(snap.get(sub(k), 0), int) else 0
            if "need_bind" in w and not num(w["need_bind"]):
                continue
            if "hide_bind" in w:
                hv = num(w["hide_bind"])
                if hv < w.get("keep_min", -(1 << 62)) or hv > w.get("keep_max", 1 << 62):
                    continue
            x, y, rw, rh = w["rect"]
            if "repeat" in w:
                if w.get("repeat_cols"):
                    x += (i % w["repeat_cols"]) * w.get("repeat_dx", 0)
                    y += (i // w["repeat_cols"]) * w.get("repeat_row_dy", 0)
                else:
                    x += i * w.get("repeat_dx", 0)
                    y += i * w.get("repeat_dy", 0)
            if "x_bind" in w:
                x += int(num(w["x_bind"]) * w.get("x_scale", 1))
            if "y_bind" in w:
                y += int(num(w["y_bind"]) * w.get("y_scale", 1))
            t = w.get("type", "label")
            if t == "rect":
                if "bg" in w:
                    d.rectangle([x, y, x + rw - 1, y + rh - 1], fill=col(w["bg"]))
            elif t == "image":
                src = w.get("src")
                if "src_names" in w:
                    v = num(w["bind"])
                    src = w["src_names"][v] if 0 <= v < len(w["src_names"]) else src
                if "src_format" in w:
                    src = w["src_format"] % num(w["bind"])
                im = load(src) if src else None
                if im is not None:
                    im2 = im.resize((rw, rh), Image.LANCZOS)
                    if "rotate_bind" in w:  # clockwise degrees about the centre
                        im2 = im2.rotate(-num(w["rotate_bind"]), resample=Image.BICUBIC)
                    img.alpha_composite(im2, (x, y))
            elif t == "bar":
                d.rectangle([x, y, x + rw - 1, y + rh - 1], fill=col(w.get("bg", "#FF000000")))
                v = num(w["bind"])
                mx = num(w["max_bind"]) if "max_bind" in w else w.get("max", 1)
                if mx > 0 and v > 0:
                    d.rectangle([x, y, x + int(rw * min(1, v / mx)) - 1, y + rh - 1], fill=col(w["color"]))
            elif t in ("label", "value"):
                if t == "label":
                    s = snap.get(sub(w["bind_text"]), "") if "bind_text" in w else w.get("text", "")
                    s = s if isinstance(s, str) else str(s)
                else:
                    s = str(num(w["bind"])).zfill(w.get("pad", 0))
                    if w.get("names"):  # value -> name (the host's "names", with "add")
                        nv = num(w["bind"]) + w.get("add", 0)
                        s = w["names"][nv] if 0 <= nv < len(w["names"]) else s
                    if "max_bind" in w:
                        s += w.get("max_sep", "/") + str(num(w["max_bind"]))
                    s += w.get("suffix", "")
                sc = w.get("text_scale", 3)
                if w.get("wrap_width"):  # greedy word wrap, max_lines, line height = scale*5*1.3
                    lines, cur = [], ""
                    for word in s.split():
                        t = (cur + " " + word).strip()
                        if font.width(t, sc) > w["wrap_width"] and cur:
                            lines.append(cur); cur = word
                        else:
                            cur = t
                    if cur:
                        lines.append(cur)
                    for k, line in enumerate(lines[:w.get("max_lines", 99)]):
                        font.draw(img, line, x, y + int(k * sc * 5 * 1.3), sc, w.get("color", "#FFE6ECF2"),
                                  w.get("outline"), w.get("outline_px", 2))
                    continue
                tw = font.width(s, sc)
                box = w.get("_box_w", rw)
                if box and tw > box + 2:
                    problems.append(f'no cabe: "{s}" ({int(tw)}px en {box}px, escala {sc})')
                al = w.get("align", "left")
                # Eden Duo anchors text at rect x: centred on it, or ending at it when right-aligned.
                tx = x - (tw if al == "right" else tw / 2 if al == "center" else 0)
                font.draw(img, s, tx, y, sc, w.get("color", "#FFE6ECF2"), w.get("outline"), w.get("outline_px", 2))
    img.save(out)
    return problems


if __name__ == "__main__":
    probs = main(*sys.argv[1:6])
    print("\n".join(sorted(set(probs))) if probs else "todo el texto cabe")
