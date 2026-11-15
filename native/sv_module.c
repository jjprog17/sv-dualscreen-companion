/*
 * Pokémon Scarlet / Violet companion module for Eden Duo (v0.2, read-only).
 *
 * Freestanding C: no libc, no malloc. It only talks to the emulator through the
 * Eden Duo C ABI (dsmod_module_abi.h), so it builds for Android arm64 without the NDK.
 *
 * Game state (all from the running game's RAM, nothing from the package):
 *   - the save-block key table (SCBlock accessor), found through the public pointer
 *     [main+0x47350D8, 0xD8, 0x0, 0x0, 0x30, 0x08] (Tera-Finder / RaidCrawler / SysBot; the same
 *     offset in game 3.0.0, 3.0.1 and 4.0.0);
 *   - party block 0x3AA1A9AD, money 0x4F35D0DD, League Points 0xADB4FE17,
 *     MyStatus 0xE3E89BD1 (block keys from PKHeX SaveBlockAccessor9SV);
 *   - MyStatus also through SysBot's pointer [main+0x47350D8,0xD8,0x8,0xB8,0x0,0x40], used as a
 *     cross-check that the key-table route is right (sv.check.mystatus).
 * Blocks are XorShift32-encrypted in RAM (PKHeX SCXorShift32); Pokémon are PK9 (PK8 crypto).
 *
 * Reference data the player supplies (the `user:` source, never shipped in the package):
 *   species/moves/items/abilities/natures/types .txt (one name per line, line N = id N),
 *   personal.bin (PKHeX personal_sv: species/form types) and movedata.bin (type, category,
 *   base PP, power per move id). Every piece is optional: what is missing shows as a number.
 * The type chart is the standard public Gen 6+ chart, in the game's type order.
 *
 * Page state the manifest drives: flags "sel" (selected party slot) and "dtab" (detail tab).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dsmod_module_abi.h"
#include "dsmod_module_extensions.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
typedef int32_t s32;

#ifndef SV_TITLE_ID
#define SV_TITLE_ID 0x0100A3D008C5C000ULL /* Scarlet */
#endif

#define EXPORT __attribute__((visibility("default")))
#define LOCAL static

/* Memory functions live in sv_alloc.c (shared with the Oodle decoder). */
void* memcpy(void* d, const void* s, size_t n);
void* memset(void* d, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
void* memmove(void* d, const void* s, size_t n);

#include "sv_game.h"

LOCAL u16 le16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
LOCAL u32 le32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
LOCAL int strcmp_(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b; }
LOCAL u64 le64(const u8* p) { return (u64)le32(p) | ((u64)le32(p + 4) << 32); }

/* Tiny string builder. */
typedef struct { char* p; size_t cap; size_t len; } Str;
LOCAL void s_init(Str* s, char* buf, size_t cap) { s->p = buf; s->cap = cap; s->len = 0; buf[0] = 0; }
LOCAL void s_ch(Str* s, char c) { if (s->len + 1 < s->cap) { s->p[s->len++] = c; s->p[s->len] = 0; } }
LOCAL void s_cat(Str* s, const char* t) { while (t && *t) s_ch(s, *t++); }
LOCAL void s_u(Str* s, u64 v, int min_digits) {
    char tmp[24]; int n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 23);
    while (n < min_digits && n < 23) tmp[n++] = '0';
    while (n) s_ch(s, tmp[--n]);
}
LOCAL void s_utf8(Str* s, u32 cp) {
    if (cp < 0x80) s_ch(s, (char)cp);
    else if (cp < 0x800) { s_ch(s, (char)(0xC0 | (cp >> 6))); s_ch(s, (char)(0x80 | (cp & 0x3F))); }
    else { s_ch(s, (char)(0xE0 | (cp >> 12))); s_ch(s, (char)(0x80 | ((cp >> 6) & 0x3F))); s_ch(s, (char)(0x80 | (cp & 0x3F))); }
}
/* UTF-16LE string (max_chars, 0-terminated) -> UTF-8. Gen 8/9 store ♂/♀ as private-use glyphs. */
LOCAL void s_utf16(Str* s, const u8* p, int max_chars) {
    for (int i = 0; i < max_chars; ++i) {
        u32 c = le16(p + 2 * i);
        if (c == 0) break;
        if (c == 0xE08E) c = 0x2642; /* ♂ */
        if (c == 0xE08F) c = 0x2640; /* ♀ */
        if (c >= 0xD800 && c <= 0xDFFF) c = '?';
        s_utf8(s, c);
    }
}

/* ---------- game constants ---------- */
#define PTR_ROOT 0x47350D8ULL
LOCAL const u64 kKeyBlockChain[] = {PTR_ROOT, 0xD8, 0x0, 0x0, 0x30, 0x08};
LOCAL const u64 kMyStatusChain[] = {PTR_ROOT, 0xD8, 0x8, 0xB8, 0x0, 0x40};

#define KEY_PARTY 0x3AA1A9ADu
#define KEY_MONEY 0x4F35D0DDu
#define KEY_LP 0xADB4FE17u
#define KEY_MYSTATUS 0xE3E89BD1u

#define SC_OBJECT 4
#define SC_UINT32 10

#define PK_STORED 0x148
#define PK_PARTY 0x158
#define PARTY_BLOCK_MIN (6 * PK_PARTY + 1)
#define MYSTATUS_SIZE 0x68
#define KEY_ENTRY_SIZE 48
#define MAX_KEY_ENTRIES 0x8000

/* Supported executables: first 16 hex digits of the main NSO build ID. The pointer above is
 * the same in all of them (SysBot/Tera-Finder history). Build IDs: cheat databases
 * (ChanseyIsTheBest/NX-60FPS-RES-GFX-Cheats, cheatslips), dated against each release. */
LOCAL const char* const kBuilds[] = {
#if SV_TITLE_ID == 0x01008F6008C5E000ULL
    "E335DE2BF4DCD951", /* Violet 3.0.0 */
    "709BFD6611529864", /* Violet 3.0.1 */
#else
    "C01884F294C053C4", /* Scarlet 3.0.0 */
    "421C5411B487EB4D", /* Scarlet 3.0.1 */
#endif
};

/* ---------- guest reads ---------- */
LOCAL int rd(const EdenDsmodHostApi* h, u64 a, void* out, size_t n) {
    if (a == 0 || a + n < a) return 0;
    return h->read_memory(h->userdata, a, out, n) == EDEN_DSMOD_TRUE;
}
LOCAL int rd64(const EdenDsmodHostApi* h, u64 a, u64* v) {
    u8 b[8];
    if (!rd(h, a, b, 8)) return 0;
    *v = le64(b);
    return 1;
}
/* sys-botbase "pointerAll": read [main+c0], then read [p+ci] for the middle hops, add the last. */
LOCAL u64 chain(const EdenDsmodHostApi* h, const u64* c, int n) {
    u64 p;
    if (!rd64(h, h->main_base + c[0], &p) || p == 0) return 0;
    for (int i = 1; i < n - 1; ++i)
        if (!rd64(h, p + c[i], &p) || p == 0) return 0;
    return p + c[n - 1];
}

/* ---------- SCBlock in RAM ---------- */
LOCAL u32 xs_adv(u32 s) { s ^= s << 2; s ^= s >> 15; s ^= s << 13; return s; }
LOCAL void sc_crypt(u32 key, u8* d, size_t n) {
    u32 st = key;
    int pc = 0;
    for (u32 k = key; k; k &= k - 1) ++pc; /* popcount without libgcc */
    for (int i = 0; i < pc; ++i) st = xs_adv(st);
    int ctr = 0;
    for (size_t i = 0; i < n; ++i) {
        d[i] ^= (u8)(st >> (ctr << 3));
        if (ctr == 3) { st = xs_adv(st); ctr = 0; } else ++ctr;
    }
}

typedef struct { u64 start, end; } KeyTable;

LOCAL int keytable_resolve(const EdenDsmodHostApi* h, KeyTable* kt) {
    u64 a = chain(h, kKeyBlockChain, 6);
    if (!a || !rd64(h, a, &kt->start) || !rd64(h, a + 8, &kt->end)) return 0;
    if (kt->start == 0 || kt->end <= kt->start) return 0;
    u64 span = kt->end - kt->start;
    if (span % KEY_ENTRY_SIZE != 0 || span / KEY_ENTRY_SIZE > MAX_KEY_ENTRIES) return 0;
    return 1;
}

/* Binary search exactly as Tera-Finder's GetBlockAddress; returns the block's data pointer. */
LOCAL u64 keytable_find(const EdenDsmodHostApi* h, const KeyTable* kt, u32 key) {
    u64 start = kt->start, end = kt->end;
    for (int guard = 0; start < end && guard < 40; ++guard) {
        u64 count = (end - start) / KEY_ENTRY_SIZE;
        u64 mid = start + (count >> 1) * KEY_ENTRY_SIZE;
        u8 b[4];
        if (!rd(h, mid, b, 4)) return 0;
        u32 found = le32(b);
        if (found == key) {
            u64 data;
            return rd64(h, mid + 8, &data) ? data : 0;
        }
        if (found >= key) end = mid; else start = mid + KEY_ENTRY_SIZE;
    }
    return 0;
}

LOCAL u8 g_blk[0x2000];
/* Object block: header = type(1) + size(4), then the payload; the whole run is one xorshift
 * stream. Copies the decrypted payload into g_blk and returns its size (0 on failure). */
LOCAL u32 read_object(const EdenDsmodHostApi* h, const KeyTable* kt, u32 key, u32 min_size) {
    u64 p = keytable_find(h, kt, key);
    u8 hdr[5];
    if (!p || !rd(h, p, hdr, 5)) return 0;
    sc_crypt(key, hdr, 5);
    u32 size = le32(hdr + 1);
    if (hdr[0] != SC_OBJECT || size < min_size || size + 5 > sizeof(g_blk)) return 0;
    if (!rd(h, p, g_blk, size + 5)) return 0;
    sc_crypt(key, g_blk, size + 5);
    memmove(g_blk, g_blk + 5, size);
    return size;
}
LOCAL int read_u32(const EdenDsmodHostApi* h, const KeyTable* kt, u32 key, u32* out) {
    u64 p = keytable_find(h, kt, key);
    u8 b[5];
    if (!p || !rd(h, p, b, 5)) return 0;
    sc_crypt(key, b, 5);
    if (b[0] != SC_UINT32) return 0;
    *out = le32(b + 1);
    return 1;
}

/* ---------- PK9 (same crypto as PK8) ---------- */
LOCAL const u8 kBlockPos[32 * 4] = {
    0,1,2,3, 0,1,3,2, 0,2,1,3, 0,3,1,2, 0,2,3,1, 0,3,2,1, 1,0,2,3, 1,0,3,2,
    2,0,1,3, 3,0,1,2, 2,0,3,1, 3,0,2,1, 1,2,0,3, 1,3,0,2, 2,1,0,3, 3,1,0,2,
    2,3,0,1, 3,2,0,1, 1,2,3,0, 1,3,2,0, 2,1,3,0, 3,1,2,0, 2,3,1,0, 3,2,1,0,
    0,1,2,3, 0,1,3,2, 0,2,1,3, 0,3,1,2, 0,2,3,1, 0,3,2,1, 1,0,2,3, 1,0,3,2,
};
LOCAL void pk_crypt(u8* d, size_t n, u32 seed) {
    for (size_t i = 0; i + 1 < n; i += 2) {
        seed = 0x41C64E6Du * seed + 0x6073u;
        u16 x = (u16)(seed >> 16);
        d[i] ^= (u8)x;
        d[i + 1] ^= (u8)(x >> 8);
    }
}
/* Decrypts a 0x158-byte party PK9 in place. Returns 1 when the checksum matches. */
LOCAL int pk9_decrypt(u8* d) {
    u32 pv = le32(d);
    u32 sv = (pv >> 13) & 31;
    pk_crypt(d + 8, PK_STORED - 8, pv);
    pk_crypt(d + PK_STORED, PK_PARTY - PK_STORED, pv);
    u8 tmp[PK_STORED - 8];
    const int bs = 0x50;
    for (int i = 0; i < 4; ++i)
        memcpy(tmp + i * bs, d + 8 + kBlockPos[sv * 4 + i] * bs, bs);
    memcpy(d + 8, tmp, sizeof(tmp));
    u16 sum = 0;
    for (int i = 8; i < PK_STORED; i += 2) sum = (u16)(sum + le16(d + i));
    return sum == le16(d + 6);
}

/* Internal gen-9 species id -> National Dex (PKHeX SpeciesConverter.Table9InternalToNational). */
LOCAL const signed char kInt2Nat[] = {
    65, -1, -1, -1, -1, 31, 31, 47, 47, 29, 29, 53, 31, 31, 46, 44, 30, 30, -7, -7, -7, 13, 13,
    -2, -2, 23, 23, 24, -21, -21, 27, 27, 47, 47, 47, 26, 14, -33, -33, -33, -17, -17, 3,
    -29, 12, -12, -31, -31, -31, 3, 3, -24, -24, -44, -44, -30, -30, -28, -28, 23, 23, 6, 7,
    29, 8, 3, 4, 4, 20, 4, 23, 6, 3, 3, 4, -1, 13, 9, 7, 5, 7, 9, 9,
    -43, -43, -43, -68, -68, -68, -58, -58, -25, -29, -31, 6, -1, 6, 0, 0, 0, 3, 3, 4,
    2, 3, 3, -5, -12, -12,
};
LOCAL u16 national9(u16 raw) {
    int shift = (int)raw - 917;
    if (shift < 0 || shift >= (int)sizeof(kInt2Nat)) return raw;
    return (u16)(raw + kInt2Nat[shift]);
}

/* National -> internal (the game's monsname.dat and the Pokédex blocks are internal-ordered). */
LOCAL u16 internal9(u16 nat) {
    if (nat < 917) return nat;
    for (u32 i = 0; i < sizeof(kInt2Nat); ++i)
        if (national9((u16)(917 + i)) == nat) return (u16)(917 + i);
    return nat;
}

/* ---------- type chart (game order) ---------- */
enum { T_NOR, T_FIG, T_FLY, T_POI, T_GRO, T_ROC, T_BUG, T_GHO, T_STE, T_FIR, T_WAT, T_GRA,
       T_ELE, T_PSY, T_ICE, T_DRA, T_DAR, T_FAI, T_COUNT };
/* (attacker, defender, effectiveness: 0 immune, 1 half, 4 double); everything else is 2. */
LOCAL const u8 kChartEx[][3] = {
    {T_NOR,T_ROC,1},{T_NOR,T_STE,1},{T_NOR,T_GHO,0},
    {T_FIG,T_NOR,4},{T_FIG,T_ROC,4},{T_FIG,T_STE,4},{T_FIG,T_ICE,4},{T_FIG,T_DAR,4},
    {T_FIG,T_FLY,1},{T_FIG,T_POI,1},{T_FIG,T_BUG,1},{T_FIG,T_PSY,1},{T_FIG,T_FAI,1},{T_FIG,T_GHO,0},
    {T_FLY,T_FIG,4},{T_FLY,T_BUG,4},{T_FLY,T_GRA,4},{T_FLY,T_ROC,1},{T_FLY,T_STE,1},{T_FLY,T_ELE,1},
    {T_POI,T_GRA,4},{T_POI,T_FAI,4},{T_POI,T_POI,1},{T_POI,T_GRO,1},{T_POI,T_ROC,1},{T_POI,T_GHO,1},{T_POI,T_STE,0},
    {T_GRO,T_POI,4},{T_GRO,T_ROC,4},{T_GRO,T_STE,4},{T_GRO,T_FIR,4},{T_GRO,T_ELE,4},
    {T_GRO,T_BUG,1},{T_GRO,T_GRA,1},{T_GRO,T_FLY,0},
    {T_ROC,T_FLY,4},{T_ROC,T_BUG,4},{T_ROC,T_FIR,4},{T_ROC,T_ICE,4},{T_ROC,T_FIG,1},{T_ROC,T_GRO,1},{T_ROC,T_STE,1},
    {T_BUG,T_GRA,4},{T_BUG,T_PSY,4},{T_BUG,T_DAR,4},{T_BUG,T_FIG,1},{T_BUG,T_FLY,1},{T_BUG,T_POI,1},
    {T_BUG,T_GHO,1},{T_BUG,T_STE,1},{T_BUG,T_FIR,1},{T_BUG,T_FAI,1},
    {T_GHO,T_GHO,4},{T_GHO,T_PSY,4},{T_GHO,T_DAR,1},{T_GHO,T_NOR,0},
    {T_STE,T_ROC,4},{T_STE,T_ICE,4},{T_STE,T_FAI,4},{T_STE,T_STE,1},{T_STE,T_FIR,1},{T_STE,T_WAT,1},{T_STE,T_ELE,1},
    {T_FIR,T_BUG,4},{T_FIR,T_STE,4},{T_FIR,T_GRA,4},{T_FIR,T_ICE,4},{T_FIR,T_ROC,1},{T_FIR,T_FIR,1},{T_FIR,T_WAT,1},{T_FIR,T_DRA,1},
    {T_WAT,T_GRO,4},{T_WAT,T_ROC,4},{T_WAT,T_FIR,4},{T_WAT,T_WAT,1},{T_WAT,T_GRA,1},{T_WAT,T_DRA,1},
    {T_GRA,T_GRO,4},{T_GRA,T_ROC,4},{T_GRA,T_WAT,4},{T_GRA,T_FLY,1},{T_GRA,T_POI,1},{T_GRA,T_BUG,1},
    {T_GRA,T_STE,1},{T_GRA,T_FIR,1},{T_GRA,T_GRA,1},{T_GRA,T_DRA,1},
    {T_ELE,T_FLY,4},{T_ELE,T_WAT,4},{T_ELE,T_GRA,1},{T_ELE,T_ELE,1},{T_ELE,T_DRA,1},{T_ELE,T_GRO,0},
    {T_PSY,T_FIG,4},{T_PSY,T_POI,4},{T_PSY,T_STE,1},{T_PSY,T_PSY,1},{T_PSY,T_DAR,0},
    {T_ICE,T_FLY,4},{T_ICE,T_GRO,4},{T_ICE,T_GRA,4},{T_ICE,T_DRA,4},{T_ICE,T_STE,1},{T_ICE,T_FIR,1},{T_ICE,T_WAT,1},{T_ICE,T_ICE,1},
    {T_DRA,T_DRA,4},{T_DRA,T_STE,1},{T_DRA,T_FAI,0},
    {T_DAR,T_GHO,4},{T_DAR,T_PSY,4},{T_DAR,T_FIG,1},{T_DAR,T_DAR,1},{T_DAR,T_FAI,1},
    {T_FAI,T_FIG,4},{T_FAI,T_DRA,4},{T_FAI,T_DAR,4},{T_FAI,T_POI,1},{T_FAI,T_STE,1},{T_FAI,T_FIR,1},
};
LOCAL u32 eff(u32 atk, u32 def) {
    for (size_t i = 0; i < sizeof(kChartEx) / sizeof(kChartEx[0]); ++i)
        if (kChartEx[i][0] == atk && kChartEx[i][1] == def) return kChartEx[i][2];
    return 2;
}

/* ---------- player-supplied reference data ---------- */
typedef struct {
    char* buf; u32 cap;
    u32* off; u32 maxn;
    u32 count;
    int tries;
    const char* path;
} NameList;
LOCAL char nb_species[24 * 1024], nb_moves[24 * 1024], nb_items[64 * 1024], nb_abil[12 * 1024],
    nb_nat[1024], nb_types[512];
LOCAL u32 no_species[1100], no_moves[1000], no_items[3000], no_abil[400], no_nat[32], no_types[24];
LOCAL NameList g_species = {nb_species, sizeof(nb_species), no_species, 1100, 0, 0, "user:species.txt"};
LOCAL NameList g_moves = {nb_moves, sizeof(nb_moves), no_moves, 1000, 0, 0, "user:moves.txt"};
LOCAL NameList g_items = {nb_items, sizeof(nb_items), no_items, 3000, 0, 0, "user:items.txt"};
LOCAL NameList g_abil = {nb_abil, sizeof(nb_abil), no_abil, 400, 0, 0, "user:abilities.txt"};
LOCAL NameList g_nat = {nb_nat, sizeof(nb_nat), no_nat, 32, 0, 0, "user:natures.txt"};
LOCAL NameList g_types = {nb_types, sizeof(nb_types), no_types, 24, 0, 0, "user:types.txt"};
LOCAL NameList* const kLists[] = {&g_species, &g_moves, &g_items, &g_abil, &g_nat, &g_types};
/* Pokédex and map texts (game only, no user: fallback). */
LOCAL char nb_zkntype[48 * 1024], nb_zukan[320 * 1024], nb_place[48 * 1024];
LOCAL u32 no_zkntype[1400], no_zukan[1400], no_place[1200];
LOCAL NameList g_zkntype = {nb_zkntype, sizeof(nb_zkntype), no_zkntype, 1400, 0, 0, 0};
LOCAL NameList g_zukan = {nb_zukan, sizeof(nb_zukan), no_zukan, 1400, 0, 0, 0};
LOCAL NameList g_place = {nb_place, sizeof(nb_place), no_place, 1200, 0, 0, 0};
/* The map's own place names (ymap_place_name.dat), their labels (.tbl, AHTB) and where each one
 * goes (world/data/ui/ymap/ymap_place_name*_data, flatbuffers: pkNX PlaceNameDataTableArray =
 * {Table: [ {ID: string, PlaceNameMstxtLabl: string, Field3DPos: Vec3f{X,Y,Z}, IsDisplayInZoomOut,
 * IsDisplayInZoomNormal, IsDisplayInZoomIn: bool} ]}). */
LOCAL char nb_ymap[24 * 1024]; LOCAL u32 no_ymap[700];
LOCAL NameList g_ymapname = {nb_ymap, sizeof(nb_ymap), no_ymap, 700, 0, 0, 0};
LOCAL char g_ylab_buf[24 * 1024]; LOCAL u32 g_ylab_off[700], g_ylab_n; /* label of each text line */
typedef struct { float x, z; u16 text; u8 field, zoom; } MapLab;
LOCAL MapLab g_mlab[512]; LOCAL u32 g_mlab_n, g_mlab_files;

LOCAL int user_source_ok(const EdenDsmodHostApi* h) {
    return h->read_romfs && (h->capabilities & EDEN_DSMOD_CAP_SOURCE_USER) != 0;
}

LOCAL void namelist_load(const EdenDsmodHostApi* h, NameList* nl) {
    nl->count = 0;
    if (!user_source_ok(h)) return;
    size_t n = h->read_romfs(h->userdata, nl->path, 0, 0, 0);
    if (n == 0 || n >= nl->cap) return;
    if (h->read_romfs(h->userdata, nl->path, 0, nl->buf, n) != n) return;
    nl->buf[n] = 0;
    u32 i = 0;
    if (n >= 3 && (u8)nl->buf[0] == 0xEF && (u8)nl->buf[1] == 0xBB && (u8)nl->buf[2] == 0xBF) i = 3;
    nl->off[nl->count++] = i;
    for (; i < n && nl->count < nl->maxn; ++i) {
        if (nl->buf[i] == '\r') nl->buf[i] = 0;
        if (nl->buf[i] == '\n') { nl->buf[i] = 0; nl->off[nl->count++] = i + 1; }
    }
}
LOCAL const char* namelist_get(const NameList* nl, u32 idx) {
    const char* s = idx < nl->count ? nl->buf + nl->off[idx] : 0;
    return s && *s ? s : 0;
}

#define PERSONAL_SIZE 0x50
LOCAL u8 g_personal[160 * 1024];
LOCAL u32 g_personal_n;
LOCAL u8 g_movedata[4 * 1024];
LOCAL u32 g_movedata_n;
LOCAL int g_bin_tries;

LOCAL u32 load_bin(const EdenDsmodHostApi* h, const char* path, u8* buf, u32 cap) {
    if (!user_source_ok(h)) return 0;
    size_t n = h->read_romfs(h->userdata, path, 0, 0, 0);
    if (n == 0 || n > cap) return 0;
    return h->read_romfs(h->userdata, path, 0, buf, n) == n ? (u32)n : 0;
}

/* ---------- data read from the player's own game files (romfs) ---------- */
typedef struct { u16 nat; u8 form; u8 t1, t2; u16 intl, dexp, dexk, dexb, height, weight; u8 growth; u8 base[6]; } GPers;
LOCAL GPers g_gpers[3000];
LOCAL u32 g_gpers_n;
typedef struct { u8 ok, type, cat, pp, power; } GMove;
LOCAL GMove g_gmove[1024];
LOCAL u32 g_gmove_n;
LOCAL int g_gtext_n;                /* how many of the six name lists came from the game */
LOCAL int g_species_internal;       /* g_species came from the game (internal order), not user: */
LOCAL u8 g_fbuf[8u << 20];          /* one decompressed game file at a time */
LOCAL int g_game_step;              /* loader position, see load_game_step() */
#define GAME_STEPS 15 /* v0.7.2: + map place names (texts, labels, three data files) */
LOCAL char g_game_why[160];         /* last loader problem, shown on the debug page */
LOCAL char g_status_problem[200];
LOCAL u32 g_img_ok, g_img_fail;    /* game pictures decoded / failed (load_image) */
LOCAL char g_img_why[96];           /* the last failure: which picture and why */

/* personal_array.bin: PersonalTable {Table:[PersonalInfo]}; PersonalInfo field 0 = struct
 * PersonalInfoDetail {u16 SpeciesInternal, u16 Form, u16 SpeciesNational, ...}, field 5 Type1,
 * field 6 Type2 (pkNX FlatBuffers/SV/Personal/Schemas/PersonalTable.fbs). */
LOCAL int parse_personal(const EdenDsmodHostApi* h, u32 n) {
    SvSrc m = {h, 0, 0, n, g_fbuf};
    u64 root, vec; u32 len;
    if (!sv_fb_root(&m, &root) || !sv_fb_vec(&m, root, 0, &vec, &len) || len == 0) return 0;
    u32 out = 0;
    for (u32 i = 0; i < len && out < sizeof(g_gpers) / sizeof(g_gpers[0]); ++i) {
        u64 t = sv_fb_vec_table(&m, vec, i);
        u64 info = t ? sv_fb_field(&m, t, 0) : 0;
        u8 d[12];
        if (!info || !sv_src_read(&m, info, d, 12)) continue;
        u32 nat = le16(d + 4), form = le16(d + 2);
        u32 t1 = sv_fb_u8(&m, t, 5, 0), t2 = sv_fb_u8(&m, t, 6, 0);
        if (nat == 0 || nat > 1100 || form > 255 || t1 >= T_COUNT || t2 >= T_COUNT) continue;
        /* field 2 Dex: struct {u16 Index; u8 Group} (absent = not in the Paldea dex) */
        u64 dx = sv_fb_field(&m, t, 2);
        u8 dd[2] = {0, 0};
        if (dx) sv_src_read(&m, dx, dd, 2);
        GPers e = {(u16)nat, (u8)form, (u8)t1, (u8)t2, le16(d), le16(dd),
                   (u16)sv_fb_u16(&m, t, 3, 0), (u16)sv_fb_u16(&m, t, 4, 0), le16(d + 8), le16(d + 10),
                   (u8)sv_fb_u8(&m, t, 10, 0), {0}};
        /* field 22 Base: struct {HP, ATK, DEF, SPA, SPD, SPE} */
        u64 bs = sv_fb_field(&m, t, 22);
        if (bs) sv_src_read(&m, bs, e.base, 6);
        g_gpers[out++] = e;
    }
    g_gpers_n = out;
    return out > 0;
}

/* waza_array.bin: WazaTable {Table:[Waza]}; Waza fields 0 MoveID, 2 Type, 4 Category, 5 Power,
 * 7 PP (pkNX FlatBuffers/SV/Waza/Schemas/Waza.fbs). */
LOCAL int parse_waza(const EdenDsmodHostApi* h, u32 n) {
    SvSrc m = {h, 0, 0, n, g_fbuf};
    u64 root, vec; u32 len;
    if (!sv_fb_root(&m, &root) || !sv_fb_vec(&m, root, 0, &vec, &len) || len == 0) return 0;
    u32 got = 0;
    for (u32 i = 0; i < len; ++i) {
        u64 t = sv_fb_vec_table(&m, vec, i);
        if (!t) continue;
        u32 id = sv_fb_u16(&m, t, 0, 0);
        if (id == 0 || id >= sizeof(g_gmove) / sizeof(g_gmove[0])) continue;
        u32 type = sv_fb_u8(&m, t, 2, 0);
        if (type >= T_COUNT) continue;
        g_gmove[id] = (GMove){1, (u8)type, (u8)sv_fb_u8(&m, t, 4, 0), (u8)sv_fb_u8(&m, t, 7, 0),
                              (u8)sv_fb_u8(&m, t, 5, 0)};
        ++got;
    }
    g_gmove_n = got;
    return got > 0;
}

/* Game text (.dat, pkNX TextFile): u16 sections(1), u16 lines, u32 total, u32 key(0),
 * u32 section offset; then u32 section length and {u32 offset, u16 length, u16 flags} per line.
 * Each line is u16 characters XOR'd with a key that rotates left by 3 per character; the key
 * starts at 0x7C89 and advances by 0x2983 per line. 0x0010 opens a variable: u16 count, then
 * `count` more u16s (skipped here). Decoded to UTF-8 into a NameList. */
