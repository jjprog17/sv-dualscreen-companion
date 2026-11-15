/* Fake Eden Duo host: builds a synthetic guest memory with the game's real in-RAM layout
 * (pointer chain -> SCBlock key table -> XorShift-encrypted blocks -> PK8-encrypted PK9s),
 * loads the module .so through the C ABI and checks what it publishes.
 * Party Pokémon come from real .pk9 files (PKHeX's legality test set) given on the command line.
 * Usage: test_host <module.so> <build16> <file.pk9>... */
#include <dlfcn.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsmod_module_abi.h"
#include "dsmod_module_extensions.h"

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

#define MAIN_BASE 0x0000008000000000ULL
#define HEAP_BASE 0x0000002000000000ULL
#define HEAP_SIZE (4u << 20)
#define ROOT 0x47350D8ULL
#define LIVE_ROOT 0x4763C98ULL /* live party objects */
#define OW_ROOT 0x473ADE0ULL   /* overworld byte chain */
static u8 main_cell[8], main_live[8], main_ow[8];
static u8 heap[HEAP_SIZE];
static u64 heap_top = 0x1000;

static u64 halloc(size_t n) { u64 a = HEAP_BASE + heap_top; heap_top = (heap_top + n + 15) & ~15ull; return a; }
static u8* hp(u64 a) { return heap + (a - HEAP_BASE); }
static void w64(u64 a, u64 v) { memcpy(hp(a), &v, 8); }
static void w32(u8* p, u32 v) { memcpy(p, &v, 4); }
static void w16(u8* p, u16 v) { memcpy(p, &v, 2); }

#define MAIN_DATA 0x4800000ULL /* a little readable .data for the learnt battle paths */
static u8 main_data[0x2000];
static u8* main_at(u64 a, u64 n) {
    if (a >= MAIN_BASE + MAIN_DATA && a + n <= MAIN_BASE + MAIN_DATA + sizeof main_data) return main_data + (a - MAIN_BASE - MAIN_DATA);
    static const u64 roots[3] = {ROOT, LIVE_ROOT, OW_ROOT};
    u8* cells[3] = {main_cell, main_live, main_ow};
    for (int i = 0; i < 3; ++i)
        if (a >= MAIN_BASE + roots[i] && a + n <= MAIN_BASE + roots[i] + 8) return cells[i] + (a - MAIN_BASE - roots[i]);
    return NULL;
}
static EdenDsmodBool is_mapped(void* u, u64 a, u64 n) {
    (void)u;
    if (a >= MAIN_BASE) return main_at(a, n) != NULL;
    return a >= HEAP_BASE && a + n <= HEAP_BASE + HEAP_SIZE;
}
static EdenDsmodBool read_memory(void* u, u64 a, void* o, size_t n) {
    if (!is_mapped(u, a, n)) return 0;
    if (a >= MAIN_BASE) memcpy(o, main_at(a, n), n);
    else memcpy(o, hp(a), n);
    return 1;
}
static const u8* get_read_pointer(void* u, u64 a, size_t n) {
    (void)u;
    if ((a & ~0xFFFull) != ((a + n - 1) & ~0xFFFull)) return NULL; /* crosses a 4 KiB page */
    if (a >= MAIN_BASE) return main_at(a, n);
    return a >= HEAP_BASE && a + n <= HEAP_BASE + HEAP_SIZE ? hp(a) : NULL;
}
static u64 heap_begin(void* u) { (void)u; return HEAP_BASE; }
static u64 heap_end(void* u) { (void)u; return HEAP_BASE + HEAP_SIZE; }
static u64 tick_now;
static int64_t flag_sel, flag_dtab;
/* SV_FLAGBUG=1 reproduces the device: "@flag:" reads from sample() only see the fallback, while
 * the manifest's derived copies (sv.ui.*) carry the live value. */
static int64_t get_i64_(void* u, const char* n, int64_t fb) {
    (void)u;
    int bug = getenv("SV_FLAGBUG") != NULL;
    /* v0.6.7: the host's clock and battery (Eden Duo runtime 16 / 21 keys), unless SV_NOCLOCK */
    if (!getenv("SV_NOCLOCK")) {
        if (!strcmp(n, "@clock.year")) return 2026;
        if (!strcmp(n, "@clock.month")) return 11;
        if (!strcmp(n, "@clock.day")) return 12;
        if (!strcmp(n, "@clock.wday")) return 4;
        if (!strcmp(n, "@clock.hour")) return 10;
        if (!strcmp(n, "@clock.minute")) return 7;
        if (!strcmp(n, "@game.seconds")) return 4980;
        if (!strcmp(n, "@device.battery_present")) return 1;
        if (!strcmp(n, "@device.battery_pct")) return 18;
        if (!strcmp(n, "@device.charging")) return 0;
    }
    if (!strcmp(n, "sv.ui.sel")) return bug ? flag_sel : fb;
    if (!strcmp(n, "sv.ui.dtab")) return bug ? flag_dtab : fb;
    if (!strcmp(n, "@flag:sel")) return bug ? fb : flag_sel;
    if (!strcmp(n, "@flag:dtab")) return bug ? fb : flag_dtab;
    return fb;
}
static u64 get_tick(void* u) { (void)u; return tick_now; }
static void logf_(void* u, u32 l, const char* m) { (void)u; printf("  [log %u] %s\n", l, m); }

#define MAXP 8192
static char pname[MAXP][64]; static char ptext[MAXP][128]; static int64_t pval[MAXP]; static int pistext[MAXP]; static int np;
static int find(const char* n) { if (np >= MAXP) { fprintf(stderr, "too many keys\n"); exit(3); } for (int i = 0; i < np; ++i) if (!strcmp(pname[i], n)) return i; return -1; }
static void pub_i(void* u, const char* n, int64_t v) { (void)u; int i = find(n); if (i < 0) { i = np++; snprintf(pname[i], 64, "%s", n); } pval[i] = v; pistext[i] = 0; }
static void pub_t(void* u, const char* n, const char* v) { (void)u; int i = find(n); if (i < 0) { i = np++; snprintf(pname[i], 64, "%s", n); } snprintf(ptext[i], 128, "%s", v); pistext[i] = 1; }
static void pub_f(void* u, const char* n, double v) { (void)u; (void)n; (void)v; }
static int64_t geti(const char* n) { int i = find(n); return i < 0 ? -999 : pval[i]; }
static const char* gett(const char* n) { int i = find(n); return i < 0 ? "<none>" : ptext[i]; }

/* user: source for name lists */
static const char* user_dir;
static const char* romfs_dir; /* SV_ROMFS: a synthetic romfs (tools/make_test_romfs.py) */
static int romfs_reads;
static size_t read_romfs(void* u, const char* path, uint64_t off, void* out, size_t size) {
    (void)u;
    char full[1024];
    if (!strncmp(path, "user:", 5)) {
        if (!user_dir) return 0;
        snprintf(full, sizeof full, "%s/%s", user_dir, path + 5);
    } else if (!strchr(path, ':')) {
        if (!romfs_dir) return 0;
        snprintf(full, sizeof full, "%s/%s", romfs_dir, path);
        romfs_reads++;
    } else {
        return 0;
    }
    FILE* f = fopen(full, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f);
    if (!out) { fclose(f); return n; }
    if (off >= n) { fclose(f); return 0; }
    fseek(f, (long)off, SEEK_SET); size_t r = fread(out, 1, size, f); fclose(f); return r;
}

/* --- independent encryptors (inverse of what the module decrypts) --- */
static u32 xs(u32 s) { s ^= s << 2; s ^= s >> 15; s ^= s << 13; return s; }
static void sc_crypt(u32 key, u8* d, size_t n) {
    u32 st = key; for (int i = 0; i < __builtin_popcount(key); ++i) st = xs(st);
    for (size_t i = 0; i < n; ++i) { d[i] ^= (u8)(st >> ((i & 3) * 8)); if ((i & 3) == 3) st = xs(st); }
}
static const u8 pos[32 * 4] = {
    0,1,2,3, 0,1,3,2, 0,2,1,3, 0,3,1,2, 0,2,3,1, 0,3,2,1, 1,0,2,3, 1,0,3,2,
    2,0,1,3, 3,0,1,2, 2,0,3,1, 3,0,2,1, 1,2,0,3, 1,3,0,2, 2,1,0,3, 3,1,0,2,
    2,3,0,1, 3,2,0,1, 1,2,3,0, 1,3,2,0, 2,1,3,0, 3,1,2,0, 2,3,1,0, 3,2,1,0,
    0,1,2,3, 0,1,3,2, 0,2,1,3, 0,3,1,2, 0,2,3,1, 0,3,2,1, 1,0,2,3, 1,0,3,2 };
static void pk_crypt(u8* d, size_t n, u32 seed) {
    for (size_t i = 0; i + 1 < n; i += 2) { seed = 0x41C64E6Du * seed + 0x6073u; d[i] ^= (u8)(seed >> 16); d[i + 1] ^= (u8)(seed >> 24); }
}
static u16 checksum(const u8* d) { u16 s = 0; for (int i = 8; i < 0x148; i += 2) s += (u16)(d[i] | d[i + 1] << 8); return s; }
static void pk9_encrypt(const u8* plain, u8* out) {
    u32 pv; memcpy(&pv, plain, 4); u32 sv = (pv >> 13) & 31;
    memcpy(out, plain, 0x158);
    for (int i = 0; i < 4; ++i) memcpy(out + 8 + pos[sv * 4 + i] * 0x50, plain + 8 + i * 0x50, 0x50);
    pk_crypt(out + 8, 0x140, pv); pk_crypt(out + 0x148, 0x10, pv);
}

typedef struct { u32 key; u64 data; } Ent;
static int cmp_ent(const void* a, const void* b) { u32 x = ((const Ent*)a)->key, y = ((const Ent*)b)->key; return x < y ? -1 : x > y; }

static u64 put_block(u32 key, u8 type, const u8* payload, u32 size, int as_object) {
    static u8 buf[0x10000]; size_t n;
    buf[0] = type;
    if (as_object) { w32(buf + 1, size); memcpy(buf + 5, payload, size); n = 5 + size; }
    else { memcpy(buf + 1, payload, size); n = 1 + size; }
    sc_crypt(key, buf, n);
    u64 a = halloc(n); memcpy(hp(a), buf, n); return a;
}

