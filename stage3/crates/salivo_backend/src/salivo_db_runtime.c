/* Stage 36.4: unified database connection pooling with native PostgreSQL and MySQL wire-protocol
 * drivers and the embedded SQLite engine. ABI: salivo_db_abi.def (intrinsics `rtdb*`).
 *
 * - One pool type for every driver: lazy connections up to max, blocking checkout with a timeout,
 *   a health check before reuse, broken connections replaced, and a connection returned while a
 *   transaction is open is rolled back before any other borrower can see it.
 * - PostgreSQL: protocol 3.0, SCRAM-SHA-256 / MD5 / cleartext authentication, simple query.
 * - MySQL: protocol 10, mysql_native_password and caching_sha2_password (fast path, and full
 *   authentication over plain TCP with the server's RSA key, OAEP padding), COM_QUERY text results.
 * - Parameters (`?`) are bound as SQL literals quoted for the connected server; a server whose
 *   quoting rules cannot be established (PostgreSQL without standard_conforming_strings) refuses
 *   parameterised statements rather than guessing.
 * Calls block the calling OS thread; a pool may be shared by threads (checkout is thread-safe, a
 * checked-out connection belongs to one thread at a time).
 * No TLS: connections are plaintext; use them on trusted networks or through a TLS tunnel. */
#define _CRT_SECURE_NO_WARNINGS
#include "sqlite3.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
typedef SOCKET db_sock;
#define DB_BAD_SOCK INVALID_SOCKET
#define db_closesock closesocket
typedef SRWLOCK db_mutex;
typedef CONDITION_VARIABLE db_cond;
static void mtx_init(db_mutex* m) { InitializeSRWLock(m); }
static void mtx_lock(db_mutex* m) { AcquireSRWLockExclusive(m); }
static void mtx_unlock(db_mutex* m) { ReleaseSRWLockExclusive(m); }
static void cond_init(db_cond* c) { InitializeConditionVariable(c); }
static int cond_wait_ms(db_cond* c, db_mutex* m, long long ms) { return SleepConditionVariableSRW(c, m, (DWORD)(ms < 0 ? 0 : ms), 0) ? 0 : -1; }
static void cond_broadcast(db_cond* c) { WakeAllConditionVariable(c); }
static long long now_ms(void) { return (long long)GetTickCount64(); }
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
static int os_random(uint8_t* out, size_t n) { return BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1; }
static void net_init(void) {
    static volatile LONG done = 0;
    if (InterlockedCompareExchange(&done, 1, 0) == 0) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
}
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int db_sock;
#define DB_BAD_SOCK (-1)
#define db_closesock close
typedef pthread_mutex_t db_mutex;
typedef pthread_cond_t db_cond;
static void mtx_init(db_mutex* m) { pthread_mutex_init(m, NULL); }
static void mtx_lock(db_mutex* m) { pthread_mutex_lock(m); }
static void mtx_unlock(db_mutex* m) { pthread_mutex_unlock(m); }
static void cond_init(db_cond* c) { pthread_cond_init(c, NULL); }
static int cond_wait_ms(db_cond* c, db_mutex* m, long long ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(c, m, &ts) == 0 ? 0 : -1;
}
static void cond_broadcast(db_cond* c) { pthread_cond_broadcast(c); }
static void sleep_ms(int ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
static long long now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
static int os_random(uint8_t* out, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < n) { ssize_t r = read(fd, out + got, n - got); if (r <= 0) { close(fd); return -1; } got += (size_t)r; }
    close(fd);
    return 0;
}
static void net_init(void) {}
#endif

/* Strings handed to Salivo come from the program's string allocator (salivo_task_runtime.c). */
void* salivo_rt_smalloc(long long n);
static char* out_str(const char* s, size_t n) {
    char* r = (char*)salivo_rt_smalloc((long long)n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

/* Diagnostics only with SALIVO_DB_TRACE set, on stderr; never SQL text or credentials */
static void db_trace(const char* what) { if (getenv("SALIVO_DB_TRACE")) fprintf(stderr, "[db] %s\n", what); }

/* Called from an async task, blocking work moves to the async runtime's blocking pool so the
 * task suspends instead of stalling its worker thread (salivo_task_runtime.c / salivo_aio_runtime.c) */
extern int (*salivo_aio_offload_hook)(long long (*fn)(long long), long long arg, long long* out);
typedef struct { long long a, b, c; const char *s1, *s2; long long r; } DbCall;
static long long run_offloaded(long long (*fn)(long long), DbCall* call) {
    long long out;
    if (salivo_aio_offload_hook && salivo_aio_offload_hook(fn, (long long)(intptr_t)call, &out)) return out;
    return fn((long long)(intptr_t)call);
}

/* ---- byte buffer ------------------------------------------------------------------------------ */
typedef struct { uint8_t* p; size_t n, cap; } Buf;
static int buf_reserve(Buf* b, size_t extra) {
    if (b->n + extra <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + extra) cap *= 2;
    uint8_t* p = (uint8_t*)realloc(b->p, cap);
    if (!p) return -1;
    b->p = p;
    b->cap = cap;
    return 0;
}
static int buf_put(Buf* b, const void* d, size_t n) { if (buf_reserve(b, n)) return -1; memcpy(b->p + b->n, d, n); b->n += n; return 0; }
static int buf_byte(Buf* b, uint8_t c) { return buf_put(b, &c, 1); }
static int buf_str0(Buf* b, const char* s) { return buf_put(b, s, strlen(s) + 1); }
static void buf_free(Buf* b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* ---- SHA-1 (MySQL native password, OAEP) and SHA-256 helpers --------------------------------- */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t blk[64]; size_t used; } Sha1;
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
static void sha1_block(Sha1* s, const uint8_t* p) {
    uint32_t w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(Sha1* s) { s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE; s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0; s->len = 0; s->used = 0; }
static void sha1_update(Sha1* s, const uint8_t* p, size_t n) {
    s->len += n;
    while (n) {
        size_t k = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->blk + s->used, p, k);
        s->used += k; p += k; n -= k;
        if (s->used == 64) { sha1_block(s, s->blk); s->used = 0; }
    }
}
static void sha1_final(Sha1* s, uint8_t out[20]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, z = 0;
    sha1_update(s, &pad, 1);
    while (s->used != 56) sha1_update(s, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, lb, 8);
    for (int i = 0; i < 5; i++) { out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16); out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i]; }
}
static void sha1(const uint8_t* p, size_t n, uint8_t out[20]) { Sha1 s; sha1_init(&s); sha1_update(&s, p, n); sha1_final(&s, out); }

typedef struct { uint32_t state[8]; uint64_t bitlen; uint8_t data[64]; uint32_t datalen; } SalivoSha256;
void salivo_sha256_init(SalivoSha256* ctx);
void salivo_sha256_update(SalivoSha256* ctx, const uint8_t* data, size_t len);
void salivo_sha256_final(SalivoSha256* ctx, uint8_t* digest);
static void sha256(const uint8_t* p, size_t n, uint8_t out[32]) { SalivoSha256 s; salivo_sha256_init(&s); salivo_sha256_update(&s, p, n); salivo_sha256_final(&s, out); }
static void hmac256(const uint8_t* key, size_t kn, const uint8_t* msg, size_t mn, uint8_t out[32]) {
    uint8_t k[64] = {0}, ip[64], op[64], inner[32];
    if (kn > 64) sha256(key, kn, k); else memcpy(k, key, kn);
    for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    SalivoSha256 s;
    salivo_sha256_init(&s); salivo_sha256_update(&s, ip, 64); salivo_sha256_update(&s, msg, mn); salivo_sha256_final(&s, inner);
    salivo_sha256_init(&s); salivo_sha256_update(&s, op, 64); salivo_sha256_update(&s, inner, 32); salivo_sha256_final(&s, out);
}
static void pbkdf2_256(const char* pw, const uint8_t* salt, size_t sn, int iters, uint8_t out[32]) {
    uint8_t u[32], msg[256];
    if (sn > 250) sn = 250;
    memcpy(msg, salt, sn);
    msg[sn] = 0; msg[sn + 1] = 0; msg[sn + 2] = 0; msg[sn + 3] = 1;
    hmac256((const uint8_t*)pw, strlen(pw), msg, sn + 4, u);
    memcpy(out, u, 32);
    for (int i = 1; i < iters; i++) {
        hmac256((const uint8_t*)pw, strlen(pw), u, 32, u);
        for (int j = 0; j < 32; j++) out[j] ^= u[j];
    }
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64enc(const uint8_t* p, size_t n, Buf* o) {
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        buf_byte(o, (uint8_t)B64[v >> 18]); buf_byte(o, (uint8_t)B64[(v >> 12) & 63]);
        buf_byte(o, i + 1 < n ? (uint8_t)B64[(v >> 6) & 63] : '=');
        buf_byte(o, i + 2 < n ? (uint8_t)B64[v & 63] : '=');
    }
}
static int b64val(int c) { const char* q = c ? strchr(B64, c) : NULL; return q ? (int)(q - B64) : -1; }
static int b64dec(const char* s, size_t n, Buf* o) {
    uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '=' ) break;
        if (s[i] == '\n' || s[i] == '\r') continue;
        int v = b64val((unsigned char)s[i]);
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; buf_byte(o, (uint8_t)(acc >> bits)); }
    }
    return 0;
}

/* ---- RSA public-key encryption with OAEP (SHA-1), for MySQL caching_sha2 full auth -------- */
#define BN_LIMBS 130 /* up to 4160-bit moduli */
typedef struct { uint32_t d[BN_LIMBS * 2]; int n; } Bn;
static void bn_trim(Bn* a) { while (a->n > 0 && a->d[a->n - 1] == 0) a->n--; }
static void bn_from_bytes(Bn* a, const uint8_t* p, size_t len) {
    memset(a, 0, sizeof *a);
    for (size_t i = 0; i < len; i++) { size_t bit = (len - 1 - i) * 8; a->d[bit / 32] |= (uint32_t)p[i] << (bit % 32); }
    a->n = (int)((len + 3) / 4);
    bn_trim(a);
}
static void bn_to_bytes(const Bn* a, uint8_t* p, size_t len) {
    for (size_t i = 0; i < len; i++) { size_t bit = (len - 1 - i) * 8; p[i] = (size_t)(bit / 32) < (size_t)a->n ? (uint8_t)(a->d[bit / 32] >> (bit % 32)) : 0; }
}
static int bn_cmp(const Bn* a, const Bn* b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}
static int bn_bits(const Bn* a) { if (!a->n) return 0; int b = 32; uint32_t top = a->d[a->n - 1]; while (!(top & 0x80000000u)) { top <<= 1; b--; } return (a->n - 1) * 32 + b; }
static void bn_shl1(Bn* a) {
    uint32_t carry = 0;
    for (int i = 0; i < a->n; i++) { uint32_t v = a->d[i]; a->d[i] = v << 1 | carry; carry = v >> 31; }
    if (carry) a->d[a->n++] = carry;
}
static void bn_sub(Bn* a, const Bn* b) { /* a -= b, a >= b */
    int64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t v = (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0) - borrow;
        borrow = v < 0;
        a->d[i] = (uint32_t)(v + (borrow ? ((int64_t)1 << 32) : 0));
    }
    bn_trim(a);
}
/* r = (a * b) mod m by shift-and-subtract; ponytail: O(bits^2) per multiply, fine for one RSA
 * encryption per connection (e = 65537 needs 17 multiplies) */