LOCAL int text_to_list(const u8* d, u32 n, NameList* nl) {
    if (n < 0x14) return 0;
    u32 lines = le16(d + 2), total = le32(d + 4), key0 = le32(d + 8), sdo = le32(d + 12);
    if (le16(d) != 1 || key0 != 0 || sdo + total != n || sdo + 4 + lines * 8 > n) return 0;
    u32 pos = 0, cnt = 0;
    u16 key = 0x7C89;
    for (u32 i = 0; i < lines && cnt < nl->maxn; ++i, key = (u16)(key + 0x2983)) {
        const u8* e = d + sdo + 4 + i * 8;
        u32 off = le32(e), len = le16(e + 4);
        nl->off[cnt++] = pos;
        if (sdo + off + len * 2 <= n) {
            const u8* p = d + sdo + off;
            u16 k = key;
            u32 skip = 0;
            int in_var = 0;
            for (u32 j = 0; j < len; ++j) {
                u32 c = (u16)(le16(p + 2 * j) ^ k);
                k = (u16)((k << 3) | (k >> 13));
                if (in_var) { skip = c; in_var = 0; continue; } /* the variable's word count */
                if (skip) { --skip; continue; }
                if (c == 0) break;
                if (c == 0x10) { in_var = 1; continue; }
                if (c == 0xE08E) c = 0x2642;
                if (c == 0xE08F) c = 0x2640;
                if (c >= 0xD800 && c <= 0xDFFF) c = '?';
                if (pos + 4 >= nl->cap) break;
                if (c < 0x80) nl->buf[pos++] = (char)c;
                else if (c < 0x800) { nl->buf[pos++] = (char)(0xC0 | (c >> 6)); nl->buf[pos++] = (char)(0x80 | (c & 0x3F)); }
                else { nl->buf[pos++] = (char)(0xE0 | (c >> 12)); nl->buf[pos++] = (char)(0x80 | ((c >> 6) & 0x3F)); nl->buf[pos++] = (char)(0x80 | (c & 0x3F)); }
            }
        }
        if (pos + 1 >= nl->cap) { nl->count = cnt; return cnt > 1; }
        nl->buf[pos++] = 0;
    }
    nl->count = cnt;
    return cnt > 1;
}

/* Message folder for the save's language (MyStatus language byte, PKHeX LanguageID). */
LOCAL const char* lang_folder(u32 lang) {
    switch (lang) {
    case 1: return "JPN";
    case 2: return "English";
    case 3: return "French";
    case 4: return "Italian";
    case 5: return "German";
    case 7: return "Spanish";
    case 8: return "Korean";
    case 9: return "Simp_Chinese";
    case 10: return "Trad_Chinese";
    default: return "Spanish";
    }
}

LOCAL void game_why(const char* what, const char* why) {
    Str s; s_init(&s, g_game_why, sizeof(g_game_why));
    s_cat(&s, what); s_cat(&s, ": "); s_cat(&s, why);
}

/* One file per call, so the tick never stalls for long. Returns 1 while there is more to do. */
LOCAL const char* const kTextFiles[6] = {"monsname", "wazaname", "itemname", "tokusei", "seikaku", "typename"};
LOCAL NameList* const kTextLists[6] = {&g_species, &g_moves, &g_items, &g_abil, &g_nat, &g_types};

/* AHTB: "AHTB", u32 count, then {u64 hash, u16 length, char name[length]} (name 0-terminated). */
LOCAL int ahtb_to_labels(const u8* d, u32 n) {
    if (n < 8 || memcmp(d, "AHTB", 4)) return 0;
    u32 cnt = le32(d + 4), p = 8, pos = 0;
    g_ylab_n = 0;
    for (u32 i = 0; i < cnt && g_ylab_n < 700; ++i) {
        if (p + 10 > n) break;
        u32 len = le16(d + p + 8);
        p += 10;
        if (p + len > n || pos + len + 1 >= sizeof(g_ylab_buf)) break;
        g_ylab_off[g_ylab_n++] = pos;
        for (u32 k = 0; k < len && d[p + k]; ++k) g_ylab_buf[pos++] = (char)d[p + k];
        g_ylab_buf[pos++] = 0;
        p += len;
    }
    return g_ylab_n > 0;
}
/* Minimal flatbuffer reading, every offset checked against the file. */
LOCAL int fb_field(const u8* b, u32 n, u32 tab, u32 id, u32* at) {
    if (tab + 4 > n) return 0;
    s64 vt = (s64)tab - (s32)le32(b + tab);
    if (vt < 0 || (u64)vt + 4 > n) return 0;
    u32 vlen = le16(b + vt);
    if (4 + 2 * id + 2 > vlen || (u64)vt + vlen > n) return 0;
    u32 fo = le16(b + vt + 4 + 2 * id);
    if (!fo || tab + fo > n) return 0;
    *at = tab + fo;
    return 1;
}
LOCAL int fb_deref(const u8* b, u32 n, u32 at, u32* out) {
    if (at + 4 > n) return 0;
    u64 t = (u64)at + le32(b + at);
    if (t + 4 > n) return 0;
    *out = (u32)t;
    return 1;
}
LOCAL const char* fb_string(const u8* b, u32 n, u32 tab, u32 id, u32* len) {
    u32 at, s;
    if (!fb_field(b, n, tab, id, &at) || !fb_deref(b, n, at, &s)) return 0;
    u32 l = le32(b + s);
    if ((u64)s + 4 + l > n) return 0;
    *len = l;
    return (const char*)b + s + 4;
}
LOCAL float fb_float(const u8* b, u32 n, u32 tab, u32 id) {
    u32 at; float f = 0;
    if (fb_field(b, n, tab, id, &at) && at + 4 <= n) memcpy(&f, b + at, 4);
    return f;
}
LOCAL int fb_bool(const u8* b, u32 n, u32 tab, u32 id) {
    u32 at;
    return fb_field(b, n, tab, id, &at) && b[at] != 0;
}
LOCAL void parse_place_names(const u8* b, u32 n, u8 field) {
    u32 root, at, vec;
    if (!fb_deref(b, n, 0, &root) || !fb_field(b, n, root, 0, &at) || !fb_deref(b, n, at, &vec)) return;
    u32 cnt = le32(b + vec);
    for (u32 i = 0; i < cnt && g_mlab_n < 512; ++i) {
        u32 e;
        if ((u64)vec + 4 + 4 * i + 4 > n || !fb_deref(b, n, vec + 4 + 4 * i, &e)) break;
        u32 ll = 0;
        const char* lab = fb_string(b, n, e, 1, &ll);
        if (!lab || !ll) continue;
        int text = -1;
        for (u32 k = 0; k < g_ylab_n && text < 0; ++k) {
            const char* c = g_ylab_buf + g_ylab_off[k];
            u32 m = 0;
            while (m < ll && c[m] && c[m] == lab[m]) ++m;
            if (m == ll && !c[m]) text = (int)k;
        }
        if (text < 0 || (u32)text >= g_ymapname.count) continue;
        u32 pa, pt;
        if (!fb_field(b, n, e, 2, &pa) || !fb_deref(b, n, pa, &pt)) continue;
        MapLab* m = &g_mlab[g_mlab_n];
        m->x = fb_float(b, n, pt, 0); m->z = fb_float(b, n, pt, 2);
        m->text = (u16)text; m->field = field;
        m->zoom = (u8)(fb_bool(b, n, e, 3) | (fb_bool(b, n, e, 4) << 1) | (fb_bool(b, n, e, 5) << 2));
        g_mlab_n++;
    }
}
LOCAL int load_game_step(const EdenDsmodHostApi* h, u32 lang) {
    const char* why = "";
    if (g_game_step == 0) {
        u32 n = sv_game_read(h, "avalon/data/personal_array.bin", g_fbuf, sizeof(g_fbuf) - 64, &why);
        if (!n) game_why("personal_array", why);
        else if (!parse_personal(h, n)) game_why("personal_array", "formato inesperado");
    } else if (g_game_step == 1) {
        u32 n = sv_game_read(h, "avalon/data/waza_array.bin", g_fbuf, sizeof(g_fbuf) - 64, &why);
        if (!n) game_why("waza_array", why);
        else if (!parse_waza(h, n)) game_why("waza_array", "formato inesperado");
    } else if (g_game_step >= 8 && g_game_step < 11) {
        static const char* const extra[3] = {"zkn_type",
#if SV_TITLE_ID == 0x01008F6008C5E000ULL
                                             "zukan_comment_B",
#else
                                             "zukan_comment_A",
#endif
                                             "place_name"};
        NameList* const lists[3] = {&g_zkntype, &g_zukan, &g_place};
        u32 k = (u32)g_game_step - 8;
        char path[96];
        Str s; s_init(&s, path, sizeof(path));
        s_cat(&s, "message/dat/"); s_cat(&s, lang_folder(lang)); s_cat(&s, "/common/");
        s_cat(&s, extra[k]); s_cat(&s, ".dat");
        u32 n = sv_game_read(h, path, g_fbuf, sizeof(g_fbuf) - 64, &why);
        if (!n || !text_to_list(g_fbuf, n, lists[k])) lists[k]->count = 0; /* optional: pages say "?" */
    } else if (g_game_step >= 11 && g_game_step < GAME_STEPS) {
        char path[128];
        Str s; s_init(&s, path, sizeof(path));
        u32 k = (u32)g_game_step - 11;
        if (k == 0) {          /* texts, then labels: needed before the data files */
            s_cat(&s, "message/dat/"); s_cat(&s, lang_folder(lang)); s_cat(&s, "/common/ymap_place_name.dat");
            u32 n = sv_game_read(h, path, g_fbuf, sizeof(g_fbuf) - 64, &why);
            if (!n || !text_to_list(g_fbuf, n, &g_ymapname)) g_ymapname.count = 0;
            s_init(&s, path, sizeof(path));
            s_cat(&s, "message/dat/"); s_cat(&s, lang_folder(lang)); s_cat(&s, "/common/ymap_place_name.tbl");
            n = sv_game_read(h, path, g_fbuf, sizeof(g_fbuf) - 64, &why);
            if (!n || !ahtb_to_labels(g_fbuf, n)) g_ylab_n = 0;
        } else if (g_ymapname.count && g_ylab_n) {
            static const char* const files[3] = {
                "world/data/ui/ymap/ymap_place_name_data/ymap_place_name_data_array.bin",
                "world/data/ui/ymap/ymap_place_name_dlc1_data/ymap_place_name_dlc1_data_array.bin",
                "world/data/ui/ymap/ymap_place_name_dlc2_data/ymap_place_name_dlc2_data_array.bin"};
            u32 n = sv_game_read(h, files[k - 1], g_fbuf, sizeof(g_fbuf) - 64, &why);
            if (n) { parse_place_names(g_fbuf, n, (u8)(k - 1)); g_mlab_files++; }
        }
    } else if (g_game_step < 8) {
        u32 k = (u32)g_game_step - 2;
        char path[96];
        Str s; s_init(&s, path, sizeof(path));
        s_cat(&s, "message/dat/"); s_cat(&s, lang_folder(lang)); s_cat(&s, "/common/");
        s_cat(&s, kTextFiles[k]); s_cat(&s, ".dat");
        u32 n = sv_game_read(h, path, g_fbuf, sizeof(g_fbuf) - 64, &why);
        if (!n) game_why(kTextFiles[k], why);
        else if (text_to_list(g_fbuf, n, kTextLists[k])) {
            kTextLists[k]->tries = 99; ++g_gtext_n;
            if (kTextLists[k] == &g_species) g_species_internal = 1;
        }
        else game_why(kTextFiles[k], "formato inesperado");
    } else {
        return 0;
    }
    ++g_game_step;
    return 1;
}

/* Types of species/form: the game's own table first, then a player-supplied personal.bin. */
LOCAL int species_types(u32 species, u32 form, u32* t1, u32* t2) {
    if (g_gpers_n) {
        const GPers* base = 0;
        for (u32 i = 0; i < g_gpers_n; ++i) {
            if (g_gpers[i].nat != species) continue;
            if (g_gpers[i].form == form) { *t1 = g_gpers[i].t1; *t2 = g_gpers[i].t2; return 1; }
            if (g_gpers[i].form == 0 && !base) base = &g_gpers[i];
        }
        if (base) { *t1 = base->t1; *t2 = base->t2; return 1; }
    }
    u32 n = g_personal_n / PERSONAL_SIZE;
    if (species == 0 || species >= n) return 0;
    const u8* e = g_personal + species * PERSONAL_SIZE;
    u32 idx = species;
    u32 fcount = e[0x1A], fstats = le16(e + 0x18);
    if (form != 0 && form < fcount && fstats != 0) idx = fstats + form - 1;
    if (idx >= n) return 0;
    e = g_personal + idx * PERSONAL_SIZE;
    if (e[6] >= T_COUNT || e[7] >= T_COUNT) return 0;
    *t1 = e[6];
    *t2 = e[7];
    return 1;
}
typedef struct { int ok; u32 type, cat, pp, power; } MoveInfo;
LOCAL MoveInfo move_info(u32 move) {
    MoveInfo m = {0, 0, 0, 0, 0};
    if (move && move < sizeof(g_gmove) / sizeof(g_gmove[0]) && g_gmove[move].ok) {
        const GMove* g = &g_gmove[move];
        m.ok = 1; m.type = g->type; m.cat = g->cat; m.pp = g->pp; m.power = g->power;
        return m;
    }
    if (move == 0 || (move + 1) * 4 > g_movedata_n) return m;
    const u8* p = g_movedata + move * 4;
    m.ok = 1; m.type = p[0]; m.cat = p[1]; m.pp = p[2]; m.power = p[3];
    return m;
}

/* ---------- cached state, republished every tick ---------- */
typedef struct {
    int valid, egg, shiny, stats_ok;
    u32 ec;
    u32 species, form, level, hp, hpmax, status, gender, item, ability, nature, tera;
    u32 stat[6]; /* PS, Ataque, Defensa, At. Esp., Def. Esp., Velocidad */
    u32 exp; u8 ev[6], iv[6];  /* same order as stat[] */
    u32 moves[4], pp[4], ppups[4];
    char nick[64];
} Mon;

typedef struct {
    int build_ok;
    int keytable_ok, party_ok, money_ok, lp_ok, status_ok, cross_ok;
    u32 party_count, money, lp, tid, lang;
    char trainer[64];
    Mon mon[6];
    u64 next_refresh;
    u64 refreshes;
    u64 bad_reads;
    int torn_streak;
    u32 live_n;          /* party slots read from the live party objects this refresh */
    u32 live_bad;        /* live objects whose level/stats were implausible (kept the save's) */
    int ow_ok, overworld; /* SysBot's overworld byte (0x11 while walking around) */
    int ingame;           /* latched on the first field byte (0x11), dropped on 18 / unreadable (title) */
    u64 live_addr0;       /* where slot 0's live PK9 is (self-test of the heap scanner) */
    u32 ow_raw;
} State;
LOCAL State g;

/* ---------- interface language (v0.10) ----------
 * English or Spanish. The "lang" setting (0 auto, 1 English, 2 Spanish) is read from the host; auto follows the save's language (MyStatus byte 0x07, 7 = Spanish):
 * Spanish games get Spanish, every other language English. Before the save is read, auto is
 * English. Game words (names, places...) always come from the game's own files, in its language. */
LOCAL int g_en = 1, g_lang_seen;
LOCAL u32 g_lang_last;
LOCAL s64 g_lang_flag;
#define TR(es, en) (g_en ? (en) : (es))
LOCAL int lang_auto_en(void) { return g_lang_seen ? g_lang_last != 7 : 1; }
LOCAL void lang_update(const EdenDsmodHostApi* h) {
    if (g.status_ok) { g_lang_seen = 1; g_lang_last = g.lang; }
    /* The manifest copies the flag into the derived value "sv.ui.lang": some hosts only answer
     * "@flag:" reads from sample() with the fallback (see test_host SV_FLAGBUG). */
    g_lang_flag = 0;
    if (h && h->struct_size >= __builtin_offsetof(EdenDsmodHostApi, get_i64) + sizeof(h->get_i64) && h->get_i64) {
        g_lang_flag = h->get_i64(h->userdata, "sv.ui.lang", -1);
        if (g_lang_flag < 0) g_lang_flag = h->get_i64(h->userdata, "@flag:lang", 0);
    }
    g_en = g_lang_flag == 1 ? 1 : g_lang_flag == 2 ? 0 : lang_auto_en();
}

LOCAL const char* status_short(u32 st) {
    if (st & 7) return TR("DOR", "SLP");
    if (st & 0x80) return TR("ENV+", "TOX");
    if (st & 0x08) return TR("ENV", "PSN");
    if (st & 0x10) return TR("QUE", "BRN");
    if (st & 0x20) return TR("CON", "FRZ");
    if (st & 0x40) return "PAR";
    return "";
}
LOCAL const char* status_long(const Mon* m) {
    if (m->egg) return TR("Huevo", "Egg");
    if (m->hp == 0) return TR("Debilitado", "Fainted");
    u32 st = m->status;
    if (st & 7) return TR("Dormido", "Asleep");
    if (st & 0x80) return TR("Gravemente envenenado", "Badly poisoned");
    if (st & 0x08) return TR("Envenenado", "Poisoned");
    if (st & 0x10) return TR("Quemado", "Burned");
    if (st & 0x20) return TR("Congelado", "Frozen");
    if (st & 0x40) return TR("Paralizado", "Paralyzed");
    return TR("Sano", "Healthy");
}

LOCAL int refresh_party(const EdenDsmodHostApi* h, const KeyTable* kt);
LOCAL void scan_refresh(const EdenDsmodHostApi* h);

LOCAL void refresh(const EdenDsmodHostApi* h) {
    KeyTable kt;
    g.keytable_ok = keytable_resolve(h, &kt);
    g.money_ok = g.lp_ok = g.status_ok = g.cross_ok = 0;
    if (!g.keytable_ok) { g.party_ok = 0; g.torn_streak = 0; return; }
    /* A torn party read (the game writing while we read) keeps the last good copy for a while;
     * a persistent failure drops it (fail closed). */
    if (!refresh_party(h, &kt)) {
        if (++g.torn_streak > 8) g.party_ok = 0;
    } else {
        g.torn_streak = 0;
    }

    scan_refresh(h);

    u32 v;
    if (read_u32(h, &kt, KEY_MONEY, &v)) { g.money = v; g.money_ok = 1; }
    if (read_u32(h, &kt, KEY_LP, &v)) { g.lp = v; g.lp_ok = 1; }

    /* Trainer: key-table copy, cross-checked against SysBot's direct pointer. */
    if (read_object(h, &kt, KEY_MYSTATUS, MYSTATUS_SIZE)) {
        u8 ms[MYSTATUS_SIZE];
        memcpy(ms, g_blk, MYSTATUS_SIZE);
        Str s; s_init(&s, g.trainer, sizeof(g.trainer));
        s_utf16(&s, ms + 0x10, 12);
        g.tid = le32(ms) % 1000000u;
        g.lang = ms[0x07];
        g.status_ok = 1;
        u8 direct[MYSTATUS_SIZE];
        u64 a = chain(h, kMyStatusChain, 6);
        g.cross_ok = a && rd(h, a, direct, MYSTATUS_SIZE) && memcmp(direct, ms, MYSTATUS_SIZE) == 0;
    }
}

/* Fills a Mon from a decrypted party-format PK9 (0x158). Returns 0 for an empty slot. */
LOCAL int mon_from_pk(const u8* pk, Mon* m) {
    memset(m, 0, sizeof(*m));
    m->species = national9(le16(pk + 0x08));
    if (m->species == 0 || m->species > 1025) return 0;
    m->valid = 1;
    m->ec = le32(pk);
    m->item = le16(pk + 0x0A);
    m->ability = le16(pk + 0x14);
    m->nature = pk[0x21] < 25 ? pk[0x21] : pk[0x20]; /* the mint-adjusted stat nature */
    m->gender = (pk[0x22] >> 1) & 3;
    m->form = pk[0x24];
    u32 iv32 = le32(pk + 0x8C);
    m->egg = (iv32 >> 30) & 1;
    u32 id32 = le32(pk + 0x0C), pid = le32(pk + 0x1C);
    m->shiny = ((id32 >> 16) ^ (id32 & 0xFFFF) ^ (pid >> 16) ^ (pid & 0xFFFF)) < 16;
    m->status = le32(pk + 0x90);
    m->tera = pk[0x95] < 19 ? pk[0x95] : pk[0x94]; /* override (Terapagos etc.) wins */
    for (int j = 0; j < 4; ++j) {
        m->moves[j] = le16(pk + 0x72 + 2 * j);
        m->pp[j] = pk[0x7A + j];
        m->ppups[j] = pk[0x7E + j];
    }
    m->level = pk[0x148];
    m->hpmax = le16(pk + 0x14A);
    m->hp = le16(pk + 0x8A);
    m->stat[0] = m->hpmax;
    m->stat[1] = le16(pk + 0x14C); /* ATK */
    m->stat[2] = le16(pk + 0x14E); /* DEF */
    m->stat[3] = le16(pk + 0x152); /* SPA */
    m->stat[4] = le16(pk + 0x154); /* SPD */
    m->stat[5] = le16(pk + 0x150); /* SPE */
    m->stats_ok = m->level >= 1 && m->level <= 100 && m->hpmax > 0 && m->hp <= m->hpmax;
    m->exp = le32(pk + 0x10);
    /* EVs 0x26.. and IVs (5 bits each from 0x8C) are stored HP ATK DEF SPE SPA SPD */
    static const u8 ord[6] = {0, 1, 2, 5, 3, 4};
    for (int k = 0; k < 6; ++k) { m->ev[ord[k]] = pk[0x26 + k]; m->iv[ord[k]] = (u8)((iv32 >> (5 * k)) & 31); }
    Str s; s_init(&s, m->nick, sizeof(m->nick));
    s_utf16(&s, pk + 0x58, 13);
    return 1;
}

/* Live party objects (SVResearches "Party", PokeViewer.NET PartyViewer, game 3.0.x):
 * PK9 (party format) at [[[[main+4763C98]+08]+(30+8*slot)]+30]. Unlike the save block they
 * follow the battle and the field as it happens. */
#define LIVE_PARTY_ROOT 0x4763C98ULL
LOCAL int read_live_slot(const EdenDsmodHostApi* h, u32 slot, Mon* m, u64* where) {
    const u64 c[] = {LIVE_PARTY_ROOT, 0x08, 0x30 + 8ull * slot, 0x30, 0x00};
    u64 a = chain(h, c, 5);
    u8 pk[PK_PARTY];
    if (!a || !rd(h, a, pk, PK_PARTY) || le32(pk) == 0 || le16(pk + 4) != 0) return 0;
    if (!pk9_decrypt(pk) || !mon_from_pk(pk, m)) return 0;
    if (where) *where = a;
    return 1;
}

LOCAL void compute_stats(Mon* m);
/* Returns 1 when a complete, checksum-valid party was read into g.mon. */
LOCAL int refresh_party(const EdenDsmodHostApi* h, const KeyTable* kt) {
    u32 size = read_object(h, kt, KEY_PARTY, PARTY_BLOCK_MIN);
    if (!size) return 0;
    u8 party[PARTY_BLOCK_MIN];
    memcpy(party, g_blk, PARTY_BLOCK_MIN);
    u32 count = party[6 * PK_PARTY];
    if (count > 6) { g.bad_reads++; return 0; }

    Mon fresh[6];
    memset(fresh, 0, sizeof(fresh));
    for (u32 i = 0; i < count; ++i) {
        u8 pk[PK_PARTY];
        memcpy(pk, party + i * PK_PARTY, PK_PARTY);
        if (!pk9_decrypt(pk)) { g.bad_reads++; return 0; } /* torn or wrong: keep the last good copy */
        mon_from_pk(pk, &fresh[i]);
    }
    /* The live objects win where they agree with the save block on who is in that slot.
     * v0.7.0: and on the ORDER. The save block keeps the order of the last save; reorder the
     * team in game and it is stale (device: Inteleon led, the companion still said Arcanine).
     * When every live slot holds one of the saved Pokémon, each a different one, the live order
     * is the order. */
    {
        Mon lv[6]; u32 map[6]; int all = count > 0, used[6] = {0, 0, 0, 0, 0, 0};
        for (u32 i = 0; i < count && all; ++i) {
            u64 wh = 0;
            if (!read_live_slot(h, i, &lv[i], &wh)) { all = 0; break; }
            int at = -1;
            for (u32 j = 0; j < count; ++j) if (!used[j] && fresh[j].valid && fresh[j].ec == lv[i].ec) { at = (int)j; break; }
            if (at < 0) { all = 0; break; }
            used[at] = 1; map[i] = (u32)at;
        }
        if (all) {
            Mon re[6];
            for (u32 i = 0; i < count; ++i) re[i] = fresh[map[i]];
            for (u32 i = 0; i < count; ++i) fresh[i] = re[i];
        }
    }
    g.live_n = 0;
    g.live_bad = 0;
    for (u32 i = 0; i < count; ++i) {
        Mon live;
        u64 where = 0;
        if (!fresh[i].valid || !read_live_slot(h, i, &live, &where) || live.ec != fresh[i].ec) continue;
        if (i == 0) g.live_addr0 = where;
        /* Bytes 0x148.. (level, stats) lie outside the checksum and the live object does not
         * always keep them (seen on device: level 9, max HP 5474 for a healthy Gallade). Take
         * them only when they are a plausible step from the save copy; otherwise keep the
         * save's level and stats and take just the checksummed live fields. */
        const Mon* f = &fresh[i];
        int plausible = live.stats_ok && f->stats_ok && live.level >= f->level && live.level <= f->level + 20 &&
                        live.hpmax >= f->hpmax && live.hpmax <= f->hpmax * 2 + 40;
        for (int k = 1; plausible && k < 6; ++k)
            if (live.stat[k] < f->stat[k] || live.stat[k] > f->stat[k] * 2 + 40) plausible = 0;
        /* Everything inside the checksum comes from the live object (held item, moves, PP,
         * ability, experience... the save block only changes when the game saves: seen on
         * device, an item given to Arcanine did not show). Level and stats: the live copy's when
         * plausible, else worked out from its experience, IVs, EVs and nature. */
        Mon merged = live;
        if (!plausible) {
            merged.level = f->level; merged.hpmax = f->hpmax; merged.stats_ok = f->stats_ok;
            memcpy(merged.stat, f->stat, sizeof(merged.stat));
            if (g_gpers_n) compute_stats(&merged);
            if (merged.hp > merged.hpmax) merged.hp = merged.hpmax;
            g.live_bad++;
        }
        fresh[i] = merged;
        g.live_n++;
    }
    memcpy(g.mon, fresh, sizeof(fresh));
    g.party_count = count;
    g.party_ok = 1;
    return 1;
}

/* ---------- opponent search (experimental) ----------
 * Nobody has published where Scarlet/Violet keep the battle's Pokémon, so the module looks for
 * them: it sweeps the guest heap for encrypted PK9s (unencrypted header: EC != 0, sanity 0,
 * checksum != 0; then the species word of block A, then a full decrypt + checksum), skipping
 * the player's own Pokémon (by EC) and the boxes. While on the field (overworld byte 0x11) it
 * records what is already in memory (address + EC); once the field is left, PK9s that were not
 * there before are the battle's. The candidates are re-read every refresh so HP stays live. */
#define OW_CHAIN_ROOT 0x473ADE0ULL
LOCAL const u64 kOverworldChain[] = {OW_CHAIN_ROOT, 0x160, 0xE8, 0x28};
LOCAL const u64 kBoxChain[] = {PTR_ROOT, 0xD8, 0x8, 0xB8, 0x30, 0x9D0, 0x0};
#define SCAN_MAX 24
#define SCAN_HALF (192ull << 20)
#define BASE_MAX 1024
typedef struct { u64 addr; u32 ec; Mon m; int from_path, isnew; } Cand; /* isnew: not on the field before */
LOCAL struct {
    u64 lo, hi, cur;         /* heap range and sweep position */
    int ready;               /* a full baseline sweep has finished */
    int mode;                /* 0 baseline (field), 1 battle */
    Cand found[SCAN_MAX]; u32 nfound;   /* this sweep (battle mode) */
    Cand opp[SCAN_MAX]; u32 nopp;       /* the battle's Pokémon, re-read live */
    struct { u64 addr; u32 ec, fp; } base[BASE_MAX]; u32 nbase;  /* fp: hash of the raw 0x158 bytes */
    u32 changed;             /* battle sweep: field PK9s whose bytes changed (the wild one fighting) */
    u64 box_lo, box_hi;
    u64 sweeps, hits, valid_pk;
    u32 recentred;
    u64 cur_field;           /* field sweep position kept across short non-field spells */
    u32 pending;             /* refreshes the overworld byte has disagreed with the mode */
    int moved;               /* window moved around the live party objects */
    int t_in, t_ptr, t_hdr, t_spc, t_pk; u32 t_al; /* self-test on the live PK9 */
    u32 msweeps;             /* sweeps since the last field/battle switch */
    u32 others;              /* battle sweep: foreign PK9s that were already there on the field */
    char other_names[64];
    u32 ow_seen[6]; u32 now_seen;  /* distinct overworld byte values, in order of appearance */
    u32 page[1024];          /* fallback page copy when the host has no get_read_pointer */
} S;

