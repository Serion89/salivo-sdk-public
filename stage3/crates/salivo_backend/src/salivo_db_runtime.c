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

/* ---- sockets ----------------------------------------------------------------------------------- */
static db_sock tcp_connect(const char* host, int port, long long timeout_ms) {
    net_init();
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0) return DB_BAD_SOCK;
    db_sock s = DB_BAD_SOCK;
    for (struct addrinfo* a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == DB_BAD_SOCK) continue;
#ifdef _WIN32
        DWORD tv = (DWORD)timeout_ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
#else
        struct timeval tv = { (time_t)(timeout_ms / 1000), (suseconds_t)((timeout_ms % 1000) * 1000) };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        db_closesock(s);
        s = DB_BAD_SOCK;
    }
    freeaddrinfo(res);
    return s;
}
static int send_all(db_sock s, const uint8_t* p, size_t n) {
    while (n) { int r = send(s, (const char*)p, (int)(n > 65536 ? 65536 : n), 0); if (r <= 0) return -1; p += r; n -= (size_t)r; }
    return 0;
}
static int recv_exact(db_sock s, uint8_t* p, size_t n) {
    while (n) { int r = recv(s, (char*)p, (int)(n > 65536 ? 65536 : n), 0); if (r <= 0) return -1; p += r; n -= (size_t)r; }
    return 0;
}

/* ---- result sets ------------------------------------------------------------------------------ */
typedef struct {
    int cols, rows, cap_rows;
    char** names;
    char** cells; /* rows * cols, NULL = SQL NULL */
    long long affected, last_id;
} Result;
static void result_free(Result* r) {
    if (!r) return;
    for (int i = 0; i < r->cols; i++) free(r->names[i]);
    for (long i = 0; i < (long)r->rows * r->cols; i++) free(r->cells[i]);
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

/* ---- connections ------------------------------------------------------------------------------ */
enum { DRV_SQLITE = 1, DRV_PG = 2, DRV_MYSQL = 3 };
typedef struct {
    int driver;
    db_sock sock;
    sqlite3* lite;
    int broken, in_txn, std_strings, no_backslash;
    long long last_used;
    char err[512];
    uint8_t seq; /* mysql packet sequence */
} Conn;
typedef struct { int driver; char host[256], user[128], pass[128], db[128], path[512]; int port; } DbUrl;

static void set_err(Conn* c, const char* fmt, const char* a) { snprintf(c->err, sizeof c->err, fmt, a ? a : ""); }

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
    if (recv_exact(c->sock, h, 5)) { c->broken = 1; set_err(c, "connection lost%s", ""); return -1; }
    uint32_t len = (uint32_t)h[1] << 24 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 8 | h[4];
    if (len < 4 || len > (1u << 30)) { c->broken = 1; set_err(c, "protocol error%s", ""); return -1; }
    body->n = 0;
    if (buf_reserve(body, len - 4 + 1)) return -1;
    if (recv_exact(c->sock, body->p, len - 4)) { c->broken = 1; set_err(c, "connection lost%s", ""); return -1; }
    body->n = len - 4;
    body->p[body->n] = 0;
    *type = h[0];
    return 0;
}
static int pg_send(Conn* c, uint8_t type, const Buf* body) {
    uint8_t h[5];
    uint32_t len = (uint32_t)body->n + 4;
    h[0] = type; h[1] = (uint8_t)(len >> 24); h[2] = (uint8_t)(len >> 16); h[3] = (uint8_t)(len >> 8); h[4] = (uint8_t)len;
    if (send_all(c->sock, h, 5) || send_all(c->sock, body->p, body->n)) { c->broken = 1; set_err(c, "connection lost%s", ""); return -1; }
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
static int pg_connect(Conn* c, const DbUrl* u, long long timeout_ms) {
    c->sock = tcp_connect(u->host, u->port, timeout_ms);
    if (c->sock == DB_BAD_SOCK) { set_err(c, "cannot connect to %s", u->host); return -1; }
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
    if (send_all(c->sock, h, 4) || send_all(c->sock, m.p, m.n)) { set_err(c, "connection lost%s", ""); goto out; }
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
            const char* v = k + strlen(k) + 1;
            if (!strcmp(k, "standard_conforming_strings")) c->std_strings = !strcmp(v, "on");
            continue;
        }
        if (t == 'Z') { c->in_txn = in.n && in.p[0] != 'I'; rc = 0; break; }
        /* K (BackendKeyData), N (notice) and others are not needed */
    }
