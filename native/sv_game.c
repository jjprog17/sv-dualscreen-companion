/* Pokémon Scarlet/Violet packed romfs reader (see sv_game.h).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "sv_game.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

void* memcpy(void* d, const void* s, size_t n);
int ooz_decompress(const unsigned char* src, size_t src_len, unsigned char* dst, size_t dst_len);

#define OOZ_SAFE_SPACE 64

static u16 le16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 le32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 le64(const u8* p) { return (u64)le32(p) | ((u64)le32(p + 4) << 32); }

uint64_t sv_fnv1a64(const char* s) {
    u64 h = 0xCBF29CE484222645ull;
    for (; *s; ++s) { h ^= (u8)*s; h *= 0x100000001B3ull; }
    return h;
}

/* ---------- ranged romfs reads with a small page cache ---------- */
#define PAGE_BITS 14 /* 16 KiB */
#define PAGE_SIZE (1u << PAGE_BITS)
#define NPAGES 16
typedef struct { const char* path; u64 page; u32 len; u32 age; u8 data[PAGE_SIZE]; } Page;
static Page g_pages[NPAGES];
static u32 g_clock;

static const u8* page_get(const EdenDsmodHostApi* h, const char* path, u64 page, u32* len) {
    Page* victim = &g_pages[0];
    for (int i = 0; i < NPAGES; ++i) {
        Page* p = &g_pages[i];
        if (p->path == path && p->page == page && p->len) { p->age = ++g_clock; *len = p->len; return p->data; }
        if (p->age < victim->age) victim = p;
    }
    size_t n = h->read_romfs(h->userdata, path, page << PAGE_BITS, victim->data, PAGE_SIZE);
    if (n == 0 || n > PAGE_SIZE) { victim->len = 0; return 0; }
    victim->path = path; victim->page = page; victim->len = (u32)n; victim->age = ++g_clock;
    *len = (u32)n;
    return victim->data;
}

int sv_src_read(SvSrc* s, uint64_t off, void* out, uint32_t n) {
    if (off > s->size || n > s->size - off) return 0;
    if (s->mem) { memcpy(out, s->mem + off, n); return 1; }
    u8* o = (u8*)out;
    u64 a = s->base + off;
    while (n) {
        u32 len;
        const u8* p = page_get(s->h, s->path, a >> PAGE_BITS, &len);
        u32 in = (u32)(a & (PAGE_SIZE - 1));
        if (!p || in >= len) return 0;
        u32 take = len - in < n ? len - in : n;
        memcpy(o, p + in, take);
        o += take; a += take; n -= take;
    }
    return 1;
}

static int rd32(SvSrc* s, u64 off, u32* v) { u8 b[4]; if (!sv_src_read(s, off, b, 4)) return 0; *v = le32(b); return 1; }
static int rd64(SvSrc* s, u64 off, u64* v) { u8 b[8]; if (!sv_src_read(s, off, b, 8)) return 0; *v = le64(b); return 1; }

/* ---------- FlatBuffers ---------- */
int sv_fb_root(SvSrc* s, uint64_t* table) {
    u32 o;
    if (!rd32(s, 0, &o)) return 0;
    *table = o;
    return 1;
}
uint64_t sv_fb_field(SvSrc* s, uint64_t table, int id) {
    u8 b[4];
    if (!sv_src_read(s, table, b, 4)) return 0;
    int32_t soff = (int32_t)le32(b);
    u64 vt = (u64)((int64_t)table - soff);
    u8 v[2];
    if (!sv_src_read(s, vt, v, 2)) return 0;
    u32 vtsize = le16(v);
    u32 at = 4 + 2 * (u32)id;
    if (at + 2 > vtsize) return 0;
    if (!sv_src_read(s, vt + at, v, 2)) return 0;
    u32 fo = le16(v);
    return fo ? table + fo : 0;
}
uint64_t sv_fb_u64(SvSrc* s, uint64_t t, int id, uint64_t def) { u64 a = sv_fb_field(s, t, id), v; return a && rd64(s, a, &v) ? v : def; }
uint32_t sv_fb_u32(SvSrc* s, uint64_t t, int id, uint32_t def) { u64 a = sv_fb_field(s, t, id); u32 v; return a && rd32(s, a, &v) ? v : def; }
uint32_t sv_fb_u16(SvSrc* s, uint64_t t, int id, uint32_t def) { u64 a = sv_fb_field(s, t, id); u8 b[2]; return a && sv_src_read(s, a, b, 2) ? le16(b) : def; }
uint32_t sv_fb_u8(SvSrc* s, uint64_t t, int id, uint32_t def) { u64 a = sv_fb_field(s, t, id); u8 b; return a && sv_src_read(s, a, &b, 1) ? b : def; }
int sv_fb_vec(SvSrc* s, uint64_t table, int id, uint64_t* start, uint32_t* len) {
    u64 a = sv_fb_field(s, table, id);
    u32 rel, n;
    if (!a || !rd32(s, a, &rel)) return 0;
    if (!rd32(s, a + rel, &n)) return 0;
    *start = a + rel + 4;
    *len = n;
    return 1;
}
uint64_t sv_fb_vec_table(SvSrc* s, uint64_t start, uint32_t i) {
    u64 e = start + 4ull * i;
    u32 rel;
    return rd32(s, e, &rel) ? e + rel : 0;
}

