/* Reader for Pokémon Scarlet/Violet's packed romfs (Trinity file system), over the Eden Duo
 * host's read_romfs. Layout from pkNX (FlatBuffers/SV/Trinity):
 *   arc/data.trpfd  FlatBuffer TrinityFileDescriptors {SubFileHashes:[u64], FilePaths:[string],
 *                   SubFileInfos:[{Index:u64}], FileInfos:[{FileSize:u64, FileCount:u64}]}
 *   arc/data.trpfs  "ONEPACK\0", u64 offset of TrinityFileSystemMetadata {FileHashes:[u64],
 *                   FileOffsets:[u64]} (pack hash = FNV-1a 64 of the pack's path)
 *   pack            FlatBuffer TrinityPak {Hashes:[u64], Files:[{Field_00:u32, CompressionType:s8,
 *                   CompressionLevel:u8, UncompressedSize:u64, Data:[u8]}]}
 * A file is found by the FNV-1a 64 hash of its path. A loose romfs file of the same path (how
 * Trinity-based mods such as Pokémon Compass replace data) is preferred when present.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "dsmod_module_abi.h"

/* In-memory or romfs-ranged FlatBuffer source. */
typedef struct {
    const EdenDsmodHostApi* h;
    const char* path; /* romfs path when mem == 0 */
    uint64_t base;
    uint64_t size;
    const uint8_t* mem;
} SvSrc;

int sv_src_read(SvSrc* s, uint64_t off, void* out, uint32_t n);
int sv_fb_root(SvSrc* s, uint64_t* table);
/* Address of field `id` in `table`, 0 when absent. */
uint64_t sv_fb_field(SvSrc* s, uint64_t table, int id);
uint64_t sv_fb_u64(SvSrc* s, uint64_t table, int id, uint64_t def);
uint32_t sv_fb_u32(SvSrc* s, uint64_t table, int id, uint32_t def);
uint32_t sv_fb_u16(SvSrc* s, uint64_t table, int id, uint32_t def);
uint32_t sv_fb_u8(SvSrc* s, uint64_t table, int id, uint32_t def);
/* Vector field: element start address and length. */
int sv_fb_vec(SvSrc* s, uint64_t table, int id, uint64_t* start, uint32_t* len);
/* Element i of a vector of tables. */
uint64_t sv_fb_vec_table(SvSrc* s, uint64_t start, uint32_t i);

uint64_t sv_fnv1a64(const char* s);

/* Opens arc/data.trpfd / arc/data.trpfs. Returns 1 when the packed file system is usable. */
int sv_game_open(const EdenDsmodHostApi* h);
/* Reads a game file by path into out (cap bytes). Returns its size, 0 on any failure.
 * *why is set to a short reason for logs. */
uint32_t sv_game_read(const EdenDsmodHostApi* h, const char* path, uint8_t* out, uint32_t cap,
                      const char** why);