out:
    buf_free(&m); buf_free(&in);
    return rc;
}
static Result* pg_query(Conn* c, const char* sql) {
    Buf q = {0}, in = {0};
    buf_str0(&q, sql);
    Result* r = (Result*)calloc(1, sizeof(Result));
    int failed = 0;
    r->affected = -1;
    if (pg_send(c, 'Q', &q)) { failed = 1; goto out; }
    for (;;) {
        uint8_t t;
        if (pg_read(c, &t, &in)) { failed = 1; goto out; }
        if (t == 'T') {
            int n = in.p[0] << 8 | in.p[1];
            for (int i = 0; i < r->cols; i++) free(r->names[i]);
            free(r->names);
            r->cols = n;
            r->names = (char**)calloc((size_t)(n ? n : 1), sizeof(char*));
            size_t off = 2;
            for (int i = 0; i < n && off < in.n; i++) { size_t l = strlen((char*)in.p + off); r->names[i] = dup_n((char*)in.p + off, l); off += l + 1 + 18; }
        } else if (t == 'D') {
            int n = in.p[0] << 8 | in.p[1];
            if (n != r->cols || result_add_row(r)) { set_err(c, "protocol error%s", ""); failed = 1; c->broken = 1; goto out; }
            size_t off = 2;
            for (int i = 0; i < n; i++) {
                int32_t l = (int32_t)((uint32_t)in.p[off] << 24 | (uint32_t)in.p[off + 1] << 16 | (uint32_t)in.p[off + 2] << 8 | in.p[off + 3]);
                off += 4;
                if (l >= 0) { r->cells[(long)(r->rows - 1) * r->cols + i] = dup_n((char*)in.p + off, (size_t)l); off += (size_t)l; }
            }
        } else if (t == 'C') {
            const char* tag = (const char*)in.p;
            const char* sp = strrchr(tag, ' ');
            if (sp && sp[1] >= '0' && sp[1] <= '9' && strncmp(tag, "SELECT", 6)) r->affected = atoll(sp + 1);
        } else if (t == 'E') {
            pg_error(c, &in); failed = 1; /* ReadyForQuery still follows */
        } else if (t == 'Z') {
            c->in_txn = in.n && in.p[0] != 'I';
            break;
        }
    }
out:
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
        if (recv_exact(c->sock, h, 4)) { c->broken = 1; set_err(c, "connection lost%s", ""); return -1; }
        size_t len = (size_t)h[0] | (size_t)h[1] << 8 | (size_t)h[2] << 16;
        c->seq = (uint8_t)(h[3] + 1);
        if (buf_reserve(b, len + 1)) return -1;
        if (recv_exact(c->sock, b->p + b->n, len)) { c->broken = 1; set_err(c, "connection lost%s", ""); return -1; }
        b->n += len;
        if (len < 0xFFFFFF) break;
    }
    b->p[b->n] = 0;
    return 0;
}
static int my_send(Conn* c, const uint8_t* p, size_t n) {
    uint8_t h[4] = { (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), c->seq++ };
    if (n >= 0xFFFFFF || send_all(c->sock, h, 4) || send_all(c->sock, p, n)) { c->broken = 1; set_err(c, "connection lost%s", ""); return -1; }
    return 0;
}
static uint64_t my_lenenc(const uint8_t** p, const uint8_t* end, int* is_null) {
    *is_null = 0;
    if (*p >= end) return 0;
    uint8_t f = *(*p)++;
    if (f < 0xfb) return f;
    if (f == 0xfb) { *is_null = 1; return 0; }
    int k = f == 0xfc ? 2 : f == 0xfd ? 3 : 8;
    uint64_t v = 0;
    for (int i = 0; i < k && *p < end; i++) v |= (uint64_t)*(*p)++ << (8 * i);
    return v;
}
static void my_err(Conn* c, const Buf* b) {
    int code = b->n >= 3 ? b->p[1] | b->p[2] << 8 : 0;
    const char* msg = b->n > 9 && b->p[3] == '#' ? (const char*)b->p + 9 : (const char*)b->p + 3;
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
static int my_connect(Conn* c, const DbUrl* u, long long timeout_ms) {
    c->sock = tcp_connect(u->host, u->port, timeout_ms);
    if (c->sock == DB_BAD_SOCK) { set_err(c, "cannot connect to %s", u->host); return -1; }
    Buf in = {0}, out = {0};
    int rc = -1;
    uint8_t salt[20];
    char plugin[64] = "mysql_native_password";
    c->seq = 0;
    if (my_read(c, &in)) goto out;
    if (in.n && in.p[0] == 0xff) { my_err(c, &in); goto out; }
    {
        const uint8_t *p = in.p, *end = in.p + in.n;
        if (*p++ != 10) { set_err(c, "unsupported MySQL protocol%s", ""); goto out; }
        p += strlen((const char*)p) + 1; /* server version */
        p += 4; /* thread id */
        memcpy(salt, p, 8); p += 8 + 1;
        uint32_t caps = p[0] | p[1] << 8; p += 2;
        p += 1 + 2; /* charset, status */
        caps |= (uint32_t)(p[0] | p[1] << 8) << 16; p += 2;
        int auth_len = *p++;
        p += 10;
        size_t part2 = auth_len > 8 ? (size_t)(auth_len - 8) : 13;
        if (part2 > 13) part2 = 13;
        if (p + part2 > end) { set_err(c, "malformed MySQL handshake%s", ""); goto out; }
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
    {
        const uint8_t* p = in.p; int nul;
        uint64_t ncols = my_lenenc(&p, in.p + in.n, &nul);
        if (ncols == 0 || ncols > 4096) { set_err(c, "protocol error%s", ""); c->broken = 1; goto out; }
        r->cols = (int)ncols;
        r->names = (char**)calloc((size_t)ncols, sizeof(char*));
        for (uint64_t i = 0; i < ncols; i++) {
            if (my_read(c, &in)) goto out;
            const uint8_t *q = in.p, *end = in.p + in.n;
            for (int f = 0; f < 4; f++) { uint64_t l = my_lenenc(&q, end, &nul); q += l; } /* catalog, schema, table, org_table */
            uint64_t l = my_lenenc(&q, end, &nul);
            r->names[i] = dup_n((const char*)q, q + l <= end ? (size_t)l : 0);
        }
        for (;;) {
            if (my_read(c, &in)) goto out;
            if (in.n && in.p[0] == 0xfe && in.n < 0xFFFFFF) { my_ok(c, &in, NULL); break; } /* OK/EOF terminator */
            if (in.n && in.p[0] == 0xff) { my_err(c, &in); goto out; }
            if (result_add_row(r)) goto out;
            const uint8_t *q = in.p, *end = in.p + in.n;
            for (int i = 0; i < r->cols; i++) {
                uint64_t l = my_lenenc(&q, end, &nul);
                if (!nul) { if (q + l > end) { set_err(c, "protocol error%s", ""); c->broken = 1; goto out; } r->cells[(long)(r->rows - 1) * r->cols + i] = dup_n((const char*)q, (size_t)l); q += l; }
            }
        }
        r->affected = -1;
        failed = 0;
    }
out:
    buf_free(&pkt); buf_free(&in);
    if (failed) { result_free(r); return NULL; }
    return r;
}

/* SQLite --------------------------------------------------------------------------------------- */
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
            r->names = (char**)calloc((size_t)n, sizeof(char*));
            for (int i = 0; i < n; i++) { const char* nm = sqlite3_column_name(st, i); r->names[i] = dup_n(nm, strlen(nm)); }
        }
        int s;
        while ((s = sqlite3_step(st)) == SQLITE_ROW) {
            if (result_add_row(r)) break;
            for (int i = 0; i < n; i++) if (sqlite3_column_type(st, i) != SQLITE_NULL) { const char* t = (const char*)sqlite3_column_text(st, i); r->cells[(long)(r->rows - 1) * r->cols + i] = dup_n(t, (size_t)sqlite3_column_bytes(st, i)); }
        }
        if (s != SQLITE_DONE) { set_err(c, "%s", sqlite3_errmsg(c->lite)); sqlite3_finalize(st); result_free(r); return NULL; }
        if (!n) { r->affected = sqlite3_changes(c->lite); r->last_id = sqlite3_last_insert_rowid(c->lite); }
        sqlite3_finalize(st);
    }
    c->in_txn = !sqlite3_get_autocommit(c->lite);
    return r;
}

