/* Stage 37.1: freestanding wasm32 runtime for modules emitted by the Stage 3 compiler.
 *
 * Built with clang --target=wasm32-unknown-unknown -nostdlib -ffreestanding -fno-builtin.
 * Signatures follow the declarations the Stage 3 IR emits (sizes are i64 there), not libc.
 *
 * Module ABI (docs/STAGE_37_1_WASM.md):
 *   imports  salivo.write(fd: i32, ptr: i32, len: i32)   host writes bytes to stdout (fd 1)
 *            salivo.exit(code: i32)                       host ends the program; never returns
 *   exports  _start()                                     runs main, then salivo.exit(status)
 *            memory                                       linear memory
 * Everything else (files, environment, processes, sockets, threads, clocks, randomness) is not
 * defined here, so a program that uses it fails to link with an "undefined symbol" error.
 */

typedef unsigned long uptr; /* 32 bits on wasm32 */
_Static_assert(sizeof(void*) == 4 && sizeof(uptr) == 4 && sizeof(long long) == 8, "wasm32 layout");
typedef __builtin_va_list va_list;
#define va_start(a, f) __builtin_va_start(a, f)
#define va_arg(a, t) __builtin_va_arg(a, t)
#define va_end(a) __builtin_va_end(a)

__attribute__((import_module("salivo"), import_name("write"))) void host_write(int fd, const char* p, int n);
__attribute__((import_module("salivo"), import_name("exit"), noreturn)) void host_exit(int code);

/* The user's main is emitted as the IR symbol "main"; bypass clang's wasm main renaming. */
extern int user_main(void) __asm__("main");

__attribute__((export_name("_start"))) void _start(void) { host_exit(user_main()); }

void _Exit(int code) { host_exit(code); }
void exit(int code) { host_exit(code); }

static void write_str(const char* s, uptr n) { host_write(1, s, (int)n); }

__attribute__((noreturn)) static void fatal(const char* msg) {
    uptr n = 0;
    while (msg[n]) n++;
    write_str(msg, n);
    host_exit(134);
}

/* ---- memory -------------------------------------------------------------------------------
 * Power-of-two size classes with one free list per class and a 16-byte header, so every block
 * is 16-byte aligned. memory.grow failure (or a request above the wasm32 address space) is a
 * deterministic fatal error rather than a NULL that callers would write through at address 0.
 * ponytail: no coalescing or splitting; a class-segregated heap wastes up to 2x on odd sizes. */

extern unsigned char __heap_base;
static uptr heap_top, heap_end;
static void* free_lists[32];

typedef struct { uptr cls; uptr pad[3]; } Header;

static void* carve(uptr bytes) {
    if (!heap_top) heap_top = heap_end = ((uptr)&__heap_base + 15) & ~(uptr)15;
    if (bytes > 0x7FFFFFFFu - heap_top) fatal("panic: out of memory\n");
    while (heap_top + bytes > heap_end) {
        uptr need = (heap_top + bytes - heap_end + 65535) / 65536;
        if (__builtin_wasm_memory_grow(0, need) == (uptr)-1) fatal("panic: out of memory\n");
        heap_end += need * 65536;
    }
    void* p = (void*)heap_top;
    heap_top += bytes;
    return p;
}

void* malloc(long long n) {
    if (n < 0 || n > 0x40000000LL) fatal("panic: out of memory\n");
    uptr cls = 4;
    while (((uptr)1 << cls) < (uptr)n + sizeof(Header)) cls++;
    Header* h = (Header*)free_lists[cls];
    if (h) free_lists[cls] = *(void**)(h + 1);
    else h = (Header*)carve((uptr)1 << cls);
    h->cls = cls;
    return h + 1;
}

void free(void* p) {
    if (!p) return;
    Header* h = (Header*)p - 1;
    if (h->cls < 4 || h->cls > 31) fatal("panic: invalid free\n");
    *(void**)p = free_lists[h->cls];
    free_lists[h->cls] = h;
    h->cls = 0; /* a second free of the same block is caught above */
}

void* memcpy(void* d, const void* s, uptr n) {
    unsigned char* a = d;
    const unsigned char* b = s;
    while (n--) *a++ = *b++;
    return d;
}

