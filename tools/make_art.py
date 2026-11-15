#!/usr/bin/env python3
"""Draws the companion's own UI art (no game assets). v0.8 look ("D"): dark night background,
slanted panels and tabs like a sports broadcast, the game's colour as the accent (Scarlet: red
and orange, Violet: violet and lilac), and colours that mean something in the matchups (red: it
hurts it, green: it holds out, blue: no effect). Everything is drawn from code at 4x and
downsampled for smooth edges.
Usage: make_art.py out_dir [scarlet|violet] [light]
"""
import math
import os
import sys
from PIL import Image, ImageDraw

SS = 4  # supersampling

# Type colours in the game's type order (PKHeX/SV ids 0..18).
TYPES = [
    ("normal", "#8E8F95"), ("fighting", "#E06A12"), ("flying", "#6E9FD6"), ("poison", "#8E3FC4"),
    ("ground", "#9A5A24"), ("rock", "#9C9468"), ("bug", "#7F9318"), ("ghost", "#6A3F78"),
    ("steel", "#4D8AA0"), ("fire", "#D8372A"), ("water", "#2C78DE"), ("grass", "#3B8A26"),
    ("electric", "#D9A400"), ("psychic", "#D63A6A"), ("ice", "#2FA9C8"), ("dragon", "#4B57CF"),
    ("dark", "#624D4E"), ("fairy", "#C95CC9"), ("stellar", "#2E9C8E"),
]

BG, PANEL, PANEL2, DIM = "#15131C", "#221E2C", "#2E2939", "#1D1A26"
CREAM, MUTED = "#F6EFE6", "#A79FB5"
THEMES = {
    # "D Escarlata": wine instead of night blue, gold of the Academy, Paldea tiles on the bands,
    # Tera crystals in the corners
    "scarlet": {"acc": "#E8452C", "acc2": "#FF8A1F", "band": "#2A141B", "bg": "#1C0E13", "panel": "#2C171D",
                "panel2": "#3A1F26", "dim": "#24131A", "cream": "#FFF1E4", "muted": "#C4A7A9", "gold": "#F2B544",
                "tiles": True, "crystals": True},
    "violet": {"acc": "#8B4DFF", "acc2": "#C9A2FF", "band": "#1E1838", "bg": BG, "panel": PANEL, "panel2": PANEL2,
               "dim": DIM, "cream": CREAM, "muted": MUTED, "gold": None, "tiles": False, "crystals": False},
}
# v0.9: a light theme of the same design (chosen in the companion: Ficha > CAMBIAR TEMA). Same
# shapes and accents; cream page, white panels, dark ink, pale matchup tiles.
LIGHT = {
    "scarlet": {"acc": "#E8452C", "acc2": "#E8701A", "band": "#EEDFD2", "bg": "#F7EFE6", "panel": "#FFFFFF",
                "panel2": "#F1E6DA", "dim": "#E6DCD2", "cream": "#FFF8F1", "muted": "#7A6468", "gold": "#C98A10",
                "tiles": True, "crystals": True},
    "violet": {"acc": "#8B4DFF", "acc2": "#7A4CD6", "band": "#E6E0F2", "bg": "#F4F1F8", "panel": "#FFFFFF",
               "panel2": "#ECE7F4", "dim": "#E2DCEC", "cream": "#FBF8FF", "muted": "#6E6680", "gold": None,
               "tiles": False, "crystals": False},
}
GOLD = None
TILES = CRYSTALS = False
INKA, KO, ZOFF, SELFILL, SELCARD = "#F6EFE6", "#3A3446", "#5A5468", "#3A2230", None