static Result* conn_query(Conn* c, const char* sql) {
    c->err[0] = 0;
    if (c->broken) { set_err(c, "connection is broken%s", ""); return NULL; }
    Result* r = c->driver == DRV_PG ? pg_query(c, sql) : c->driver == DRV_MYSQL ? my_command(c, 0x03, sql) : lite_query(c, sql);
    c->last_used = now_ms();
    return r;
}
static void conn_close(Conn* c) {
    if (!c) return;
    if (c->driver == DRV_PG && c->sock != DB_BAD_SOCK) { Buf e = {0}; pg_send(c, 'X', &e); }
    if (c->driver == DRV_MYSQL && c->sock != DB_BAD_SOCK) { uint8_t q = 0x01; c->seq = 0; my_send(c, &q, 1); }
    if (c->sock != DB_BAD_SOCK) db_closesock(c->sock);
    if (c->lite) sqlite3_close(c->lite);
    free(c);
}
static Conn* conn_open(const DbUrl* u, long long timeout_ms, char* err, size_t errcap) {
    Conn* c = (Conn*)calloc(1, sizeof(Conn));
    c->driver = u->driver;
    c->sock = DB_BAD_SOCK;
    int rc;
    if (u->driver == DRV_SQLITE) {
        rc = sqlite3_open(u->path, &c->lite) == SQLITE_OK ? 0 : -1;
        if (rc) set_err(c, "%s", c->lite ? sqlite3_errmsg(c->lite) : "cannot open database");
        else sqlite3_busy_timeout(c->lite, (int)timeout_ms);
        c->std_strings = 1;
    } else if (u->driver == DRV_PG) {
        rc = pg_connect(c, u, timeout_ms);
    } else {
        rc = my_connect(c, u, timeout_ms);
    }
    if (rc) { snprintf(err, errcap, "%s", c->err); conn_close(c); return NULL; }
    c->last_used = now_ms();
    return c;
}
static int conn_ping(Conn* c) {
    if (c->broken) return -1;
    if (c->driver == DRV_SQLITE) return 0;
    Result* r = c->driver == DRV_MYSQL ? my_command(c, 0x0e, NULL) : pg_query(c, "SELECT 1");
    int ok = r != NULL;
    result_free(r);
    return ok ? 0 : -1;
}