static void bn_mulmod(const Bn* a, const Bn* b, const Bn* m, Bn* r) {
    Bn acc; memset(&acc, 0, sizeof acc);
    for (int i = bn_bits(b) - 1; i >= 0; i--) {
        bn_shl1(&acc);
        if (bn_cmp(&acc, m) >= 0) bn_sub(&acc, m);
        if (b->d[i / 32] >> (i % 32) & 1) {
            uint64_t carry = 0;
            int n = a->n > acc.n ? a->n : acc.n;
            for (int k = 0; k < n; k++) { uint64_t v = (uint64_t)(k < acc.n ? acc.d[k] : 0) + (k < a->n ? a->d[k] : 0) + carry; acc.d[k] = (uint32_t)v; carry = v >> 32; }
            acc.n = n;
            if (carry) acc.d[acc.n++] = (uint32_t)carry;
            if (bn_cmp(&acc, m) >= 0) bn_sub(&acc, m);
        }
    }
    *r = acc;
}
static void bn_powmod(const Bn* base, const Bn* e, const Bn* m, Bn* r) {
    Bn result; memset(&result, 0, sizeof result); result.d[0] = 1; result.n = 1;
    for (int i = bn_bits(e) - 1; i >= 0; i--) {
        bn_mulmod(&result, &result, m, &result);
        if (e->d[i / 32] >> (i % 32) & 1) bn_mulmod(&result, base, m, &result);
    }
    *r = result;
}
/* DER: read tag/length at *p, return content length or -1 */
static long der_hdr(const uint8_t** p, const uint8_t* end, int tag) {
    if (*p >= end || **p != tag) return -1;
    (*p)++;
    if (*p >= end) return -1;
    long len = **p; (*p)++;
    if (len & 0x80) {
        int k = (int)(len & 0x7f); len = 0;
        if (k > 4 || *p + k > end) return -1;
        while (k--) { len = len << 8 | **p; (*p)++; }
    }
    return *p + len <= end ? len : -1;
}
/* PEM "-----BEGIN PUBLIC KEY-----" (SubjectPublicKeyInfo) -> modulus and exponent */
static int rsa_parse_pem(const char* pem, Bn* n, Bn* e, size_t* nlen) {
    const char* a = strstr(pem, "-----BEGIN PUBLIC KEY-----");
    const char* z = strstr(pem, "-----END PUBLIC KEY-----");
    if (!a || !z) return -1;
    a += 26;
    Buf der = {0};
    if (b64dec(a, (size_t)(z - a), &der)) { buf_free(&der); return -1; }
    const uint8_t *p = der.p, *end = der.p + der.n;
    long l;
    int rc = -1;
    if ((l = der_hdr(&p, end, 0x30)) < 0) goto out;
    if ((l = der_hdr(&p, end, 0x30)) < 0) goto out;
    p += l; /* AlgorithmIdentifier */
    if ((l = der_hdr(&p, end, 0x03)) < 1) goto out;
    p++; /* unused-bits byte */
    if ((l = der_hdr(&p, end, 0x30)) < 0) goto out;
    if ((l = der_hdr(&p, end, 0x02)) < 1) goto out;
    { const uint8_t* m = p; long ml = l; while (ml > 0 && *m == 0) { m++; ml--; } if (ml > BN_LIMBS * 4 - 8) goto out; bn_from_bytes(n, m, (size_t)ml); *nlen = (size_t)ml; }
    p += l;
    if ((l = der_hdr(&p, end, 0x02)) < 1 || l > 8) goto out;
    bn_from_bytes(e, p, (size_t)l);
    rc = 0;
out:
    buf_free(&der);
    return rc;
}
static void mgf1_sha1(const uint8_t* seed, size_t sn, uint8_t* mask, size_t mn) {
    uint8_t in[512 + 4], h[20];
    memcpy(in, seed, sn);
    for (uint32_t c = 0, off = 0; off < mn; c++) {
        in[sn] = (uint8_t)(c >> 24); in[sn + 1] = (uint8_t)(c >> 16); in[sn + 2] = (uint8_t)(c >> 8); in[sn + 3] = (uint8_t)c;
        sha1(in, sn + 4, h);
        for (int i = 0; i < 20 && off < mn; i++) mask[off++] = h[i];
    }
}
/* RSAES-OAEP (SHA-1, empty label) of msg under the PEM key; out receives k bytes */
static int rsa_oaep_encrypt(const char* pem, const uint8_t* msg, size_t mlen, uint8_t* out, size_t* olen) {
    Bn n, e, m, c;
    size_t k;
    if (rsa_parse_pem(pem, &n, &e, &k)) return -1;
    if (k < 2 * 20 + 2 || mlen > k - 2 * 20 - 2 || k > 512) return -1;
    uint8_t em[512], db[512], seed[20], mask[512];
    size_t dblen = k - 20 - 1;
    memset(db, 0, dblen);
    sha1((const uint8_t*)"", 0, db); /* lHash */
    db[dblen - mlen - 1] = 1;
    memcpy(db + dblen - mlen, msg, mlen);
    if (os_random(seed, 20)) return -1;
    mgf1_sha1(seed, 20, mask, dblen);
    for (size_t i = 0; i < dblen; i++) db[i] ^= mask[i];
    mgf1_sha1(db, dblen, mask, 20);
    em[0] = 0;
    for (int i = 0; i < 20; i++) em[1 + i] = seed[i] ^ mask[i];
    memcpy(em + 21, db, dblen);
    bn_from_bytes(&m, em, k);
    bn_powmod(&m, &e, &n, &c);
    bn_to_bytes(&c, out, k);
    *olen = k;
    return 0;
}

/* ---- result sets ------------------------------------------------------------------------------ */
/* Cancellation token: cancelling it stops the checkout or statement it was passed to */
typedef struct { volatile int cancelled; int gen, used; } Token;
/* A row stream reads one row at a time; its lease stays busy until the last row or an early close */
typedef struct {
    int lease, done, failed, tok_gen;
    Token* tok;
    sqlite3_stmt* st;
    char** row; /* the row being read, swapped with the visible one */
    Buf in;
} Stream;
typedef struct {
    int cols, rows, cap_rows;
    char** names;
    char** cells; /* rows * cols, NULL = SQL NULL */
    long long affected, last_id;
    Stream* s; /* row streams only: rows is 1 while a current row is visible */
} Result;
static void free_row(char** row, int n) { for (int i = 0; row && i < n; i++) { free(row[i]); row[i] = NULL; } }
static void result_free(Result* r) {
    if (!r) return;
    for (int i = 0; i < r->cols; i++) free(r->names[i]);
    for (long i = 0; i < (long)r->rows * r->cols; i++) free(r->cells[i]);
    if (r->s) { free_row(r->s->row, r->cols); free(r->s->row); if (r->s->st) sqlite3_finalize(r->s->st); buf_free(&r->s->in); free(r->s); }
    free(r->names); free(r->cells); free(r);
}
static int result_add_row(Result* r) {
    if (r->rows == r->cap_rows) {
        int cap = r->cap_rows ? r->cap_rows * 2 : 16;
        char** c = (char**)realloc(r->cells, sizeof(char*) * (size_t)cap * (size_t)(r->cols ? r->cols : 1));
        if (!c) return -1;
        r->cells = c; r->cap_rows = cap;
    }
    for (int i = 0; i < r->cols; i++) r->cells[(long)r->rows * r->cols + i] = NULL;
    r->rows++;
    return 0;
}
static char* dup_n(const char* s, size_t n) { char* r = (char*)malloc(n + 1); if (r) { memcpy(r, s, n); r[n] = 0; } return r; }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* ---- connections ------------------------------------------------------------------------------ */
enum { DRV_SQLITE = 1, DRV_PG = 2, DRV_MYSQL = 3 };
typedef struct { int driver; char host[256], user[128], pass[128], db[128], path[512]; int port; } DbUrl;
typedef struct {
    int driver;
    db_sock sock;
    sqlite3* lite;
    int broken, in_txn, std_strings, no_backslash;
    long long last_used, created;
    char err[512];
    uint8_t seq; /* mysql packet sequence */
    DbUrl url;   /* for the separate connection that carries a cancel request */
    uint32_t pg_pid, pg_secret, my_thread;
    long long connect_ms; /* connect timeout, also how long a cancelled statement may take to stop */
    /* The operation in progress. deadline 0 = none. A statement (in_query) that is cancelled or
     * passes its deadline gets a server-side cancel; stop then records why (1 cancelled, 2 timed
     * out) and grace bounds the wait for the server's answer. Anything else just fails. */
    long long deadline, grace;
    Token* tok;
    int tok_gen, in_query, stop;
    volatile int kill; /* dbCloseWait ran out of time: cancel what runs, refuse what comes */
} Conn;

static void set_err(Conn* c, const char* fmt, const char* a) { snprintf(c->err, sizeof c->err, fmt, a ? a : ""); }

/* ---- sockets with deadlines and cancellation --------------------------------------------------- */
static int sock_poll(db_sock s, int wr, int ms) {
#ifdef _WIN32
    fd_set f, e;
    FD_ZERO(&f); FD_ZERO(&e); FD_SET(s, &f); FD_SET(s, &e);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return select(0, wr ? NULL : &f, wr ? &f : NULL, &e, &tv);
#else
    struct pollfd p = { s, (short)(wr ? POLLOUT : POLLIN), 0 };
    int r = poll(&p, 1, ms);
    return r < 0 && errno == EINTR ? 0 : r;
#endif
}
static void sock_blocking(db_sock s, int on) {
#ifdef _WIN32
    u_long nb = !on;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? fl & ~O_NONBLOCK : fl | O_NONBLOCK);
