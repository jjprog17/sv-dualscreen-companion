#!/usr/bin/env python3
"""Builds a synthetic Scarlet/Violet romfs for native/test_host: arc/data.trpfd + arc/data.trpfs
with Trinity packs holding avalon/data/personal_array.bin, avalon/data/waza_array.bin and the
Spanish common .dat texts, in the layouts pkNX documents (TrinityFileDescriptors,
TrinityFileSystemMetadata, TrinityPak, PersonalTable, WazaTable, TextFile). Content comes from
PKHeX (personal_sv, Spanish lists, MoveInfo9) and PokeAPI (move categories/power), so the module
can be checked against known answers.

Options:
  --oodle FILE    store personal_array.bin as this raw Oodle stream instead (ooz testdata,
                  4-byte size header) to exercise decompression; parsing it then fails on purpose.
  --loose-waza    also write a loose avalon/data/waza_array.bin (a 'mod') where Tackle is Fire.
Usage: make_test_romfs.py out_dir pkhex_core moves.csv [--oodle F] [--loose-waza]
"""
import csv
import os
import re
import struct
import sys

import flatbuffers

FNV_OFF, FNV_PRIME = 0xCBF29CE484222645, 0x100000001B3


def fnv(s):
    h = FNV_OFF
    for b in s.encode():
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def vec_u64(b, vals):
    b.StartVector(8, len(vals), 8)
    for v in reversed(vals):
        b.PrependUint64(v)
    return b.EndVector()


def vec_u8(b, data):
    return b.CreateByteVector(bytes(data))


def vec_off(b, offs):
    b.StartVector(4, len(offs), 4)
    for o in reversed(offs):
        b.PrependUOffsetTRelative(o)
    return b.EndVector()


def finish(b, root):
    b.Finish(root)
    return bytes(b.Output())


# ---------- game data payloads ----------
def personal_array(core):
    """PersonalTable from PKHeX personal_sv: one entry per (national, form)."""
    raw = open(os.path.join(core, "Resources/byte/personal/personal_sv"), "rb").read()
    n = len(raw) // 0x50
    entries = []
    for sp in range(1, 1026):
        e = raw[sp * 0x50:(sp + 1) * 0x50]
        fcount, fstats = e[0x1A], struct.unpack_from("<H", e, 0x18)[0]
        for form in range(max(1, fcount)):
            idx = sp if form == 0 or not fstats else fstats + form - 1
            if idx >= n:
                continue
            r = raw[idx * 0x50:(idx + 1) * 0x50]
            dexp, h, w = struct.unpack_from("<HHH", r, 0x1E)
            entries.append((sp, form, r[6], r[7], dexp, r[0x1D], r[0x4A], r[0x4B], h, w,
                            r[0x0F], (r[0], r[1], r[2], r[4], r[5], r[3])))
    b = flatbuffers.Builder(1 << 20)
    tabs = []
    i2n = int2nat()
    n2i = {v: k for k, v in i2n.items()}
    for sp, form, t1, t2, dexp, grp, dexk, dexb, h, w, growth, base in entries:
        b.StartObject(23)
        b.Prep(4, 24)  # struct PersonalInfoDetail, written back to front
        for v, fmt in reversed([(n2i.get(sp, sp), "H"), (form, "H"), (sp, "H"), (0, "B"), (0, "B"), (h, "H"),
                                (w, "H"), (0, "I"), (0, "I"), (0, "I")]):
            {"H": b.PrependUint16, "B": b.PrependUint8, "I": b.PrependUint32}[fmt](v)
        b.PrependStructSlot(0, b.Offset(), 0)
        b.PrependBoolSlot(1, True, False)
        if dexp:
            b.Prep(2, 4)  # struct PersonalInfoDex {u16 Index; u8 Group}
            b.Pad(1)
            b.PrependUint8(grp)
            b.PrependUint16(dexp)
            b.PrependStructSlot(2, b.Offset(), 0)
        b.Prep(1, 6)  # struct PersonalInfoStats Base {HP, ATK, DEF, SPA, SPD, SPE}
        for v in reversed(base):
            b.PrependUint8(v)
        b.PrependStructSlot(22, b.Offset(), 0)
        b.PrependUint8Slot(10, growth, 0)
        b.PrependUint16Slot(3, dexk, 0)
        b.PrependUint16Slot(4, dexb, 0)
        b.PrependUint8Slot(5, t1, 0)
        b.PrependUint8Slot(6, t2, 0)
        b.PrependUint16Slot(7, 65, 0)
        tabs.append(b.EndObject())
    table = vec_off(b, tabs)
    b.StartObject(1)
    b.PrependUOffsetTRelativeSlot(0, table, 0)
    return finish(b, b.EndObject()), len(entries)