/* Quote ? parameters as literals for this connection. params is `<len>:<bytes>` repeated, one entry
 * per parameter (lengths in bytes, so values may contain any byte except NUL). */
static char* bind_params(Conn* c, const char* sql, const char* params, int nparams) {
    if (nparams == 0) return dup_n(sql, strlen(sql));
    if (c->driver == DRV_PG && !c->std_strings) { set_err(c, "parameters need standard_conforming_strings=on%s", ""); return NULL; }
    Buf o = {0};
    const char* p = params;
    int used = 0;
    char quote = 0;
    for (const char* s = sql; *s; s++) {
        if (quote) { buf_byte(&o, (uint8_t)*s); if (*s == quote) quote = 0; continue; }
        if (*s == '\'' || *s == '"' || *s == '`') { quote = *s; buf_byte(&o, (uint8_t)*s); continue; }
        if (*s != '?') { buf_byte(&o, (uint8_t)*s); continue; }
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
typedef struct {
    int used, closing;
    DbUrl url;
    int max, open, in_use;
    long long timeout_ms, idle_check_ms;
    Conn* idle[MAX_CONNS_PER_POOL];
    int nidle;
    db_mutex mu;
    db_cond cv;
    long long stat_created, stat_waits, stat_timeouts, stat_broken, stat_rollbacks, stat_checkouts;
    char err[512];
} Pool;
typedef struct { Conn* c; int pool; int used; } Lease;
static Pool g_pools[MAX_POOLS + 1];
static Lease g_leases[MAX_POOLS * MAX_CONNS_PER_POOL + 1];
static Result* g_results[MAX_RESULTS + 1];
static char g_last_err[512];
#ifdef _WIN32
static db_mutex g_tab_mu = SRWLOCK_INIT;
#else
static db_mutex g_tab_mu = PTHREAD_MUTEX_INITIALIZER;
#endif
static void tab_lock(void) { mtx_lock(&g_tab_mu); }

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
        mtx_init(&p->mu); cond_init(&p->cv);
    }
    mtx_unlock(&g_tab_mu);
    if (h < 0) { snprintf(g_last_err, sizeof g_last_err, "too many pools"); return h; }
    /* one eager connection proves the URL and credentials work */
    char err[512];
    Conn* c = conn_open(&u, g_pools[h].timeout_ms, err, sizeof err);
    if (!c) { snprintf(g_last_err, sizeof g_last_err, "%s", err); g_pools[h].used = 0; return -8; }
    Pool* p = &g_pools[h];
    mtx_lock(&p->mu);
    p->idle[p->nidle++] = c; p->open = 1; p->stat_created = 1;
    mtx_unlock(&p->mu);
    return h;
}
static Pool* pool_get(long long h) { return h >= 1 && h <= MAX_POOLS && g_pools[h].used && !g_pools[h].closing ? &g_pools[h] : NULL; }

/* Returns a lease handle (> 0) or a negative error: -4 Timeout, -8 connect failure, -10 bad handle */
static long long db_acquire_impl(long long pool) {
    Pool* p = pool_get(pool);
    if (!p) return -10;
    long long deadline = now_ms() + p->timeout_ms;
    Conn* c = NULL;
    mtx_lock(&p->mu);
    for (;;) {
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
            }
            c = cand;
        }
        if (c) break;
        if (p->open < p->max) {
            p->open++;
            mtx_unlock(&p->mu);
            char err[512];
            c = conn_open(&p->url, p->timeout_ms, err, sizeof err);
            mtx_lock(&p->mu);
            if (!c) { p->open--; snprintf(p->err, sizeof p->err, "%s", err); cond_broadcast(&p->cv); mtx_unlock(&p->mu); return -8; }
            p->stat_created++;
            break;
        }
        long long left = deadline - now_ms();
        if (left <= 0) { p->stat_timeouts++; snprintf(p->err, sizeof p->err, "timed out waiting for a pooled connection"); mtx_unlock(&p->mu); return -4; }
        p->stat_waits++;
        cond_wait_ms(&p->cv, &p->mu, left);
    }
    p->in_use++;
    p->stat_checkouts++;
    mtx_unlock(&p->mu);
    tab_lock();
    long long h = -11;
    for (int i = 1; i <= MAX_POOLS * MAX_CONNS_PER_POOL; i++) if (!g_leases[i].used) { g_leases[i].used = 1; g_leases[i].c = c; g_leases[i].pool = (int)pool; h = i; break; }
    mtx_unlock(&g_tab_mu);
    return h;
}
static Lease* lease_get(long long h) { return h >= 1 && h <= MAX_POOLS * MAX_CONNS_PER_POOL && g_leases[h].used ? &g_leases[h] : NULL; }