#endif
}
static int connect_pending(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS;
#endif
}
static int sock_error(db_sock s) { int e = 0; socklen_t n = sizeof e; getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&e, &n); return e != 0; }
static int tok_cancelled(const Token* t, int gen) { return t && t->gen == gen && t->cancelled; }
/* 0 keep going, 1 cancelled (token or shutdown), 2 deadline passed */
static int conn_stop_reason(const Conn* c) {
    if (c->kill || tok_cancelled(c->tok, c->tok_gen)) return 1;
    return c->deadline && now_ms() >= c->deadline ? 2 : 0;
}
static void conn_send_cancel(Conn* c);
/* Waits until the socket is ready, checking every 50 ms for a cancel or the deadline. */
static int conn_wait(Conn* c, int wr) {
    for (;;) {
        int why = conn_stop_reason(c);
        if (why && !c->in_query) { set_err(c, why == 1 ? "operation cancelled%s" : "timed out%s", ""); return -1; }
        if (why && !c->stop) { c->stop = why; c->grace = now_ms() + c->connect_ms; conn_send_cancel(c); }
        if (c->stop && now_ms() >= c->grace) { set_err(c, "the server did not answer the cancel request%s", ""); return -1; }
        int r = sock_poll(c->sock, wr, 50);
        if (r > 0) return 0;
        if (r < 0) { set_err(c, "connection lost%s", ""); return -1; }
    }
}
static int conn_send(Conn* c, const uint8_t* p, size_t n) {
    while (n) {
        if (conn_wait(c, 1)) return -1;
        int r = send(c->sock, (const char*)p, (int)(n > 65536 ? 65536 : n), 0);
        if (r <= 0) { set_err(c, "connection lost%s", ""); return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}
static int conn_recv(Conn* c, uint8_t* p, size_t n) {
    while (n) {
        if (conn_wait(c, 0)) return -1;
        int r = recv(c->sock, (char*)p, (int)(n > 65536 ? 65536 : n), 0);
        if (r <= 0) { set_err(c, "connection lost%s", ""); return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}
/* Connects c->sock before c->deadline; a cancel aborts the attempt */
static int tcp_connect(Conn* c, const char* host, int port) {
    net_init();
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    /* ponytail: name resolution is not bounded by the connect timeout; an async resolver if slow DNS shows up */
    if (getaddrinfo(host, ps, &hints, &res) != 0) { set_err(c, "cannot resolve %s", host); return -1; }
    for (struct addrinfo* a = res; a && c->sock == DB_BAD_SOCK; a = a->ai_next) {
        db_sock s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == DB_BAD_SOCK) continue;
        sock_blocking(s, 0);
        int r = connect(s, a->ai_addr, (int)a->ai_addrlen);
        c->sock = s;
        if (r != 0 && (!connect_pending() || conn_wait(c, 1) || sock_error(s))) {
            db_closesock(s);
            c->sock = DB_BAD_SOCK;
            if (conn_stop_reason(c)) break;
            continue;
        }
        sock_blocking(s, 1);
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
    }
    freeaddrinfo(res);
    if (c->sock == DB_BAD_SOCK) { if (!c->err[0]) set_err(c, "cannot connect to %s", host); return -1; }
    return 0;
}

/* url: postgres://user:pass@host:port/db | mysql://user:pass@host:port/db | sqlite:path | sqlite::memory: */
static int parse_url(const char* url, DbUrl* u) {
    memset(u, 0, sizeof *u);
    if (!url) return -1;
    if (!strncmp(url, "sqlite:", 7)) { u->driver = DRV_SQLITE; snprintf(u->path, sizeof u->path, "%s", url + 7); return u->path[0] ? 0 : -1; }
    const char* rest;
    if (!strncmp(url, "postgres://", 11)) { u->driver = DRV_PG; u->port = 5432; rest = url + 11; }
    else if (!strncmp(url, "postgresql://", 13)) { u->driver = DRV_PG; u->port = 5432; rest = url + 13; }
    else if (!strncmp(url, "mysql://", 8)) { u->driver = DRV_MYSQL; u->port = 3306; rest = url + 8; }
    else return -1;
    const char* at = strrchr(rest, '@');
    const char* hostp = rest;
    if (at) {
        const char* colon = memchr(rest, ':', (size_t)(at - rest));
        size_t ul = (size_t)((colon ? colon : at) - rest);
        if (ul >= sizeof u->user) return -1;
        memcpy(u->user, rest, ul);
        if (colon) { size_t pl = (size_t)(at - colon - 1); if (pl >= sizeof u->pass) return -1; memcpy(u->pass, colon + 1, pl); }
        hostp = at + 1;
    }
    const char* slash = strchr(hostp, '/');
    const char* hend = slash ? slash : hostp + strlen(hostp);
    const char* pc = memchr(hostp, ':', (size_t)(hend - hostp));
    size_t hl = (size_t)((pc ? pc : hend) - hostp);
    if (!hl || hl >= sizeof u->host) return -1;
    memcpy(u->host, hostp, hl);
    if (pc) { u->port = atoi(pc + 1); if (u->port <= 0 || u->port > 65535) return -1; }
    if (slash) snprintf(u->db, sizeof u->db, "%s", slash + 1);
    return u->user[0] ? 0 : -1;
}

/* PostgreSQL ------------------------------------------------------------------------------------ */
static int pg_read(Conn* c, uint8_t* type, Buf* body) {
    uint8_t h[5];
    if (conn_recv(c, h, 5)) { c->broken = 1; return -1; }
    uint32_t len = (uint32_t)h[1] << 24 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 8 | h[4];
    if (len < 4 || len > (1u << 30)) { c->broken = 1; set_err(c, "protocol error%s", ""); return -1; }
    body->n = 0;
    if (buf_reserve(body, len - 4 + 1)) return -1;
    if (conn_recv(c, body->p, len - 4)) { c->broken = 1; return -1; }
    body->n = len - 4;
    body->p[body->n] = 0;
    *type = h[0];
    return 0;
}
static int pg_send(Conn* c, uint8_t type, const Buf* body) {
    uint8_t h[5];
    uint32_t len = (uint32_t)body->n + 4;
    h[0] = type; h[1] = (uint8_t)(len >> 24); h[2] = (uint8_t)(len >> 16); h[3] = (uint8_t)(len >> 8); h[4] = (uint8_t)len;
    if (conn_send(c, h, 5) || conn_send(c, body->p, body->n)) { c->broken = 1; return -1; }
    return 0;
}
static void pg_error(Conn* c, const Buf* b) {
    const char *msg = "", *code = "";
    for (size_t i = 0; i < b->n && b->p[i];) {
        char f = (char)b->p[i];
        const char* v = (const char*)b->p + i + 1;
        if (f == 'M') msg = v;
        if (f == 'C') code = v;
        i += strlen(v) + 2;
    }
    snprintf(c->err, sizeof c->err, "%s (SQLSTATE %s)", msg, code);
}
static int scram_attr(const char* s, char key, char* out, size_t cap) {
    for (const char* p = s; *p;) {
        if (p[0] == key && p[1] == '=') {
            const char* e = strchr(p + 2, ',');
            size_t n = e ? (size_t)(e - p - 2) : strlen(p + 2);
            if (n >= cap) return -1;
            memcpy(out, p + 2, n); out[n] = 0;
            return 0;
        }
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    return -1;
}
static int pg_scram(Conn* c, const DbUrl* u, Buf* in) {
    if (!strstr((const char*)in->p, "SCRAM-SHA-256")) { set_err(c, "server offers no supported SASL mechanism%s", ""); return -1; }
    uint8_t nonce_raw[18];
    if (os_random(nonce_raw, sizeof nonce_raw)) return -1;
    Buf nonce = {0}; b64enc(nonce_raw, sizeof nonce_raw, &nonce); buf_byte(&nonce, 0);
    char first_bare[256];
    snprintf(first_bare, sizeof first_bare, "n=,r=%s", (char*)nonce.p);
    Buf m = {0};
    buf_str0(&m, "SCRAM-SHA-256");
    char cf[300]; snprintf(cf, sizeof cf, "n,,%s", first_bare);
    uint32_t l = (uint32_t)strlen(cf);
    uint8_t lb[4] = { (uint8_t)(l >> 24), (uint8_t)(l >> 16), (uint8_t)(l >> 8), (uint8_t)l };
    buf_put(&m, lb, 4); buf_put(&m, cf, l);
    int rc = -1;
    uint8_t t;
    char server_first[1024], r[512], s64[256], ibuf[32];
    if (pg_send(c, 'p', &m) || pg_read(c, &t, in)) goto out;
    if (t == 'E') { pg_error(c, in); goto out; }
    if (t != 'R' || in->n < 4 || in->p[3] != 11 || in->n - 4 >= sizeof server_first) { set_err(c, "unexpected SASL reply%s", ""); goto out; }
    memcpy(server_first, in->p + 4, in->n - 4); server_first[in->n - 4] = 0;
    if (scram_attr(server_first, 'r', r, sizeof r) || scram_attr(server_first, 's', s64, sizeof s64) || scram_attr(server_first, 'i', ibuf, sizeof ibuf)) { set_err(c, "malformed SCRAM server message%s", ""); goto out; }
    if (strncmp(r, (char*)nonce.p, strlen((char*)nonce.p))) { set_err(c, "SCRAM nonce mismatch%s", ""); goto out; }
    int iters = atoi(ibuf);
    if (iters < 1 || iters > 10000000) { set_err(c, "bad SCRAM iteration count%s", ""); goto out; }
    Buf salt = {0};
    if (b64dec(s64, strlen(s64), &salt)) { buf_free(&salt); goto out; }
    uint8_t salted[32], ckey[32], skey[32], stored[32], sig[32], proof[32], ssig[32];
    pbkdf2_256(u->pass, salt.p, salt.n, iters, salted);
    buf_free(&salt);
    hmac256(salted, 32, (const uint8_t*)"Client Key", 10, ckey);
    sha256(ckey, 32, stored);
    char final_wo[600];
    snprintf(final_wo, sizeof final_wo, "c=biws,r=%s", r);
    char auth[2048];
    snprintf(auth, sizeof auth, "%s,%s,%s", first_bare, server_first, final_wo);
    hmac256(stored, 32, (const uint8_t*)auth, strlen(auth), sig);
    for (int i = 0; i < 32; i++) proof[i] = ckey[i] ^ sig[i];
    hmac256(salted, 32, (const uint8_t*)"Server Key", 10, skey);
    hmac256(skey, 32, (const uint8_t*)auth, strlen(auth), ssig);
    Buf fin = {0}, p64 = {0};
    b64enc(proof, 32, &p64); buf_byte(&p64, 0);
    char fm[800]; snprintf(fm, sizeof fm, "%s,p=%s", final_wo, (char*)p64.p);
    buf_put(&fin, fm, strlen(fm));
    int sent = pg_send(c, 'p', &fin);
    buf_free(&fin); buf_free(&p64);
    if (sent || pg_read(c, &t, in)) goto out;
    if (t == 'E') { pg_error(c, in); goto out; }
    if (t != 'R' || in->n < 4 || in->p[3] != 12) { set_err(c, "unexpected SASL final%s", ""); goto out; }
    /* the server must prove it knows the password too */
    Buf v64 = {0}; b64enc(ssig, 32, &v64); buf_byte(&v64, 0);
    char vexp[128]; snprintf(vexp, sizeof vexp, "v=%s", (char*)v64.p);
    buf_free(&v64);
    if (in->n - 4 != strlen(vexp) || memcmp(in->p + 4, vexp, strlen(vexp))) { set_err(c, "server SCRAM signature invalid%s", ""); goto out; }
    rc = 0;
out:
    buf_free(&m); buf_free(&nonce);
    return rc;
}
static void md5_hex(const uint8_t* p, size_t n, char out[33]);
static int pg_connect(Conn* c, const DbUrl* u) {
    if (tcp_connect(c, u->host, u->port)) return -1;
    Buf m = {0}, in = {0};
    uint8_t ver[4] = { 0, 3, 0, 0 };
    buf_put(&m, ver, 4);
    buf_str0(&m, "user"); buf_str0(&m, u->user);
    buf_str0(&m, "database"); buf_str0(&m, u->db[0] ? u->db : u->user);
    buf_str0(&m, "client_encoding"); buf_str0(&m, "UTF8");
    buf_byte(&m, 0);
    uint8_t h[4]; uint32_t len = (uint32_t)m.n + 4;
    h[0] = (uint8_t)(len >> 24); h[1] = (uint8_t)(len >> 16); h[2] = (uint8_t)(len >> 8); h[3] = (uint8_t)len;
    int rc = -1;
    if (conn_send(c, h, 4) || conn_send(c, m.p, m.n)) goto out;
    for (;;) {
        uint8_t t;
        if (pg_read(c, &t, &in)) goto out;
        if (t == 'E') { pg_error(c, &in); goto out; }
        if (t == 'R') {
            uint32_t code = in.n >= 4 ? (uint32_t)in.p[0] << 24 | (uint32_t)in.p[1] << 16 | (uint32_t)in.p[2] << 8 | in.p[3] : 99;
            if (code == 0) continue;
            if (code == 3) { db_trace("postgres auth: cleartext"); Buf pw = {0}; buf_str0(&pw, u->pass); int s = pg_send(c, 'p', &pw); buf_free(&pw); if (s) goto out; continue; }
            if (code == 5 && in.n >= 8) {
                db_trace("postgres auth: md5");
                char h1[33], h2[33], tmp[300];
                snprintf(tmp, sizeof tmp, "%s%s", u->pass, u->user);
                md5_hex((const uint8_t*)tmp, strlen(tmp), h1);
                uint8_t cat[36]; memcpy(cat, h1, 32); memcpy(cat + 32, in.p + 4, 4);
                md5_hex(cat, 36, h2);
                Buf pw = {0}; buf_put(&pw, "md5", 3); buf_str0(&pw, h2);
                int s = pg_send(c, 'p', &pw); buf_free(&pw);
                if (s) goto out;
                continue;
            }
            if (code == 10) { db_trace("postgres auth: scram-sha-256"); memmove(in.p, in.p + 4, in.n - 4); in.n -= 4; in.p[in.n] = 0; if (pg_scram(c, u, &in)) goto out; continue; }
            set_err(c, "unsupported PostgreSQL authentication method%s", ""); goto out;
        }
        if (t == 'S' && in.n) {
            const char* k = (const char*)in.p;
            const char* v = strlen(k) + 1 < in.n ? k + strlen(k) + 1 : "";
            if (!strcmp(k, "standard_conforming_strings")) c->std_strings = !strcmp(v, "on");
            continue;
        }
        if (t == 'K' && in.n >= 8) { c->pg_pid = be32(in.p); c->pg_secret = be32(in.p + 4); continue; } /* for cancel requests */
        if (t == 'Z') { c->in_txn = in.n && in.p[0] != 'I'; rc = 0; break; }
        /* N (notice) and others are not needed */
    }
out:
    buf_free(&m); buf_free(&in);
    return rc;
}
/* Reads one message of a simple-query response into r. Returns 1 for a DataRow (stored in
 * stream_row, or appended to r without one), 2 for ReadyForQuery, 3 for a RowDescription, 0 for
 * anything else and -1 when the connection broke. A server error sets *failed; the caller reads on
 * to ReadyForQuery. A stream takes one row-returning statement: a second RowDescription fails it. */
static int pg_step(Conn* c, Buf* in, Result* r, char** stream_row, int* failed) {
    uint8_t t;
    if (pg_read(c, &t, in)) return -1;
    if ((t == 'T' || t == 'D') && in->n < 2) goto bad;
    if (t == 'T') {
        if (stream_row) { if (!*failed) set_err(c, "a stream runs one row-returning statement%s", ""); *failed = 1; return 0; }
        int n = in->p[0] << 8 | in->p[1];
        /* a later statement's rows replace an earlier one's */
        for (int i = 0; i < r->cols; i++) free(r->names[i]);
        for (long i = 0; i < (long)r->rows * r->cols; i++) free(r->cells[i]);
        free(r->names); free(r->cells);
        r->cells = NULL; r->rows = r->cap_rows = 0;
        r->cols = n;
        r->names = (char**)calloc((size_t)(n ? n : 1), sizeof(char*));
        size_t off = 2;
        for (int i = 0; i < n; i++) {
            const uint8_t* z = off < in->n ? memchr(in->p + off, 0, in->n - off) : NULL;
            if (!z || (size_t)(z - in->p) + 1 + 18 > in->n) goto bad;
            size_t l = (size_t)(z - in->p) - off;
            r->names[i] = dup_n((char*)in->p + off, l); off += l + 1 + 18;
        }
        return 3;
    }
    if (t == 'D') {
        if (*failed) return 0;
        int n = in->p[0] << 8 | in->p[1];
        if (n != r->cols) goto bad;
        char** row = stream_row;
        if (!row) { if (result_add_row(r)) goto bad; row = &r->cells[(long)(r->rows - 1) * r->cols]; }
        size_t off = 2;
        for (int i = 0; i < n; i++) {
            if (off + 4 > in->n) goto bad;
            int32_t l = (int32_t)be32(in->p + off);
            off += 4;
            if (l < -1 || (l > 0 && (size_t)l > in->n - off)) goto bad;
            if (l >= 0) { row[i] = dup_n((char*)in->p + off, (size_t)l); off += (size_t)l; }
        }
        return 1;
    }
    if (t == 'C') {
        const char* tag = (const char*)in->p;
        const char* sp = strrchr(tag, ' ');
        if (sp && sp[1] >= '0' && sp[1] <= '9' && strncmp(tag, "SELECT", 6)) r->affected = atoll(sp + 1);
    } else if (t == 'E') {
        if (!*failed) pg_error(c, in);
        *failed = 1;
    } else if (t == 'Z') {
        c->in_txn = in->n && in->p[0] != 'I';
        return 2;
    }
    return 0;
bad:
    set_err(c, "protocol error%s", "");
    c->broken = 1;
    return -1;
}
static Result* pg_query(Conn* c, const char* sql) {
    Buf q = {0}, in = {0};
    buf_str0(&q, sql);
    Result* r = (Result*)calloc(1, sizeof(Result));
    int failed = 0, k = 0;
    r->affected = -1;
    if (pg_send(c, 'Q', &q)) failed = 1;
    else while ((k = pg_step(c, &in, r, NULL, &failed)) != 2) if (k < 0) { failed = 1; break; }
    buf_free(&q); buf_free(&in);
    if (failed) { result_free(r); return NULL; }
    if (r->affected < 0 && r->cols == 0) r->affected = 0; /* BEGIN, CREATE ...: no row count */
    return r;
}

/* MD5 for PostgreSQL md5 auth (RFC 1321) */
static void md5_hex(const uint8_t* msg, size_t n, char out[33]) {
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
    static const uint8_t S[64] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
    uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;
    size_t total = ((n + 8) / 64 + 1) * 64;
    uint8_t* m = (uint8_t*)calloc(total, 1);
    memcpy(m, msg, n); m[n] = 0x80;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) m[total - 8 + i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[16], a = h0, b = h1, c = h2, d = h3;
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)m[off + 4 * i] | (uint32_t)m[off + 4 * i + 1] << 8 | (uint32_t)m[off + 4 * i + 2] << 16 | (uint32_t)m[off + 4 * i + 3] << 24;
        for (int i = 0; i < 64; i++) {
            uint32_t f; int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            uint32_t t = d; d = c; c = b;
            b = b + ROL(a + f + K[i] + w[g], S[i]);
            a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
    }
    free(m);
    uint32_t hs[4] = { h0, h1, h2, h3 };
    for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", (hs[i / 4] >> (8 * (i % 4))) & 0xff);
}

/* MySQL ---------------------------------------------------------------------------------------- */
static int my_read(Conn* c, Buf* b) {
    uint8_t h[4];
    b->n = 0;
    for (;;) {
        if (conn_recv(c, h, 4)) { c->broken = 1; return -1; }
        size_t len = (size_t)h[0] | (size_t)h[1] << 8 | (size_t)h[2] << 16;
        c->seq = (uint8_t)(h[3] + 1);
        if (buf_reserve(b, len + 1)) return -1;
        if (conn_recv(c, b->p + b->n, len)) { c->broken = 1; return -1; }
        b->n += len;
        if (len < 0xFFFFFF) break;
    }
    b->p[b->n] = 0;
    return 0;
}
static int my_send(Conn* c, const uint8_t* p, size_t n) {
    uint8_t h[4] = { (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), c->seq++ };
    if (n >= 0xFFFFFF) { set_err(c, "statement too large%s", ""); return -1; }
    if (conn_send(c, h, 4) || conn_send(c, p, n)) { c->broken = 1; return -1; }
    return 0;
}
static uint64_t my_lenenc(const uint8_t** p, const uint8_t* end, int* is_null) {
    *is_null = -1;
    if (*p >= end) return 0;
    uint8_t f = *(*p)++;
    if (f == 0xff) return 0;
    *is_null = f == 0xfb;
    if (f <= 0xfb) return f == 0xfb ? 0 : f;
    int k = f == 0xfc ? 2 : f == 0xfd ? 3 : 8;
    if (end - *p < k) { *is_null = -1; return 0; }
    uint64_t v = 0;
    for (int i = 0; i < k; i++) v |= (uint64_t)*(*p)++ << (8 * i);
    return v;
}
static void my_err(Conn* c, const Buf* b) {
    int code = b->n >= 3 ? b->p[1] | b->p[2] << 8 : 0;
    const char* msg = b->n > 9 && b->p[3] == '#' ? (const char*)b->p + 9 : b->n > 3 ? (const char*)b->p + 3 : "";
    snprintf(c->err, sizeof c->err, "%s (MySQL error %d)", msg, code);
}
static void my_ok(Conn* c, const Buf* b, Result* r) {
    const uint8_t *p = b->p + 1, *end = b->p + b->n;
    int nul;
    uint64_t aff = my_lenenc(&p, end, &nul), id = my_lenenc(&p, end, &nul);
    uint16_t status = p + 2 <= end ? (uint16_t)(p[0] | p[1] << 8) : 0;
    c->in_txn = status & 1;
    c->no_backslash = (status & 0x200) != 0;
    if (r) { r->affected = (long long)aff; r->last_id = (long long)id; }
}
static void xor_bytes(uint8_t* a, const uint8_t* b, size_t n) { for (size_t i = 0; i < n; i++) a[i] ^= b[i]; }
static void my_native_scramble(const char* pw, const uint8_t* salt, uint8_t out[20]) {
    uint8_t h1[20], h2[20], cat[40];
    sha1((const uint8_t*)pw, strlen(pw), h1);
    sha1(h1, 20, h2);
    memcpy(cat, salt, 20); memcpy(cat + 20, h2, 20);
    sha1(cat, 40, out);
    xor_bytes(out, h1, 20);
}
static void my_sha2_scramble(const char* pw, const uint8_t* salt, uint8_t out[32]) {
    uint8_t h1[32], h2[32], cat[52];
    sha256((const uint8_t*)pw, strlen(pw), h1);
    sha256(h1, 32, h2);
    memcpy(cat, h2, 32); memcpy(cat + 32, salt, 20);
    sha256(cat, 52, out);
    xor_bytes(out, h1, 32);
}
static int my_auth_response(const char* plugin, const char* pw, const uint8_t* salt, uint8_t* out, size_t* n) {
    if (!pw[0]) { *n = 0; return 0; }
    if (!strcmp(plugin, "mysql_native_password")) { my_native_scramble(pw, salt, out); *n = 20; return 0; }
    if (!strcmp(plugin, "caching_sha2_password")) { my_sha2_scramble(pw, salt, out); *n = 32; return 0; }
    return -1;
}
static int my_connect(Conn* c, const DbUrl* u) {
    if (tcp_connect(c, u->host, u->port)) return -1;
    Buf in = {0}, out = {0};
    int rc = -1;
    uint8_t salt[20];
    char plugin[64] = "mysql_native_password";
    c->seq = 0;
    if (my_read(c, &in)) goto out;
    if (in.n && in.p[0] == 0xff) { my_err(c, &in); goto out; }
    {
        const uint8_t *p = in.p, *end = in.p + in.n;
        if (!in.n || *p++ != 10) { set_err(c, "unsupported MySQL protocol%s", ""); goto out; }
        const uint8_t* z = memchr(p, 0, (size_t)(end - p));
        if (!z || end - z < 1 + 4 + 9 + 2 + 3 + 2 + 1 + 10) { set_err(c, "malformed MySQL handshake%s", ""); goto out; }
        p = z + 1; /* server version */
        c->my_thread = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; p += 4; /* for KILL QUERY */
        memcpy(salt, p, 8); p += 8 + 1;
        uint32_t caps = p[0] | p[1] << 8; p += 2;
        p += 1 + 2; /* charset, status */
        caps |= (uint32_t)(p[0] | p[1] << 8) << 16; p += 2;
        int auth_len = *p++;
        p += 10;
        size_t part2 = auth_len > 8 ? (size_t)(auth_len - 8) : 13;
        if (part2 > 13) part2 = 13;
        if (part2 < 12 || p + part2 > end) { set_err(c, "malformed MySQL handshake%s", ""); goto out; }
        memcpy(salt + 8, p, 12); p += part2;
        if ((caps & 0x80000) && p < end) snprintf(plugin, sizeof plugin, "%s", (const char*)p);
        if (!(caps & 0x200)) { set_err(c, "MySQL server lacks protocol 4.1%s", ""); goto out; }
    }
    {
        uint32_t flags = 0x1 | 0x4 | 0x200 | 0x2000 | 0x8000 | 0x80000 | 0x200000 | 0x1000000 /* long pwd, long flag, 4.1, transactions, secure conn, plugin auth, lenenc auth, deprecate EOF */;
        if (u->db[0]) flags |= 0x8;
        uint8_t hdr[32] = {0};
        hdr[0] = (uint8_t)flags; hdr[1] = (uint8_t)(flags >> 8); hdr[2] = (uint8_t)(flags >> 16); hdr[3] = (uint8_t)(flags >> 24);
        hdr[4] = 0; hdr[5] = 0; hdr[6] = 0; hdr[7] = 1; /* 16 MiB max packet */
        hdr[8] = 255; /* utf8mb4_0900_ai_ci */
        buf_put(&out, hdr, 32);
        buf_str0(&out, u->user);
        uint8_t resp[64]; size_t rn;
        if (my_auth_response(plugin, u->pass, salt, resp, &rn)) snprintf(plugin, sizeof plugin, "caching_sha2_password"), my_auth_response(plugin, u->pass, salt, resp, &rn);
        buf_byte(&out, (uint8_t)rn); buf_put(&out, resp, rn);
        if (u->db[0]) buf_str0(&out, u->db);
        buf_str0(&out, plugin);
        if (my_send(c, out.p, out.n)) goto out;
    }
    for (;;) {
        if (my_read(c, &in)) goto out;
        if (!in.n) goto out;
        uint8_t k = in.p[0];
        if (k == 0x00) { my_ok(c, &in, NULL); rc = 0; if (!strcmp(plugin, "mysql_native_password")) db_trace("mysql auth: native"); break; }
        if (k == 0xff) { my_err(c, &in); goto out; }
        if (k == 0xfe) { /* auth switch */
            snprintf(plugin, sizeof plugin, "%s", (const char*)in.p + 1);
            const uint8_t* s = in.p + 2 + strlen(plugin);
            if (s + 20 > in.p + in.n) { set_err(c, "malformed auth switch%s", ""); goto out; }
            memcpy(salt, s, 20);
            uint8_t resp[64]; size_t rn;
            if (my_auth_response(plugin, u->pass, salt, resp, &rn)) { set_err(c, "unsupported MySQL auth plugin %s", plugin); goto out; }
            if (my_send(c, resp, rn)) goto out;
            continue;
        }
        if (k == 0x01 && in.n >= 2 && !strcmp(plugin, "caching_sha2_password")) {
            if (in.p[1] == 3) { db_trace("mysql auth: caching_sha2 fast"); continue; } /* OK follows */
            if (in.p[1] == 4) { /* full auth: without TLS the password goes RSA-encrypted */
                db_trace("mysql auth: caching_sha2 full (RSA-OAEP)");
                uint8_t req = 2;
                if (my_send(c, &req, 1) || my_read(c, &in)) goto out;
                if (!in.n || in.p[0] != 0x01) { set_err(c, "server did not send its RSA key%s", ""); goto out; }
                size_t pl = strlen(u->pass) + 1;
                uint8_t* xp = (uint8_t*)malloc(pl);
                memcpy(xp, u->pass, pl);
                for (size_t i = 0; i < pl; i++) xp[i] ^= salt[i % 20];
                uint8_t enc[512]; size_t en;
                char* pem = dup_n((const char*)in.p + 1, in.n - 1);
                int e = rsa_oaep_encrypt(pem, xp, pl, enc, &en);
                free(pem); free(xp);
                if (e) { set_err(c, "RSA encryption of the password failed%s", ""); goto out; }
                if (my_send(c, enc, en)) goto out;
                continue;
            }
            if (in.p[1] != 0) { memmove(in.p, in.p + 1, in.n - 1); in.n--; continue; }
        }
        set_err(c, "unexpected MySQL authentication packet%s", ""); goto out;
    }
out:
    buf_free(&in); buf_free(&out);
    return rc;
}
/* Reads the column definitions announced by the column-count packet in `in` into r */
static int my_columns(Conn* c, Buf* in, Result* r) {
    const uint8_t* p = in->p;
    int nul;
    uint64_t ncols = my_lenenc(&p, in->p + in->n, &nul);
    if (nul || ncols == 0 || ncols > 4096) goto bad;
    r->cols = (int)ncols;
    r->names = (char**)calloc((size_t)ncols, sizeof(char*));
    for (uint64_t i = 0; i < ncols; i++) {
        if (my_read(c, in)) return -1;
        const uint8_t *q = in->p, *end = in->p + in->n;
        uint64_t l = 0;
        for (int f = 0; f < 5; f++) { /* catalog, schema, table, org_table, name */
            if (f) q += l;
            l = my_lenenc(&q, end, &nul);
            if (nul < 0 || l > (uint64_t)(end - q)) goto bad;
        }
        r->names[i] = dup_n((const char*)q, (size_t)l);
    }
    return 0;
bad:
    set_err(c, "protocol error%s", "");
    c->broken = 1;
    return -1;
}
/* Reads the next text-protocol row into row: 1 row, 0 end of rows, -1 error */
static int my_next_row(Conn* c, Buf* in, int cols, char** row) {
    if (my_read(c, in)) return -1;
    if (in->n && in->p[0] == 0xfe && in->n < 0xFFFFFF) { my_ok(c, in, NULL); return 0; } /* OK/EOF terminator */
    if (in->n && in->p[0] == 0xff) { my_err(c, in); return -1; }
    const uint8_t *q = in->p, *end = in->p + in->n;
    for (int i = 0; i < cols; i++) {
        int nul;
        uint64_t l = my_lenenc(&q, end, &nul);
        if (nul < 0 || (!nul && l > (uint64_t)(end - q))) { free_row(row, cols); set_err(c, "protocol error%s", ""); c->broken = 1; return -1; }
        if (!nul) { row[i] = dup_n((const char*)q, (size_t)l); q += l; }
    }
    return 1;
}
static Result* my_command(Conn* c, uint8_t cmd, const char* sql) {
    Buf pkt = {0}, in = {0};
    Result* r = (Result*)calloc(1, sizeof(Result));
    int failed = 1;
    buf_byte(&pkt, cmd);
    if (sql) buf_put(&pkt, sql, strlen(sql));
    c->seq = 0;
    if (my_send(c, pkt.p, pkt.n) || my_read(c, &in) || !in.n) goto out;
    if (in.p[0] == 0x00) { my_ok(c, &in, r); failed = 0; goto out; }
    if (in.p[0] == 0xff) { my_err(c, &in); goto out; }
    if (my_columns(c, &in, r)) goto out;
    for (;;) {
        if (result_add_row(r)) goto out;
        int k = my_next_row(c, &in, r->cols, &r->cells[(long)(r->rows - 1) * r->cols]);
        if (k < 0) goto out;
        if (k == 0) { r->rows--; break; }
    }
    r->affected = -1;
    failed = 0;
out:
    buf_free(&pkt); buf_free(&in);
    if (failed) { result_free(r); return NULL; }
    return r;
}

/* SQLite --------------------------------------------------------------------------------------- */
static void lite_row(sqlite3_stmt* st, int n, char** row) {
    for (int i = 0; i < n; i++)
        if (sqlite3_column_type(st, i) != SQLITE_NULL) { const char* t = (const char*)sqlite3_column_text(st, i); row[i] = dup_n(t, (size_t)sqlite3_column_bytes(st, i)); }
}
static char** lite_names(sqlite3_stmt* st, int n) {
    char** names = (char**)calloc((size_t)(n ? n : 1), sizeof(char*));
    for (int i = 0; i < n; i++) { const char* nm = sqlite3_column_name(st, i); names[i] = dup_n(nm, strlen(nm)); }
    return names;
}
/* Progress callback: a cancelled or timed-out statement is interrupted (the connection stays usable) */
static int lite_progress(void* p) {
    Conn* c = (Conn*)p;
    int why = c->in_query ? conn_stop_reason(c) : 0;
    if (why && !c->stop) c->stop = why;
    return why != 0;
}
static Result* lite_query(Conn* c, const char* sql) {
    Result* r = (Result*)calloc(1, sizeof(Result));
    r->affected = -1;
    const char* tail = sql;
    while (tail && *tail) {
        sqlite3_stmt* st = NULL;
        if (sqlite3_prepare_v2(c->lite, tail, -1, &st, &tail) != SQLITE_OK) { set_err(c, "%s", sqlite3_errmsg(c->lite)); result_free(r); return NULL; }
        if (!st) break;
        int n = sqlite3_column_count(st);
        if (n) {
            for (int i = 0; i < r->cols; i++) free(r->names[i]);
            free(r->names);
            for (long i = 0; i < (long)r->rows * r->cols; i++) free(r->cells[i]);
            free(r->cells); r->cells = NULL; r->rows = r->cap_rows = 0;
            r->cols = n;
            r->names = lite_names(st, n);
        }
        int s;
        while ((s = sqlite3_step(st)) == SQLITE_ROW) {
            if (result_add_row(r)) break;
            lite_row(st, n, &r->cells[(long)(r->rows - 1) * r->cols]);
        }
        if (s != SQLITE_DONE) { set_err(c, "%s", sqlite3_errmsg(c->lite)); sqlite3_finalize(st); result_free(r); c->in_txn = !sqlite3_get_autocommit(c->lite); return NULL; }
        if (!n) { r->affected = sqlite3_changes(c->lite); r->last_id = sqlite3_last_insert_rowid(c->lite); }
        sqlite3_finalize(st);
    }
    c->in_txn = !sqlite3_get_autocommit(c->lite);
    return r;
}

/* ---- connections: statements, deadlines, cancellation ------------------------------------------ */
/* Starts a statement: the token (may be NULL) and query_ms (0 = none) can stop it */
static void conn_begin(Conn* c, Token* t, int gen, long long query_ms) {
    c->tok = t; c->tok_gen = gen; c->deadline = query_ms > 0 ? now_ms() + query_ms : 0; c->stop = 0; c->in_query = 1;
}
static void conn_end(Conn* c) { c->in_query = 0; c->deadline = 0; c->tok = NULL; }
/* -1 (with c->err) when no statement may run on the connection now */
static int conn_ready(Conn* c) {
    c->err[0] = 0;
    if (c->broken) { set_err(c, "connection is broken%s", ""); return -1; }
    if (c->kill) { c->stop = 1; set_err(c, "the pool is shutting down%s", ""); return -1; }
    if (c->in_query && conn_stop_reason(c) == 1) { c->stop = 1; set_err(c, "query cancelled%s", ""); return -1; }
    return 0;
}
static void conn_stopped_err(Conn* c) { if (c->stop) set_err(c, c->stop == 1 ? (c->kill ? "the pool is shutting down%s" : "query cancelled%s") : "query timed out%s", ""); }
/* Error code for a failed statement */
static long long conn_rc(const Conn* c) { return c->stop == 1 ? -14 : c->stop == 2 ? -15 : c->broken ? -3 : -18; }
/* Runs sql. A statement that was cancelled or timed out fails even if the server finished it. */
static Result* conn_query(Conn* c, const char* sql) {
    if (conn_ready(c)) return NULL;
    Result* r = c->driver == DRV_PG ? pg_query(c, sql) : c->driver == DRV_MYSQL ? my_command(c, 0x03, sql) : lite_query(c, sql);
    c->last_used = now_ms();
    if (c->stop) { result_free(r); r = NULL; conn_stopped_err(c); }
    return r;
}
/* Housekeeping statement (health check, rollback on release): fails rather than waits past the connect timeout */
static Result* conn_quick(Conn* c, const char* sql) {
    c->in_query = 0; c->tok = NULL; c->deadline = now_ms() + c->connect_ms;
    Result* r = conn_ready(c) ? NULL : c->driver == DRV_PG ? pg_query(c, sql) : c->driver == DRV_MYSQL ? my_command(c, sql ? 0x03 : 0x0e, sql) : lite_query(c, sql ? sql : "SELECT 1");
    if (!r && c->driver != DRV_SQLITE && conn_stop_reason(c)) c->broken = 1;
    c->deadline = 0;
    c->last_used = now_ms();
    return r;
}
static void conn_close(Conn* c) {
    if (!c) return;
    c->in_query = 0; c->tok = NULL; c->kill = 0; c->deadline = now_ms() + 1000; /* a goodbye never waits long */
    if (c->driver == DRV_PG && c->sock != DB_BAD_SOCK && !c->broken) { Buf e = {0}; pg_send(c, 'X', &e); }
    if (c->driver == DRV_MYSQL && c->sock != DB_BAD_SOCK && !c->broken) { uint8_t q = 0x01; c->seq = 0; my_send(c, &q, 1); }
    if (c->sock != DB_BAD_SOCK) db_closesock(c->sock);
    if (c->lite) sqlite3_close(c->lite);
    free(c);
}
static Conn* conn_open(const DbUrl* u, long long connect_ms, Token* t, int gen, char* err, size_t errcap) {
    Conn* c = (Conn*)calloc(1, sizeof(Conn));
    c->driver = u->driver;
    c->sock = DB_BAD_SOCK;
    c->url = *u;
    c->connect_ms = connect_ms;
    c->tok = t; c->tok_gen = gen; c->deadline = now_ms() + connect_ms; /* the whole handshake */
    int rc;
    if (u->driver == DRV_SQLITE) {
        rc = sqlite3_open(u->path, &c->lite) == SQLITE_OK ? 0 : -1;
        if (rc) set_err(c, "%s", c->lite ? sqlite3_errmsg(c->lite) : "cannot open database");
        else { sqlite3_busy_timeout(c->lite, (int)connect_ms); sqlite3_progress_handler(c->lite, 1000, lite_progress, c); }
        c->std_strings = 1;
    } else if (u->driver == DRV_PG) {
        rc = pg_connect(c, u);
    } else {
        rc = my_connect(c, u);
    }
    if (rc) { snprintf(err, errcap, "%s", c->err); conn_close(c); return NULL; }
    c->tok = NULL; c->deadline = 0;
    c->last_used = c->created = now_ms();
    return c;
}
static int conn_ping(Conn* c) {
    if (c->broken) return -1;
    if (c->driver == DRV_SQLITE) return 0;
    Result* r = conn_quick(c, c->driver == DRV_MYSQL ? NULL : "SELECT 1");
    int ok = r != NULL;
    result_free(r);
    return ok ? 0 : -1;
}
/* Asks the server to stop the running statement, over a separate short-lived connection. A request
 * that arrives after the statement finished is ignored by both servers. */
static void conn_send_cancel(Conn* c) {
    if (c->driver == DRV_SQLITE) return; /* the progress callback interrupts SQLite */
    db_trace("sending a cancel request");
    Conn* k = (Conn*)calloc(1, sizeof(Conn));
    k->driver = c->driver; k->sock = DB_BAD_SOCK; k->url = c->url; k->connect_ms = c->connect_ms;
    k->deadline = now_ms() + c->connect_ms;
    if (c->driver == DRV_PG) {
        if (!tcp_connect(k, c->url.host, c->url.port)) {
            uint8_t m[16];
            put_be32(m, 16); put_be32(m + 4, 80877102); put_be32(m + 8, c->pg_pid); put_be32(m + 12, c->pg_secret);
            conn_send(k, m, 16);
        }
        k->broken = 1; /* CancelRequest connections get no Terminate */
    } else if (!my_connect(k, &c->url)) {
        char sql[48];
        snprintf(sql, sizeof sql, "KILL QUERY %u", c->my_thread);
        result_free(my_command(k, 0x03, sql));
    }
    conn_close(k);
}

/* ---- row streams ------------------------------------------------------------------------------- */
/* Sends sql and reads up to its first row. 0 ok (s->done when the statement returns no rows). */
static int stream_open(Conn* c, const char* sql, Result* r) {
    Stream* s = r->s;
    if (c->driver == DRV_SQLITE) {
        const char* tail = NULL;
        if (sqlite3_prepare_v2(c->lite, sql, -1, &s->st, &tail) != SQLITE_OK) { set_err(c, "%s", sqlite3_errmsg(c->lite)); s->done = 1; return -1; }
        while (tail && isspace((unsigned char)*tail)) tail++;
        if (!s->st || (tail && *tail)) { set_err(c, "a stream runs one statement%s", ""); s->done = 1; return -1; }
        r->cols = sqlite3_column_count(s->st);
        r->names = lite_names(s->st, r->cols);
        if (!r->cols) { /* nothing to stream: run it now */
            int k = sqlite3_step(s->st);
            if (k != SQLITE_DONE) set_err(c, "%s", sqlite3_errmsg(c->lite));
            else { r->affected = sqlite3_changes(c->lite); r->last_id = sqlite3_last_insert_rowid(c->lite); }
            sqlite3_finalize(s->st); s->st = NULL; s->done = 1;
            c->in_txn = !sqlite3_get_autocommit(c->lite);
            return k == SQLITE_DONE ? 0 : -1;
        }
        return 0;
    }
    if (c->driver == DRV_PG) {
        Buf q = {0};
        buf_str0(&q, sql);
        int e = pg_send(c, 'Q', &q);
        buf_free(&q);
        if (e) { s->done = 1; return -1; }
        for (;;) {
            int k = pg_step(c, &s->in, r, NULL, &s->failed);
            if (k == 3) return 0;
            if (k < 0 || k == 2) { s->done = 1; if (!r->names) r->names = (char**)calloc(1, sizeof(char*)); return k < 0 || s->failed ? -1 : 0; }
        }
    }
    Buf pkt = {0};
    buf_byte(&pkt, 0x03);
    buf_put(&pkt, sql, strlen(sql));
    c->seq = 0;
    int e = my_send(c, pkt.p, pkt.n) || my_read(c, &s->in) || !s->in.n;
    buf_free(&pkt);
    if (e) { s->done = 1; return -1; }
    if (s->in.p[0] == 0x00) { my_ok(c, &s->in, r); r->names = (char**)calloc(1, sizeof(char*)); s->done = 1; return 0; }
    if (s->in.p[0] == 0xff) { my_err(c, &s->in); s->done = 1; return -1; }
    if (my_columns(c, &s->in, r)) { s->done = 1; return -1; }
    return 0;
}
/* Reads the next row into s->row: 1 row, 0 end, -1 error. s->done is set at the end and on errors. */
static int stream_next(Conn* c, Result* r) {
    Stream* s = r->s;
    if (c->driver == DRV_SQLITE) {
        int k = sqlite3_step(s->st);
        if (k == SQLITE_ROW) { lite_row(s->st, r->cols, s->row); return 1; }
        if (k != SQLITE_DONE) set_err(c, "%s", sqlite3_errmsg(c->lite));
        sqlite3_finalize(s->st); s->st = NULL; s->done = 1;
        c->in_txn = !sqlite3_get_autocommit(c->lite);
        return k == SQLITE_DONE ? 0 : -1;
    }
    if (c->driver == DRV_PG) {
        for (;;) {
            int k = pg_step(c, &s->in, r, s->row, &s->failed);
            if (k == 1) return 1;
            if (k < 0 || k == 2) { s->done = 1; free_row(s->row, r->cols); return k < 0 || s->failed ? -1 : 0; }
        }
    }
    int k = my_next_row(c, &s->in, r->cols, s->row);
    if (k <= 0) s->done = 1;
    return k;
}
/* Ends a stream early. The rest of the result is read and dropped; one that is still streaming after
 * 50 ms gets a server-side cancel. The connection error (if any) is kept. */
static void stream_drain(Conn* c, Result* r) {
    Stream* s = r->s;
    char err[512];
    snprintf(err, sizeof err, "%s", c->err);
    if (c->driver == DRV_SQLITE || c->broken) {
        if (s->st) sqlite3_finalize(s->st);
        s->st = NULL; s->done = 1;
        if (c->lite) c->in_txn = !sqlite3_get_autocommit(c->lite);
        return;
    }
    conn_begin(c, NULL, 0, 50);
    while (!s->done) if (stream_next(c, r) > 0) free_row(s->row, r->cols);
    conn_end(c);
    c->stop = 0;
    if (!c->broken) snprintf(c->err, sizeof c->err, "%s", err);
}

static int ident_char(char ch) { return isalnum((unsigned char)ch) || ch == '_' || (unsigned char)ch >= 0x80; }
/* Length of the comment, quoted string or quoted identifier starting at s (where a ? is not a
 * placeholder), or 0. Unterminated spans run to the end of the text. */
static size_t sql_skip(const Conn* c, const char* sql, const char* s) {
    const char* e;
    int my = c->driver == DRV_MYSQL, pg = c->driver == DRV_PG;
    if ((s[0] == '-' && s[1] == '-' && (!my || !s[2] || isspace((unsigned char)s[2]))) || (s[0] == '#' && my))
        return strcspn(s, "\n");
    if (s[0] == '/' && s[1] == '*') { /* PostgreSQL comments nest, MySQL and SQLite ones do not */
        int depth = 0;
        for (e = s; *e;) {
            if (e[0] == '/' && e[1] == '*') { if (!depth || pg) depth++; e += 2; }
            else if (e[0] == '*' && e[1] == '/') { e += 2; if (!--depth) break; }
            else e++;
        }
        return (size_t)(e - s);
    }
    char close = *s == '[' && c->driver == DRV_SQLITE ? ']' : *s;
    if (close == '\'' || close == '"' || close == '`' || close == ']') {
        /* backslash escapes: MySQL strings (unless NO_BACKSLASH_ESCAPES) and PostgreSQL E'...' strings */
        int bs = (my && !c->no_backslash && close != '`') || (pg && close == '\'' && s > sql && (s[-1] == 'E' || s[-1] == 'e') && (s - 1 == sql || !ident_char(s[-2])));
        for (e = s + 1; *e; e++) {
            if (bs && *e == '\\' && e[1]) { e++; continue; }
            if (*e == close) { if (close != ']' && e[1] == close) { e++; continue; } return (size_t)(e + 1 - s); }
        }
        return (size_t)(e - s);
    }
    if (*s == '$' && pg && (s == sql || !ident_char(s[-1]))) { /* $tag$ ... $tag$ */
        const char* t = s + 1;
        if (!isdigit((unsigned char)*t)) while (ident_char(*t)) t++;
        if (*t == '$') {
            size_t tl = (size_t)(t + 1 - s);
            for (e = t + 1; *e; e++) if (!strncmp(e, s, tl)) return (size_t)(e + tl - s);
            return (size_t)(e - s);
        }
    }
    return 0;
}

/* Quote ? parameters as literals for this connection. params is `<len>:<bytes>` repeated, one entry
 * per parameter (lengths in bytes, so values may contain any byte except NUL). */
static char* bind_params(Conn* c, const char* sql, const char* params, int nparams) {
    if (nparams == 0) return dup_n(sql, strlen(sql));
    if (c->driver == DRV_PG && !c->std_strings) { set_err(c, "parameters need standard_conforming_strings=on%s", ""); return NULL; }
    Buf o = {0};
    const char* p = params;
    int used = 0;
    for (const char* s = sql; *s;) {
        size_t skip = sql_skip(c, sql, s);
        if (skip) { buf_put(&o, s, skip); s += skip; continue; }
        if (*s != '?') { buf_byte(&o, (uint8_t)*s++); continue; }
        s++;
        if (used == nparams) { buf_free(&o); set_err(c, "more ? placeholders than parameters%s", ""); return NULL; }
        char* colon = NULL;
        long long n = strtoll(p, &colon, 10);
        if (!colon || *colon != ':' || n < 0 || (size_t)n > strlen(colon + 1)) { buf_free(&o); set_err(c, "malformed parameter list%s", ""); return NULL; }
        p = colon + 1;
        {
            buf_byte(&o, '\'');
            for (long long i = 0; i < n; i++) {
                char ch = p[i];
                if (ch == '\0') break;
                if (ch == '\'') buf_byte(&o, '\'');
                if (ch == '\\' && c->driver == DRV_MYSQL && !c->no_backslash) buf_byte(&o, '\\');
                buf_byte(&o, (uint8_t)ch);
            }
            buf_byte(&o, '\'');
        }
        used++;
        p += n;
    }
    if (used != nparams) { buf_free(&o); set_err(c, "fewer ? placeholders than parameters%s", ""); return NULL; }
    buf_byte(&o, 0);
    return (char*)o.p;
}

/* ---- pools and handles ------------------------------------------------------------------------ */
#define MAX_POOLS 64
#define MAX_CONNS_PER_POOL 256
#define MAX_RESULTS 4096
#define MAX_TOKENS 4096
typedef struct {
    int used, closing, busy; /* busy: acquirers and closers in flight; the slot is freed once closing with no leases or busy */
    DbUrl url;
    int max, open, in_use;
    long long timeout_ms, idle_check_ms;
    volatile long long connect_ms, query_ms, idle_ms, life_ms; /* rtdbsetlimit; 0 = off (connect is always on) */
    Conn* idle[MAX_CONNS_PER_POOL];
    int nidle;
    db_mutex mu;
    db_cond cv;
    long long stat_created, stat_waits, stat_timeouts, stat_broken, stat_rollbacks, stat_checkouts, stat_expired;
    char err[512];
} Pool;
/* busy: a statement or stream read is running on the lease; stream: its open row stream */
typedef struct { Conn* c; int pool; int used, busy; Result* stream; } Lease;
static Pool g_pools[MAX_POOLS + 1];
static Lease g_leases[MAX_POOLS * MAX_CONNS_PER_POOL + 1];
static Result* g_results[MAX_RESULTS + 1];
static Token g_tokens[MAX_TOKENS + 1];
/* Free handle stacks (under g_tab_mu): O(1) allocation instead of scanning the tables */
#define MAX_LEASES (MAX_POOLS * MAX_CONNS_PER_POOL)
static int g_free_lease[MAX_LEASES], g_free_lease_n = -1;
static int g_free_res[MAX_RESULTS], g_free_res_n = -1;
static int g_free_tok[MAX_TOKENS], g_free_tok_n = -1, g_tok_gen;
static void free_stacks_init(void) {
    if (g_free_lease_n >= 0) return;
    for (int i = 0; i < MAX_LEASES; i++) g_free_lease[i] = MAX_LEASES - i; /* 1 on top */
    g_free_lease_n = MAX_LEASES;
    for (int i = 0; i < MAX_RESULTS; i++) g_free_res[i] = MAX_RESULTS - i;
    g_free_res_n = MAX_RESULTS;
    for (int i = 0; i < MAX_TOKENS; i++) g_free_tok[i] = MAX_TOKENS - i;
    g_free_tok_n = MAX_TOKENS;
}
static char g_last_err[512];
#ifdef _WIN32
static db_mutex g_tab_mu = SRWLOCK_INIT;
#else
static db_mutex g_tab_mu = PTHREAD_MUTEX_INITIALIZER;
#endif
static void tab_lock(void) { mtx_lock(&g_tab_mu); }

/* ---- cancellation tokens ---- */
static Token* tok_get(long long h) { return h >= 1 && h <= MAX_TOKENS && g_tokens[h].used ? &g_tokens[h] : NULL; }
/* Resolves an optional token argument (0 = none); -1 for an invalid handle */
static int tok_resolve(long long h, Token** t, int* gen) {
    *t = NULL; *gen = 0;
    if (!h) return 0;
    tab_lock();
    Token* k = tok_get(h);
    if (k) { *t = k; *gen = k->gen; }
    mtx_unlock(&g_tab_mu);
    return k ? 0 : -1;
}
long long salivo_db_token_new(void) {
    tab_lock();
    free_stacks_init();
    long long h = -11;
    if (g_free_tok_n > 0) { int i = g_free_tok[--g_free_tok_n]; g_tokens[i].used = 1; g_tokens[i].cancelled = 0; g_tokens[i].gen = ++g_tok_gen; h = i; }
    mtx_unlock(&g_tab_mu);
    return h;
}
/* The checkout or statement using the token stops within 50 ms (statements get a server-side cancel) */
long long salivo_db_token_cancel(long long h) { tab_lock(); Token* t = tok_get(h); if (t) t->cancelled = 1; mtx_unlock(&g_tab_mu); return t ? 0 : -10; }
long long salivo_db_token_free(long long h) {
    tab_lock();
    Token* t = tok_get(h);
    if (t) { t->used = 0; t->gen = ++g_tok_gen; g_free_tok[g_free_tok_n++] = (int)h; }
    mtx_unlock(&g_tab_mu);
    return t ? 0 : -10;
}

/* ---- pools ---- */
static Pool* pool_get(long long h) { return h >= 1 && h <= MAX_POOLS && g_pools[h].used ? &g_pools[h] : NULL; }
/* Locks a pool slot; g_tab_mu is held while locking so the slot cannot be freed and reused meanwhile */
static Pool* pool_lock(long long h) {
    tab_lock();
    Pool* p = pool_get(h);
    if (p) mtx_lock(&p->mu);
    mtx_unlock(&g_tab_mu);
    return p;
}
/* Unlocks p->mu; the last one out of a closing pool frees its slot */
static void pool_unlock(Pool* p) {
    int done = p->closing && p->in_use == 0 && p->busy == 0 && p->used;
    mtx_unlock(&p->mu);
    if (done) { tab_lock(); p->used = 0; mtx_unlock(&g_tab_mu); }
}
static int conn_expired(const Pool* p, const Conn* c, long long now) {
    return (p->idle_ms > 0 && now - c->last_used >= p->idle_ms) || (p->life_ms > 0 && now - c->created >= p->life_ms);
}
/* Closes idle connections past the idle timeout or their lifetime (p->mu held) */
static void pool_reap(Pool* p) {
    long long now = now_ms();
    int k = 0;
    for (int i = 0; i < p->nidle; i++) {
        Conn* c = p->idle[i];
        if (conn_expired(p, c, now)) { conn_close(c); p->open--; p->stat_expired++; }
        else p->idle[k++] = c;
    }
    p->nidle = k;
}
/* Expiry also happens while nothing checks out: one background thread, started by the first idle
 * timeout or lifetime, sweeps every pool */
static int g_reaper;
#ifdef _WIN32
static DWORD WINAPI reaper_main(LPVOID x)
#else
static void* reaper_main(void* x)
#endif
{
    (void)x;
    for (;;) {
        sleep_ms(250); /* ponytail: expiry is enforced within a quarter second; a timer wheel if that is too coarse */
        for (int h = 1; h <= MAX_POOLS; h++) {
            Pool* p = pool_lock(h);
            if (!p) continue;
            if (!p->closing) pool_reap(p);
            mtx_unlock(&p->mu);
        }
    }
    return 0; /* not reached */
}
static void reaper_start(void) {
    tab_lock();
    int start = !g_reaper;
    g_reaper = 1;
    mtx_unlock(&g_tab_mu);
    if (!start) return;
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, reaper_main, NULL, 0, NULL);
    if (t) CloseHandle(t);
#else
    pthread_t t;
    if (!pthread_create(&t, NULL, reaper_main, NULL)) pthread_detach(t);
#endif
}

static long long db_pool_open_impl(const char* url, long long max_conns, long long timeout_ms) {
    DbUrl u;
    if (parse_url(url, &u) || max_conns < 1 || max_conns > MAX_CONNS_PER_POOL || timeout_ms < 0) { snprintf(g_last_err, sizeof g_last_err, "invalid database URL or pool size"); return -7; }
    tab_lock();
    long long h = -11;
    for (int i = 1; i <= MAX_POOLS; i++) if (!g_pools[i].used) { h = i; g_pools[i].used = 1; break; }
    if (h > 0) {
        Pool* p = &g_pools[h];
        memset(p, 0, sizeof *p);
        p->used = 1; p->url = u; p->max = (int)max_conns; p->timeout_ms = timeout_ms ? timeout_ms : 5000; p->idle_check_ms = 1000;
        p->connect_ms = p->timeout_ms;
        mtx_init(&p->mu); cond_init(&p->cv);
    }
    mtx_unlock(&g_tab_mu);
    if (h < 0) { snprintf(g_last_err, sizeof g_last_err, "too many pools"); return h; }
    /* one eager connection proves the URL and credentials work */
    char err[512];
    Conn* c = conn_open(&u, g_pools[h].connect_ms, NULL, 0, err, sizeof err);
    if (!c) { snprintf(g_last_err, sizeof g_last_err, "%s", err); tab_lock(); g_pools[h].used = 0; mtx_unlock(&g_tab_mu); return -8; }
    Pool* p = &g_pools[h];
    mtx_lock(&p->mu);
    p->idle[p->nidle++] = c; p->open = 1; p->stat_created = 1;
    mtx_unlock(&p->mu);
    return h;
}

/* Returns a lease handle (> 0) or a negative error: -4 timeout, -8 connect failure, -10 bad handle,
 * -12 pool closed, -14 cancelled through the token */
static long long db_acquire_impl(long long pool, long long tok) {
    Token* t;
    int gen;
    if (tok_resolve(tok, &t, &gen)) return -10;
    Pool* p = pool_lock(pool);
    if (!p) return -10;
    if (p->closing) { mtx_unlock(&p->mu); return -12; }
    p->busy++;
    long long deadline = now_ms() + p->timeout_ms, rc = 0;
    int waited = 0;
    Conn* c = NULL;
    for (;;) {
        if (p->closing) { rc = -12; break; } /* dbClose ran while this checkout waited */
        if (tok_cancelled(t, gen)) { rc = -14; break; }
        pool_reap(p);
        while (p->nidle > 0 && !c) {
            Conn* cand = p->idle[--p->nidle];
            if (now_ms() - cand->last_used >= p->idle_check_ms) {
                /* health check without holding the pool lock: the ping is a network round trip */
                mtx_unlock(&p->mu);
                int dead = conn_ping(cand);
                db_trace(dead ? "health check: dead connection replaced" : "health check: ok");
                if (dead) conn_close(cand);
                mtx_lock(&p->mu);
                if (dead) { p->open--; p->stat_broken++; continue; }
                if (p->closing) { conn_close(cand); p->open--; continue; }
            }
            c = cand;
        }
        if (c) break;
        if (p->open < p->max) {
            p->open++;
            long long cms = p->connect_ms;
            mtx_unlock(&p->mu);
            char err[512];
            c = conn_open(&p->url, cms, t, gen, err, sizeof err);
            mtx_lock(&p->mu);
            if (!c) { p->open--; snprintf(p->err, sizeof p->err, "%s", err); cond_broadcast(&p->cv); rc = tok_cancelled(t, gen) ? -14 : -8; break; }
            p->stat_created++;
            if (p->closing) { conn_close(c); c = NULL; p->open--; rc = -12; }
            break;
        }
        long long left = deadline - now_ms();
        if (left <= 0) { p->stat_timeouts++; snprintf(p->err, sizeof p->err, "timed out waiting for a pooled connection"); rc = -4; break; }
        if (!waited) { p->stat_waits++; waited = 1; }
        cond_wait_ms(&p->cv, &p->mu, t && left > 50 ? 50 : left); /* a token is checked every 50 ms */
    }
    if (rc == -12) snprintf(p->err, sizeof p->err, "pool is closed");
    if (rc == -14) snprintf(p->err, sizeof p->err, "checkout cancelled");
    p->busy--;
    if (c) { p->in_use++; p->stat_checkouts++; }
    pool_unlock(p);
    if (!c) return rc;
    tab_lock();
    long long h = -11;
    free_stacks_init();
    if (g_free_lease_n > 0) { int i = g_free_lease[--g_free_lease_n]; Lease* l = &g_leases[i]; l->used = 1; l->busy = 0; l->stream = NULL; l->c = c; l->pool = (int)pool; h = i; }
    mtx_unlock(&g_tab_mu);
    return h;
}
static Lease* lease_get(long long h) { return h >= 1 && h <= MAX_LEASES && g_leases[h].used ? &g_leases[h] : NULL; }
/* Claims the lease for one statement: -10 bad handle, -13 a statement or row stream is using it */
static long long lease_enter(long long lease, Lease** out) {
    tab_lock();
    Lease* l = lease_get(lease);
    long long rc = !l ? -10 : l->busy || l->stream ? -13 : 0;
    if (!rc) l->busy = 1;
    mtx_unlock(&g_tab_mu);
    *out = l;
    return rc;
}
static void lease_exit(Lease* l) { tab_lock(); l->busy = 0; mtx_unlock(&g_tab_mu); }
static Result* res_get(long long h) { return h >= 1 && h <= MAX_RESULTS ? g_results[h] : NULL; }
/* Stores a result in the handle table (g_tab_mu held) */
static long long res_put(Result* r) {
    free_stacks_init();
    if (g_free_res_n <= 0) return -11;
    int i = g_free_res[--g_free_res_n];
    g_results[i] = r;
    return i;
}

/* After an early end: hides the stream's last row and gives the lease back for statements */
static void stream_detach(Lease* l, Result* r) {
    tab_lock();
    char** shown = r->cells;
    r->cells = r->s->row; r->s->row = shown;
    r->rows = 0;
    l->stream = NULL;
    l->busy = 0;
    mtx_unlock(&g_tab_mu);
    free_row(r->s->row, r->cols);
}

/* Gives the connection back after any running statement; an open row stream is ended, an open
 * transaction rolled back, and a broken, expired or shut-down connection closed */
static long long db_release_impl(long long lease) {
    Lease* l;
    Lease copy = {0};
    for (;;) {
        tab_lock();
        l = lease_get(lease);
        int busy = l && l->busy;
        Result* sr = l && !busy ? l->stream : NULL;
        if (sr) l->busy = 1;
        else if (l && !busy) { copy = *l; l->used = 0; } /* stale handles fail from here on; the slot is reused only after cleanup */
        mtx_unlock(&g_tab_mu);
        if (sr) { stream_drain(l->c, sr); stream_detach(l, sr); continue; }
        if (!busy) break;
        sleep_ms(1); /* ponytail: polls for a statement on another thread to finish; a per-lease condvar if this ever shows up */
    }
    if (!l) return -10;
    Conn* c = copy.c;
    int rolled = 0;
    if (!c->broken && c->in_txn) { Result* r = conn_quick(c, "ROLLBACK"); result_free(r); rolled = 1; if (c->in_txn) c->broken = 1; }
    Pool* p = &g_pools[copy.pool]; /* the lease keeps the slot alive (in_use > 0) */
    mtx_lock(&p->mu);
    p->in_use--;
    if (rolled) p->stat_rollbacks++;
    int expired = !c->broken && !c->kill && !p->closing && conn_expired(p, c, now_ms());
    int drop = c->broken || c->kill || p->closing || expired;
    if (drop) { p->open--; if (c->broken) p->stat_broken++; if (expired) p->stat_expired++; }
    else p->idle[p->nidle++] = c;
    cond_broadcast(&p->cv);
    pool_unlock(p);
    if (drop) conn_close(c);
    tab_lock();
    g_free_lease[g_free_lease_n++] = (int)lease;
    mtx_unlock(&g_tab_mu);
    return rolled;
}

/* params: see bind_params. Returns a result handle or a negative error: -13 the lease is busy,
 * -14 cancelled, -15 query timeout, -3 connection lost, -18 query error */
static long long db_query_impl(long long lease, const char* sql, const char* params, long long nparams, long long tok) {
    Token* t;
    int gen;
    if (tok_resolve(tok, &t, &gen)) return -10;
    Lease* l;
    long long h = lease_enter(lease, &l);
    if (h) return h;
    Conn* c = l->c;
    char* bound = bind_params(c, sql ? sql : "", params ? params : "", (int)nparams);
    Result* r = NULL;
    h = -7;
    if (bound) {
        conn_begin(c, t, gen, g_pools[l->pool].query_ms);
        r = conn_query(c, bound);
        conn_end(c);
        free(bound);
        h = r ? -11 : conn_rc(c);
    }
    tab_lock();
    l->busy = 0;
    if (r) h = res_put(r);
    mtx_unlock(&g_tab_mu);
    if (h < 0) result_free(r);
    return h;
}

/* Opens a row stream: rows are read one at a time with db_next, so memory stays bounded and a slow
 * reader slows the server (TCP flow control). The lease runs nothing else until the stream ends or
 * its handle is freed. Errors as db_query. */
static long long db_stream_impl(long long lease, const char* sql, const char* params, long long nparams, long long tok) {
    Token* t;
    int gen;
    if (tok_resolve(tok, &t, &gen)) return -10;
    Lease* l;
    long long rc = lease_enter(lease, &l);
    if (rc) return rc;
    Conn* c = l->c;
    char* bound = bind_params(c, sql ? sql : "", params ? params : "", (int)nparams);
    if (!bound) { lease_exit(l); return -7; }
    Result* r = (Result*)calloc(1, sizeof(Result));
    Stream* s = (Stream*)calloc(1, sizeof(Stream));
    r->s = s; r->affected = -1;
    s->lease = (int)lease; s->tok = t; s->tok_gen = gen;
    conn_begin(c, t, gen, g_pools[l->pool].query_ms);
    int k = conn_ready(c) ? -1 : stream_open(c, bound, r);
    if (!k && c->stop) k = -1;
    if (k) { rc = conn_rc(c); conn_stopped_err(c); }
    conn_end(c);
    free(bound);
    c->last_used = now_ms();
    if (!k) {
        int n = r->cols ? r->cols : 1;
        s->row = (char**)calloc((size_t)n, sizeof(char*));
        r->cells = (char**)calloc((size_t)n, sizeof(char*));
        r->cap_rows = 1;
        tab_lock();
        rc = res_put(r);
        if (rc > 0) { if (!s->done) l->stream = r; l->busy = 0; }
        mtx_unlock(&g_tab_mu);
    }
    if (rc < 0) {
        if (!s->done) stream_drain(c, r);
        result_free(r);
        lease_exit(l);
    }
    return rc;
}

/* Moves a row stream to its next row, readable as row 0: 1 row, 0 end, negative error (the stream
 * then ends). Errors as db_query. */
static long long db_next_impl(long long h) {
    tab_lock();
    Result* r = res_get(h);
    Stream* s = r ? r->s : NULL;
    Lease* l = s && !s->done ? &g_leases[s->lease] : NULL;
    long long rc = !r ? -10 : !s ? -7 : l && l->busy ? -13 : 0;
    if (!rc && l) l->busy = 1; /* also keeps r alive: freeing a stream waits for its lease */
    mtx_unlock(&g_tab_mu);
    if (rc || !l) return rc;
    Conn* c = l->c;
    conn_begin(c, s->tok, s->tok_gen, g_pools[l->pool].query_ms);
    int k = conn_ready(c) ? -1 : stream_next(c, r);
    if (k > 0 && c->stop) k = -1;
    if (k < 0) { rc = conn_rc(c); conn_stopped_err(c); }
    conn_end(c);
    c->last_used = now_ms();
    if (k < 0 && !s->done) stream_drain(c, r);
    if (k <= 0) free_row(s->row, r->cols);
    tab_lock();
    char** shown = r->cells; /* the visible row becomes the read buffer */
    r->cells = s->row; s->row = shown;
    r->rows = k > 0;
    if (s->done) l->stream = NULL;
    l->busy = 0;
    mtx_unlock(&g_tab_mu);
    free_row(s->row, r->cols);
    return k > 0 ? 1 : k == 0 ? 0 : rc;
}

/* Ends an open row stream whose handle is being freed: waits for its lease, then drains */
static void stream_close(Result* r) {
    Stream* s = r->s;
    Lease* l = NULL;
    for (;;) {
        tab_lock();
        int open = !s->done, busy = open && g_leases[s->lease].busy;
        if (open && !busy) { l = &g_leases[s->lease]; l->busy = 1; }
        mtx_unlock(&g_tab_mu);
        if (!busy) break;
        sleep_ms(1);
    }
    if (!l) return;
    stream_drain(l->c, r);
    stream_detach(l, r);
}
static long long db_result_free_impl(long long h) {
    tab_lock();
    Result* r = res_get(h);
    if (r) { g_results[h] = NULL; g_free_res[g_free_res_n++] = (int)h; }
    mtx_unlock(&g_tab_mu);
    if (!r) return -10;
    if (r->s) stream_close(r);
    result_free(r);
    return 0;
}
/* ponytail: one global lock guards every handle read against a concurrent free; per-handle refcounts if it contends */
#define RES_READ(T, expr) { tab_lock(); Result* r = res_get(h); T v = expr; mtx_unlock(&g_tab_mu); return v; }
static const char* res_name(Result* r, long long i) { return r && i >= 0 && i < r->cols && r->names[i] ? r->names[i] : ""; }
long long salivo_db_rows(long long h) RES_READ(long long, r ? r->rows : -10)
long long salivo_db_cols(long long h) RES_READ(long long, r ? r->cols : -10)
long long salivo_db_affected(long long h) RES_READ(long long, r ? r->affected : -10)
long long salivo_db_last_id(long long h) RES_READ(long long, r ? r->last_id : -10)
char* salivo_db_col_name(long long h, long long i) RES_READ(char*, out_str(res_name(r, i), strlen(res_name(r, i))))
long long salivo_db_is_null(long long h, long long row, long long col) RES_READ(long long, !r || row < 0 || row >= r->rows || col < 0 || col >= r->cols ? -7 : r->cells[row * r->cols + col] == NULL)
char* salivo_db_text(long long h, long long row, long long col) {
    tab_lock();
    Result* r = res_get(h);
    const char* s = r && row >= 0 && row < r->rows && col >= 0 && col < r->cols && r->cells[row * r->cols + col] ? r->cells[row * r->cols + col] : "";
    char* v = out_str(s, strlen(s));
    mtx_unlock(&g_tab_mu);
    return v;
}
long long salivo_db_in_txn(long long lease) { tab_lock(); Lease* l = lease_get(lease); long long v = !l ? -10 : l->busy ? -13 : l->c->in_txn; mtx_unlock(&g_tab_mu); return v; }
long long salivo_db_driver(long long pool) { Pool* p = pool_lock(pool); if (!p) return -10; long long v = p->url.driver; mtx_unlock(&p->mu); return v; }
/* Last error of a lease (> 0), or of a pool (as -pool), or the pool-open error (0) */
char* salivo_db_error(long long h) {
    char s[512];
    snprintf(s, sizeof s, "%s", g_last_err);
    if (h > 0) {
        tab_lock();
        Lease* l = lease_get(h);
        snprintf(s, sizeof s, "%s", !l ? "invalid connection handle" : l->busy ? "a statement is running on this connection" : l->c->err);
        mtx_unlock(&g_tab_mu);
    } else if (h < 0) {
        Pool* p = pool_lock(-h);
        snprintf(s, sizeof s, "%s", p ? p->err : "invalid pool handle");
        if (p) mtx_unlock(&p->mu);
    }
    return out_str(s, strlen(s));
}
/* 0 open, 1 idle, 2 in use, 3 max, 4 created, 5 waits, 6 timeouts, 7 broken, 8 rollbacks, 9 checkouts, 10 expired */
long long salivo_db_stat(long long pool, long long key) {
    Pool* p = pool_lock(pool);
    if (!p) return -10;
    long long v = key == 0 ? p->open : key == 1 ? p->nidle : key == 2 ? p->in_use : key == 3 ? p->max : key == 4 ? p->stat_created :
        key == 5 ? p->stat_waits : key == 6 ? p->stat_timeouts : key == 7 ? p->stat_broken : key == 8 ? p->stat_rollbacks : key == 9 ? p->stat_checkouts :
        key == 10 ? p->stat_expired : -7;
    mtx_unlock(&p->mu);
    return v;
}
/* Marks idle connections for a health check on their next checkout (after a server restart) */
long long salivo_db_set_idle_check(long long pool, long long ms) { Pool* p = pool_lock(pool); if (!p) return -10; p->idle_check_ms = ms < 0 ? 0 : ms; mtx_unlock(&p->mu); return 0; }
/* key: 0 connect timeout (> 0), 1 query timeout, 2 idle timeout, 3 connection max lifetime; ms, 0 = off */
long long salivo_db_set_limit(long long pool, long long key, long long ms) {
    if (key < 0 || key > 3 || ms < 0 || (key == 0 && ms == 0)) return -7;
    Pool* p = pool_lock(pool);
    if (!p) return -10;
    if (key == 0) p->connect_ms = ms; else if (key == 1) p->query_ms = ms; else if (key == 2) p->idle_ms = ms; else p->life_ms = ms;
    mtx_unlock(&p->mu);
    if (key >= 2 && ms > 0) reaper_start();
    return 0;
}
static long long db_pool_close_impl(long long pool) {
    Pool* p = pool_lock(pool);
    if (!p) return -10;
    if (p->closing) { mtx_unlock(&p->mu); return -12; }
    p->closing = 1; /* new checkouts and waiters fail with -12; leases still out close on release */
    while (p->nidle) { conn_close(p->idle[--p->nidle]); p->open--; }
    cond_broadcast(&p->cv);
    pool_unlock(p);
    return 0;
}
/* Closes the pool and waits up to ms for every lease to come back: 0 drained, -4 leases still out
 * (their running statements are cancelled, new ones refused, and they close when released) */
static long long db_pool_close_wait_impl(long long pool, long long ms) {
    Pool* p = pool_lock(pool);
    if (!p) return -10;
    if (!p->closing) { p->closing = 1; while (p->nidle) { conn_close(p->idle[--p->nidle]); p->open--; } cond_broadcast(&p->cv); }
    p->busy++;
    long long deadline = now_ms() + (ms > 0 ? ms : 0), rc = 0;
    while (p->in_use > 0) {
        long long left = deadline - now_ms();
        if (left <= 0) { rc = -4; break; }
        cond_wait_ms(&p->cv, &p->mu, left);
    }
    mtx_unlock(&p->mu);
    if (rc) {
        tab_lock();
        for (int i = 1; i <= MAX_LEASES; i++) if (g_leases[i].used && g_leases[i].pool == pool) g_leases[i].c->kill = 1;
        mtx_unlock(&g_tab_mu);
        snprintf(p->err, sizeof p->err, "shutdown timed out with leases still out");
    }
    mtx_lock(&p->mu);
    p->busy--;
    pool_unlock(p);
    return rc;
}

/* Public entry points: inline on ordinary threads, on the blocking pool inside async tasks */
static long long tramp_pool_open(long long p) { DbCall* c = (DbCall*)(intptr_t)p; return db_pool_open_impl(c->s1, c->a, c->b); }
static long long tramp_acquire(long long p) { DbCall* c = (DbCall*)(intptr_t)p; return db_acquire_impl(c->a, c->b); }
static long long tramp_release(long long p) { DbCall* c = (DbCall*)(intptr_t)p; return db_release_impl(c->a); }
static long long tramp_query(long long p) { DbCall* c = (DbCall*)(intptr_t)p; return db_query_impl(c->a, c->s1, c->s2, c->b, c->c); }
static long long tramp_stream(long long p) { DbCall* c = (DbCall*)(intptr_t)p; return db_stream_impl(c->a, c->s1, c->s2, c->b, c->c); }
static long long tramp_next(long long p) { DbCall* c = (DbCall*)(intptr_t)p; return db_next_impl(c->a); }
static long long tramp_result_free(long long p) { DbCall* c = (DbCall*)(intptr_t)p; return db_result_free_impl(c->a); }
static long long tramp_pool_close(long long p) { DbCall* c = (DbCall*)(intptr_t)p; return db_pool_close_impl(c->a); }
static long long tramp_pool_close_wait(long long p) { DbCall* c = (DbCall*)(intptr_t)p; return db_pool_close_wait_impl(c->a, c->b); }
long long salivo_db_pool_open(const char* url, long long max_conns, long long timeout_ms) { DbCall c = { max_conns, timeout_ms, 0, url, NULL, 0 }; return run_offloaded(tramp_pool_open, &c); }
long long salivo_db_acquire(long long pool) { DbCall c = { pool, 0, 0, NULL, NULL, 0 }; return run_offloaded(tramp_acquire, &c); }
long long salivo_db_acquire_with(long long pool, long long tok) { DbCall c = { pool, tok, 0, NULL, NULL, 0 }; return run_offloaded(tramp_acquire, &c); }
long long salivo_db_release(long long lease) { DbCall c = { lease, 0, 0, NULL, NULL, 0 }; return run_offloaded(tramp_release, &c); }
long long salivo_db_query(long long lease, const char* sql, const char* params, long long nparams) { DbCall c = { lease, nparams, 0, sql, params, 0 }; return run_offloaded(tramp_query, &c); }
long long salivo_db_query_with(long long lease, const char* sql, const char* params, long long nparams, long long tok) { DbCall c = { lease, nparams, tok, sql, params, 0 }; return run_offloaded(tramp_query, &c); }
long long salivo_db_stream(long long lease, const char* sql, const char* params, long long nparams, long long tok) { DbCall c = { lease, nparams, tok, sql, params, 0 }; return run_offloaded(tramp_stream, &c); }
long long salivo_db_next(long long h) { DbCall c = { h, 0, 0, NULL, NULL, 0 }; return run_offloaded(tramp_next, &c); }
long long salivo_db_result_free(long long h) { DbCall c = { h, 0, 0, NULL, NULL, 0 }; return run_offloaded(tramp_result_free, &c); }
long long salivo_db_pool_close(long long pool) { DbCall c = { pool, 0, 0, NULL, NULL, 0 }; return run_offloaded(tramp_pool_close, &c); }
long long salivo_db_pool_close_wait(long long pool, long long ms) { DbCall c = { pool, ms, 0, NULL, NULL, 0 }; return run_offloaded(tramp_pool_close_wait, &c); }