/* Binary search in a sorted [u64] vector. Returns the index or -1. */
static int64_t bsearch64(SvSrc* s, u64 start, u32 len, u64 key) {
    int64_t lo = 0, hi = (int64_t)len - 1;
    while (lo <= hi) {
        int64_t mid = lo + ((hi - lo) >> 1);
        u64 v;
        if (!rd64(s, start + 8ull * (u64)mid, &v)) return -1;
        if (v < key) lo = mid + 1;
        else if (v > key) hi = mid - 1;
        else return mid;
    }
    return -1;
}

/* ---------- Trinity file system ---------- */
static const char* const kFd = "arc/data.trpfd";
static const char* const kFs = "arc/data.trpfs";

static struct {
    int open;
    SvSrc fd, meta;
    u64 fs_size;
    u64 fd_root, meta_root;
    u64 sub_hashes; u32 n_sub;
    u64 sub_infos; u32 n_sub_infos;
    u64 paths; u32 n_paths;
    u64 file_infos; u32 n_file_infos;
    u64 meta_hashes; u32 n_meta;
    u64 meta_offsets; u32 n_meta_off;
} G;

int sv_game_open(const EdenDsmodHostApi* h) {
    if (G.open) return 1;
    if (!h->read_romfs) return 0;
    u64 fd_size = h->read_romfs(h->userdata, kFd, 0, 0, 0);
    u64 fs_size = h->read_romfs(h->userdata, kFs, 0, 0, 0);
    if (fd_size < 16 || fs_size < 16) return 0;
    G.fd = (SvSrc){h, kFd, 0, fd_size, 0};
    SvSrc fs = {h, kFs, 0, fs_size, 0};
    u64 magic, meta_ofs;
    if (!rd64(&fs, 0, &magic) || magic != 0x004B434150454E4Full || !rd64(&fs, 8, &meta_ofs) ||
        meta_ofs >= fs_size)
        return 0;
    G.fs_size = fs_size;
    G.meta = (SvSrc){h, kFs, meta_ofs, fs_size - meta_ofs, 0};
    if (!sv_fb_root(&G.fd, &G.fd_root) || !sv_fb_root(&G.meta, &G.meta_root)) return 0;
    if (!sv_fb_vec(&G.fd, G.fd_root, 0, &G.sub_hashes, &G.n_sub) ||
        !sv_fb_vec(&G.fd, G.fd_root, 1, &G.paths, &G.n_paths) ||
        !sv_fb_vec(&G.fd, G.fd_root, 2, &G.sub_infos, &G.n_sub_infos) ||
        !sv_fb_vec(&G.fd, G.fd_root, 3, &G.file_infos, &G.n_file_infos) ||
        !sv_fb_vec(&G.meta, G.meta_root, 0, &G.meta_hashes, &G.n_meta) ||
        !sv_fb_vec(&G.meta, G.meta_root, 1, &G.meta_offsets, &G.n_meta_off))
        return 0;
    if (G.n_sub != G.n_sub_infos || G.n_paths != G.n_file_infos || G.n_meta != G.n_meta_off) return 0;
    G.open = 1;
    return 1;
}

static u8 g_cbuf[12u << 20]; /* compressed file bytes */

