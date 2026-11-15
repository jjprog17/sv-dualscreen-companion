/* Freestanding runtime for the module: memory functions and a small arena allocator for the
 * Oodle decoder (ooz allocates one aligned scratch block per call and frees it before returning).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stddef.h>
#include <stdint.h>

#define HIDDEN __attribute__((visibility("hidden")))

HIDDEN void* memcpy(void* d, const void* s, size_t n) {
    uint8_t* dp = (uint8_t*)d;
    const uint8_t* sp = (const uint8_t*)s;
    while (n--) *dp++ = *sp++;
    return d;
}
HIDDEN void* memset(void* d, int c, size_t n) {
    uint8_t* dp = (uint8_t*)d;
    while (n--) *dp++ = (uint8_t)c;
    return d;
}
HIDDEN int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (; n; --n, ++x, ++y)
        if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}
HIDDEN void* memmove(void* d, const void* s, size_t n) {
    uint8_t* dp = (uint8_t*)d;
    const uint8_t* sp = (const uint8_t*)s;
    if (dp < sp) { while (n--) *dp++ = *sp++; }
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}

/* Bump arena, reset when every block is freed. 4 MiB covers ooz's scratch (~0x6C000) easily. */
static uint8_t g_arena[4u << 20] __attribute__((aligned(64)));
static size_t g_top;
static size_t g_live;
HIDDEN void* malloc(size_t n) {
    size_t at = (g_top + 63) & ~(size_t)63;
    if (n > sizeof(g_arena) || at > sizeof(g_arena) - n) return 0;
    g_top = at + n;
    g_live++;
    return g_arena + at;
}
HIDDEN void free(void* p) {
    if (!p) return;
    if (g_live && --g_live == 0) g_top = 0;
}
HIDDEN void abort(void) { __builtin_trap(); }
