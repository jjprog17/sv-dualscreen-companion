/* Freestanding shim: only what the vendored ooz decoder needs (sv_alloc.c implements it). */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* malloc(size_t n);
void free(void* p);
void abort(void) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif
