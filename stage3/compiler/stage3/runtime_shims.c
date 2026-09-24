/* std `rt*` intrinsics that the host backend lowers inline; Stage 3 links these instead. */
#include <stdlib.h>
#include <string.h>

long long rtcharat(const char* s, long long i) {
    return s ? (long long)(unsigned char)s[i] : 0;
}

void rtexit(long long code) {
    exit((int)code);
}

long long rtfloattoint(double f) {
    return (long long)f;
}

long long rtstrtoptr(const char* s) {
    return (long long)s;
}

/* Slot and `global let` accessors (rtmemreadval, rtglobalget, ...) are defined in the IR preamble. */

/* ---- HashMap<K, V> lookup index ----
   Map object: { len, cap, entries, index, index_cap }; entries = cap x { key, value }.
   Lookups build an open-addressing index (slot = entry + 1, load <= 1/2) once a map holds 8+ keys.
   The low bit of index_cap records the key kind (0 = string contents, 1 = 64-bit identity), so a map
   probed both ways just rebuilds. salivo_map_index_add extends the index after an insert; removals
   discard it and the next lookup rebuilds it. Slots are verified against the entry key. */
typedef struct { long long len, cap, entries, index, index_cap; } SalivoMapObj;

static unsigned long long shim_str_hash(const char* s) {
    unsigned long long h = 1469598103934665603ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

static unsigned long long shim_int_hash(unsigned long long x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return x;
}

static long long shim_map_raw_key(SalivoMapObj* m, long long i) { return ((long long*)m->entries)[2 * i]; }

static const char* shim_map_key(SalivoMapObj* m, long long i) {
    const char* k = (const char*)shim_map_raw_key(m, i);
    return k ? k : "";
}

static unsigned long long shim_entry_hash(SalivoMapObj* m, long long i, int kind) {
    return kind ? shim_int_hash((unsigned long long)shim_map_raw_key(m, i)) : shim_str_hash(shim_map_key(m, i));
}

static void shim_index_insert(SalivoMapObj* m, long long i) {
    long long* ix = (long long*)m->index;
    int kind = (int)(m->index_cap & 1);
    unsigned long long mask = (unsigned long long)(m->index_cap & ~1LL) - 1;
    unsigned long long h = shim_entry_hash(m, i, kind) & mask;
    while (ix[h]) h = (h + 1) & mask;
    ix[h] = i + 1;
}

void salivo_map_index_free(SalivoMapObj* m) {
    if (m && m->index) {
        free((void*)m->index);
        m->index = 0;
        m->index_cap = 0;
    }
}

void salivo_map_index_add(long long handle, long long i) {
    SalivoMapObj* m = (SalivoMapObj*)handle;
    if (!m || !m->index) return;
    if (m->len * 2 > (m->index_cap & ~1LL)) { salivo_map_index_free(m); return; }
    shim_index_insert(m, i);
}

/* Returns 1 when m has an index of the given kind (building it if needed), 0 to fall back to a scan. */
static int shim_index_ready(SalivoMapObj* m, int kind) {
    if (m->index && (int)(m->index_cap & 1) == kind) return 1;
    salivo_map_index_free(m);
    long long cap = 16;
    while (cap < m->len * 2 + 2) cap <<= 1;
    m->index = (long long)calloc((size_t)cap, sizeof(long long));
    if (!m->index) return 0;
    m->index_cap = cap | kind;
    for (long long i = 0; i < m->len; i++) shim_index_insert(m, i);
    return 1;
}

long long salivo_map_find_str(long long handle, long long key) {
    SalivoMapObj* m = (SalivoMapObj*)handle;
    if (!m) return -1;
    const char* k = key ? (const char*)key : "";
    if (m->len < 8 || !shim_index_ready(m, 0)) {
        for (long long i = 0; i < m->len; i++) if (strcmp(shim_map_key(m, i), k) == 0) return i;
        return -1;
    }
    long long* ix = (long long*)m->index;
    unsigned long long mask = (unsigned long long)(m->index_cap & ~1LL) - 1;
    for (unsigned long long h = shim_str_hash(k) & mask; ix[h]; h = (h + 1) & mask) {
        long long i = ix[h] - 1;
        if (i < m->len && strcmp(shim_map_key(m, i), k) == 0) return i;
    }
    return -1;
}

long long salivo_map_find_int(long long handle, long long key) {
    SalivoMapObj* m = (SalivoMapObj*)handle;
    if (!m) return -1;
    if (m->len < 8 || !shim_index_ready(m, 1)) {
        for (long long i = 0; i < m->len; i++) if (shim_map_raw_key(m, i) == key) return i;
        return -1;
    }
    long long* ix = (long long*)m->index;
    unsigned long long mask = (unsigned long long)(m->index_cap & ~1LL) - 1;
    for (unsigned long long h = shim_int_hash((unsigned long long)key) & mask; ix[h]; h = (h + 1) & mask) {
        long long i = ix[h] - 1;
        if (i < m->len && shim_map_raw_key(m, i) == key) return i;
    }
    return -1;
}

/* ---- Scratch string cache: memo tables for the compiler, emptied explicitly by the caller ---- */
typedef struct { char* key; char* value; } ShimKV;
static ShimKV* shim_kv;
static long long shim_kv_cap, shim_kv_len;

long long salivo_scratch_clear(void) {
    for (long long i = 0; i < shim_kv_cap; i++) {
        free(shim_kv[i].key);
        free(shim_kv[i].value);
    }
    free(shim_kv);
    shim_kv = NULL;
    shim_kv_cap = shim_kv_len = 0;
    return 0;
}

static ShimKV* shim_kv_slot(ShimKV* table, long long cap, const char* key) {
    unsigned long long mask = (unsigned long long)cap - 1;
    unsigned long long h = shim_str_hash(key) & mask;
    while (table[h].key && strcmp(table[h].key, key) != 0) h = (h + 1) & mask;
    return &table[h];
}

long long salivo_scratch_get(const char* key) {
    if (!shim_kv || !key) return 0;
    ShimKV* s = shim_kv_slot(shim_kv, shim_kv_cap, key);
    return s->key ? (long long)s->value : 0;
}

long long salivo_scratch_put(const char* key, const char* value) {
    if (!key || !value) return 0;
    if ((shim_kv_len + 1) * 2 > shim_kv_cap) {
        long long cap = shim_kv_cap ? shim_kv_cap * 2 : 256;
        ShimKV* table = (ShimKV*)calloc((size_t)cap, sizeof(ShimKV));
        if (!table) return 0;
        for (long long i = 0; i < shim_kv_cap; i++) {
            if (shim_kv[i].key) *shim_kv_slot(table, cap, shim_kv[i].key) = shim_kv[i];
        }
        free(shim_kv);
        shim_kv = table;
        shim_kv_cap = cap;
    }
    ShimKV* s = shim_kv_slot(shim_kv, shim_kv_cap, key);
    if (s->key) {
        free(s->value);
    } else {
        s->key = strdup(key);
        shim_kv_len++;
    }
    s->value = strdup(value);
    return 0;
}
