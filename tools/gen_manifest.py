#!/usr/bin/env python3
"""Writes packages/<Game>/dualscreen/{manifest.json, art/, font/} for the Scarlet/Violet
companion. 1240x1080 canvas (the AYN Thor's second screen).

v0.8 look ("D"): dark night background, slanted panels and tabs, Barlow Condensed (SIL OFL)
with leaning headings (Eden Duo runtime 22 "italic"; older runtimes draw them upright), the
game's colour as the accent (Scarlet red/orange, Violet violet/lilac) and matchup colours that
mean something: red = it hurts, green = it holds out, blue = no effect. A bar along the top of
the main pages shows where you are, your money, the battery and the time.

Art and the font are the companion's own (tools/make_art.py, tools/make_font.py). Game words
come from the module at run time; Pokémon pictures from the player's own
`user:sprites/<national>.png` or the game's icons.
"""
import copy
import json
import pathlib
import shutil
import sys

VERSION = "v0.9.0"
GAMES = {
    "PokemonScarlet": {"title_id": "0100A3D008C5C000", "name": "Pokemon Scarlet dual screen",
                        "title": "Pokémon Escarlata", "theme": "scarlet"},
    "PokemonViolet": {"title_id": "01008F6008C5E000", "name": "Pokemon Violet dual screen",
                       "title": "Pokémon Púrpura", "theme": "violet"},
}
# accent, second accent, gold, text, labels, secondary text, page background
THEMES = {"scarlet": ("#FFE8452C", "#FFFF8A1F", "#FFF2B544", "#FFFFF1E4", "#FFC4A7A9", "#FFE3CFC8", "#FF1C0E13"),
          "violet": ("#FF8B4DFF", "#FFC9A2FF", "#FFC9A2FF", "#FFF6EFE6", "#FFA79FB5", "#FFCFC7DA", "#FF15131C")}

W, H = 1240, 1080
WHITE = "#FFFFFFFF"    # text on the accent (tabs, buttons, type badges): white in both themes
HEAD = "#FFFEFEFE"     # headings on the panels: white in the dark theme, dark ink in the light one
INK = "#FFF6EFE6"      # main text (cream on the dark panels)
GREY = "#FFA79FB5"     # labels
SOFT = "#FFCFC7DA"     # secondary text
DARK = "#FF15131C"
ACC, ACC2, GOLD, _, _, _, PAGE_BG = THEMES["scarlet"]
HURT_T, HOLD_T, NONE_T = "#FFFF9A8C", "#FF8FE3B0", "#FF9FEFFF"
MAPTXT = "#FF241F2E"
TYPES = 19
SPRITE = "user:sprites/%d.png"


def set_theme(theme):
    global ACC, ACC2, GOLD, INK, GREY, SOFT, PAGE_BG
    ACC, ACC2, GOLD, INK, GREY, SOFT, PAGE_BG = THEMES[theme]


def A(p):
    return "file:art/" + p


def img(rect, src=None, **kw):
    d = {"type": "image", "rect": rect}
    if src:
        d["src"] = src
    d.update(kw)
    return d


def lab(rect, text=None, bind=None, scale=4, color=None, **kw):
    d = {"type": "label", "rect": rect, "text_scale": scale, "color": color or INK}
    if text is not None:
        d["text"] = text
    if bind:
        d["bind_text"] = bind
    d.update(kw)
    return d


def val(rect, bind, scale=4, color=None, **kw):
    d = {"type": "value", "rect": rect, "bind": bind, "text_scale": scale, "color": color or INK}
    d.update(kw)
    return d


def tap(rect, action, **kw):
    d = {"type": "rect", "rect": rect, "color": "#00000000", "on_tap": action}
    d.update(kw)
    return d


def bar(rect, bind, color, max_bind=None, mx=None, bg=None, **kw):
    d = {"type": "bar", "rect": rect, "bind": bind, "color": color, "bg": bg or PAGE_BG}
    if max_bind:
        d["max_bind"] = max_bind
    if mx is not None:
        d["max"] = mx
    d.update(kw)
    return d


IT = {"italic": True}
TYPE_SRC = [A(f"t/{i}.png") for i in range(TYPES)]
MOVE_SRC = [A(f"mv/{i}.png") for i in range(TYPES)]
MOVEN_SRC = [A(f"mvn/{i}.png") for i in range(TYPES)]
GEM_SRC = [A(f"gem/{i}.png") for i in range(TYPES)]
CAT_SRC = [A(f"cat{i}.png") for i in range(3)]
HP_COLORS = (("hpg", "#FF4ED07A"), ("hpy", "#FFFFC23A"), ("hpr", "#FFFF5A47"))

TABS = [("party", "Equipo"), ("dex", "Pokédex"), ("bag", "Mochila"), ("map", "Mapa"), ("card", "Ficha")]

CARD_GATES = {}   # name -> expr (host-key gates: clock, battery)
BATTLE_GATES = {}  # name -> expr, filled by battle_page()


def gate_card(name, expr):
    CARD_GATES[name] = expr
    return name


def bgate(name, expr):
    BATTLE_GATES[name] = expr
    return name


def tab_bar(active):
    out = []
    for k, (key, label) in enumerate(TABS):
        x = 8 + k * 244
        on = key == active
        out.append(img([x, 972, 246, 92], A(f"tab_{key}{'_on' if on else ''}.png")))
        out.append(lab([x, 1000, 246, 40], label, scale=8, color=WHITE if on else SOFT, align="center", **IT))
        if not on:
            out.append(tap([x, 972, 246, 92], f"go_{key}"))
    return out


def clock_widgets(x_end=1214, y=10):
    """Time (and battery, runtime 21) from the host's own keys: a module cannot read @clock."""
    CK = gate_card("ui.c.clock", "@clock.year > 2000")
    BAT = gate_card("ui.c.bat", "@device.battery_present && @device.battery_pct >= 0")
    return [
        val([x_end - 124, y, 60, 0], "@clock.hour", scale=9, color=HEAD, pad=2, align="right", need_bind=CK, **IT),
        lab([x_end - 61, y - 2, 12, 46], ":", scale=9, color=HEAD, need_bind=CK),
        val([x_end - 50, y, 0, 0], "@clock.minute", scale=9, color=HEAD, pad=2, need_bind=CK, **IT),
        img([x_end - 272, y + 10, 50, 24], A("battery.png"), need_bind=BAT),
        bar([x_end - 266, y + 15, 36, 14], "@device.battery_pct", "#FF4ED07A", mx=100, bg="#00000000", need_bind=BAT),
        val([x_end - 138, y + 8, 0, 0], "@device.battery_pct", scale=6, color=SOFT, align="right", suffix=" %",
            need_bind=BAT),
    ]


def top_bar(title=None):
    """Where you are (or a title), your money, the battery and the time."""
    out = [img([0, 64, 1240, 3], A("goldline.png")), img([0, 0, 620, 64], A("topbar.png"))]
    if title:
        out.append(lab([24, 12, 560, 40], title, scale=8, color=WHITE, **IT))
    else:
        out += [lab([24, 12, 560, 40], bind="sv.map.place", scale=8, color=WHITE, **IT),
                img([652, 15, 34, 34], A("coin.png")),
                val([696, 16, 0, 0], "sv.money", scale=7, color=ACC2, **IT)]
    return out + clock_widgets()


# ---------------------------------------------------------------- party page
def party_cards():
    rep = {"repeat": 6, "repeat_dy": 144}
    X, Y = 18, 78

    def r(d, need="sv.p{i}.valid"):
        d = dict(d)
        d.update(rep)
        d.setdefault("need_bind", need)
        return d

    out = [
        r(img([X, Y, 560, 130], A("card.png"))),
        r(img([X, Y, 560, 130], A("card_ko.png")), "sv.p{i}.fainted"),
        r(img([X, Y, 560, 130], A("card_sel.png")), "ui.show{i}"),
        r(img([X + 22, Y + 8, 100, 114], bind="sv.d{i}.t1", src_names=GEM_SRC), "ui.gem{i}"),
        r(img([X + 22, Y + 8, 100, 114], A("gem_ko.png")), "sv.p{i}.fainted"),
        r(img([X + 38, Y + 30, 68, 68], A("ball_ph.png"), hide_bind="sv.p{i}.has_spr", keep_max=0)),
        r(img([X + 8, Y - 2, 128, 128], bind="sv.p{i}.spr", src_format=SPRITE), "sv.p{i}.has_spr"),
        r(lab([X + 140, Y + 5, 300, 44], bind="sv.p{i}.name", scale=9, color=HEAD, **IT)),
        r(img([X + 404, Y + 16, 28, 28], A("star.png")), "sv.p{i}.shiny"),
        r(img([X + 436, Y + 16, 28, 28], A("g0.png")), "sv.p{i}.male"),
        r(img([X + 436, Y + 16, 28, 28], A("g1.png")), "sv.p{i}.female"),
    ]
    for band, col in HP_COLORS:
        out.append(r(bar([X + 140, Y + 60, 300, 14], "sv.p{i}.hp", col, max_bind="sv.p{i}.hpmax"), "sv.p{i}." + band))
    out += [
        r(val([X + 140, Y + 78, 0, 0], "sv.p{i}.hp", scale=6, color=SOFT, max_bind="sv.p{i}.hpmax", max_sep="/")),
        r(img([X + 262, Y + 80, 92, 26], bind="sv.d{i}.t1", src_names=TYPE_SRC), "sv.d{i}.typed"),
        r(lab([X + 262, Y + 85, 92, 18], bind="sv.d{i}.t1n", scale=3, color=WHITE, align="center"), "sv.d{i}.typed"),
        r(img([X + 358, Y + 80, 92, 26], bind="sv.d{i}.t2", src_names=TYPE_SRC), "sv.d{i}.dual"),
        r(lab([X + 358, Y + 85, 92, 18], bind="sv.d{i}.t2n", scale=3, color=WHITE, align="center"), "sv.d{i}.dual"),
        r(lab([X + 140, Y + 110, 120, 18], bind="sv.p{i}.st", scale=3, color="#FFFFD166")),
        r(lab([X + 474, Y + 14, 60, 20], "NV.", scale=3, color=GREY)),
        r(val([X + 476, Y + 40, 64, 60], "sv.p{i}.level", scale=12, color=HEAD, align="right", **IT)),
        r(tap([X, Y, 560, 130], "sel{i}")),
    ]
    return out


