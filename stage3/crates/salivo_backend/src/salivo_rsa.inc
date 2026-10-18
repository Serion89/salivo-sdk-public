/* RSA PKCS#1 v1.5 with SHA-256 (JWT RS256), included at the end of salivo_task_runtime.c.
 *
 * Keys come from PEM (SubjectPublicKeyInfo, PKCS#1 public, PKCS#1 private, PKCS#8 private) or from a
 * JWK's n / e, and are handed to Salivo code as one normalized string "rsa1:<n hex>:<e hex>:<d hex>"
 * (d empty for a public key). Big numbers are little-endian 32-bit limbs up to 4096-bit moduli;
 * modular exponentiation is Montgomery multiplication with a fixed 4-bit window whose table lookup
 * reads every entry, and the private exponent always runs over the full modulus length, so signing
 * time and memory access do not depend on the key's bits. */

#define RSA_MAX_LIMBS 128 /* 4096-bit moduli */

typedef struct { uint32_t w[RSA_MAX_LIMBS]; int n; } RsaBn;

static void rsa_wipe(void* p, size_t n) {
    volatile unsigned char* v = (volatile unsigned char*)p;
    while (n--) *v++ = 0;
}

/* Big-endian bytes into limbs (k limbs, high bytes beyond the buffer are zero); -1 if it does not fit */
static int rsa_bn_from_bytes(RsaBn* a, const uint8_t* b, size_t len, int k) {
    memset(a, 0, sizeof *a);
    while (len > 0 && b[0] == 0) { b++; len--; }
    if (len > (size_t)k * 4 || k > RSA_MAX_LIMBS) return -1;
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        a->w[bit / 32] |= (uint32_t)b[i] << (bit % 32);
    }
    a->n = k;
    return 0;
}

static void rsa_bn_to_bytes(const RsaBn* a, uint8_t* out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        out[i] = (bit / 32 < (size_t)a->n) ? (uint8_t)(a->w[bit / 32] >> (bit % 32)) : 0;
    }
}

/* a >= b (same limb count), branch-free over the limbs */
static uint32_t rsa_bn_ge(const uint32_t* a, const uint32_t* b, int k) {
    uint32_t gt = 0, lt = 0;
    for (int i = k - 1; i >= 0; i--) {
        uint32_t x = a[i], y = b[i];
        uint32_t g = (uint32_t)(x > y) & ~(gt | lt);
        uint32_t l = (uint32_t)(x < y) & ~(gt | lt);
        gt |= g;
        lt |= l;
    }
    return 1 ^ lt;
}

/* r = a - (mask ? b : 0) over k limbs; returns the borrow */
static uint32_t rsa_bn_sub_masked(uint32_t* r, const uint32_t* a, const uint32_t* b, int k, uint32_t mask) {
    uint64_t borrow = 0;
    for (int i = 0; i < k; i++) {
        uint64_t d = (uint64_t)a[i] - (b[i] & mask) - borrow;
        r[i] = (uint32_t)d;
        borrow = (d >> 63) & 1;
    }
    return (uint32_t)borrow;
}

typedef struct {
    RsaBn n;
    uint32_t n0inv; /* -n^-1 mod 2^32 */
    RsaBn rr;       /* R^2 mod n, R = 2^(32k) */
    int k;
} RsaMont;