void* memmove(void* d, const void* s, uptr n) {
    unsigned char* a = d;
    const unsigned char* b = s;
    if (a < b) while (n--) *a++ = *b++;
    else while (n--) a[n] = b[n];
    return d;
}

void* memset(void* d, int c, uptr n) {
    unsigned char* a = d;
    while (n--) *a++ = (unsigned char)c;
    return d;
}

int memcmp(const void* x, const void* y, uptr n) {
    const unsigned char *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}

void* realloc(void* p, long long n) {
    if (!p) return malloc(n);
    Header* h = (Header*)p - 1;
    uptr cap = ((uptr)1 << h->cls) - sizeof(Header);
    if (n >= 0 && (uptr)n <= cap) return p;
    void* q = malloc(n);
    memcpy(q, p, cap);
    free(p);
    return q;
}

void* calloc(long long a, long long b) {
    if (a < 0 || b < 0 || (b && a > 0x40000000LL / b)) fatal("panic: out of memory\n");
    return memset(malloc(a * b), 0, (uptr)(a * b));
}

/* salivo.std.mem (allocate / alloczero / deallocate / reallocate), with the native runtime's
 * contract: size <= 0 yields 0, reallocate of 0 allocates, reallocate to size <= 0 frees. Blocks
 * are 16-byte aligned; a larger alignment is not available on wasm32 and is a fatal error. */
static void check_align(long long a) {
    if (a > 16) fatal("panic: alignment above 16 bytes is not supported on wasm32\n");
}
long long salivo_crt_salivo_alloc(long long n, long long a) {
    check_align(a);
    return n <= 0 ? 0 : (long long)(uptr)malloc(n);
}
long long salivo_crt_salivo_alloc_zeroed(long long n, long long a) {
    check_align(a);
    return n <= 0 ? 0 : (long long)(uptr)calloc(n, 1);
}
void salivo_crt_salivo_deallocate(long long p, long long n, long long a) {
    (void)n; (void)a;
    free((void*)(uptr)p);
}
long long salivo_crt_salivo_reallocate(long long p, long long old, long long n, long long a) {
    (void)old;
    check_align(a);
    if (n <= 0) { free((void*)(uptr)p); return 0; }
    return (long long)(uptr)realloc((void*)(uptr)p, n);
}

/* Single linear heap: the per-request heap hooks of the native runtime are not used on wasm32. */
void salivo_heap_hooks(void* swap, void* release, void* str_alloc) { (void)swap; (void)release; (void)str_alloc; }
void salivo_set_str_alloc(void* f) { (void)f; }

/* ---- strings ----------------------------------------------------------------------------- */