def set_theme(th, light=False):
    global BG, PANEL, PANEL2, DIM, CREAM, MUTED, GOLD, TILES, CRYSTALS, INKA, KO, ZOFF, SELFILL, SELCARD
    global HURT, HURT2, HOLD, HOLD2, NONE
    BG, PANEL, PANEL2, DIM = th["bg"], th["panel"], th["panel2"], th["dim"]
    CREAM, MUTED, GOLD = th["cream"], th["muted"], th["gold"]
    TILES, CRYSTALS = th["tiles"], th["crystals"]
    if light:
        INKA, KO, ZOFF, SELFILL, SELCARD = "#2A1A1E", "#C9BDB3", "#B8ADA4", "#FCE3DA", "#FDE4DC"
        HURT, HURT2 = ("#FBE1DD", "#E0503F"), ("#FDEDEA", "#E07A6E")
        HOLD, HOLD2 = ("#DFF3E6", "#2E9A58"), ("#EDF8F1", "#5DBB84")
        NONE = ("#DDF1F7", "#2A9CC0")
    else:
        INKA, KO, ZOFF, SELFILL, SELCARD = th["cream"], "#3A3446", "#5A5468", "#3A2230", None
        HURT, HURT2 = ("#4A1A1E", "#FF6B5A"), ("#3A1A20", "#C2554A")
        HOLD, HOLD2 = ("#16302A", "#4ED07A"), ("#10261F", "#2F9A58")
        NONE = ("#13293A", "#6FE7FF")
# matchup meanings: fill, edge
HURT = ("#4A1A1E", "#FF6B5A")
HURT2 = ("#3A1A20", "#C2554A")
HOLD = ("#16302A", "#4ED07A")
HOLD2 = ("#10261F", "#2F9A58")
NONE = ("#13293A", "#6FE7FF")


def rgb(h, a=255):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)


def mix(c1, c2, t):
    return tuple(int(round(a + (b - a) * t)) for a, b in zip(c1, c2))


def canvas(w, h):
    return Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))