LOCAL const char* mon_name(const Mon* m);
LOCAL int is_party_ec(u32 ec) {
    for (u32 i = 0; i < 6; ++i) if (g.mon[i].valid && g.mon[i].ec == ec) return 1;
    return 0;
}
LOCAL int base_find(u64 addr, u32 ec) {
    for (u32 i = 0; i < S.nbase; ++i) if (S.base[i].addr == addr && S.base[i].ec == ec) return (int)i;
    return -1;
}
LOCAL int in_base(u64 addr, u32 ec) { return base_find(addr, ec) >= 0; }
LOCAL u32 g_probe_fp; /* fingerprint of the PK9 the last successful probe() accepted */
LOCAL u32 fnv32(const u8* d, u32 n) { u32 x = 0x811C9DC5u; for (u32 i = 0; i < n; ++i) { x ^= d[i]; x *= 0x01000193u; } return x; }
/* Cheap test before a full decrypt: the species word of block A must be a real species. */
LOCAL int quick_species(const u8* p) {
    u32 ec = le32(p);
    u32 bp = kBlockPos[((ec >> 13) & 31) * 4 + 0];
    u32 k = bp * 0x50 / 2; /* u16 index of block A's first word in the encrypted region */
    u32 seed = ec;
    for (u32 t = 0; t <= k; ++t) seed = 0x41C64E6Du * seed + 0x6073u;
    u32 raw = le16(p + 8 + bp * 0x50) ^ (u16)(seed >> 16);
    return raw >= 1 && raw <= 1400;
}
/* Checks one position; on a valid PK9 that is not the player's, fills *out. */
LOCAL int probe(const EdenDsmodHostApi* h, u64 addr, const u8* p, u32 avail, Mon* out) {
    if (le32(p) == 0 || le16(p + 4) != 0 || le16(p + 6) == 0) return 0;
    u8 pk[PK_PARTY];
    if (avail >= PK_PARTY) memcpy(pk, p, PK_PARTY);
    else if (!rd(h, addr, pk, PK_PARTY)) return 0;
    if (!quick_species(pk)) return 0;
    S.hits++;
    g_probe_fp = fnv32(pk, PK_PARTY);
    if (!pk9_decrypt(pk) || !mon_from_pk(pk, out)) return 0;
    S.valid_pk++;
    if (is_party_ec(out->ec)) return 0;
    if (addr >= S.box_lo && addr < S.box_hi) return 0;
    return 1;
}

LOCAL void scan_step(const EdenDsmodHostApi* h, u64 budget) {
    if (!S.lo || S.hi <= S.lo) return;
    u64 end = S.cur + budget;
    while (S.cur < end) {
        if (S.cur >= S.hi) {                      /* sweep complete */
            S.sweeps++;
            S.msweeps++;
            if (S.mode == 0) S.ready = 1;
            else {                                /* adopt this sweep's battle Pokémon */
                /* keep the newer reading of the ones already followed: a found entry holds the
                 * bytes from when the sweep passed it, maybe seconds old (v0.6.6: HP jumped back) */
                for (u32 i = 0; i < S.nfound; ++i)
                    for (u32 j = 0; j < S.nopp; ++j)
                        if (S.opp[j].addr == S.found[i].addr && S.opp[j].ec == S.found[i].ec) { S.found[i].m = S.opp[j].m; break; }
                memcpy(S.opp, S.found, sizeof(Cand) * S.nfound);
                S.nopp = S.nfound;
            }
            S.nfound = 0;
            S.cur = S.lo;
            return;
        }
        u64 page = S.cur & ~0xFFFull;
        const u8* p = h->get_read_pointer ? h->get_read_pointer(h->userdata, page, 0x1000) : 0;
        if (!p) {
            if (h->is_mapped && !h->is_mapped(h->userdata, page, 0x1000)) { S.cur = page + 0x1000; continue; }
            if (!rd(h, page, S.page, 0x1000)) { S.cur = page + 0x1000; continue; }
            p = (const u8*)S.page;
        }
        for (u32 off = (u32)(S.cur - page); off + 8 <= 0x1000; off += 8) {
            Mon m;
            u64 addr = page + off;
            if (!probe(h, addr, p + off, 0x1000 - off, &m)) continue;
            int bi = base_find(addr, m.ec);
            if (S.mode == 0) {
                if (bi >= 0) S.base[bi].fp = g_probe_fp; /* keep the field picture current */
                else if (S.nbase < BASE_MAX) { S.base[S.nbase].addr = addr; S.base[S.nbase].ec = m.ec; S.base[S.nbase].fp = g_probe_fp; S.nbase++; }
            } else if (bi < 0 || S.base[bi].fp != g_probe_fp) {
                /* New in memory since the field, or a field Pokémon whose bytes changed: in Scarlet
                 * and Violet the wild Pokémon walking about IS the one you fight (seen on device:
                 * no new PK9 appears), so the one that changes (stats filled in, HP lost) is it. */
                if (bi >= 0 && S.msweeps == 0) S.changed++;
                if (S.nfound < SCAN_MAX) {
                    S.found[S.nfound].addr = addr; S.found[S.nfound].ec = m.ec; S.found[S.nfound].m = m; S.found[S.nfound].from_path = 0; S.found[S.nfound].isnew = bi < 0; S.nfound++;
                }
                /* show it straight away instead of at the end of the sweep */
                int have = 0;
                for (u32 i = 0; i < S.nopp; ++i) if (S.opp[i].addr == addr && S.opp[i].ec == m.ec) have = 1;
                if (!have && S.nopp < SCAN_MAX) {
                    S.opp[S.nopp].addr = addr; S.opp[S.nopp].ec = m.ec; S.opp[S.nopp].m = m; S.opp[S.nopp].from_path = 0; S.opp[S.nopp].isnew = bi < 0; S.nopp++;
                }
            } else if (S.msweeps == 0) {
                if (S.others < 3) {
                    Str t; t.p = S.other_names; t.cap = sizeof(S.other_names);
                    for (t.len = 0; t.len + 1 < t.cap && S.other_names[t.len]; ++t.len) {}
                    if (t.len) s_ch(&t, ',');
                    s_cat(&t, mon_name(&m));
                }
                S.others++;
            }
        }
        S.cur = page + 0x1000;
    }
}

/* ---------- learning where the game keeps the battle (v0.5.4) ----------
 * Once the sweep has identified the rival (its PK9 at address A), walk the pointers backwards:
 * find every word in the sweep window and in the main module that points at (or a little
 * before) A, then at those holders, and so on, until a holder sits in the main module: that is
 * a fixed path [main+R]+ok...+o1 = A (chain() form) that the next battles can follow directly,
 * whatever the rival (Tera Pokémon included, where nothing in the field copy changes). A path
 * that still leads to the same Pokémon once back on the field is the field's own list, not the
 * battle's, and is dropped. Paths live for the session; the debug line shows them. */
#define LEARN_T 12          /* holders kept per level */
#define LEARN_LV 5          /* pointer hops */
#define LEARN_SPAN 0x800    /* how far before a target a pointer may point (field offset) */
#define CHAINS_MAX 16
typedef struct { u64 addr; u32 off; u8 parent; } LNode;
typedef struct { u64 c[LEARN_LV + 1]; int n; int checked, field; u32 hits, confirmed, field_streak, obs; } LChain;
LOCAL struct {
    int state;               /* 0 idle, 1 searching, 2 finished */
    int level;               /* level being searched (1..LEARN_LV) */
    u32 ec;
    LNode node[LEARN_LV + 1][LEARN_T]; u32 nn[LEARN_LV + 1];
    u64 cur; int region;     /* 0 sweep window, 1 main module */
    u64 tmin, tmax;
    LChain ch[CHAINS_MAX]; u32 nch;
    u32 runs;
    int hit;                 /* a battle path gave this battle's rival */
} L;

LOCAL void learn_targets(void) {
    L.tmin = ~0ull; L.tmax = 0;
    for (u32 i = 0; i < L.nn[L.level - 1]; ++i) {
        u64 t = L.node[L.level - 1][i].addr;
        if (t < L.tmin) L.tmin = t;
        if (t > L.tmax) L.tmax = t;
    }
    L.tmin = L.tmin > LEARN_SPAN ? L.tmin - LEARN_SPAN : 0;
}
LOCAL void learn_start(u64 a, u32 ec) {
    memset(L.nn, 0, sizeof(L.nn));
    L.state = 1; L.level = 1; L.ec = ec; L.region = 0; L.cur = S.lo; L.runs++;
    L.node[0][0].addr = a; L.node[0][0].off = 0; L.node[0][0].parent = 0;
    L.nn[0] = 1;
    learn_targets();
}
LOCAL int chain_same(const LChain* a, const LChain* b) {
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; ++i) if (a->c[i] != b->c[i]) return 0;
    return 1;
}
LOCAL void learn_found(const EdenDsmodHostApi* h, u64 holder, u32 off, u32 parent) {
    if (holder >= h->main_base && holder < h->main_base + h->main_size) {
        /* a fixed path: main offset, this hop's offset, then each parent's offset down to A */
        LChain c; memset(&c, 0, sizeof(c));
        c.c[0] = holder - h->main_base;
        c.c[1] = off;
        int n = 2;
        u32 p = parent;
        for (int lv = L.level - 1; lv >= 1; --lv) { c.c[n++] = L.node[lv][p].off; p = L.node[lv][p].parent; }
        c.n = n;
        for (u32 i = 0; i < L.nch; ++i) if (chain_same(&L.ch[i], &c)) return;
        if (L.nch < CHAINS_MAX) L.ch[L.nch++] = c;
        return;
    }
    if (L.level >= LEARN_LV) return;
    LNode* row = L.node[L.level];
    u32* n = &L.nn[L.level];
    for (u32 i = 0; i < *n; ++i) if (row[i].addr == holder) return;
    LNode nd = {holder, off, (u8)parent};
    if (*n < LEARN_T) { row[(*n)++] = nd; return; }
    u32 worst = 0;                                    /* keep the tightest offsets */
    for (u32 i = 1; i < *n; ++i) if (row[i].off > row[worst].off) worst = i;
    if (off < row[worst].off) row[worst] = nd;
}
LOCAL void learn_step(const EdenDsmodHostApi* h, u64 budget) {
    if (L.state != 1) return;
    u64 lo = L.region == 0 ? S.lo : h->main_base, hi = L.region == 0 ? S.hi : h->main_base + h->main_size;
    u64 end = L.cur + budget;
    while (L.cur < end) {
        if (L.cur >= hi) {
            if (L.region == 0 && h->main_size) { L.region = 1; L.cur = h->main_base & ~0xFFFull; return; }
            /* level done */
            if (L.nn[L.level] == 0 || L.level >= LEARN_LV) { L.state = 2; return; }
            L.level++; L.region = 0; L.cur = S.lo;
            learn_targets();
            return;
        }
        u64 page = L.cur & ~0xFFFull;
        const u8* p = h->get_read_pointer ? h->get_read_pointer(h->userdata, page, 0x1000) : 0;
        if (!p) {
            if (h->is_mapped && !h->is_mapped(h->userdata, page, 0x1000)) { L.cur = page + 0x1000; continue; }
            if (!rd(h, page, S.page, 0x1000)) { L.cur = page + 0x1000; continue; }
            p = (const u8*)S.page;
        }
        const LNode* tg = L.node[L.level - 1];
        u32 nt = L.nn[L.level - 1];
        for (u32 off = 0; off < 0x1000; off += 8) {
            u64 v = le64(p + off);
            if (v < L.tmin || v > L.tmax) continue;
            for (u32 t = 0; t < nt; ++t)
                if (v <= tg[t].addr && tg[t].addr - v <= LEARN_SPAN && page + off != tg[t].addr)
                    learn_found(h, page + off, (u32)(tg[t].addr - v), t);
        }
        L.cur = page + 0x1000;
        (void)lo;
    }
}
/* Follows a path; 1 with the Pokémon when it ends on a valid PK9 that is not the player's. */
LOCAL int chain_mon(const EdenDsmodHostApi* h, const LChain* c, u64* at, Mon* m) {
    u64 a = chain(h, c->c, c->n);
    u8 pk[PK_PARTY];
    if (!a || !rd(h, a, pk, PK_PARTY) || le32(pk) == 0 || le16(pk + 4) != 0) return 0;
    if (!pk9_decrypt(pk) || !mon_from_pk(pk, m) || is_party_ec(m->ec)) return 0;
    *at = a;
    return 1;
}
/* On the way back to the field: a path that still reaches the same Pokémon is the field's. */
/* ---------- choosing the rival by hand (v0.5.7) ----------
 * On device the "which field Pokémon changed" guess picked the wrong one (Greedent for a
 * Capsakid). So in a battle the companion lists the wild Pokémon it has seen around you, the
 * guess first, and you tap the one you are fighting. That choice is certain, and it is also
 * the starting point the path learner needs. */
#define PICK_MAX 16
LOCAL int g_sp_open, g_sp_letter = -1; /* species browser (see publish_species_picker) */
LOCAL Cand g_pick[PICK_MAX]; LOCAL u32 g_npick;
LOCAL Cand g_chosen; LOCAL int g_has_chosen, g_pick_open_req;
LOCAL u32 g_sp_level; LOCAL int g_sp_level_set; /* a rival known by species only: its level (found next to its name, or set by you) */
LOCAL int cand_read(const EdenDsmodHostApi* h, u64 addr, u32 ec, Mon* m) {
    u8 pk[PK_PARTY];
    if (!rd(h, addr, pk, PK_PARTY) || le32(pk) != ec || le16(pk + 4) != 0) return 0;
    return pk9_decrypt(pk) && mon_from_pk(pk, m) && !m->egg && !is_party_ec(m->ec);
}
LOCAL void pick_add(const EdenDsmodHostApi* h, u64 addr, u32 ec) {
    if (g_npick >= PICK_MAX) return;
    for (u32 i = 0; i < g_npick; ++i) if (g_pick[i].addr == addr) return;
    Mon m;
    if (!cand_read(h, addr, ec, &m)) return;
    g_pick[g_npick].addr = addr; g_pick[g_npick].ec = ec; g_pick[g_npick].m = m; g_pick[g_npick].from_path = 0;
    g_npick++;
}
LOCAL void pick_build(const EdenDsmodHostApi* h) {
    g_npick = 0;
    for (u32 i = 0; i < S.nopp; ++i) pick_add(h, S.opp[i].addr, S.opp[i].ec);      /* the guesses first */
    for (u32 i = S.nbase; i-- > 0 && g_npick < PICK_MAX;) pick_add(h, S.base[i].addr, S.base[i].ec); /* newest first */
}

LOCAL void learn_battle_over(const EdenDsmodHostApi* h) {
    g_has_chosen = 0; g_pick_open_req = 0; g_npick = 0; g_sp_open = 0; g_sp_letter = -1; g_sp_level = 0; g_sp_level_set = 0;
    (void)h;
    for (u32 i = 0; i < L.nch; ++i) L.ch[i].checked = 1; /* judged on the field from now on */
    /* an unfinished search starts again next battle, and so does a finished one that found no
     * path (v0.6.6 stayed at "hecho | caminos 0" for good after one fruitless battle) */
    if (L.state == 1 || (L.state == 2 && !L.hit)) L.state = 0;
    L.hit = 0;
}
/* In battle: a learnt battle path wins over the sweep's guess. */
LOCAL void learn_use(const EdenDsmodHostApi* h) {
    L.hit = 0;
    u32 sweep_ec = 0; /* the rival the sweep found on its own, if any */
    for (u32 i = 0; i < S.nopp; ++i) if (!S.opp[i].from_path) { sweep_ec = S.opp[i].ec; break; }
    if (g_has_chosen) sweep_ec = g_chosen.ec; /* chosen by hand: certain */
    int best = -1; u64 best_a = 0; Mon best_m;
    for (u32 i = 0; i < L.nch; ++i) {
        /* only paths watched on the field for a while (a field list would have shown itself) */
        if (!L.ch[i].checked || L.ch[i].field || (L.ch[i].obs < 20 && !L.ch[i].confirmed)) continue;
        u64 a; Mon m;
        if (!chain_mon(h, &L.ch[i], &a, &m)) continue;
        if (sweep_ec && m.ec == sweep_ec) L.ch[i].confirmed++;   /* independent agreement */
        /* a confirmed path always; an unconfirmed one only when the sweep has nothing */
        if (best < 0 || L.ch[i].confirmed > L.ch[best].confirmed) {
            if (L.ch[i].confirmed || !sweep_ec) { best = (int)i; best_a = a; best_m = m; }
        }
    }
    if (best < 0) return;
    S.opp[0].addr = best_a; S.opp[0].ec = best_m.ec; S.opp[0].m = best_m; S.opp[0].from_path = 1;
    if (S.nopp == 0) S.nopp = 1;
    L.ch[best].hits++;
    L.hit = 1;
}
LOCAL void hexs(Str* s, u64 v) {
    const char* d = "0123456789ABCDEF";
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) { u32 n = (u32)(v >> i) & 15; if (n || started || i == 0) { s_ch(s, d[n]); started = 1; } }
}

/* Called every refresh: overworld state, mode switch, live re-read of the battle's Pokémon. */
LOCAL void ns_slots_read(const EdenDsmodHostApi* h);
LOCAL void scan_refresh(const EdenDsmodHostApi* h) {
    ns_slots_read(h);
    u64 a = chain(h, kOverworldChain, 4);
    u8 b;
    g.ow_ok = a && rd(h, a, &b, 1);
    g.ow_raw = g.ow_ok ? b : 0;
    g.overworld = g.ow_ok && b == 0x11;
    if (g.overworld) g.ingame = 1;
    /* Never dropped: on device the byte also reads 18 during battles, not only at the title. */
    if (g.ow_ok) {
        int seen = 0;
        for (u32 i = 0; i < S.now_seen; ++i) if (S.ow_seen[i] == b) seen = 1;
        if (!seen && S.now_seen < 6) S.ow_seen[S.now_seen++] = b;
    }
    /* Sweep window: around the player's own live Pokémon. On device (Eden, NCE) they sit
     * ~2 GB past the host's heap window, which held none at all; the battle's Pokémon come from
     * the same allocator. A narrow window keeps the sweep short and away from memory the game
     * maps and unmaps while loading. Re-centred if the live objects move far. */
    if (g.live_addr0) {
        u64 a0 = g.live_addr0 & ~0xFFFull;
        if (!S.lo || a0 < S.lo + SCAN_HALF / 4 || a0 >= S.hi - SCAN_HALF / 4) {
            S.lo = a0 > SCAN_HALF ? a0 - SCAN_HALF : 0x1000;
            S.hi = a0 + SCAN_HALF;
            S.cur = S.lo; S.ready = 0; S.nbase = 0; S.nfound = 0; S.nopp = 0; S.recentred++;
        }
    }
    /* v0.4.2 moved the sweep window around the live party objects when they sat outside the
     * host's heap window; on device that crashed the emulator at boot (pages outside the heap
     * window are not safe to sweep through host pointers). Removed: only the self-test below
     * reports where they are. */
    /* Self-test: does the probe accept the player's own live PK9 where it really is? */
    if (g.live_addr0) {
        u64 a0 = g.live_addr0;
        const u8* p = h->get_read_pointer ? h->get_read_pointer(h->userdata, a0 & ~0xFFFull, 0x1000) : 0;
        u8 pk[PK_PARTY];
        int got = rd(h, a0, pk, PK_PARTY);
        S.t_in = a0 >= S.lo && a0 < S.hi;
        S.t_ptr = p != 0;
        S.t_hdr = got && le32(pk) != 0 && le16(pk + 4) == 0 && le16(pk + 6) != 0;
        S.t_spc = S.t_hdr && quick_species(pk);
        S.t_pk = S.t_spc && pk9_decrypt(pk);
        S.t_al = (u32)(a0 & 7);
    }
    u64 box = chain(h, kBoxChain, 7);
    if (box) { S.box_lo = box; S.box_hi = box + 32ull * 30 * PK_PARTY; }
    int want = g.ow_ok && !g.overworld ? 1 : 0;
    /* The byte flickers (4/5/6/18 seen on device while walking around and in menus): only a
     * state that holds for two refreshes in a row switches, so the field sweep can finish. */
    if (want == S.mode) S.pending = 0;
    else if (++S.pending < 2) want = S.mode;
    if (want != S.mode) {
        int restart = 0;
        if (S.mode == 0) S.cur_field = S.cur; /* the field sweep resumes where it was */
        S.pending = 0;                 /* entering or leaving the field: start a new sweep */
        if (S.mode == 1) {                /* a battle ended: its Pokémon are history from now on */
            for (u32 i = 0; i < S.nopp && S.nbase < BASE_MAX; ++i)
                if (!in_base(S.opp[i].addr, S.opp[i].ec)) { S.base[S.nbase].addr = S.opp[i].addr; S.base[S.nbase].ec = S.opp[i].ec; S.nbase++; }
            for (u32 i = 0; i < S.nfound && S.nbase < BASE_MAX; ++i)
                if (!in_base(S.found[i].addr, S.found[i].ec)) { S.base[S.nbase].addr = S.found[i].addr; S.base[S.nbase].ec = S.found[i].ec; S.nbase++; }
            /* The game writes the battle's result back into the field Pokémon (HP, status...)
             * after the battle: until a fresh field sweep has seen every PK9 again, a new battle
             * would take that old rival for "the one that changed" (seen on device). */
            if (S.nopp || S.nfound) { S.ready = 0; restart = 1; } /* a menu (nothing found) keeps the baseline */
            learn_battle_over(h);
        }
        S.mode = want;
        /* On device the byte also flips between 17 and other values (19...) while you walk: a
         * field sweep restarted at every flip never finished (v0.5.3: "campo 42% v0"). */
        S.cur = want == 0 && !restart && S.cur_field > S.lo && S.cur_field < S.hi ? S.cur_field : S.lo;
        S.nfound = 0;
        S.others = 0; S.other_names[0] = 0; S.msweeps = 0; S.changed = 0;
        if (!want) S.nopp = 0;
    }
    /* Keep the battle's Pokémon current; drop any that stopped being valid. */
    u32 keep = 0;
    for (u32 i = 0; i < S.nopp; ++i) {
        u8 pk[PK_PARTY];
        Mon m;
        if (!rd(h, S.opp[i].addr, pk, PK_PARTY) || le32(pk) != S.opp[i].ec || !pk9_decrypt(pk) ||
            !mon_from_pk(pk, &m))
            continue;
        S.opp[keep] = S.opp[i];
        S.opp[keep].m = m;
        keep++;
    }
    S.nopp = keep;
    if (S.mode == 0) {
        /* on the field, a path that reaches any Pokémon is a field list, not the battle */
        /* A field list keeps reaching Pokémon while you walk about; the battle's own
         * structure is emptied when the battle ends. Five seconds of hits = a field list. */
        for (u32 i = 0; i < L.nch; ++i) {
            u64 at; Mon m;
            if (!L.ch[i].checked || L.ch[i].field || !g.overworld) continue;
            if (chain_mon(h, &L.ch[i], &at, &m)) { if (++L.ch[i].field_streak >= 20) L.ch[i].field = 1; }
            else L.ch[i].field_streak = 0;
            L.ch[i].obs++;
        }
    } else {
        learn_use(h);
        if (g_has_chosen && g_chosen.addr) {   /* addr 0: chosen by species from the list */
            Mon m;
            if (cand_read(h, g_chosen.addr, g_chosen.ec, &m)) g_chosen.m = m; else g_has_chosen = 0;
        }
        if (!g_has_chosen || g_pick_open_req) pick_build(h);
        /* the sweep found the rival on its own: learn the way to it (once per battle) */
        if (!L.hit && S.nopp && L.state == 0 && S.ready && L.runs < 12 && L.nch < CHAINS_MAX)
            learn_start(S.opp[0].addr, S.opp[0].ec);
    }
}

/* Whether the player's optional user:sprites/<national>.png exists (asked once per species). */
LOCAL u8 g_spr_known[1100]; /* 0 unknown, 1 present, 2 absent */
LOCAL const EdenDsmodHostApi* P;
LOCAL int sprite_present(u32 species) {
    if (species == 0 || species >= sizeof(g_spr_known)) return 0;
    if (!g_spr_known[species]) {
        int ok = 0;
        if (user_source_ok(P)) {
            char path[48];
            Str s; s_init(&s, path, sizeof(path));
            s_cat(&s, "user:sprites/"); s_u(&s, species, 0); s_cat(&s, ".png");
            ok = P->read_romfs(P->userdata, path, 0, 0, 0) > 0;
        }
        g_spr_known[species] = ok ? 1 : 2;
    }
    return g_spr_known[species] == 1;
}

/* ---------- publishing ---------- */
LOCAL char g_key[96], g_val[240];
LOCAL void pi(const char* name, s64 v) { P->publish_i64(P->userdata, name, v); }
LOCAL void pt(const char* name, const char* v) { P->publish_text(P->userdata, name, v ? v : ""); }
/* key "<prefix><index><suffix>" */
LOCAL const char* K(const char* prefix, s64 idx, const char* suffix) {
    Str s; s_init(&s, g_key, sizeof(g_key)); s_cat(&s, prefix);
    if (idx >= 0) s_u(&s, (u64)idx, 0);
    s_cat(&s, suffix);
    return g_key;
}
LOCAL const char* name_or_num(const NameList* nl, u32 id, const char* fallback_prefix) {
    const char* n = namelist_get(nl, id);
    if (n) return n;
    Str s; s_init(&s, g_val, sizeof(g_val)); s_cat(&s, fallback_prefix); s_u(&s, id, 0);
    return g_val;
}
LOCAL const char* species_name(u32 nat) {
    return name_or_num(&g_species, g_species_internal ? internal9((u16)nat) : nat, "#");
}
LOCAL const char* mon_name(const Mon* m) {
    if (m->egg) return TR("Huevo", "Egg");
    if (m->nick[0]) return m->nick;
    return species_name(m->species);
}

LOCAL void publish_card(u32 i, const Mon* m) {
    pi(K("sv.p", i, ".valid"), 1);
    pi(K("sv.p", i, ".species"), m->species);
    pi(K("sv.p", i, ".spr"), m->egg ? 0 : m->species);
    pi(K("sv.p", i, ".has_spr"), !m->egg && sprite_present(m->species));
    pi(K("sv.p", i, ".form"), m->form);
    pi(K("sv.p", i, ".egg"), m->egg);
    pi(K("sv.p", i, ".shiny"), m->shiny);
    pi(K("sv.p", i, ".level"), m->level);
    pi(K("sv.p", i, ".hp"), m->hp);
    pi(K("sv.p", i, ".hpmax"), m->hpmax);
    pi(K("sv.p", i, ".fainted"), m->hp == 0 && !m->egg);
    pi(K("sv.p", i, ".male"), m->gender == 0);
    pi(K("sv.p", i, ".female"), m->gender == 1);
    pi(K("sv.p", i, ".status"), m->status);
    pt(K("sv.p", i, ".st"), m->hp == 0 ? TR("DEB", "FNT") : status_short(m->status));
    pt(K("sv.p", i, ".name"), mon_name(m));
    /* HP colour band like the games: green > 50%, yellow > 20%, red below. */
    u32 band = m->hpmax == 0 ? 0 : (m->hp * 100 > m->hpmax * 50 ? 0 : (m->hp * 100 > m->hpmax * 20 ? 1 : 2));
    pi(K("sv.p", i, ".hpband"), band);
    pi(K("sv.p", i, ".hpg"), band == 0);
    pi(K("sv.p", i, ".hpy"), band == 1);
    pi(K("sv.p", i, ".hpr"), band == 2);
    pi(K("sv.p", i, ".alive"), m->hp != 0 || m->egg);
}

LOCAL void publish_type(const char* prefix, s64 idx, const char* suffix, u32 type) {
    Str s; char base[64]; s_init(&s, base, sizeof(base)); s_cat(&s, prefix);
    if (idx >= 0) s_u(&s, (u64)idx, 0);
    s_cat(&s, suffix);
    Str k; char key[96];
    s_init(&k, key, sizeof(key)); s_cat(&k, base); pi(key, type);
    s_init(&k, key, sizeof(key)); s_cat(&k, base); s_cat(&k, "n");
    pt(key, name_or_num(&g_types, type, TR("Tipo ", "Type ")));
}