/* rewrites the saved-position block in place (the game updates it while you walk) */
static u64 coords_at;
static void set_coords(float x, float y, float z) {
    u8 buf[17]; float v[3] = {x, y, z};
    buf[0] = 4; w32(buf + 1, 12); memcpy(buf + 5, v, 12);
    sc_crypt(0x708D1511u, buf, sizeof buf); memcpy(hp(coords_at), buf, sizeof buf);
}
static u8 img_px[1024 * 1024 * 4]; static unsigned img_w, img_h;
static void img_sink(void* r, uint32_t w, uint32_t h, const uint8_t* px, size_t n) {
    (void)r; img_w = w; img_h = h; if (n <= sizeof img_px) memcpy(img_px, px, n);
}
static const char* opp_path; /* --opp file.pk9: the wild Pokémon of the battle test */
static u64 put_pk(const u8* plain_pk) { u64 a = halloc(0x160); pk9_encrypt(plain_pk, hp(a)); return a; }

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s module.so BUILD16 a.pk9 [b.pk9 ...] [--user DIR]\n", argv[0]); return 2; }
    int npk = 0; const char* pk_paths[6];
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--user") && i + 1 < argc) { user_dir = argv[++i]; continue; }
        if (!strcmp(argv[i], "--opp") && i + 1 < argc) { opp_path = argv[++i]; continue; }
        if (npk < 6) pk_paths[npk++] = argv[i];
    }
    void* so = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!so) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
    EdenDsmodGetModuleFn get = (EdenDsmodGetModuleFn)dlsym(so, "eden_dsmod_get_module");
    const EdenDsmodModuleApi* m = get(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    if (!m) { printf("FAIL get_module\n"); return 1; }
    printf("module: %s title=%016llX supports_build(%s)=%u\n", m->name, (unsigned long long)m->title_id, argv[2], m->supports_build(argv[2]));

    /* --- party: real PK9s + synthetic party stats --- */
    u8 party[6 * 0x158 + 1]; memset(party, 0, sizeof party);
    u16 exp_species[6]; u16 exp_hp[6], exp_hpmax[6]; u8 exp_level[6];
    static u8 plain[6][0x158];
    for (int i = 0; i < npk; ++i) {
        FILE* f = fopen(pk_paths[i], "rb"); if (!f) { perror(pk_paths[i]); return 1; }
        u8 pk[0x158]; memset(pk, 0, sizeof pk); size_t n = fread(pk, 1, sizeof pk, f); fclose(f);
        if (n < 0x148) { printf("FAIL short pk9 %s\n", pk_paths[i]); return 1; }
        u16 ck_file = (u16)(pk[6] | pk[7] << 8);
        printf("pk9[%d] %s: file checksum %s\n", i, strrchr(pk_paths[i], '/') + 1, ck_file == checksum(pk) ? "valid" : "INVALID");
        exp_level[i] = (u8)(30 + 7 * i); exp_hpmax[i] = (u16)(100 + i); exp_hp[i] = (u16)(i == 1 ? 0 : 60 + i);
        w16(pk + 0x8A, exp_hp[i]); w16(pk + 6, checksum(pk));
        pk[0x148] = exp_level[i]; w16(pk + 0x14A, exp_hpmax[i]);
        exp_species[i] = (u16)(pk[8] | pk[9] << 8);
        memcpy(plain[i], pk, 0x158);
        pk9_encrypt(pk, party + i * 0x158);
    }
    party[6 * 0x158] = (u8)npk;

    u8 ms[0x68]; memset(ms, 0, sizeof ms); ms[0x07] = 7; /* Spanish */
    w32(ms, 123456789u); const char* tn = "Entrenador";
    for (int i = 0; tn[i]; ++i) w16(ms + 0x10 + 2 * i, (u16)tn[i]);

    /* --- blocks + sorted key table (with filler keys to exercise the binary search) --- */
    Ent e[64]; int ne = 0;
    u32 money = 987654, lp = 4321;
    e[ne++] = (Ent){0x3AA1A9ADu, put_block(0x3AA1A9ADu, 4, party, sizeof party, 1)};
    e[ne++] = (Ent){0x4F35D0DDu, put_block(0x4F35D0DDu, 10, (u8*)&money, 4, 0)};
    e[ne++] = (Ent){0xADB4FE17u, put_block(0xADB4FE17u, 10, (u8*)&lp, 4, 0)};
    e[ne++] = (Ent){0xE3E89BD1u, put_block(0xE3E89BD1u, 4, ms, sizeof ms, 1)};
    /* v0.5 pages: bag (MyItem9), Pokédex (Kitakami format), where the player is */
    static u8 items[3000 * 16]; memset(items, 0, sizeof items);
    #define ITEM(id, pouch, count) do { w32(items + (id) * 16, pouch); w32(items + (id) * 16 + 4, count); w32(items + (id) * 16 + 8, 4); } while (0)
    for (int i = 0; i < 20; ++i) ITEM(17 + i, 0, 1 + i);   /* 20 medicines: two pages */
    ITEM(1, 1, 3); ITEM(4, 1, 25);                        /* balls */
    ITEM(1255, 8, 1);                                     /* a key item */
    e[ne++] = (Ent){0x21C9BD44u, put_block(0x21C9BD44u, 4, items, sizeof items, 1)};
    static u8 zukan[1400 * 32]; memset(zukan, 0, sizeof zukan);
    for (int i = 1; i < 1400; ++i) { if (i % 3 == 0) w32(zukan + i * 32, 1); if (i % 3 != 2) w32(zukan + i * 32 + 4, 1); }
    e[ne++] = (Ent){0xF5D7C0E2u, put_block(0xF5D7C0E2u, 4, zukan, sizeof zukan, 1)};
    u32 loc = 5, fld = 0; float xyz[3] = {1234.5f, 80.0f, -2210.25f};
    e[ne++] = (Ent){0x19FC5B7Bu, put_block(0x19FC5B7Bu, 10, (u8*)&loc, 4, 0)};
    e[ne++] = (Ent){0xF17EB014u, put_block(0xF17EB014u, 10, (u8*)&fld, 4, 0)};
    coords_at = put_block(0x708D1511u, 4, (u8*)xyz, 12, 1);
    e[ne++] = (Ent){0x708D1511u, coords_at};
    u32 played[3] = {123, 5, 9}, bp = 2750;   /* v0.6.7 trainer card: play time, Blueberry Points */
    e[ne++] = (Ent){0xEDAFF794u, put_block(0xEDAFF794u, 4, (u8*)played, 12, 1)};
    e[ne++] = (Ent){0x66A33824u, put_block(0x66A33824u, 10, (u8*)&bp, 4, 0)};
    for (u32 k = 0; ne < 47; ++k) { u32 key = 0x01000000u * (k + 1) + 0x123u * k; u32 z = 0; e[ne++] = (Ent){key, put_block(key, 10, (u8*)&z, 4, 0)}; }
    qsort(e, ne, sizeof e[0], cmp_ent);
    u64 table = halloc(48 * ne); memset(hp(table), 0xCC, 48 * ne);
    for (int i = 0; i < ne; ++i) { w32(hp(table + 48 * i), e[i].key); w64(table + 48 * i + 8, e[i].data); }

    /* --- pointer chains: [main+ROOT]=A, [A+D8]=B, [B+0]=C, [C+0]=D, [D+30]=E, keytable at E+8 ---
     *     MyStatus: [B+8]=F, [F+B8]=G, [G+0]=Hh, data at Hh+40 (plain copy) */
    u64 A = halloc(0x100), B = halloc(0x100), C = halloc(0x100), D = halloc(0x100), E = halloc(0x100);
    u64 F = halloc(0x100), G = halloc(0x100), Hh = halloc(0x100);
    memcpy(main_cell, &A, 8);
    w64(A + 0xD8, B); w64(B + 0, C); w64(C + 0, D); w64(D + 0x30, E);
    w64(E + 0x08, table); w64(E + 0x10, table + 48 * ne);
    w64(B + 0x8, F); w64(F + 0xB8, G); w64(G + 0, Hh); memcpy(hp(Hh + 0x40), ms, sizeof ms);

    /* Boxes (SysBot B1S1: [[[[[[main+ROOT]+D8]+8]+B8]+30]+9D0]): two of the party in box 1. */
    u64 X = halloc(0x1000), boxes = halloc(32 * 30 * 0x158);
    w64(G + 0x30, X); w64(X + 0x9D0, boxes);
    { u8 t[0x158]; memcpy(t, plain[0], 0x158); w32(t, 0x0BADF00Du); pk9_encrypt(t, hp(boxes + 5 * 0x158)); }
    /* Live party objects: [[[[main+LIVE]+8]+(30+8i)]+30] -> PK9 (same Pokémon as the save). */
    u64 L1 = halloc(0x100), L2 = halloc(0x100);
    memcpy(main_live, &L1, 8); w64(L1 + 8, L2);
    u64 live_pk[6] = {0};
    for (int i = 0; i < npk; ++i) {
        u64 obj = halloc(0x100);
        if (i == 1) { /* device bug: live object with garbage level / max HP past the checksum */
            u8 t[0x158]; memcpy(t, plain[i], 0x158); t[0x148] = 9; w16(t + 0x14A, 5474);
            live_pk[i] = put_pk(t);
        } else live_pk[i] = put_pk(plain[i]);
        w64(L2 + 0x30 + 8 * i, obj); w64(obj + 0x30, live_pk[i]);
    }
    /* Overworld byte: [[[main+OW]+160]+E8]+28 == 0x11 on the field. */
    u64 W1 = halloc(0x200), W2 = halloc(0x100), W3 = halloc(0x100);
    memcpy(main_ow, &W1, 8); w64(W1 + 0x160, W2); w64(W2 + 0xE8, W3); hp(W3 + 0x28)[0] = 0x11;
    /* A Pokémon already in memory on the field (must never count as the rival). */
    u64 stale = 0;
    if (npk > 1) { u8 t[0x158]; memcpy(t, plain[1], 0x158); w32(t, 0x51A1E000u); stale = put_pk(t); }

    EdenDsmodHostApi h; memset(&h, 0, sizeof h);
    h.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION; h.struct_size = sizeof h; h.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    h.capabilities = EDEN_DSMOD_CAP_SOURCE_PREFIXES | EDEN_DSMOD_CAP_SOURCE_USER;
    h.title_id = m->title_id; h.main_base = MAIN_BASE; h.main_size = 0x5000000;
    for (int i = 0; i < 8; ++i) { unsigned v; sscanf(argv[2] + 2 * i, "%2x", &v); h.build_id[i] = (u8)v; }
    h.get_tick = get_tick; h.is_mapped = is_mapped; h.read_memory = read_memory; h.log = logf_;
    h.get_read_pointer = get_read_pointer; h.get_heap_begin = heap_begin; h.get_heap_end = heap_end;
    h.publish_i64 = pub_i; h.get_i64 = get_i64_; h.publish_text = pub_t; h.publish_f64 = pub_f; h.read_romfs = read_romfs;

    romfs_dir = getenv("SV_ROMFS");
    void* inst = m->create(&h, "{}");
    if (!inst) { printf("FAIL create\n"); return 1; }
    for (tick_now = 0; tick_now < 40; ++tick_now) m->sample(inst, &h);

    int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
    for (int t = 0; t < 200; ++t, ++tick_now) m->sample(inst, &h); /* let the game loader finish */
    printf("sv.status = %s\n", gett("sv.status"));
    printf("juego: tipos=%lld movs=%lld textos=%lld idioma=%s problema='%s' lecturas_romfs=%d\n",
           (long long)geti("sv.game.types"), (long long)geti("sv.game.moves"), (long long)geti("sv.game.texts"),
           gett("sv.game.lang"), gett("sv.game.problem"), romfs_reads);
    printf("sv.trainer = %s  tid=%lld  money=%lld  lp=%lld  mystatus_check=%lld\n", gett("sv.trainer"),
           (long long)geti("sv.tid"), (long long)geti("sv.money"), (long long)geti("sv.lp"), (long long)geti("sv.check.mystatus"));
    CHECK(geti("sv.ready") == 1, "sv.ready");
    CHECK(geti("sv.home") == 1, "sv.home (on the field)");
    CHECK(geti("sv.money") == money, "money");
    if (npk > 1) CHECK(geti("sv.p1.level") != 9 && geti("sv.p1.level") >= 1 && geti("sv.p1.level") <= 100 && geti("sv.p1.hpmax") < 714,
                       "implausible live stats ignored (Nv %lld PS max %lld)", (long long)geti("sv.p1.level"), (long long)geti("sv.p1.hpmax"));
    CHECK(geti("sv.lp") == lp, "lp");
    CHECK(geti("sv.tid") == 123456789 % 1000000, "tid");
    CHECK(!strcmp(gett("sv.trainer"), "Entrenador"), "trainer");
    CHECK(geti("sv.check.mystatus") == 1, "mystatus cross-check");
    CHECK(geti("sv.party.count") == npk, "party count");
    for (int i = 0; i < npk; ++i) {
        char k[64];
        #define G(f) (snprintf(k, sizeof k, "sv.p%d." f, i), geti(k))
        #define T(f) (snprintf(k, sizeof k, "sv.p%d." f, i), gett(k))
        printf("p%d: %-22s #%04lld lvl %lld  PS %lld/%lld  st='%s' banda=%lld\n", i, T("name"), (long long)G("species"),
               (long long)G("level"), (long long)G("hp"), (long long)G("hpmax"), T("st"), (long long)G("hpband"));
        CHECK(G("valid") == 1, "p%d valid", i);
        if (i != 1) { /* slot 1's live object has garbage stats: worked out from its experience instead */
            CHECK(G("level") == exp_level[i], "p%d level", i);
            CHECK(G("hp") == exp_hp[i] && G("hpmax") == exp_hpmax[i], "p%d hp", i);
        }
        long long nat = G("species");
        const char* fn = strrchr(pk_paths[i], '/') + 1;
        CHECK(nat == atoi(fn), "p%d national species %lld vs file name %d (internal %u)", i, nat, atoi(fn), exp_species[i]);
    }
    CHECK(geti("sv.bad_reads") == 0, "bad reads %lld", (long long)geti("sv.bad_reads"));

    /* Torn read: corrupt one byte of the encrypted party; the module must keep the last good copy. */
    u8* pblk = NULL; for (int i = 0; i < ne; ++i) if (e[i].key == 0x3AA1A9ADu) pblk = hp(e[i].data);
    pblk[5 + 0x40] ^= 0x5A;
    for (int t = 0; t < 40; ++t, ++tick_now) m->sample(inst, &h);
    CHECK(geti("sv.bad_reads") > 0 && geti("sv.party.count") == npk && geti("sv.p0.valid") == 1, "torn read handling");
    pblk[5 + 0x40] ^= 0x5A;

    /* Missing chain -> waiting, never garbage. */
    u64 zero = 0; memcpy(main_cell, &zero, 8);
    for (int t = 0; t < 40; ++t, ++tick_now) m->sample(inst, &h);
    CHECK(geti("sv.keytable_ok") == 0 && geti("sv.ready") == 0 && geti("sv.party.count") == 0, "null root handling");
    printf("after null root: %s\n", gett("sv.status"));
    memcpy(main_cell, &A, 8); /* the game is back */
    for (int t = 0; t < 40; ++t, ++tick_now) m->sample(inst, &h);
    /* Detail sheets: the module publishes one per slot (sv.d<slot>.*); the manifest picks. */
    if (user_dir) {
        char dn[16][80];
#define D(f) (dnx = (dnx + 1) & 15, snprintf(dn[dnx], 80, "sv.d%d." f, i), dn[dnx])
        int dnx = 0;
        for (int i = 0; i < npk; ++i) {
            printf("sel %d: %s Nv%lld  tipos %s/%s  tera %s  objeto %s  hab %s  nat %s  stats %lld/%lld/%lld/%lld/%lld/%lld\n", i,
                   gett(D("name")), (long long)geti(D("level")), gett(D("t1n")), geti(D("dual")) ? gett(D("t2n")) : "-",
                   gett(D("teran")), gett(D("item")), gett(D("ability")), gett(D("nature")),
                   (long long)geti(D("stat0")), (long long)geti(D("stat1")), (long long)geti(D("stat2")),
                   (long long)geti(D("stat3")), (long long)geti(D("stat4")), (long long)geti(D("stat5")));
            char k[64], line[600]; int o;
            for (int j = 0; j < 4; ++j) {
                snprintf(k, 64, "sv.d%d.m%d.valid", i, j); if (geti(k) != 1) continue;
                char a[64], b[64], c[64], d[64];
                snprintf(a, 64, "sv.d%d.m%d.name", i, j); snprintf(b, 64, "sv.d%d.m%d.tn", i, j); snprintf(c, 64, "sv.d%d.m%d.pp", i, j); snprintf(d, 64, "sv.d%d.m%d.cat", i, j);
                printf("     mov: %-20s %-10s cat=%lld PP %s\n", gett(a), gett(b), (long long)geti(d), gett(c));
            }
            const char* lists[3] = {"wk", "rs", "im"};
            for (int L = 0; L < 3; ++L) {
                snprintf(k, 64, "sv.d%d.%s.n", i, lists[L]); long long n = geti(k);
                o = snprintf(line, sizeof line, "     %s:", lists[L]);
                for (long long q = 0; q < n; ++q) { char a[64], b[64]; snprintf(a, 64, "sv.d%d.%s%lld.tn", i, lists[L], q); snprintf(b, 64, "sv.d%d.%s%lld.x", i, lists[L], q); o += snprintf(line + o, sizeof line - o, " %s %s,", gett(a), gett(b)); }
                printf("%s\n", line);
            }
            CHECK(geti(D("valid")) == 1, "detail %d published", i);
            CHECK(geti(D("typed")) == 1, "types known for %d", i);
        }
    }
    /* ---- battle: field baseline, then a wild Pokémon appears and the field byte drops ---- */
    CHECK(geti("sv.live_n") == npk, "live party read (%lld of %d)", (long long)geti("sv.live_n"), npk);
    CHECK(geti("sv.scan.ready") == 1, "baseline sweep done");
    CHECK(geti("sv.battle") == 0, "no battle on the field");
    hp(W3 + 0x28)[0] = 0x00; /* a menu: field byte off, nothing new in memory */
    for (int t = 0; t < 120; ++t, ++tick_now) m->sample(inst, &h);
    CHECK(geti("sv.battle") == 0, "a menu is not a battle (candidates %lld)", (long long)geti("sv.scan.opp"));
    hp(W3 + 0x28)[0] = 0x11;
    for (int t = 0; t < 60; ++t, ++tick_now) m->sample(inst, &h);
    if (opp_path) {
        u8 o[0x158]; memset(o, 0, sizeof o);
        FILE* f = fopen(opp_path, "rb"); size_t n = f ? fread(o, 1, sizeof o, f) : 0; if (f) fclose(f);
        CHECK(n >= 0x148, "opponent file");
        o[0x148] = 12; w16(o + 0x14A, 40); w16(o + 0x8A, 40); w16(o + 0x150, 999); w16(o + 6, checksum(o));
        hp(W3 + 0x28)[0] = 0x00;
        for (int t = 0; t < 40; ++t, ++tick_now) m->sample(inst, &h);
        u64 opp = put_pk(o); /* the battle's own Pokémon appears once the battle is on */
        for (int t = 0; t < 200; ++t, ++tick_now) m->sample(inst, &h);
        printf("combate: battle=%lld rival=%s Nv%lld PS %lld/%lld tipos %s/%s | yo=%s | %s | m0 %s (%s)\n",
               (long long)geti("sv.battle"), gett("sv.opp.name"), (long long)geti("sv.opp.level"),
               (long long)geti("sv.opp.hp"), (long long)geti("sv.opp.hpmax"), gett("sv.opp.t1n"),
               geti("sv.opp.dual") ? gett("sv.opp.t2n") : "-", gett("sv.me.name"), gett("sv.opp.speed"),
               gett("sv.me.m0.name"), gett("sv.me.m0.eff"));
        printf("  %s\n", gett("sv.scan.text"));
        CHECK(geti("sv.battle") == 1 && geti("sv.scan.opp") == 1, "battle detected with one rival (%lld)", (long long)geti("sv.scan.opp"));
        /* v0.6.8: a PK9 that is only new (on device: something that spawned nearby, Rufflet while
         * fighting Drifloon) is not shown by itself; after 2 s the list opens with it first */
        CHECK(geti("sv.opp.valid") != 1 && geti("sv.pick.open") == 1 && geti("sv.pick.0.valid") == 1,
              "a merely new PK9 is offered, not shown (valid %lld, list %lld)", (long long)geti("sv.opp.valid"), (long long)geti("sv.pick.open"));
        /* live HP: the rival takes damage */
        w16(o + 0x8A, 17); w16(o + 6, checksum(o)); pk9_encrypt(o, hp(opp));
        for (int t = 0; t < 30; ++t, ++tick_now) m->sample(inst, &h);
        CHECK(geti("sv.opp.hp") == 17, "rival HP live (%lld)", (long long)geti("sv.opp.hp"));
        {   /* v0.6.4 sheet: base stats with colour band, matchups with a level, Tera toggle */
            CHECK(geti("sv.opp.bs_ok") == 1 && geti("sv.opp.bs5") > 0, "rival base stats (%lld)", (long long)geti("sv.opp.bs5"));
            int bands = 0; for (int q = 0; q < 5; ++q) { char k[32]; snprintf(k, 32, "sv.opp.bs1.c%d", q); bands += (int)geti(k); }
            CHECK(bands == 1, "one colour band per stat (%d)", bands);
            CHECK(geti("sv.opp.tera_on") == 0, "Tera off for a new rival");
            long long wk0 = geti("sv.opp.wk.n"), lvsum = geti("sv.opp.wk0.lv0") + geti("sv.opp.wk0.lv1");
            CHECK(wk0 == 0 || lvsum == 1, "weakness level flag (%lld)", lvsum);
            EdenDsmodGetExtensionsFn gx2 = (EdenDsmodGetExtensionsFn)dlsym(so, "eden_dsmod_get_extensions");
            const EdenDsmodModuleExtensions* x2 = gx2 ? gx2(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH) : NULL;
            if (x2 && geti("sv.opp.tera_can")) {
                x2->on_action(inst, "tera_toggle", 0); m->sample(inst, &h);
                CHECK(geti("sv.opp.tera_on") == 1, "Tera toggled on");
                printf("tera %s: debil %lld resiste %lld inmune %lld\n", gett("sv.opp.teran"), (long long)geti("sv.opp.wk.n"),
                       (long long)geti("sv.opp.rs.n"), (long long)geti("sv.opp.im.n"));
                x2->on_action(inst, "tera_toggle", 0); m->sample(inst, &h);
                CHECK(geti("sv.opp.tera_on") == 0, "Tera toggled off");
            }
        }
        {   /* v0.6.7: a trainer sends out the next Pokémon; the old one stays in memory */
            u8 o2[0x158]; memcpy(o2, o, sizeof o2);
            o2[0] ^= 0x3C; o2[1] ^= 0x15; w16(o2 + 0x8A, 40); w16(o2 + 6, checksum(o2));
            u64 opp2 = put_pk(o2);
            for (int t = 0; t < 200; ++t, ++tick_now) m->sample(inst, &h);
            CHECK(geti("sv.opp.hp") == 17, "first rival still shown while nothing happens (%lld)", (long long)geti("sv.opp.hp"));
            w16(o2 + 0x8A, 33); w16(o2 + 6, checksum(o2)); pk9_encrypt(o2, hp(opp2)); /* the new one is hit */
            {   /* and the HP shown never jumps back to an old value */
                int back = 0;
                for (int t = 0; t < 120; ++t, ++tick_now) { m->sample(inst, &h); if (t > 20 && geti("sv.opp.hp") != 33) back++; }
                CHECK(back == 0, "HP steady after the hit (%d jumps)", back);
            }
            CHECK(geti("sv.opp.hp") == 33, "the Pokémon taking damage is shown (%lld)", (long long)geti("sv.opp.hp"));
            w16(o2 + 0x8A, 0); w16(o2 + 6, checksum(o2)); pk9_encrypt(o2, hp(opp2));   /* it faints */
            for (int t = 0; t < 30; ++t, ++tick_now) m->sample(inst, &h);
            CHECK(geti("sv.opp.hp") == 17, "fainted: back to the one still standing (%lld)", (long long)geti("sv.opp.hp"));
            memset(hp(opp2), 0, 8);                                                     /* and is gone */
            for (int t = 0; t < 30; ++t, ++tick_now) m->sample(inst, &h);
        }
        /* our HP live: slot 0 takes damage in its live object only */
        { u8 t[0x158]; memcpy(t, plain[0], 0x158); w16(t + 0x8A, 5); w16(t + 6, checksum(t)); pk9_encrypt(t, hp(live_pk[0])); }
        for (int t = 0; t < 30; ++t, ++tick_now) m->sample(inst, &h);
        CHECK(geti("sv.p0.hp") == 5, "own HP from the live object (%lld)", (long long)geti("sv.p0.hp"));
        if (getenv("SV_DUMP_BATTLE")) { /* leave the battle running for the dump below */ }
        else {
            hp(W3 + 0x28)[0] = 0x11;
            for (int t = 0; t < 90; ++t, ++tick_now) m->sample(inst, &h);
            CHECK(geti("sv.battle") == 0, "battle over back on the field");
            /* Second battle, the Scarlet/Violet way (seen on device): the wild Pokémon was already
             * walking about on the field, so no new PK9 appears; its bytes change in battle. */
            u8 w2[0x158]; memcpy(w2, o, sizeof w2);
            w2[0] ^= 0x5A; w2[1] ^= 0x33; /* another encryption constant: another Pokémon */
            w16(w2 + 0x8A, 40); w2[0x148] = 0; w16(w2 + 0x14A, 4633); /* field copy: no party stats */
            w32(w2 + 0x10, 100000); /* experience: around level 46 */
            w16(w2 + 6, checksum(w2));
            u64 spawn = put_pk(w2);
            u64 list1 = halloc(0x40); w64(list1 + 0x10, spawn);          /* the field's spawn list */
            { u64 v = list1; memcpy(main_data + 0x1200, &v, 8); }
            u64 bt1 = halloc(0x80), bt2 = halloc(0x80); w64(bt1 + 0x40, bt2); /* the battle */
            for (int t = 0; t < 400; ++t, ++tick_now) m->sample(inst, &h); /* field sweeps see it */
            hp(W3 + 0x28)[0] = 0x06;
            for (int t = 0; t < 60; ++t, ++tick_now) m->sample(inst, &h);
            CHECK(geti("sv.pick.open") == 0, "no list in the first seconds (it would flash on and off)");
            for (int t = 0; t < 180; ++t, ++tick_now) m->sample(inst, &h);
            CHECK(geti("sv.opp.valid") != 1 && geti("sv.pick.open") == 1, "nothing guessed yet: the picker is offered (%lld)", (long long)geti("sv.scan.opp"));
            w64(bt2 + 0x30, spawn); { u64 v = bt1; memcpy(main_data + 0x100, &v, 8); }
            w16(w2 + 0x8A, 31); w16(w2 + 6, checksum(w2)); pk9_encrypt(w2, hp(spawn)); /* it takes a hit */
            for (int t = 0; t < 700; ++t, ++tick_now) m->sample(inst, &h); /* long enough to learn the way */
            printf("  %s\n", gett("sv.learn.text"));
            printf("  %s\n", gett("sv.scan.text"));
            CHECK(geti("sv.battle") == 1 && geti("sv.opp.hp") == 31, "the field Pokémon whose bytes changed is the rival (hp %lld)", (long long)geti("sv.opp.hp"));
            printf("  rival calculado: Nv %lld PS %lld/%lld | %s\n", (long long)geti("sv.opp.level"), (long long)geti("sv.opp.hp"),
                   (long long)geti("sv.opp.hpmax"), gett("sv.opp.speed"));
            CHECK(geti("sv.opp.lvl_ok") == 1 && geti("sv.opp.hpmax") > 0 && geti("sv.opp.hpmax") < 714, "rival level and HP from its experience and base stats");
            {   /* v0.6.6: the rival's PK9 unreadable for a moment (seen on device as flicker) */
                u8 save[8]; memcpy(save, hp(spawn), 8); memset(hp(spawn), 0, 8);
                int flips = 0, last = (int)geti("sv.opp.valid");
                for (int t = 0; t < 60; ++t, ++tick_now) {
                    m->sample(inst, &h);
                    if ((int)geti("sv.opp.valid") != last || geti("sv.pick.open")) flips++;
                    last = (int)geti("sv.opp.valid");
                }
                memcpy(hp(spawn), save, 8);
                for (int t = 0; t < 30; ++t, ++tick_now) m->sample(inst, &h);
                CHECK(flips == 0 && geti("sv.opp.valid") == 1 && geti("sv.opp.hp") == 31, "rival held through a read gap (%d flips)", flips);
            }
            for (int k = 0; k < 2; ++k) {
                char a[32], b2[32], c[32]; snprintf(a, 32, "sv.sg%d.name", k); snprintf(b2, 32, "sv.sg%d.l0", k);
                snprintf(c, 32, "sv.sg%d.l1", k);
                printf("  equipo %d: %s | %s | %s\n", k, gett(a), gett(b2), gett(c));
            }
            CHECK(geti("sv.sugg.n") >= 1 && geti("sv.sg0.valid") == 1, "switch suggestions");
            hp(W3 + 0x28)[0] = 0x11; w64(bt2 + 0x30, 0); /* battle over: its structure is cleared */
            for (int t = 0; t < 90; ++t, ++tick_now) m->sample(inst, &h);
            printf("  %s\n", gett("sv.learn.text"));
            CHECK(strstr(gett("sv.learn.text"), "4800100>") != NULL, "a battle path learnt");
            /* Third battle right away, against another field Pokémon, while the game writes the
             * second rival's result back into its field copy: the old rival must not come back. */
            u8 w3[0x158]; memcpy(w3, w2, sizeof w3); w3[0] ^= 0x77; w3[2] ^= 0x11; w16(w3 + 0x8A, 50); w16(w3 + 6, checksum(w3));
            u64 spawn3 = put_pk(w3);
            w16(w2 + 0x8A, 12); w16(w2 + 6, checksum(w2)); pk9_encrypt(w2, hp(spawn)); /* write-back */
            hp(W3 + 0x28)[0] = 0x06;
            for (int t = 0; t < 60; ++t, ++tick_now) m->sample(inst, &h);
            CHECK(geti("sv.battle") == 0 || geti("sv.opp.hp") != 12, "the previous rival does not come back (hp %lld)", (long long)geti("sv.opp.hp"));
            hp(W3 + 0x28)[0] = 0x11;
            for (int t = 0; t < 400; ++t, ++tick_now) m->sample(inst, &h);
            hp(W3 + 0x28)[0] = 0x06;
            for (int t = 0; t < 60; ++t, ++tick_now) m->sample(inst, &h);
            w16(w3 + 0x8A, 44); w16(w3 + 6, checksum(w3)); pk9_encrypt(w3, hp(spawn3));
            for (int t = 0; t < 120; ++t, ++tick_now) m->sample(inst, &h);
            CHECK(geti("sv.battle") == 1 && geti("sv.opp.hp") == 44, "next battle finds the new rival (hp %lld)", (long long)geti("sv.opp.hp"));
            hp(W3 + 0x28)[0] = 0x11;
            for (int t = 0; t < 400; ++t, ++tick_now) m->sample(inst, &h);
            /* A Tera Pokémon: its field copy never changes, only the learnt battle path finds it. */
            u8 w4[0x158]; memcpy(w4, w2, sizeof w4); w4[0] ^= 0x2C; w4[3] ^= 0x41; w16(w4 + 0x8A, 77); w16(w4 + 6, checksum(w4));
            u64 spawn4 = put_pk(w4);
            for (int t = 0; t < 400; ++t, ++tick_now) m->sample(inst, &h);
            printf("  %s\n", gett("sv.learn.text"));
            CHECK(strstr(gett("sv.learn.text"), "COMBATE 4801200>10>0") == NULL, "the field spawn list told apart");
            hp(W3 + 0x28)[0] = 0x06; w64(bt2 + 0x30, spawn4);
            for (int t = 0; t < 60; ++t, ++tick_now) m->sample(inst, &h);
            printf("  %s\n", gett("sv.learn.text"));
            CHECK(geti("sv.battle") == 1 && geti("sv.opp.hp") == 77, "Tera-style rival found through the learnt path (hp %lld)", (long long)geti("sv.opp.hp"));
            hp(W3 + 0x28)[0] = 0x11; w64(bt2 + 0x30, 0);
            for (int t = 0; t < 90; ++t, ++tick_now) m->sample(inst, &h);
            /* After a battle the field byte flickers (17 <-> 19 seen on device): the baseline
             * sweep must still get done. */
            for (int t = 0; t < 900; ++t, ++tick_now) { hp(W3 + 0x28)[0] = (t / 35) % 2 ? 0x13 : 0x11; m->sample(inst, &h); }
            hp(W3 + 0x28)[0] = 0x11;
            for (int t = 0; t < 30; ++t, ++tick_now) m->sample(inst, &h);
            CHECK(geti("sv.scan.ready") == 1, "baseline finishes despite a flickering field byte");
        }
    }
    (void)stale;

    /* ---- v0.5 pages through the module's actions ---- */
    {
        EdenDsmodGetExtensionsFn gx = (EdenDsmodGetExtensionsFn)dlsym(so, "eden_dsmod_get_extensions");
        const EdenDsmodModuleExtensions* x = gx ? gx(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH) : NULL;
        CHECK(x && x->on_action && x->load_image && x->configure, "module extensions exported");
        if (x) {
            for (int t = 0; t < 60; ++t, ++tick_now) m->sample(inst, &h);
            CHECK(geti("sv.bag.ok") == 1 && geti("sv.bag.n") == 20 && geti("sv.bag.pages") == 2, "bag: 20 medicines on 2 pages (%lld)", (long long)geti("sv.bag.n"));
            printf("mochila: %s x%lld, %s x%lld | balls=%lld clave=%lld\n", gett("sv.bag.0.name"), (long long)geti("sv.bag.0.count"),
                   gett("sv.bag.1.name"), (long long)geti("sv.bag.1.count"), (long long)geti("sv.bag.pc1"), (long long)geti("sv.bag.pc8"));
            x->on_action(inst, "bag_page", 1); m->sample(inst, &h);
            CHECK(geti("sv.bag.page") == 2 && geti("sv.bag.5.valid") == 1 && geti("sv.bag.6.valid") == 0, "bag page 2 has 6 items");
            x->on_action(inst, "bag_pouch", 1); m->sample(inst, &h);
            CHECK(geti("sv.bag.n") == 2 && geti("sv.bag.page") == 1, "bag: balls pouch");
            CHECK(geti("sv.dex.ok") == 1 && geti("sv.dex.total0") > 300, "dex: Paldea list (%lld)", (long long)geti("sv.dex.total0"));
            printf("pokédex: Paldea %lld/%lld/%lld Kitakami %lld Arándano %lld | #1 %s st=%lld | ficha: %s, %s, %s %s | %s\n",
                   (long long)geti("sv.dex.caught0"), (long long)geti("sv.dex.seen0"), (long long)geti("sv.dex.total0"),
                   (long long)geti("sv.dex.total1"), (long long)geti("sv.dex.total2"), gett("sv.dx.name"), (long long)geti("sv.dex.0.st"),
                   gett("sv.dx.cat"), gett("sv.dx.t1n"), gett("sv.dx.height"), gett("sv.dx.weight"), gett("sv.dx.text"));
            CHECK(geti("sv.dex.seen0") > 0 && geti("sv.dex.caught0") > 0, "dex: seen/caught counted");
            x->on_action(inst, "dex_page", 1); x->on_action(inst, "dex_sel", 4); m->sample(inst, &h);
            CHECK(geti("sv.dx.num") == 25 && geti("sv.dex.4.sel") == 1, "dex: page 2, entry 25 selected (%lld)", (long long)geti("sv.dx.num"));
            printf("mapa: %s (%s) x=%lld z=%lld\n", gett("sv.map.place"), gett("sv.map.field"), (long long)geti("sv.map.x"), (long long)geti("sv.map.z"));
            CHECK(geti("sv.map.ok") == 1 && geti("sv.map.x") == 1234 && geti("sv.map.z") == -2210, "map: place and saved position");
            CHECK(x->on_action(inst, "nonsense", 0) == EDEN_DSMOD_FALSE, "unknown action declined");
            /* the game's own pictures, decoded from (synthetic) BNTX files in the romfs */
            if (romfs_dir) {
                x->configure(inst, NULL);
                const char* keys[4] = {"module:item/17", "module:item/18", "module:poke/2500", "module:item/19"};
                const u8 want[4][3] = {{220, 30, 30}, {0, 255, 0}, {30, 60, 220}, {201, 101, 51}};
                for (int k = 0; k < 4; ++k) {
                    img_w = img_h = 0;
                    EdenDsmodBool ok = x->load_image(inst, &h, keys[k], NULL, img_sink);
                    const u8* p = img_px + ((img_h / 2) * img_w + img_w / 2) * 4;
                    printf("imagen %s: %s %ux%u centro %u,%u,%u,%u\n", keys[k], ok ? "ok" : "FALLO", img_w, img_h, p[0], p[1], p[2], p[3]);
                    CHECK(ok && img_w > 0 && abs(p[0] - want[k][0]) < 8 && abs(p[1] - want[k][1]) < 8 && abs(p[2] - want[k][2]) < 8 && p[3] == 255,
                          "game picture %s decoded", keys[k]);
                }
                CHECK(x->load_image(inst, &h, "module:item/999", NULL, img_sink) == EDEN_DSMOD_FALSE, "missing picture refused");
                img_w = 0;
                CHECK(x->load_image(inst, &h, "module:map/0", NULL, img_sink) == EDEN_DSMOD_TRUE && img_w == 1020,
                      "region map built from its 100 tiles: %ux%u", img_w, img_h);
            }
            /* the map marker: saved position first, then the live copy once it moves */
            {
                float f3[3] = {1234.5f, 80.0f, -2210.25f};
                /* v0.6.8: 40 copies that never move come first in memory (the device showed
                 * "copias 24", the cap then), a Pokémon standing on the same spot walks off
                 * elsewhere, and the player's position is held twice */
                u64 decoys = halloc(40 * 16);
                for (int k = 0; k < 40; ++k) memcpy(hp(decoys + k * 16), f3, 12);
                u64 npc = halloc(32); memcpy(hp(npc + 4), f3, 12);
                u64 posa = halloc(64), posb = halloc(32);
                memcpy(hp(posa + 16), f3, 12); memcpy(hp(posb + 8), f3, 12);
                /* v0.7.5: the camera's position, a few units behind you, moves too */
                u64 cam = halloc(32); memcpy(hp(cam), f3, 12);
#define WALK_TO(X, Z) do { float o_[3]; memcpy(o_, hp(posa + 16), 12); float n_[3] = {(X), 80.0f, (Z)}; \
        float dx_ = n_[0] - o_[0], dz_ = n_[2] - o_[2], l_ = sqrtf(dx_ * dx_ + dz_ * dz_); \
        float c_[3] = {n_[0] - (l_ > 0 ? dx_ / l_ * 6 : 0), 80.0f, n_[2] - (l_ > 0 ? dz_ / l_ * 6 : 0)}; \
        memcpy(hp(posa + 16), n_, 12); memcpy(hp(posb + 8), n_, 12); memcpy(hp(cam), c_, 12); } while (0)
                for (int t = 0; t < 1000; ++t, ++tick_now) m->sample(inst, &h);
                printf("mapa: marcador %lld,%lld directo=%lld guardada=%lld copias=%lld\n", (long long)geti("sv.map.mx"),
                       (long long)geti("sv.map.my"), (long long)geti("sv.map.live"), (long long)geti("sv.map.saved"), (long long)geti("sv.map.cands"));
                CHECK(geti("sv.map.on") == 1 && geti("sv.map.saved") == 1 && geti("sv.map.mx") == 222 && geti("sv.map.my") == 592,
                      "marker at the saved position");
                f3[0] = 1500.0f; f3[2] = -2000.0f; WALK_TO(1500.0f, -2000.0f); /* the player walks */
                { float g3[3] = {1100.0f, 80.0f, -2400.0f}; memcpy(hp(npc + 4), g3, 12); }               /* so does the Pokémon */
                for (int t = 0; t < 30; ++t, ++tick_now) m->sample(inst, &h);
                printf("mapa: tras moverse %lld,%lld directo=%lld X=%lld\n", (long long)geti("sv.map.mx"), (long long)geti("sv.map.my"),
                       (long long)geti("sv.map.live"), (long long)geti("sv.map.px"));
                CHECK(geti("sv.map.live") == 1 && geti("sv.map.px") == 1500 && geti("sv.map.mx") == 270, "marker follows the live position");
                /* walking east, then north: the arrow turns that way */
                f3[0] = 1520.0f; WALK_TO(1520.0f, -2000.0f);
                for (int t = 0; t < 120; ++t, ++tick_now) m->sample(inst, &h);
                CHECK(geti("sv.map.arrow") == 1 && geti("sv.map.heading") == 90 && strcmp(gett("sv.map.dir"), "Este") == 0,
                      "facing east (%lld %s)", (long long)geti("sv.map.heading"), gett("sv.map.dir"));
                f3[2] = -2030.0f; WALK_TO(1520.0f, -2030.0f);
                for (int t = 0; t < 120; ++t, ++tick_now) m->sample(inst, &h);
                CHECK(geti("sv.map.heading") == 0 && strcmp(gett("sv.map.dir"), "Norte") == 0, "facing north (%lld)", (long long)geti("sv.map.heading"));
                /* turning right round (north to south-west) is believed after two steps that way */
                WALK_TO(1510.0f, -2020.0f);
                for (int t = 0; t < 60; ++t, ++tick_now) m->sample(inst, &h);
                CHECK(geti("sv.map.heading") == 0, "one step back is not yet a turn (%lld)", (long long)geti("sv.map.heading"));
                f3[0] = 1500.0f; f3[2] = -2010.0f; WALK_TO(1500.0f, -2010.0f);
                for (int t = 0; t < 120; ++t, ++tick_now) m->sample(inst, &h);
                CHECK(geti("sv.map.heading") == 225 && strcmp(gett("sv.map.dir"), "Suroeste") == 0, "facing south-west (%lld)", (long long)geti("sv.map.heading"));
                /* v0.7.2: the map's place names, at the zoom they are meant for */
                printf("mapa nombres: %lld | 0=%s (%lld,%lld) | %s\n", (long long)geti("sv.map.labels"), gett("sv.map.l0.t"),
                       (long long)geti("sv.map.l0.x"), (long long)geti("sv.map.l0.y"), gett("sv.map.diag"));
                CHECK(geti("sv.map.labels") == 2 && geti("sv.map.l0.ok") == 1 && !strcmp(gett("sv.map.l0.t"), "Pueblo Artesanía")
                      && geti("sv.map.l1.ok") == 0, "place names on the map (zoom-in only one hidden)");
                /* heading up: the round picture turns, your arrow points up, the compass turns back */
                x->on_action(inst, "map_rot", 0);
                for (int t = 0; t < 30; ++t, ++tick_now) m->sample(inst, &h);
                {
                    long long code = geti("sv.map.img");
                    CHECK(geti("sv.map.rot") == 1 && ((code >> 23) & 1) && ((code >> 24) & 0x7F) == 45 && geti("sv.map.north") == 135
                          && geti("sv.map.heading") == 0, "heading up (code %llx, north %lld)", code, (long long)geti("sv.map.north"));
                    char key[48]; snprintf(key, sizeof key, "module:map/%lld", code);
                    img_w = 0;
                    CHECK(x->load_image(inst, &h, key, NULL, img_sink) == EDEN_DSMOD_TRUE && img_w == 768 && img_px[3] == 0
                          && img_px[((u64)384 * 768 + 384) * 4 + 3] == 255, "round turned picture (%d px)", img_w);
                    long long mx = geti("sv.map.mx"), my = geti("sv.map.my");
                    CHECK(mx > 370 && mx < 530 && my > 370 && my < 530, "you near the middle (%lld,%lld)", mx, my);
                    /* a place name ahead of you (south-west of the marker would be up) */
                    printf("girado: nombre %s ok=%lld (%lld,%lld)\n", gett("sv.map.l0.t"), (long long)geti("sv.map.l0.ok"),
                           (long long)geti("sv.map.l0.x"), (long long)geti("sv.map.l0.y"));
                }
                /* v0.7.5 (device: the map flipped between north and south up and the marker jumped):
                 * walk north with heading up while the game keeps updating the saved position, the
                 * followed copies drop out for a moment, and the camera's copy trails behind */
                {
                    float px = 1500.0f, pz = -2010.0f;
                    int rot_off = 0, saved_shown = 0, wrong_north = 0, jumps = 0;
                    long long lastx = geti("sv.map.px"), lastz = geti("sv.map.pz");
                    for (int k = 0; k < 60; ++k) {
                        pz -= 2.0f; WALK_TO(px, pz);
                        if (k % 5 == 4) set_coords(px, 80.0f, pz + 1.5f);      /* the block lags a little */
                        float nanv[3] = {__builtin_nanf(""), 0, 0};
                        if (k == 30 || k == 31) memcpy(hp(posa + 16), nanv, 12); /* one copy drops out */
                        if (k == 40 || k == 41) { memcpy(hp(posa + 16), nanv, 12); memcpy(hp(posb + 8), nanv, 12); } /* both */
                        for (int t = 0; t < 15; ++t, ++tick_now) {
                            m->sample(inst, &h);
                            if (!geti("sv.map.rot")) rot_off++;
                            if (geti("sv.map.saved")) saved_shown++;
                            if (k >= 8 && geti("sv.map.north") != 0) wrong_north++;
                            long long nx = geti("sv.map.px"), nz = geti("sv.map.pz");
                            if (llabs(nx - lastx) > 8 || llabs(nz - lastz) > 8) jumps++; /* steps are 2 units; a hold may catch up 3 */
                            lastx = nx; lastz = nz;
                        }
                    }
                    printf("mapa al caminar: giro apagado %d, guardada %d, norte mal %d, saltos %d | %s\n", rot_off, saved_shown,
                           wrong_north, jumps, gett("sv.map.diag"));
                    CHECK(rot_off == 0 && saved_shown == 0 && wrong_north == 0 && jumps == 0,
                          "walking north with the saved position updating: steady map");
                    CHECK(geti("sv.map.px") == 1500 && geti("sv.map.pz") == -2130 && geti("sv.map.heading") == 0,
                          "on the player, facing up (%lld,%lld)", (long long)geti("sv.map.px"), (long long)geti("sv.map.pz"));
                    /* a saved position far away (a fast travel the copies have not followed): search again */
                    set_coords(3000.0f, 80.0f, -3000.0f);
                    float far3[3] = {3000.0f, 80.0f, -3000.0f};
                    u64 there = halloc(32); memcpy(hp(there), far3, 12);
                    for (int t = 0; t < 400; ++t, ++tick_now) m->sample(inst, &h);
                    far3[2] = -3010.0f; memcpy(hp(there), far3, 12); memcpy(hp(there), far3, 12);
                    for (int t = 0; t < 60; ++t, ++tick_now) m->sample(inst, &h);
                    printf("mapa tras viajar: %lld,%lld | %s\n", (long long)geti("sv.map.px"), (long long)geti("sv.map.pz"), gett("sv.map.diag"));
                    CHECK(geti("sv.map.px") == 3000 && geti("sv.map.pz") == -3010, "after a fast travel the new copy is followed");
                    WALK_TO(1500.0f, -2130.0f);
                }
                x->on_action(inst, "map_rot", 0);
                for (int t = 0; t < 10; ++t, ++tick_now) m->sample(inst, &h);
                CHECK(geti("sv.map.rot") == 0, "north up again");
                /* v0.8.2: the guided search for the move-list cursor. The cursor is one word among
                 * decoys: words that happen to follow the first taps, then stop */
                {
                    u64 cur = halloc(64), dec1 = halloc(64), dec2 = halloc(64);
                    u32 v;
#define SETW(a, x) do { v = (x); memcpy(hp(a), &v, 4); } while (0)
#define RUN(n) for (int t = 0; t < (n); ++t, ++tick_now) m->sample(inst, &h)
                    x->on_action(inst, "cur_open", 0);
                    SETW(cur, 1); SETW(dec1, 1); SETW(dec2, 1);
                    RUN(2);
                    x->on_action(inst, "cur_mark", 1); RUN(60);
                    CHECK(geti("sv.cur.open") == 1 && geti("sv.cur.ok") == 0, "cursor search: first mark");
                    SETW(cur, 3); SETW(dec1, 3); SETW(dec2, 7);
                    x->on_action(inst, "cur_mark", 3); RUN(60);
                    printf("cursor tras 2 toques: %s | %s\n", gett("sv.cur.text"), gett("sv.cur.diag"));
                    SETW(cur, 0); SETW(dec1, 2);
                    x->on_action(inst, "cur_mark", 0); RUN(20);
                    printf("cursor tras 3 toques: %s | %s\n", gett("sv.cur.text"), gett("sv.cur.diag"));
                    CHECK(geti("sv.cur.ok") == 1 && geti("sv.cur.idx") == 0, "cursor found (%lld)", (long long)geti("sv.cur.idx"));
                    SETW(cur, 2); RUN(20);
                    CHECK(geti("sv.cur.idx") == 2, "cursor followed live (%lld)", (long long)geti("sv.cur.idx"));
                    SETW(cur, 0x1234); RUN(20);
                    CHECK(geti("sv.cur.ok") == 0, "cursor out of range: not shown");
                    x->on_action(inst, "cur_reset", 0); x->on_action(inst, "cur_open", 0); RUN(5);
                    CHECK(geti("sv.cur.open") == 0 && geti("sv.cur.ok") == 0, "cursor search reset and closed");
#undef SETW
#undef RUN
                }
                /* zoom x4: the picture is a crop around you and the marker stays on you */
                x->on_action(inst, "map_zoom", 1); x->on_action(inst, "map_zoom", 1);
                for (int t = 0; t < 30; ++t, ++tick_now) m->sample(inst, &h);
                long long code = geti("sv.map.img");
                CHECK(geti("sv.map.zoom") == 4 && ((code >> 2) & 3) == 2, "zoom x4 (%lld)", code);
                char key[48]; snprintf(key, sizeof key, "module:map/%lld", code);
                img_w = 0;
                CHECK(x->load_image(inst, &h, key, NULL, img_sink) == EDEN_DSMOD_TRUE && img_w == 510, "zoomed crop (%d px)", img_w);
                long long mx = geti("sv.map.mx"), my = geti("sv.map.my");
                CHECK(geti("sv.map.arrow") == 1 && mx > 0 && mx < 900 && my > 0 && my < 900, "marker inside the zoomed view (%lld,%lld)", mx, my);
                printf("mapa x4: imagen %s marcador %lld,%lld rumbo %lld\n", key, mx, my, (long long)geti("sv.map.heading"));
                for (int k = 0; k < 4; ++k) x->on_action(inst, "map_zoom", 1);
                m->sample(inst, &h);
                CHECK(geti("sv.map.zoom") == 8 && geti("sv.map.can_in") == 0, "zoom stops at x8");
                for (int k = 0; k < 5; ++k) x->on_action(inst, "map_zoom", -1);
                m->sample(inst, &h);
                CHECK(geti("sv.map.zoom") == 1 && geti("sv.map.can_out") == 0 && (geti("sv.map.img") & 0xFFFFF) == 0, "back to the whole map");
                {   /* v0.6.6: the map is the 10 x 10 tile mosaic; tile (a, b) is colour (a*20+10, b*20+10) */
                    img_w = 0;
                    { char k0[48]; snprintf(k0, sizeof k0, "module:map/%lld", (long long)geti("sv.map.img"));
                    CHECK(x->load_image(inst, &h, k0, NULL, img_sink) == EDEN_DSMOD_TRUE && img_w == 1020,
                          "map mosaic (%d px)", img_w); }
                    /* pixel in row 2, column 7 of the mosaic = tile 02_07 */
                    const u8* q = img_px + ((u64)(2 * 102 + 50) * img_w + 7 * 102 + 50) * 4;
                    /* default mode 3: file AA_BB = column AA, row counted from the bottom BB -> row 2 col 7 = 07_07 */
                    CHECK(q[0] == 150 && q[1] == 150, "tile 07_07 at row 2, column 7 (%d,%d)", q[0], q[1]);
                    {   const u8* q2 = img_px + ((u64)(0 * 102 + 50) * img_w + 1 * 102 + 50) * 4; /* top row, column 1 = 01_09 */
                        CHECK(q2[0] == 30 && q2[1] == 190, "top-left area is file 01_09 (%d,%d)", q2[0], q2[1]); }
                    x->on_action(inst, "map_flip", 0); m->sample(inst, &h);
                    char key[48]; snprintf(key, sizeof key, "module:map/%lld", (long long)geti("sv.map.img"));
                    x->load_image(inst, &h, key, NULL, img_sink);
                    q = img_px + ((u64)(2 * 102 + 50) * img_w + 7 * 102 + 50) * 4;
                    CHECK(q[0] == 50 && q[1] == 50, "next mode (4): tile 02_02 there (%d,%d)", q[0], q[1]);
                    for (int k = 0; k < 7; ++k) x->on_action(inst, "map_flip", 0);   /* all 8 modes: back to 3 */
                    m->sample(inst, &h);
                    CHECK(((geti("sv.map.img") >> 20) & 7) == 3, "eight modes, back to the default");
                    printf("mapa piezas: %s\n", gett("sv.map.diag"));
                }
                printf("mapa diag: %s\n", gett("sv.map.diag"));
            }
            /* v0.6.7 trainer card */

            CHECK(geti("sv.tr.time_ok") == 1 && strcmp(gett("sv.tr.time"), "123 h 05 min") == 0, "play time (%s)", gett("sv.tr.time"));
            CHECK(geti("sv.tr.bp_ok") == 1 && geti("sv.tr.bp") == 2750, "Blueberry Points");
            CHECK(geti("sv.tr.dex_ok") == 1 && geti("sv.tr.seen") >= geti("sv.tr.caught"), "pokedex counts %lld/%lld",
                  (long long)geti("sv.tr.caught"), (long long)geti("sv.tr.seen"));
            printf("ficha: dex %lld/%lld | tiempo %s (%lld) | BP %lld (%lld)\n", (long long)geti("sv.tr.caught"), (long long)geti("sv.tr.seen"),
                   gett("sv.tr.time"), (long long)geti("sv.tr.time_ok"), (long long)geti("sv.tr.bp"), (long long)geti("sv.tr.bp_ok"));
            /* v0.6.9: a wild battle whose Pokémon is nowhere as a PK9 (device: Skiddo, Drifloon),
             * but whose name the game writes as UTF-16 text: HP box "Skiddo ... Nv. 26" and
             * "¡Un Skiddo salvaje te ha salido al paso!". Names already there on the field (a
             * Pokédex text "Skiddo") and the player's own ("Kingambit") do not count. */
            {
                #define PUT16(addr, txt) do { const char* _t = (txt); u64 _a = (addr); \
                    for (int _i = 0; _t[_i]; ++_i) { u16 _c = (u8)_t[_i]; if (_c == 0xC2 || _c == 0xC3) { _c = (u16)(((_c & 0x1F) << 6) | ((u8)_t[_i + 1] & 0x3F)); ++_i; } \
                        hp(_a)[0] = (u8)_c; hp(_a)[1] = (u8)(_c >> 8); _a += 2; } hp(_a)[0] = 0; hp(_a)[1] = 0; } while (0)
                u64 old = halloc(64); PUT16(old, "Skiddo");          /* on the field already */
                u64 mine0 = halloc(64); PUT16(mine0, "Kingambit");
                /* v0.7.0: the team reordered in game (live order), not saved yet */
                u64 s0 = 0, s1 = 0; memcpy(&s0, hp(L2 + 0x30), 8); memcpy(&s1, hp(L2 + 0x38), 8);
                w64(L2 + 0x30, s1); w64(L2 + 0x38, s0);
                hp(W3 + 0x28)[0] = 0x11;
                for (int t = 0; t < 600; ++t, ++tick_now) m->sample(inst, &h);
                CHECK(!strcmp(gett("sv.p0.name"), "Muk") && !strcmp(gett("sv.p1.name"), "Kingambit"),
                      "team in its live order (%s, %s)", gett("sv.p0.name"), gett("sv.p1.name"));
                hp(W3 + 0x28)[0] = 0x06;
                u64 box = halloc(0x300); PUT16(box, "Skiddo");
                u64 lvbox = halloc(0x40); PUT16(lvbox, "Nv. 26");      /* the level kept apart from the name */
                u64 mybox = halloc(0x40); PUT16(mybox, "Nv. 44");      /* yours: Deerling's level */
                u64 myname = halloc(0x40); PUT16(myname, "Deerling");  /* and its name in your HP box */
                u64 go = halloc(0x80); PUT16(go, "¡Adelante, Deerling!"); /* you sent out your third one */
                u64 msg = halloc(0x100); PUT16(msg, "¡Un Skiddo salvaje te ha salido al paso!");
                u64 mine = halloc(64); PUT16(mine, "Kingambit");
                u64 near = halloc(64); PUT16(near, "Pikachu");       /* one stray name elsewhere */
                for (int t = 0; t < 500; ++t, ++tick_now) m->sample(inst, &h);
                printf("por nombre: %s Nv%lld (%s) completo=%lld | %s\n", gett("sv.opp.name"), (long long)geti("sv.opp.splvl"),
                       gett("sv.opp.how"), (long long)geti("sv.opp.full"), gett("sv.learn.text"));
                CHECK(geti("sv.opp.valid") == 1 && !strcmp(gett("sv.opp.name"), "Skiddo"), "rival found by its name (%s)", gett("sv.opp.name"));
                CHECK(geti("sv.opp.splvl") == 26 && geti("sv.opp.level") == 26 && geti("sv.opp.lvl_ok") == 1, "its level read beside the name");
                CHECK(strstr(gett("sv.opp.how"), "nombre") != NULL && geti("sv.pick.open") == 0, "says how, no list");
                CHECK(!strcmp(gett("sv.me.name"), "Deerling"), "your Pokémon out, by its name (%s)", gett("sv.me.name"));
                (void)lvbox; (void)mybox; (void)go;
                hp(W3 + 0x28)[0] = 0x11;
                for (int t = 0; t < 90; ++t, ++tick_now) m->sample(inst, &h);
                CHECK(geti("sv.battle") == 0, "name battle over");
                /* straight into a trainer battle: the last battle's texts are still in memory;
                 * the trainer sends Gyarados, then Pikachu (the log keeps "Gyarados") */
                hp(W3 + 0x28)[0] = 0x06;
                /* v0.7.1, as on device: the HP box buffer is reused ("Gyarados" where "Skiddo"
                 * was) and getting off Koraidon writes its name too */
                memset(hp(box), 0, 0x40); PUT16(box, "Gyarados");
                u64 box2 = halloc(0x300); PUT16(box2 + 0x40, "Nv. 31");
                u64 log1 = halloc(0x100); PUT16(log1, "¡Ana saca a Gyarados!");
                u64 kor = halloc(0x40); PUT16(kor, "Koraidon");
                for (int t = 0; t < 60; ++t, ++tick_now) m->sample(inst, &h);
                CHECK(!strcmp(gett("sv.opp.name"), "Gyarados"), "second battle: named at once from the learnt slot (%s)", gett("sv.opp.name"));
                for (int t = 0; t < 440; ++t, ++tick_now) m->sample(inst, &h);
                CHECK(strcmp(gett("sv.opp.name"), "Koraidon") != 0, "the Koraidon you ride is never the rival");
                /* yours fainted, you send another: the same message buffer, a new name */
                memset(hp(go), 0, 0x80); PUT16(go, "¡Adelante, Muk!");
                for (int t = 0; t < 500; ++t, ++tick_now) m->sample(inst, &h);
                CHECK(!strcmp(gett("sv.me.name"), "Muk"), "your new Pokémon out, same buffer (%s)", gett("sv.me.name"));
                CHECK(!strcmp(gett("sv.opp.name"), "Gyarados") && geti("sv.opp.splvl") == 31,
                      "next battle: the new name, not the last battle's (%s Nv%lld)", gett("sv.opp.name"), (long long)geti("sv.opp.splvl"));
                memset(hp(box), 0, 0x40); PUT16(box, "Pikachu"); memset(hp(box2), 0, 0x300); PUT16(box2 + 0x40, "Nv. 33");
                u64 log2 = halloc(0x100); PUT16(log2, "¡Ana saca a Pikachu!");
                for (int t = 0; t < 500; ++t, ++tick_now) m->sample(inst, &h);
                CHECK(!strcmp(gett("sv.opp.name"), "Pikachu") && geti("sv.opp.splvl") == 33,
                      "trainer's next Pokémon followed (%s Nv%lld)", gett("sv.opp.name"), (long long)geti("sv.opp.splvl"));
                {   /* v0.7.3, as on device in a trainer battle:
                     * 1) a wild Pokémon nearby whose PK9 keeps changing HP must not take over */
                    u8 wild[0x158]; memcpy(wild, plain[1], 0x158); w32(wild, 0x7E57AB1Eu); w16(wild + 6, checksum(wild));
                    u64 wa = put_pk(wild);
                    int flips = 0;
                    for (int t = 0; t < 400; ++t, ++tick_now) {
                        if (t % 40 == 20) { w16(wild + 0x8A, (u16)(5 + t / 40)); w16(wild + 6, checksum(wild)); pk9_encrypt(wild, hp(wa)); }
                        m->sample(inst, &h);
                        if (strcmp(gett("sv.opp.name"), "Pikachu") != 0) flips++;
                    }
                    CHECK(flips == 0, "a wild Pokémon nearby never takes the rival's place (%d flips)", flips);
                    memset(hp(wa), 0, 8);
                    /* 2) dragged in by Whirlwind: no "¡Adelante!", another message names it */
                    u64 drag = halloc(0x100); PUT16(drag, "¡Deerling fue arrastrado al combate!");
                    /* 3) the team menu writes every one of your levels, far from the rival's box */
                    u64 menu = halloc(0x400); PUT16(menu, "Nv. 30"); PUT16(menu + 0x80, "Nv. 20"); PUT16(menu + 0x100, "Nv. 44");
                    for (int t = 0; t < 500; ++t, ++tick_now) m->sample(inst, &h);
                    CHECK(!strcmp(gett("sv.me.name"), "Deerling"), "your Pokémon dragged in is followed (%s)", gett("sv.me.name"));
                    CHECK(!strcmp(gett("sv.opp.name"), "Pikachu") && geti("sv.opp.splvl") == 33,
                          "rival's level still the one by its name (%s Nv%lld)", gett("sv.opp.name"), (long long)geti("sv.opp.splvl"));
                    memset(hp(drag), 0, 0x100); memset(hp(menu), 0, 0x400);
                }
                printf("entrenador: %s | %s\n", gett("sv.opp.name"), gett("sv.learn.text"));
                hp(W3 + 0x28)[0] = 0x11;
                for (int t = 0; t < 90; ++t, ++tick_now) m->sample(inst, &h);
                memset(hp(box), 0, 0x300); memset(hp(msg), 0, 0x100); memset(hp(near), 0, 64);
                memset(hp(box2), 0, 0x300); memset(hp(log1), 0, 0x100); memset(hp(log2), 0, 0x100);
                memset(hp(lvbox), 0, 0x40); memset(hp(mybox), 0, 0x40); memset(hp(myname), 0, 0x40); memset(hp(go), 0, 0x80); memset(hp(kor), 0, 0x40);
                w64(L2 + 0x30, s0); w64(L2 + 0x38, s1);                /* team order back */
                for (int t = 0; t < 400; ++t, ++tick_now) m->sample(inst, &h);
                (void)old; (void)mine0; (void)mine;
            }
            /* a battle nothing gives away: the picker lists the Pokémon around, you tap one */
            {
                hp(W3 + 0x28)[0] = 0x06;
                for (int t = 0; t < 240; ++t, ++tick_now) m->sample(inst, &h);
                printf("selector: abierto=%lld n=%lld 0=%s Nv%lld\n", (long long)geti("sv.pick.open"), (long long)geti("sv.pick.n"),
                       gett("sv.pick.0.name"), (long long)geti("sv.pick.0.level"));
                CHECK(geti("sv.battle") == 1 && geti("sv.pick.open") == 1 && geti("sv.pick.n") >= 1, "rival picker offered");
                CHECK(x->on_action(inst, "pick", 0) == EDEN_DSMOD_TRUE, "pick accepted");
                for (int t = 0; t < 30; ++t, ++tick_now) m->sample(inst, &h);
                CHECK(geti("sv.pick.open") == 0 && geti("sv.opp.valid") == 1 && geti("sv.opp.sure") == 1, "the chosen rival shown");
                CHECK(x->on_action(inst, "pick", 99) == EDEN_DSMOD_FALSE, "bad pick refused");
                /* a trainer's Pokémon is in no list: choose it by name */
                x->on_action(inst, "pick_open", 0); x->on_action(inst, "sp_open", 0); x->on_action(inst, "sp_letter", 6); /* G */
                m->sample(inst, &h);
                printf("buscador: letras=%lld lista=%lld pag %lld/%lld 0=%s 1=%s\n", (long long)geti("sv.sp.letters"), (long long)geti("sv.sp.listing"),
                       (long long)geti("sv.sp.page"), (long long)geti("sv.sp.pages"), gett("sv.sp.0.name"), gett("sv.sp.1.name"));
                CHECK(geti("sv.sp.listing") == 1 && geti("sv.sp.0.valid") == 1, "species browser lists the letter");
                int gy = -1;
                for (int pg = 0; pg < 4 && gy < 0; ++pg) {
                    for (int i = 0; i < 16; ++i) { char k2[32]; snprintf(k2, 32, "sv.sp.%d.name", i); if (!strcmp(gett(k2), "Gyarados")) gy = i; }
                    if (gy < 0) { x->on_action(inst, "sp_page", 1); m->sample(inst, &h); }
                }
                CHECK(gy >= 0, "Gyarados found under G");
                if (gy >= 0) x->on_action(inst, "sp_pick", gy);
                m->sample(inst, &h);
                printf("elegido por nombre: %s %s/%s completo=%lld débil0=%s\n", gett("sv.opp.name"), gett("sv.opp.t1n"), gett("sv.opp.t2n"),
                       (long long)geti("sv.opp.full"), gett("sv.opp.wk0.tn"));
                CHECK(geti("sv.opp.valid") == 1 && geti("sv.opp.full") == 0 && !strcmp(gett("sv.opp.t1n"), "Agua"), "species-only rival with its types");
                {   /* v0.6.8: you set its level; never "Debilitado"; stats and speed from that level */
                    long long lv0 = geti("sv.opp.splvl");
                    CHECK(lv0 == 0 && geti("sv.opp.splvl_unk") == 1 && geti("sv.opp.lvl_ok") == 0,
                          "species-only: level unknown, shown as ? (%lld)", lv0);
                    CHECK(strstr(gett("sv.opp.speed"), "Pon su nivel") != NULL, "speed waits for the level");
                    lv0 = 50; /* the buttons start from 50 */
                    CHECK(strcmp(gett("sv.opp.status"), "Debilitado") != 0, "species-only: not shown as fainted (%s)", gett("sv.opp.status"));
                    x->on_action(inst, "opp_lvl", 5); x->on_action(inst, "opp_lvl", -1); m->sample(inst, &h);
                    CHECK(geti("sv.opp.splvl") == lv0 + 4 && geti("sv.opp.level") == lv0 + 4, "level +5 -1 (%lld)", (long long)geti("sv.opp.splvl"));
                    for (int k = 0; k < 30; ++k) x->on_action(inst, "opp_lvl", 5);
                    m->sample(inst, &h);
                    CHECK(geti("sv.opp.splvl") == 100 && geti("sv.opp.stat5") > 0, "level stops at 100");
                    printf("especie a mano: Nv %lld vel %lld | %s\n", (long long)geti("sv.opp.level"), (long long)geti("sv.opp.stat5"), gett("sv.opp.speed"));
                    CHECK(strcmp(gett("sv.opp.speed"), "Velocidad desconocida") != 0, "speed compared over its range");
                }
                hp(W3 + 0x28)[0] = 0x11;
                for (int t = 0; t < 90; ++t, ++tick_now) m->sample(inst, &h);
                CHECK(geti("sv.battle") == 0, "battle over");
            }
            /* back to the defaults for the dump */
            x->on_action(inst, "bag_pouch", 0); x->on_action(inst, "dex_page", -1); x->on_action(inst, "dex_sel", 0);
            if (getenv("SV_ACT")) { /* "name=arg,name=arg" before the dump */
                char buf[256]; snprintf(buf, sizeof buf, "%s", getenv("SV_ACT"));
                for (char* tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
                    char* eq = strchr(tok, '='); if (!eq) continue; *eq = 0; x->on_action(inst, tok, atoll(eq + 1));
                }
            }
        }
    }

    if (getenv("SV_DUMP")) { /* snapshot for tools/preview.py */
        flag_sel = getenv("SV_SEL") ? atoi(getenv("SV_SEL")) : 0;
        flag_dtab = getenv("SV_DTAB") ? atoi(getenv("SV_DTAB")) : 0;
        np = 0;
        for (int t = 0; t < 20; ++t, ++tick_now) m->sample(inst, &h);
        FILE* f = fopen(getenv("SV_DUMP"), "w");
        for (int i = 0; i < np; ++i) {
            if (pistext[i]) fprintf(f, "T\t%s\t%s\n", pname[i], ptext[i]);
            else fprintf(f, "I\t%s\t%lld\n", pname[i], (long long)pval[i]);
        }
        fclose(f);
    }
    m->destroy(inst);
    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails != 0;
}
