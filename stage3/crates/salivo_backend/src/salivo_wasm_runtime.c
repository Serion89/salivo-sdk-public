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
 * Buddy allocator: power-of-two blocks (16-byte header, so every block is 16-byte aligned), each at
 * an offset from heap_base that is a multiple of its size. A free block merges with its free buddy
 * of the same size, and a request with no block of its size splits a larger free one, so memory a
 * program frees is reusable at any size. Every byte below heap_top belongs to a block with a valid
 * header (alignment gaps become free blocks), which makes reading a buddy's header always safe.
 * wasm memory cannot shrink, so the heap's high-water mark stays. memory.grow failure (or a request
 * above the wasm32 address space) is a deterministic fatal error rather than a NULL that callers
 * would write through at address 0. */

extern unsigned char __heap_base;
static uptr heap_base, heap_top, heap_end;
static void* free_lists[32];

typedef struct { uptr cls; uptr is_free; uptr next; uptr prev; } Header;

static void list_push(Header* h, uptr cls) {
    h->cls = cls;
    h->is_free = 1;
    h->prev = 0;
    h->next = (uptr)free_lists[cls];
    if (free_lists[cls]) ((Header*)free_lists[cls])->prev = (uptr)h;
    free_lists[cls] = h;
}

static void list_remove(Header* h) {
    Header* n = (Header*)h->next;
    Header* pv = (Header*)h->prev;
    if (pv) pv->next = (uptr)n;
    else free_lists[h->cls] = n;
    if (n) n->prev = (uptr)pv;
    h->is_free = 0;
}

/* A new block of 2^cls bytes at the top of the heap, aligned to its size */
static Header* carve(uptr cls) {
    if (!heap_base) heap_base = heap_top = heap_end = ((uptr)&__heap_base + 15) & ~(uptr)15;
    uptr size = (uptr)1 << cls;
    uptr off = heap_top - heap_base;
    uptr at = (off + size - 1) & ~(size - 1);
    if (at + size < at || at + size > 0x7FFFFFFFu - heap_base) fatal("panic: out of memory\n");
    uptr new_top = heap_base + at + size;
    while (new_top > heap_end) {
        uptr need = (new_top - heap_end + 65535) / 65536;
        if (__builtin_wasm_memory_grow(0, need) == (uptr)-1) fatal("panic: out of memory\n");
        heap_end += need * 65536;
    }
    /* the alignment gap becomes free blocks, each aligned to its own size */
    while (off < at) {
        uptr k = 4;
        while (k + 1 < cls && (off & (((uptr)1 << (k + 1)) - 1)) == 0 && off + ((uptr)1 << (k + 1)) <= at) k++;
        list_push((Header*)(heap_base + off), k);
        off += (uptr)1 << k;
    }
    heap_top = new_top;
    return (Header*)(heap_base + at);
}

void* malloc(long long n) {
    if (n < 0 || n > 0x40000000LL) fatal("panic: out of memory\n");
    uptr cls = 4;
    while (((uptr)1 << cls) < (uptr)n + sizeof(Header)) cls++;
    Header* h = (Header*)free_lists[cls];
    if (h) {
        list_remove(h);
    } else {
        uptr k = cls + 1;
        while (k < 32 && !free_lists[k]) k++;
        if (k < 32) {
            h = (Header*)free_lists[k];
            list_remove(h);
            while (k > cls) { /* keep the lower half, free the upper half */
                k--;
                list_push((Header*)((uptr)h + ((uptr)1 << k)), k);
            }
        } else {
            h = carve(cls);
        }
    }
    h->cls = cls;
    h->is_free = 0;
    return h + 1;
}

/* An allocation aligned above 16 bytes lives inside a larger block; the 16 bytes before it hold a
   redirect header (cls 0, is_free ALIGN_REDIRECT, next = the block's own pointer) */
#define ALIGN_REDIRECT 0xA11Du

static void* unredirect(void* p) {
    Header* h = (Header*)p - 1;
    return (h->cls == 0 && h->is_free == ALIGN_REDIRECT) ? (void*)h->next : p;
}

/* n bytes aligned to a (a power of two); 16 or less is every block's own alignment */
static void* malloc_aligned(long long n, long long a) {
    if (a <= 16) return malloc(n);
    if ((a & (a - 1)) != 0 || a > (1LL << 24)) fatal("panic: alignment must be a power of two up to 16 MiB\n");
    if (n > 0x40000000LL) fatal("panic: out of memory\n");
    uptr raw = (uptr)malloc(n + a + (long long)sizeof(Header));
    uptr p = (raw + sizeof(Header) + (uptr)a - 1) & ~((uptr)a - 1);
    Header* r = (Header*)p - 1; /* inside the block: p >= raw + 16 */
    r->cls = 0;
    r->is_free = ALIGN_REDIRECT;
    r->next = raw;
    r->prev = 0;
    return (void*)p;
}