def detail_template():
    X, Y = 612, 78
    V = dict(need_bind="sv.sel.valid")
    out = [
        img([X, Y, 600, 862], A("panel_d.png"), **V),
        lab([X + 22, Y + 8, 300, 22], "FICHA", scale=3, color=GOLD, **V),
        lab([X + 22, Y + 30, 440, 56], bind="sv.sel.name", scale=11, color=HEAD, **IT, **V),
        lab([X + 470, Y + 14, 50, 20], "NV.", scale=3, color=GREY, **V),
        val([X + 476, Y + 30, 104, 56], "sv.sel.level", scale=11, color=ACC2, align="right", **IT, **V),
        img([X + 50, Y + 120, 140, 140], A("ball_ph.png"), need_bind="sv.sel.valid", hide_bind="sv.sel.has_spr",
            keep_max=0),
        img([X + 20, Y + 94, 200, 200], bind="sv.sel.spr", src_format=SPRITE, need_bind="sv.sel.has_spr"),
        img([X + 196, Y + 100, 30, 30], A("star.png"), need_bind="sv.sel.shiny"),
        lab([X + 20, Y + 288, 200, 24], bind="sv.sel.status", scale=4, color=GREY, align="center", **V),
        img([X + 236, Y + 102, 160, 40], bind="sv.sel.t1", src_names=TYPE_SRC, need_bind="sv.sel.typed"),
        lab([X + 236, Y + 110, 160, 26], bind="sv.sel.t1n", scale=5, color=WHITE, align="center", **IT,
            need_bind="sv.sel.typed"),
        img([X + 404, Y + 102, 160, 40], bind="sv.sel.t2", src_names=TYPE_SRC, need_bind="sv.sel.dual"),
        lab([X + 404, Y + 110, 160, 26], bind="sv.sel.t2n", scale=5, color=WHITE, align="center", **IT,
            need_bind="sv.sel.dual"),
        lab([X + 238, Y + 160, 160, 20], "OBJETO", scale=3, color=GREY, **V),
        lab([X + 238, Y + 182, 340, 34], bind="sv.sel.item", scale=6, color=INK, **V),
        lab([X + 238, Y + 228, 160, 20], "HABILIDAD", scale=3, color=GREY, **V),
        lab([X + 238, Y + 250, 340, 34], bind="sv.sel.ability", scale=6, color=INK, **V),
    ]
    # detail tabs
    for k, (key, label) in enumerate([("moves", "ATAQUES"), ("stats", "DATOS"), ("weak", "DEBILIDADES")]):
        x = X + 20 + k * 188
        out += [
            img([x, Y + 318, 180, 46], A("pill2.png"), **V),
            img([x, Y + 318, 180, 46], A("pill.png"), need_bind=f"sv.dtab.{key}"),
            lab([x, Y + 328, 180, 28], label, scale=5, color=SOFT, align="center", **IT, **V),
            lab([x, Y + 328, 180, 28], label, scale=5, color=WHITE, align="center", **IT, need_bind=f"sv.dtab.{key}"),
            tap([x, Y + 310, 180, 62], f"dtab{k}", **V),
        ]
    # Moves tab
    for j in range(4):
        y = Y + 382 + j * 118
        need = f"sv.sel.m{j}.valid"
        g = dict(need_bind=need, hide_bind="sv.dtab.moves", keep_min=1)
        out += [
            img([X + 20, y, 560, 104], bind=f"sv.sel.m{j}.t", src_names=MOVE_SRC, **g),
            img([X + 38, y + 29, 46, 46], bind=f"sv.sel.m{j}.cat", src_names=CAT_SRC, **g),
            lab([X + 98, y + 14, 300, 36], bind=f"sv.sel.m{j}.name", scale=7, color=HEAD, **IT, **g),
            img([X + 98, y + 60, 124, 30], bind=f"sv.sel.m{j}.t", src_names=TYPE_SRC, **g),
            lab([X + 98, y + 66, 124, 20], bind=f"sv.sel.m{j}.tn", scale=3, color=WHITE, align="center", **g),
            lab([X + 236, y + 64, 140, 24], bind=f"sv.sel.m{j}.pow", scale=4, color=GREY, **g),
            lab([X + 400, y + 44, 150, 40], bind=f"sv.sel.m{j}.pp", scale=7, color=INK, align="right", **IT, **g),
        ]
    # Data tab: nature, Tera type, stats
    D = "sv.dtab.stats"
    out += [
        lab([X + 34, Y + 392, 220, 30], "NATURALEZA", scale=4, color=GREY, need_bind=D),
        lab([X + 236, Y + 386, 330, 36], bind="sv.sel.nature", scale=7, color=INK, **IT, need_bind=D),
        lab([X + 34, Y + 446, 220, 30], "TERATIPO", scale=4, color=GREY, need_bind=D),
        img([X + 236, Y + 438, 160, 40], bind="sv.sel.tera", src_names=TYPE_SRC, need_bind=D),
        lab([X + 236, Y + 446, 160, 26], bind="sv.sel.teran", scale=5, color=WHITE, align="center", **IT, need_bind=D),
    ]
    for k, name in enumerate(["PS", "Ataque", "Defensa", "At. Esp.", "Def. Esp.", "Velocidad"]):
        y = Y + 506 + k * 58
        out += [
            lab([X + 34, y, 190, 34], name, scale=6, color=SOFT, need_bind=D),
            bar([X + 210, y + 8, 260, 22], f"sv.sel.stat{k}", ACC if k != 1 else ACC2, mx=400, need_bind=D),
            val([X + 478, y - 6, 100, 44], f"sv.sel.stat{k}", scale=8, color=HEAD, align="right", **IT, need_bind=D),
        ]
    # Weaknesses tab: red = hurts it, green = it holds out, blue = no effect
    WK = "sv.dtab.weak"

    def grid(prefix, y0, rows, frame, xcol):
        rp = dict(repeat=3 * rows, repeat_cols=3, repeat_dx=184, repeat_row_dy=42)
        g = dict(need_bind=prefix + "{i}.ok", hide_bind=WK, keep_min=1, **rp)
        return [
            img([X + 26, y0, 176, 38], A(frame), **g),
            lab([X + 40, y0 + 8, 120, 24], bind=prefix + "{i}.tn", scale=5, color=INK, **IT, **g),
            lab([X + 154, y0 + 12, 42, 18], bind=prefix + "{i}.x", scale=3, color=xcol, align="right", **g),
        ]

    for title, prefix, y0, rows, frame, col in [
            ("LE HACEN DAÑO", "sv.sel.wk", Y + 380, 3, "frm_wk.png", HURT_T),
            ("AGUANTA BIEN", "sv.sel.rs", Y + 548, 4, "frm_rs.png", HOLD_T),
            ("NO LE AFECTA", "sv.sel.im", Y + 758, 1, "frm_im.png", NONE_T)]:
        out += [
            lab([X + 30, y0, 300, 26], title, scale=5, color=col, **IT, need_bind=WK),
            lab([X + 30, y0 + 34, 300, 30], "Nada", scale=5, color=GREY, need_bind=prefix + ".none",
                hide_bind=WK, keep_min=1),
        ] + grid(prefix, y0 + 32, rows, frame, col)
    out.append(lab([X + 30, Y + 836, 540, 20], "Según los tipos de la especie; no cuenta habilidades ni Teracristal.",
                   scale=2, color=GREY, need_bind=WK))
    return out


TAB_KEYS = ["moves", "stats", "weak"]


def detail_sheets():
    """One copy of the detail sheet per party slot, shown only while that slot is selected.
    The template above speaks of "sv.sel.*" and "sv.dtab.*"; here each copy reads its slot's
    "sv.d<slot>.*" and is gated by derived values the runtime evaluates after the tick's flags
    ("ui.show<slot>", "ui.show<slot>.<tab>", see derived_gates()). A widget's data condition
    (a move present, a second type...) moves to hide_bind with keep_min 1."""
    out = []
    for slot in range(6):
        for w in detail_template():
            w = copy.deepcopy(w)
            need, hide = w.pop("need_bind", None), w.get("hide_bind")
            tab = None
            data = None
            for g in (need, hide if w.get("keep_min") == 1 else None):
                if g and g.startswith("sv.dtab."):
                    tab = g[len("sv.dtab."):]
                elif g and g.startswith("sv.sel.") and g != "sv.sel.valid":
                    data = g
                elif g and "{i}" in g:  # weakness grid entries: sv.sel.wk{i}.ok
                    data = g
            if hide and hide.startswith("sv.dtab."):
                w.pop("hide_bind"); w.pop("keep_min", None)
            if w.get("hide_bind") == "sv.sel.has_spr":   # the placeholder: shown without a picture
                data = None
            w["need_bind"] = f"ui.show{slot}" + (f".{tab}" if tab else "")
            if data:
                assert "hide_bind" not in w, w
                w["hide_bind"] = data
                w["keep_min"] = 1
            txt = json.dumps(w, ensure_ascii=False).replace("sv.sel.", f"sv.d{slot}.")
            out.append(json.loads(txt))
    return out


def derived_gates():
    d = [{"name": f"ui.sel{s}", "cmp": "eq", "a": "@flag:sel", "b": s} for s in range(6)]
    d += [{"name": f"ui.tab.{k}", "cmp": "eq", "a": "@flag:dtab", "b": i} for i, k in enumerate(TAB_KEYS)]
    d += [{"name": f"ui.show{s}", "all_nonzero": [f"ui.sel{s}", f"sv.d{s}.valid"]} for s in range(6)]
    d += [{"name": f"ui.show{s}.{k}", "all_nonzero": [f"ui.show{s}", f"ui.tab.{k}"]}
          for s in range(6) for k in TAB_KEYS]
    d += [{"name": f"ui.gem{s}", "expr": f"sv.d{s}.typed && !sv.p{s}.fainted"} for s in range(6)]
    return d