/* Gives the connection back; an open transaction is rolled back first, a broken connection closed */
static long long db_release_impl(long long lease) {
    tab_lock();
    Lease* l = lease_get(lease);
    Lease copy = l ? *l : (Lease){0};
    if (l) l->used = 0;
    mtx_unlock(&g_tab_mu);
    if (!l) return -10;
    Pool* p;
    Conn* c = copy.c;
    int rolled = 0;
    if (!c->broken && c->in_txn) { Result* r = conn_query(c, "ROLLBACK"); result_free(r); rolled = 1; if (c->in_txn) c->broken = 1; }
    p = copy.pool >= 1 && copy.pool <= MAX_POOLS ? &g_pools[copy.pool] : NULL;
    if (!p || !p->used) { conn_close(c); return rolled; }
    mtx_lock(&p->mu);
    p->in_use--;
    if (rolled) p->stat_rollbacks++;
    int drop = c->broken || p->closing;
    if (drop) { p->open--; if (c->broken) p->stat_broken++; }
    else p->idle[p->nidle++] = c;
    if (p->closing && p->in_use == 0) p->used = 0; /* the slot is reusable once every lease is back */
    cond_broadcast(&p->cv);
    mtx_unlock(&p->mu);
    if (drop) conn_close(c);
    return rolled;
}

/* params: see bind_params. Returns a result handle or a negative error. */
static long long db_query_impl(long long lease, const char* sql, const char* params, long long nparams) {
    Lease* l = lease_get(lease);
    if (!l) return -10;
    Conn* c = l->c;
    char* bound = bind_params(c, sql ? sql : "", params ? params : "", (int)nparams);
    if (!bound) return -7;
    Result* r = conn_query(c, bound);
    free(bound);
    if (!r) return c->broken ? -3 : -18;
    tab_lock();
    long long h = -11;
    for (int i = 1; i <= MAX_RESULTS; i++) if (!g_results[i]) { g_results[i] = r; h = i; break; }
    mtx_unlock(&g_tab_mu);
    if (h < 0) result_free(r);
    return h;
}
static Result* res_get(long long h) { return h >= 1 && h <= MAX_RESULTS ? g_results[h] : NULL; }
long long salivo_db_result_free(long long h) {
    tab_lock();
    Result* r = res_get(h);
    if (r) g_results[h] = NULL;
    mtx_unlock(&g_tab_mu);
    result_free(r);
    return r ? 0 : -10;
}
long long salivo_db_rows(long long h) { Result* r = res_get(h); return r ? r->rows : -10; }
long long salivo_db_cols(long long h) { Result* r = res_get(h); return r ? r->cols : -10; }
long long salivo_db_affected(long long h) { Result* r = res_get(h); return r ? r->affected : -10; }
long long salivo_db_last_id(long long h) { Result* r = res_get(h); return r ? r->last_id : -10; }
char* salivo_db_col_name(long long h, long long i) {
    Result* r = res_get(h);
    const char* s = r && i >= 0 && i < r->cols && r->names[i] ? r->names[i] : "";
    return out_str(s, strlen(s));
}
long long salivo_db_is_null(long long h, long long row, long long col) {
    Result* r = res_get(h);
    if (!r || row < 0 || row >= r->rows || col < 0 || col >= r->cols) return -7;
    return r->cells[row * r->cols + col] == NULL;
}
char* salivo_db_text(long long h, long long row, long long col) {
    Result* r = res_get(h);
    const char* s = r && row >= 0 && row < r->rows && col >= 0 && col < r->cols && r->cells[row * r->cols + col] ? r->cells[row * r->cols + col] : "";
    return out_str(s, strlen(s));
}
long long salivo_db_in_txn(long long lease) { Lease* l = lease_get(lease); return l ? l->c->in_txn : -10; }
long long salivo_db_driver(long long pool) { Pool* p = pool_get(pool); return p ? p->url.driver : -10; }
/* Last error of a lease (> 0), or of a pool (as -pool), or the pool-open error (0) */
char* salivo_db_error(long long h) {
    const char* s = g_last_err;
    if (h > 0) { Lease* l = lease_get(h); s = l ? l->c->err : "invalid connection handle"; }
    else if (h < 0) { Pool* p = pool_get(-h); s = p ? p->err : "invalid pool handle"; }
    return out_str(s, strlen(s));
}
/* 0 open, 1 idle, 2 in use, 3 max, 4 created, 5 waits, 6 timeouts, 7 broken, 8 rollbacks, 9 checkouts */
long long salivo_db_stat(long long pool, long long key) {
    Pool* p = pool_get(pool);
    if (!p) return -10;
    mtx_lock(&p->mu);
    long long v = key == 0 ? p->open : key == 1 ? p->nidle : key == 2 ? p->in_use : key == 3 ? p->max : key == 4 ? p->stat_created :
        key == 5 ? p->stat_waits : key == 6 ? p->stat_timeouts : key == 7 ? p->stat_broken : key == 8 ? p->stat_rollbacks : key == 9 ? p->stat_checkouts : -7;
    mtx_unlock(&p->mu);
    return v;
}
/* Marks idle connections for a health check on their next checkout (after a server restart) */
long long salivo_db_set_idle_check(long long pool, long long ms) { Pool* p = pool_get(pool); if (!p) return -10; p->idle_check_ms = ms < 0 ? 0 : ms; return 0; }
static long long db_pool_close_impl(long long pool) {
    Pool* p = pool_get(pool);
    if (!p) return -10;
    mtx_lock(&p->mu);
    while (p->nidle) { conn_close(p->idle[--p->nidle]); p->open--; }
    p->closing = 1; /* leases still out close on release */
    if (p->in_use == 0) p->used = 0;
    cond_broadcast(&p->cv);
    mtx_unlock(&p->mu);
    return 0;
}