/* Bytes usable at p (an aligned allocation ends where its block ends) */
static uptr usable_size(void* p) {
    void* base = unredirect(p);
    Header* h = (Header*)base - 1;
    return ((uptr)1 << h->cls) - sizeof(Header) - ((uptr)p - (uptr)base);
}

void free(void* p) {
    if (!p) return;
    p = unredirect(p);
    Header* h = (Header*)p - 1;
    if (h->cls < 4 || h->cls > 31 || h->is_free) fatal("panic: invalid free\n");
    uptr cls = h->cls;
    while (cls < 30) {
        uptr size = (uptr)1 << cls;
        uptr buddy = heap_base + (((uptr)h - heap_base) ^ size);
        if (buddy + size > heap_top) break;
        Header* bh = (Header*)buddy;
        if (!bh->is_free || bh->cls != cls) break;
        list_remove(bh);
        if (bh < h) h = bh;
        cls++;
    }
    list_push(h, cls);
}

/* With bulk memory (-mbulk-memory) these are single memory.copy / memory.fill instructions; the
   builtins lower to them there, never to a call back into these functions */
void* memcpy(void* d, const void* s, uptr n) {
#if defined(__wasm_bulk_memory__)
    __builtin_memcpy(d, s, n);
#else
    unsigned char* a = d;
    const unsigned char* b = s;
    while (n--) *a++ = *b++;
#endif
    return d;
}

void* memmove(void* d, const void* s, uptr n) {
#if defined(__wasm_bulk_memory__)
    __builtin_memmove(d, s, n);
#else
    unsigned char* a = d;
    const unsigned char* b = s;
    if (a < b) while (n--) *a++ = *b++;
    else while (n--) a[n] = b[n];
#endif
    return d;
}

void* memset(void* d, int c, uptr n) {
#if defined(__wasm_bulk_memory__)
    __builtin_memset(d, c, n);
#else
    unsigned char* a = d;
    while (n--) *a++ = (unsigned char)c;
#endif
    return d;
}

int memcmp(const void* x, const void* y, uptr n) {
    const unsigned char *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}

void* realloc(void* p, long long n) {
    if (!p) return malloc(n);
    uptr cap = usable_size(p);
    if (n >= 0 && (uptr)n <= cap && unredirect(p) == p) return p;
    void* q = malloc(n);
    memcpy(q, p, (uptr)n < cap ? (uptr)n : cap);
    free(p);
    return q;
}

/* realloc that keeps alignment a */
static void* realloc_aligned(void* p, long long n, long long a) {
    if (a <= 16) return realloc(p, n);
    if (!p) return malloc_aligned(n, a);
    uptr cap = usable_size(p);
    if ((uptr)n <= cap) return p;
    void* q = malloc_aligned(n, a);
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
 * are 16-byte aligned; a larger power-of-two alignment over-allocates (malloc_aligned). */
long long salivo_crt_salivo_alloc(long long n, long long a) {
    return n <= 0 ? 0 : (long long)(uptr)malloc_aligned(n, a);
}
long long salivo_crt_salivo_alloc_zeroed(long long n, long long a) {
    if (n <= 0) return 0;
    void* p = malloc_aligned(n, a);
    return (long long)(uptr)memset(p, 0, (uptr)n);
}
void salivo_crt_salivo_deallocate(long long p, long long n, long long a) {
    (void)n; (void)a;
    free((void*)(uptr)p);
}
long long salivo_crt_salivo_reallocate(long long p, long long old, long long n, long long a) {
    (void)old;
    if (n <= 0) { free((void*)(uptr)p); return 0; }
    return (long long)(uptr)realloc_aligned((void*)(uptr)p, n, a);
}

/* Single linear heap: the per-request heap hooks of the native runtime are not used on wasm32. */
void salivo_heap_hooks(void* swap, void* release, void* str_alloc, void* block_free) { (void)swap; (void)release; (void)str_alloc; (void)block_free; }
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
/* x with prec digits after the point (0..9 exact; more digits are zeros), ties to even like libc */
static void put_fp(Out* o, double x, int prec) {
    if (x != x) { put(o, 'n'); put(o, 'a'); put(o, 'n'); return; }
    if (x < 0 || (x == 0 && 1 / x < 0)) { put(o, '-'); x = -x; }
    if (x > 1.7976931348623157e308) { put(o, 'i'); put(o, 'n'); put(o, 'f'); return; }
    if (x >= 9.2e18) {
        int e = 0;
        while (x >= 10) x /= 10, e++;
        put_fp(o, x, prec);
        put(o, 'e'); put(o, '+'); put_u(o, (unsigned long long)e);
        return;
    }
    int exact = prec > 9 ? 9 : prec;
    unsigned long long scale = 1;
    for (int i = 0; i < exact; i++) scale *= 10;
    unsigned long long ip = (unsigned long long)x;
    double frac = (x - (double)ip) * (double)scale;
    unsigned long long fp = (unsigned long long)frac;
    double rem = frac - (double)fp;
    if (rem > 0.5 || (rem == 0.5 && ((exact ? fp : ip) & 1))) fp++;
    if (fp >= scale) { fp -= scale; ip++; }
    put_u(o, ip);
    if (prec == 0) return;
    put(o, '.');
    for (unsigned long long d = scale / 10; d; d /= 10) put(o, (char)('0' + (fp / d) % 10));
    for (int i = exact; i < prec; i++) put(o, '0');
}

static void put_f(Out* o, double x) { put_fp(o, x, 6); }

static void put_base(Out* o, unsigned long long v, unsigned base, int upper) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    do tmp[n++] = digits[v % base]; while (v /= base);
    while (n) put(o, tmp[--n]);
}