/* Montgomery product r = a * b / R mod n (CIOS), constant time */
static void rsa_mont_mul(const RsaMont* m, uint32_t* r, const uint32_t* a, const uint32_t* b) {
    int k = m->k;
    uint32_t t[RSA_MAX_LIMBS + 2];
    memset(t, 0, sizeof(uint32_t) * (size_t)(k + 2));
    for (int i = 0; i < k; i++) {
        uint64_t c = 0;
        for (int j = 0; j < k; j++) {
            uint64_t s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + c;
            t[j] = (uint32_t)s;
            c = s >> 32;
        }
        uint64_t s = (uint64_t)t[k] + c;
        t[k] = (uint32_t)s;
        t[k + 1] = (uint32_t)(s >> 32);
        uint32_t u = t[0] * m->n0inv;
        c = ((uint64_t)t[0] + (uint64_t)u * m->n.w[0]) >> 32;
        for (int j = 1; j < k; j++) {
            uint64_t s2 = (uint64_t)t[j] + (uint64_t)u * m->n.w[j] + c;
            t[j - 1] = (uint32_t)s2;
            c = s2 >> 32;
        }
        s = (uint64_t)t[k] + c;
        t[k - 1] = (uint32_t)s;
        t[k] = t[k + 1] + (uint32_t)(s >> 32);
    }
    /* t < 2n: subtract n once when t >= n (t[k] set means t >= R > n) */
    uint32_t tmp[RSA_MAX_LIMBS];
    uint32_t borrow = rsa_bn_sub_masked(tmp, t, m->n.w, k, 0xFFFFFFFFu);
    uint32_t use_sub = (uint32_t)0 - (uint32_t)((t[k] != 0) | (borrow == 0));
    for (int i = 0; i < k; i++) r[i] = (tmp[i] & use_sub) | (t[i] & ~use_sub);
    rsa_wipe(t, sizeof t);
    rsa_wipe(tmp, sizeof tmp);
}

static int rsa_mont_init(RsaMont* m, const RsaBn* n) {
    int k = n->n;
    while (k > 1 && n->w[k - 1] == 0) k--;
    if ((n->w[0] & 1) == 0 || k < 16) return -1; /* even or under 512 bits: not an RSA modulus */
    m->n = *n;
    m->n.n = k;
    m->k = k;
    uint32_t x = 1; /* Newton iteration: x = n0^-1 mod 2^32 */
    for (int i = 0; i < 5; i++) x *= 2 - n->w[0] * x;
    m->n0inv = (uint32_t)0 - x;
    /* rr = 2^(64k) mod n by doubling 1 with a conditional subtraction each step */
    uint32_t v[RSA_MAX_LIMBS + 1];
    memset(v, 0, sizeof v);
    v[0] = 1;
    for (int i = 0; i < 64 * k; i++) {
        uint32_t carry = 0;
        for (int j = 0; j < k; j++) {
            uint32_t nx = (v[j] >> 31);
            v[j] = (v[j] << 1) | carry;
            carry = nx;
        }
        uint32_t tmp[RSA_MAX_LIMBS];
        uint32_t borrow = rsa_bn_sub_masked(tmp, v, m->n.w, k, 0xFFFFFFFFu);
        uint32_t use = (uint32_t)0 - (uint32_t)(carry | (borrow == 0));
        for (int j = 0; j < k; j++) v[j] = (tmp[j] & use) | (v[j] & ~use);
    }
    memset(&m->rr, 0, sizeof m->rr);
    memcpy(m->rr.w, v, sizeof(uint32_t) * (size_t)k);
    m->rr.n = k;
    return 0;
}