/* Detail sheet of one party slot under "sv.d<slot>." (the manifest shows the selected one). */
LOCAL char g_pfx[16], g_ks[96];
LOCAL const char* KS(const char* suffix) {
    Str s; s_init(&s, g_ks, sizeof(g_ks)); s_cat(&s, g_pfx); s_cat(&s, suffix);
    return g_ks;
}
LOCAL u32 g_def_tera; /* 1 + Tera type used as the defensive type of the next sheet, 0 = species types */
LOCAL const GPers* base_of(const Mon* m) {
    const GPers* pe = 0;
    for (u32 i = 0; i < g_gpers_n && !m->egg; ++i) {
        if (g_gpers[i].nat != m->species) continue;
        if (g_gpers[i].form == m->form) { pe = &g_gpers[i]; break; }
        if (g_gpers[i].form == 0 && !pe) pe = &g_gpers[i];
    }
    return pe && pe->base[0] ? pe : 0;
}
LOCAL void publish_detail_pfx(const char* pfx, const Mon* m) {
    { Str s; s_init(&s, g_pfx, sizeof(g_pfx)); s_cat(&s, pfx); }
    char p_wk[24], p_rs[24], p_im[24];
    { Str s; s_init(&s, p_wk, sizeof(p_wk)); s_cat(&s, g_pfx); s_cat(&s, "wk"); }
    { Str s; s_init(&s, p_rs, sizeof(p_rs)); s_cat(&s, g_pfx); s_cat(&s, "rs"); }
    { Str s; s_init(&s, p_im, sizeof(p_im)); s_cat(&s, g_pfx); s_cat(&s, "im"); }
    pi(KS("valid"), 1);
    pt(KS("name"), mon_name(m));
    pi(KS("spr"), m->egg ? 0 : m->species);
    pi(KS("has_spr"), !m->egg && sprite_present(m->species));
    pi(KS("level"), m->level);
    pi(KS("male"), m->gender == 0);
    pi(KS("female"), m->gender == 1);
    pi(KS("shiny"), m->shiny);
    pt(KS("status"), status_long(m));
    pt(KS("item"), m->item ? name_or_num(&g_items, m->item, TR("Objeto ", "Item ")) : TR("Ninguno", "None"));
    pt(KS("ability"), name_or_num(&g_abil, m->ability, TR("Habilidad ", "Ability ")));
    pt(KS("nature"), name_or_num(&g_nat, m->nature, TR("Natur. ", "Nature ")));
    publish_type(KS("tera"), -1, "", m->tera <= 18 ? m->tera : 0);
    pi(KS("tera_ok"), m->tera < 19);

    u32 t1 = 0, t2 = 0;
    int typed = !m->egg && species_types(m->species, m->form, &t1, &t2);
    pi(KS("typed"), typed);
    if (typed) {
        publish_type(KS("t1"), -1, "", t1);
        pi(KS("dual"), t2 != t1);
        if (t2 != t1) publish_type(KS("t2"), -1, "", t2);
    }
    for (u32 k = 0; k < 6; ++k) pi(K(KS("stat"), k, ""), m->stat[k]);
    if (typed && g_def_tera) { t1 = t2 = g_def_tera - 1; }

    for (u32 j = 0; j < 4; ++j) {
        u32 mv = m->moves[j];
        pi(K(KS("m"), j, ".valid"), mv != 0);
        if (!mv) continue;
        MoveInfo mi = move_info(mv);
        pt(K(KS("m"), j, ".name"), name_or_num(&g_moves, mv, TR("Mov. ", "Move ")));
        pi(K(KS("m"), j, ".known"), mi.ok);
        publish_type(KS("m"), j, ".t", mi.ok ? mi.type : 0);
        pi(K(KS("m"), j, ".cat"), mi.cat);
        Str s; s_init(&s, g_val, sizeof(g_val));
        s_u(&s, m->pp[j], 0);
        if (mi.ok) { s_ch(&s, '/'); s_u(&s, mi.pp * (5 + m->ppups[j]) / 5, 0); }
        pt(K(KS("m"), j, ".pp"), g_val);
        s_init(&s, g_val, sizeof(g_val));
        if (mi.ok && mi.power) { s_cat(&s, TR("Pot. ", "Pow. ")); s_u(&s, mi.power, 0); }
        pt(K(KS("m"), j, ".pow"), g_val);
    }

    /* Defensive matchups for the species' types (no abilities, items or Tera). */
    u32 nwk = 0, nrs = 0, nim = 0;
    /* Strongest first: x4 before x2, x0.25 before x0.5. */
    static const u32 order[5] = {0, 16, 8, 1, 2};
    if (typed) for (u32 pass = 0; pass < 5; ++pass) {
        for (u32 a = 0; a < T_COUNT; ++a) {
            u32 e1 = eff(a, t1), e2 = t2 != t1 ? eff(a, t2) : 2;
            u32 mul = e1 * e2; /* 0, 1, 2, 4 (= x0.25..), in quarters: 4 = x1 */
            if (mul != order[pass]) continue;
            const char* x = 0; const char* list = 0; u32* n = 0; u32 lv = 0;
            if (mul == 0) { list = p_im; n = &nim; x = "x0"; lv = 4; }
            else if (mul == 16) { list = p_wk; n = &nwk; x = "x4"; lv = 0; }
            else if (mul == 8) { list = p_wk; n = &nwk; x = "x2"; lv = 1; }
            else if (mul == 2) { list = p_rs; n = &nrs; x = TR("x0,5", "x0.5"); lv = 2; }
            else if (mul == 1) { list = p_rs; n = &nrs; x = TR("x0,25", "x0.25"); lv = 3; }
            if (!list) continue;
            publish_type(list, *n, ".t", a);
            pt(K(list, *n, ".x"), x);
            pi(K(list, *n, ".ok"), 1);
            pi(K(list, *n, ".lv"), lv);
            { static const char* lvk[5] = {".lv0", ".lv1", ".lv2", ".lv3", ".lv4"};
              for (u32 q = 0; q < 5; ++q) pi(K(list, *n, lvk[q]), q == lv); }
            ++*n;
        }
    }
    pi(KS("wk.n"), nwk); pi(KS("rs.n"), nrs); pi(KS("im.n"), nim);
    /* Battle sheet shows 6 weaknesses and 9 resistances; say how many more there are. */
    { Str s; s_init(&s, g_val, sizeof(g_val)); if (nwk > 6) { s_cat(&s, TR("y ", "and ")); s_u(&s, nwk - 6, 0); s_cat(&s, TR(" más", " more")); } pt(KS("wk.more"), g_val); }
    { Str s; s_init(&s, g_val, sizeof(g_val)); if (nrs > 9) { s_cat(&s, TR("y ", "and ")); s_u(&s, nrs - 9, 0); s_cat(&s, TR(" más", " more")); } pt(KS("rs.more"), g_val); }
    /* Base stats of the species/form. */
    {
        const GPers* pe = base_of(m);
        int ok = pe != 0;
        pi(KS("bs_ok"), ok);
        u32 tot = 0;
        static const char* ck[5] = {".c0", ".c1", ".c2", ".c3", ".c4"};
        for (u32 k = 0; k < 6; ++k) {
            u32 v = ok ? pe->base[k] : 0; tot += v; pi(K(KS("bs"), k, ""), v);
            u32 band = v < 50 ? 0 : v < 80 ? 1 : v < 100 ? 2 : v < 130 ? 3 : 4; /* red .. blue */
            for (u32 q = 0; q < 5; ++q) pi(K(KS("bs"), k, ck[q]), ok && q == band);
        }
        pi(KS("bst"), tot);
    }
    pi(KS("wk.none"), typed && nwk == 0);
    pi(KS("rs.none"), typed && nrs == 0);
    pi(KS("im.none"), typed && nim == 0);
}

LOCAL void publish_detail(u32 slot, const Mon* m) {
    char pfx[16];
    Str s; s_init(&s, pfx, sizeof(pfx)); s_cat(&s, "sv.d"); s_u(&s, slot, 0); s_ch(&s, '.');
    publish_detail_pfx(pfx, m);
}

/* ---------- battle ---------- */

LOCAL void publish_hp(const char* pfx, const Mon* m) {
    Str s; char k[48];
    #define HPK(f) (s_init(&s, k, sizeof(k)), s_cat(&s, pfx), s_cat(&s, f), k)
    u32 band = m->hpmax == 0 ? 0 : (m->hp * 100 > m->hpmax * 50 ? 0 : (m->hp * 100 > m->hpmax * 20 ? 1 : 2));
    pi(HPK("hp"), m->hp);
    pi(HPK("hpmax"), m->hpmax);
    pi(HPK("hpg"), band == 0);
    pi(HPK("hpy"), band == 1);
    pi(HPK("hpr"), band == 2);
    #undef HPK
}

/* Experience needed for a level, by growth rate (PKHeX order: 0 medium fast, 1 erratic,
 * 2 fluctuating, 3 medium slow, 4 fast, 5 slow). */
LOCAL u32 exp_for(u32 g, u32 n) {
    if (n <= 1) return 0;
    u64 c = (u64)n * n * n;
    switch (g) {
    case 1: return (u32)(n < 50 ? c * (100 - n) / 50 : n < 68 ? c * (150 - n) / 100 : n < 98 ? c * ((1911 - 10 * n) / 3) / 500 : c * (160 - n) / 100);
    case 2: return (u32)(n < 15 ? c * ((n + 1) / 3 + 24) / 50 : n < 36 ? c * (n + 14) / 50 : c * (n / 2 + 32) / 50);
    case 3: return (u32)(6 * c / 5 + 100 * n - 15 * (u64)n * n - 140);
    case 4: return (u32)(4 * c / 5);
    case 5: return (u32)(5 * c / 4);
    default: return (u32)c;
    }
}
/* A Pokémon whose party stats are not in memory (the wild one you fight is still the field
 * copy, box format): level from its experience, stats from base stats, IVs, EVs and nature. */
LOCAL void compute_stats(Mon* m) {
    const GPers* pe = 0;
    for (u32 i = 0; i < g_gpers_n; ++i) {
        if (g_gpers[i].nat != m->species) continue;
        if (g_gpers[i].form == m->form) { pe = &g_gpers[i]; break; }
        if (g_gpers[i].form == 0 && !pe) pe = &g_gpers[i];
    }
    if (!pe || !pe->base[0]) return;
    u32 lv = 1;
    while (lv < 100 && exp_for(pe->growth, lv + 1) <= m->exp) ++lv;
    static const u8 nat_stat[5] = {1, 2, 5, 3, 4}; /* nature order: ATK DEF SPE SPA SPD */
    u32 up = m->nature < 25 ? nat_stat[m->nature / 5] : 0, down = m->nature < 25 ? nat_stat[m->nature % 5] : 0;
    for (u32 k = 0; k < 6; ++k) {
        u32 core = (2u * pe->base[k] + m->iv[k] + m->ev[k] / 4u) * lv / 100u;
        u32 v = k == 0 ? core + lv + 10 : core + 5;
        if (k && up != down) { if (k == up) v = v * 110 / 100; else if (k == down) v = v * 90 / 100; }
        m->stat[k] = v;
    }
    if (m->species == 292) m->stat[0] = 1; /* Shedinja */
    m->level = lv;
    m->hpmax = m->stat[0];
    if (m->hp > m->hpmax) m->hp = m->hpmax;
    m->stats_ok = 1;
}

/* ---------- the rival by its name (v0.6.9, rewritten v0.7.3) ----------
 * In a battle the game writes names as UTF-16 text: the HP boxes, "¡Un Skiddo salvaje...!",
 * "¡Adelante, Inteleon!". The battle's own PK9 is nowhere to be found on the device, but these
 * texts are. NS sweeps the window for species names and "Nv. N" and remembers, for each place,
 * what was written there. On the field that is the picture of what is old. In a battle, a place
 * whose text CHANGES is a write: the game just put that name or level there. So:
 *  - the rival is the species (not in your team) written last, still on screen;
 *  - your Pokémon out is the species of your team written last (a forced switch writes it
 *    too: v0.7.2 looked at the first time a name was seen and missed Inteleon dragged in);
 *  - the rival's level is the "Nv. N" written nearest to the rival's name (the team menu
 *    writes all your levels: v0.7.2 took one of them).
 * Where the rival's name and level went is learnt at the end of a battle and read directly
 * in the next ones. */
#define NS_AM 16384
#define NS_TAB 4096
typedef struct { u64 addr; u16 what; u8 bw; u8 pad; } NsPlace;   /* what: species, or 2000 + level */
typedef struct { u64 addr; u16 what; u16 sweep; } NsWrite;
LOCAL struct {
    u16 pool[16384]; u32 npool;              /* names as UTF-16, one after another */
    u32 off[1100]; u8 len[1100]; int built;
    struct { u64 key; u16 sp; } tab[NS_TAB];
    NsPlace am[NS_AM]; u32 nam;              /* every place a name or level text was seen */
    u64 cur; int last_mode; u32 bsweep;
    u16 lastw[1100], lw[101];                /* battle sweep of the last write (0: none) */
    u16 cur_n[1100], last_n[1100];           /* written in this battle and on screen, per sweep */
    u16 lcur[101], llast[101];
    NsWrite rn[128]; u32 nrn;                /* the last writes of non-team names */
    NsWrite lv[128]; u32 nlv;                /* the last writes of levels */
    u64 slot[8]; u32 nslot; u32 slot_sp;     /* where the rival's name was last time, what it says now */
    u64 lslot; u32 slot_lv;                  /* and its level */
    u32 final, final_lv; u64 final_lv_at;
    u32 best, best_n, hits, writes, sweeps;
} NS;
LOCAL u64 ns_key(u16 a, u16 b, u16 c) { return (u64)a | ((u64)b << 16) | ((u64)c << 32); }
LOCAL u32 ns_slot(u64 k) { u64 x = k * 0x9E3779B97F4A7C15ull; return (u32)(x >> 52) & (NS_TAB - 1); }
LOCAL int ns_letter(u16 c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c < 0x250 && c != 0xD7 && c != 0xF7); }
LOCAL void ns_build(void) {
    if (NS.built || !g_species.count) return;
    NS.npool = 0;
    for (u32 sp = 1; sp < 1100; ++sp) {
        const char* n = species_name(sp);
        NS.len[sp] = 0;
        if (!n || !n[0] || n[0] == '#') continue;
        u32 start = NS.npool, l = 0;
        for (const u8* q = (const u8*)n; *q && NS.npool < 16380 && l < 24; ) {  /* UTF-8 -> UTF-16 */
            u32 c = *q++;
            if (c >= 0xC0 && c < 0xE0 && *q) c = ((c & 0x1F) << 6) | (*q++ & 0x3F);
            else if (c >= 0xE0 && c < 0xF0 && q[0] && q[1]) { c = ((c & 0x0F) << 12) | ((q[0] & 0x3F) << 6) | (q[1] & 0x3F); q += 2; }
            NS.pool[NS.npool++] = (u16)c; ++l;
        }
        if (l < 3) { NS.npool = start; continue; }
        NS.off[sp] = start; NS.len[sp] = (u8)l;
        u64 k = ns_key(NS.pool[start], NS.pool[start + 1], NS.pool[start + 2]);
        for (u32 i = ns_slot(k), tries = 0; tries < NS_TAB; i = (i + 1) & (NS_TAB - 1), ++tries)
            if (!NS.tab[i].key) { NS.tab[i].key = k; NS.tab[i].sp = (u16)sp; break; }
    }
    NS.built = 1;
}
LOCAL NsPlace* ns_place(u64 a) {
    /* full: start over, but only on the field (at half full, so a battle always has room);
     * mid-battle a fresh table would make every text look new */
    if (NS.nam >= (S.mode == 1 ? NS_AM * 3 / 4 : NS_AM / 2)) {
        if (S.mode == 1) {
            for (u32 i = (u32)((a * 0x9E3779B97F4A7C15ull) >> 50) & (NS_AM - 1), t = 0; t < 64; i = (i + 1) & (NS_AM - 1), ++t) {
                if (NS.am[i].addr == a) return &NS.am[i];
                if (!NS.am[i].addr) return 0;
            }
            return 0;
        }
        memset(NS.am, 0, sizeof(NS.am)); NS.nam = 0;
    }
    for (u32 i = (u32)((a * 0x9E3779B97F4A7C15ull) >> 50) & (NS_AM - 1), t = 0; t < 64; i = (i + 1) & (NS_AM - 1), ++t) {
        if (NS.am[i].addr == a) return &NS.am[i];
        if (!NS.am[i].addr) { NS.am[i].addr = a; NS.am[i].what = 0; NS.am[i].bw = 0; NS.nam++; return &NS.am[i]; }
    }
    return 0;
}
LOCAL int ns_party_species(u32 sp) {
    for (u32 i = 0; i < g.party_count && i < 6; ++i) if (g.mon[i].valid && g.mon[i].species == sp) return 1;
    return 0;
}
LOCAL int ns_never_rival(u32 sp) { return sp == 1007 || sp == 1008; }  /* Koraidon / Miraidon: the one you ride */
/* One text seen at a: record it, and in a battle count it. */
LOCAL void ns_seen(u64 a, u32 what) {
    NsPlace* e = ns_place(a);
    if (!e) return;
    NS.hits++;
    if (S.mode == 0) { e->what = (u16)what; e->bw = 0; return; }
    u16 now = (u16)(NS.bsweep + 1);
    if (e->what != what) {                       /* written during the battle */
        e->what = (u16)what; e->bw = 1; NS.writes++;
        if (what >= 2000) {
            u32 v = what - 2000;
            NS.lw[v] = now;
            NsWrite* w = &NS.lv[NS.nlv++ % 128]; w->addr = a; w->what = (u16)v; w->sweep = now;
        } else {
            NS.lastw[what] = now;
            if (!ns_party_species(what) && !ns_never_rival(what)) {
                NsWrite* w = &NS.rn[NS.nrn++ % 128]; w->addr = a; w->what = (u16)what; w->sweep = now;
            }
        }
    }
    if (!e->bw) return;                          /* there since the field: old news */
    if (what >= 2000) { if (NS.lcur[what - 2000] < 60000) NS.lcur[what - 2000]++; }
    else if (NS.cur_n[what] < 60000) NS.cur_n[what]++;
}
LOCAL void ns_step(const EdenDsmodHostApi* h, u64 budget) {
    ns_build();
    if (!NS.built || !S.lo || S.hi <= S.lo) return;
    if (S.mode != NS.last_mode) {
        if (S.mode == 1) {                       /* a battle starts: nothing written yet */
            for (u32 i = 0; i < NS_AM; ++i) NS.am[i].bw = 0;
            memset(NS.lastw, 0, sizeof(NS.lastw)); memset(NS.lw, 0, sizeof(NS.lw));
            memset(NS.cur_n, 0, sizeof(NS.cur_n)); memset(NS.last_n, 0, sizeof(NS.last_n));
            memset(NS.lcur, 0, sizeof(NS.lcur)); memset(NS.llast, 0, sizeof(NS.llast));
            NS.nrn = NS.nlv = 0; NS.bsweep = 0; NS.best = 0; NS.best_n = 0; NS.writes = 0;
            NS.final = 0; NS.final_lv = 0; NS.final_lv_at = 0;
        } else {                                 /* a battle ends: learn where the rival's texts sat */
            u32 n = 0; u64 sl[8];
            for (u32 i = 0; i < NS.nrn && i < 128 && n < 8; ++i)
                if (NS.final && NS.rn[i].what == NS.final) {
                    int dup = 0; for (u32 q = 0; q < n; ++q) dup |= sl[q] == NS.rn[i].addr;
                    if (!dup) sl[n++] = NS.rn[i].addr;
                }
            if (n) { memcpy(NS.slot, sl, sizeof(sl)); NS.nslot = n; }
            if (NS.final_lv_at) NS.lslot = NS.final_lv_at;
        }
        NS.last_mode = S.mode;
        NS.cur = S.lo;
    }
    if (NS.cur < S.lo) NS.cur = S.lo;
    u64 end = NS.cur + budget;
    while (NS.cur < end) {
        if (NS.cur >= S.hi) {
            NS.cur = S.lo; NS.sweeps++;
            if (S.mode == 1) {
                memcpy(NS.last_n, NS.cur_n, sizeof(NS.cur_n)); memset(NS.cur_n, 0, sizeof(NS.cur_n));
                memcpy(NS.llast, NS.lcur, sizeof(NS.lcur)); memset(NS.lcur, 0, sizeof(NS.lcur));
                NS.bsweep++;
            }
            break;
        }
        u64 page = NS.cur & ~0xFFFull;
        const u8* p = h->get_read_pointer ? h->get_read_pointer(h->userdata, page, 0x1000) : 0;
        NS.cur = page + 0x1000;
        if (!p) continue;
        for (u32 o = 2; o + 6 <= 0x1000; o += 2) {
            u16 c0 = (u16)(p[o] | (p[o + 1] << 8));
            if (!((c0 >= 'A' && c0 <= 'Z') || c0 == 0xC1 || c0 == 0xC9 || c0 == 0xCD || c0 == 0xD3 || c0 == 0xDA || c0 == 0xD1)) continue;
            u16 c1 = (u16)(p[o + 2] | (p[o + 3] << 8));
            if (c1 < 'a' || (c1 > 'z' && c1 < 0xC0)) continue;          /* names go on in lower case */
            u16 pv = (u16)(p[o - 2] | (p[o - 1] << 8));
            if (ns_letter(pv)) continue;
            if (c0 == 'N' && c1 == 'v' && o + 16 <= 0x1000) {   /* "Nv. 27" */
                u32 j = o + 4;
                if (p[j] == '.' && !p[j + 1]) j += 2;
                while (j + 2 <= 0x1000 && (p[j] == ' ' || p[j] == 0xA0) && !p[j + 1]) j += 2;
                u32 v = 0, d = 0;
                while (j + 2 <= 0x1000 && p[j] >= '0' && p[j] <= '9' && !p[j + 1] && d < 3) { v = v * 10 + (p[j] - '0'); j += 2; ++d; }
                if (d && v >= 1 && v <= 100) ns_seen(page + o, 2000 + v);
                if (d) continue;
            }
            u64 k = ns_key(c0, c1, (u16)(p[o + 4] | (p[o + 5] << 8)));
            for (u32 i = ns_slot(k), t = 0; t < 8 && NS.tab[i].key; i = (i + 1) & (NS_TAB - 1), ++t) {
                if (NS.tab[i].key != k) continue;
                u32 sp = NS.tab[i].sp, L = NS.len[sp];
                if (o + 2 * (L + 1) > 0x1000) continue;                 /* crosses the page: skip */
                const u16* nm = NS.pool + NS.off[sp];
                u32 m = 0;
                while (m < L && (u16)(p[o + 2 * m] | (p[o + 2 * m + 1] << 8)) == nm[m]) ++m;
                if (m != L) continue;
                if (ns_letter((u16)(p[o + 2 * L] | (p[o + 2 * L + 1] << 8)))) continue; /* longer word */
                ns_seen(page + o, sp);
                break;
            }
        }
    }
}
/* Read the learnt slots (every refresh, in battle): the species and level written there now. */
LOCAL void ns_slots_read(const EdenDsmodHostApi* h) {
    NS.slot_sp = 0; NS.slot_lv = 0;
    if (S.mode != 1 || !NS.built) return;
    u32 cnt[8] = {0}, sps[8] = {0}, ns = 0;
    for (u32 i = 0; i < NS.nslot; ++i) {
        u16 b[26];
        if (!rd(h, NS.slot[i], b, sizeof(b))) continue;
        u64 k = ns_key(b[0], b[1], b[2]);
        for (u32 j = ns_slot(k), t = 0; t < 8 && NS.tab[j].key; j = (j + 1) & (NS_TAB - 1), ++t) {
            if (NS.tab[j].key != k) continue;
            u32 sp = NS.tab[j].sp, L = NS.len[sp], m = 0;
            if (L > 24) continue;
            while (m < L && b[m] == NS.pool[NS.off[sp] + m]) ++m;
            if (m != L || ns_letter(b[L]) || ns_party_species(sp) || ns_never_rival(sp)) continue;
            u32 q = 0; while (q < ns && sps[q] != sp) ++q;
            if (q == ns && ns < 8) { sps[ns] = sp; cnt[ns] = 0; ns++; }
            if (q < 8) cnt[q]++;
            break;
        }
    }
    u32 bi = 0;
    for (u32 q = 1; q < ns; ++q) if (cnt[q] > cnt[bi]) bi = q;
    if (ns) NS.slot_sp = sps[bi];
    if (NS.lslot) {
        u16 b[8];
        const NsPlace* e = 0;   /* only once written in this battle: else it is the last rival's */
        for (u32 i = (u32)((NS.lslot * 0x9E3779B97F4A7C15ull) >> 50) & (NS_AM - 1), t = 0; t < 64; i = (i + 1) & (NS_AM - 1), ++t) {
            if (NS.am[i].addr == NS.lslot) { e = &NS.am[i]; break; }
            if (!NS.am[i].addr) break;
        }
        if (e && e->bw && rd(h, NS.lslot, b, sizeof(b)) && b[0] == 'N' && b[1] == 'v') {
            u32 j = 2, v = 0, d = 0;
            if (b[j] == '.') ++j;
            while (j < 8 && (b[j] == ' ' || b[j] == 0xA0)) ++j;
            while (j < 8 && b[j] >= '0' && b[j] <= '9' && d < 3) { v = v * 10 + (b[j] - '0'); ++j; ++d; }
            if (d && v >= 1 && v <= 100) NS.slot_lv = v;
        }
    }
}
/* The species written last among those on screen (written in this battle, this sweep or the
 * last), then the one shown most; party = 1: your team's, 0: the rest. 0 when none is clear. */
LOCAL u32 ns_pick(int party) {
    if (S.mode != 1 || NS.bsweep < 1) return 0;   /* after one whole sweep of the battle */
    u32 best = 0, bw = 0, bn = 0, tie = 0;
    for (u32 sp = 1; sp < 1100; ++sp) {
        if (!NS.lastw[sp] || ns_party_species(sp) != party || (!party && ns_never_rival(sp))) continue;
        u32 n = (u32)NS.cur_n[sp] > NS.last_n[sp] ? NS.cur_n[sp] : NS.last_n[sp];
        if (!n) continue;
        u32 w = NS.lastw[sp];
        if (w > bw || (w == bw && n > bn)) { best = sp; bw = w; bn = n; tie = 0; }
        else if (w == bw && n == bn) tie = 1;
    }
    if (!party) { NS.best = best; NS.best_n = bn; }
    return tie ? 0 : best;
}
LOCAL u32 ns_mine(void) { return ns_pick(1); }
/* The rival's level: of the "Nv. N" written in this battle and still there, the one nearest in
 * memory to where the rival's name was written; not your Pokémon's level unless nothing else is
 * near. Then the learnt slot. 0: unknown. */
LOCAL u32 ns_level(u32 my_level) {
    if (S.mode != 1) return 0;
    u32 sp = NS.best;
    u64 bestd = ~0ull; u32 best = 0; u64 at = 0;
    for (int pass = 0; pass < 2 && !best; ++pass)
        for (u32 i = 0; i < NS.nlv && i < 128; ++i) {
            const NsWrite* w = &NS.lv[i];
            if (!pass) {                         /* your team's levels (the team menu writes them all), unless nothing else */
                int team = w->what == my_level;
                for (u32 q = 0; q < g.party_count && q < 6 && !team; ++q) team = g.mon[q].valid && g.mon[q].level == w->what;
                if (team) continue;
            }
            u32 v = w->what;
            if (!((u32)NS.lcur[v] > 0 || NS.llast[v] > 0)) continue;   /* gone from the screen */
            for (u32 r = 0; r < NS.nrn && r < 128; ++r) {
                if (!sp || NS.rn[r].what != sp) continue;
                u64 d = w->addr > NS.rn[r].addr ? w->addr - NS.rn[r].addr : NS.rn[r].addr - w->addr;
                if (d < bestd || (d == bestd && w->sweep > 0)) { bestd = d; best = v; at = w->addr; }
            }
        }
    if (best) { NS.final_lv = best; NS.final_lv_at = at; return best; }
    return NS.slot_lv;
}
LOCAL u32 ns_rival(void) {
    u32 best = ns_pick(0);
    if (best) NS.final = best;
    return best;
}
/* The rival's name: what the sweep saw written, else what the learnt slots say right now (instant
 * from the second battle on, and right even when it is the same species as last time). */
LOCAL u32 ns_rival_any(void) {
    u32 sp = ns_rival();
    if (!sp && S.mode == 1 && NS.slot_sp) { sp = NS.slot_sp; NS.final = sp; NS.best = sp; }
    return sp;
}

LOCAL const Mon* active_me(void);
LOCAL void learn_text(void) {
    Str s; s_init(&s, g_val, sizeof(g_val));
    s_cat(&s, "nombres: ");
    if (NS.best) { s_cat(&s, species_name(NS.best)); s_cat(&s, " x"); s_u(&s, NS.best_n, 0); s_cat(&s, " escrito "); s_u(&s, NS.lastw[NS.best], 0); }
    else s_ch(&s, '-');
    { u32 ms = ns_mine(); if (ms) { s_cat(&s, " | tuyo "); s_cat(&s, species_name(ms)); } }
    { const Mon* me = active_me(); u32 lv = ns_level(me ? me->level : 0); if (lv) { s_cat(&s, " | Nv rival "); s_u(&s, lv, 0); } }
    if (NS.nslot) { s_cat(&s, " | huecos "); s_u(&s, NS.nslot, 0); if (NS.slot_sp) { s_ch(&s, ' '); s_cat(&s, species_name(NS.slot_sp)); } }
    if (NS.lslot) { s_cat(&s, " nv-hueco"); if (NS.slot_lv) { s_ch(&s, ' '); s_u(&s, NS.slot_lv, 0); } }
    s_cat(&s, " | escrituras "); s_u(&s, NS.writes, 0); s_cat(&s, " total "); s_u(&s, NS.hits, 0);
    s_cat(&s, " sitios "); s_u(&s, NS.nam, 0); s_cat(&s, " v"); s_u(&s, NS.sweeps, 0);
    s_cat(&s, " | aprendiendo: ");
    if (L.state == 1) { s_cat(&s, "salto "); s_u(&s, (u64)L.level, 0); s_cat(&s, "/5 "); s_cat(&s, L.region ? "main" : "heap"); }
    else if (L.state == 2) s_cat(&s, "hecho");
    else s_cat(&s, L.runs ? "espera" : "-");
    s_cat(&s, " | caminos "); s_u(&s, L.nch, 0);
    u32 shown = 0;
    for (int pass = 0; pass < 2; ++pass)
    for (u32 i = 0; i < L.nch && shown < 3; ++i) {
        int battle_path = L.ch[i].checked && !L.ch[i].field && L.ch[i].obs >= 20;
        if ((pass == 0) != battle_path) continue;
        ++shown;
        s_cat(&s, " | ");
        s_cat(&s, L.ch[i].field ? "campo " : battle_path ? "COMBATE " : "? ");
        for (int k = 0; k < L.ch[i].n; ++k) { if (k) s_ch(&s, '>'); hexs(&s, L.ch[i].c[k]); }
        if (L.ch[i].hits) { s_cat(&s, " x"); s_u(&s, L.ch[i].hits, 0); }
        if (L.ch[i].confirmed) s_cat(&s, " OK");
    }
    pt("sv.learn.text", g_val);
}
/* "Otro...": any species by its first letter, for rivals the companion cannot see (trainers'
 * Pokémon on device were not among the field Pokémon). Only the species is known then: types,
 * weaknesses, and the switch suggestions still work. */