def done(img, path):
    w, h = img.size
    img.resize((w // SS, h // SS), Image.LANCZOS).save(path, optimize=True)


def P(pts):
    return [(x * SS, y * SS) for x, y in pts]


def para(w, h, fill, slant=None, left=True, right=True):
    """A slanted panel: the right edge leans (and the left one, if `left`)."""
    s = slant if slant is not None else max(6, h // 6)
    c = canvas(w, h)
    d = ImageDraw.Draw(c)
    pts = [(s if left else 0, 0), (w, 0), (w - s if right else w, h), (0, h)]
    d.polygon(P(pts), fill=rgb(fill) if isinstance(fill, str) else fill)
    return c, d


def save_para(out, w, h, fill, **kw):
    c, _ = para(w, h, fill, **kw)
    done(c, out)


def cut_panel(out, w, h, fill=None, cut=36, edge=None, top=None, top_h=0):
    """Flat panel with its bottom-right corner cut; optional coloured top band."""
    fill = fill or PANEL
    c = canvas(w, h)
    d = ImageDraw.Draw(c)
    pts = [(0, 0), (w, 0), (w, h - cut), (w - cut, h), (0, h)]
    d.polygon(P(pts), fill=rgb(fill))
    if top:
        d.polygon(P([(0, 0), (w, 0), (w, top_h), (0, top_h)]), fill=rgb(top))
    if edge:
        d.line(P(pts + [pts[0]]), fill=rgb(edge), width=3 * SS)
    done(c, out)


def azulejos(w, h, color, alpha=46, cell=40):
    """Paldea tiles: rings round every corner of a 40 px grid and a dot in each cell (a
    quatrefoil look), faint gold."""
    t = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(t)
    col = rgb(color, alpha)
    r, k = 14, 2.2
    for gy in range(0, h + cell, cell):
        for gx in range(0, w + cell, cell):
            d.ellipse([(gx - r) * SS, (gy - r) * SS, (gx + r) * SS, (gy + r) * SS], outline=col, width=int(k * SS))
            cx, cy = gx + cell / 2, gy + cell / 2
            d.ellipse([(cx - 4) * SS, (cy - 4) * SS, (cx + 4) * SS, (cy + 4) * SS], fill=col)
    return t


def crystal(d, pts, color, alpha):
    d.polygon(P(pts), fill=rgb(color, alpha))


def background(out, band, w=1240, h=1080, crystals=()):
    c = Image.new("RGBA", (w * SS, h * SS), rgb(BG))
    d = ImageDraw.Draw(c)
    # two diagonal bands, as in the mock-ups (Scarlet: with tiles)
    mask = Image.new("L", c.size, 0)
    md = ImageDraw.Draw(mask)
    for x0, bw in ((700, 260), (1000, 70)):
        dx = int(math.tan(math.radians(18)) * h)
        poly = P([(x0 + dx, 0), (x0 + dx + bw, 0), (x0 + bw, h), (x0, h)])
        d.polygon(poly, fill=rgb(band))
        md.polygon(poly, fill=255)
    if TILES:
        tiles = azulejos(w, h, GOLD)
        c.paste(tiles, (0, 0), Image.composite(tiles, Image.new("RGBA", c.size, (0, 0, 0, 0)), mask))
        d = ImageDraw.Draw(c)
    if CRYSTALS:
        for pts, col, a in crystals:
            crystal(d, pts, col, a)
    done(c, out)


def shard(x, y, sx, sy, kind=0):
    shapes = [[(.5, 0), (1, .35), (.7, 1), (.1, .85), (0, .3)], [(.5, 0), (1, .4), (.6, 1), (0, .6)],
              [(.4, 0), (1, .3), (.8, 1), (0, .7)]]
    return [(x + px * sx, y + py * sy) for px, py in shapes[kind]]


def splash(out, acc, acc2, band, w=1240, h=1080):
    """Start screen (Violet; Scarlet uses the player's own cover art): dark with the accent bands."""
    c = Image.new("RGBA", (w * SS, h * SS), rgb(BG))
    d = ImageDraw.Draw(c)
    for x0, bw, col in ((640, 340, band), (1020, 90, band), (-200, 1600, None)):
        if col is None:
            continue
        dx = int(math.tan(math.radians(18)) * h)
        d.polygon(P([(x0 + dx, 0), (x0 + dx + bw, 0), (x0 + bw, h), (x0, h)]), fill=rgb(col))
    # a red ribbon across, leaning
    d.polygon(P([(0, 700), (w, 610), (w, 660), (0, 750)]), fill=rgb(acc))
    d.polygon(P([(0, 760), (w, 670), (w, 680), (0, 770)]), fill=rgb(acc2))
    done(c, out)


def party_card(out, fill, stripe, w=560, h=130, bottom=None):
    c, d = para(w, h, fill, slant=h // 6, left=False)
    if bottom:  # a gold edge underneath (the chosen card, Scarlet)
        s = h // 6
        d.polygon(P([(0, h - 5), (w - s * (h - 5) / h, h - 5), (w - s, h), (0, h)]), fill=rgb(bottom))
    d.rectangle([0, 0, 10 * SS, h * SS], fill=rgb(stripe))
    done(c, out)


def sel_frame(out, color, w=576, h=146):
    """Selection mark round the chosen card: an accent edge along the slanted outline."""
    c = canvas(w, h)
    d = ImageDraw.Draw(c)
    s = (h - 16) // 6
    pts = [(2, 2), (w - 2, 2), (w - 2 - s - 4, h - 2), (2, h - 2)]
    d.line(P(pts + [pts[0]]), fill=rgb(color), width=5 * SS, joint="curve")
    done(c, out)


def gem(out, color, w=120, h=136, inner=None):
    inner = inner or BG
    c = canvas(w, h)
    d = ImageDraw.Draw(c)
    pts = [(w / 2, 0), (w, h * .27), (w, h * .73), (w / 2, h), (0, h * .73), (0, h * .27)]
    d.polygon(P(pts), fill=rgb(color))
    k = 5
    pts2 = [(w / 2, k * 1.2), (w - k, h * .27 + k * .5), (w - k, h * .73 - k * .5), (w / 2, h - k * 1.2),
            (k, h * .73 - k * .5), (k, h * .27 + k * .5)]
    d.polygon(P(pts2), fill=rgb(inner))
    done(c, out)


def type_badge(out, color, w=150, h=38):
    save_para(out, w, h, color, slant=8)


def move_card(out, color, w=540, h=104, fill=None):
    """A move row: dark slanted body with a block of the type's colour on the left."""
    fill = fill or PANEL
    c, d = para(w, h, fill, slant=10, left=False)
    d.polygon(P([(0, 0), (16, 0), (16, h), (0, h)]), fill=rgb(color))
    done(c, out)


def category(out, kind, size=46):
    c = canvas(size, size)
    d = ImageDraw.Draw(c)
    s = size * SS
    if kind == 0:  # status: two concentric rings
        d.ellipse([s * .1, s * .1, s * .9, s * .9], fill=rgb("#8C8C9C"))
        d.ellipse([s * .3, s * .3, s * .7, s * .7], fill=rgb(PANEL))
    elif kind == 1:  # physical: burst
        d.ellipse([0, 0, s - 1, s - 1], fill=rgb("#C92B1E"))
        pts = []
        for i in range(16):
            r = s * (0.42 if i % 2 == 0 else 0.2)
            a = i * math.pi / 8 - math.pi / 2
            pts.append((s / 2 + r * math.cos(a), s / 2 + r * math.sin(a)))
        d.polygon(pts, fill=rgb("#FFD24A"))
    else:  # special: rings
        d.ellipse([0, 0, s - 1, s - 1], fill=rgb("#4F5A8C"))
        for r in (0.36, 0.22):
            d.ellipse([s / 2 - s * r, s / 2 - s * r, s / 2 + s * r, s / 2 + s * r],
                      outline=rgb("#DCE2FF"), width=int(s * 0.07))
    done(c, out)


def gender(out, female, size=40):
    c = canvas(size, size)
    d = ImageDraw.Draw(c)
    s = size * SS
    col = rgb("#FF6B8A") if female else rgb("#5CA8FF")
    w = int(s * 0.11)
    if female:
        d.ellipse([s * .28, s * .1, s * .72, s * .54], outline=col, width=w)
        d.line([(s * .5, s * .54), (s * .5, s * .92)], fill=col, width=w)
        d.line([(s * .34, s * .75), (s * .66, s * .75)], fill=col, width=w)
    else:
        d.ellipse([s * .12, s * .38, s * .58, s * .84], outline=col, width=w)
        d.line([(s * .5, s * .46), (s * .84, s * .12)], fill=col, width=w)
        d.line([(s * .54, s * .14), (s * .86, s * .14)], fill=col, width=w)
        d.line([(s * .86, s * .12), (s * .86, s * .44)], fill=col, width=w)
    done(c, out)


def star(out, size=40):
    c = canvas(size, size)
    d = ImageDraw.Draw(c)
    s = size * SS
    pts = []
    for i in range(10):
        r = s * (0.48 if i % 2 == 0 else 0.2)
        a = i * math.pi / 5 - math.pi / 2
        pts.append((s / 2 + r * math.cos(a), s / 2 + r * math.sin(a)))
    d.polygon(pts, fill=rgb("#FFD166"), outline=rgb("#8A6200"), width=2 * SS)
    done(c, out)


def ball_placeholder(out, size=128, col=None):
    """Shown where a Pokémon picture is missing: a faint ball outline."""
    col = col or rgb(INKA, 70)
    c = canvas(size, size)
    d = ImageDraw.Draw(c)
    s = size * SS
    w = int(s * 0.06)
    d.ellipse([s * .12, s * .12, s * .88, s * .88], outline=col, width=w)
    d.line([(s * .12, s / 2), (s * .38, s / 2)], fill=col, width=w)
    d.line([(s * .62, s / 2), (s * .88, s / 2)], fill=col, width=w)
    d.ellipse([s * .38, s * .38, s * .62, s * .62], outline=col, width=w)
    done(c, out)


def header_panel(out, w, h, band, hdr=56, cut=34):
    """Dark sheet with a slanted coloured header band (battle panels, the trainer's licence)."""
    c = canvas(w, h)
    d = ImageDraw.Draw(c)
    pts = [(0, 0), (w, 0), (w, h - cut), (w - cut, h), (0, h)]
    d.polygon(P(pts), fill=rgb(PANEL))
    s = hdr // 2
    d.polygon(P([(0, 0), (w * .62 + s, 0), (w * .62, hdr), (0, hdr)]), fill=rgb(band))
    done(c, out)


def chip(out, w, h, fill=None, edge=None, stripe=None, slant=0):
    fill = fill or PANEL
    c = canvas(w, h)
    d = ImageDraw.Draw(c)
    pts = [(slant, 0), (w, 0), (w - slant, h), (0, h)]
    d.polygon(P(pts), fill=rgb(fill))
    if stripe:
        d.polygon(P([(slant, 0), (slant + 8, 0), (8, h), (0, h)]), fill=rgb(stripe))
    if edge:
        d.line(P(pts + [pts[0]]), fill=rgb(edge), width=3 * SS)
    done(c, out)


def arrow_button(out, left, color, w=120, h=64):
    c, d = para(w, h, color, slant=10)
    cx, cy, a = w / 2, h / 2, 14
    pts = [(cx + a * .8, cy - a), (cx - a * .8, cy), (cx + a * .8, cy + a)] if left else \
          [(cx - a * .8, cy - a), (cx + a * .8, cy), (cx - a * .8, cy + a)]
    d.polygon(P(pts), fill=rgb(CREAM))
    done(c, out)


def mini_ball(out, size=30):
    c = canvas(size, size)
    d = ImageDraw.Draw(c)
    s = size * SS
    k = int(s * .08)
    d.ellipse([s * .04, s * .04, s * .96, s * .96], fill=rgb("#FFF8F1"), outline=rgb("#1C0E13"), width=k)
    d.pieslice([s * .04, s * .04, s * .96, s * .96], 180, 360, fill=rgb("#E8452C"), outline=rgb("#1C0E13"), width=k)
    d.line([(s * .04, s / 2), (s * .96, s / 2)], fill=rgb("#1C0E13"), width=k)
    d.ellipse([s * .36, s * .36, s * .64, s * .64], fill=rgb("#FFF8F1"), outline=rgb("#1C0E13"), width=int(s * .06))
    done(c, out)


def matchup_frame(out, fill, edge, w=228, h=50):
    """A matchup cell: dark tinted tile with the meaning's colour along the left edge."""
    c, d = para(w, h, fill, slant=8, left=False)
    d.polygon(P([(0, 0), (7, 0), (7, h), (0, h)]), fill=rgb(edge))
    done(c, out)


def main(out, game="scarlet", light=False):
    th = (LIGHT if light else THEMES)[game]
    set_theme(th, light)
    acc, acc2, band = th["acc"], th["acc2"], th["band"]
    gold = GOLD or acc2
    for sub in ("t", "mv", "mvn", "bph", "gem"):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    o = lambda *p: os.path.join(out, *p)
    background(o("bg.png"), band, crystals=[(shard(1150, 860, 120, 150), acc2, 90), (shard(1196, 820, 60, 76, 1), gold, 100)])
    # battle: crystals round the rival's frame
    background(o("bg_b.png"), band, crystals=[(shard(900, 300, 70, 92), acc2, 140), (shard(1186, 96, 52, 70, 1), gold, 140),
                                               (shard(1170, 300, 90, 110, 2), "#FF5F7A", 100)])
    # a fine gold line under the top bar; the tag of the advice strip
    c = canvas(1240, 3)
    ImageDraw.Draw(c).rectangle([0, 0, 1240 * SS, 3 * SS], fill=rgb(gold) if GOLD else (0, 0, 0, 0))
    done(c, o("goldline.png"))
    save_para(o("tag.png"), 200, 56, gold, slant=14)
    splash(o("splash.png"), acc, acc2, band)
    splash(o("splash_v.png"), acc, acc2, band)
    # party
    party_card(o("card.png"), PANEL, acc)
    party_card(o("card_sel.png"), SELCARD or acc, acc2 if not SELCARD else acc, bottom=GOLD)
    party_card(o("card_ko.png"), DIM, KO)
    sel_frame(o("sel.png"), acc2)
    for i, (_, col) in enumerate(TYPES):
        gem(o("gem", f"{i}.png"), col)
    gem(o("gem_ko.png"), KO)
    # panels (same sizes as before)
    for name, w, h in (("panel.png", 600, 930), ("panel_s.png", 600, 300), ("panel_c.png", 800, 560),
                       ("panel_w.png", 960, 930), ("panel_tall.png", 284, 908), ("panel_d.png", 600, 862),
                       ("panel_dw.png", 960, 862), ("panel_dx.png", 600, 862)):
        cut_panel(o(name), w, h)
    # top bar: the place on an accent ribbon
    save_para(o("topbar.png"), 620, 64, acc, slant=34, left=False)
    # tabs: slanted segments; the tab bar itself is clear
    Image.new("RGBA", (8, 8), (0, 0, 0, 0)).save(o("tabbar.png"))
    for key in ("party", "dex", "bag", "map", "card"):
        save_para(o(f"tab_{key}.png"), 246, 92, PANEL, slant=14)
        c, d = para(246, 92, acc, slant=14)
        if GOLD:
            d.polygon(P([(14 * 4 / 92, 88), (246 - 14 * 88 / 92, 88), (232, 92), (0, 92)]), fill=rgb(GOLD))
        done(c, o(f"tab_{key}_on.png"))
    star(o("star.png"))
    # bag, Pokédex, map
    save_para(o("pouch.png"), 228, 78, PANEL, slant=10, left=False)
    save_para(o("pouch_on.png"), 228, 78, acc, slant=10, left=False)
    save_para(o("rtab.png"), 194, 60, PANEL, slant=12)
    save_para(o("rtab_on.png"), 194, 60, acc, slant=12)
    chip(o("itemcard.png"), 466, 92, fill=PANEL2, stripe=acc, slant=8)
    chip(o("dexcell.png"), 112, 150, fill=PANEL)
    chip(o("dexcell_on.png"), 112, 150, fill=SELFILL, edge=acc2)
    arrow_button(o("btn_prev.png"), True, acc)
    arrow_button(o("btn_next.png"), False, acc)
    mini_ball(o("pball.png"))
    for name, col in (("me_live.png", acc), ("me_saved.png", "#8E86A0")):
        c = canvas(44, 44)
        d = ImageDraw.Draw(c)
        d.ellipse([2 * SS, 4 * SS, 42 * SS, 44 * SS], fill=(0, 0, 0, 110))
        d.ellipse([2 * SS, 2 * SS, 40 * SS, 40 * SS], fill=rgb(CREAM))
        d.ellipse([8 * SS, 8 * SS, 34 * SS, 34 * SS], fill=rgb(col))
        d.ellipse([16 * SS, 16 * SS, 26 * SS, 26 * SS], fill=rgb(CREAM))
        done(c, o(name))
    chip(o("mapframe.png"), 924, 924, fill=PANEL, edge=acc)
    # trainer card
    c = canvas(1200, 214)
    d = ImageDraw.Draw(c)
    d.polygon(P([(0, 0), (1200, 0), (1200, 180), (1166, 214), (0, 214)]), fill=rgb(PANEL))
    d.polygon(P([(0, 0), (16, 0), (16, 214), (0, 214)]), fill=rgb(acc))
    done(c, o("clockband.png"))
    c = canvas(150, 72)
    d = ImageDraw.Draw(c)
    d.polygon(P([(10, 4), (136, 4), (128, 68), (2, 68)]), outline=rgb(INKA), width=5 * SS)
    d.polygon(P([(138, 24), (146, 24), (143, 48), (135, 48)]), fill=rgb(INKA))
    done(c, o("battery.png"))
    c = canvas(40, 56)
    d = ImageDraw.Draw(c)
    d.polygon(P([(24, 2), (6, 32), (19, 32), (14, 54), (34, 22), (21, 22), (27, 2)]), fill=rgb("#FFD166"))
    done(c, o("bolt.png"))
    header_panel(o("tcard_s.png"), 1200, 720, acc, hdr=76)
    header_panel(o("tcard_v.png"), 1200, 720, acc, hdr=76)
    chip(o("tile.png"), 372, 118, fill=PANEL2, stripe=acc2)
    chip(o("pslot.png"), 180, 176, fill=PANEL2)
    # map: facing arrow, compass, zoom buttons
    c = canvas(64, 64)
    d = ImageDraw.Draw(c)
    d.ellipse([10 * SS, 12 * SS, 56 * SS, 58 * SS], fill=(0, 0, 0, 90))
    d.ellipse([8 * SS, 8 * SS, 56 * SS, 56 * SS], fill=rgb(CREAM, 240))
    d.polygon(P([(32, 10), (47, 46), (32, 37), (17, 46)]), fill=rgb(acc))
    done(c, o("me_arrow.png"))
    c = canvas(900, 900)
    d = ImageDraw.Draw(c)
    d.ellipse([3 * SS, 3 * SS, 897 * SS, 897 * SS], outline=rgb(acc), width=8 * SS)
    d.ellipse([9 * SS, 9 * SS, 891 * SS, 891 * SS], outline=(0, 0, 0, 110), width=3 * SS)
    done(c, o("mapring.png"))
    c = canvas(16, 16)
    d = ImageDraw.Draw(c)
    d.ellipse([1 * SS, 1 * SS, 15 * SS, 15 * SS], fill=rgb(CREAM), outline=rgb(BG), width=3 * SS)
    done(c, o("placedot.png"))
    c = canvas(108, 108)
    d = ImageDraw.Draw(c)
    d.ellipse([2 * SS, 4 * SS, 106 * SS, 108 * SS], fill=(0, 0, 0, 90))
    d.ellipse([2 * SS, 2 * SS, 104 * SS, 104 * SS], fill=rgb(PANEL, 235))
    d.ellipse([2 * SS, 2 * SS, 104 * SS, 104 * SS], outline=rgb(acc2), width=3 * SS)
    d.polygon(P([(53, 26), (61, 53), (45, 53)]), fill=rgb(acc))
    d.polygon(P([(45, 53), (61, 53), (53, 80)]), fill=rgb(MUTED))
    done(c, o("compass.png"))
    for name, plus, on in (("zin.png", True, True), ("zin_off.png", True, False),
                           ("zout.png", False, True), ("zout_off.png", False, False)):
        c, d = para(96, 72, acc if on else PANEL2, slant=10)
        cx, cy = 48, 36
        col = rgb("#FFF8F1") if on else rgb(ZOFF)
        d.rectangle([(cx - 16) * SS, (cy - 3) * SS, (cx + 16) * SS, (cy + 3) * SS], fill=col)
        if plus:
            d.rectangle([(cx - 3) * SS, (cy - 16) * SS, (cx + 3) * SS, (cy + 16) * SS], fill=col)
        done(c, o(name))
    save_para(o("pill.png"), 166, 44, acc, slant=10)
    save_para(o("pill2.png"), 166, 44, PANEL2, slant=10)
    save_para(o("best.png"), 84, 34, acc2, slant=7)
    chip(o("sgcard.png"), 424, 204, fill=PANEL2, stripe=acc2)
    chip(o("sgrow.png"), 424, 124, fill=PANEL2)
    chip(o("pickcell.png"), 172, 142, fill=PANEL2)
    chip(o("pickcell_g.png"), 172, 142, fill=SELFILL, edge=acc2)
    for name, up in (("chev_down.png", False), ("chev_up.png", True)):
        c = canvas(22, 16)
        d = ImageDraw.Draw(c)
        pts = [(2, 14), (11, 2), (20, 14)] if up else [(2, 2), (11, 14), (20, 2)]
        d.polygon(P(pts), fill=rgb(CREAM))
        done(c, o(name))
    # battle
    header_panel(o("bcard_me.png"), 600, 186, acc2)
    header_panel(o("bcard_opp.png"), 600, 186, acc)
    header_panel(o("bpanel.png"), 750, 736, acc, hdr=64)
    cut_panel(o("bover.png"), 1204, 900)
    c = canvas(300, 320)                      # the rival's crystal frame
    d = ImageDraw.Draw(c)
    hexp = lambda k: [(150, k), (300 - k, 80 + k * .5), (300 - k, 240 - k * .5), (150, 320 - k), (k, 240 - k * .5), (k, 80 + k * .5)]
    if GOLD:
        d.polygon(P(hexp(0)), fill=rgb(GOLD))
        d.polygon(P(hexp(5)), fill=rgb(acc))
        d.polygon(P(hexp(11)), fill=rgb(PANEL))
    else:
        d.polygon(P(hexp(0)), fill=rgb(acc))
        d.polygon(P(hexp(5)), fill=rgb(PANEL))
    done(c, o("bframe.png"))
    c = canvas(1400, 66)                      # the VS ribbon
    d = ImageDraw.Draw(c)
    if GOLD:
        d.polygon(P([(0, 52), (1400, 12), (1400, 26), (0, 66)]), fill=rgb(GOLD))
    d.polygon(P([(0, 40), (1400, 0), (1400, 20), (0, 60)]), fill=rgb(acc))
    done(c, o("vsband.png"))
    for name, fill, edge in (("mu_hurt.png", *HURT), ("mu_hold.png", *HOLD), ("mu_none.png", *NONE)):
        c, d = para(420, 250, fill, slant=16, left=False)
        d.polygon(P([(0, 0), (8, 0), (8, 250), (0, 250)]), fill=rgb(edge))
        done(c, o(name))
    for i, (_, col) in enumerate(TYPES):
        save_para(o("bph", f"{i}.png"), 746, 64, col, slant=20, left=False)
    for k, (fill, edge) in enumerate([HURT, HURT2, HOLD, HOLD2, NONE]):
        matchup_frame(o(f"frm{k}.png"), fill, edge)
    for k, (fill, edge) in (("wk", HURT2), ("rs", HOLD), ("im", NONE)):
        matchup_frame(o(f"frm_{k}.png"), fill, edge, w=176, h=40)
    for name, up, col in (("arr_up.png", True, "#FF6B5A"), ("arr_dn.png", False, "#4ED07A")):
        c = canvas(20, 18)
        d = ImageDraw.Draw(c)
        pts = [(1, 17), (10, 1), (19, 17)] if up else [(1, 1), (10, 17), (19, 1)]
        d.polygon(P(pts), fill=rgb(col))
        done(c, o(name))
    save_para(o("pill_off.png"), 170, 40, PANEL2, slant=10)
    c = canvas(862, 48)                       # the move the game's cursor is on
    d = ImageDraw.Draw(c)
    pts = [(2, 2), (860, 2), (852, 46), (2, 46)]
    d.line(P(pts + [pts[0]]), fill=rgb(GOLD or acc2), width=4 * SS, joint="curve")
    done(c, o("mvsel.png"))
    header_panel(o("bmoves.png"), 452, 736, acc2, hdr=64)
    for i, (_, col) in enumerate(TYPES):
        move_card(o("mvn", f"{i}.png"), col, w=860, h=68)
        type_badge(o("t", f"{i}.png"), col)
        move_card(o("mv", f"{i}.png"), col)
    ball_placeholder(o("ball_ph.png"))
    ball_placeholder(o("ball_ph_ink.png"))
    for k in range(3):
        category(o(f"cat{k}.png"), k)
    gender(o("g0.png"), False)
    gender(o("g1.png"), True)
    c = canvas(34, 34)                        # money: a coin
    d = ImageDraw.Draw(c)
    d.ellipse([1 * SS, 1 * SS, 33 * SS, 33 * SS], fill=rgb(acc2))
    d.ellipse([6 * SS, 6 * SS, 28 * SS, 28 * SS], outline=rgb(BG), width=3 * SS)
    d.line(P([(14, 11), (14, 24)]), fill=rgb(BG), width=3 * SS)
    d.arc([12 * SS, 10 * SS, 23 * SS, 19 * SS], 270, 90, fill=rgb(BG), width=3 * SS)
    d.line(P([(14, 11), (17.5, 11)]), fill=rgb(BG), width=3 * SS)
    d.line(P([(14, 18.5), (17.5, 18.5)]), fill=rgb(BG), width=3 * SS)
    done(c, o("coin.png"))
    Image.new("RGBA", (4, 4), rgb(acc2)).save(o("uline.png"))
    print("art written to", out, "(" + game + ")")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "art", sys.argv[2] if len(sys.argv) > 2 else "scarlet",
         len(sys.argv) > 3 and sys.argv[3] == "light")