/* r = base^exp mod n over exp_bits bits of exp (a multiple of 4), fixed window, constant-time lookup */
static void rsa_mod_exp(const RsaMont* m, uint32_t* r, const uint32_t* base, const RsaBn* exp, int exp_bits) {
    int k = m->k;
    uint32_t table[16][RSA_MAX_LIMBS];
    uint32_t one[RSA_MAX_LIMBS];
    memset(one, 0, sizeof one);
    one[0] = 1;
    rsa_mont_mul(m, table[0], one, m->rr.w);   /* 1 in Montgomery form */
    rsa_mont_mul(m, table[1], base, m->rr.w);  /* base in Montgomery form */
    for (int i = 2; i < 16; i++) rsa_mont_mul(m, table[i], table[i - 1], table[1]);
    uint32_t acc[RSA_MAX_LIMBS], sel[RSA_MAX_LIMBS];
    memcpy(acc, table[0], sizeof(uint32_t) * (size_t)k);
    for (int bit = exp_bits - 4; bit >= 0; bit -= 4) {
        for (int s = 0; s < 4; s++) rsa_mont_mul(m, acc, acc, acc);
        uint32_t w = 0;
        for (int b = 3; b >= 0; b--) {
            int pos = bit + b;
            uint32_t limb = pos / 32 < exp->n ? exp->w[pos / 32] : 0;
            w = (w << 1) | ((limb >> (pos % 32)) & 1);
        }
        memset(sel, 0, sizeof(uint32_t) * (size_t)k);
        for (uint32_t t = 0; t < 16; t++) {
            uint32_t mask = (uint32_t)0 - (uint32_t)(t == w);
            for (int j = 0; j < k; j++) sel[j] |= table[t][j] & mask;
        }
        rsa_mont_mul(m, acc, acc, sel);
    }
    rsa_mont_mul(m, r, acc, one); /* out of Montgomery form */
    rsa_wipe(table, sizeof table);
    rsa_wipe(acc, sizeof acc);
    rsa_wipe(sel, sizeof sel);
}

/* ---- encodings ---- */

static int rsa_b64_val(int c, int url) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == (url ? '-' : '+')) return 62;
    if (c == (url ? '_' : '/')) return 63;
    return -1;
}

/* Base64 (url = 0) or base64url (url = 1) into out; whitespace and '=' are skipped; -1 bad char or overflow */
static long rsa_b64_decode(const char* s, size_t n, int url, uint8_t* out, size_t cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t len = 0;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        if (c == '=' || c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        int v = rsa_b64_val(c, url);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (len >= cap) return -1;
            out[len++] = (uint8_t)(acc >> bits);
        }
    }
    return (long)len;
}

static char* rsa_b64url_encode(const uint8_t* b, size_t n) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t cap = (n * 4 + 2) / 3 + 1;
    char* tmp = (char*)malloc(cap);
    if (!tmp) return rt_out_cstr("");
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)b[i] << 16;
        if (i + 1 < n) v |= (uint32_t)b[i + 1] << 8;
        if (i + 2 < n) v |= b[i + 2];
        tmp[o++] = abc[(v >> 18) & 63];
        tmp[o++] = abc[(v >> 12) & 63];
        if (i + 1 < n) tmp[o++] = abc[(v >> 6) & 63];
        if (i + 2 < n) tmp[o++] = abc[v & 63];
    }
    char* r = rt_out_str(tmp, o);
    free(tmp);
    return r;
}

static int rsa_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ---- DER ---- */

/* One TLV at p (< end): tag, content start and length; -1 when malformed */
static int rsa_der_tlv(const uint8_t* p, const uint8_t* end, int* tag, const uint8_t** content, size_t* clen) {
    if (end - p < 2) return -1;
    *tag = p[0];
    size_t len = p[1];
    const uint8_t* q = p + 2;
    if (len & 0x80) {
        int nb = (int)(len & 0x7F);
        if (nb == 0 || nb > 4 || end - q < nb) return -1;
        len = 0;
        for (int i = 0; i < nb; i++) len = (len << 8) | *q++;
    }
    if ((size_t)(end - q) < len) return -1;
    *content = q;
    *clen = len;
    return 0;
}

typedef struct { const uint8_t* p; size_t n; } RsaSpan;

/* The INTEGER fields of a SEQUENCE (up to max), skipping nothing; returns how many were read */
static int rsa_der_ints(const uint8_t* seq, size_t len, RsaSpan* out, int max) {
    const uint8_t* p = seq;
    const uint8_t* end = seq + len;
    int count = 0;
    while (p < end && count < max) {
        int tag;
        const uint8_t* c;
        size_t cl;
        if (rsa_der_tlv(p, end, &tag, &c, &cl) || tag != 0x02) break;
        out[count].p = c;
        out[count].n = cl;
        count++;
        p = c + cl;
    }
    return count;
}