#define SP_PER_PAGE 16
LOCAL u32 g_sp_page;
LOCAL int letter_of(const char* n) {
    const u8* p = (const u8*)n;
    if (!p || !*p) return -1;
    u32 c = p[0];
    if (c == 0xC3 && p[1]) { /* À-ÿ: fold the accented capitals */
        u32 d = p[1];
        if (d >= 0x80 && d <= 0x85) c = 'A'; else if (d >= 0x88 && d <= 0x8B) c = 'E'; else if (d >= 0x8C && d <= 0x8F) c = 'I';
        else if (d == 0x91) c = 'N'; else if (d >= 0x92 && d <= 0x96) c = 'O'; else if (d >= 0x99 && d <= 0x9C) c = 'U'; else return -1;
    }
    if (c >= 'a' && c <= 'z') c -= 32;
    return c >= 'A' && c <= 'Z' ? (int)(c - 'A') : -1;
}
LOCAL u16 g_sp_cache[400]; LOCAL u32 g_sp_cache_n; LOCAL int g_sp_cache_letter = -1;
LOCAL u32 sp_list(int letter, u16* out, u32 cap) { /* national numbers starting with that letter, A-Z */
    if (letter != g_sp_cache_letter) {
        u32 n = 0;
        for (u32 nat = 1; nat <= 1025 && n < 400; ++nat)
            if (letter_of(species_name(nat)) == letter) g_sp_cache[n++] = (u16)nat;
        for (u32 i = 1; i < n; ++i) { /* insertion sort by name */
            u16 v = g_sp_cache[i]; u32 j = i;
            char a[32]; Str t; s_init(&t, a, sizeof(a)); s_cat(&t, species_name(v));
            while (j > 0 && strcmp_(species_name(g_sp_cache[j - 1]), a) > 0) { g_sp_cache[j] = g_sp_cache[j - 1]; --j; }
            g_sp_cache[j] = v;
        }
        g_sp_cache_n = n; g_sp_cache_letter = letter;
    }
    u32 n = g_sp_cache_n < cap ? g_sp_cache_n : cap;
    memcpy(out, g_sp_cache, n * sizeof(u16));
    return n;
}
LOCAL void publish_species_picker(int battle) {
    pi("sv.sp.open", battle && g_sp_open);
    pi("sv.sp.letters", battle && g_sp_open && g_sp_letter < 0);
    pi("sv.sp.listing", battle && g_sp_open && g_sp_letter >= 0);
    if (!battle || !g_sp_open || g_sp_letter < 0) return;
    static u16 lst[400];
    u32 n = sp_list(g_sp_letter, lst, 400);
    u32 pages = n ? (n + SP_PER_PAGE - 1) / SP_PER_PAGE : 1;
    if (g_sp_page >= pages) g_sp_page = pages - 1;
    pi("sv.sp.page", g_sp_page + 1); pi("sv.sp.pages", pages);
    pi("sv.sp.prev", g_sp_page > 0); pi("sv.sp.next", g_sp_page + 1 < pages);
    for (u32 i = 0; i < SP_PER_PAGE; ++i) {
        u32 at = g_sp_page * SP_PER_PAGE + i;
        pi(K("sv.sp.", i, ".valid"), at < n);
        if (at >= n) continue;
        u32 nat = lst[at];
        pt(K("sv.sp.", i, ".name"), species_name(nat));
        pi(K("sv.sp.", i, ".spr"), nat);
        int us = sprite_present(nat);
        pi(K("sv.sp.", i, ".has_spr"), us);
        pi(K("sv.sp.", i, ".icon"), (s64)nat * 100);
        pi(K("sv.sp.", i, ".game_icon"), !us);
    }
}
LOCAL int g_tera_on; LOCAL u32 g_tera_key; /* "Tera" pill on the rival's sheet */
LOCAL int g_sugg_open; /* switch-suggestion list unfolded (module action "sugg_toggle") */
/* What the battle page shows, held steady for the whole battle (v0.6.6). On device the sweep's
 * guess came and went (its PK9 unreadable for a moment, another candidate first), so the page
 * flipped between the rival and the "¿Contra quién luchas?" list several times a second. Now:
 * once a battle starts it stays until the field is back for a second; the rival shown stays until
 * it is gone for 3 s (or the learnt path / your own choice says otherwise); the list opens by
 * itself only after 3 s without any rival, and then stays until a rival turns up. */
LOCAL struct { int active, has, auto_pick; u64 addr; u32 ec; Mon m; u32 last_ref, miss, none, off; struct { u32 ec, hp, shown; } H[16]; u32 nh; int sponly; } B;
/* Your Pokémon in battle: the one named on screen last, else the first that can fight. */
LOCAL const Mon* active_me(void) {
    u32 sp = ns_mine();
    for (u32 i = 0; sp && i < g.party_count && i < 6; ++i)
        if (g.mon[i].valid && !g.mon[i].egg && g.mon[i].species == sp) return &g.mon[i];
    for (u32 i = 0; i < g.party_count && i < 6; ++i)
        if (g.mon[i].valid && !g.mon[i].egg && g.mon[i].hp > 0) return &g.mon[i];
    return 0;
}
LOCAL u32 rival_level(u32 sp) {
    (void)sp;
    const Mon* me = active_me();
    return ns_level(me ? me->level : 0);
}
LOCAL int cand_auto(const Cand* c) { return (!c->isnew || c->from_path) && c->m.exp > 0; }
LOCAL void battle_hold(void) {
    int raw = S.mode == 1 && (g_has_chosen || L.hit || (S.ready && S.nopp > 0) || g.ow_raw == 6);
    int tick = g.refreshes != B.last_ref;     /* count in refreshes (4 a second) */
    B.last_ref = g.refreshes;
    if (S.mode != 1) {                         /* the field for 1 s ends the battle page */
        if (B.active && tick && ++B.off >= 4) memset(&B, 0, sizeof(B));
        return;
    }
    B.off = 0;
    if (raw) B.active = 1;
    if (!B.active) return;
    /* Its name on screen decides (v0.7.3: before HP or anything else; a wild Pokémon passing
     * by during a trainer battle made the page flip between the two). Your own choice still wins. */
    if (!g_has_chosen) {
        u32 sp = ns_rival_any();
        if (sp) {
            int got = -1;
            for (u32 i = 0; i < S.nopp; ++i)       /* the same one as before, else any of that species */
                if (S.opp[i].m.species == sp && S.opp[i].m.exp > 0 && (got < 0 || S.opp[i].ec == B.ec)) got = (int)i;
            if (got >= 0) {
                B.addr = S.opp[got].addr; B.ec = S.opp[got].ec; B.m = S.opp[got].m; B.sponly = 0;
            } else {
                if (!(B.has && B.sponly && B.m.species == sp)) {
                    memset(&B.m, 0, sizeof(B.m)); B.m.valid = 1; B.m.species = (u16)sp; B.addr = 0; B.ec = 0;
                    g_sp_level = 0; g_sp_level_set = 0;
                }
                B.sponly = 1;
                if (!g_sp_level_set) { u32 lv = rival_level(sp); if (lv) g_sp_level = lv; }
            }
            B.has = 1; B.miss = 0;
            return;
        }
    }
    /* HP that changes only happens in battle: the Pokémon whose HP just changed is the one being
     * fought, new in memory or not, and even over a choice made by hand (a trainer's next
     * Pokémon; the old one stays readable). A fainted one gives way to one still standing. */
    {
        #define H B.H
        #define nh B.nh
        int moved = -1;
        for (u32 i = 0; i < S.nopp; ++i) {
            u32 k = 0;
            while (k < nh && H[k].ec != S.opp[i].ec) ++k;
            if (k == nh) { if (nh < 16) { H[nh].ec = S.opp[i].ec; H[nh].hp = S.opp[i].m.hp; H[nh].shown = 0; nh++; } continue; }
            if (H[k].hp != S.opp[i].m.hp) { H[k].hp = S.opp[i].m.hp; if (S.opp[i].m.hp > 0 && moved < 0) moved = (int)i; }
        }
        u32 shown_ec = g_has_chosen ? g_chosen.ec : B.ec;
        for (u32 k = 0; k < nh; ++k) if ((g_has_chosen || B.has) && H[k].ec == shown_ec) H[k].shown = 1;
        if (moved >= 0 && (!(g_has_chosen || B.has) || S.opp[moved].ec != shown_ec)) {
            g_has_chosen = 0; B.sponly = 0;
            B.has = 1; B.addr = S.opp[moved].addr; B.ec = S.opp[moved].ec; B.m = S.opp[moved].m; B.miss = 0; return;
        }
        if (!g_has_chosen && B.has && B.m.hp == 0)
            for (u32 i = 0; i < S.nopp; ++i) {
                int was = 0;   /* back to one already shown in this battle */
                for (u32 k = 0; k < nh; ++k) if (H[k].ec == S.opp[i].ec && H[k].shown) was = 1;
                if (S.opp[i].ec != B.ec && S.opp[i].m.hp > 0 && (was || cand_auto(&S.opp[i]))) {
                    B.addr = S.opp[i].addr; B.ec = S.opp[i].ec; B.m = S.opp[i].m; B.miss = 0; return;
                }
            }
        #undef H
        #undef nh
    }
    if (L.hit && S.nopp) {                     /* the learnt path, when no name says otherwise */
        B.has = 1; B.addr = S.opp[0].addr; B.ec = S.opp[0].ec; B.m = S.opp[0].m; B.miss = 0; B.sponly = 0;
        return;
    }
    int at = -1;
    for (u32 i = 0; B.has && i < S.nopp; ++i) if (S.opp[i].ec == B.ec) { at = (int)i; break; }
    if (at >= 0) { B.m = S.opp[at].m; B.addr = S.opp[at].addr; B.miss = 0; }
    else if (B.has) {
        if (tick && ++B.miss >= 12)
            for (u32 i = 0; i < S.nopp; ++i)
                if (cand_auto(&S.opp[i])) { B.addr = S.opp[i].addr; B.ec = S.opp[i].ec; B.m = S.opp[i].m; B.miss = 0; break; }
    } else {
        /* Only a field Pokémon whose bytes changed counts by itself. A PK9 that is merely new is
         * usually something that spawned nearby during the battle (device: Rufflet shown while
         * fighting Drifloon, "cambiados 0"): it goes first in the list instead. */
        for (u32 i = 0; i < S.nopp; ++i)
            if (cand_auto(&S.opp[i])) { B.has = 1; B.addr = S.opp[i].addr; B.ec = S.opp[i].ec; B.m = S.opp[i].m; B.miss = 0; break; }
    }
    if (!B.has && !g_has_chosen && tick && ++B.none >= 8) B.auto_pick = 1;
}
LOCAL void publish_battle(void) {
    battle_hold();
    int battle = B.active;
    /* the list: asked for ("Cambiar"), or no rival at all for 3 s */
    int pick_open = battle && ((B.auto_pick && !B.has && !g_has_chosen) || g_pick_open_req);
    if (g_sp_open) pick_open = 0;         /* the species browser replaces the list */
    pi("sv.pick.open", pick_open);
    publish_species_picker(battle);
    pi("sv.pick.n", g_npick);
    for (u32 i = 0; i < PICK_MAX; ++i) {
        char pfx[16]; Str ps; s_init(&ps, pfx, sizeof(pfx)); s_cat(&ps, "sv.pick."); s_u(&ps, i, 0); s_ch(&ps, '.');
        #define PK(f) (s_init(&ps, g_key, sizeof(g_key)), s_cat(&ps, pfx), s_cat(&ps, f), g_key)
        pi(PK("valid"), pick_open && i < g_npick);
        if (!pick_open || i >= g_npick) continue;
        Mon mm = g_pick[i].m;
        if (!mm.stats_ok || mm.hpmax > 714 || mm.level > 100) compute_stats(&mm);
        pt(PK("name"), species_name(mm.species));
        pi(PK("level"), mm.level);
        pi(PK("spr"), mm.species);
        int us = sprite_present(mm.species);
        pi(PK("has_spr"), us);
        pi(PK("icon"), (s64)mm.species * 100 + mm.form);
        pi(PK("game_icon"), !us);
        pi(PK("guess"), i < S.nopp);
        #undef PK
    }
    learn_text();
    pi("sv.battle", battle);
    pi("sv.overworld", g.overworld);
    pi("sv.ow_raw", g.ow_raw);
    pi("sv.live_n", g.live_n);
    pi("sv.scan.ready", S.ready);
    pi("sv.scan.base", S.nbase);
    pi("sv.scan.opp", S.nopp);
    pi("sv.scan.sweeps", (s64)S.sweeps);
    pi("sv.scan.hits", (s64)S.hits);
    pi("sv.scan.valid", (s64)S.valid_pk);
    pi("sv.scan.pct", S.hi > S.lo ? (s64)((S.cur - S.lo) * 100 / (S.hi - S.lo)) : 0);
    {
        Str s; s_init(&s, g_val, sizeof(g_val));
        s_cat(&s, "v0.10.0 mapa "); if (g.ow_ok) s_u(&s, g.ow_raw, 0); else s_cat(&s, "?");
        s_cat(&s, " (vistos");
        for (u32 i = 0; i < S.now_seen; ++i) { s_ch(&s, ' '); s_u(&s, S.ow_seen[i], 0); }
        s_cat(&s, ") | "); s_cat(&s, S.mode ? "COMBATE " : "campo "); s_u(&s, (u64)(S.hi > S.lo ? (S.cur - S.lo) * 100 / (S.hi - S.lo) : 0), 0);
        s_cat(&s, "% v"); s_u(&s, S.sweeps, 0);
        s_cat(&s, " | base "); s_u(&s, S.nbase, 0); s_cat(&s, S.ready ? "" : " (preparando)");
        s_cat(&s, " | pk9 "); s_u(&s, S.valid_pk, 0);
        s_cat(&s, " | rival ");
        if (S.nopp) { s_cat(&s, mon_name(&S.opp[0].m)); s_cat(&s, " x"); s_u(&s, S.nopp, 0); }
        else s_cat(&s, "-");
        if (S.mode) { s_cat(&s, " | cambiados "); s_u(&s, S.changed, 0); }
        if (S.mode) { s_cat(&s, " | ya estaban "); s_u(&s, S.others, 0); if (S.others) { s_ch(&s, ' '); s_cat(&s, S.other_names); } }
        s_cat(&s, " | vivo "); s_u(&s, g.live_n, 0); if (g.live_bad) { s_ch(&s, '!'); s_u(&s, g.live_bad, 0); }
        if (S.recentred > 1) { s_cat(&s, " | recentrado "); s_u(&s, S.recentred, 0); }
        s_cat(&s, " | img "); s_u(&s, g_img_ok, 0); s_ch(&s, '/'); s_u(&s, g_img_fail, 0);
        s_cat(&s, " | test ");
        if (!g.live_addr0) s_cat(&s, "-");
        else {
            s_cat(&s, S.t_in ? "D" : "F"); s_cat(&s, S.t_ptr ? "P" : "p"); s_cat(&s, S.t_hdr ? "H" : "h");
            s_cat(&s, S.t_spc ? "E" : "e"); s_cat(&s, S.t_pk ? "K" : "k"); s_u(&s, S.t_al, 0);
            s_ch(&s, ' '); s_u(&s, (g.live_addr0 >> 20), 0); s_cat(&s, "MB/"); s_u(&s, S.lo >> 20, 0);
        }
        pt("sv.scan.text", g_val);
    }
    if (!battle || (!g_has_chosen && !B.has)) { pi("sv.opp.valid", 0); pi("sv.me.valid", 0); return; }

    static Mon oc;
    oc = g_has_chosen ? g_chosen.m : B.m;
    pi("sv.opp.sure", g_has_chosen || L.hit);
    int species_only = g_has_chosen ? !g_chosen.addr : B.sponly;
    pi("sv.opp.full", !species_only);   /* level, HP, ability, item known */
    pi("sv.me.full", 1);
    if (!species_only && (!oc.stats_ok || oc.hpmax > 714 || oc.level > 100)) compute_stats(&oc);
    if (species_only) {   /* its level (beside its name, or yours to set), IVs 15, no EVs, neutral nature;
                           * unknown level: no guess shown (v0.6.8 put your own level, read as a bug) */
        const GPers* pe = base_of(&oc);
        if (pe) {
            oc.exp = exp_for(pe->growth, g_sp_level ? g_sp_level : 50);
            for (u32 k = 0; k < 6; ++k) { oc.iv[k] = 15; oc.ev[k] = 0; }
            oc.nature = 0; oc.hp = 0xFFFF;
            compute_stats(&oc);
        }
    }
    pi("sv.opp.splvl", species_only ? g_sp_level : 0);
    pi("sv.opp.splvl_unk", species_only && !g_sp_level);
    const Mon* o = &oc;
    {   /* a new rival starts without Tera */
        u32 key = (o->ec ^ (o->species << 16) ^ o->form) | 1u;
        if (key != g_tera_key) { g_tera_key = key; g_tera_on = 0; }
    }
    int tera_can = !species_only && o->tera < T_COUNT;
    int tera_use = tera_can && g_tera_on;
    pi("sv.opp.tera_can", tera_can);
    pi("sv.opp.tera_on", tera_use);
    g_def_tera = tera_use ? o->tera + 1 : 0;
    publish_detail_pfx("sv.opp.", o);
    g_def_tera = 0;
    publish_hp("sv.opp.", o);
    if (species_only) pt("sv.opp.status", "");   /* its HP are unknown: never "Debilitado" */
    pi("sv.opp.n", S.nopp);
    {   /* how this rival was found, on the line under the sheet */
        const char* how = g_has_chosen ? (g_chosen.addr ? TR("Rival elegido por ti", "Opponent chosen by you")
                                                        : TR("Rival elegido por ti (por nombre)", "Opponent chosen by you (by name)"))
                        : B.sponly ? TR("Rival detectado por su nombre en pantalla", "Opponent found by its name on screen")
                        : (ns_rival_any() && ns_rival_any() == o->species)
                              ? TR("Rival detectado por su nombre (datos completos)", "Opponent found by its name (full data)")
                        : L.hit ? TR("Rival detectado por el camino aprendido", "Opponent found by the learned path")
                        : TR("Rival supuesto (toca Cambiar si no es)", "Opponent guessed (tap Change if wrong)");
        pt("sv.opp.how", how);
    }
    pi("sv.opp.lvl_ok", o->stats_ok && !(species_only && !g_sp_level));

    /* Our Pokémon in battle: named on screen last, else the first that can fight. */
    const Mon* me = active_me();
    if (!me) { pi("sv.me.valid", 0); return; }
    publish_detail_pfx("sv.me.", me);
    publish_hp("sv.me.", me);
    {   /* rival's base stats against ours: arrow up = the rival has more */
        const GPers* pb = base_of(o); const GPers* mb = base_of(me);
        for (u32 k = 0; k < 6; ++k) {
            int up = pb && mb && pb->base[k] > mb->base[k], dn = pb && mb && pb->base[k] < mb->base[k];
            pi(K("sv.opp.bs", k, ".up"), up); pi(K("sv.opp.bs", k, ".dn"), dn);
        }
        Str s; s_init(&s, g_val, sizeof(g_val));
        if (pb && mb) { s_cat(&s, TR("Flechas: frente a tu ", "Arrows: against your ")); s_cat(&s, mon_name(me)); }
        pt("sv.opp.cmp", g_val);
    }

    /* Speed, from the two stats alone (no boosts, items, abilities or priority). */
    const char* spd = TR("Velocidad desconocida", "Speed unknown");
    if (species_only && !g_sp_level) spd = TR("Pon su nivel para comparar la velocidad", "Set its level to compare Speed");
    else if (species_only && o->stats_ok && me->stats_ok) {
        /* its IVs and nature are unknown: compare with the whole range at that level */
        const GPers* pe = base_of(o);
        u32 lv = o->level, b = pe ? pe->base[5] : 0;
        u32 lo = ((2 * b) * lv / 100 + 5) * 90 / 100, hi = ((2 * b + 31) * lv / 100 + 5) * 110 / 100;
        u32 a = me->stat[5];
        spd = a > hi ? TR("Actúas primero (sin prioridad)", "You move first (no priority)")
            : a < lo ? TR("El rival actúa primero", "The opponent moves first")
                     : TR("Velocidad pareja: depende del rival", "Close Speed: depends on the opponent");
    } else if (o->stats_ok && me->stats_ok) {
        u32 a = me->stat[5], b = o->stat[5];
        spd = a > b ? TR("Seguramente actúas primero", "You most likely move first")
            : a < b ? TR("Seguramente actúa primero el rival", "The opponent most likely moves first")
                    : TR("Empate de velocidad", "Speed tie");
    }
    pt("sv.opp.speed", spd);

    /* Our moves against the rival's types. */
    u32 t1 = 0, t2 = 0;
    int typed = species_types(o->species, o->form, &t1, &t2);
    u32 at1 = t1, at2 = t2;                 /* its attacks keep the species types (STAB) */
    if (typed && tera_use) t1 = t2 = o->tera; /* Terastallized: defends with the Tera type */
    for (u32 j = 0; j < 4; ++j) {
        MoveInfo mi = move_info(me->moves[j]);
        const char* txt = "";
        u32 cls = 0;
        if (me->moves[j] && mi.ok && typed && mi.cat != 0) {
            u32 mul = eff(mi.type, t1) * (t2 != t1 ? eff(mi.type, t2) : 2);
            if (mul == 0) { txt = TR("No afecta", "No effect"); cls = 1; }
            else if (mul <= 2) { txt = mul == 1 ? TR("x0,25 Poco eficaz", "x0.25 Resisted")
                                                : TR("x0,5 Poco eficaz", "x0.5 Resisted"); cls = 2; }
            else if (mul == 4) { txt = "x1"; cls = 3; }
            else { txt = mul == 16 ? TR("x4 Súper eficaz", "x4 Super effective") : TR("x2 Súper eficaz", "x2 Super effective"); cls = 4; }
        }
        pt(K("sv.me.m", j, ".eff"), txt);
        pi(K("sv.me.m", j, ".effc"), cls);
        pi(K("sv.me.m", j, ".eff_lo"), cls == 1 || cls == 2);
        pi(K("sv.me.m", j, ".eff_hi"), cls == 4);
    }

    /* Team against the rival ("Cambios"): every other team member, best first, each with two
     * lines: attack (its own types as STAB, else its best move) and defence (the rival's types
     * as attacks against it), plus who is faster. */
    pi("sv.sugg.open", g_sugg_open);
    typedef struct { const Mon* m; s32 score; u32 type_mul, type_t, move_mul, move; u32 worst_mul, worst_t, best_def_mul, best_def_t; int fast; } Sg;
    Sg sg[6]; u32 nsg = 0;
    static const s32 rank[17] = {-3, -2, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2}; /* by mul in quarters */
    for (u32 i = 0; typed && i < g.party_count && i < 6; ++i) {
        const Mon* c = &g.mon[i];
        if (!c->valid || c->egg || c->hp == 0 || c == me) continue;
        u32 c1, c2;
        if (!species_types(c->species, c->form, &c1, &c2)) continue;
        Sg x; memset(&x, 0, sizeof(x)); x.m = c; x.best_def_mul = 99;
        u32 own[2] = {c1, c2}, rt[2] = {at1, at2};
        for (u32 k = 0; k < (c2 != c1 ? 2u : 1u); ++k) {      /* attack with its own types */
            u32 mul = eff(own[k], t1) * (t2 != t1 ? eff(own[k], t2) : 2);
            if (mul > x.type_mul) { x.type_mul = mul; x.type_t = own[k]; }
        }
        for (u32 k = 0; k < (at2 != at1 ? 2u : 1u); ++k) {    /* the rival's types against it */
            u32 mul = eff(rt[k], c1) * (c2 != c1 ? eff(rt[k], c2) : 2);
            if (mul >= x.worst_mul) { x.worst_mul = mul; x.worst_t = rt[k]; }
            if (mul < x.best_def_mul) { x.best_def_mul = mul; x.best_def_t = rt[k]; }
        }
        for (u32 j = 0; j < 4; ++j) {
            MoveInfo mi = move_info(c->moves[j]);
            if (!c->moves[j] || !mi.ok || mi.cat == 0 || !mi.power) continue;
            u32 mul = eff(mi.type, t1) * (t2 != t1 ? eff(mi.type, t2) : 2);
            if (mul > x.move_mul) { x.move_mul = mul; x.move = c->moves[j]; }
        }
        x.fast = o->stats_ok && c->stats_ok ? (c->stat[5] > o->stat[5] ? 1 : c->stat[5] < o->stat[5] ? -1 : 0) : 0;
        u32 atk = x.type_mul > x.move_mul ? x.type_mul : x.move_mul;
        x.score = 3 * rank[atk > 16 ? 16 : atk] - 3 * rank[x.worst_mul > 16 ? 16 : x.worst_mul] + x.fast
                  + (s32)(c->hp * 2 / (c->hpmax ? c->hpmax : 1));
        sg[nsg++] = x;
    }
    for (u32 a2 = 0; a2 < nsg; ++a2) /* best first */
        for (u32 b2 = a2 + 1; b2 < nsg; ++b2)
            if (sg[b2].score > sg[a2].score) { Sg tmp = sg[a2]; sg[a2] = sg[b2]; sg[b2] = tmp; }
    pi("sv.sugg.n", nsg);
    /* As Luminescent Platinum's "Sugerencias de cambio": the best three, each with short lines
     * (bad ones in red): resists the rival's type, is super effective by its own type (or a
     * move is), is faster, is weak to the rival's type. */
    for (u32 k = 0; k < 3; ++k) {
        char pfx[16]; Str ps; s_init(&ps, pfx, sizeof(pfx)); s_cat(&ps, "sv.sg"); s_u(&ps, k, 0); s_ch(&ps, '.');
        #define SK(f) (s_init(&ps, g_key, sizeof(g_key)), s_cat(&ps, pfx), s_cat(&ps, f), g_key)
        pi(SK("valid"), k < nsg);
        if (k >= nsg) continue;
        const Sg* x = &sg[k];
        const Mon* c = x->m;
        pt(SK("name"), mon_name(c));
        pi(SK("spr"), c->species);
        pi(SK("has_spr"), sprite_present(c->species));
        pi(SK("male"), c->gender == 0);
        pi(SK("female"), c->gender == 1);
        publish_hp(pfx, c);
        pi(SK("best"), k == 0 && x->score > 0);
        char lp[20]; Str lps; s_init(&lps, lp, sizeof(lp)); s_cat(&lps, pfx); s_ch(&lps, 'l');
        char line[64]; Str ls;
        u32 nl = 0;
        #define LINE(bad_) do { pt(K(lp, nl, ""), line); pi(K(lp, nl, ".bad"), bad_); pi(K(lp, nl, ".ok"), !(bad_)); ++nl; } while (0)
        if (x->best_def_mul <= 2) {
            s_init(&ls, line, sizeof(line)); s_cat(&ls, x->best_def_mul == 0 ? TR("Inmune a ", "Immune to ") : TR("Resistente a ", "Resists "));
            s_cat(&ls, name_or_num(&g_types, x->best_def_t, TR("Tipo ", "Type "))); LINE(0);
        }
        if (x->type_mul >= 8) {
            s_init(&ls, line, sizeof(line)); s_cat(&ls, TR("Eficaz por tipo (", "Type advantage (")); s_cat(&ls, name_or_num(&g_types, x->type_t, TR("Tipo ", "Type ")));
            s_cat(&ls, x->type_mul >= 16 ? ") x4" : ")"); LINE(0);
        } else if (x->move_mul >= 8) {
            s_init(&ls, line, sizeof(line)); s_cat(&ls, name_or_num(&g_moves, x->move, TR("Mov. ", "Move "))); s_cat(&ls, TR(" supereficaz", " super effective")); LINE(0);
        } else if (x->type_mul <= 2) {
            s_init(&ls, line, sizeof(line)); s_cat(&ls, TR("Su tipo es poco eficaz", "Its type is not very effective")); LINE(1);
        }
        if (x->fast > 0) { s_init(&ls, line, sizeof(line)); s_cat(&ls, TR("Más rápido", "Faster")); LINE(0); }
        if (x->worst_mul >= 8 && nl < 4) {
            s_init(&ls, line, sizeof(line)); s_cat(&ls, TR("Débil frente a ", "Weak to ")); s_cat(&ls, name_or_num(&g_types, x->worst_t, TR("Tipo ", "Type "))); LINE(1);
        }
        if (nl == 0) { s_init(&ls, line, sizeof(line)); s_cat(&ls, TR("Sin ventaja clara", "No clear advantage")); LINE(0); }
        for (; nl < 4; ++nl) { pt(K(lp, nl, ""), ""); pi(K(lp, nl, ".bad"), 0); pi(K(lp, nl, ".ok"), 0); }
        #undef LINE
        #undef SK
    }
}

/* ---------- the game's own pictures (BNTX inside the Trinity packs) ----------
 * load_image("module:item/<id>") / ("module:poke/<national>") pulls the texture out of the
 * player's romfs (sv_game_read, Oodle included), un-tiles Tegra's block-linear layout and
 * decodes it to RGBA: raw formats, BC1-5/BC7 (vendored bc_decoder, MPL-2.0) and ASTC (the
 * host's decoder). Same layout rules as Eden's own BNTX reader (mod_nx_assets.cpp). Paths from
 * PokeDocs' SV hash list: appli/tex/icon_item/item_NNNN/item_NNNN.bntx and
 * pokemon/data/pmXXXX/pmXXXX_FF_00/icon/pmXXXX_FF_00_00.bntx (internal species number). */