def int2nat():
    """internal -> national for Gen 9 species, from the module's own kInt2Nat table."""
    src = open(os.path.join(os.path.dirname(__file__), "..", "native", "sv_module.c"), encoding="utf-8").read()
    body = re.search(r"kInt2Nat\[\] = \{(.*?)\};", src, re.S).group(1)
    deltas = [int(v) for v in re.findall(r"-?\d+", body)]
    return {917 + i: 917 + i + d for i, d in enumerate(deltas)}


def waza_array(core, moves_csv, tackle_type=None):
    src = open(os.path.join(core, "Moves/MoveInfo9.cs"), encoding="utf-8").read()
    span = lambda name: [int(v) for v in re.findall(r"\d+", re.search(
        r"ReadOnlySpan<byte>\s+" + name + r"\s*=>\s*\[(.*?)\];", src, re.S).group(1))]
    pp, typ = span("PP"), span("Type")
    cat, pw = {}, {}
    for row in csv.DictReader(open(moves_csv, encoding="utf-8")):
        i = int(row["id"])
        cat[i] = {"1": 0, "2": 1, "3": 2}.get(row["damage_class_id"], 0)
        pw[i] = min(255, int(row["power"] or 0))
    b = flatbuffers.Builder(1 << 18)
    tabs = []
    for mid in range(1, len(pp)):
        t = typ[mid] if not (tackle_type is not None and mid == 33) else tackle_type
        b.StartObject(12)
        b.PrependUint16Slot(0, mid, 0)
        b.PrependBoolSlot(1, True, False)
        b.PrependUint8Slot(2, t, 0)
        b.PrependUint8Slot(4, cat.get(mid, 0), 0)
        b.PrependUint8Slot(5, pw.get(mid, 0), 0)
        b.PrependUint8Slot(6, 100, 0)
        b.PrependUint8Slot(7, pp[mid], 0)
        tabs.append(b.EndObject())
    table = vec_off(b, tabs)
    b.StartObject(1)
    b.PrependUOffsetTRelativeSlot(0, table, 0)
    return finish(b, b.EndObject())