/* "rsa1:<n>:<e>:<d>" in hex from big-endian spans (d may be empty) */
static char* rsa_key_string(RsaSpan n, RsaSpan e, RsaSpan d) {
    static const char* hx = "0123456789abcdef";
    size_t cap = 8 + 2 * (n.n + e.n + d.n);
    char* s = (char*)malloc(cap);
    if (!s) return rt_out_cstr("");
    size_t o = 0;
    memcpy(s, "rsa1:", 5);
    o = 5;
    RsaSpan parts[3] = { n, e, d };
    for (int k = 0; k < 3; k++) {
        size_t i = 0;
        while (i < parts[k].n && parts[k].p[i] == 0) i++; /* DER sign byte */
        for (; i < parts[k].n; i++) {
            s[o++] = hx[parts[k].p[i] >> 4];
            s[o++] = hx[parts[k].p[i] & 15];
        }
        if (k < 2) s[o++] = ':';
    }
    char* r = rt_out_str(s, o);
    rsa_wipe(s, cap);
    free(s);
    return r;
}

static const uint8_t RSA_OID[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };

/* Normalized key from DER: PKCS#1 public/private, SubjectPublicKeyInfo, PKCS#8 private; "" if unknown */
static char* rsa_key_from_der(const uint8_t* der, size_t len, int depth) {
    int tag;
    const uint8_t* c;
    size_t cl;
    if (depth > 2 || rsa_der_tlv(der, der + len, &tag, &c, &cl) || tag != 0x30) return rt_out_cstr("");
    RsaSpan ints[9];
    int ni = rsa_der_ints(c, cl, ints, 9);
    RsaSpan none = { 0, 0 };
    if (ni == 2) return rsa_key_string(ints[0], ints[1], none);           /* PKCS#1 RSAPublicKey */
    if (ni == 9) return rsa_key_string(ints[1], ints[2], ints[3]);        /* PKCS#1 RSAPrivateKey */
    /* SPKI { AlgorithmIdentifier, BIT STRING } or PKCS#8 { INTEGER 0, AlgorithmIdentifier, OCTET STRING } */
    const uint8_t* p = c;
    const uint8_t* end = c + cl;
    if (ni == 1) {
        int t2; const uint8_t* c2; size_t l2;
        if (rsa_der_tlv(p, end, &t2, &c2, &l2)) return rt_out_cstr("");
        p = c2 + l2; /* skip the version INTEGER */
    }
    int ta; const uint8_t* ca; size_t la;
    if (rsa_der_tlv(p, end, &ta, &ca, &la) || ta != 0x30) return rt_out_cstr("");
    int to; const uint8_t* co; size_t lo;
    if (rsa_der_tlv(ca, ca + la, &to, &co, &lo) || to != 0x06 || lo != sizeof RSA_OID || memcmp(co, RSA_OID, lo)) {
        return rt_out_cstr(""); /* not rsaEncryption */
    }
    p = ca + la;
    int tb; const uint8_t* cb; size_t lb;
    if (rsa_der_tlv(p, end, &tb, &cb, &lb)) return rt_out_cstr("");
    if (tb == 0x03 && lb > 1 && cb[0] == 0) return rsa_key_from_der(cb + 1, lb - 1, depth + 1); /* BIT STRING */
    if (tb == 0x04) return rsa_key_from_der(cb, lb, depth + 1);                                 /* OCTET STRING */
    return rt_out_cstr("");
}

/* ---- exported ---- */

/* Normalized key from a PEM block (any of the four RSA forms); "" when it is not one */
char* salivo_rsa_key_pem_str(const char* pem) {
    if (!pem) return rt_out_cstr("");
    const char* b = strstr(pem, "-----BEGIN ");
    if (!b) return rt_out_cstr("");
    b = strstr(b + 11, "-----");
    if (!b) return rt_out_cstr("");
    b += 5;
    const char* e = strstr(b, "-----END ");
    if (!e) return rt_out_cstr("");
    size_t cap = (size_t)(e - b);
    uint8_t* der = (uint8_t*)malloc(cap + 1);
    if (!der) return rt_out_cstr("");
    long n = rsa_b64_decode(b, (size_t)(e - b), 0, der, cap);
    char* r = n > 0 ? rsa_key_from_der(der, (size_t)n, 0) : rt_out_cstr("");
    rsa_wipe(der, cap);
    free(der);
    return r;
}