extern void sv_bc_decode(unsigned type, const unsigned char* src, unsigned char* dst, size_t x, size_t y,
                         size_t w, size_t h);
LOCAL const EdenDsmodHostExtensions* g_hostx;
LOCAL char g_game_lock;   /* sv_game_read and its decompressor are single-threaded */
LOCAL void game_lock(void) { while (__atomic_test_and_set(&g_game_lock, __ATOMIC_ACQUIRE)) { } }
LOCAL int game_trylock(void) { return !__atomic_test_and_set(&g_game_lock, __ATOMIC_ACQUIRE); }
LOCAL void game_unlock(void) { __atomic_clear(&g_game_lock, __ATOMIC_RELEASE); }

#define TEX_MAX_SIDE 2048
#define TEX_OUT_SIDE 1024   /* pictures handed to the host are halved down to this */
LOCAL u8 g_tex_file[40u << 20];
LOCAL u8 g_tex_lin[16u << 20];
LOCAL u8 g_tex_rgba[TEX_MAX_SIDE * TEX_MAX_SIDE * 4];
LOCAL u8 g_tex_narrow[TEX_MAX_SIDE * TEX_MAX_SIDE * 2];

LOCAL u64 bl_offset(u32 x, u32 y, u32 bpp, u32 bh, u64 gobs_per_row) {
    u64 gob = (u64)(y / (8 * bh)) * 512 * bh * gobs_per_row + (u64)x * bpp / 64 * 512 * bh + (u64)(y % (8 * bh) / 8) * 512;
    u64 xb = (u64)x * bpp;
    return gob + ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 + (y % 2) * 16 + (xb % 16);
}
/* format >> 8 -> kind (1 raw, 2 BCn, 3 ASTC), block size and bytes per texel/block */
LOCAL int tex_format(u32 type, u32* kind, u32* bw, u32* bh, u32* bytes) {
    static const u8 astc[][3] = {{0x2D,4,4},{0x2E,5,4},{0x2F,5,5},{0x30,6,5},{0x31,6,6},{0x32,8,5},{0x33,8,6},
                                 {0x34,8,8},{0x35,10,5},{0x36,10,6},{0x37,10,8},{0x38,10,10},{0x39,12,10},{0x3A,12,12}};
    *bw = *bh = 1;
    switch (type) {
    case 0x02: *kind = 1; *bytes = 1; return 1;
    case 0x07: case 0x09: case 0x0F: *kind = 1; *bytes = 2; return 1;
    case 0x0B: case 0x0C: *kind = 1; *bytes = 4; return 1;
    case 0x1A: case 0x1D: *kind = 2; *bw = *bh = 4; *bytes = 8; return 1;
    case 0x1B: case 0x1C: case 0x1E: case 0x20: *kind = 2; *bw = *bh = 4; *bytes = 16; return 1;
    }
    for (u32 i = 0; i < sizeof(astc) / sizeof(astc[0]); ++i)
        if (astc[i][0] == type) { *kind = 3; *bw = astc[i][1]; *bh = astc[i][2]; *bytes = 16; return 1; }
    return 0;
}
/* Texture 0, mip 0, layer 0 of a BNTX file -> g_tex_rgba. */
LOCAL int bntx_decode(const u8* f, u32 n, u32* ow, u32* oh) {
    if (n < 0x40 || memcmp(f, "BNTX", 4) != 0 || memcmp(f + 0x20, "NX  ", 4) != 0) return 0;
    u64 table = le64(f + 0x28);
    if (le32(f + 0x24) == 0 || table + 8 > n) return 0;
    u64 info = le64(f + table);
    if (info + 0x78 > n || memcmp(f + info, "BRTI", 4) != 0) return 0;
    const u8* t = f + info;
    u32 format = le32(t + 0x1C), w = le32(t + 0x24), h = le32(t + 0x28);
    u32 bhl = le32(t + 0x34) & 7;
    u32 nmips = le16(t + 0x16);
    u64 mips = le64(t + 0x70);
    if (w == 0 || h == 0 || nmips == 0 || nmips > 16 || mips + 8ull * nmips > n) return 0;
    u32 type = format >> 8, kind, bw, bhk, bpp;
    if (!tex_format(type, &kind, &bw, &bhk, &bpp)) return 0;
    /* a picture larger than we decode (a whole-region map): use a smaller mip level */
    u32 mip = 0;
    while ((w > TEX_MAX_SIDE || h > TEX_MAX_SIDE) && mip + 1 < nmips) { ++mip; w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; }
    if (w > TEX_MAX_SIDE || h > TEX_MAX_SIDE) return 0;
    u64 data = le64(f + mips + 8ull * mip);
    u32 gw = (w + bw - 1) / bw, gh = (h + bhk - 1) / bhk;
    u32 blockh = 1u << bhl;
    /* Tegra shrinks the block height of small mips: halve it while the mip fits in half */
    if (mip) while (blockh > 1 && gh <= blockh * 8 / 2) blockh /= 2;
    u64 gobs = ((u64)gw * bpp + 63) / 64;
    if ((u64)gw * gh * bpp > sizeof(g_tex_lin) || data >= n) return 0;
    const u8* tiled = f + data;
    u64 avail = n - data;
    for (u32 y = 0; y < gh; ++y)
        for (u32 x = 0; x < gw; ++x) {
            u64 at = bl_offset(x, y, bpp, blockh, gobs);
            u8* dst = g_tex_lin + ((u64)y * gw + x) * bpp;
            if (at + bpp <= avail) memcpy(dst, tiled + at, bpp); else memset(dst, 0, bpp);
        }
    u64 px = (u64)w * h;
    memset(g_tex_rgba, 0, px * 4);
    if (kind == 3) {
        if (!g_hostx || !g_hostx->decode_astc ||
            !g_hostx->decode_astc(g_hostx->userdata, w, h, bw, bhk, g_tex_lin, (size_t)gw * gh * 16, g_tex_rgba, px * 4))
            return 0;
    } else if (kind == 2) {
        u32 out_bpp = type == 0x1D ? 1 : type == 0x1E ? 2 : 4;
        u8* target = out_bpp == 4 ? g_tex_rgba : g_tex_narrow;
        if (out_bpp != 4) memset(g_tex_narrow, 0, px * out_bpp);
        for (u32 by = 0; by < gh; ++by)
            for (u32 bx = 0; bx < gw; ++bx)
                sv_bc_decode(type, g_tex_lin + ((u64)by * gw + bx) * bpp, target + ((u64)by * 4 * w + bx * 4) * out_bpp,
                             bx * 4, by * 4, w, h);
        if (out_bpp != 4)
            for (u64 i = 0; i < px; ++i) {
                g_tex_rgba[i * 4] = g_tex_narrow[i * out_bpp];
                g_tex_rgba[i * 4 + 1] = out_bpp == 2 ? g_tex_narrow[i * 2 + 1] : 0;
                g_tex_rgba[i * 4 + 3] = 255;
            }
    } else {
        for (u64 i = 0; i < px; ++i) {
            const u8* s = g_tex_lin + i * bpp;
            u8* d = g_tex_rgba + i * 4;
            switch (type) {
            case 0x02: d[0] = s[0]; d[3] = 255; break;
            case 0x09: d[0] = s[0]; d[1] = s[1]; d[3] = 255; break;
            case 0x0B: memcpy(d, s, 4); break;
            case 0x0C: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; break;
            default: {
                u32 v = le16(s), lo = v & 31, mid = (v >> 5) & 63, hi = (v >> 11) & 31;
                u32 r = type == 0x07 ? lo : hi, b = type == 0x07 ? hi : lo;
                d[0] = (u8)(r * 255 / 31); d[1] = (u8)(mid * 255 / 63); d[2] = (u8)(b * 255 / 31); d[3] = 255;
            }
            }
        }
    }
    /* per-texture channel map (0 = zero, 1 = one, 2..5 = R,G,B,A) */
    const u8* ch = t + 0x58;
    if (!(ch[0] == 2 && ch[1] == 3 && ch[2] == 4 && ch[3] == 5))
        for (u64 i = 0; i < px; ++i) {
            u8* p = g_tex_rgba + i * 4;
            u8 src[4] = {p[0], p[1], p[2], p[3]};
            for (u32 c = 0; c < 4; ++c) {
                u8 sel = ch[c];
                p[c] = sel == 0 ? 0 : sel == 1 ? 255 : sel <= 5 ? src[sel - 2] : src[c];
            }
        }
    *ow = w; *oh = h;
    return 1;
}
LOCAL void s_u4(Str* s, u32 v) { s_u(s, v, 4); }
LOCAL u32 parse_uint(const char* p) { u32 v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + (u32)(*p++ - '0'); return v; }
LOCAL int starts(const char* s, const char* pre) { while (*pre) if (*s++ != *pre++) return 0; return 1; }
/* sin/cos in whole degrees (Bhaskara I, error < 0.2 %): no libm in the module. */
LOCAL float sin_deg(s32 d) {
    d %= 360; if (d < 0) d += 360;
    int neg = d >= 180; if (neg) d -= 180;
    float x = (float)d, v = 4.0f * x * (180.0f - x) / (40500.0f - x * (180.0f - x));
    return neg ? -v : v;
}
LOCAL float cos_deg(s32 d) { return sin_deg(d + 90); }
/* The region map. In the game files it is a mosaic (PokeDocs SV hash list): Paldea is 10 x 10
 * tiles ymap_ter_small_RR_CC (00..09), Kitakami and Blueberry 4 x 4 (ymap_ter_dlc1/2_small_RR_CC),
 * with a ring of sea tiles (RRR_CCC) around them; ymap_ter_1x1 is not the whole map (v0.6.5 showed
 * only sea). The tiles are put together once per region (2048 px square) and cropped for the zoom.
 * Which index is the row is not documented: "swap" (map_flip action) puts them the other way. */
LOCAL u8 g_map_px[TEX_MAX_SIDE * TEX_MAX_SIDE * 4];
LOCAL u32 g_map_w, g_map_h; LOCAL s32 g_map_f = -1;
LOCAL u32 g_map_tiles_ok, g_map_tiles, g_map_tile_px;
LOCAL void img_fail_note(const char* key, u32 n, const char* why);
LOCAL void s_u2(Str* s, u32 v) { s_ch(s, (char)('0' + v / 10 % 10)); s_ch(s, (char)('0' + v % 10)); }
LOCAL EdenDsmodBool map_view(const EdenDsmodHostApi* host, u32 f, u32 zl, u32 tx, u32 ty, u32 swap, u32 rot, u32 hb,
                             void* receiver, EdenDsmodImageSink sink) {
    static const char* const dirs[3] = {"ymap_ter_small", "ymap_ter_dlc1_small", "ymap_ter_dlc2_small"};
    static const char* const stems[3] = {"ymap_ter_small_", "ymap_ter_dlc1_small_", "ymap_ter_dlc2_small_"};
    game_lock();
    s32 want = (s32)(f | (swap << 2));
    (void)0;
    if (g_map_f != want) {
        u32 n = f == 0 ? 10 : 4, tp = TEX_MAX_SIDE / n, side = tp * n;
        const char* why = "";
        u32 ok_n = 0, last_n = 0;
        for (u32 i = 0; i < side * side; ++i) { /* sea blue under missing tiles */
            u8* o = g_map_px + (u64)i * 4; o[0] = 0x3C; o[1] = 0x78; o[2] = 0xC8; o[3] = 255;
        }
        for (u32 a = 0; a < n; ++a)
            for (u32 b = 0; b < n; ++b) {
                char path[160]; Str s; s_init(&s, path, sizeof(path));
                s_cat(&s, "appli/tex/"); s_cat(&s, dirs[f]); s_ch(&s, '/'); s_cat(&s, stems[f]); s_u2(&s, a); s_ch(&s, '_'); s_u2(&s, b);
                s_ch(&s, '/'); s_cat(&s, stems[f]); s_u2(&s, a); s_ch(&s, '_'); s_u2(&s, b); s_cat(&s, ".bntx");
                u32 w = 0, hgt = 0;
                u32 rn = sv_game_read(host, path, g_tex_file, sizeof(g_tex_file), &why);
                last_n = rn;
                if (!rn || !bntx_decode(g_tex_file, rn, &w, &hgt) || !w || !hgt) continue;
                ok_n++; g_map_tile_px = w;
                /* orientation (v0.6.8, from the device photos): the first index is the column
                 * (west to east) and the second the row counted from the south. "swap" is now
                 * a 3-bit mode: 1 = first index is the column, 2 = rows from the bottom,
                 * 4 = columns from the right; default 3, "¿Mapa revuelto?" steps through all 8. */
                u32 row = (swap & 1) ? b : a, col = (swap & 1) ? a : b;
                if (swap & 2) row = n - 1 - row;
                if (swap & 4) col = n - 1 - col;
                for (u32 y = 0; y < tp; ++y)        /* scale the tile into its square (box average) */
                    for (u32 x = 0; x < tp; ++x) {
                        u32 sx0 = x * w / tp, sx1 = (x + 1) * w / tp, sy0 = y * hgt / tp, sy1 = (y + 1) * hgt / tp;
                        if (sx1 <= sx0) sx1 = sx0 + 1; if (sy1 <= sy0) sy1 = sy0 + 1;
                        u32 acc[4] = {0, 0, 0, 0}, cnt = 0;
                        for (u32 yy = sy0; yy < sy1 && yy < hgt; ++yy)
                            for (u32 xx = sx0; xx < sx1 && xx < w; ++xx) {
                                const u8* q = g_tex_rgba + ((u64)yy * w + xx) * 4;
                                acc[0] += q[0]; acc[1] += q[1]; acc[2] += q[2]; acc[3] += q[3]; cnt++;
                            }
                        if (!cnt) continue;
                        u8* o = g_map_px + ((u64)(row * tp + y) * side + col * tp + x) * 4;
                        for (u32 c = 0; c < 3; ++c) o[c] = (u8)(acc[c] / cnt);
                        o[3] = 255;
                    }
            }
        g_map_tiles_ok = ok_n; g_map_tiles = n * n;
        if (!ok_n) {
            g_img_fail++;
            img_fail_note(f == 0 ? "mapa" : f == 1 ? "mapa Kitakami" : "mapa Arandano", last_n, why);
            game_unlock();
            return EDEN_DSMOD_FALSE;
        }
        g_map_w = g_map_h = side; g_map_f = want;
    }
    if (rot) {
        /* "heading up" (v0.7.2): a round picture, centred on the grid point near you, turned so
         * the way you walk points up. Outside the map: sea. */
        u32 zoom = 1u << zl, cells = 16u * zoom;
        float cx = (float)tx * g_map_w / cells, cy = (float)ty * g_map_h / cells;
        float r = (float)g_map_w / (2.0f * zoom);                 /* source radius in pixels */
        u32 O = TEX_OUT_SIDE < 768 ? TEX_OUT_SIDE : 768, half = O / 2;
        float sc = r / (float)half, c = cos_deg((s32)hb * 5), sn = sin_deg((s32)hb * 5);
        for (u32 oy = 0; oy < O; ++oy)
            for (u32 ox = 0; ox < O; ++ox) {
                u8* o = g_tex_rgba + ((u64)oy * O + ox) * 4;
                float dx = (float)ox - half + 0.5f, dy = (float)oy - half + 0.5f;
                if (dx * dx + dy * dy > (float)half * half) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
                float mx = cx + (dx * c - dy * sn) * sc, my = cy + (dx * sn + dy * c) * sc;
                if (mx < 0 || my < 0 || mx >= g_map_w || my >= g_map_h) { o[0] = 0x3C; o[1] = 0x78; o[2] = 0xC8; o[3] = 255; continue; }
                const u8* a = g_map_px + ((u64)(u32)my * g_map_w + (u32)mx) * 4;
                o[0] = a[0]; o[1] = a[1]; o[2] = a[2]; o[3] = 255;
            }
        g_img_ok++;
        sink(receiver, O, O, g_tex_rgba, (size_t)O * O * 4);
        game_unlock();
        return EDEN_DSMOD_TRUE;
    }
    u32 zoom = 1u << zl, cells = 8u * zoom;
    u32 cw = g_map_w / zoom, ch = g_map_h / zoom;
    s64 x0 = zl ? (s64)((u64)tx * g_map_w / cells) - cw / 2 : 0;
    s64 y0 = zl ? (s64)((u64)ty * g_map_h / cells) - ch / 2 : 0;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x0 + cw > g_map_w) x0 = g_map_w - cw; if (y0 + ch > g_map_h) y0 = g_map_h - ch;
    /* crop into g_tex_rgba, halving while above the host's 1024 px */
    u32 step = 1;
    while (cw / step > TEX_OUT_SIDE || ch / step > TEX_OUT_SIDE) step *= 2;
    u32 ow = cw / step, oh = ch / step;
    for (u32 y = 0; y < oh; ++y)
        for (u32 x = 0; x < ow; ++x) {
            u32 acc[4] = {0, 0, 0, 0};
            for (u32 dy = 0; dy < step; ++dy)
                for (u32 dx = 0; dx < step; ++dx) {
                    const u8* a = g_map_px + (((u64)(y0 + y * step + dy)) * g_map_w + (u64)(x0 + x * step + dx)) * 4;
                    acc[0] += a[0]; acc[1] += a[1]; acc[2] += a[2]; acc[3] += a[3];
                }
            u8* o = g_tex_rgba + ((u64)y * ow + x) * 4;
            for (u32 c = 0; c < 4; ++c) o[c] = (u8)(acc[c] / (step * step));
        }
    g_img_ok++;
    sink(receiver, ow, oh, g_tex_rgba, (size_t)ow * oh * 4);
    game_unlock();
    return EDEN_DSMOD_TRUE;
}
LOCAL EdenDsmodBool load_image_impl(const EdenDsmodHostApi* host, const char* key, void* receiver, EdenDsmodImageSink sink) {
    char path[160];
    Str s; s_init(&s, path, sizeof(path));
    const char* alt[2] = {path, 0};
    u32 nalt = 1;
    if (starts(key, "module:item/")) {
        u32 id = parse_uint(key + 12);
        s_cat(&s, "appli/tex/icon_item/item_"); s_u4(&s, id); s_cat(&s, "/item_"); s_u4(&s, id); s_cat(&s, ".bntx");
    } else if (starts(key, "module:poke/")) {
        u32 v = parse_uint(key + 12), nat = v / 100, form = v % 100, sp = internal9((u16)nat);
        if (!nat) return EDEN_DSMOD_FALSE;
        s_cat(&s, "pokemon/data/pm"); s_u4(&s, sp); s_cat(&s, "/pm"); s_u4(&s, sp); s_ch(&s, '_'); s_u(&s, form, 2);
        s_cat(&s, "_00/icon/pm"); s_u4(&s, sp); s_ch(&s, '_'); s_u(&s, form, 2); s_cat(&s, "_00_00.bntx");
    } else if (starts(key, "module:map/")) {
        /* the whole region in one picture (PokeDocs SV hash list): ymap_ter_*_1x1 */
        static const char* const maps[3][2] = {
            {"appli/tex/ymap_ter_normal/ymap_ter_1x1/ymap_ter_1x1.bntx",
             "appli/tex/ymap_ter_small/ymap_ter_small_1x1/ymap_ter_small_1x1.bntx"},
            {"appli/tex/ymap_ter_dlc1_normal/ymap_ter_dlc1_1x1/ymap_ter_dlc1_1x1.bntx",
             "appli/tex/ymap_ter_dlc1_small/ymap_ter_dlc1_small_1x1/ymap_ter_dlc1_small_1x1.bntx"},
            {"appli/tex/ymap_ter_dlc2_normal/ymap_ter_dlc2_1x1/ymap_ter_dlc2_1x1.bntx",
             "appli/tex/ymap_ter_dlc2_small/ymap_ter_dlc2_small_1x1/ymap_ter_dlc2_small_1x1.bntx"}};
        u32 code = parse_uint(key + 11), f = code & 3;
        if (f > 2) return EDEN_DSMOD_FALSE;
        (void)maps;
        return map_view(host, f, (code >> 2) & 3, (code >> 4) & 0xFF, (code >> 12) & 0xFF, (code >> 20) & 7,
                        (code >> 23) & 1, (code >> 24) & 0x7F, receiver, sink);
    } else {
        return EDEN_DSMOD_FALSE;
    }
    const char* why = "";
    game_lock();
    u32 n = 0, w = 0, hgt = 0;
    int ok = 0;
    for (u32 k = 0; k < nalt && !ok; ++k) { /* a map has a preferred and a fallback picture */
        n = sv_game_read(host, alt[k], g_tex_file, sizeof(g_tex_file), &why);
        ok = n && bntx_decode(g_tex_file, n, &w, &hgt);
    }
    while (ok && (w > TEX_OUT_SIDE || hgt > TEX_OUT_SIDE)) { /* 2x2 box filter, in place */
        u32 nw = w / 2, nh = hgt / 2;
        for (u32 y = 0; y < nh; ++y)
            for (u32 x = 0; x < nw; ++x)
                for (u32 c = 0; c < 4; ++c) {
                    const u8* a = g_tex_rgba + ((u64)(2 * y) * w + 2 * x) * 4 + c;
                    u32 sum = a[0] + a[4] + a[(u64)w * 4] + a[(u64)w * 4 + 4];
                    g_tex_rgba[((u64)y * nw + x) * 4 + c] = (u8)(sum / 4);
                }
        w = nw; hgt = nh;
    }
    if (ok) { g_img_ok++; sink(receiver, w, hgt, g_tex_rgba, (size_t)w * hgt * 4); }
    else { g_img_fail++; img_fail_note(key + 7, n, why); }
    game_unlock();
    return ok ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}
LOCAL void img_fail_note(const char* key, u32 n, const char* why) {
    {
        Str ws; s_init(&ws, g_img_why, sizeof(g_img_why)); s_cat(&ws, key); s_cat(&ws, ": ");
        if (!n) s_cat(&ws, why && *why ? why : "sin archivo");
        else {   /* read but not decoded: say which texture format it is */
            s_cat(&ws, "formato ");
            u32 fmt = n >= 0x40 && le32(g_tex_file + 0x24) && le64(g_tex_file + 0x28) + 8 <= n ? 0 : 1;
            if (!fmt) {
                u64 info = le64(g_tex_file + le64(g_tex_file + 0x28));
                if (info + 0x78 <= n) { u32 f = le32(g_tex_file + info + 0x1C); s_u(&ws, f >> 8, 0); s_ch(&ws, 'x'); s_u(&ws, le32(g_tex_file + info + 0x24), 0); }
            } else s_cat(&ws, "no BNTX");
        }
    }
}

/* ====================== Bag, Pokédex and Map pages (v0.5) ======================
 * Read-only views of three more save blocks (PKHeX SaveBlockAccessor9SV):
 *   KItem 0x21C9BD44   MyItem9: 3000 x {u32 pouch, u32 count, u32 flags, u32 pad}, by item ID
 *   KZukanT1 0xF5D7C0E2 Kitakami-format Pokédex (2.0.1+): 0x20 per internal species,
 *                      +0 forms obtained, +4 forms seen, +8 forms heard of
 *   KZukan 0x0DEAAEBD  older Paldea-format Pokédex: 0x18 per internal species, +0 state 0..3
 *   KPlayerCurrentLocationID 0x19FC5B7B (place_name line), KPlayerCurrentFieldID 0xF17EB014
 *   (0 Paldea, 1 Kitakami, 2 Blueberry), KCoordinates 0x708D1511 (3 floats; saved position)
 * Which pouch / Pokédex page / entry is shown is UI state the manifest sends here through module
 * actions (on_action), so only the visible page is published each tick. */
#define KEY_ITEMS 0x21C9BD44u
#define KEY_ZUKAN_K 0xF5D7C0E2u
#define KEY_ZUKAN_P 0x0DEAAEBDu
#define KEY_LOCATION 0x19FC5B7Bu
#define KEY_FIELD 0xF17EB014u
#define KEY_COORDS 0x708D1511u

/* Streams a (possibly large) object block through `fn` in 4 KiB decrypted chunks. */
typedef void (*ChunkFn)(void* ctx, u32 off, const u8* d, u32 n);
typedef struct { u32 st; int ctr; } XsStream;
LOCAL void xs_init(XsStream* x, u32 key) {
    x->st = key; x->ctr = 0;
    int pc = 0;
    for (u32 k = key; k; k &= k - 1) ++pc;
    for (int i = 0; i < pc; ++i) x->st = xs_adv(x->st);
}
LOCAL void xs_apply(XsStream* x, u8* d, u32 n) {
    for (u32 i = 0; i < n; ++i) {
        d[i] ^= (u8)(x->st >> (x->ctr << 3));
        if (x->ctr == 3) { x->st = xs_adv(x->st); x->ctr = 0; } else ++x->ctr;
    }
}
LOCAL u8 g_chunk[4096];
LOCAL u32 stream_object(const EdenDsmodHostApi* h, const KeyTable* kt, u32 key, u32 max_size, ChunkFn fn, void* ctx) {
    u64 p = keytable_find(h, kt, key);
    u8 hdr[5];
    if (!p || !rd(h, p, hdr, 5)) return 0;
    XsStream x; xs_init(&x, key);
    xs_apply(&x, hdr, 5);
    u32 size = le32(hdr + 1);
    if (hdr[0] != SC_OBJECT || size == 0 || size > max_size) return 0;
    for (u32 pos = 0; pos < size;) {
        u32 n = size - pos < sizeof(g_chunk) ? size - pos : (u32)sizeof(g_chunk);
        if (!rd(h, p + 5 + pos, g_chunk, n)) return 0;
        xs_apply(&x, g_chunk, n);
        fn(ctx, pos, g_chunk, n);
        pos += n;
    }
    return size;
}

/* Any block, whatever its SCBlock type (scalar, object or array): its bytes into out, the
 * byte count returned (0 = missing). *type gets the type code (0 when the key is absent), shown
 * on the map page's diagnostic line. */
LOCAL u32 sc_elem(u8 t) { return t == 3 || t == 8 || t == 12 ? 1 : t == 9 || t == 13 ? 2 : t == 10 || t == 14 || t == 16 ? 4 : t == 11 || t == 15 || t == 17 ? 8 : 0; }
LOCAL u32 read_any(const EdenDsmodHostApi* h, const KeyTable* kt, u32 key, u8* out, u32 cap, u8* type) {
    *type = 0;
    u64 p = keytable_find(h, kt, key);
    u8 hd[6];
    if (!p || !rd(h, p, hd, 6)) return 0;
    XsStream x; xs_init(&x, key);
    xs_apply(&x, hd, 6);
    *type = hd[0];
    u32 skip, n;
    if (hd[0] == 4) { n = le32(hd + 1); skip = 5; }                                   /* object */
    else if (hd[0] == 5) { u32 e = sc_elem(hd[5]); n = e ? le32(hd + 1) * e : 0; skip = 6; } /* array */
    else if (hd[0] == 1 || hd[0] == 2) { out[0] = hd[0] == 2; return 1; }             /* bool */
    else { n = sc_elem(hd[0]); skip = 1; }                                            /* scalar */
    if (n == 0 || n > cap) return 0;
    u8 buf[64];
    if (skip + n > sizeof(buf)) return 0;
    if (!rd(h, p, buf, skip + n)) return 0;
    XsStream y; xs_init(&y, key);
    xs_apply(&y, buf, skip + n);
    memcpy(out, buf + skip, n);
    return n;
}

/* ---------- UI state, set by module actions ---------- */
LOCAL struct { u32 pouch, bpage, region, dpage, dsel; } ui;

/* ---------- Bag ---------- */
#define POUCHES 10
#define BAG_MAX 420
#define BAG_PER_PAGE 14
typedef struct { u16 id, count; } BagItem;
LOCAL BagItem g_bag[POUCHES][BAG_MAX], g_bag_tmp[POUCHES][BAG_MAX];
LOCAL u16 g_bag_n[POUCHES], g_bag_tmp_n[POUCHES];
LOCAL int g_bag_ok;
LOCAL void bag_chunk(void* ctx, u32 off, const u8* d, u32 n) {
    (void)ctx;
    for (u32 j = 0; j + 16 <= n; j += 16) {
        u32 id = (off + j) / 16, pouch = le32(d + j), count = le32(d + j + 4);
        if (id == 0 || pouch >= POUCHES || count == 0 || count > 9999) continue;
        if (g_bag_tmp_n[pouch] < BAG_MAX) g_bag_tmp[pouch][g_bag_tmp_n[pouch]++] = (BagItem){(u16)id, (u16)count};
    }
}
LOCAL void bag_refresh(const EdenDsmodHostApi* h, const KeyTable* kt) {
    memset(g_bag_tmp_n, 0, sizeof(g_bag_tmp_n));
    if (!stream_object(h, kt, KEY_ITEMS, 0x20000, bag_chunk, 0)) return; /* keep the last good copy */
    memcpy(g_bag, g_bag_tmp, sizeof(g_bag));
    memcpy(g_bag_n, g_bag_tmp_n, sizeof(g_bag_n));
    g_bag_ok = 1;
}

