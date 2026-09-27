/* Stage 37.1: C prototypes matching the wasm32 runtime (salivo_wasm_runtime.c). Sizes are
 * long long because the Stage 3 IR declares these functions with i64 sizes; a size_t (i32)
 * prototype would give the same symbol two wasm signatures. */
#ifndef SALIVO_WASM_LIBC_H
#define SALIVO_WASM_LIBC_H
typedef unsigned long size_t;
#define NULL ((void*)0)
void* malloc(long long n);
void* realloc(void* p, long long n);
void* calloc(long long a, long long b);
void free(void* p);
void* memcpy(void* d, const void* s, size_t n);
void* memmove(void* d, const void* s, size_t n);
void* memset(void* d, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
long long strlen(const char* s);
int strcmp(const char* a, const char* b);
void exit(int code);
char* strdup(const char* s);
#endif