/* Normalized public key from a JWK's base64url n and e; "" when they do not decode */
char* salivo_rsa_key_jwk_str(const char* n64, const char* e64) {
    if (!n64 || !e64) return rt_out_cstr("");
    uint8_t nb[RSA_MAX_LIMBS * 4 + 1], eb[16];
    long nl = rsa_b64_decode(n64, strlen(n64), 1, nb, sizeof nb);
    long el = rsa_b64_decode(e64, strlen(e64), 1, eb, sizeof eb);
    if (nl <= 0 || el <= 0) return rt_out_cstr("");
    RsaSpan n = { nb, (size_t)nl }, e = { eb, (size_t)el }, d = { 0, 0 };
    return rsa_key_string(n, e, d);
}

/* Parses "rsa1:n:e:d" into big numbers sized to the modulus; returns the modulus byte length or -1 */
static int rsa_parse_key(const char* key, RsaBn* n, RsaBn* e, RsaBn* d, int* has_d) {
    if (!key || strncmp(key, "rsa1:", 5)) return -1;
    const char* parts[3];
    size_t lens[3];
    const char* p = key + 5;
    for (int i = 0; i < 3; i++) {
        const char* q = i < 2 ? strchr(p, ':') : p + strlen(p);
        if (!q) return -1;
        parts[i] = p;
        lens[i] = (size_t)(q - p);
        p = q + 1;
    }
    if (lens[0] == 0 || lens[0] % 2 || lens[0] / 2 > RSA_MAX_LIMBS * 4 || lens[1] == 0 || lens[1] % 2 || lens[2] % 2) return -1;
    uint8_t buf[RSA_MAX_LIMBS * 4];
    RsaBn* out[3] = { n, e, d };
    int nbytes = (int)(lens[0] / 2);
    int k = (nbytes + 3) / 4;
    for (int i = 0; i < 3; i++) {
        size_t bl = lens[i] / 2;
        if (bl > sizeof buf) return -1;
        for (size_t j = 0; j < bl; j++) {
            int hi = rsa_hexval(parts[i][2 * j]), lo = rsa_hexval(parts[i][2 * j + 1]);
            if (hi < 0 || lo < 0) return -1;
            buf[j] = (uint8_t)(hi << 4 | lo);
        }
        if (rsa_bn_from_bytes(out[i], buf, bl, k)) return -1;
    }
    rsa_wipe(buf, sizeof buf);
    *has_d = lens[2] > 0;
    return nbytes;
}

/* EMSA-PKCS1-v1_5 encoding of SHA-256(msg) into em (len bytes); -1 when the modulus is too small */
static int rsa_emsa_sha256(const char* msg, uint8_t* em, int len) {
    static const uint8_t prefix[19] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65,
                                        0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
    if (len < 19 + 32 + 11) return -1;
    uint8_t h[32];
    salivo_crypto_sha256_buf((long long)(uintptr_t)msg, (long long)strlen(msg), (long long)(uintptr_t)h);
    int ps = len - 3 - 19 - 32;
    em[0] = 0;
    em[1] = 1;
    memset(em + 2, 0xFF, (size_t)ps);
    em[2 + ps] = 0;
    memcpy(em + 3 + ps, prefix, 19);
    memcpy(em + 3 + ps + 19, h, 32);
    return 0;
}