# ---------------------------------------------------------------- battle page
def battle_page():
    """Shown while the module sees a battle (sv.battle). The rival large at the top (name,
    level, types, HP), a line of advice, its base stats against yours, then what hurts it, what
    it holds out against and what does not affect it; your Pokémon and its moves at the bottom.
    No command buttons: the game keeps those."""
    O, M = "sv.opp.", "sv.me."
    w = [img([0, 0, W, H], A("bg_b.png")),
         img([-80, 496, 1400, 66], A("vsband.png"))]
    w += top_bar("¡COMBATE!")
    # -- the rival
    w += [
        img([946, 70, 256, 272], A("bframe.png")),
        img([1010, 142, 128, 128], A("ball_ph.png"), hide_bind=O + "has_spr", keep_max=0),
        img([974, 106, 200, 200], bind=O + "spr", src_format=SPRITE, need_bind=O + "has_spr"),
        img([948, 76, 34, 34], A("star.png"), need_bind=O + "shiny"),
        lab([24, 140, 880, 90], bind=O + "name", scale=18, color=HEAD, **IT, need_bind=O + "valid"),
        lab([24, 150, 880, 60], "Buscando al rival…", scale=12, color=SOFT, **IT, hide_bind=O + "valid", keep_max=0),
        img([862, 160, 40, 40], A("g0.png"), need_bind=O + "male"),
        img([862, 160, 40, 40], A("g1.png"), need_bind=O + "female"),
        # change it / Tera
        img([24, 80, 166, 44], A("pill.png"), need_bind=O + "valid"),
        lab([24, 88, 166, 28], "CAMBIAR", scale=5, color=WHITE, align="center", **IT, need_bind=O + "valid"),
        tap([20, 74, 174, 56], "pick_open", need_bind=O + "valid"),
        img([204, 80, 200, 44], A("pill2.png"), need_bind=bgate("ui.b.teraoff", "sv.opp.tera_can && !sv.opp.tera_on")),
        lab([204, 88, 200, 28], "¿TERACRISTAL?", scale=5, color=SOFT, align="center", **IT, need_bind="ui.b.teraoff"),
        img([204, 80, 200, 44], bind=O + "tera", src_names=TYPE_SRC, need_bind=O + "tera_on"),
        lab([216, 88, 70, 28], "TERA", scale=5, color=WHITE, **IT, need_bind=O + "tera_on"),
        lab([392, 88, 0, 28], bind=O + "teran", scale=5, color=WHITE, align="right", **IT, need_bind=O + "tera_on"),
        tap([200, 74, 208, 56], "tera_toggle", need_bind=O + "tera_can"),
        lab([420, 92, 480, 22], bind=O + "how", scale=3, color=GREY, need_bind=O + "valid"),
        # level and types
        lab([24, 248, 60, 30], "NV.", scale=6, color=ACC2, **IT, need_bind=O + "lvl_ok"),
        val([76, 236, 0, 0], O + "level", scale=10, color=ACC2, **IT, need_bind=O + "lvl_ok"),
        img([190, 240, 170, 44], bind=O + "t1", src_names=TYPE_SRC, need_bind=O + "typed"),
        lab([190, 249, 170, 28], bind=O + "t1n", scale=6, color=WHITE, align="center", **IT, need_bind=O + "typed"),
        img([368, 240, 170, 44], bind=O + "t2", src_names=TYPE_SRC, need_bind=O + "dual"),
        lab([368, 249, 170, 28], bind=O + "t2n", scale=6, color=WHITE, align="center", **IT, need_bind=O + "dual"),
        lab([560, 236, 160, 18], "HABILIDAD", scale=2, color=GREY, need_bind=O + "full"),
        lab([560, 254, 180, 30], bind=O + "ability", scale=5, color=INK, need_bind=O + "full"),
        lab([744, 236, 160, 18], "OBJETO", scale=2, color=GREY, need_bind=O + "full"),
        lab([744, 254, 180, 30], bind=O + "item", scale=5, color=INK, need_bind=O + "full"),
    ]
    for band, col in HP_COLORS:
        w.append(bar([24, 302, 680, 26], O + "hp", col, max_bind=O + "hpmax",
                     need_bind=bgate(f"ui.b.{O}{band}", f"{O}full && {O}{band}")))
    w.append(val([900, 292, 0, 0], O + "hp", scale=8, color=HEAD, align="right", max_bind=O + "hpmax",
                 max_sep="/", **IT, need_bind=O + "full"))
    # chosen by species: you set its level; HP, ability and item unknown
    SP = bgate("ui.b.sponly", "sv.opp.valid && !sv.opp.full")
    w += [lab([24, 300, 200, 30], "SU NIVEL", scale=5, color=GREY, **IT, need_bind=SP)]
    for k, (txt, d) in enumerate([("-5", -5), ("-1", -1), ("+1", 1), ("+5", 5)]):
        bx = 150 + k * 82 + (110 if k >= 2 else 0)
        w += [img([bx, 292, 74, 46], A("pill.png"), need_bind=SP),
              lab([bx, 300, 74, 30], txt, scale=6, color=WHITE, align="center", **IT, need_bind=SP),
              tap([bx - 4, 286, 82, 58], "opp_lvl_%s" % ("m" if d < 0 else "p") + str(abs(d)), need_bind=SP)]
    w += [
        val([369, 290, 0, 0], O + "splvl", scale=10, color=ACC2, align="center", **IT,
            need_bind=bgate("ui.b.splvl", "ui.b.sponly && !sv.opp.splvl_unk")),
        lab([369, 290, 0, 50], "?", scale=10, color=HURT_T, align="center",
            need_bind=bgate("ui.b.splvlq", "ui.b.sponly && sv.opp.splvl_unk")),
        lab([904, 300, 0, 22], "PS, habilidad y objeto: desconocidos", scale=3, color=GREY, align="right", need_bind=SP),
    ]
    # -- advice strip: who is faster and the best switch
    w += [
        img([18, 350, 1204, 56], A("pill2.png"), need_bind=O + "valid"),
        img([18, 350, 200, 56], A("tag.png"), need_bind=O + "valid"),
        lab([18, 362, 200, 32], "CONSEJO", scale=7, color=DARK, align="center", **IT, need_bind=O + "valid"),
        lab([236, 364, 580, 30], bind=O + "speed", scale=6, color=INK, need_bind=O + "valid"),
        lab([1020, 368, 0, 26], "Mejor cambio:", scale=5, color=GREY, align="right",
            need_bind=bgate("ui.b.best", "sv.opp.valid && sv.sg0.valid && sv.sg0.best")),
        lab([1032, 362, 180, 32], bind="sv.sg0.name", scale=7, color=ACC2, **IT, need_bind="ui.b.best"),
    ]
    # -- base stats against yours
    BS = bgate("ui.b.bs", "sv.opp.valid && sv.opp.bs_ok")
    for k, name in enumerate(["PS", "ATAQUE", "DEFENSA", "AT. ESP.", "DEF. ESP.", "VELOC."]):
        cx = 24 + k * 162
        w += [
            lab([cx, 418, 150, 20], name, scale=3, color=GREY, need_bind=BS),
            val([cx, 438, 0, 0], O + "bs%d" % k, scale=7, color=INK, **IT, need_bind=BS),
            img([cx + 66, 446, 18, 16], A("arr_up.png"), need_bind=O + "bs%d.up" % k),
            img([cx + 66, 446, 18, 16], A("arr_dn.png"), need_bind=O + "bs%d.dn" % k),
        ]
        for q, col in enumerate(["#FFFF5A47", "#FFFF8A1F", "#FFFFC23A", "#FF4ED07A", "#FF6FC8FF"]):
            w.append(bar([cx, 478, 140, 8], O + "bs%d" % k, col, mx=160, need_bind=O + "bs%d.c%d" % (k, q)))
    w.append(lab([1216, 424, 0, 20], bind=O + "cmp", scale=3, color=GREY, align="right",
                 need_bind=bgate("ui.b.cmp", "sv.opp.valid && sv.opp.bs_ok && sv.opp.full")))
    w.append(lab([1216, 456, 0, 20], "Base de la especie", scale=3, color=GREY, align="right", need_bind=BS))

    # -- matchups
    def grid(prefix, x0, y0, rows, cols, cw):
        g = dict(need_bind=prefix + "{i}.ok", repeat=cols * rows, repeat_cols=cols, repeat_dx=cw + 8,
                 repeat_row_dy=46)
        out = [img([x0, y0, cw, 40], A("frm%d.png" % q), **dict(g, need_bind=prefix + "{i}.lv%d" % q))
               for q in range(5)]
        out += [lab([x0 + 16, y0 + 10, cw - 56, 22], bind=prefix + "{i}.tn", scale=4, color=INK, **IT, **g)]
        for q, c in enumerate([HURT_T, HURT_T, HOLD_T, HOLD_T, NONE_T]):
            out.append(lab([x0 + cw - 12, y0 + 13, 0, 18], bind=prefix + "{i}.x", scale=3, color=c, align="right",
                           **dict(g, need_bind=prefix + "{i}.lv%d" % q)))
        return out

    MY = 532
    for title, prefix, x0, wd, rows, cols, art, col in [
            ("LE HACE DAÑO", O + "wk", 18, 370, 3, 2, "mu_hurt.png", HURT_T),
            ("AGUANTA", O + "rs", 400, 572, 3, 3, "mu_hold.png", HOLD_T),
            ("NO LE AFECTA", O + "im", 984, 238, 3, 1, "mu_none.png", NONE_T)]:
        cw = (wd - 40 - 8 * (cols - 1)) // cols
        w += [img([x0, MY, wd, 222], A(art), need_bind=O + "typed"),
              lab([x0 + 22, MY + 12, 300, 30], title, scale=6, color=col, **IT, need_bind=O + "typed"),
              lab([x0 + 22, MY + 60, 300, 30], "Nada", scale=6, color=GREY, **IT, need_bind=prefix + ".none")]
        if prefix != O + "im":
            w.append(lab([x0 + wd - 30, MY + 16, 0, 24], bind=prefix + ".more", scale=4, color=col, align="right",
                         need_bind=O + "typed"))
        w += grid(prefix, x0 + 22, MY + 52, rows if prefix != O + "im" else 3, cols, cw)
    w += [
        lab([24, 762, 900, 22], "Solo por tipos (sin habilidad). Toca ¿Teracristal? si lo usa.", scale=3, color=GREY,
            need_bind=bgate("ui.b.notera", "sv.opp.typed && !sv.opp.tera_on")),
        lab([24, 760, 900, 26], "Calculado con su teratipo", scale=5, color=HURT_T, **IT, need_bind=O + "tera_on"),
    ]
    # -- you
    w += [
        lab([24, 800, 300, 22], "TU POKÉMON", scale=4, color=GOLD, need_bind=M + "valid"),
        lab([24, 826, 330, 56], bind=M + "name", scale=11, color=HEAD, **IT, need_bind=M + "valid"),
        img([170, 798, 26, 26], A("g0.png"), need_bind=M + "male"),
        img([170, 798, 26, 26], A("g1.png"), need_bind=M + "female"),
        lab([24, 896, 60, 28], "NV.", scale=5, color=GREY, **IT, need_bind=M + "valid"),
        val([72, 890, 0, 0], M + "level", scale=8, color=INK, **IT, need_bind=M + "valid"),
        val([330, 890, 0, 0], M + "hp", scale=8, color=INK, align="right", max_bind=M + "hpmax", max_sep="/", **IT,
            need_bind=M + "full"),
        lab([24, 980, 320, 26], bind=M + "status", scale=4, color="#FFFFD166", need_bind=M + "valid"),
    ]
    for band, col in HP_COLORS:
        w.append(bar([24, 944, 306, 18], M + "hp", col, max_bind=M + "hpmax",
                     need_bind=bgate(f"ui.b.{M}{band}", f"{M}full && {M}{band}")))
    # -- your moves, or (tap "Cambios") the switch suggestions
    X2 = 360
    closed = bgate("ui.b.closed", "sv.sugg.open == 0")
    opened = bgate("ui.b.open", "sv.sugg.open != 0")
    w += [
        lab([X2, 800, 400, 26], "TUS ATAQUES", scale=5, color=ACC2, **IT, need_bind=closed),
        lab([X2, 800, 600, 26], "SUGERENCIAS DE CAMBIO", scale=5, color=ACC2, **IT, need_bind=opened),
        img([1056, 792, 166, 40], A("pill.png")),
        lab([1056, 799, 140, 26], "CAMBIOS", scale=5, color=WHITE, align="center", **IT, need_bind=closed),
        lab([1056, 799, 140, 26], "ATAQUES", scale=5, color=WHITE, align="center", **IT, need_bind=opened),
        img([1190, 805, 20, 14], A("chev_down.png"), need_bind=closed),
        img([1190, 805, 20, 14], A("chev_up.png"), need_bind=opened),
        tap([1050, 786, 178, 52], "sugg_toggle"),
    ]
    for j in range(4):
        y = 840 + j * 54
        need = bgate(f"ui.b.m{j}", f"sv.sugg.open == 0 && {M}m{j}.valid")
        hi = bgate(f"ui.b.m{j}hi", f"sv.sugg.open == 0 && {M}m{j}.eff_hi")
        lo = bgate(f"ui.b.m{j}lo", f"sv.sugg.open == 0 && {M}m{j}.eff_lo")
        nrm = bgate(f"ui.b.m{j}n", f"sv.sugg.open == 0 && {M}m{j}.effc == 3")
        st = bgate(f"ui.b.m{j}s", f"sv.sugg.open == 0 && {M}m{j}.valid && {M}m{j}.cat == 0")
        w += [
            img([X2, y, 862, 48], bind=f"{M}m{j}.t", src_names=MOVEN_SRC, need_bind=need),
            lab([X2 + 28, y + 9, 330, 32], bind=f"{M}m{j}.name", scale=7, color=HEAD, **IT, need_bind=need),
            img([X2 + 360, y + 10, 112, 28], bind=f"{M}m{j}.t", src_names=TYPE_SRC, need_bind=need),
            lab([X2 + 360, y + 15, 112, 20], bind=f"{M}m{j}.tn", scale=3, color=WHITE, align="center", need_bind=need),
            lab([X2 + 484, y + 12, 110, 28], bind=f"{M}m{j}.pp", scale=5, color=SOFT, need_bind=need),
            lab([X2 + 840, y + 10, 0, 30], bind=f"{M}m{j}.eff", scale=6, color=HOLD_T, align="right", **IT, need_bind=hi),
            lab([X2 + 840, y + 10, 0, 30], bind=f"{M}m{j}.eff", scale=6, color=GREY, align="right", **IT, need_bind=lo),
            lab([X2 + 840, y + 10, 0, 30], "x1", scale=6, color=INK, align="right", **IT, need_bind=nrm),
            lab([X2 + 840, y + 12, 0, 26], "Estado", scale=5, color=GREY, align="right", need_bind=st),
        ]
    # the move the game's cursor is on (once the guided search below has found it)
    for j in range(4):
        w.append(img([X2, 840 + j * 54, 862, 48], A("mvsel.png"),
                     need_bind=bgate(f"ui.b.cur{j}", f"sv.sugg.open == 0 && sv.cur.ok && sv.cur.idx == {j}")))
    BAD = HURT_T
    for k in range(3):
        x, y = X2 + k * 290, 840
        Pf = f"sv.sg{k}."
        card = bgate(f"ui.b.s{k}", f"sv.sugg.open != 0 && {Pf}valid")
        g2 = lambda name, cond: bgate(f"ui.b.s{k}{name}", f"sv.sugg.open != 0 && {Pf}valid && {cond}")
        w += [
            img([x, y, 282, 204], A("sgcard.png"), need_bind=card),
            img([x + 16, y + 10, 52, 52], A("ball_ph.png"), need_bind=g2("ph", f"!{Pf}has_spr")),
            img([x + 8, y + 2, 68, 68], bind=Pf + "spr", src_format=SPRITE, need_bind=g2("sp", f"{Pf}has_spr")),
            lab([x + 80, y + 10, 190, 34], bind=Pf + "name", scale=7, color=HEAD, **IT, need_bind=card),
            img([x + 190, y + 46, 80, 30], A("best.png"), need_bind=g2("b", f"{Pf}best")),
            lab([x + 190, y + 50, 80, 22], "MEJOR", scale=4, color=DARK, align="center", **IT, need_bind=f"ui.b.s{k}b"),
        ]
        for band, col in HP_COLORS:
            w.append(bar([x + 80, y + 54, 100, 10], Pf + "hp", col, max_bind=Pf + "hpmax",
                         need_bind=g2(band, f"{Pf}{band}")))
        for n in range(4):
            ly = y + 86 + n * 28
            w += [lab([x + 20, ly, 256, 24], bind=f"{Pf}l{n}", scale=4, color=INK, need_bind=g2(f"l{n}", f"{Pf}l{n}.ok")),
                  lab([x + 20, ly, 256, 24], bind=f"{Pf}l{n}", scale=4, color=BAD, need_bind=g2(f"l{n}x", f"{Pf}l{n}.bad"))]
    w.append(lab([X2, 860, 700, 30], "No hay otros Pokémon que puedan luchar.", scale=5, color=GREY,
                 need_bind=bgate("ui.b.snone", "sv.sugg.open != 0 && sv.sugg.n == 0")))
    w += [
        lab([24, 1052, 1192, 12], bind="sv.scan.text", scale=2, color="#FFFFE08A"),
        lab([24, 1066, 1192, 12], bind="sv.learn.text", scale=2, color="#FFB8F0FF"),
    ]
    # -- guided search for the game's move-list cursor (v0.8.2)
    CO = "sv.cur.open"
    w += [
        img([440, 12, 160, 40], A("pill2.png")),
        lab([440, 19, 160, 26], "CURSOR", scale=5, color=SOFT, align="center", **IT),
        tap([436, 4, 168, 56], "cur_open"),
        img([18, 516, 1204, 268], A("bover.png"), need_bind=CO),
        lab([40, 530, 900, 40], "BUSCAR EL CURSOR DE ATAQUES", scale=8, color=GOLD, **IT, need_bind=CO),
        lab([40, 580, 1150, 30], bind="sv.cur.text", scale=5, color=INK, wrap_width=1150, max_lines=2, need_bind=CO),
    ]
    for j in range(4):
        x = 40 + j * 222
        w += [img([x, 640, 210, 90], A("pickcell.png"), need_bind=CO),
              lab([x + 14, 652, 50, 60], str(j + 1), scale=12, color=GOLD, **IT, need_bind=CO),
              lab([x + 66, 664, 140, 50], bind=f"{M}m{j}.name", scale=4, color=INK, wrap_width=136, max_lines=2,
                  need_bind=CO),
              tap([x, 640, 210, 90], f"cur_mark{j}", need_bind=CO)]
    w += [
        img([940, 640, 260, 40], A("pill.png"), need_bind=CO),
        lab([940, 647, 260, 26], "REINICIAR", scale=5, color=WHITE, align="center", **IT, need_bind=CO),
        tap([936, 634, 268, 52], "cur_reset", need_bind=CO),
        img([940, 690, 260, 40], A("pill2.png"), need_bind=CO),
        lab([940, 697, 260, 26], "CERRAR", scale=5, color=SOFT, align="center", **IT, need_bind=CO),
        tap([936, 684, 268, 52], "cur_open", need_bind=CO),
        lab([40, 738, 1160, 16], bind="sv.cur.diag", scale=2, color=GREY, need_bind=CO),
        lab([40, 756, 1160, 18], "Toca el número del ataque donde está el cursor del juego. Muévelo y repite hasta que se resalte.",
            scale=3, color=GREY, need_bind=CO),
    ]
    # -- the rival picker, over the page while open
    X, Y = 245, 96
    PO = "sv.pick.open"
    w += [
        img([18, 76, 1204, 900], A("bover.png"), need_bind=PO),
        lab([X, Y + 4, 720, 50], "¿CONTRA QUIÉN LUCHAS?", scale=10, color=HEAD, **IT, need_bind=PO),
        lab([X, Y + 66, 720, 30], "Toca el Pokémon del combate (el más probable va primero).",
            scale=4, color=GREY, need_bind=PO),
        lab([X, Y + 140, 720, 40], "Buscando Pokémon cerca...", scale=6, color=GREY,
            need_bind=bgate("ui.b.pnone", "sv.pick.open != 0 && sv.pick.n == 0")),
    ]
    rp = dict(repeat=16, repeat_cols=4, repeat_dx=180, repeat_row_dy=150)
    px, py = X, Y + 108
    for i in range(16):  # derived gates are global names: one per cell
        bgate(f"ui.b.pg{i}", f"sv.pick.{i}.valid && sv.pick.{i}.guess")
        bgate(f"ui.b.ps{i}", f"sv.pick.{i}.valid && sv.pick.{i}.has_spr")
        bgate(f"ui.b.pi{i}", f"sv.pick.{i}.valid && sv.pick.{i}.game_icon")
    w += [
        img([px, py, 172, 142], A("pickcell.png"), need_bind="sv.pick.{i}.valid", **rp),
        img([px, py, 172, 142], A("pickcell_g.png"), need_bind="ui.b.pg{i}", **rp),
        img([px + 42, py + 4, 88, 88], bind="sv.pick.{i}.spr", src_format=SPRITE, need_bind="ui.b.ps{i}", **rp),
        img([px + 46, py + 8, 80, 80], bind="sv.pick.{i}.icon", src_format="module:poke/%d",
            need_bind="ui.b.pi{i}", **rp),
        lab([px + 6, py + 88, 160, 26], bind="sv.pick.{i}.name", scale=5, color=INK, align="center", **IT,
            need_bind="sv.pick.{i}.valid", **rp),
        lab([px + 40, py + 118, 50, 20], "NV.", scale=3, color=GREY, need_bind="sv.pick.{i}.valid", **rp),
        val([px + 74, py + 116, 50, 22], "sv.pick.{i}.level", scale=4, color=INK, need_bind="sv.pick.{i}.valid", **rp),
        tap([px, py, 172, 142], "pick{i}", need_bind="sv.pick.{i}.valid", **rp),
        img([X + 190, Y + 724, 370, 52], A("pill.png"), need_bind=PO),
        lab([X + 190, Y + 734, 370, 32], "¿NO ESTÁ? BUSCAR POR NOMBRE", scale=5, color=WHITE, align="center", **IT,
            need_bind=PO),
        tap([X + 190, Y + 718, 370, 64], "sp_open", need_bind=PO),
    ]
    SO, SL, SI = "sv.sp.open", "sv.sp.letters", "sv.sp.listing"
    w += [
        img([18, 76, 1204, 900], A("bover.png"), need_bind=SO),
        lab([X, Y + 4, 520, 50], "BUSCAR POR NOMBRE", scale=10, color=HEAD, **IT, need_bind=SO),
        img([X + 560, Y + 8, 170, 44], A("pill.png"), need_bind=SO),
        lab([X + 560, Y + 16, 170, 28], "VOLVER", scale=5, color=WHITE, align="center", **IT, need_bind=SO),
        tap([X + 556, Y + 2, 178, 56], "sp_back", need_bind=SO),
        lab([X, Y + 76, 700, 30], "Toca la primera letra del Pokémon.", scale=5, color=GREY, need_bind=SL),
    ]
    for k in range(26):
        cx, cy = X + (k % 7) * 104, Y + 126 + (k // 7) * 100
        w += [img([cx, cy, 96, 90], A("pickcell.png"), need_bind=SL),
              lab([cx, cy + 20, 96, 48], chr(65 + k), scale=10, color=INK, align="center", **IT, need_bind=SL),
              tap([cx, cy, 96, 90], f"spl{k}", need_bind=SL)]
    rs = dict(repeat=16, repeat_cols=4, repeat_dx=180, repeat_row_dy=134)
    sx, sy = X, Y + 76
    for i in range(16):
        bgate(f"ui.b.ss{i}", f"sv.sp.listing && sv.sp.{i}.valid && sv.sp.{i}.has_spr")
        bgate(f"ui.b.si{i}", f"sv.sp.listing && sv.sp.{i}.valid && sv.sp.{i}.game_icon")
        bgate(f"ui.b.sv{i}", f"sv.sp.listing && sv.sp.{i}.valid")
    w += [
        img([sx, sy, 172, 128], A("pickcell.png"), need_bind="ui.b.sv{i}", **rs),
        img([sx + 46, sy + 4, 80, 80], bind="sv.sp.{i}.spr", src_format=SPRITE, need_bind="ui.b.ss{i}", **rs),
        img([sx + 50, sy + 8, 72, 72], bind="sv.sp.{i}.icon", src_format="module:poke/%d", need_bind="ui.b.si{i}", **rs),
        lab([sx + 6, sy + 92, 160, 26], bind="sv.sp.{i}.name", scale=5, color=INK, align="center", **IT,
            need_bind="ui.b.sv{i}", **rs),
        tap([sx, sy, 172, 128], "spk{i}", need_bind="ui.b.sv{i}", **rs),
        img([X, Y + 640, 120, 64], A("btn_prev.png"), need_bind=bgate("ui.b.spp", "sv.sp.listing && sv.sp.prev")),
        tap([X, Y + 640, 120, 64], "sp_prev", need_bind="ui.b.spp"),
        img([X + 600, Y + 640, 120, 64], A("btn_next.png"), need_bind=bgate("ui.b.spn", "sv.sp.listing && sv.sp.next")),
        tap([X + 600, Y + 640, 120, 64], "sp_next", need_bind="ui.b.spn"),
        val([X + 360, Y + 650, 0, 0], "sv.sp.page", scale=8, color=INK, align="center", max_bind="sv.sp.pages",
            max_sep=" / ", **IT, need_bind=SI),
    ]
    return {"id": "battle", "widgets": w}


# ---------------------------------------------------------------- bag, Pokédex, map
POUCHES = ["Medicinas", "Poké Balls", "Combate", "Bayas", "Otros objetos", "Máquinas técnicas", "Tesoros",
           "Ingredientes", "Objetos clave", "Materiales"]
POUCH_TABS = ["Medicinas", "Poké Balls", "Combate", "Bayas", "Otros", "MT", "Tesoros", "Picnic", "Clave",
              "Materiales"]
REGIONS = ["Paldea", "Kitakami", "Arándano"]
DEX_CELLS = 20


def bag_page():
    w = [img([0, 0, W, H], A("bg.png"))] + top_bar()
    for p, label in enumerate(POUCH_TABS):
        x, y = 18, 78 + p * 88
        w += [
            img([x, y, 228, 78], A("pouch.png"), hide_bind=f"ui.pouch{p}", keep_max=0),
            img([x, y, 228, 78], A("pouch_on.png"), need_bind=f"ui.pouch{p}"),
            lab([x + 18, y + 24, 170, 30], label, scale=6, color=WHITE, **IT, need_bind=f"ui.pouch{p}"),
            lab([x + 18, y + 24, 170, 30], label, scale=6, color=SOFT, **IT, hide_bind=f"ui.pouch{p}", keep_max=0),
            val([x + 206, y + 28, 0, 0], f"sv.bag.pc{p}", scale=4, color=SOFT, align="right"),
            tap([x, y, 228, 78], f"bagp{p}"),
        ]
    X, Y = 262, 78
    w.append(img([X, Y, 960, 870], A("panel_dw.png")))
    for p, title in enumerate(POUCHES):
        w.append(lab([X + 24, Y + 14, 640, 50], title.upper(), scale=10, color=HEAD, **IT, need_bind=f"ui.pouch{p}"))
    w += [
        val([X + 936, Y + 20, 0, 0], "sv.bag.page", scale=7, color=ACC2, align="right", max_bind="sv.bag.pages",
            max_sep=" / ", **IT, need_bind="sv.bag.ok"),
        lab([X + 40, Y + 120, 860, 40], "No tienes nada en este bolsillo.", scale=7, color=GREY, need_bind="sv.bag.empty"),
        lab([X + 40, Y + 120, 860, 40], "Leyendo la mochila...", scale=7, color=GREY, hide_bind="sv.bag.ok", keep_max=0),
    ]
    rep = dict(repeat=14, repeat_cols=2, repeat_dx=474, repeat_row_dy=98)
    gx, gy = X + 12, Y + 80
    w += [
        img([gx, gy, 466, 90], A("itemcard.png"), need_bind="sv.bag.{i}.valid", **rep),
        img([gx + 18, gy + 6, 76, 76], bind="sv.bag.{i}.id", src_format="module:item/%d", need_bind="sv.bag.{i}.valid",
            **rep),
        lab([gx + 104, gy + 28, 280, 34], bind="sv.bag.{i}.name", scale=6, color=INK, need_bind="sv.bag.{i}.valid", **rep),
        val([gx + 446, gy + 24, 0, 0], "sv.bag.{i}.count", scale=7, color=ACC2, align="right", **IT,
            need_bind="sv.bag.{i}.many", **rep),
    ]
    w += [
        img([X + 14, Y + 784, 120, 64], A("btn_prev.png"), need_bind="sv.bag.prev"),
        tap([X + 14, Y + 784, 120, 64], "bag_prev", need_bind="sv.bag.prev"),
        img([X + 822, Y + 784, 120, 64], A("btn_next.png"), need_bind="sv.bag.next"),
        tap([X + 822, Y + 784, 120, 64], "bag_next", need_bind="sv.bag.next"),
        lab([X + 150, Y + 802, 660, 30], "Toca un bolsillo a la izquierda; las flechas pasan de página.", scale=4,
            color=GREY, align="center"),
    ]
    return {"id": "bag", "widgets": w + tab_bar("bag")}


def dex_page():
    w = [img([0, 0, W, H], A("bg.png"))] + top_bar()
    for r, name in enumerate(REGIONS):
        x = 18 + r * 198
        w += [
            img([x, 76, 194, 60], A("rtab.png"), hide_bind=f"ui.region{r}", keep_max=0),
            img([x, 76, 194, 60], A("rtab_on.png"), need_bind=f"ui.region{r}"),
            lab([x, 90, 194, 34], name.upper(), scale=7, color=WHITE, align="center", **IT, need_bind=f"ui.region{r}"),
            lab([x, 90, 194, 34], name.upper(), scale=7, color=SOFT, align="center", **IT, hide_bind=f"ui.region{r}",
                keep_max=0),
            tap([x, 76, 194, 60], f"dexr{r}"),
        ]
        w += [
            lab([22, 148, 120, 28], "VISTOS", scale=4, color=GREY, need_bind=f"ui.region{r}"),
            val([104, 142, 0, 0], f"sv.dex.seen{r}", scale=7, color=INK, **IT, need_bind=f"ui.region{r}"),
            lab([236, 148, 170, 28], "CAPTURADOS", scale=4, color=GREY, need_bind=f"ui.region{r}"),
            val([376, 142, 0, 0], f"sv.dex.caught{r}", scale=7, color=ACC2, max_bind=f"sv.dex.total{r}",
                max_sep=" / ", **IT, need_bind=f"ui.region{r}"),
        ]
    rep = dict(repeat=DEX_CELLS, repeat_cols=5, repeat_dx=118, repeat_row_dy=156)
    gx, gy = 18, 188
    w += [
        img([gx, gy, 112, 148], A("dexcell.png"), need_bind="sv.dex.{i}.valid", hide_bind="sv.dex.{i}.sel",
            keep_max=0, **rep),
        img([gx, gy, 112, 148], A("dexcell_on.png"), need_bind="sv.dex.{i}.sel", **rep),
        img([gx + 22, gy + 18, 68, 68], A("ball_ph.png"), need_bind="sv.dex.{i}.valid",
            hide_bind="sv.dex.{i}.known", keep_max=0, **rep),
        img([gx + 8, gy + 6, 96, 96], bind="sv.dex.{i}.icon", src_format="module:poke/%d",
            need_bind="sv.dex.{i}.game_icon", **rep),
        img([gx + 6, gy + 4, 100, 100], bind="sv.dex.{i}.spr", src_format=SPRITE, need_bind="sv.dex.{i}.has_spr", **rep),
        img([gx + 80, gy + 6, 26, 26], A("pball.png"), need_bind="sv.dex.{i}.caught", **rep),
        val([gx + 56, gy + 108, 0, 0], "sv.dex.{i}.num", scale=6, color=INK, align="center", pad=3, **IT,
            need_bind="sv.dex.{i}.valid", **rep),
        tap([gx, gy, 112, 148], "dexsel{i}", need_bind="sv.dex.{i}.valid", **rep),
    ]
    w += [
        img([18, 822, 120, 64], A("btn_prev.png"), need_bind="sv.dex.prev"),
        tap([18, 822, 120, 64], "dex_prev", need_bind="sv.dex.prev"),
        img([482, 822, 120, 64], A("btn_next.png"), need_bind="sv.dex.next"),
        tap([482, 822, 120, 64], "dex_next", need_bind="sv.dex.next"),
        lab([150, 838, 120, 30], "PÁGINA", scale=5, color=GREY, align="right"),
        val([284, 832, 0, 0], "sv.dex.page", scale=8, color=INK, max_bind="sv.dex.pages", max_sep=" / ", **IT),
        lab([24, 900, 580, 28], "Leyendo la Pokédex...", scale=5, color=GREY, hide_bind="sv.dex.ok", keep_max=0),
    ]
    X, Y = 612, 78
    D = "sv.dx."
    w += [
        img([X, Y, 600, 870], A("panel_dx.png")),
        lab([X + 22, Y + 18, 400, 56], bind=D + "name", scale=11, color=HEAD, **IT, need_bind=D + "valid"),
        lab([X + 470, Y + 20, 50, 20], "Nº", scale=4, color=GREY, need_bind=D + "valid"),
        val([X + 580, Y + 22, 0, 0], D + "num", scale=10, color=ACC2, align="right", pad=3, **IT, need_bind=D + "valid"),
        img([X + 50, Y + 120, 140, 140], A("ball_ph.png"), need_bind=D + "valid", hide_bind=D + "known", keep_max=0),
        img([X + 30, Y + 100, 180, 180], bind=D + "icon", src_format="module:poke/%d", need_bind=D + "game_icon"),
        img([X + 20, Y + 90, 200, 200], bind=D + "spr", src_format=SPRITE, need_bind=D + "has_spr"),
        img([X + 196, Y + 96, 30, 30], A("pball.png"), need_bind=D + "caught"),
        img([X + 236, Y + 98, 160, 40], bind=D + "t1", src_names=TYPE_SRC, need_bind=D + "known"),
        lab([X + 236, Y + 106, 160, 26], bind=D + "t1n", scale=5, color=WHITE, align="center", **IT, need_bind=D + "known"),
        img([X + 404, Y + 98, 160, 40], bind=D + "t2", src_names=TYPE_SRC, need_bind=D + "dual"),
        lab([X + 404, Y + 106, 160, 26], bind=D + "t2n", scale=5, color=WHITE, align="center", **IT, need_bind=D + "dual"),
        lab([X + 238, Y + 156, 340, 30], bind=D + "cat", scale=5, color=SOFT, **IT, need_bind=D + "known"),
        lab([X + 238, Y + 204, 120, 22], "ALTURA", scale=3, color=GREY, need_bind=D + "known"),
        lab([X + 238, Y + 226, 160, 34], bind=D + "height", scale=7, color=INK, **IT, need_bind=D + "known"),
        lab([X + 410, Y + 204, 120, 22], "PESO", scale=3, color=GREY, need_bind=D + "known"),
        lab([X + 410, Y + 226, 170, 34], bind=D + "weight", scale=7, color=INK, **IT, need_bind=D + "known"),
        {"type": "rect", "rect": [X + 20, Y + 304, 556, 3], "bg": ACC, "color": "#00000000"},
        lab([X + 30, Y + 326, 540, 32], bind=D + "text", scale=6, color=INK, wrap_width=540, max_lines=12,
            need_bind=D + "valid"),
        lab([X + 30, Y + 820, 540, 30], "CAPTURADO", scale=6, color=HOLD_T, **IT, need_bind=D + "caught"),
    ]
    return {"id": "dex", "widgets": w + tab_bar("dex")}


def map_page():
    """The region's own map (decoded by the module from the game files) with you on it: zoom
    (the module crops the picture around you), your facing as a turning arrow, and a compass."""
    MX, MY, MS = 22, 22, 900  # map square (the module places the marker for MAP_PX = 900)
    w = [img([0, 0, W, H], A("bg.png")),
         img([MX - 12, MY - 12, MS + 24, MS + 24], A("mapframe.png")),
         img([MX, MY, MS, MS], bind="sv.map.img", src_format="module:map/%d"),
         img([MX, MY, MS, MS], A("mapring.png"), need_bind="sv.map.rot"),
         ] + [x for i in range(24) for x in (
             img([MX - 8, MY - 8, 16, 16], A("placedot.png"), need_bind=f"sv.map.l{i}.ok",
                 x_bind=f"sv.map.l{i}.x", y_bind=f"sv.map.l{i}.y"),
             lab([MX, MY + 8, 0, 22], bind=f"sv.map.l{i}.t", scale=4, color=MAPTXT, outline="#E0FFFFFF", outline_px=3,
                 align="center", need_bind=f"sv.map.l{i}.ok", x_bind=f"sv.map.l{i}.x", y_bind=f"sv.map.l{i}.y"))] + [
         img([MX - 22, MY - 22, 44, 44], A("me_saved.png"), need_bind="sv.map.saved",
             x_bind="sv.map.mx", y_bind="sv.map.my"),
         img([MX - 22, MY - 22, 44, 44], A("me_live.png"), need_bind="sv.map.live",
             x_bind="sv.map.mx", y_bind="sv.map.my"),
         dict(img([MX - 32, MY - 32, 64, 64], A("me_arrow.png"), need_bind="sv.map.arrow",
                  x_bind="sv.map.mx", y_bind="sv.map.my"), rotate_bind="sv.map.heading"),
         dict(img([MX + MS - 124, MY + 16, 108, 108], A("compass.png")), rotate_bind="sv.map.north"),
         lab([MX + MS - 70, MY + 20, 0, 22], "N", scale=4, color=HEAD, align="center", hide_bind="sv.map.rot", keep_max=0),
         lab([MX + MS - 70, MY + 98, 0, 22], "S", scale=4, color=GREY, align="center", hide_bind="sv.map.rot", keep_max=0),
         lab([MX + MS - 110, MY + 59, 0, 22], "O", scale=4, color=GREY, align="center", hide_bind="sv.map.rot", keep_max=0),
         lab([MX + MS - 31, MY + 59, 0, 22], "E", scale=4, color=GREY, align="center", hide_bind="sv.map.rot", keep_max=0)]
    X, Y = 942, 22
    w += [img([X, Y, 284, 908], A("panel_tall.png")),
          lab([X + 18, Y + 12, 240, 50], "MAPA", scale=11, color=HEAD, **IT),
          lab([X + 20, Y + 76, 240, 22], "ESTÁS EN", scale=3, color=GREY),
          lab([X + 20, Y + 100, 248, 36], bind="sv.map.place", scale=7, color=INK, **IT, wrap_width=244, max_lines=3),
          lab([X + 20, Y + 214, 240, 30], bind="sv.map.field", scale=5, color=ACC2, **IT),
          lab([X + 20, Y + 262, 240, 22], "ZOOM", scale=3, color=GREY),
          img([X + 16, Y + 292, 96, 72], A("zout.png"), need_bind="sv.map.can_out"),
          img([X + 16, Y + 292, 96, 72], A("zout_off.png"), hide_bind="sv.map.can_out", keep_max=0),
          tap([X + 16, Y + 288, 96, 80], "map_zoom_out", need_bind="sv.map.can_out"),
          img([X + 172, Y + 292, 96, 72], A("zin.png"), need_bind="sv.map.can_in"),
          img([X + 172, Y + 292, 96, 72], A("zin_off.png"), hide_bind="sv.map.can_in", keep_max=0),
          tap([X + 172, Y + 288, 96, 80], "map_zoom_in", need_bind="sv.map.can_in"),
          lab([X + 124, Y + 308, 20, 34], "x", scale=6, color=INK, **IT),
          val([X + 144, Y + 302, 0, 40], "sv.map.zoom", scale=8, color=INK, **IT),
          lab([X + 20, Y + 374, 248, 22], "Sin tu posición se ve el mapa entero.", scale=3, color=GREY,
              hide_bind="sv.map.on", keep_max=0),
          lab([X + 20, Y + 416, 240, 22], "ORIENTACIÓN", scale=3, color=GREY),
          lab([X + 20, Y + 440, 240, 40], bind="sv.map.dir", scale=8, color=INK, **IT, need_bind="sv.map.dir_ok"),
          lab([X + 20, Y + 444, 248, 50], "Camina un poco y la flecha mirará hacia donde vas.", scale=4,
              color=GREY, wrap_width=244, max_lines=2, hide_bind="sv.map.dir_ok", keep_max=0),
          img([X + 16, Y + 500, 252, 50], A("pill.png")),
          lab([X + 16, Y + 510, 252, 30], "NORTE ARRIBA", scale=5, color=WHITE, align="center", **IT,
              hide_bind="sv.map.rot_want", keep_max=0),
          lab([X + 16, Y + 510, 252, 30], "HACIA DONDE VAS", scale=5, color=WHITE, align="center", **IT,
              need_bind="sv.map.rot_want"),
          tap([X + 12, Y + 494, 260, 62], "map_rot"),
          lab([X + 18, Y + 556, 252, 22], "Necesita tu posición en directo", scale=3, color=HURT_T, need_bind="sv.map.rot_wait"),
          img([X + 18, Y + 586, 40, 40], A("me_arrow.png")),
          lab([X + 66, Y + 594, 200, 28], "Tú, en directo", scale=5, color=INK),
          img([X + 20, Y + 632, 36, 36], A("me_saved.png")),
          lab([X + 66, Y + 630, 200, 28], "Al guardar", scale=5, color=INK),
          lab([X + 66, Y + 658, 200, 22], "si aún no te mueves", scale=3, color=GREY),
          lab([X + 16, Y + 692, 30, 30], "X", scale=4, color=GREY, need_bind="sv.map.on"),
          val([X + 42, Y + 686, 100, 34], "sv.map.px", scale=6, color=INK, need_bind="sv.map.on"),
          lab([X + 148, Y + 692, 30, 30], "Z", scale=4, color=GREY, need_bind="sv.map.on"),
          val([X + 174, Y + 686, 100, 34], "sv.map.pz", scale=6, color=INK, need_bind="sv.map.on"),
          img([X + 20, Y + 728, 244, 42], A("pill2.png")),
          lab([X + 20, Y + 737, 244, 26], "¿MAPA REVUELTO?", scale=5, color=SOFT, align="center", **IT),
          tap([X + 16, Y + 724, 252, 50], "map_flip"),
          lab([X + 16, Y + 782, 252, 20], bind="sv.map.diag", scale=2, color=GREY, wrap_width=252, max_lines=5),
          lab([X + 16, Y + 868, 252, 20], bind="sv.map.why", scale=2, color=HURT_T, wrap_width=252, max_lines=3)]
    return {"id": "map", "widgets": w + tab_bar("map")}


def page_gates():
    d = [{"name": f"ui.pouch{p}", "cmp": "eq", "a": "sv.bag.pouch", "b": p} for p in range(len(POUCHES))]
    d += [{"name": f"ui.region{r}", "cmp": "eq", "a": "sv.dex.region", "b": r} for r in range(len(REGIONS))]
    return d


def page_actions():
    a = {}
    mod = lambda act, arg: {"kind": "module", "action": act, "argument": arg}
    for p in range(len(POUCHES)):
        a[f"bagp{p}"] = mod("bag_pouch", p)
    a["bag_prev"], a["bag_next"] = mod("bag_page", -1), mod("bag_page", 1)
    for r in range(len(REGIONS)):
        a[f"dexr{r}"] = mod("dex_region", r)
    a["dex_prev"], a["dex_next"] = mod("dex_page", -1), mod("dex_page", 1)
    a["sugg_toggle"] = mod("sugg_toggle", 0)
    a["tera_toggle"] = mod("tera_toggle", 0)
    a["cur_open"], a["cur_reset"] = mod("cur_open", 0), mod("cur_reset", 0)
    for j in range(4):
        a[f"cur_mark{j}"] = mod("cur_mark", j)
    a["map_zoom_in"], a["map_zoom_out"] = mod("map_zoom", 1), mod("map_zoom", -1)
    a["map_flip"] = mod("map_flip", 0)
    a["map_rot"] = mod("map_rot", 0)
    for d in (-5, -1, 1, 5):
        a["opp_lvl_%s%d" % ("m" if d < 0 else "p", abs(d))] = mod("opp_lvl", d)
    for i in range(16):
        a[f"pick{i}"] = mod("pick", i)
    a["pick_open"] = mod("pick_open", 0)
    a["sp_open"], a["sp_back"] = mod("sp_open", 0), mod("sp_back", 0)
    a["sp_prev"], a["sp_next"] = mod("sp_page", -1), mod("sp_page", 1)
    for k in range(26):
        a[f"spl{k}"] = mod("sp_letter", k)
    for i in range(16):
        a[f"spk{i}"] = mod("sp_pick", i)
    for i in range(DEX_CELLS):
        a[f"dexsel{i}"] = mod("dex_sel", i)
    return a


def trainer_page(game_key="PokemonScarlet"):
    """Trainer card: the console's clock, date and battery (Eden Duo host keys), then a licence
    in the game's colour with the save's numbers and the team."""
    w = [img([0, 0, W, H], A("bg.png"))]
    CX, CY = 20, 16
    CK = gate_card("ui.c.clock", "@clock.year > 2000")
    BAT = gate_card("ui.c.bat", "@device.battery_present && @device.battery_pct >= 0")
    WD = ["Domingo,", "Lunes,", "Martes,", "Miércoles,", "Jueves,", "Viernes,", "Sábado,"]
    MO = ["", "de enero", "de febrero", "de marzo", "de abril", "de mayo", "de junio", "de julio", "de agosto",
          "de septiembre", "de octubre", "de noviembre", "de diciembre"]
    w += [img([CX, CY, 1200, 214], A("clockband.png")),
          val([CX + 44, CY + 18, 0, 0], "@clock.hour", scale=20, color=HEAD, pad=2, need_bind=CK, **IT),
          lab([CX + 160, CY + 12, 40, 110], ":", scale=20, color=HEAD, need_bind=CK),
          val([CX + 192, CY + 18, 0, 0], "@clock.minute", scale=20, color=HEAD, pad=2, need_bind=CK, **IT),
          dict(val([CX + 300, CY + 150, 0, 0], "@clock.wday", scale=7, color=SOFT, align="right", need_bind=CK, **IT), names=WD),
          val([CX + 334, CY + 150, 0, 0], "@clock.day", scale=7, color=SOFT, align="center", need_bind=CK, **IT),
          dict(val([CX + 364, CY + 150, 0, 0], "@clock.month", scale=7, color=SOFT, need_bind=CK, **IT), names=MO),
          lab([CX + 44, CY + 70, 760, 40], "Reloj y batería: actualiza Eden Duo", scale=6, color=SOFT,
              need_bind=gate_card("ui.c.noclock", "!(@clock.year > 2000)")),
          val([CX + 500, CY + 40, 0, 0], "@perf.fps", scale=4, color=GREY, suffix=" FPS", need_bind="@perf.fps"),
          dict(val([CX + 500, CY + 74, 0, 0], "@game.seconds", scale=4, color=GREY, suffix=" min de sesión",
                   need_bind="@game.seconds"), div=60)]
    # the light / dark theme (kept between sessions: "settings" below makes the flag persistent)
    w += [img([CX + 962, CY + 156, 220, 44], A("pill.png")),
          lab([CX + 962, CY + 164, 220, 28], "CAMBIAR TEMA", scale=5, color=WHITE, align="center", **IT),
          tap([CX + 956, CY + 150, 232, 58], "theme_toggle")]
    BX, BY = CX + 830, CY + 46
    w += [img([BX, BY, 150, 72], A("battery.png"), need_bind=BAT)]
    for band, expr, col in (("low", "<= 20", "#FFFF5A47"), ("mid", "> 20 && @device.battery_pct <= 50", "#FFFFC23A"),
                            ("high", "> 50", "#FF4ED07A")):
        g = gate_card("ui.c.bat" + band, "@device.battery_present && @device.battery_pct " + expr)
        w.append(bar([BX + 14, BY + 12, 106, 44], "@device.battery_pct", col, mx=100, bg="#00000000", need_bind=g))
    w += [img([BX + 44, BY + 8, 40, 56], A("bolt.png"), need_bind="@device.charging"),
          val([BX + 350, BY + 4, 0, 0], "@device.battery_pct", scale=11, color=HEAD, align="right", suffix=" %",
              need_bind=BAT, **IT),
          lab([BX + 2, BY + 92, 300, 30], "CARGANDO", scale=5, color="#FFFFD166", **IT, need_bind="@device.charging"),
          lab([BX + 2, BY + 92, 300, 30], "BATERÍA BAJA", scale=5, color=HURT_T, **IT,
              need_bind=gate_card("ui.c.lowwarn", "@device.battery_present && @device.battery_pct <= 20 && !@device.charging"))]
    TX, TY = 20, 244
    card = "tcard_v.png" if game_key == "PokemonViolet" else "tcard_s.png"
    w += [img([TX, TY, 1200, 700], A(card)),
          lab([TX + 30, TY + 18, 700, 44], "LICENCIA DE ENTRENADOR", scale=9, color=WHITE, **IT),
          lab([TX + 1170, TY + 26, 0, 30], GAMES[game_key]["title"].upper(), scale=5, color=SOFT, align="right", **IT),
          lab([TX + 34, TY + 94, 200, 22], "NOMBRE", scale=3, color=GREY),
          lab([TX + 34, TY + 116, 560, 60], bind="sv.trainer", scale=11, color=HEAD, **IT),
          lab([TX + 640, TY + 94, 200, 22], "ID", scale=3, color=GREY),
          val([TX + 640, TY + 116, 0, 0], "sv.tid", scale=11, color=ACC2, pad=6, **IT)]
    tiles = [("DINERO", "sv.money", None, None), ("PUNTOS DE LIGA", "sv.lp", None, None),
             ("TIEMPO DE JUEGO", None, "sv.tr.time", "sv.tr.time_ok"),
             ("POKÉDEX", "sv.tr.caught", None, "sv.tr.dex_ok"),
             ("PUNTOS ARÁNDANO", "sv.tr.bp", None, "sv.tr.bp_ok"), ("EQUIPO", "sv.party.count", None, None)]
    for k, (title, vbind, tbind, need) in enumerate(tiles):
        x, y = TX + 24 + (k % 3) * 388, TY + 196 + (k // 3) * 132
        nb = {"need_bind": need} if need else {}
        w += [img([x, y, 372, 118], A("tile.png"), **nb),
              lab([x + 26, y + 14, 330, 22], title, scale=3, color=GREY, **nb)]
        if tbind:
            w.append(lab([x + 26, y + 44, 340, 50], bind=tbind, scale=9, color=INK, **IT, **nb))
        elif vbind == "sv.tr.caught":
            w += [val([x + 26, y + 44, 0, 0], vbind, scale=9, color=INK, **IT, **nb),
                  lab([x + 350, y + 66, 0, 30], "capturados", scale=4, color=GREY, align="right", **nb),
                  val([x + 350, y + 16, 0, 0], "sv.tr.seen", scale=3, color=GREY, align="right", suffix=" vistos", **nb)]
        elif vbind == "sv.party.count":
            w.append(val([x + 26, y + 44, 0, 0], vbind, scale=9, color=INK, suffix=" / 6", **IT))
        else:
            w.append(val([x + 26, y + 44, 0, 0], vbind, scale=9, color=INK, **IT, **nb))
    w.append(lab([TX + 34, TY + 466, 400, 30], "TU EQUIPO", scale=6, color=ACC2, **IT))
    for i in range(6):
        x, y = TX + 24 + i * 194, TY + 506
        v = f"sv.p{i}.valid"
        w += [img([x, y, 180, 176], A("pslot.png")),
              img([x + 50, y + 34, 80, 80], A("ball_ph.png"), need_bind=v, hide_bind=f"sv.p{i}.has_spr", keep_max=0),
              img([x + 30, y + 16, 120, 120], bind=f"sv.tr.p{i}.icon", src_format="module:poke/%d",
                  need_bind=f"sv.tr.p{i}.game_icon"),
              img([x + 22, y + 6, 136, 136], bind=f"sv.p{i}.spr", src_format=SPRITE, need_bind=f"sv.p{i}.has_spr"),
              lab([x + 90, y + 140, 0, 28], bind=f"sv.p{i}.name", scale=5, color=INK, align="center", **IT, need_bind=v),
              lab([x + 112, y + 10, 40, 22], "NV.", scale=3, color=GREY, need_bind=v),
              val([x + 172, y + 8, 0, 0], f"sv.p{i}.level", scale=4, color=INK, align="right", need_bind=v)]
    w += [lab([24, 950, 1192, 12], bind="sv.status", scale=2, color="#FFFFE08A", need_bind="sv.warn")]
    return {"id": "card", "widgets": w + tab_bar("card")}


def anchor_text(pages):
    """Eden Duo anchors label/value text at rect x: centred text is centred ON x and
    right-aligned text ENDS at x (mod_ui.cpp DrawLabel/DrawValue, DrawTextAligned). The widgets
    above are written as boxes, so move x to the box's centre / right edge and make the text
    unsized; the box width is kept as "_box_w" (ignored by the runtime) for tools/preview.py."""
    for page in pages:
        for w in page["widgets"]:
            if w.get("type") not in ("label", "value") or w.get("align") not in ("center", "right"):
                continue
            x, y, bw, bh = w["rect"]
            w["_box_w"] = bw
            w["rect"] = [x + (bw // 2 if w["align"] == "center" else bw), y, 0, 0]
    return pages


HAVE_COVER = False


# ---------------------------------------------------------------- light theme (v0.9)
# Every widget whose look depends on the theme is written twice: the dark one as built above
# and a light copy (light colours, pictures from art/l/), each shown only in its theme. The
# theme is the flag "theme" (0 dark, 1 light). A widget that already uses need_bind and
# hide_bind gets a derived gate "<need> and theme"; a repeated one is first unrolled.
ART_DIR = pathlib.Path("art")
THEME_GATES = []
_GATE_NAMES = set()
LIGHT_COLORS = {
    "scarlet": {"#FFF1E4": "#2A1A1E", "#C4A7A9": "#7A6468", "#E3CFC8": "#4A3438", "#FEFEFE": "#241216",
                "#F2B544": "#A8740A", "#FF8A1F": "#D2600A", "#FF9A8C": "#C0392B", "#8FE3B0": "#23804A",
                "#9FEFFF": "#1C7FA0", "#FFD166": "#A8740A", "#FFE08A": "#8A6A00", "#B8F0FF": "#2A6A80",
                "#FFE6D8": "#7A4A30", "#1C0E13": "#E6DCD2"},
    "violet": {"#F6EFE6": "#221C2E", "#A79FB5": "#6E6680", "#CFC7DA": "#4A4258", "#FEFEFE": "#1E1830",
               "#C9A2FF": "#6A3CC6", "#FF9A8C": "#C0392B", "#8FE3B0": "#23804A", "#9FEFFF": "#1C7FA0",
               "#FFD166": "#A8740A", "#FFE08A": "#8A6A00", "#B8F0FF": "#2A6A80", "#FFE6D8": "#5A4A70",
               "#15131C": "#E2DCEC"},
}


def _lcol(c, cmap):
    """ARGB "#AARRGGBB" -> its light-theme colour (alpha kept), or None when unchanged."""
    if not isinstance(c, str) or len(c) != 9:
        return None
    rgb = "#" + c[3:].upper()
    if rgb in cmap:
        return c[:3] + cmap[rgb][1:]
    if rgb == "#FFFFFF" and c[:3] != "#FF":   # translucent white text (the version line)
        return c[:3] + "241216"
    return None


def _lsrc(src):
    if not isinstance(src, str) or not src.startswith("file:art/"):
        return None
    rel = src[len("file:art/"):]
    dark, light = ART_DIR / rel, ART_DIR / "l" / rel
    if not light.exists() or (dark.exists() and dark.read_bytes() == light.read_bytes()):
        return None
    return "file:art/l/" + rel


def _light_copy(w, cmap):
    lw, changed = copy.deepcopy(w), False
    if w.get("type") in ("label", "value"):
        c = _lcol(w.get("color"), cmap)
        if c:
            lw["color"], changed = c, True
    elif w.get("type") == "image":
        if "src" in w:
            ls = _lsrc(w["src"])
            if ls:
                lw["src"], changed = ls, True
        if "src_names" in w:
            names = [(_lsrc(x) or x) for x in w["src_names"]]
            if names != w["src_names"]:
                lw["src_names"], changed = names, True
    elif w.get("type") == "bar":
        c = _lcol(w.get("bg"), cmap)
        if c:
            lw["bg"], changed = c, True
    return lw if changed else None


def _unroll(w):
    out = []
    for i in range(w["repeat"]):
        txt = json.dumps({k: v for k, v in w.items() if not k.startswith("repeat")}, ensure_ascii=False)
        c = json.loads(txt.replace("{i}", str(i)))
        x, y = c["rect"][0], c["rect"][1]
        if w.get("repeat_cols"):
            x += (i % w["repeat_cols"]) * w.get("repeat_dx", 0)
            y += (i // w["repeat_cols"]) * w.get("repeat_row_dy", 0)
        else:
            x += i * w.get("repeat_dx", 0)
            y += i * w.get("repeat_dy", 0)
        c["rect"] = [x, y] + c["rect"][2:]
        out.append(c)
    return out


def _gated(w, light):
    w = copy.deepcopy(w)
    if "hide_bind" not in w:
        w.pop("keep_min", None)
        w.pop("keep_max", None)
        w["hide_bind"] = "ui.light"
        if light:
            w["keep_min"] = 1
        else:
            w["keep_max"] = 0
        return [w]
    if "need_bind" not in w:
        w["need_bind"] = "ui.light" if light else "ui.dark"
        return [w]
    if "{i}" in w["need_bind"] and w.get("repeat"):
        return [x for c in _unroll(w) for x in _gated(c, light)]
    need = w["need_bind"]
    name = ("ui.L|" if light else "ui.D|") + need
    if name not in _GATE_NAMES:
        _GATE_NAMES.add(name)
        THEME_GATES.append({"name": name, "expr": f"({need}) && " + ("ui.light" if light else "!ui.light")})
    w["need_bind"] = name
    return [w]


def themeify(pages, theme):
    THEME_GATES.clear()
    _GATE_NAMES.clear()
    THEME_GATES.extend([{"name": "ui.light", "cmp": "eq", "a": "@flag:theme", "b": 1},
                        {"name": "ui.dark", "cmp": "eq", "a": "@flag:theme", "b": 0}])
    cmap = LIGHT_COLORS[theme]
    for page in pages:
        out = []
        for w in page["widgets"]:
            lw = _light_copy(w, cmap)
            if lw is None:
                out.append(w)
            else:
                out += _gated(w, False) + _gated(lw, True)
        page["widgets"] = out
    return pages


def manifest(game_key):
    g = GAMES[game_key]
    set_theme(g["theme"])
    BATTLE_GATES.clear()
    CARD_GATES.clear()
    actions = {f"sel{i}": {"kind": "flag", "flag": "sel", "value": i} for i in range(6)}
    actions.update({f"dtab{k}": {"kind": "flag", "flag": "dtab", "value": k} for k in range(3)})
    actions.update(page_actions())
    actions["theme_toggle"] = {"kind": "flag", "flag": "theme", "cycle": 2}
    for key, _ in TABS:
        actions[f"go_{key}"] = {"kind": "page", "page": key, "transition": "fade", "duration_ms": 160}
    party = [img([0, 0, W, H], A("bg.png"))] + top_bar() + party_cards() + detail_sheets() + [
        lab([24, 940, 560, 22], "Toca un Pokémon para ver su ficha.", scale=3, color=GREY,
            hide_bind="sv.warn", keep_max=0),
        lab([24, 940, 580, 22], bind="sv.status", scale=3, color="#FFFFE08A", need_bind="sv.warn"),
        lab([620, 946, 600, 10], bind="sv.scan.text", scale=2, color="#FFFFE08A"),
        lab([620, 958, 600, 10], bind="sv.learn.text", scale=2, color="#FFB8F0FF"),
    ] + tab_bar("party")
    # Scarlet shows the player's own cover art when there is one (art/splash_s.png, not in the public
    # repository: it is fan art); otherwise both games use the generated start screen.
    if game_key == "PokemonScarlet" and HAVE_COVER:
        O2 = "#C0400A14"
        waiting = [
            img([0, 0, W, H], A("splash_s.png")),
            lab([40, 120, 340, 56], "POKÉMON", scale=12, color=WHITE, outline=O2, outline_px=4, **IT),
            lab([40, 190, 340, 56], "ESCARLATA", scale=12, color=WHITE, outline=O2, outline_px=4, **IT),
            lab([42, 280, 340, 34], "Compañero de", scale=7, color="#FFFFD27A", outline=O2, outline_px=3, **IT),
            lab([42, 320, 340, 34], "doble pantalla", scale=7, color="#FFFFD27A", outline=O2, outline_px=3, **IT),
            lab([42, 820, 340, 30], "Carga tu partida:", scale=6, color="#FFFFF1E6", outline=O2, outline_px=2),
            lab([42, 856, 340, 30], "aquí verás tu equipo", scale=6, color="#FFFFF1E6", outline=O2, outline_px=2),
            lab([42, 892, 340, 30], "y la ficha del rival.", scale=6, color="#FFFFF1E6", outline=O2, outline_px=2),
            lab([42, 1010, 340, 26], bind="sv.status", scale=3, color="#FFFFE6D8", need_bind="sv.warn"),
            lab([1060, 1040, 160, 24], VERSION, scale=4, color="#E0FFFFFF", align="right", outline=O2, outline_px=2),
        ]
    else:
        waiting = [
            img([0, 0, W, H], A("splash_v.png")),
            lab([96, 330, 900, 100], g["title"].upper(), scale=20, color=HEAD, **IT),
            lab([100, 450, 820, 44], "COMPAÑERO DE DOBLE PANTALLA", scale=9, color=ACC2, **IT),
            lab([100, 530, 820, 34], "Carga tu partida: aquí verás tu equipo", scale=6, color=SOFT),
            lab([100, 570, 820, 34], "y, en combate, la ficha del rival.", scale=6, color=SOFT),
            lab([60, 1010, 1120, 30], bind="sv.status", scale=3, color="#FFFFE6D8", need_bind="sv.warn"),
            lab([1060, 1040, 160, 24], VERSION, scale=4, color="#B0FFFFFF", align="right"),
        ]

    pages = themeify(anchor_text([
        {"id": "waiting", "widgets": waiting},
        {"id": "party", "widgets": party},
        dex_page(),
        bag_page(),
        map_page(),
        trainer_page(game_key),
        battle_page(),
    ]), g["theme"])
    return {
        "format": 1,
        "title_id": g["title_id"],
        "name": g["name"],
        "_about": f"Scarlet/Violet companion {VERSION} (read-only): live party with a detail sheet (moves, "
                  "data, weaknesses), a battle page with the rival found by the module (its name on screen, "
                  "its HP...) and its matchups, Pokédex, bag, the region's map with you on it and a trainer "
                  "card. Ships no game text or art: names, data, icons and the map come from the player's own "
                  "game files (romfs); Pokémon pictures optionally from the player's user: folder. The UI art "
                  "and the font (Barlow Condensed, SIL OFL 1.1) are the companion's own.",
        "min_runtime": 17,
        "requires_module": True,
        "canvas_w": W,
        "canvas_h": H,
        "background": PAGE_BG,
        "font": "file:font/barlow.mfnt",
        "font_atlas": "file:font/barlow.png",
        "flags": {"sel": 0, "dtab": 0, "theme": 0},
        # runtime 17: the theme as a setting, so Eden Duo keeps it between sessions
        "settings": [{"flag": "theme", "label": "Tema", "type": "choice", "choices": ["Oscuro", "Claro"],
                      "default": 0}],
        # Which slot / detail tab is shown is decided here, not by the module: on some runtimes
        # a module's sample() runs before the tick's flags are published and only ever sees the
        # default (the Super Mario Odyssey module documents the same). Derived values are
        # evaluated after the flags, in this order.
        "derived": derived_gates() + page_gates() + [{"name": k, "expr": v} for k, v in BATTLE_GATES.items()]
                   + [{"name": k, "expr": v} for k, v in CARD_GATES.items()] + THEME_GATES,
        "debug_page": True,
        "actions": actions,
        "pages": pages,
        "page_binds": [
            {"point": "sv.home", "equals": 0,
             "when_equal": {"page": "waiting", "transition": "fade", "duration_ms": 200},
             "when_not_equal": {"page": "party", "transition": "fade", "duration_ms": 200}},
            # Edge-triggered: into a battle shows the battle page, out of it goes back to the party.
            {"point": "sv.battle", "equals": 1, "ready_bind": "sv.home",
             "when_equal": {"page": "battle", "transition": "fade", "duration_ms": 160},
             "when_not_equal": {"page": "party", "transition": "fade", "duration_ms": 160}},
        ],
    }


def main(root, art):
    global HAVE_COVER
    root, art = pathlib.Path(root), pathlib.Path(art)
    HAVE_COVER = (art / "splash_s.png").exists()
    global ART_DIR
    for key in GAMES:
        d = root / key / "dualscreen"
        src = art if GAMES[key]["theme"] == "scarlet" else pathlib.Path(str(art) + "_v")
        if not src.exists():
            src = art
        if (d / "art").exists():
            shutil.rmtree(d / "art")
        shutil.copytree(src, d / "art", ignore=shutil.ignore_patterns("barlow.*", "nunito.*", "LICENSE-*"))
        if (d / "font").exists():
            shutil.rmtree(d / "font")
        (d / "font").mkdir(parents=True, exist_ok=True)
        for f in ["barlow.mfnt", "barlow.png"]:
            shutil.copy(art / f, d / "font" / f)
        shutil.copy(art / "LICENSE-Barlow-OFL.txt", d / "font" / "LICENSE-Barlow-OFL.txt")
        ART_DIR = src
        (d / "manifest.json").write_text(json.dumps(manifest(key), indent=1, ensure_ascii=False) + "\n")
        print("wrote", d)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "packages", sys.argv[2] if len(sys.argv) > 2 else "art")