long long strlen(const char* s) {
    const char* p = s;
    while (*p) p++;
    return p - s;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

/* ponytail: accumulates in double, so inputs beyond 17 significant digits may round differently
 * from a correctly rounded libc strtod. */
double strtod(const char* s, char** end) {
    const char* p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    int neg = *p == '-';
    if (*p == '-' || *p == '+') p++;
    double v = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0'), digits++;
    if (*p == '.') {
        p++;
        double scale = 0.1;
        while (*p >= '0' && *p <= '9') v += (*p++ - '0') * scale, scale *= 0.1, digits++;
    }
    if (!digits) {
        if (end) *end = (char*)s;
        return 0;
    }
    if (*p == 'e' || *p == 'E') {
        const char* q = p + 1;
        int eneg = *q == '-';
        if (*q == '-' || *q == '+') q++;
        if (*q >= '0' && *q <= '9') {
            int e = 0;
            while (*q >= '0' && *q <= '9') e = e * 10 + (*q++ - '0');
            while (e--) v = eneg ? v / 10 : v * 10;
            p = q;
        }
    }
    if (end) *end = (char*)p;
    return neg ? -v : v;
}

/* ---- formatting: %lld %lli %llu %d %i %u %s %c %f %% (the forms the Stage 3 IR emits) ----- */

typedef struct { char* buf; uptr cap; uptr len; } Out;

static void put(Out* o, char c) {
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void put_u(Out* o, unsigned long long v) {
    char tmp[24];
    int n = 0;
    do tmp[n++] = (char)('0' + v % 10); while (v /= 10);
    while (n) put(o, tmp[--n]);
}

/* ponytail: %f matches libc's 6-digit output for |x| < 2^63; larger magnitudes print in
 * scientific form "1e+NN"-style approximations. */
static void put_f(Out* o, double x) {
    if (x != x) { put(o, 'n'); put(o, 'a'); put(o, 'n'); return; }
    if (x < 0 || (x == 0 && 1 / x < 0)) { put(o, '-'); x = -x; }
    if (x > 1.7976931348623157e308) { put(o, 'i'); put(o, 'n'); put(o, 'f'); return; }
    if (x >= 9.2e18) {
        int e = 0;
        while (x >= 10) x /= 10, e++;
        put_f(o, x);
        put(o, 'e'); put(o, '+'); put_u(o, (unsigned long long)e);
        return;
    }
    unsigned long long ip = (unsigned long long)x;
    double frac = (x - (double)ip) * 1000000.0;
    unsigned long long fp = (unsigned long long)frac;
    double rem = frac - (double)fp;
    if (rem > 0.5 || (rem == 0.5 && (fp & 1))) fp++;
    if (fp >= 1000000) { fp -= 1000000; ip++; }
    put_u(o, ip);
    put(o, '.');
    for (unsigned long long d = 100000; d; d /= 10) put(o, (char)('0' + (fp / d) % 10));
}

static uptr vformat(char* buf, uptr cap, const char* f, va_list ap) {
    Out o = {buf, cap, 0};
    for (; *f; f++) {
        if (*f != '%') { put(&o, *f); continue; }
        f++;
        int longs = 0;
        while (*f == 'l') longs++, f++;
        switch (*f) {
        case 'd': case 'i': {
            long long v = longs ? va_arg(ap, long long) : va_arg(ap, int);
            if (v < 0) { put(&o, '-'); put_u(&o, 0ull - (unsigned long long)v); }
            else put_u(&o, (unsigned long long)v);
            break;
        }
        case 'u': put_u(&o, longs ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned)); break;
        case 's': { const char* s = va_arg(ap, const char*); if (!s) s = "(null)"; while (*s) put(&o, *s++); break; }
        case 'c': put(&o, (char)va_arg(ap, int)); break;
        case 'f': put_f(&o, va_arg(ap, double)); break;
        case '%': put(&o, '%'); break;
        default: fatal("panic: unsupported format directive on wasm32\n");
        }
    }
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return o.len;
}

int snprintf(char* buf, long long n, const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    uptr len = vformat(buf, n < 0 ? 0 : (uptr)n, f, ap);
    va_end(ap);
    return (int)len;
}

int printf(const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    char small[512];
    uptr len = vformat(small, sizeof small, f, ap);
    va_end(ap);
    if (len < sizeof small) {
        write_str(small, len);
    } else {
        char* big = malloc((long long)len + 1);
        va_start(ap, f);
        vformat(big, len + 1, f, ap);
        va_end(ap);
        write_str(big, len);
        free(big);
    }
    return (int)len;
}

int puts(const char* s) {
    write_str(s, (uptr)strlen(s));
    write_str("\n", 1);
    return 0;
}

/* Writes are unbuffered, so there is nothing to flush. */
int fflush(void* stream) { (void)stream; return 0; }

char* strdup(const char* s) {
    long long n = strlen(s) + 1;
    return memcpy(malloc(n), s, (uptr)n);
}

/* `:int` / `:float` conversions (atoll / atof semantics of the native runtime). The IR passes the
 * string as an i64 handle. */
long long rtstrint(long long h) {
    const char* p = (const char*)(uptr)h;
    if (!p) return 0;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    int neg = *p == '-';
    if (*p == '-' || *p == '+') p++;
    unsigned long long v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (unsigned long long)(*p++ - '0');
    return neg ? (long long)(0ull - v) : (long long)v;
}

double rtstrfloat(long long h) {
    return h ? strtod((const char*)(uptr)h, 0) : 0.0;
}

/* LLVM rewrites printf("x") into putchar at -O1 and above */
int putchar(int c) {
    char b = (char)c;
    write_str(&b, 1);
    return c & 0xff;
}