/* Bits of a public exponent rounded up to the window size (public: its length may show) */
static int rsa_pub_bits(const RsaBn* e) {
    int top = 0;
    for (int i = e->n * 32 - 1; i >= 0; i--) {
        if ((e->w[i / 32] >> (i % 32)) & 1) { top = i + 1; break; }
    }
    return top < 4 ? 4 : (top + 3) / 4 * 4;
}

/* RS256 check of a base64url signature over msg: 1 valid, 0 invalid, -1 unusable key */
long long salivo_rs256_verify(const char* key, const char* msg, const char* sig64) {
    RsaBn n, e, d;
    int has_d;
    int len = rsa_parse_key(key, &n, &e, &d, &has_d);
    if (len < 0 || !msg || !sig64) return -1;
    RsaMont m;
    if (rsa_mont_init(&m, &n)) return -1;
    uint8_t sig[RSA_MAX_LIMBS * 4], em[RSA_MAX_LIMBS * 4], got[RSA_MAX_LIMBS * 4];
    long sl = rsa_b64_decode(sig64, strlen(sig64), 1, sig, sizeof sig);
    if (sl != len) return 0;
    RsaBn s;
    if (rsa_bn_from_bytes(&s, sig, (size_t)sl, m.k)) return 0;
    if (rsa_bn_ge(s.w, m.n.w, m.k)) return 0; /* s must be below n */
    if (rsa_emsa_sha256(msg, em, len)) return -1;
    uint32_t r[RSA_MAX_LIMBS];
    rsa_mod_exp(&m, r, s.w, &e, rsa_pub_bits(&e));
    RsaBn rb;
    memset(&rb, 0, sizeof rb);
    memcpy(rb.w, r, sizeof(uint32_t) * (size_t)m.k);
    rb.n = m.k;
    rsa_bn_to_bytes(&rb, got, (size_t)len);
    uint8_t diff = 0;
    for (int i = 0; i < len; i++) diff |= (uint8_t)(got[i] ^ em[i]);
    return diff == 0 ? 1 : 0;
}

/* RS256 signature of msg as base64url; "" when the key has no private exponent or is unusable */
char* salivo_rs256_sign_str(const char* key, const char* msg) {
    RsaBn n, e, d;
    int has_d;
    int len = rsa_parse_key(key, &n, &e, &d, &has_d);
    if (len < 0 || !has_d || !msg) return rt_out_cstr("");
    RsaMont m;
    if (rsa_mont_init(&m, &n)) return rt_out_cstr("");
    uint8_t em[RSA_MAX_LIMBS * 4], out[RSA_MAX_LIMBS * 4];
    if (rsa_emsa_sha256(msg, em, len)) return rt_out_cstr("");
    RsaBn x;
    if (rsa_bn_from_bytes(&x, em, (size_t)len, m.k)) return rt_out_cstr("");
    uint32_t r[RSA_MAX_LIMBS];
    rsa_mod_exp(&m, r, x.w, &d, 32 * m.k); /* full-length exponent: time independent of d */
    RsaBn rb;
    memset(&rb, 0, sizeof rb);
    memcpy(rb.w, r, sizeof(uint32_t) * (size_t)m.k);
    rb.n = m.k;
    rsa_bn_to_bytes(&rb, out, (size_t)len);
    /* fault check: the signature must verify under e before it leaves */
    uint32_t chk[RSA_MAX_LIMBS];
    rsa_mod_exp(&m, chk, r, &e, rsa_pub_bits(&e));
    RsaBn cb;
    memset(&cb, 0, sizeof cb);
    memcpy(cb.w, chk, sizeof(uint32_t) * (size_t)m.k);
    cb.n = m.k;
    uint8_t back[RSA_MAX_LIMBS * 4];
    rsa_bn_to_bytes(&cb, back, (size_t)len);
    int ok = memcmp(back, em, (size_t)len) == 0;
    rsa_wipe(&d, sizeof d);
    rsa_wipe(&m, sizeof m);
    if (!ok) return rt_out_cstr("");
    return rsa_b64url_encode(out, (size_t)len);
}