/* %[flags -0+ ][width][.precision][hh h l ll z j t]d i u x X o p s c f F % ; unknown directives print
   as written (like libc), never abort */
static uptr vformat(char* buf, uptr cap, const char* f, va_list ap) {
    Out o = {buf, cap, 0};
    for (; *f; f++) {
        if (*f != '%') { put(&o, *f); continue; }
        const char* start = f;
        f++;
        int left = 0, zero = 0, plus = 0, space = 0, width = 0, prec = -1;
        for (;; f++) {
            if (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else if (*f == '+') plus = 1;
            else if (*f == ' ') space = 1;
            else if (*f == '#') {}
            else break;
        }
        if (*f == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } f++; }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++;
            prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        /* 64-bit arguments: ll and j (long, size_t and ptrdiff_t are 32 bits on wasm32) */
        int ls = 0, longs = 0;
        for (;; f++) {
            if (*f == 'l') ls++;
            else if (*f == 'j') longs = 1;
            else if (*f != 'h' && *f != 'z' && *f != 't') break;
        }
        if (ls >= 2) longs = 1;
        if (!*f) { while (start < f) put(&o, *start++); break; }
        /* format the field into a scratch buffer, then pad it */
        char field[352];
        Out fo = {field, sizeof field, 0};
        char sign = 0;
        int numeric = 1;
        switch (*f) {
        case 'd': case 'i': {
            long long v = longs ? va_arg(ap, long long) : va_arg(ap, int);
            unsigned long long m = v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
            sign = v < 0 ? '-' : plus ? '+' : space ? ' ' : 0;
            put_u(&fo, m);
            break;
        }
        case 'u': put_u(&fo, longs ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned)); break;
        case 'x': case 'X': put_base(&fo, longs ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned), 16, *f == 'X'); break;
        case 'o': put_base(&fo, longs ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned), 8, 0); break;
        case 'p': put(&fo, '0'); put(&fo, 'x'); put_base(&fo, (uptr)va_arg(ap, void*), 16, 0); break;
        case 'f': case 'F': {
            double x = va_arg(ap, double);
            if (x < 0 || (x == 0 && 1 / x < 0)) { sign = '-'; x = -x; }
            else sign = plus ? '+' : space ? ' ' : 0;
            put_fp(&fo, x, prec < 0 ? 6 : prec > 300 ? 300 : prec);
            break;
        }
        case 's': {
            /* streamed straight to the output: a string can be longer than the scratch field */
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            uptr n = 0;
            while (s[n] && (prec < 0 || n < (uptr)prec)) n++;
            uptr spad = (uptr)width > n ? (uptr)width - n : 0;
            if (!left) while (spad) put(&o, ' '), spad--;
            for (uptr i = 0; i < n; i++) put(&o, s[i]);
            while (spad) put(&o, ' '), spad--;
            continue;
        }
        case 'c': numeric = 0; put(&fo, (char)va_arg(ap, int)); break;
        case '%': put(&o, '%'); continue;
        default:
            while (start <= f) put(&o, *start++);
            continue;
        }
        uptr flen = fo.len < sizeof field ? fo.len : sizeof field;
        if (numeric && prec >= 0 && *f != 'f' && *f != 'F' && (uptr)prec > flen) {
            /* integer precision: minimum digits */
            uptr pad = (uptr)prec - flen;
            if (flen + pad > sizeof field) pad = sizeof field - flen;
            memmove(field + pad, field, flen);
            for (uptr i = 0; i < pad; i++) field[i] = '0';
            flen += pad;
        }
        uptr total = flen + (sign ? 1 : 0);
        uptr pad = (uptr)width > total ? (uptr)width - total : 0;
        if (!left && !(zero && numeric)) while (pad) put(&o, ' '), pad--;
        if (sign) put(&o, sign);
        if (!left && zero && numeric) while (pad) put(&o, '0'), pad--;
        for (uptr i = 0; i < flen; i++) put(&o, field[i]);
        while (pad) put(&o, ' '), pad--;
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

/* Indirect calls of Salivo function values (func(int) -> int, func(int, int) -> int): std map, filter,
   reduce and sortBy pass a function as an int. Every Salivo function takes and returns 64-bit values. */
long long salivo_call_fn1(long long fn, long long a) {
    return ((long long (*)(long long))(uptr)fn)(a);
}
long long salivo_call_fn2(long long fn, long long a, long long b) {
    return ((long long (*)(long long, long long))(uptr)fn)(a, b);
}