/* Public entry points: inline on ordinary threads, on the blocking pool inside async tasks */
static long long tramp_pool_open(long long p) { DbCall* c = (DbCall*)(intptr_t)p; return db_pool_open_impl(c->s1, c->a, c->b); }
static long long tramp_acquire(long long p) { DbCall* c = (DbCall*)(intptr_t)p; return db_acquire_impl(c->a); }
static long long tramp_release(long long p) { DbCall* c = (DbCall*)(intptr_t)p; return db_release_impl(c->a); }
static long long tramp_query(long long p) { DbCall* c = (DbCall*)(intptr_t)p; return db_query_impl(c->a, c->s1, c->s2, c->b); }
static long long tramp_pool_close(long long p) { DbCall* c = (DbCall*)(intptr_t)p; return db_pool_close_impl(c->a); }
long long salivo_db_pool_open(const char* url, long long max_conns, long long timeout_ms) { DbCall c = { max_conns, timeout_ms, 0, url, NULL, 0 }; return run_offloaded(tramp_pool_open, &c); }
long long salivo_db_acquire(long long pool) { DbCall c = { pool, 0, 0, NULL, NULL, 0 }; return run_offloaded(tramp_acquire, &c); }
long long salivo_db_release(long long lease) { DbCall c = { lease, 0, 0, NULL, NULL, 0 }; return run_offloaded(tramp_release, &c); }
long long salivo_db_query(long long lease, const char* sql, const char* params, long long nparams) { DbCall c = { lease, nparams, 0, sql, params, 0 }; return run_offloaded(tramp_query, &c); }
long long salivo_db_pool_close(long long pool) { DbCall c = { pool, 0, 0, NULL, NULL, 0 }; return run_offloaded(tramp_pool_close, &c); }