def ahtb(labels):
    """AHTB label table: "AHTB", count, then {u64 FNV-1a 64 hash, u16 length, name + NUL}."""
    out = bytearray(b"AHTB" + struct.pack("<I", len(labels)))
    for lab in labels:
        h = 0xCBF29CE484222645
        for ch in lab.encode():
            h = ((h ^ ch) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        name = lab.encode() + b"\0"
        out += struct.pack("<QH", h, len(name)) + name
    return bytes(out)


def place_name_fb(entries):
    """pkNX PlaceNameDataTableArray: {Table: [{ID, PlaceNameMstxtLabl, Field3DPos{X,Y,Z}, ZoomOut,
    ZoomNormal, ZoomIn}]}, written front to back with every uoffset patched forward."""
    b = bytearray(4)
    patches = []                       # (field position, key of the target)
    targets = {}

    def pad4():
        while len(b) % 4:
            b.append(0)

    def table(fields):                 # fields: list of (kind, value) by id; kind: off/f32/bool/None
        pad4()
        vt = len(b)
        n = len(fields)
        vlen, tlen = 4 + 2 * n, 4 + 4 * n
        b.extend(struct.pack("<HH", vlen, tlen))
        offs = [0 if k is None else 4 + 4 * i for i, (k, v) in enumerate(fields)]
        b.extend(struct.pack("<%dH" % n, *offs))
        pad4()
        tab = len(b)
        b.extend(struct.pack("<i", tab - vt))
        for i, (k, v) in enumerate(fields):
            pos = len(b)
            if k == "off":
                patches.append((pos, v)); b.extend(b"\0\0\0\0")
            elif k == "f32":
                b.extend(struct.pack("<f", v))
            elif k == "bool":
                b.extend(bytes([1 if v else 0, 0, 0, 0]))
            else:
                b.extend(b"\0\0\0\0")
        return tab

    root = table([("off", "vec")])
    struct.pack_into("<I", b, 0, root)
    pad4()
    targets["vec"] = len(b)
    b.extend(struct.pack("<I", len(entries)))
    for i in range(len(entries)):
        patches.append((len(b), f"e{i}")); b.extend(b"\0\0\0\0")
    for i, (ident, label, x, y, z, zo, zn, zi) in enumerate(entries):
        targets[f"e{i}"] = table([("off", f"id{i}"), ("off", f"lab{i}"), ("off", f"pos{i}"),
                                  ("bool", zo), ("bool", zn), ("bool", zi)])
        targets[f"pos{i}"] = table([("f32", x), ("f32", y), ("f32", z)])
        for key, text in ((f"id{i}", ident), (f"lab{i}", label)):
            pad4()
            targets[key] = len(b)
            raw = text.encode()
            b.extend(struct.pack("<I", len(raw)) + raw + b"\0")
    for pos, key in patches:
        struct.pack_into("<I", b, pos, targets[key] - pos)
    return bytes(b)


def text_dat(lines, inject=False):
    """pkNX TextFile writer: XOR key 0x7C89 (+0x2983 per line), rotate-left-3 per char."""
    enc, key = [], 0x7C89
    for i, line in enumerate(lines):
        words = [ord(c) for c in line]
        if inject and i == 3:  # a line with a variable (0x10, count, code, arg) to exercise the skip
            words = [ord("V"), 0x10, 2, 0xFF00, 7, ord("Z")]
        words.append(0)
        if len(words) % 2:
            words.append(0)  # keep lines 4-byte aligned like the game
        k, out = key, bytearray()
        for w in words:
            out += struct.pack("<H", w ^ k)
            k = ((k << 3) | (k >> 13)) & 0xFFFF
        enc.append(out)
        key = (key + 0x2983) & 0xFFFF
    sec = bytearray(struct.pack("<I", 0))
    offs = 4 + 8 * len(lines)
    table = bytearray()
    for e in enc:
        table += struct.pack("<IHH", offs, len(e) // 2, 0)
        offs += len(e)
    sec += table + b"".join(enc)
    struct.pack_into("<I", sec, 0, len(sec))
    hdr = struct.pack("<HHIII", 1, len(lines), len(sec), 0, 0x10)
    return hdr + bytes(sec)


# ---------- BNTX (Switch textures), block-linear like the game's ----------
def bl_offset(x, y, bpp, bh, gobs):
    gob = (y // (8 * bh)) * 512 * bh * gobs + (x * bpp // 64) * 512 * bh + (y % (8 * bh) // 8) * 512
    xb = x * bpp
    return gob + ((xb % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((xb % 32) // 16) * 32 + (y % 2) * 16 + (xb % 16)


def bntx(w, h, fmt, block):
    """One texture; `block(x, y)` gives the bytes of texel/block (x, y). fmt: 0x0B01 RGBA8, 0x1A01 BC1."""
    bw, bpp = (4, 8) if fmt >> 8 == 0x1A else (4, 16) if fmt >> 8 == 0x20 else (1, 4)
    gw, gh = (w + bw - 1) // bw, (h + bw - 1) // bw
    bhl = 2
    bh = 1 << bhl
    gobs = (gw * bpp + 63) // 64
    rows = (gh + 8 * bh - 1) // (8 * bh)
    data = bytearray(rows * 512 * bh * gobs)
    for y in range(gh):
        for x in range(gw):
            o = bl_offset(x, y, bpp, bh, gobs)
            data[o:o + bpp] = block(x, y)
    hdr = bytearray(0x40)
    hdr[0:4] = b"BNTX"
    hdr[0x20:0x24] = b"NX  "
    info, name, mips, dat = 0x40, 0x100, 0x120, 0x200
    struct.pack_into("<IQQ", hdr, 0x24, 1, 0x38, dat)
    struct.pack_into("<Q", hdr, 0x38, info)
    brti = bytearray(0xC0)
    brti[0:4] = b"BRTI"
    struct.pack_into("<H", brti, 0x16, 1)
    struct.pack_into("<IIIIII", brti, 0x1C, fmt, 0, w, h, 1, 1)
    struct.pack_into("<I", brti, 0x34, bhl)
    struct.pack_into("<II", brti, 0x50, len(data), 512)
    brti[0x58:0x5C] = bytes([2, 3, 4, 5])
    struct.pack_into("<QQQ", brti, 0x60, name, 0, mips)
    f = bytearray(dat)
    f[0:0x40] = hdr
    f[info:info + len(brti)] = brti
    struct.pack_into("<H", f, name, 4); f[name + 2:name + 6] = b"icon"
    struct.pack_into("<Q", f, mips, dat)
    return bytes(f + data)


def test_textures():
    red = bntx(64, 64, 0x0B01, lambda x, y: bytes([220, 30, 30, 255]))
    # BC1 block: c0 = pure green (565: 0x07E0), c1 = 0, indices 0 -> every texel c0
    green = bntx(64, 64, 0x1A01, lambda x, y: struct.pack("<HHI", 0x07E0, 0, 0))
    blue = bntx(32, 32, 0x0B01, lambda x, y: bytes([30, 60, 220, 255]))

    def bc7_mode6(r, g, b, a, p):  # solid colour: both endpoints equal, every index 0
        v, pos = 0, 0
        def put(val, n):
            nonlocal v, pos
            v |= (val & ((1 << n) - 1)) << pos
            pos += n
        put(1 << 6, 7)
        for c in (r, g, b, a):
            put(c, 7); put(c, 7)
        put(p, 1); put(p, 1)
        return v.to_bytes(16, "little")
    mapimg = bntx(256, 256, 0x0B01, lambda x, y: bytes([40 + x // 2, 120 + y // 4, 90, 255]))
    orange = bntx(48, 48, 0x2001, lambda x, y: bc7_mode6(100, 50, 25, 127, 1))
    return [("appli/tex/icon_item/item_0017/item_0017.bntx", red, -1, len(red)),
            ("appli/tex/icon_item/item_0018/item_0018.bntx", green, -1, len(green)),
            ("appli/tex/icon_item/item_0019/item_0019.bntx", orange, -1, len(orange)),
            ("appli/tex/ymap_ter_small/ymap_ter_small_1x1/ymap_ter_small_1x1.bntx", mapimg, -1, len(mapimg))] + [
            # the real map: 10 x 10 tiles NN_MM; colour = (first index, second index) for the tests
            ("appli/tex/ymap_ter_small/ymap_ter_small_%02d_%02d/ymap_ter_small_%02d_%02d.bntx" % (a, b2, a, b2), t, -1, len(t))
            for a in range(10) for b2 in range(10)
            for t in [bntx(32, 32, 0x0B01, lambda x, y, a=a, b2=b2: bytes([a * 20 + 10, b2 * 20 + 10, 200, 255]))]] + [
            ("pokemon/data/pm0025/pm0025_00_00/icon/pm0025_00_00_00.bntx", blue, -1, len(blue)),
            ]


# ---------- Trinity containers ----------
def trinity_pak(files):
    """files: list of (path, payload, ctype, usize)."""
    items = sorted(((fnv(p), d, c, u) for p, d, c, u in files), key=lambda x: x[0])
    b = flatbuffers.Builder(1 << 20)
    datas = [vec_u8(b, d) for _, d, _, _ in items]
    tabs = []
    for (h, d, c, u), dv in zip(items, datas):
        b.StartObject(5)
        b.PrependUint32Slot(0, 0, 0)
        b.PrependInt8Slot(1, c, 0)
        b.PrependUint8Slot(2, 0, 0)
        b.PrependUint64Slot(3, u, 0)
        b.PrependUOffsetTRelativeSlot(4, dv, 0)
        tabs.append(b.EndObject())
    files_v = vec_off(b, tabs)
    hashes_v = vec_u64(b, [h for h, *_ in items])
    b.StartObject(2)
    b.PrependUOffsetTRelativeSlot(0, hashes_v, 0)
    b.PrependUOffsetTRelativeSlot(1, files_v, 0)
    return finish(b, b.EndObject())


def build(out, packs):
    """packs: list of (pack_path, files)."""
    os.makedirs(os.path.join(out, "arc"), exist_ok=True)
    blobs = [(p, trinity_pak(f), f) for p, f in packs]
    fs = bytearray(b"ONEPACK\0" + b"\0" * 8)
    offsets = {}
    for p, blob, _ in blobs:
        while len(fs) % 0x100:
            fs += b"\0"
        offsets[p] = len(fs)
        fs += blob
    while len(fs) % 0x10:
        fs += b"\0"
    meta_ofs = len(fs)
    b = flatbuffers.Builder(4096)
    order = sorted(blobs, key=lambda x: fnv(x[0]))
    hv = vec_u64(b, [fnv(p) for p, *_ in order])
    ov = vec_u64(b, [offsets[p] for p, *_ in order])
    b.StartObject(2)
    b.PrependUOffsetTRelativeSlot(0, hv, 0)
    b.PrependUOffsetTRelativeSlot(1, ov, 0)
    fs += finish(b, b.EndObject())
    struct.pack_into("<Q", fs, 8, meta_ofs)
    open(os.path.join(out, "arc/data.trpfs"), "wb").write(fs)

    # descriptors
    subs = sorted(((fnv(fp), pi) for pi, (_, _, files) in enumerate(blobs) for fp, *_ in files))
    b = flatbuffers.Builder(1 << 16)
    paths = [b.CreateString(p) for p, *_ in blobs]
    infos = []
    for _, pi in subs:
        b.StartObject(1)
        b.PrependUint32Slot(0, 0, 0)
        unk = b.EndObject()
        b.StartObject(2)
        b.PrependUint64Slot(0, pi, 0)
        b.PrependUOffsetTRelativeSlot(1, unk, 0)
        infos.append(b.EndObject())
    finfos = []
    for p, blob, files in blobs:
        b.StartObject(2)
        b.PrependUint64Slot(0, len(blob), 0)
        b.PrependUint64Slot(1, len(files), 0)
        finfos.append(b.EndObject())
    sh = vec_u64(b, [h for h, _ in subs])
    pv = vec_off(b, paths)
    iv = vec_off(b, infos)
    fv = vec_off(b, finfos)
    b.StartObject(4)
    b.PrependUOffsetTRelativeSlot(0, sh, 0)
    b.PrependUOffsetTRelativeSlot(1, pv, 0)
    b.PrependUOffsetTRelativeSlot(2, iv, 0)
    b.PrependUOffsetTRelativeSlot(3, fv, 0)
    open(os.path.join(out, "arc/data.trpfd"), "wb").write(finish(b, b.EndObject()))


def main():
    out, core, moves_csv = sys.argv[1:4]
    oodle = sys.argv[sys.argv.index("--oodle") + 1] if "--oodle" in sys.argv else None
    personal, n_pers = personal_array(core)
    if oodle:
        raw = open(oodle, "rb").read()
        usize = struct.unpack_from("<I", raw, 0)[0]
        pers_entry = ("avalon/data/personal_array.bin", raw[4:], 3, usize)
    else:
        pers_entry = ("avalon/data/personal_array.bin", personal, -1, len(personal))
    waza = waza_array(core, moves_csv)
    lists = {"monsname": "other/es/text_Species_es.txt", "wazaname": "other/es/text_Moves_es.txt",
             "itemname": "items/text_Items_es.txt", "tokusei": "other/es/text_Abilities_es.txt",
             "seikaku": "other/es/text_Natures_es.txt", "typename": "other/es/text_Types_es.txt"}
    texts = []
    for name, src in lists.items():
        lines = open(os.path.join(core, "Resources/text", src), encoding="utf-8").read().split("\n")
        if name == "monsname":  # the game's list is in internal order
            i2n = int2nat()
            lines = [lines[i2n.get(i, i)] if i2n.get(i, i) < len(lines) else "" for i in range(len(lines))]
        d = text_dat(lines, inject=(name == "monsname"))
        texts.append((f"message/dat/Spanish/common/{name}.dat", d, -1, len(d)))
    # Pokédex / map texts (synthetic: the real ones are game text), internal species order
    extra = {"zkn_type": [f"Pokémon Prueba {i}" for i in range(1400)],
             "zukan_comment_A": [f"Descripción de la especie {i}. Vive en Paldea y le gusta el sol." for i in range(1400)],
             "place_name": [f"Lugar {i}" for i in range(400)]}
    for name, lines in extra.items():
        d = text_dat(lines)
        texts.append((f"message/dat/Spanish/common/{name}.dat", d, -1, len(d)))
    # the map's own place names: texts, labels and where they go (v0.7.2)
    ymap_lines = ["Nada", "Pueblo Artesanía", "Cueva Escondida"]
    ymap_labels = ["PLACE_NONE", "PLACE_T01", "PLACE_CAVE"]
    d = text_dat(ymap_lines)
    texts.append(("message/dat/Spanish/common/ymap_place_name.dat", d, -1, len(d)))
    t = ahtb(ymap_labels)
    texts.append(("message/dat/Spanish/common/ymap_place_name.tbl", t, -1, len(t)))
    pn = place_name_fb([("T01", "PLACE_T01", 1300.0, 80.0, -2300.0, True, True, True),
                        ("CAVE", "PLACE_CAVE", 4000.0, 20.0, -1000.0, False, False, True)])
    texts.append(("world/data/ui/ymap/ymap_place_name_data/ymap_place_name_data_array.bin", pn, -1, len(pn)))
    build(out, [
        ("arc/avalon_data.trpak", [pers_entry, ("avalon/data/waza_array.bin", waza, -1, len(waza))]),
        ("arc/message_spanish.trpak", texts),
        ("arc/appli_tex.trpak", test_textures()),
        ("arc/filler.trpak", [(f"dummy/file{i}.bin", bytes([i]) * 7, -1, 7) for i in range(40)]),
    ])
    if "--loose-waza" in sys.argv:
        os.makedirs(os.path.join(out, "avalon/data"), exist_ok=True)
        open(os.path.join(out, "avalon/data/waza_array.bin"), "wb").write(
            waza_array(core, moves_csv, tackle_type=9))
    print(f"romfs -> {out}: {n_pers} personal entries", "(personal = oodle stream)" if oodle else "")


if __name__ == "__main__":
    main()