uint32_t sv_game_read(const EdenDsmodHostApi* h, const char* path, uint8_t* out, uint32_t cap,
                      const char** why) {
    const char* dummy;
    if (!why) why = &dummy;
    /* 1. A loose file (mods) wins. */
    size_t loose = h->read_romfs ? h->read_romfs(h->userdata, path, 0, 0, 0) : 0;
    if (loose) {
        if (loose > cap) { *why = "loose file too big"; return 0; }
        if (h->read_romfs(h->userdata, path, 0, out, loose) != loose) { *why = "loose read"; return 0; }
        *why = "loose";
        return (u32)loose;
    }
    if (!sv_game_open(h)) { *why = "no trpfs"; return 0; }
    u64 hash = sv_fnv1a64(path);
    /* 2. Which pack holds it. */
    int64_t si = bsearch64(&G.fd, G.sub_hashes, G.n_sub, hash);
    if (si < 0) { *why = "not in trpfd"; return 0; }
    u64 info = sv_fb_vec_table(&G.fd, G.sub_infos, (u32)si);
    u64 pack = info ? sv_fb_u64(&G.fd, info, 0, 0) : ~0ull; /* FlatBuffers omit a 0 */
    if (pack >= G.n_paths) { *why = "bad pack index"; return 0; }
    /* 3. The pack's path -> hash -> offset in the trpfs; its size from FileInfos. */
    u64 pstr = G.paths + 4ull * pack;
    u32 rel, plen;
    if (!rd32(&G.fd, pstr, &rel) || !rd32(&G.fd, pstr + rel, &plen) || plen == 0 || plen > 400) {
        *why = "bad pack path"; return 0;
    }
    char ppath[404];
    if (!sv_src_read(&G.fd, pstr + rel + 4, ppath, plen)) { *why = "pack path read"; return 0; }
    ppath[plen] = 0;
    u64 finfo = sv_fb_vec_table(&G.fd, G.file_infos, (u32)pack);
    u64 psize = finfo ? sv_fb_u64(&G.fd, finfo, 0, 0) : 0;
    int64_t mi = bsearch64(&G.meta, G.meta_hashes, G.n_meta, sv_fnv1a64(ppath));
    u64 pofs;
    if (mi < 0 || !rd64(&G.meta, G.meta_offsets + 8ull * (u64)mi, &pofs) || psize == 0 ||
        pofs > G.fs_size || psize > G.fs_size - pofs) {
        *why = "pack not in trpfs"; return 0;
    }
    /* 4. The file inside the pack. */
    SvSrc pk = {h, kFs, pofs, psize, 0};
    u64 root, hashes, files;
    u32 nh, nf;
    if (!sv_fb_root(&pk, &root) || !sv_fb_vec(&pk, root, 0, &hashes, &nh) ||
        !sv_fb_vec(&pk, root, 1, &files, &nf) || nh != nf) {
        *why = "bad pack"; return 0;
    }
    int64_t fi = bsearch64(&pk, hashes, nh, hash);
    if (fi < 0) { *why = "not in pack"; return 0; }
    u64 fd = sv_fb_vec_table(&pk, files, (u32)fi);
    int ctype = (int8_t)sv_fb_u8(&pk, fd, 1, 0);
    u64 usize = sv_fb_u64(&pk, fd, 3, 0);
    u64 dstart; u32 dlen;
    if (!fd || !sv_fb_vec(&pk, fd, 4, &dstart, &dlen)) { *why = "no data"; return 0; }
    if (dlen > sizeof(g_cbuf)) { *why = "file too big"; return 0; }
    /* Bulk read straight from the romfs (no page cache for the payload). */
    if (h->read_romfs(h->userdata, kFs, pofs + dstart, g_cbuf, dlen) != dlen) { *why = "data read"; return 0; }
    if (ctype == -1) {
        if (dlen > cap) { *why = "too big"; return 0; }
        memcpy(out, g_cbuf, dlen);
        *why = "stored";
        return dlen;
    }
    if (ctype >= 3 && ctype <= 7) {
        if (usize == 0 || usize + OOZ_SAFE_SPACE > cap) { *why = "too big"; return 0; }
        int r = ooz_decompress(g_cbuf, dlen, out, (size_t)usize);
        if (r != (int)usize) { *why = "oodle failed"; return 0; }
        *why = "oodle";
        return (u32)usize;
    }
    *why = ctype == 1 ? "zlib not supported" : ctype == 2 ? "lz4 not supported" : "unknown compression";
    return 0;
}