/* ---------- Pokédex ---------- */
#define DEX_SPECIES 1400          /* internal species slots kept */
#define DEX_REGIONS 3
#define DEX_PER_PAGE 20
LOCAL u8 g_dex_state[DEX_SPECIES], g_dex_tmp[DEX_SPECIES]; /* 0 unknown, 1 heard of, 2 seen, 3 caught */
LOCAL int g_dex_ok, g_dex_kitakami;
LOCAL u16 g_dex_list[DEX_REGIONS][420];   /* regional number -> national species (0 = none) */
LOCAL u16 g_dex_len[DEX_REGIONS];
LOCAL int g_dex_lists_ok;
LOCAL void zukan_k_chunk(void* ctx, u32 off, const u8* d, u32 n) {
    (void)ctx;
    for (u32 j = 0; j + 32 <= n; j += 32) {
        u32 sp = (off + j) / 32;
        if (sp >= DEX_SPECIES) return;
        g_dex_tmp[sp] = le32(d + j) ? 3 : le32(d + j + 4) ? 2 : le32(d + j + 8) ? 1 : 0;
    }
}
LOCAL void zukan_p_chunk(void* ctx, u32 off, const u8* d, u32 n) {
    (void)ctx;
    /* 0x18-byte entries do not line up with 4 KiB chunks: walk by absolute offset. */
    u32 first = (off + 23) / 24;
    for (u32 sp = first; sp * 24 + 4 <= off + n; ++sp) {
        if (sp >= DEX_SPECIES) return;
        u32 at = sp * 24 - off;
        u32 v = le32(d + at);
        g_dex_tmp[sp] = v > 3 ? 0 : (u8)v;
    }
}
LOCAL void dex_lists_build(void) {
    memset(g_dex_list, 0, sizeof(g_dex_list));
    memset(g_dex_len, 0, sizeof(g_dex_len));
    for (u32 i = 0; i < g_gpers_n; ++i) {
        const GPers* e = &g_gpers[i];
        u32 idx[DEX_REGIONS] = {e->dexp, e->dexk, e->dexb};
        for (u32 r = 0; r < DEX_REGIONS; ++r) {
            u32 k = idx[r];
            if (k == 0 || k >= 420 || g_dex_list[r][k]) continue; /* first form wins */
            g_dex_list[r][k] = e->nat;
            if (k > g_dex_len[r]) g_dex_len[r] = (u16)k;
        }
    }
    g_dex_lists_ok = g_dex_len[0] > 0;
}
LOCAL void trainer_refresh(const EdenDsmodHostApi* h, const KeyTable* kt);
LOCAL void dex_refresh(const EdenDsmodHostApi* h, const KeyTable* kt) {
    memset(g_dex_tmp, 0, sizeof(g_dex_tmp));
    if (stream_object(h, kt, KEY_ZUKAN_K, 0x40000, zukan_k_chunk, 0)) g_dex_kitakami = 1;
    else if (stream_object(h, kt, KEY_ZUKAN_P, 0x40000, zukan_p_chunk, 0)) g_dex_kitakami = 0;
    else return;
    memcpy(g_dex_state, g_dex_tmp, sizeof(g_dex_state));
    g_dex_ok = 1;
}
LOCAL u32 dex_state(u32 nat) {
    u32 sp = internal9((u16)nat);
    return sp < DEX_SPECIES ? g_dex_state[sp] : 0;
}
LOCAL const GPers* pers_of(u32 nat) {
    for (u32 i = 0; i < g_gpers_n; ++i) if (g_gpers[i].nat == nat && g_gpers[i].form == 0) return &g_gpers[i];
    return 0;
}

/* ---------- where the player is ---------- */
/* Live position. The save block only holds where you were at the last save, but right after a
 * save (and right after loading) the player stands exactly there: find that float triple in
 * memory, then keep the copies that move as you walk. RaidCrawler's MapMagic gives the scale:
 * Paldea x 0..5000, z -5500..-500; Kitakami and Blueberry x 0..2000, z -2000..0. */
#define POS_CANDS 384 /* v0.6.7 kept 24 and the device showed "copias 24": the live one was cut off */
#define POS_HOLD 12   /* refreshes (3 s) a copy that stopped reading is still shown where it was */
/* v0.7.5: the device showed the map flipping between north up and south up and the marker
 * jumping. The saved position in the save block changes far more often than at a save, and every
 * change used to throw the live copy away and search again: for the second or two of each search
 * the map fell back to the saved spot (heading up switched off, the compass jumped back to north)
 * and the next pick could be another copy (the camera's, behind you), so the heading worked out
 * between two different copies pointed backwards. Now the copy being followed is kept through new
 * saved positions that agree with it, searches only add copies, the pick has a score that changes
 * slowly, a copy that stops reading is held for a moment, and the heading only comes from the
 * same copy's own steps (a sudden about-turn needs two steps to be believed). */
LOCAL struct {
    float sx, sz, sy;        /* the saved position the last search was for */
    float bx, bz, by; int bseen;  /* the last saved position read from the block */
    int pending;             /* a search is due */
    u64 cur;
    u64 addr[POS_CANDS]; float x0[POS_CANDS], z0[POS_CANDS];
    float cx[POS_CANDS], cz[POS_CANDS], score[POS_CANDS];
    u8 ok[POS_CANDS], moved[POS_CANDS], bad[POS_CANDS], pen[POS_CANDS]; /* pen: a saved position far from it since */
    u32 n;
    int live;                /* index of the copy followed, -1 none */
    int shown;               /* a live position is shown: the followed copy, or held a moment */
    u32 lost;                /* refreshes the followed copy has not read */
    float lx, lz;
    float hx, hz; int hok, hidx, dir_ok; s32 heading, pend; u32 pend_n;  /* facing, from steps */
    u32 searches, retry, switches, agreed, disagreed, cluster, turns_held;
} PS;
/* atan2 in whole degrees, 0..359 clockwise from +y (here: east = 90). No libm in the module. */
LOCAL s32 atan2_deg(float x, float y) {
    float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    if (ax == 0 && ay == 0) return 0;
    float a = ax < ay ? ax / ay : ay / ax;                       /* 0..1 */
    float t = a * (45.0f - (a - 1.0f) * (14.0f + 3.83f * a));   /* atan(a) in degrees, ~0.1 deg */
    if (ax > ay) t = 90.0f - t;                                  /* angle from +y towards +x */
    if (y < 0) t = 180.0f - t;
    if (x < 0) t = 360.0f - t;
    s32 d = (s32)(t + 0.5f);
    return d >= 360 ? d - 360 : d;
}
LOCAL s32 ang_diff(s32 a, s32 b) { s32 d = a - b; d %= 360; if (d < 0) d += 360; return d > 180 ? 360 - d : d; }
LOCAL float fabsf_(float v) { return v < 0 ? -v : v; }
LOCAL int pos_ok(float x, float z) { return x == x && z == z && x > -6000 && x < 12000 && z > -12000 && z < 6000; }
/* A search keeps what it had (the copy followed and every copy that ever moved) and adds the
 * places that hold the saved position now. Copies that never moved are dropped only when the
 * list is getting full. */
LOCAL void pos_search_begin(void) {
    if (PS.n > POS_CANDS * 3 / 4) {
        u32 k = 0; int live = -1;
        for (u32 i = 0; i < PS.n; ++i) {
            if ((int)i != PS.live && (!PS.moved[i] || PS.bad[i])) continue;
            if ((int)i == PS.live) live = (int)k;
            PS.addr[k] = PS.addr[i]; PS.x0[k] = PS.x0[i]; PS.z0[k] = PS.z0[i]; PS.cx[k] = PS.cx[i]; PS.cz[k] = PS.cz[i];
            PS.score[k] = PS.score[i]; PS.ok[k] = PS.ok[i]; PS.moved[k] = PS.moved[i]; PS.bad[k] = PS.bad[i]; PS.pen[k] = PS.pen[i];
            k++;
        }
        PS.n = k; PS.live = live; PS.hok = 0;
    }
    PS.cur = S.lo;
}
LOCAL void pos_search_step(const EdenDsmodHostApi* h, u64 budget) {
    if (!PS.pending || !S.lo) return;
    if (!PS.cur) pos_search_begin();
    u64 end = PS.cur + budget;
    while (PS.cur < end) {
        if (PS.cur >= S.hi) { PS.pending = 0; PS.cur = 0; PS.searches++; return; }
        u64 page = PS.cur & ~0xFFFull;
        const u8* p = h->get_read_pointer ? h->get_read_pointer(h->userdata, page, 0x1000) : 0;
        if (!p) { PS.cur = page + 0x1000; continue; }
        for (u32 off = 0; off + 12 <= 0x1000; off += 4) {
            float x, y, z;
            memcpy(&x, p + off, 4);
            if (fabsf_(x - PS.sx) > 0.5f) continue;
            memcpy(&y, p + off + 4, 4); memcpy(&z, p + off + 8, 4);
            if (fabsf_(y - PS.sy) > 0.5f || fabsf_(z - PS.sz) > 0.5f) continue;
            u64 a = page + off;
            int dup = 0;
            for (u32 i = 0; i < PS.n && !dup; ++i) if (PS.addr[i] == a) dup = 1;
            if (dup || PS.n >= POS_CANDS) continue;
            u32 k = PS.n++;
            PS.addr[k] = a; PS.x0[k] = PS.cx[k] = x; PS.z0[k] = PS.cz[k] = z;
            PS.score[k] = 0; PS.ok[k] = 1; PS.moved[k] = 0; PS.bad[k] = 0; PS.pen[k] = 0;
        }
        PS.cur = page + 0x1000;
    }
}
LOCAL void pos_heading(float px, float pz) {
    if (!PS.hok || PS.hidx != PS.live) { PS.hx = px; PS.hz = pz; PS.hok = 1; PS.hidx = PS.live; return; }
    float dx = px - PS.hx, dz = pz - PS.hz, d2 = dx * dx + dz * dz;
    if (d2 > 200.0f * 200.0f) { PS.hx = px; PS.hz = pz; return; }   /* flew somewhere: start again */
    if (d2 < 16.0f) return;                                          /* 4 units from the last step */
    s32 hd = atan2_deg(dx, -dz); /* clockwise from north (map up = -z) */
    PS.hx = px; PS.hz = pz;
    if (!PS.dir_ok || ang_diff(hd, PS.heading) <= 100) { PS.heading = hd; PS.dir_ok = 1; PS.pend_n = 0; }
    else if (PS.pend_n && ang_diff(hd, PS.pend) <= 45) { PS.heading = hd; PS.pend_n = 0; }  /* really turned */
    else { PS.pend = hd; PS.pend_n = 1; PS.turns_held++; }
}
LOCAL void pos_track(const EdenDsmodHostApi* h) {
    for (u32 i = 0; i < PS.n; ++i) {
        float v[3];
        PS.ok[i] = 0;
        if (PS.bad[i] || !rd(h, PS.addr[i], v, 12) || !pos_ok(v[0], v[2])) continue;
        PS.ok[i] = 1; PS.cx[i] = v[0]; PS.cz[i] = v[2];
        if (fabsf_(v[0] - PS.x0[i]) > 1.0f || fabsf_(v[2] - PS.z0[i]) > 1.0f) PS.moved[i] = 1;
    }
    /* The player's position is kept in several places at once (transform, physics...): copies
     * that moved and agree with many others score; the score changes slowly. */
    int best = -1;
    for (u32 i = 0; i < PS.n; ++i) {
        if (!PS.ok[i] || !PS.moved[i] || PS.pen[i]) { PS.score[i] *= 0.9f; continue; }
        u32 same = 0;
        for (u32 j = 0; j < PS.n; ++j)
            if (PS.ok[j] && PS.moved[j] && !PS.pen[j] && fabsf_(PS.cx[j] - PS.cx[i]) < 0.5f && fabsf_(PS.cz[j] - PS.cz[i]) < 0.5f) same++;
        PS.score[i] = PS.score[i] * 0.95f + (float)same;
        if (best < 0 || PS.score[i] > PS.score[best]) best = (int)i;
    }
    int live = PS.live;
    if (live >= 0 && PS.pen[live] && best >= 0) { live = best; PS.lost = 0; PS.switches++; }  /* the saved spot says it is wrong */
    else if (live >= 0 && PS.ok[live] && PS.moved[live]) {
        PS.lost = 0;
        if (best >= 0 && best != live && PS.score[best] > PS.score[live] * 1.5f + 2.0f) { live = best; PS.switches++; }
    } else if (live >= 0) {
        /* the copy followed did not read: one that is where it was takes over at once, any
         * other only after a moment */
        ++PS.lost;
        if (best >= 0 && fabsf_(PS.cx[best] - PS.lx) < 3.0f && fabsf_(PS.cz[best] - PS.lz) < 3.0f) { live = best; PS.lost = 0; PS.switches++; }
        else if (PS.lost >= POS_HOLD) { live = best; PS.lost = 0; PS.switches++; }
    } else live = best;
    PS.live = live;
    if (live >= 0 && PS.ok[live]) {
        PS.lx = PS.cx[live]; PS.lz = PS.cz[live]; PS.shown = 1;
        u32 same = 0;
        for (u32 j = 0; j < PS.n; ++j)
            if (PS.ok[j] && PS.moved[j] && fabsf_(PS.cx[j] - PS.lx) < 0.5f && fabsf_(PS.cz[j] - PS.lz) < 0.5f) same++;
        PS.cluster = same;
        pos_heading(PS.lx, PS.lz);
    } else if (live < 0) PS.shown = 0;   /* (a copy held for a moment stays shown where it was) */
}

LOCAL struct { int ok, field_ok, coords_ok; s64 location, field; float x, y, z; u8 t_loc, t_field, t_pos; u32 n_pos; } g_place_now;
LOCAL void place_refresh(const EdenDsmodHostApi* h, const KeyTable* kt) {
    u8 b[32]; u32 n;
    n = read_any(h, kt, KEY_LOCATION, b, sizeof(b), &g_place_now.t_loc);
    g_place_now.ok = n == 1 || n == 2 || n == 4 || n == 8;
    if (g_place_now.ok) g_place_now.location = n == 1 ? b[0] : n == 2 ? le16(b) : le32(b);
    n = read_any(h, kt, KEY_FIELD, b, sizeof(b), &g_place_now.t_field);
    g_place_now.field_ok = n == 1 || n == 2 || n == 4 || n == 8;
    if (g_place_now.field_ok) g_place_now.field = n == 1 ? b[0] : n == 2 ? le16(b) : le32(b);
    g_place_now.coords_ok = 0;
    n = read_any(h, kt, KEY_COORDS, b, sizeof(b), &g_place_now.t_pos);
    g_place_now.n_pos = n;
    if (n >= 12) {
        memcpy(&g_place_now.x, b, 4); memcpy(&g_place_now.y, b + 4, 4); memcpy(&g_place_now.z, b + 8, 4);
        g_place_now.coords_ok = 1;
        float x = g_place_now.x, y = g_place_now.y, z = g_place_now.z;
        if (g.ingame && pos_ok(x, z) && (!PS.bseen || x != PS.bx || z != PS.bz || y != PS.by)) {
            PS.bx = x; PS.by = y; PS.bz = z; PS.bseen = 1;
            /* The player stood there a moment ago. If that is where the copy followed is, it is
             * right: copies there gain, nothing else changes. If not, look for that spot. */
            float ex = PS.lx - x, ez = PS.lz - z;
            if (PS.shown && ex * ex + ez * ez <= 80.0f * 80.0f) {
                PS.agreed++;
                for (u32 i = 0; i < PS.n; ++i)
                    if (PS.ok[i] && PS.moved[i] && fabsf_(PS.cx[i] - x) < 40.0f && fabsf_(PS.cz[i] - z) < 40.0f) { PS.score[i] += 20.0f; PS.pen[i] = 0; }
            } else {
                if (PS.shown) {
                    PS.disagreed++;
                    for (u32 i = 0; i < PS.n; ++i)
                        if (PS.ok[i] && (fabsf_(PS.cx[i] - x) > 80.0f || fabsf_(PS.cz[i] - z) > 80.0f)) { PS.score[i] = 0; PS.pen[i] = 1; }
                }
                if (!PS.pending) { PS.sx = x; PS.sy = y; PS.sz = z; PS.pending = 1; PS.cur = 0; }
            }
        }
    }
    /* nothing live yet (the player object may not have existed, or did not move): look for
     * the saved spot again (every 10 s with nothing found, 30 s with copies that never moved) */
    if (g.ingame && g_place_now.coords_ok && !PS.pending && !PS.shown && ++PS.retry >= (PS.n ? 30u : 10u)) {
        PS.retry = 0; PS.sx = g_place_now.x; PS.sy = g_place_now.y; PS.sz = g_place_now.z; PS.pending = 1; PS.cur = 0;
    }
    pos_track(h);
}

LOCAL u32 g_pages_ticks;
LOCAL void pages_refresh(const EdenDsmodHostApi* h) {
    if (!g.party_ok) return;
    KeyTable kt;
    if (!keytable_resolve(h, &kt)) return;
    u32 t = g_pages_ticks++;
    if (t % 8 == 0) bag_refresh(h, &kt);             /* every 2 s */
    if (t % 12 == 4) dex_refresh(h, &kt);            /* every 3 s */
    if (t % 8 == 5) trainer_refresh(h, &kt);         /* every 2 s */
    if (t % 4 == 2) place_refresh(h, &kt);           /* every second */
    else pos_track(h);                               /* the live marker 4 times a second */
    if (!g_dex_lists_ok && g_gpers_n) dex_lists_build();
}

/* ---------- publishing the visible page ---------- */
LOCAL void s_dec1(Str* s, u32 v, const char* unit) { /* 123 -> "12,3 unit" ("12.3" in English) */
    s_u(s, v / 10, 0); s_ch(s, g_en ? '.' : ','); s_u(s, v % 10, 0); s_ch(s, ' '); s_cat(s, unit);
}
LOCAL void publish_bag(void) {
    if (ui.pouch >= POUCHES) ui.pouch = 0;
    u32 n = g_bag_n[ui.pouch];
    u32 pages = n ? (n + BAG_PER_PAGE - 1) / BAG_PER_PAGE : 1;
    if (ui.bpage >= pages) ui.bpage = pages - 1;
    pi("sv.bag.ok", g_bag_ok);
    pi("sv.bag.pouch", ui.pouch);
    pi("sv.bag.n", n);
    pi("sv.bag.page", ui.bpage + 1);
    pi("sv.bag.pages", pages);
    pi("sv.bag.prev", ui.bpage > 0);
    pi("sv.bag.next", ui.bpage + 1 < pages);
    pi("sv.bag.empty", g_bag_ok && n == 0);
    for (u32 p = 0; p < POUCHES; ++p) pi(K("sv.bag.pc", p, ""), g_bag_n[p]);
    for (u32 i = 0; i < BAG_PER_PAGE; ++i) {
        u32 at = ui.bpage * BAG_PER_PAGE + i;
        int ok = at < n;
        pi(K("sv.bag.", i, ".valid"), ok);
        if (!ok) continue;
        const BagItem* it = &g_bag[ui.pouch][at];
        pt(K("sv.bag.", i, ".name"), name_or_num(&g_items, it->id, TR("Objeto ", "Item ")));
        pi(K("sv.bag.", i, ".count"), it->count);
        pi(K("sv.bag.", i, ".id"), it->id);
        pi(K("sv.bag.", i, ".many"), ui.pouch != 8 && ui.pouch != 5); /* key items / TMs: no "x1" */
    }
}
LOCAL void publish_dex(void) {
    if (ui.region >= DEX_REGIONS) ui.region = 0;
    u32 len = g_dex_len[ui.region];
    u32 pages = len ? (len + DEX_PER_PAGE - 1) / DEX_PER_PAGE : 1;
    if (ui.dpage >= pages) ui.dpage = pages - 1;
    if (ui.dsel >= DEX_PER_PAGE) ui.dsel = 0;
    pi("sv.dex.ok", g_dex_ok && g_dex_lists_ok);
    pi("sv.dex.region", ui.region);
    pi("sv.dex.page", ui.dpage + 1);
    pi("sv.dex.pages", pages);
    pi("sv.dex.prev", ui.dpage > 0);
    pi("sv.dex.next", ui.dpage + 1 < pages);
    for (u32 r = 0; r < DEX_REGIONS; ++r) {
        u32 seen = 0, caught = 0;
        for (u32 k = 1; k <= g_dex_len[r]; ++k) {
            u32 st = g_dex_list[r][k] ? dex_state(g_dex_list[r][k]) : 0;
            seen += st >= 2; caught += st >= 3;
        }
        pi(K("sv.dex.seen", r, ""), seen);
        pi(K("sv.dex.caught", r, ""), caught);
        pi(K("sv.dex.total", r, ""), g_dex_len[r]);
    }
    for (u32 i = 0; i < DEX_PER_PAGE; ++i) {
        u32 num = ui.dpage * DEX_PER_PAGE + i + 1;
        u32 nat = num <= len ? g_dex_list[ui.region][num] : 0;
        u32 st = nat ? dex_state(nat) : 0;
        pi(K("sv.dex.", i, ".valid"), nat != 0);
        if (!nat) continue;
        pi(K("sv.dex.", i, ".num"), num);
        pi(K("sv.dex.", i, ".st"), st);
        pi(K("sv.dex.", i, ".known"), st >= 2);
        pi(K("sv.dex.", i, ".caught"), st >= 3);
        pi(K("sv.dex.", i, ".spr"), nat);
        int us = st >= 2 && sprite_present(nat);
        pi(K("sv.dex.", i, ".has_spr"), us);
        pi(K("sv.dex.", i, ".icon"), (s64)nat * 100);           /* module:poke/<nat*100+form> */
        pi(K("sv.dex.", i, ".game_icon"), st >= 2 && !us);
        pi(K("sv.dex.", i, ".sel"), i == ui.dsel);
    }
    /* the selected entry */
    u32 num = ui.dpage * DEX_PER_PAGE + ui.dsel + 1;
    u32 nat = num <= len ? g_dex_list[ui.region][num] : 0;
    u32 st = nat ? dex_state(nat) : 0;
    const GPers* pe = nat ? pers_of(nat) : 0;
    u32 intl = nat ? internal9((u16)nat) : 0;
    pi("sv.dx.valid", nat != 0);
    pi("sv.dx.num", num);
    pi("sv.dx.known", st >= 2);
    pi("sv.dx.caught", st >= 3);
    pi("sv.dx.spr", nat);
    int dus = st >= 2 && nat && sprite_present(nat);
    pi("sv.dx.has_spr", dus);
    pi("sv.dx.icon", (s64)nat * 100);
    pi("sv.dx.game_icon", st >= 2 && nat && !dus);
    pt("sv.dx.name", st >= 1 && nat ? species_name(nat) : "???");
    if (st >= 2 && pe) {
        pt("sv.dx.cat", namelist_get(&g_zkntype, intl));
        pt("sv.dx.text", namelist_get(&g_zukan, intl));
        Str s; char b[32];
        s_init(&s, b, sizeof(b)); /* personal height is in cm */
        s_u(&s, pe->height / 100, 0); s_ch(&s, g_en ? '.' : ','); s_u(&s, (pe->height % 100) / 10, 0);
        if (pe->height % 10) s_u(&s, pe->height % 10, 0);
        s_cat(&s, " m"); pt("sv.dx.height", b);
        s_init(&s, b, sizeof(b)); s_dec1(&s, pe->weight, "kg"); pt("sv.dx.weight", b);
        pi("sv.dx.t1", pe->t1); pi("sv.dx.t2", pe->t2); pi("sv.dx.dual", pe->t2 != pe->t1);
        pt("sv.dx.t1n", name_or_num(&g_types, pe->t1, TR("Tipo ", "Type "))); pt("sv.dx.t2n", name_or_num(&g_types, pe->t2, TR("Tipo ", "Type ")));
    } else {
        pt("sv.dx.text", nat ? (st == 1 ? "Aún no has visto a este Pokémon." : "Aún no sabes nada de este Pokémon.") : "");
    }
}
LOCAL u32 g_map_zoom; /* 0..3 = x1, x2, x4, x8 (module action "map_zoom" +1 / -1) */
LOCAL u32 g_map_swap = 3; /* tile orientation mode, see map_view (module action "map_flip") */
LOCAL int g_map_rot;       /* "heading up" asked for (module action "map_rot") */
LOCAL void publish_place(void) {
    static const char* const fields_es[3] = {"Paldea", "Kitakami", "Instituto Arándano"};
    static const char* const fields_en[3] = {"Paldea", "Kitakami", "Blueberry Academy"};
    const char* const* fields = g_en ? fields_en : fields_es;
    pi("sv.map.ok", g_place_now.ok);
    pt("sv.map.place", g_place_now.ok ? name_or_num(&g_place, (u32)g_place_now.location, TR("Lugar ", "Place ")) : TR("Desconocido", "Unknown"));
    pt("sv.map.field", g_place_now.field_ok && g_place_now.field >= 0 && g_place_now.field < 3 ? fields[g_place_now.field] : "");
    pi("sv.map.coords_ok", g_place_now.coords_ok);
    pi("sv.map.x", (s64)g_place_now.x);
    pi("sv.map.y", (s64)g_place_now.y);
    pi("sv.map.z", (s64)g_place_now.z);
    pi("sv.map.loc", g_place_now.location);
    /* marker on the map picture: MAP_PX square in the manifest */
    #define MAP_PX 900
    int f = g_place_now.field_ok && g_place_now.field >= 0 && g_place_now.field < 3 ? (int)g_place_now.field : 0;
    int live = PS.shown;
    float x = live ? PS.lx : g_place_now.x, z = live ? PS.lz : g_place_now.z;
    float span = f == 0 ? 5000.0f : 2000.0f, zoff = f == 0 ? 5500.0f : 2000.0f;
    float u = x / span, v = (z + zoff) / span;
    int on = (g_place_now.coords_ok || live) && u >= 0 && u <= 1 && v >= 0 && v <= 1;
    /* zoom: the module crops the region picture around you (module:map key below), on a grid of
     * 1/(8*zoom) of the map so the picture is reloaded only after you walk a little. */
    int rot = g_map_rot && on && live && PS.dir_ok;
    u32 zl = on ? g_map_zoom : 0, zoom = 1u << zl;
    if (rot && !zl) { zl = 1; zoom = 2; }       /* turning the whole region makes little sense */
    u32 tx = 0, ty = 0, hb = rot ? (u32)(((PS.heading + 2) / 5) % 72) : 0;
    float u0 = 0, v0 = 0, vs = 1.0f;
    float rc = 1, rsn = 0, cu0 = 0, cv0 = 0;    /* rotation and centre (rot only) */
    if (rot) {
        u32 cells = 16u * zoom;
        float cu = u < 0 ? 0 : u > 1 ? 1 : u, cv = v < 0 ? 0 : v > 1 ? 1 : v;
        tx = (u32)(cu * cells + 0.5f); ty = (u32)(cv * cells + 0.5f);
        cu0 = (float)tx / cells; cv0 = (float)ty / cells;
        vs = 1.0f / (float)zoom;
        rc = cos_deg((s32)hb * 5); rsn = sin_deg((s32)hb * 5);
    } else if (zl) {
        u32 cells = 8u * zoom;                     /* grid cells across the map */
        float half = 0.5f / (float)zoom;
        float cu = u < half ? half : u > 1 - half ? 1 - half : u;
        float cv = v < half ? half : v > 1 - half ? 1 - half : v;
        tx = (u32)(cu * cells + 0.5f); ty = (u32)(cv * cells + 0.5f);
        if (tx < cells / (2 * zoom)) tx = cells / (2 * zoom);
        if (ty < cells / (2 * zoom)) ty = cells / (2 * zoom);
        if (tx > cells - cells / (2 * zoom)) tx = cells - cells / (2 * zoom);
        if (ty > cells - cells / (2 * zoom)) ty = cells - cells / (2 * zoom);
        vs = 1.0f / (float)zoom;
        u0 = (float)tx / cells - vs / 2; v0 = (float)ty / cells - vs / 2;
    }
    pi("sv.map.img", (s64)f | ((s64)zl << 2) | ((s64)tx << 4) | ((s64)ty << 12) | ((s64)g_map_swap << 20)
                     | ((s64)rot << 23) | ((s64)hb << 24));
    pi("sv.map.rot", rot);
    pi("sv.map.rot_want", g_map_rot);
    pi("sv.map.rot_wait", g_map_rot && !rot);
    pi("sv.map.north", rot ? (360 - (s64)hb * 5) % 360 : 0);
    pi("sv.map.zoom", zoom);
    pi("sv.map.can_in", zl < 3);
    pi("sv.map.can_out", zl > 0);
    float mu = (u - u0) / vs, mv = (v - v0) / vs;
    if (rot) {   /* map units -> the turned round picture (inverse of map_view's turn) */
        float du = (u - cu0) / vs, dv = (v - cv0) / vs;
        mu = 0.5f + du * rc + dv * rsn; mv = 0.5f - du * rsn + dv * rc;
    }
    int inview = on && mu >= 0 && mu <= 1 && mv >= 0 && mv <= 1;
    pi("sv.map.on", on);
    pi("sv.map.live", inview && live && !PS.dir_ok);
    pi("sv.map.arrow", inview && live && PS.dir_ok);
    pi("sv.map.heading", rot ? 0 : PS.heading);   /* heading up: your arrow always points up */
    pi("sv.map.dir_ok", live && PS.dir_ok);
    pi("sv.map.saved", inview && !live);
    pi("sv.map.mx", inview ? (s64)(mu * MAP_PX) : 0);
    pi("sv.map.my", inview ? (s64)(mv * MAP_PX) : 0);
    {
        static const char* const dirs_es[8] = {"Norte", "Noreste", "Este", "Sureste", "Sur", "Suroeste", "Oeste", "Noroeste"};
        static const char* const dirs_en[8] = {"North", "Northeast", "East", "Southeast", "South", "Southwest", "West", "Northwest"};
        const char* const* dirs = g_en ? dirs_en : dirs_es;
        pt("sv.map.dir", live && PS.dir_ok ? dirs[((PS.heading + 22) / 45) % 8] : "");
    }
    {   /* place names inside the view: the zoom level each one is meant for */
        u32 k = 0;
        u8 want = zl == 0 ? 1 : zl == 1 ? 2 : 4;
        for (u32 i = 0; i < g_mlab_n && k < 24; ++i) {
            const MapLab* L2 = &g_mlab[i];
            if ((int)L2->field != f || (L2->zoom && !(L2->zoom & want))) continue;
            float lu = L2->x / span, lv = (L2->z + zoff) / span, su, sv;
            if (rot) {
                float du = (lu - cu0) / vs, dv = (lv - cv0) / vs;
                su = 0.5f + du * rc + dv * rsn; sv = 0.5f - du * rsn + dv * rc;
                float ex = su - 0.5f, ey = sv - 0.5f;
                if (ex * ex + ey * ey > 0.2f) continue;            /* inside the round picture */
            } else {
                su = (lu - u0) / vs; sv = (lv - v0) / vs;
                if (su < 0.04f || su > 0.96f || sv < 0.03f || sv > 0.97f) continue;
            }
            pi(K("sv.map.l", k, ".ok"), 1);
            pi(K("sv.map.l", k, ".x"), (s64)(su * MAP_PX));
            pi(K("sv.map.l", k, ".y"), (s64)(sv * MAP_PX));
            pt(K("sv.map.l", k, ".t"), name_or_num(&g_ymapname, L2->text, ""));
            ++k;
        }
        for (u32 j = k; j < 24; ++j) pi(K("sv.map.l", j, ".ok"), 0);
        pi("sv.map.labels", g_mlab_n);
    }
    pi("sv.map.px", (s64)x);
    pi("sv.map.pz", (s64)z);
    pi("sv.map.cands", PS.n);
    {   /* what the map page knows, for a photo when something is off */
        Str d; s_init(&d, g_val, sizeof(g_val));
        /* the live position first: the label shows five short lines */
        if (PS.shown) {
            s_cat(&d, PS.lost ? "RETENIDO" : "EN VIVO"); s_cat(&d, " grupo "); s_u(&d, PS.cluster, 0);
            s_cat(&d, " cambios "); s_u(&d, PS.switches, 0);
        } else s_cat(&d, PS.pending ? "buscando" : "sin mover");
        s_cat(&d, " | copias "); s_u(&d, PS.n, 0); s_cat(&d, " busq "); s_u(&d, PS.searches, 0);
        s_cat(&d, " | guardada "); s_u(&d, PS.agreed, 0); s_ch(&d, '/'); s_u(&d, PS.disagreed, 0);
        s_cat(&d, " giros "); s_u(&d, PS.turns_held, 0);
        s_cat(&d, " | pos ");
        if (g_place_now.coords_ok) {
            s64 vals[3] = {(s64)g_place_now.x, (s64)g_place_now.y, (s64)g_place_now.z};
            for (int k = 0; k < 3; ++k) { if (k) s_ch(&d, ','); if (vals[k] < 0) { s_ch(&d, '-'); vals[k] = -vals[k]; } s_u(&d, (u64)vals[k], 0); }
        } else s_cat(&d, "?");
        s_cat(&d, " | lugar "); if (g_place_now.ok) s_u(&d, (u64)g_place_now.location, 0); else s_cat(&d, "?");
        s_cat(&d, " campo "); if (g_place_now.field_ok) s_u(&d, (u64)g_place_now.field, 0); else s_cat(&d, "?");
        s_cat(&d, " | img "); s_u(&d, g_img_ok, 0); s_ch(&d, '/'); s_u(&d, g_img_fail, 0);
        s_cat(&d, " piezas "); s_u(&d, g_map_tiles_ok, 0); s_ch(&d, '/'); s_u(&d, g_map_tiles, 0);
        s_cat(&d, " modo "); s_u(&d, g_map_swap, 0);
        s_cat(&d, " | nombres "); s_u(&d, g_mlab_n, 0);
        pt("sv.map.diag", g_val);
        pt("sv.map.why", g_img_why[0] ? g_img_why : "imagen: sin fallos");
    }
}
/* ---------- trainer card (v0.6.7) ----------
 * Save blocks (PKHeX SaveBlockAccessor9SV): KPlayTime 0xEDAFF794 = {s32 hours, s32 minutes,
 * s32 seconds}; KBlueberryPoints 0x66A33824 = u32 (only with The Indigo Disk). The device's clock,
 * battery and the emulator's FPS come from the host (Eden Duo runtime 16 / 21 keys): an older
 * runtime answers the fallback and the card simply leaves them out. */
#define KEY_PLAYTIME 0xEDAFF794u
#define KEY_BP 0x66A33824u
LOCAL struct { int time_ok, bp_ok; u32 h, m, bp; } TR;
LOCAL void trainer_refresh(const EdenDsmodHostApi* h, const KeyTable* kt) {
    u8 b[32]; u8 t = 0;
    u32 n = read_any(h, kt, KEY_PLAYTIME, b, sizeof(b), &t);
    TR.time_ok = n >= 8 && le32(b) < 100000 && le32(b + 4) < 60;
    if (TR.time_ok) { TR.h = le32(b); TR.m = le32(b + 4); }
    u32 v;
    TR.bp_ok = read_u32(h, kt, KEY_BP, &v) && v < 10000000;
    if (TR.bp_ok) TR.bp = v;
}
LOCAL void publish_trainer(const EdenDsmodHostApi* h) {
    Str s; char b[64];
    pi("sv.tr.time_ok", TR.time_ok);
    if (TR.time_ok) { s_init(&s, b, sizeof(b)); s_u(&s, TR.h, 0); s_cat(&s, " h "); s_u(&s, TR.m, 2); s_cat(&s, " min"); pt("sv.tr.time", b); }
    pi("sv.tr.bp_ok", TR.bp_ok);
    pi("sv.tr.bp", TR.bp);
    u32 seen = 0, caught = 0;
    for (u32 i = 0; g_dex_ok && i < DEX_SPECIES; ++i) { if (g_dex_state[i] >= 2) seen++; if (g_dex_state[i] >= 3) caught++; }
    pi("sv.tr.dex_ok", g_dex_ok);
    pi("sv.tr.seen", seen);
    pi("sv.tr.caught", caught);
    /* the party's icons on the card: own sprite, else the game's icon */
    for (u32 i = 0; i < 6; ++i) {
        const Mon* m = &g.mon[i];
        int v = g.party_ok && i < g.party_count && m->valid && !m->egg;
        pi(K("sv.tr.p", i, ".icon"), (s64)m->species * 100 + m->form);
        pi(K("sv.tr.p", i, ".game_icon"), v && !sprite_present(m->species));
    }
    (void)h; /* the clock and battery are bound in the manifest: a module cannot read @clock.* */
}
LOCAL void publish_pages(void) { publish_bag(); publish_dex(); publish_place(); }

/* ---------- module actions and extensions ---------- */

/* ---- v0.8.2: where does the game keep the move-list cursor? (a guided search) ----
 * The player opens "Luchar", puts the cursor on a move and taps that move's number on the
 * bottom screen. The first tap marks every aligned u32 in the sweep window that holds that
 * number (a 12 MiB bitmap, one bit per word); the second keeps the marked words that now hold the
 * new number (a list); every further tap narrows the list. One left: that is the cursor, and the
 * battle page highlights the move it is on. Reads only, like everything else. */
#define CUR_WORDS ((2 * SCAN_HALF) / 4)
#define CUR_MAX 4096
LOCAL u8 g_cur_bits[CUR_WORDS / 8];
LOCAL struct {
    int open, phase;          /* 0 nothing yet, 1 first sweep, 2 marked, 3 second sweep, 4 a list */
    u32 want, taps;
    u64 lo, pos;
    u64 cand[CUR_MAX]; u32 n; int overflow, none;
    int found; u64 addr; s32 idx; int idx_ok;
} CUR;

LOCAL volatile int g_cur_tap = -1;   /* a tap waits for sample(), which has the host */
LOCAL void cur_reset(void) { int open = CUR.open; memset(&CUR, 0, sizeof(CUR)); CUR.open = open; }

LOCAL void cur_filter(const EdenDsmodHostApi* h, u32 k, int any_small) {
    u32 m = 0;
    for (u32 i = 0; i < CUR.n; ++i) {
        u32 v;
        if (!rd(h, CUR.cand[i], &v, 4)) continue;
        if (any_small ? v <= 3 : v == k) CUR.cand[m++] = CUR.cand[i];
    }
    CUR.n = m;
    if (!m) { CUR.none = 1; CUR.phase = 0; return; }
    if (m == 1) { CUR.found = 1; CUR.addr = CUR.cand[0]; }
}

LOCAL void cur_mark(const EdenDsmodHostApi* h, u32 k) {
    if (k > 3 || !S.lo || CUR.phase == 1 || CUR.phase == 3) return;
    CUR.none = 0;
    if (CUR.found) {           /* found already: a tap that disagrees starts over */
        u32 v;
        if (rd(h, CUR.addr, &v, 4) && v == k) return;
        cur_reset();
    }
    CUR.want = k; CUR.taps++;
    if (CUR.phase == 0) { memset(g_cur_bits, 0, sizeof(g_cur_bits)); CUR.lo = S.lo & ~0xFFFull; CUR.pos = CUR.lo; CUR.phase = 1; return; }
    if (CUR.phase == 2) { CUR.pos = CUR.lo; CUR.n = 0; CUR.overflow = 0; CUR.phase = 3; return; }
    if (CUR.phase == 4) cur_filter(h, k, 0);
}

LOCAL void cur_step(const EdenDsmodHostApi* h, u64 budget) {
    if (CUR.phase != 1 && CUR.phase != 3) return;
    u64 hi = CUR.lo + 2 * SCAN_HALF, end = CUR.pos + budget;
    while (CUR.pos < end) {
        if (CUR.pos >= hi) {
            if (CUR.phase == 1) CUR.phase = 2;
            else {
                CUR.phase = 4;
                if (!CUR.n) { CUR.none = 1; CUR.phase = 0; }
                else if (CUR.n == 1) { CUR.found = 1; CUR.addr = CUR.cand[0]; }
            }
            return;
        }
        u64 page = CUR.pos & ~0xFFFull;
        const u8* p = h->get_read_pointer ? h->get_read_pointer(h->userdata, page, 0x1000) : 0;
        CUR.pos = page + 0x1000;
        if (!p) continue;
        u64 w0 = (page - CUR.lo) / 4;          /* first word of the page: a multiple of 1024 */
        u8* bits = g_cur_bits + w0 / 8;
        if (CUR.phase == 1) {
            for (u32 off = 0; off < 0x1000; off += 4) {
                u32 v; memcpy(&v, p + off, 4);
                if (v == CUR.want) bits[off >> 5] |= (u8)(1u << ((off >> 2) & 7));
            }
        } else {
            for (u32 b = 0; b < 128; ++b) {
                if (!bits[b]) continue;
                for (u32 j = 0; j < 8; ++j) {
                    if (!(bits[b] & (1u << j))) continue;
                    u32 off = (b * 8 + j) * 4, v;
                    memcpy(&v, p + off, 4);
                    if (v != CUR.want) continue;
                    if (CUR.n < CUR_MAX) CUR.cand[CUR.n++] = page + off; else CUR.overflow = 1;
                }
            }
        }
    }
}

LOCAL void publish_cursor(const EdenDsmodHostApi* h) {
    if (CUR.phase == 4 && CUR.n > 1 && !CUR.found) cur_filter(h, 0, 1);   /* a cursor is 0..3 */
    CUR.idx_ok = 0;
    if (CUR.found) {
        u32 v;
        if (rd(h, CUR.addr, &v, 4) && v <= 3) { CUR.idx = (s32)v; CUR.idx_ok = 1; }
    }
    pi("sv.cur.open", CUR.open);
    pi("sv.cur.ok", CUR.idx_ok);
    pi("sv.cur.idx", CUR.idx_ok ? CUR.idx : -1);
    Str d; s_init(&d, g_val, sizeof(g_val));
    if (CUR.found) {
        s_cat(&d, CUR.idx_ok ? TR("¡Encontrado! El ataque marcado se resalta abajo. Mándame una foto de esta línea.",
                                  "Found! The selected move is highlighted below. Please report this line.")
                             : TR("Encontrado, pero ahora no se lee (normal fuera del menú de ataques).",
                                  "Found, but not readable right now (normal outside the move menu)."));
    } else if (CUR.phase == 1 || CUR.phase == 3) {
        u64 done = CUR.pos > CUR.lo ? (CUR.pos - CUR.lo) * 100 / (2 * SCAN_HALF) : 0;
        s_cat(&d, TR("Buscando... no muevas el cursor (", "Searching... don't move the cursor (")); s_u(&d, done, 0); s_cat(&d, " %)");
    } else if (CUR.phase == 2) {
        s_cat(&d, TR("Bien. Ahora mueve el cursor a OTRO ataque y toca su número.",
                     "Good. Now move the cursor to ANOTHER move and tap its number."));
    } else if (CUR.phase == 4) {
        s_cat(&d, TR("Quedan ", "")); s_u(&d, CUR.n, 0); s_cat(&d, CUR.overflow ? "+" : "");
        s_cat(&d, TR(" posibles. Mueve el cursor a otro ataque y toca su número.",
                     " left. Move the cursor to another move and tap its number."));
    } else if (CUR.none) {
        s_cat(&d, TR("No ha salido nada. Toca REINICIAR y prueba otra vez (sin mover el cursor mientras busca).",
                     "Nothing found. Tap RESET and try again (don't move the cursor while it searches)."));
    } else {
        s_cat(&d, TR("Abre «Luchar», pon el cursor en un ataque y toca su número aquí.",
                     "Open \"Fight\", put the cursor on a move and tap its number here."));
    }
    pt("sv.cur.text", g_val);
    s_init(&d, g_val, sizeof(g_val));
    s_cat(&d, "toques "); s_u(&d, CUR.taps, 0); s_cat(&d, " | fase "); s_u(&d, (u64)CUR.phase, 0);
    if (CUR.phase == 4 || CUR.found) {
        u32 show = CUR.found ? 1 : CUR.n < 6 ? CUR.n : 6;
        for (u32 i = 0; i < show; ++i) {
            u64 a = CUR.found ? CUR.addr : CUR.cand[i];
            u32 v = 0; rd(h, a, &v, 4);
            s_cat(&d, " | 0x");
            for (int sh = 36; sh >= 0; sh -= 4) s_ch(&d, "0123456789ABCDEF"[(a >> sh) & 15]);
            s_ch(&d, '='); s_u(&d, v, 0);
        }
        if (S.lo) {
            s_cat(&d, " | rel "); u64 a = CUR.found ? CUR.addr : (CUR.n ? CUR.cand[0] : 0), base = S.lo + SCAN_HALF;
            u64 dlt = a > base ? a - base : base - a;
            s_ch(&d, a < base ? '-' : '+'); s_cat(&d, "0x");
            for (int sh = 28; sh >= 0; sh -= 4) s_ch(&d, "0123456789ABCDEF"[(dlt >> sh) & 15]);
        }
    }
    pt("sv.cur.diag", g_val);
}

LOCAL EdenDsmodBool on_action(void* inst, const char* action, int64_t arg) {
    (void)inst;
    #define IS(a) (strcmp_(action, a) == 0)
    if (IS("bag_pouch")) { if (arg >= 0 && arg < POUCHES) { ui.pouch = (u32)arg; ui.bpage = 0; } return EDEN_DSMOD_TRUE; }
    if (IS("bag_page")) { if (arg < 0 && ui.bpage > 0) ui.bpage--; else if (arg > 0) ui.bpage++; return EDEN_DSMOD_TRUE; }
    if (IS("dex_region")) { if (arg >= 0 && arg < DEX_REGIONS) { ui.region = (u32)arg; ui.dpage = 0; ui.dsel = 0; } return EDEN_DSMOD_TRUE; }
    if (IS("dex_page")) {
        s64 p = (s64)ui.dpage + arg;
        ui.dpage = p < 0 ? 0 : (u32)p; /* clamped against the page count when published */
        return EDEN_DSMOD_TRUE;
    }
    if (IS("pick")) {
        if (arg < 0 || (u64)arg >= g_npick) return EDEN_DSMOD_FALSE;
        g_chosen = g_pick[arg]; g_has_chosen = 1; g_pick_open_req = 0; g_sp_level = 0; g_sp_level_set = 0;
        /* a rival known for certain: learn the way to it (the path learner's ground truth) */
        if (L.state != 1 && L.runs < 12 && L.nch < CHAINS_MAX) learn_start(g_chosen.addr, g_chosen.ec);
        return EDEN_DSMOD_TRUE;
    }
    if (IS("pick_open")) { g_pick_open_req = 1; g_sp_open = 0; return EDEN_DSMOD_TRUE; }
    if (IS("sp_open")) { g_sp_open = 1; g_sp_letter = -1; g_sp_page = 0; return EDEN_DSMOD_TRUE; }
    if (IS("sp_back")) { if (g_sp_letter >= 0) g_sp_letter = -1; else g_sp_open = 0; return EDEN_DSMOD_TRUE; }
    if (IS("sp_letter")) { if (arg >= 0 && arg < 26) { g_sp_letter = (int)arg; g_sp_page = 0; } return EDEN_DSMOD_TRUE; }
    if (IS("sp_page")) { if (arg < 0 && g_sp_page > 0) g_sp_page--; else if (arg > 0) g_sp_page++; return EDEN_DSMOD_TRUE; }
    if (IS("sp_pick")) {
        static u16 lst[400];
        if (g_sp_letter < 0 || arg < 0) return EDEN_DSMOD_FALSE;
        u32 n = sp_list(g_sp_letter, lst, 400), at = g_sp_page * SP_PER_PAGE + (u32)arg;
        if (at >= n) return EDEN_DSMOD_FALSE;
        g_pick_open_req = 0; g_sp_open = 0; g_has_chosen = 1;
        /* that species among the Pokémon the sweep or the list holds: take it whole */
        for (u32 i = 0; i < S.nopp; ++i) if (S.opp[i].m.species == lst[at]) { g_chosen = S.opp[i]; return EDEN_DSMOD_TRUE; }
        for (u32 i = 0; i < g_npick; ++i) if (g_pick[i].m.species == lst[at] && g_pick[i].addr) { g_chosen = g_pick[i]; return EDEN_DSMOD_TRUE; }
        memset(&g_chosen, 0, sizeof(g_chosen));
        g_chosen.m.valid = 1; g_chosen.m.species = lst[at];
        g_sp_level = 0; g_sp_level_set = 0; /* unknown until you set it */
        return EDEN_DSMOD_TRUE;
    }
    if (IS("opp_lvl")) { s64 v = (s64)(g_sp_level ? g_sp_level : 50) + arg; g_sp_level = v < 1 ? 1 : v > 100 ? 100 : (u32)v; g_sp_level_set = 1; return EDEN_DSMOD_TRUE; }
    if (IS("sugg_toggle")) { g_sugg_open = !g_sugg_open; return EDEN_DSMOD_TRUE; }
    if (IS("tera_toggle")) { g_tera_on = !g_tera_on; return EDEN_DSMOD_TRUE; }
    if (IS("cur_open")) { CUR.open = !CUR.open; return EDEN_DSMOD_TRUE; }
    if (IS("cur_reset")) { cur_reset(); return EDEN_DSMOD_TRUE; }
    if (IS("cur_mark")) { if (arg >= 0 && arg <= 3) g_cur_tap = (int)arg; return EDEN_DSMOD_TRUE; }
    if (IS("map_rot")) { g_map_rot = !g_map_rot; return EDEN_DSMOD_TRUE; }
    if (IS("map_flip")) { g_map_swap = (g_map_swap + 1) & 7; return EDEN_DSMOD_TRUE; }
    if (IS("map_zoom")) { s64 z = (s64)g_map_zoom + (arg > 0 ? 1 : -1); g_map_zoom = z < 0 ? 0 : z > 3 ? 3 : (u32)z; return EDEN_DSMOD_TRUE; }
    if (IS("dex_sel")) { if (arg >= 0 && arg < DEX_PER_PAGE) ui.dsel = (u32)arg; return EDEN_DSMOD_TRUE; }
    #undef IS
    return EDEN_DSMOD_FALSE;
}
LOCAL void ext_configure(void* inst, const EdenDsmodHostExtensions* host) { (void)inst; g_hostx = host; }
LOCAL EdenDsmodBool load_image(void* inst, const EdenDsmodHostApi* host, const char* key, void* receiver,
                               EdenDsmodImageSink sink) {
    (void)inst;
    return load_image_impl(host, key, receiver, sink);
}
LOCAL const EdenDsmodModuleExtensions kExtensions = {
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH,
    ext_configure, on_action, load_image,
};
EXPORT const EdenDsmodModuleExtensions* eden_dsmod_get_extensions(uint32_t version, uint64_t abi_hash) {
    if (version != EDEN_DSMOD_EXT_VERSION || abi_hash != EDEN_DSMOD_EXT_HASH) return 0;
    return &kExtensions;
}

LOCAL void publish(const EdenDsmodHostApi* h) {
    P = h;
    pi("sv.build_ok", g.build_ok);
    pi("sv.keytable_ok", g.keytable_ok);
    pi("sv.party_ok", g.party_ok);
    pi("sv.check.mystatus", g.cross_ok);
    pi("sv.bad_reads", (s64)g.bad_reads);
    pi("sv.ready", g.build_ok && g.party_ok);
    pi("sv.home", g.build_ok && g.party_ok && g.ingame);
    pi("sv.pack.names", g_moves.count > 1 && g_species.count > 1);
    pi("sv.pack.data", g_personal_n > 0 && g_movedata_n > 0);
    pi("sv.game.types", g_gpers_n);
    pi("sv.game.moves", g_gmove_n);
    pi("sv.game.texts", g_gtext_n);
    pt("sv.game.lang", lang_folder(g.lang));
    pt("sv.game.problem", g_game_why);
    int have_data = (g_gpers_n || g_personal_n) && (g_gmove_n || g_movedata_n) && g_moves.count > 1;
    {
        Str s; s_init(&s, g_status_problem, sizeof(g_status_problem));
        s_cat(&s, TR("Sin datos del juego - ", "No game data - "));
        s_cat(&s, g_game_why[0] ? g_game_why : TR("motivo desconocido", "unknown reason"));
    }
    pt("sv.status",
       !g.build_ok      ? TR("Versión del juego no soportada (hace falta la 3.0.0 o 3.0.1)",
                             "Unsupported game version (3.0.0 or 3.0.1 needed)")
       : !g.keytable_ok ? TR("Esperando a la partida...", "Waiting for your save...")
       : !g.party_ok    ? TR("Leyendo equipo...", "Reading your team...")
       : g_game_step < GAME_STEPS ? TR("Leyendo datos del juego...", "Reading the game data...")
       : !have_data     ? g_status_problem
       : g.cross_ok     ? "OK"
                        : TR("OK - aviso: MyStatus no coincide", "OK - warning: MyStatus mismatch"));
    pi("sv.lang_en", lang_auto_en());
    pi("sv.en", g_en);
    pi("sv.warn", !(g.build_ok && g.keytable_ok && g.party_ok && g.cross_ok && have_data));
    if (g.money_ok) pi("sv.money", g.money);
    if (g.lp_ok) pi("sv.lp", g.lp);
    if (g.status_ok) { pt("sv.trainer", g.trainer); pi("sv.tid", g.tid); }

    u32 count = g.party_ok ? g.party_count : 0;
    pi("sv.party.count", count);
    for (u32 i = 0; i < 6; ++i) {
        const Mon* m = &g.mon[i];
        if (i < count && m->valid) {
            publish_card(i, m);
            publish_detail(i, m);
        } else {
            pi(K("sv.p", i, ".valid"), 0);
            pi(K("sv.d", i, ".valid"), 0);
        }
    }
    publish_battle();
    publish_cursor(h);
    publish_pages();
    publish_trainer(h);
}

/* ---------- ABI ---------- */
LOCAL int g_instance_token;

LOCAL EdenDsmodBool supports_build(const char* build_hex) {
    if (!build_hex) return EDEN_DSMOD_FALSE;
    for (size_t b = 0; b < sizeof(kBuilds) / sizeof(kBuilds[0]); ++b) {
        int ok = 1;
        for (int i = 0; i < 16; ++i) {
            char c = build_hex[i];
            if (c >= 'a' && c <= 'f') c = (char)(c - 32);
            if (c != kBuilds[b][i]) { ok = 0; break; }
        }
        if (ok) return EDEN_DSMOD_TRUE;
    }
    return EDEN_DSMOD_FALSE;
}

LOCAL void* create(const EdenDsmodHostApi* host, const char* config_json) {
    (void)config_json;
    if (!host || host->abi_version != EDEN_DSMOD_MODULE_ABI_VERSION ||
        host->abi_hash != EDEN_DSMOD_MODULE_ABI_HASH)
        return 0;
    memset(&g, 0, sizeof(g));
    /* Re-check the build from the host's 32-byte id (first 8 bytes, upper-case hex). */
    char hex[17];
    const char* digits = "0123456789ABCDEF";
    for (int i = 0; i < 8; ++i) {
        hex[2 * i] = digits[host->build_id[i] >> 4];
        hex[2 * i + 1] = digits[host->build_id[i] & 15];
    }
    hex[16] = 0;
    g.build_ok = supports_build(hex) == EDEN_DSMOD_TRUE;
    for (size_t i = 0; i < sizeof(kLists) / sizeof(kLists[0]); ++i) { kLists[i]->count = 0; kLists[i]->tries = 0; }
    g_personal_n = g_movedata_n = 0;
    g_gpers_n = g_gmove_n = 0; g_gtext_n = 0; g_species_internal = 0; g_game_step = 0; g_game_why[0] = 0;
    memset(g_gmove, 0, sizeof(g_gmove));
    memset(g_spr_known, 0, sizeof(g_spr_known));
    memset(&S, 0, sizeof(S));
    g_bin_tries = 0;
    if (host->log)
        host->log(host->userdata, EDEN_DSMOD_LOG_INFO,
                  g.build_ok ? "SV companion: supported build" : "SV companion: unsupported build");
    return &g_instance_token;
}

LOCAL void destroy(void* inst) { (void)inst; }

LOCAL void load_reference(const EdenDsmodHostApi* h) {
    /* The user: source may appear late: retry each missing piece a few times. */
    for (size_t i = 0; i < sizeof(kLists) / sizeof(kLists[0]); ++i)
        if (kLists[i]->count == 0 && kLists[i]->tries < 5) { kLists[i]->tries++; namelist_load(h, kLists[i]); }
    if ((!g_personal_n || !g_movedata_n) && g_bin_tries < 5) {
        g_bin_tries++;
        if (!g_personal_n) g_personal_n = load_bin(h, "user:personal.bin", g_personal, sizeof(g_personal));
        if (!g_movedata_n) g_movedata_n = load_bin(h, "user:movedata.bin", g_movedata, sizeof(g_movedata));
    }
}

LOCAL void sample(void* inst, const EdenDsmodHostApi* h) {
    (void)inst;
    lang_update(h);
    if (g.build_ok) {
        u64 now = h->get_tick ? h->get_tick(h->userdata) : g.next_refresh;
        if (now >= g.next_refresh) {
            refresh(h);
            if (g.status_ok && g_game_step < GAME_STEPS && game_trylock()) { load_game_step(h, g.lang); game_unlock(); }
            else if (g_game_step >= GAME_STEPS && (g.refreshes % 4) == 0) load_reference(h);
            pages_refresh(h);
            g.refreshes++;
            g.next_refresh = now + 15; /* 4 times a second at 60 Hz */
        }
        /* Heap sweep: brisk during a battle, slow on the field (it only refreshes the baseline). */
        if (g.party_ok && g.ingame) scan_step(h, S.mode == 1 ? (4ull << 20) : (S.ready ? (1ull << 20) : (2ull << 20)));
        if (g.party_ok && g.ingame && S.mode == 1) learn_step(h, 8ull << 20);
        if (g.party_ok && g.ingame) ns_step(h, S.mode == 1 ? 2ull << 20 : 1ull << 20); /* half pace on the field */
        if (g.party_ok && g.ingame && S.mode == 0) pos_search_step(h, 4ull << 20);
        if (g_cur_tap >= 0) { int k = g_cur_tap; g_cur_tap = -1; cur_mark(h, (u32)k); }
        if (g.party_ok && g.ingame) cur_step(h, 16ull << 20);
    }
    publish(h);
}

LOCAL void tick(void* inst, const EdenDsmodHostApi* h) { (void)inst; (void)h; }

LOCAL const EdenDsmodModuleApi kModule = {
    EDEN_DSMOD_MODULE_ABI_VERSION,
    sizeof(EdenDsmodModuleApi),
    0,
    EDEN_DSMOD_MODULE_ABI_HASH,
    SV_TITLE_ID,
#if SV_TITLE_ID == 0x01008F6008C5E000ULL
    "Pokemon Violet companion",
#else
    "Pokemon Scarlet companion",
#endif
    0, /* capabilities: none required */
    supports_build,
    create,
    destroy,
    sample,
    tick,
};

EXPORT const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t host_abi_version, uint64_t host_abi_hash) {
    if (host_abi_version != EDEN_DSMOD_MODULE_ABI_VERSION || host_abi_hash != EDEN_DSMOD_MODULE_ABI_HASH)
        return 0;
    return &kModule;
}
