/* ============================================================================================
 * Salivo Stage 36.2 - HTTP/1.1 and HTTP/2 transport on the Stage 36.1 runtime.
 * Included by salivo_aio_runtime.c after the platform I/O backend. Design and guarantees:
 * docs/STAGE_36_2_HTTP.md.
 *
 *  - Socket I/O goes only through salivo_aio_read/write/close (the portable 36.1 transport):
 *    no platform code, no second scheduler, no second socket abstraction.
 *  - Received bytes land in reference-counted pooled segments (HSeg). Request lines, header
 *    fields and body chunks are views (segment + offset + length) holding a segment reference,
 *    so a view never outlives its bytes. Every copy is counted (salivo_aio_http_stat).
 *  - HTTP/1.1: incremental byte-level parser; one exchange at a time per connection, pipelined
 *    requests are answered in order.
 *  - HTTP/2: one reader task per connection parses frames and routes them to streams under the
 *    connection lock (never held across I/O or user code). Application tasks write frames while
 *    holding the connection's write token. Control frames produced by the reader go through a
 *    bounded queue that the token holder flushes.
 *
 * Ownership: the connection owns the socket, the receive segment, the parser, the HTTP/2 stream
 * table (one reference per open stream) and the accept queue (one reference per queued stream).
 * An exchange refers to its connection only by handle, so there is no ownership cycle.
 * ========================================================================================== */

/* HTTP error codes (the ABI's @errors line lists them next to the runtime's). */
enum {
    HE_BADMSG = -18,   /* malformed message syntax */
    HE_HEADER = -19,   /* invalid header field */
    HE_FRAMING = -20,  /* invalid or ambiguous message framing (Content-Length / Transfer-Encoding) */
    HE_HDRSIZE = -21,  /* request line, header field or header section too large */
    HE_BODYSIZE = -22, /* body larger than the configured limit */
    HE_VERSION = -23,  /* unsupported HTTP version */
    HE_PROTOCOL = -24, /* HTTP/2 protocol violation */
    HE_RESET = -25,    /* stream reset by the peer */
    HE_FLOW = -26,     /* HTTP/2 flow-control violation */
    HE_COMPRESS = -27, /* HPACK decoding failure */
    HE_REFUSED = -28,  /* stream refused or connection limit reached */
    HE_GOAWAY = -29    /* the connection is going away */
};

/* ---- statistics (process-wide, relaxed atomics) ------------------------------------------ */
enum {
    HS_SEG_ALLOC, HS_POOL_HIT, HS_POOL_MISS, HS_SEG_LIVE, HS_PARSE_COPY, HS_HDR_COPY, HS_BODY_COPY,
    HS_SER_COPY, HS_BODY_VIEW, HS_WRITE_DIRECT, HS_CONNS, HS_EXCHANGES, HS_STREAMS, HS_RESETS,
    HS_REQUESTS, HS_SEG_CACHED, HS_TIMEOUTS, HS_CTL_PEAK, HS_RELOCATIONS, HS_FRAMES, HS_COUNT
};
static int64_t g_hst[HS_COUNT];
#define HST(k, n) __atomic_add_fetch(&g_hst[k], (int64_t)(n), __ATOMIC_RELAXED)
/* Instrumentation mode (SALIVO_HTTP_STATS=1 or rtaiohttpset(0, 101, 1)): per-byte copy/view and
 * per-frame counters touch a shared cache line on every operation, so they are off by default.
 * Object, segment and request counters are always kept (limits and leak checks use them). */
static int32_t g_hstats_on;
#define HSTX(k, n) do { if (__atomic_load_n(&g_hstats_on, __ATOMIC_RELAXED)) HST(k, n); } while (0)

/* ---- limits ------------------------------------------------------------------------------ */
enum {
    HL_REQLINE, HL_HDRCOUNT, HL_HDRBYTES, HL_FIELD, HL_BODY, HL_CHUNK, HL_FRAME, HL_HLIST, HL_STREAMS,
    HL_STREAMWIN, HL_CONNWIN, HL_T_HEADER, HL_T_IDLE, HL_T_READ, HL_T_WRITE, HL_T_REQUEST, HL_T_STREAM,
    HL_CTLQ, HL_DRAIN, HL_RESETS, HL_COUNT
};
static int64_t g_hlim[HL_COUNT] = {8192, 100, 32768, 8192, 8 << 20, 8 << 20, 16384, 32768, 100, 65535,
                                   1 << 20, 10000, 60000, 30000, 30000, 60000, 60000, 16384, 65536, 1000};
static int64_t g_hmax_conns = 10000;

/* ---- character classes ------------------------------------------------------------------- */
enum { HC_TCHAR = 1, HC_FVCHAR = 2 };
static unsigned char g_hct[256];
static int32_t g_hct_ready;
static void h_ctype_init(void) {
    if (A_LOAD(&g_hct_ready)) return;
    for (int c = 0; c < 256; c++) {
        int t = (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') ||
                (c && strchr("!#$%&'*+-.^_`|~", c));
        g_hct[c] = (unsigned char)((t ? HC_TCHAR : 0) | ((c >= 0x21 && c != 0x7f) ? HC_FVCHAR : 0));
    }
    const char* st = getenv("SALIVO_HTTP_STATS");
    if (st && *st == '1') A_STORE(&g_hstats_on, 1);
    A_STORE(&g_hct_ready, 1);
}
#define H_TCHAR(c) (g_hct[(unsigned char)(c)] & HC_TCHAR)
#define H_FVCHAR(c) (g_hct[(unsigned char)(c)] & HC_FVCHAR)
static unsigned char h_lc(unsigned char c) { return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c; }

/* Case-insensitive comparison against a lowercase literal, without allocating. */
static int h_ieq(const unsigned char* a, uint32_t n, const char* lower) {
    size_t m = strlen(lower);
    if (n != m) return 0;
    for (uint32_t i = 0; i < n; i++)
        if (h_lc(a[i]) != (unsigned char)lower[i]) return 0;
    return 1;
}

/* Parses 1*DIGIT (no sign, no whitespace, at most 18 digits). */
static int h_digits(const unsigned char* p, uint32_t n, int64_t* out) {
    if (n == 0 || n > 18) return 0;
    int64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return 0;
        v = v * 10 + (p[i] - '0');
    }
    *out = v;
    return 1;
}

static char* h_strdup(const unsigned char* p, uint32_t n) {
    char* s = (char*)malloc((size_t)n + 1);
    if (!s) return NULL;
    if (n) memcpy(s, p, n);
    s[n] = 0;
    HSTX(HS_HDR_COPY, n);
    return s;
}

/* ============================================================================================
 * Receive segments: reference counted, pooled in three size classes with explicit caps.
 * ========================================================================================== */
typedef struct HSeg {
    int64_t refs; /* atomic */
    int32_t cls;
    uint32_t cap;
    struct HSeg* next;
    unsigned char data[];
} HSeg;

static const uint32_t g_hcls_size[3] = {4096, 16384, 65536};
static const int32_t g_hcls_max[3] = {64, 128, 16}; /* at most ~3.3 MiB cached process-wide */
static HSeg* g_hfree[3];
static int32_t g_hfree_n[3];
static sa_mutex g_hpool_lock = SA_MUTEX_INIT;

static HSeg* hseg_get(size_t need) {
    int cls = 3;
    for (int i = 0; i < 3; i++)
        if (need <= g_hcls_size[i]) {
            cls = i;
            break;
        }
    HSeg* s = NULL;
    if (cls < 3) {
        sa_mutex_lock(&g_hpool_lock);
        if ((s = g_hfree[cls]) != NULL) {
            g_hfree[cls] = s->next;
            g_hfree_n[cls]--;
        }
        sa_mutex_unlock(&g_hpool_lock);
    }
    if (s) {
        HST(HS_POOL_HIT, 1);
        HST(HS_SEG_CACHED, -1);
    } else {
        size_t cap = cls < 3 ? g_hcls_size[cls] : need;
        s = (HSeg*)malloc(sizeof(HSeg) + cap);
        if (!s) return NULL;
        s->cls = cls;
        s->cap = (uint32_t)cap;
        HST(HS_POOL_MISS, 1);
        HST(HS_SEG_ALLOC, 1);
    }
    s->refs = 1;
    s->next = NULL;
    HST(HS_SEG_LIVE, 1);
    return s;
}

static void hseg_ref(HSeg* s) { A_ADD(&s->refs, 1); }

static void hseg_unref(HSeg* s) {
    if (!s || A_SUB(&s->refs, 1) != 0) return;
    HST(HS_SEG_LIVE, -1);
    if (s->cls < 3) {
        sa_mutex_lock(&g_hpool_lock);
        if (g_hfree_n[s->cls] < g_hcls_max[s->cls]) {
            s->next = g_hfree[s->cls];
            g_hfree[s->cls] = s;
            g_hfree_n[s->cls]++;
            s = NULL;
            HST(HS_SEG_CACHED, 1);
        }
        sa_mutex_unlock(&g_hpool_lock);
    }
    free(s);
}

/* A view: bytes inside a segment (seg holds a reference) or inside an owned node (own). */
typedef struct {
    HSeg* seg;
    void* own;
    unsigned char* p;
    uint32_t len;
} HView;

static void h_view_drop(HView* v) {
    hseg_unref(v->seg);
    free(v->own);
    memset(v, 0, sizeof(*v));
}

/* Inbound HTTP/2 DATA: views for large pieces, small pieces coalesced into owned 4 KiB nodes so a
 * flood of tiny frames can neither pin segments nor allocate one node per frame. */
#define H_SMALL 4096
typedef struct HChunk {
    struct HChunk* next;
    HSeg* seg; /* NULL: owned bytes follow the node */
    unsigned char* p;
    uint32_t len, cap;
} HChunk;

/* ============================================================================================
 * Exchanges and connections
 * ========================================================================================== */
typedef struct {
    uint32_t noff, nlen, voff, vlen;
} HField;

enum { PM_METHOD, PM_TARGET, PM_AUTH, PM_SCHEME, PM_REASON, PM_VERSION, PM_STATUS, PM_COUNT };
enum { BM_NONE, BM_FIXED, BM_CHUNKED, BM_CLOSE };
enum { CS_SIZE0, CS_SIZE, CS_BWS, CS_EXT, CS_SIZE_LF, CS_DATA, CS_DATA_CR, CS_DATA_LF, CS_TSTART, CS_TNAME,
       CS_TOWS, CS_TVAL, CS_TLF, CS_END_LF };
#define H_FIN 16

typedef struct HEx {
    SaObj obj;
    int64_t conn_h;
    int32_t proto, client;
    uint32_t sid;
    /* inbound message: offsets are relative to hbase (receive segment or arena) */
    unsigned char* hbase;
    HSeg* hseg;
    unsigned char* arena;
    HField fin[H_FIN];
    HField* f;
    int32_t nf, fcap;
    HField pm[PM_COUNT]; /* nlen = 1 marks a present pseudo field; voff/vlen locate its value */
    int32_t version, status, tform, keepalive, expect100, is_head_req;
    int64_t clen;
    unsigned char* tarena;
    uint32_t tn, tcap, tnoff, tnlen, tvoff, tvend;
    HField* tf;
    int32_t ntf;
    int32_t head_ready, in_done;
    int64_t in_err;
    int32_t bmode, cst;
    int64_t bleft, btotal, req_deadline_ns;
    uint64_t csize;
    uint32_t cext;
    /* HTTP/2 inbound */
    HChunk *dq_h, *dq_t;
    int64_t unacked, recv_win, data_total;
    HView cur;
    /* outbound */
    int32_t out_state; /* 0 nothing sent, 1 head sent, 2 finished */
    int32_t out_chunked, out_nobody, out_close, pending_crlf;
    int64_t out_len, out_sent, send_win;
    unsigned char* oh; /* "name\0value\0" pairs */
    uint32_t ohn, ohcap;
    char *mout, *tout; /* client: request method and target */
    unsigned char* ph; /* response head held back to share a system call with the first body bytes */
    uint32_t phn;
    /* HTTP/2 stream state */
    int32_t in_table, queued, reset, reset_code, local_end, remote_end;
    SaWaiter* w; /* the owning task, waiting for head/data/window */
    struct HEx* aq_next;
} HEx;

typedef struct {
    int32_t st, lines;
    uint32_t pos, mark, noff, nlen, voff, vend;
    int32_t err_status;
} H1P;

typedef struct {
    unsigned char* p;
    uint32_t nlen, vlen;
} HDynE;

typedef struct {
    HDynE e[4096 / 32 + 1];
    uint32_t head, n, size, max;
} HDyn;

typedef struct H2 {
    int32_t preface_left, got_settings, settings_acked, goaway_sent, goaway_recv, ping_out;
    unsigned char fh[9];
    int32_t fhn;
    uint32_t flen, fsid, fgot, pad, fdata;
    uint8_t ftype, fflags;
    HEx* fex; /* DATA target (holds a reference) */
    unsigned char* fbuf;
    unsigned char* hb;
    uint32_t hbn, hbcap, hb_sid;
    int32_t hb_on, hb_end;
    int64_t peer_frame, peer_initwin, peer_streams;
    int64_t conn_send, conn_recv, conn_credit;
    uint32_t last_peer_sid, next_sid, goaway_last;
    HEx** tab;
    int32_t ntab, tabcap;
    HEx *aq_h, *aq_t, *hot; /* hot: last stream found (frames arrive in runs per stream) */
    int32_t naq;
    SaWaiter *accept_w, *ping_w, *sw_h, *sw_t;
    HDyn dyn;
    unsigned char *dout, *dstr; /* HPACK decode scratch */
    HField* dfields;
    int64_t ping_ns, ping_rtt, peer_resets, empty_frames;
    uint64_t ping_seq;
} H2;

typedef struct HConn {
    SaObj obj;
    int64_t sock, self;
    int32_t proto, client, dead, busy, timed_out, h1_close, wbusy, h2_starting, h2_ready, reader_closes;
    int64_t dead_err;
    sa_mutex lock;
    int64_t lim[HL_COUNT];
    HSeg* rseg;
    uint32_t rpos, rfill, hstart;
    H1P p;
    HEx* cur; /* HTTP/1.1: exchange in progress (one reference) */
    int64_t served;
    H2* h2;
    SaWaiter *ww_h, *ww_t;
    unsigned char *cq, *cq2, *wb, *wb2;
    uint32_t cqn, wbcap, wb2cap;
    char* authority; /* client: default Host / :authority */
} HConn;

static void h_chunks_free(HEx* e) {
    HChunk* c = e->dq_h;
    while (c) {
        HChunk* n = c->next;
        hseg_unref(c->seg);
        free(c);
        c = n;
    }
    e->dq_h = e->dq_t = NULL;
}

static void h_ex_destroy(SaObj* o) {
    HEx* e = (HEx*)o;
    hseg_unref(e->hseg);
    free(e->arena);
    if (e->f != e->fin) free(e->f);
    free(e->tarena);
    free(e->tf);
    h_chunks_free(e);
    h_view_drop(&e->cur);
    free(e->oh);
    free(e->mout);
    free(e->tout);
    free(e->ph);
    HST(HS_EXCHANGES, -1);
    free(e);
}

static HEx* h_ex_new(HConn* c, int refs) {
    HEx* e = (HEx*)calloc(1, sizeof(HEx));
    if (!e) return NULL;
    sa_obj_init(c->obj.rt, &e->obj, OT_HTTP_EX, refs, h_ex_destroy);
    HST(HS_EXCHANGES, 1);
    e->conn_h = c->self;
    e->proto = c->proto;
    e->client = c->client;
    e->f = e->fin;
    e->fcap = H_FIN;
    e->clen = -1;
    e->reset_code = -1;
    return e;
}

static int h_ex_addfield(HEx* e, HField f) {
    if (e->nf == e->fcap) {
        int32_t nc = e->fcap * 2;
        HField* nf = (HField*)malloc(sizeof(HField) * (size_t)nc);
        if (!nf) return -1;
        memcpy(nf, e->f, sizeof(HField) * (size_t)e->nf);
        if (e->f != e->fin) free(e->f);
        e->f = nf;
        e->fcap = nc;
    }
    e->f[e->nf++] = f;
    return 0;
}

static void h_pm(HEx* e, int k, uint32_t a, uint32_t b) {
    e->pm[k].nlen = 1;
    e->pm[k].voff = a;
    e->pm[k].vlen = b - a;
}

static HField* h_find(HEx* e, const unsigned char* name, uint32_t n, int32_t idx) {
    for (int32_t i = 0; i < e->nf; i++) {
        HField* f = &e->f[i];
        if (f->nlen != n) continue;
        const unsigned char* a = e->hbase + f->noff;
        uint32_t k = 0;
        while (k < n && h_lc(a[k]) == h_lc(name[k])) k++;
        if (k == n && idx-- == 0) return f;
    }
    return NULL;
}

static void h_conn_destroy(SaObj* o) {
    HConn* c = (HConn*)o;
    if (c->sock) salivo_aio_close(c->sock);
    hseg_unref(c->rseg);
    if (c->cur) sa_obj_unref(&c->cur->obj);
    H2* h = c->h2;
    if (h) {
        for (int32_t i = 0; i < h->ntab; i++) {
            h->tab[i]->in_table = 0;
            sa_obj_unref(&h->tab[i]->obj);
        }
        while (h->aq_h) {
            HEx* e = h->aq_h;
            h->aq_h = e->aq_next;
            sa_obj_unref(&e->obj);
        }
        if (h->fex) sa_obj_unref(&h->fex->obj);
        for (uint32_t i = 0; i < h->dyn.n; i++) free(h->dyn.e[(h->dyn.head + i) % (4096 / 32 + 1)].p);
        free(h->tab);
        free(h->fbuf);
        free(h->hb);
        free(h->dout);
        free(h->dstr);
        free(h->dfields);
        free(h);
    }
    free(c->cq);
    free(c->cq2);
    free(c->wb);
    free(c->wb2);
    free(c->authority);
    sa_mutex_destroy(&c->lock);
    HST(HS_CONNS, -1);
    free(c);
}

static unsigned char* h_wb(HConn* c, size_t need) {
    if (need > c->wbcap) {
        size_t nc = need < 4096 ? 4096 : need;
        unsigned char* p = (unsigned char*)realloc(c->wb, nc);
        if (!p) return NULL;
        c->wb = p;
        c->wbcap = (uint32_t)nc;
    }
    return c->wb;
}

/* ---- deadlines: a runtime timer whose expiry closes the socket (aborting the pending I/O) ---- */
typedef struct {
    SaTimer tm; /* first member */
    HConn* c;
} HDeadline;

static void h_deadline_fire(SaTimer* tm) {
    HConn* c = ((HDeadline*)tm)->c;
    A_STORE(&c->timed_out, 1);
    HST(HS_TIMEOUTS, 1);
    salivo_aio_close(c->sock);
}

/* One socket read or write, bounded by `ms` (0 = no deadline). */
static int64_t h_io(HConn* c, int wr, unsigned char* p, int64_t n, int64_t ms) {
    HDeadline d;
    int armed = 0;
    if (A_LOAD(&c->timed_out)) return SA_ETIMEOUT;
    if (ms > 0) {
        d.c = c;
        armed = sa_timer_arm_cb(c->obj.rt, &d.tm, ms, h_deadline_fire);
    }
    int64_t r = wr ? salivo_aio_write(c->sock, (int64_t)(intptr_t)p, n)
                   : salivo_aio_read(c->sock, (int64_t)(intptr_t)p, n);
    if (armed) sa_timer_disarm(c->obj.rt, &d.tm);
    if (r < 0 && A_LOAD(&c->timed_out)) r = SA_ETIMEOUT;
    return r;
}

/* Two buffers in one system call (gathered write in the 36.1 transport), same deadline rule. */
static int64_t h_io2(HConn* c, const unsigned char* p1, int64_t n1, const unsigned char* p2, int64_t n2, int64_t ms) {
    HDeadline d;
    int armed = 0;
    if (A_LOAD(&c->timed_out)) return SA_ETIMEOUT;
    if (ms > 0) {
        d.c = c;
        armed = sa_timer_arm_cb(c->obj.rt, &d.tm, ms, h_deadline_fire);
    }
    int64_t r = sa_aio_write2(c->sock, p1, n1, p2, n2);
    if (armed) sa_timer_disarm(c->obj.rt, &d.tm);
    if (r < 0 && A_LOAD(&c->timed_out)) r = SA_ETIMEOUT;
    return r;
}

/* Guarantees `want` free bytes after rfill, keeping [keep, rfill) contiguous. Only a partial
 * message head is ever kept; relocating it is the one parser copy (counted). */
static int64_t hc_space(HConn* c, uint32_t keep, uint32_t want) {
    HSeg* s = c->rseg;
    if (s && s->cap - c->rfill >= want) return SA_OK;
    if (s && keep == c->rfill && A_LOAD(&s->refs) == 1) { /* nothing retained: reuse in place */
        c->rpos = c->rfill = c->hstart = 0;
        if (s->cap >= want) return SA_OK;
    }
    uint32_t carry = s ? c->rfill - keep : 0;
    size_t need = (size_t)carry + want;
    HSeg* n = hseg_get(need < 16384 ? 16384 : need + 4096);
    if (!n) return SA_EOS;
    if (carry) {
        memcpy(n->data, s->data + keep, carry);
        HSTX(HS_PARSE_COPY, carry);
        HSTX(HS_RELOCATIONS, 1);
    }
    c->rpos = c->rpos >= keep ? c->rpos - keep : 0;
    c->hstart = c->hstart >= keep ? c->hstart - keep : 0;
    c->rfill = carry;
    hseg_unref(s);
    c->rseg = n;
    return SA_OK;
}

static int64_t h_fill(HConn* c, uint32_t keep, int64_t ms) {
    int64_t r = hc_space(c, keep, 2048);
    if (r) return r;
    HSeg* s = c->rseg;
    r = h_io(c, 0, s->data + c->rfill, s->cap - c->rfill, ms);
    if (r > 0) c->rfill += (uint32_t)r;
    return r;
}

/* ============================================================================================
 * HTTP/2 wiring used by both engines (defined further down)
 * ========================================================================================== */
static void h2_wake_all(HConn* c);
static void h2_kick(HConn* c);
static int h2_ctl(HConn* c, uint8_t type, uint8_t flags, uint32_t sid, const unsigned char* p, uint32_t n);

static void h_die_ex(HConn* c, int64_t err, int h2code, int linger);
static void h_die(HConn* c, int64_t err, int h2code) { h_die_ex(c, err, h2code, 0); }

/* Lingering close: after a final error response / GOAWAY, half-close and discard what the peer
 * still sends (bounded: 1 s idle, 1 MiB), so closing with unread input does not reset the
 * connection and destroy the answer in flight. */
static void h_linger(HConn* c) {
    salivo_aio_tcp_shutdown(c->sock);
    int64_t total = 0, r;
    while (total < (1 << 20) && (r = h_fill(c, c->rfill, 1000)) > 0) {
        total += r;
        c->rpos = c->rfill;
    }
}

/* Marks the connection dead, wakes every waiter and closes the socket. h2code >= 0 queues a
 * GOAWAY with that code first. */
static void h_die_ex(HConn* c, int64_t err, int h2code, int linger) {
    sa_mutex_lock(&c->lock);
    if (!c->dead) {
        c->dead = 1;
        c->dead_err = err;
        if (h2code >= 0 && c->h2 && !c->h2->goaway_sent) {
            unsigned char g[8];
            uint32_t last = c->h2->last_peer_sid;
            g[0] = (unsigned char)(last >> 24); g[1] = (unsigned char)(last >> 16);
            g[2] = (unsigned char)(last >> 8); g[3] = (unsigned char)last;
            g[4] = g[5] = g[6] = 0; g[7] = (unsigned char)h2code;
            h2_ctl(c, 7, 0, 0, g, 8);
            c->h2->goaway_sent = 1;
        }
    }
    if (c->h2) h2_wake_all(c);
    sa_mutex_unlock(&c->lock);
    if (c->h2) h2_kick(c);
    if (linger && c->h2) /* another task may be flushing: half-close only after the GOAWAY left */
        for (;;) {
            sa_mutex_lock(&c->lock);
            int busy = c->wbusy, pending = c->cqn != 0;
            sa_mutex_unlock(&c->lock);
            if (!busy && !pending) break;
            if (!busy) h2_kick(c);
            else salivo_aio_yield();
        }
    if (linger) h_linger(c);
    salivo_aio_close(c->sock);
}

/* ============================================================================================
 * HTTP/1.1
 * ========================================================================================== */
enum { P_START, P_START_LF, P_METHOD, P_TARGET, P_VERSION, P_RL_LF, P_STATUS, P_REASON, P_HSTART, P_HNAME,
       P_HOWS, P_HVAL, P_HLF, P_END_LF };

static int h_version_ok(const unsigned char* v, uint32_t n) {
    return n == 8 && memcmp(v, "HTTP/", 5) == 0 && v[5] >= '0' && v[5] <= '9' && v[6] == '.' && v[7] >= '0' &&
           v[7] <= '9';
}

/* Incremental head parser. Resumes at p->pos over bytes [hstart, rfill). Returns 1 when the head
 * is complete, 0 when more bytes are needed, or an error. Offsets are relative to hstart, so a
 * relocated partial head stays valid. */
static int64_t h1_parse(HConn* c, HEx* e) {
    H1P* p = &c->p;
    const unsigned char* b = c->rseg->data + c->hstart;
    uint32_t n = c->rfill - c->hstart;
    uint32_t maxrl = (uint32_t)c->lim[HL_REQLINE] + 8, maxhead = maxrl + (uint32_t)c->lim[HL_HDRBYTES];
    for (; p->pos < n; p->pos++) {
        uint32_t i = p->pos;
        unsigned char ch = b[i];
        if (p->st <= P_RL_LF ? i >= maxrl : i >= maxhead) {
            p->err_status = p->st <= P_RL_LF ? 414 : 431;
            return HE_HDRSIZE;
        }
        switch (p->st) {
        case P_START:
            if (ch == '\r') {
                if (++p->lines > 4) return HE_BADMSG;
                p->st = P_START_LF;
            } else if (!c->client && H_TCHAR(ch)) {
                p->mark = i;
                p->st = P_METHOD;
            } else if (c->client && ch == 'H') {
                p->mark = i;
                p->st = P_VERSION;
            } else return HE_BADMSG;
            break;
        case P_START_LF:
            if (ch != '\n') return HE_BADMSG;
            p->st = P_START;
            break;
        case P_METHOD:
            if (ch == ' ') {
                h_pm(e, PM_METHOD, p->mark, i);
                p->mark = i + 1;
                p->st = P_TARGET;
            } else if (!H_TCHAR(ch)) return HE_BADMSG;
            break;
        case P_TARGET:
            if (ch == ' ') {
                if (i == p->mark) return HE_BADMSG;
                h_pm(e, PM_TARGET, p->mark, i);
                p->mark = i + 1;
                p->st = P_VERSION;
            } else if (ch < 0x21 || ch > 0x7e) return HE_BADMSG;
            break;
        case P_VERSION:
            if (ch == (c->client ? ' ' : '\r')) {
                if (!h_version_ok(b + p->mark, i - p->mark)) return HE_BADMSG;
                h_pm(e, PM_VERSION, p->mark, i);
                p->mark = i + 1;
                p->st = c->client ? P_STATUS : P_RL_LF;
            } else if (i - p->mark >= 8) {
                return HE_BADMSG;
            }
            break;
        case P_STATUS:
            if (i - p->mark < 3) {
                if (ch < '0' || ch > '9') return HE_BADMSG;
            } else if (ch == ' ' || ch == '\r') {
                h_pm(e, PM_STATUS, p->mark, i);
                p->mark = i + 1;
                if (ch == '\r') h_pm(e, PM_REASON, i, i);
                p->st = ch == ' ' ? P_REASON : P_RL_LF;
            } else return HE_BADMSG;
            break;
        case P_REASON:
            if (ch == '\r') {
                h_pm(e, PM_REASON, p->mark, i);
                p->st = P_RL_LF;
            } else if (!(ch == '\t' || ch == ' ' || H_FVCHAR(ch))) return HE_BADMSG;
            break;
        case P_RL_LF:
            if (ch != '\n') return HE_BADMSG;
            p->st = P_HSTART;
            break;
        case P_HSTART:
            if (ch == '\r') p->st = P_END_LF;
            else if (H_TCHAR(ch)) {
                p->noff = i;
                p->st = P_HNAME;
            } else return HE_HEADER; /* obs-fold, whitespace or junk where a field name belongs */
            break;
        case P_HNAME:
            if (ch == ':') {
                p->nlen = i - p->noff;
                p->st = P_HOWS;
            } else if (!H_TCHAR(ch)) return HE_HEADER; /* includes whitespace before the colon */
            break;
        case P_HOWS:
            if (ch == ' ' || ch == '\t') break;
            p->voff = p->vend = i;
            if (ch == '\r') {
                p->st = P_HLF;
                break;
            }
            if (!H_FVCHAR(ch)) return HE_HEADER;
            p->vend = i + 1;
            p->st = P_HVAL;
            break;
        case P_HVAL:
            if (ch == '\r') p->st = P_HLF;
            else if (H_FVCHAR(ch)) p->vend = i + 1;
            else if (ch != ' ' && ch != '\t') return HE_HEADER; /* NUL, bare LF, other controls */
            break;
        case P_HLF: {
            if (ch != '\n') return HE_HEADER; /* bare CR */
            if (i - p->noff > (uint32_t)c->lim[HL_FIELD] || e->nf >= c->lim[HL_HDRCOUNT]) {
                p->err_status = 431;
                return HE_HDRSIZE;
            }
            HField f = {p->noff, p->nlen, p->voff, p->vend - p->voff};
            if (h_ex_addfield(e, f)) return SA_EOS;
            p->st = P_HSTART;
            break;
        }
        case P_END_LF:
            if (ch != '\n') return HE_BADMSG;
            p->pos++;
            return 1;
        }
    }
    return 0;
}

/* Walks a comma-separated token list (Connection) and reports close / keep-alive. */
static void h_conn_tokens(const unsigned char* v, uint32_t n, int* close, int* keep) {
    uint32_t i = 0;
    while (i < n) {
        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        uint32_t s = i;
        while (i < n && v[i] != ',' && v[i] != ' ' && v[i] != '\t') i++;
        if (h_ieq(v + s, i - s, "close")) *close = 1;
        if (h_ieq(v + s, i - s, "keep-alive")) *keep = 1;
    }
}

/* Framing and connection semantics after a complete head (request or response). One framing
 * interpretation only: anything ambiguous fails closed. */
/* RFC 9110 9.3.6: a CONNECT target is authority-form `host:port` with a non-empty host and a
   decimal port in 1..65535; an IPv6 host is bracketed ("[::1]:443"). */
static int h_valid_connect_authority(const unsigned char* t, uint32_t tl) {
    if (tl < 3) return 0;
    uint32_t colon;
    if (t[0] == '[') {
        const unsigned char* rb = (const unsigned char*)memchr(t, ']', tl);
        if (!rb || rb == t + 1) return 0;
        colon = (uint32_t)(rb - t) + 1;
        if (colon >= tl || t[colon] != ':') return 0;
    } else {
        colon = tl;
        while (colon > 0 && t[colon - 1] != ':') colon--;
        if (colon == 0) return 0;
        colon--;
        if (colon == 0 || memchr(t, ':', colon)) return 0; /* empty host, or a bare IPv6 literal */
        for (uint32_t k = 0; k < colon; k++)
            if (t[k] <= ' ' || t[k] == '/' || t[k] == '@' || t[k] == 0x7f) return 0;
    }
    uint32_t digits = tl - colon - 1;
    if (digits == 0 || digits > 5) return 0;
    uint32_t port = 0;
    for (uint32_t k = colon + 1; k < tl; k++) {
        if (t[k] < '0' || t[k] > '9') return 0;
        port = port * 10 + (uint32_t)(t[k] - '0');
    }
    return port >= 1 && port <= 65535;
}

static int64_t h1_semantics(HConn* c, HEx* e) {
    const unsigned char* b = e->hbase;
    const unsigned char* v = b + e->pm[PM_VERSION].voff;
    if (v[5] != '1') {
        c->p.err_status = 505;
        return HE_VERSION;
    }
    e->version = v[7] == '0' ? 10 : 11;
    int close = 0, keep = 0, hosts = 0, cls = 0, tes = 0, te_chunked = 0;
    int64_t clen = -1;
    for (int32_t i = 0; i < e->nf; i++) {
        HField* f = &e->f[i];
        const unsigned char *nm = b + f->noff, *val = b + f->voff;
        if (h_ieq(nm, f->nlen, "host")) hosts++;
        else if (h_ieq(nm, f->nlen, "content-length")) {
            if (++cls > 1 || !h_digits(val, f->vlen, &clen)) return HE_FRAMING;
        } else if (h_ieq(nm, f->nlen, "transfer-encoding")) {
            if (++tes > 1) return HE_FRAMING;
            te_chunked = h_ieq(val, f->vlen, "chunked");
            if (!te_chunked && !c->client) {
                int token = f->vlen > 0;
                for (uint32_t k = 0; k < f->vlen; k++) token &= H_TCHAR(val[k]) != 0;
                c->p.err_status = token ? 501 : 400; /* unknown coding vs. malformed list */
                return HE_FRAMING;
            }
        } else if (h_ieq(nm, f->nlen, "connection")) h_conn_tokens(val, f->vlen, &close, &keep);
        else if (!c->client && h_ieq(nm, f->nlen, "expect") && h_ieq(val, f->vlen, "100-continue")) e->expect100 = 1;
    }
    e->keepalive = !close && (e->version == 11 || keep);
    if (tes && cls) return HE_FRAMING; /* Content-Length + Transfer-Encoding: request smuggling */
    if (!c->client) {
        if (tes && e->version == 10) return HE_FRAMING;
        if (hosts > 1 || (e->version == 11 && hosts != 1)) return HE_BADMSG;
        const unsigned char* m = b + e->pm[PM_METHOD].voff;
        uint32_t ml = e->pm[PM_METHOD].vlen;
        const unsigned char* t = b + e->pm[PM_TARGET].voff;
        uint32_t tl = e->pm[PM_TARGET].vlen;
        int connect = ml == 7 && memcmp(m, "CONNECT", 7) == 0;
        e->is_head_req = ml == 4 && memcmp(m, "HEAD", 4) == 0;
        if (tl == 1 && t[0] == '*') {
            if (!(ml == 7 && memcmp(m, "OPTIONS", 7) == 0)) return HE_BADMSG;
            e->tform = 3;
        } else if (connect) {
            if (!h_valid_connect_authority(t, tl)) return HE_BADMSG;
            /* An HTTP/1.1 CONNECT would switch the connection to a raw tunnel after a 2xx response
               (no Content-Length / Transfer-Encoding); that mode is not implemented, so decline it. */
            c->p.err_status = 501;
            return HE_BADMSG;
        } else if (t[0] == '/') e->tform = 0;
        else {
            uint32_t k = 0;
            while (k < tl && (((t[k] | 0x20) >= 'a' && (t[k] | 0x20) <= 'z') || (k && strchr("+-.0123456789", t[k]))))
                k++;
            if (k == 0 || k + 3 > tl || memcmp(t + k, "://", 3) != 0) return HE_BADMSG;
            e->tform = 1;
        }
        if (tes) e->bmode = BM_CHUNKED;
        else if (cls) {
            if (clen > c->lim[HL_BODY]) {
                c->p.err_status = 413;
                return HE_BODYSIZE;
            }
            e->bmode = clen ? BM_FIXED : BM_NONE;
            e->bleft = clen;
        } else e->bmode = BM_NONE;
    } else {
        int64_t st = 0;
        h_digits(b + e->pm[PM_STATUS].voff, 3, &st);
        if (st < 100) return HE_BADMSG;
        e->status = (int32_t)st;
        if (e->is_head_req || st < 200 || st == 204 || st == 205 || st == 304) e->bmode = BM_NONE;
        else if (tes) e->bmode = te_chunked ? BM_CHUNKED : BM_CLOSE;
        else if (cls) {
            e->bmode = clen ? BM_FIXED : BM_NONE;
            e->bleft = clen;
        } else e->bmode = BM_CLOSE;
        if (e->bmode == BM_CLOSE) e->keepalive = 0;
    }
    e->clen = clen;
    e->in_done = e->bmode == BM_NONE;
    return SA_OK;
}

static int64_t h1_read_ms(HConn* c, HEx* e) {
    int64_t ms = c->lim[HL_T_READ];
    if (e->req_deadline_ns) {
        int64_t rem = (e->req_deadline_ns - sa_now_ns()) / 1000000;
        if (rem <= 0) return -1;
        if (!ms || rem < ms) ms = rem;
    }
    return ms;
}

/* Reads one head into `e` (request on a server, response on a client). */
static int64_t h1_read_head(HConn* c, HEx* e) {
    memset(&c->p, 0, sizeof(c->p));
    c->hstart = c->rpos;
    int64_t t0 = 0;
    for (;;) {
        if (c->rfill > c->hstart) {
            if (!t0) t0 = sa_now_ns();
            int64_t r = h1_parse(c, e);
            if (r) {
                if (r < 0) return r;
                break;
            }
        }
        int64_t ms;
        if (c->client) ms = c->lim[HL_T_READ];
        else if (!t0) ms = c->lim[HL_T_IDLE];
        else if ((ms = c->lim[HL_T_HEADER]) > 0) {
            ms -= (sa_now_ns() - t0) / 1000000;
            if (ms <= 0) return SA_ETIMEOUT;
        }
        int64_t r = h_fill(c, c->hstart, ms);
        if (r == SA_EEOF) return c->rfill > c->hstart ? SA_ECLOSED : SA_EEOF;
        if (r < 0) return r;
    }
    hseg_ref(c->rseg);
    e->hseg = c->rseg;
    e->hbase = c->rseg->data + c->hstart;
    c->rpos = c->hstart + c->p.pos;
    e->head_ready = 1;
    HST(HS_REQUESTS, 1);
    return h1_semantics(c, e);
}

static int h_tpush(HConn* c, HEx* e, unsigned char ch) {
    if (e->tn >= (uint32_t)c->lim[HL_HDRBYTES]) return -1;
    if (e->tn == e->tcap) {
        uint32_t nc = e->tcap ? e->tcap * 2 : 256;
        unsigned char* p = (unsigned char*)realloc(e->tarena, nc);
        if (!p) return -1;
        e->tarena = p;
        e->tcap = nc;
    }
    e->tarena[e->tn++] = ch;
    HSTX(HS_HDR_COPY, 1);
    return 0;
}

/* Chunked decoder over [b, b+avail). Returns a data view length (> 0), 0 when it consumed only
 * framing bytes (in_done may now be set), or an error. */
static int64_t h1_chunk_step(HConn* c, HEx* e, unsigned char* b, uint32_t avail, uint32_t max, HView* out) {
    uint32_t i = 0;
    while (i < avail) {
        unsigned char ch = b[i];
        switch (e->cst) {
        case CS_SIZE0:
        case CS_SIZE: {
            int d = ch >= '0' && ch <= '9' ? ch - '0' : ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'f') ? (ch | 0x20) - 'a' + 10 : -1;
            if (d >= 0) {
                if (e->csize > ((uint64_t)c->lim[HL_CHUNK] >> 4) + 1) return HE_BODYSIZE; /* overflow guard */
                e->csize = e->csize * 16 + (uint64_t)d;
                if (e->csize > (uint64_t)c->lim[HL_CHUNK]) return HE_BODYSIZE;
                e->cst = CS_SIZE;
            } else if (e->cst == CS_SIZE0) return HE_FRAMING;
            else if (ch == ';') e->cst = CS_EXT;
            else if (ch == ' ' || ch == '\t') e->cst = CS_BWS;
            else if (ch == '\r') e->cst = CS_SIZE_LF;
            else return HE_FRAMING;
            i++;
            break;
        }
        case CS_BWS:
            if (ch == ';') e->cst = CS_EXT;
            else if (ch != ' ' && ch != '\t') return HE_FRAMING;
            i++;
            break;
        case CS_EXT:
            if (ch == '\r') e->cst = CS_SIZE_LF;
            else if (!(ch == '\t' || ch == ' ' || H_FVCHAR(ch)) || ++e->cext > 4096) return HE_FRAMING;
            i++;
            break;
        case CS_SIZE_LF:
            if (ch != '\n') return HE_FRAMING;
            i++;
            if (e->csize == 0) e->cst = CS_TSTART;
            else {
                if (e->btotal + (int64_t)e->csize > c->lim[HL_BODY]) return HE_BODYSIZE;
                e->cst = CS_DATA;
            }
            break;
        case CS_DATA: {
            uint32_t n = avail - i;
            if (n > max) n = max;
            if (n > e->csize) n = (uint32_t)e->csize;
            if (out) {
                hseg_ref(c->rseg);
                out->seg = c->rseg;
                out->p = b + i;
                out->len = n;
            }
            c->rpos += i + n;
            e->btotal += n;
            if ((e->csize -= n) == 0) e->cst = CS_DATA_CR;
            return n;
        }
        case CS_DATA_CR:
            if (ch != '\r') return HE_FRAMING;
            e->cst = CS_DATA_LF;
            i++;
            break;
        case CS_DATA_LF:
            if (ch != '\n') return HE_FRAMING;
            e->cst = CS_SIZE0;
            e->cext = 0;
            i++;
            break;
        case CS_TSTART:
            if (ch == '\r') {
                e->cst = CS_END_LF;
                i++;
                break;
            }
            if (!H_TCHAR(ch)) return HE_HEADER;
            e->tnoff = e->tn;
            e->cst = CS_TNAME;
            break;
        case CS_TNAME:
            if (ch == ':') {
                e->tnlen = e->tn - e->tnoff;
                e->cst = CS_TOWS;
            } else if (!H_TCHAR(ch) || h_tpush(c, e, ch)) return HE_HEADER;
            i++;
            break;
        case CS_TOWS:
            if (ch == ' ' || ch == '\t') {
                i++;
                break;
            }
            e->tvoff = e->tvend = e->tn;
            e->cst = CS_TVAL;
            break;
        case CS_TVAL:
            if (ch == '\r') e->cst = CS_TLF;
            else if (H_FVCHAR(ch) || ch == ' ' || ch == '\t') {
                if (h_tpush(c, e, ch)) return HE_HDRSIZE;
                if (ch != ' ' && ch != '\t') e->tvend = e->tn;
            } else return HE_HEADER;
            i++;
            break;
        case CS_TLF: {
            if (ch != '\n' || e->ntf >= c->lim[HL_HDRCOUNT]) return HE_HEADER;
            HField* nf = (HField*)realloc(e->tf, sizeof(HField) * (size_t)(e->ntf + 1));
            if (!nf) return SA_EOS;
            e->tf = nf;
            HField f = {e->tnoff, e->tnlen, e->tvoff, e->tvend - e->tvoff};
            e->tf[e->ntf++] = f;
            e->cst = CS_TSTART;
            i++;
            break;
        }
        case CS_END_LF:
            if (ch != '\n') return HE_FRAMING;
            c->rpos += i + 1;
            e->in_done = 1;
            return 0;
        }
    }
    c->rpos += i;
    return 0;
}

/* Next view of the inbound body: > 0 bytes, 0 at the end, or an error. out == NULL discards. */
static int64_t h1_body(HConn* c, HEx* e, uint32_t max, HView* out) {
    for (;;) {
        if (e->in_done) return 0;
        if (e->in_err) return e->in_err;
        uint32_t avail = c->rfill - c->rpos;
        if (avail == 0) {
            int64_t ms = h1_read_ms(c, e);
            if (ms < 0) return SA_ETIMEOUT;
            int64_t r = h_fill(c, c->rpos, ms);
            if (r == SA_EEOF) {
                if (e->bmode == BM_CLOSE) {
                    e->in_done = 1;
                    return 0;
                }
                return SA_ECLOSED; /* truncated body */
            }
            if (r < 0) return r;
            continue;
        }
        unsigned char* b = c->rseg->data + c->rpos;
        if (e->bmode == BM_CHUNKED) {
            int64_t r = h1_chunk_step(c, e, b, avail, max, out);
            if (r) return r;
            continue;
        }
        uint32_t n = avail < max ? avail : max;
        if (e->bmode == BM_FIXED && (int64_t)n > e->bleft) n = (uint32_t)e->bleft;
        if (e->bmode == BM_CLOSE && e->btotal + n > c->lim[HL_BODY]) return HE_BODYSIZE;
        if (out) {
            hseg_ref(c->rseg);
            out->seg = c->rseg;
            out->p = b;
            out->len = n;
        }
        c->rpos += n;
        e->btotal += n;
        if (e->bmode == BM_FIXED && (e->bleft -= n) == 0) e->in_done = 1;
        return n;
    }
}

static const char* h_reason(int64_t s) {
    switch (s) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 417: return "Expectation Failed";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "";
    }
}

/* Answers a request that failed to parse, then kills the connection. */
static void h1_reject(HConn* c, int64_t err) {
    int st = c->p.err_status;
    if (!st) st = err == HE_HDRSIZE ? 431 : err == HE_BODYSIZE ? 413 : err == HE_VERSION ? 505 : 400;
    if (err > HE_BADMSG || err < HE_GOAWAY) st = 0; /* I/O failures get no response */
    if (st) {
        char buf[160];
        int n = snprintf(buf, sizeof buf, "HTTP/1.1 %d %s\r\ncontent-length: 0\r\nconnection: close\r\n\r\n", st, h_reason(st));
        h_io(c, 1, (unsigned char*)buf, n, c->lim[HL_T_WRITE]);
    }
    h_die_ex(c, err, -1, st != 0);
}

static int64_t h1_next(HConn* c) {
    if (c->cur) {
        HEx* e = c->cur;
        if (e->out_state != 2) return SA_EINVAL; /* answer the current request first */
        int64_t drained = 0, r = 0;
        while (!e->in_done && (r = h1_body(c, e, 65536, NULL)) > 0)
            if ((drained += r) > c->lim[HL_DRAIN]) break;
        if (!e->in_done) c->h1_close = 1;
        c->cur = NULL;
        sa_obj_unref(&e->obj);
    }
    if (c->h1_close) return SA_EEOF;
    HEx* e = h_ex_new(c, 2);
    if (!e) return SA_EOS;
    e->proto = 1;
    int64_t r = h1_read_head(c, e);
    if (r) {
        if (r != SA_EEOF) h1_reject(c, r);
        else h_die(c, SA_EEOF, -1);
        sa_obj_unref(&e->obj);
        sa_obj_unref(&e->obj);
        return r;
    }
    if (c->lim[HL_T_REQUEST] > 0) e->req_deadline_ns = sa_now_ns() + c->lim[HL_T_REQUEST] * 1000000;
    c->served++;
    c->cur = e;
    int64_t h = sa_tab_insert(c->obj.rt, &e->obj);
    if (!h) {
        sa_obj_unref(&e->obj);
        return SA_EOS;
    }
    return h;
}

/* Writes the head of a response (server) or request (client). */
static int64_t h1_send(HConn* c, HEx* e, int64_t status, int64_t len) {
    int interim = !e->client && status < 200;
    if (interim && status == 101) return SA_EINVAL; /* protocol upgrades are not supported */
    if (!interim) {
        if (!e->client) {
            e->out_nobody = e->is_head_req || status == 204 || status == 205 || status == 304;
            if (len < 0 && e->version == 10) c->h1_close = 1; /* close-delimited body */
            if (!e->keepalive || e->out_close) c->h1_close = 1;
        } else if (e->out_close) c->h1_close = 1;
        e->out_chunked = len < 0 && !e->out_nobody && (e->client || e->version == 11);
    }
    int need_host = e->client; /* HTTP/1.1 requires Host: default to the connection's authority */
    for (uint32_t i = 0; need_host && i < e->ohn;) {
        uint32_t nl = (uint32_t)strlen((const char*)e->oh + i);
        if (h_ieq(e->oh + i, nl, "host")) need_host = 0;
        i += nl + 1 + (uint32_t)strlen((const char*)e->oh + i + nl + 1) + 1;
    }
    if (need_host && !c->authority) return SA_EINVAL;
    size_t cap = (size_t)e->ohn * 2 + 256 + (e->mout ? strlen(e->mout) + strlen(e->tout) : 0) +
                 (need_host ? strlen(c->authority) : 0);
    unsigned char* w = h_wb(c, cap);
    if (!w) return SA_EOS;
    int n;
    if (e->client && need_host) n = snprintf((char*)w, cap, "%s %s HTTP/1.1\r\nhost: %s\r\n", e->mout, e->tout, c->authority);
    else if (e->client) n = snprintf((char*)w, cap, "%s %s HTTP/1.1\r\n", e->mout, e->tout);
    else n = snprintf((char*)w, cap, "HTTP/1.%d %d %s\r\n", e->version == 10 ? 0 : 1, (int)status, h_reason(status));
    if (!interim) {
        for (uint32_t i = 0; i < e->ohn;) {
            const char* nm = (const char*)e->oh + i;
            size_t nl = strlen(nm);
            const char* v = nm + nl + 1;
            size_t vl = strlen(v);
            memcpy(w + n, nm, nl); n += (int)nl;
            memcpy(w + n, ": ", 2); n += 2;
            memcpy(w + n, v, vl); n += (int)vl;
            memcpy(w + n, "\r\n", 2); n += 2;
            i += (uint32_t)(nl + vl + 2);
        }
        if (e->out_chunked) n += snprintf((char*)w + n, cap - (size_t)n, "transfer-encoding: chunked\r\n");
        else if (!e->client && status == 205) /* no content; an explicit 0 keeps the connection reusable */
            n += snprintf((char*)w + n, cap - (size_t)n, "content-length: 0\r\n");
        else if (len >= 0 && status != 204 && status != 304 && (len > 0 || !e->client))
            n += snprintf((char*)w + n, cap - (size_t)n, "content-length: %lld\r\n", (long long)len);
        if (!e->client && c->h1_close) n += snprintf((char*)w + n, cap - (size_t)n, "connection: close\r\n");
        else if (!e->client && e->version == 10) n += snprintf((char*)w + n, cap - (size_t)n, "connection: keep-alive\r\n");
    }
    memcpy(w + n, "\r\n", 2);
    n += 2;
    HSTX(HS_SER_COPY, n);
    if (!e->client && !interim && len > 0 && !e->out_nobody) e->phn = (uint32_t)n; /* sent with the first body bytes */
    else {
        int64_t r = h_io(c, 1, w, n, c->lim[HL_T_WRITE]);
        if (r < 0) {
            h_die(c, r, -1);
            return r;
        }
    }
    if (!interim) {
        e->out_state = len == 0 ? 2 : 1; /* an empty body is complete with its head */
        e->out_len = len;
    }
    return SA_OK;
}

static int64_t h1_write(HConn* c, HEx* e, const unsigned char* p, int64_t n) {
    int64_t ms = c->lim[HL_T_WRITE], r;
    if (e->phn) { /* the held-back head (in c->wb) and the first body bytes share a system call */
        uint32_t hn = e->phn;
        e->phn = 0;
        if (n <= H_SMALL) {
            unsigned char* w = h_wb(c, (size_t)hn + (size_t)n);
            if (!w) return SA_EOS;
            memcpy(w + hn, p, (size_t)n);
            HSTX(HS_SER_COPY, n);
            r = h_io(c, 1, w, hn + n, ms);
            if (r < 0) h_die(c, r, -1);
            return r < 0 ? r : SA_OK;
        }
        HSTX(HS_WRITE_DIRECT, n);
        r = h_io2(c, c->wb, hn, p, n, ms);
        if (r < 0) h_die(c, r, -1);
        return r < 0 ? r : SA_OK;
    }
    if (!e->out_chunked) {
        HSTX(HS_WRITE_DIRECT, n);
        r = h_io(c, 1, (unsigned char*)p, n, ms);
    } else {
        char hdr[32];
        int hl = snprintf(hdr, sizeof hdr, "%s%llx\r\n", e->pending_crlf ? "\r\n" : "", (unsigned long long)n);
        if (n <= H_SMALL) { /* coalesce: one system call */
            unsigned char* w = h_wb(c, (size_t)hl + (size_t)n);
            if (!w) return SA_EOS;
            memcpy(w, hdr, (size_t)hl);
            memcpy(w + hl, p, (size_t)n);
            HSTX(HS_SER_COPY, n);
            r = h_io(c, 1, w, hl + n, ms);
        } else {
            HSTX(HS_WRITE_DIRECT, n);
            r = h_io2(c, (unsigned char*)hdr, hl, p, n, ms);
        }
        e->pending_crlf = 1;
    }
    if (r < 0) h_die(c, r, -1);
    return r < 0 ? r : SA_OK;
}

static int64_t h1_finish(HConn* c, HEx* e) {
    int64_t r = SA_OK;
    e->phn = 0;
    if (e->out_chunked) {
        const char* t = e->pending_crlf ? "\r\n0\r\n\r\n" : "0\r\n\r\n";
        r = h_io(c, 1, (unsigned char*)t, (int64_t)strlen(t), c->lim[HL_T_WRITE]);
        if (r < 0) h_die(c, r, -1);
    } else if (!e->out_nobody && e->out_len >= 0 && e->out_sent != e->out_len) {
        h_die(c, HE_FRAMING, -1); /* the declared length was not honored: the stream is unusable */
        r = HE_FRAMING;
    }
    e->out_state = 2;
    if (!e->client && c->h1_close && !c->dead) salivo_aio_tcp_shutdown(c->sock);
    return r < 0 ? r : SA_OK;
}

static int64_t h1_response(HConn* c, HEx* e) {
    for (;;) {
        e->nf = 0;
        memset(e->pm, 0, sizeof(e->pm));
        hseg_unref(e->hseg);
        e->hseg = NULL;
        int64_t r = h1_read_head(c, e);
        if (r) {
            if (r != SA_EEOF) h_die(c, r, -1);
            return r == SA_EEOF ? SA_ECLOSED : r;
        }
        if (e->status >= 200 || e->status == 101) break; /* skip 1xx interim responses */
    }
    if (!e->keepalive) c->h1_close = 1;
    return e->status;
}

/* ============================================================================================
 * HPACK (RFC 7541)
 * ========================================================================================== */
static const char* const g_hs[61][2] = {
    {":authority", ""}, {":method", "GET"}, {":method", "POST"}, {":path", "/"}, {":path", "/index.html"},
    {":scheme", "http"}, {":scheme", "https"}, {":status", "200"}, {":status", "204"}, {":status", "206"},
    {":status", "304"}, {":status", "400"}, {":status", "404"}, {":status", "500"}, {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"}, {"accept-language", ""}, {"accept-ranges", ""}, {"accept", ""},
    {"access-control-allow-origin", ""}, {"age", ""}, {"allow", ""}, {"authorization", ""},
    {"cache-control", ""}, {"content-disposition", ""}, {"content-encoding", ""}, {"content-language", ""},
    {"content-length", ""}, {"content-location", ""}, {"content-range", ""}, {"content-type", ""},
    {"cookie", ""}, {"date", ""}, {"etag", ""}, {"expect", ""}, {"expires", ""}, {"from", ""}, {"host", ""},
    {"if-match", ""}, {"if-modified-since", ""}, {"if-none-match", ""}, {"if-range", ""},
    {"if-unmodified-since", ""}, {"last-modified", ""}, {"link", ""}, {"location", ""}, {"max-forwards", ""},
    {"proxy-authenticate", ""}, {"proxy-authorization", ""}, {"range", ""}, {"referer", ""}, {"refresh", ""},
    {"retry-after", ""}, {"server", ""}, {"set-cookie", ""}, {"strict-transport-security", ""},
    {"transfer-encoding", ""}, {"user-agent", ""}, {"vary", ""}, {"via", ""}, {"www-authenticate", ""}};

/* Huffman code lengths (RFC 7541 Appendix B). The code is canonical, so the codes themselves
 * are derived from the lengths. */
static const unsigned char g_hl[257] = {
    13,23,28,28,28,28,28,28,28,24,30,28,28,30,28,28,28,28,28,28,28,28,30,28,28,28,28,28,28,28,28,28,6,10,10,12,
    13,6,8,11,10,10,8,11,8,6,6,6,5,5,5,6,6,6,6,6,6,6,7,8,15,6,12,10,13,6,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,8,7,8,13,19,13,14,6,15,5,6,5,6,5,6,6,6,5,7,7,6,6,6,5,6,7,6,5,5,6,7,7,7,7,7,15,11,14,13,28,20,22,20,20,
    22,22,22,23,22,23,23,23,23,23,24,23,24,24,22,23,24,23,23,23,23,21,22,23,22,23,23,24,22,21,20,22,22,23,23,
    21,23,22,22,24,21,22,23,23,21,21,22,21,23,22,23,23,20,22,22,22,23,22,22,23,26,26,20,19,22,23,22,25,26,26,
    26,27,27,26,24,25,19,21,26,27,27,26,27,24,21,21,26,26,28,27,27,27,20,24,20,21,22,21,21,23,22,22,25,25,24,
    24,26,23,26,27,26,26,27,27,27,27,27,28,27,27,27,27,27,26,30};
static uint32_t g_hcode[257];
static uint32_t g_hfirst[31], g_hcount[31], g_hoff[31];
static uint16_t g_hsym[257];
static int32_t g_huff_ready;

static void hp_huff_init(void) {
    if (A_LOAD(&g_huff_ready)) return;
    sa_mutex_lock(&g_hpool_lock);
    if (!g_huff_ready) {
        uint32_t k = 0, code = 0;
        for (uint32_t len = 1; len <= 30; len++) {
            g_hoff[len] = k;
            g_hfirst[len] = code;
            for (uint32_t s = 0; s < 257; s++)
                if (g_hl[s] == len) {
                    g_hsym[k++] = (uint16_t)s;
                    g_hcode[s] = code++;
                }
            g_hcount[len] = k - g_hoff[len];
            code <<= 1;
        }
        A_STORE(&g_huff_ready, 1);
    }
    sa_mutex_unlock(&g_hpool_lock);
}

static int hp_int(const unsigned char** pp, const unsigned char* end, int prefix, uint32_t* out) {
    const unsigned char* p = *pp;
    if (p >= end) return -1;
    uint32_t mask = (1u << prefix) - 1, v = *p++ & mask;
    if (v == mask) {
        uint32_t m = 0;
        for (;;) {
            if (p >= end || m > 21) return -1; /* integer overflow / truncated */
            unsigned char b = *p++;
            v += (uint32_t)(b & 0x7f) << m;
            m += 7;
            if (!(b & 0x80)) break;
        }
        if (v > (1u << 28)) return -1;
    }
    *pp = p;
    *out = v;
    return 0;
}

/* Decodes one string literal into dst (capacity cap). */
static int hp_str(const unsigned char** pp, const unsigned char* end, unsigned char* dst, uint32_t cap, uint32_t* olen) {
    const unsigned char* p = *pp;
    if (p >= end) return -1;
    int huff = *p & 0x80;
    uint32_t n;
    if (hp_int(&p, end, 7, &n) || n > (uint32_t)(end - p)) return -1;
    if (!huff) {
        if (n > cap) return -1;
        memcpy(dst, p, n);
        *olen = n;
    } else {
        uint32_t code = 0, len = 0, o = 0;
        for (uint32_t i = 0; i < n; i++)
            for (int bit = 7; bit >= 0; bit--) {
                code = (code << 1) | ((p[i] >> bit) & 1);
                len++;
                if (len > 30) return -1;
                if (g_hcount[len] && code - g_hfirst[len] < g_hcount[len]) {
                    uint16_t s = g_hsym[g_hoff[len] + code - g_hfirst[len]];
                    if (s == 256 || o >= cap) return -1; /* EOS inside a string / expansion bound */
                    dst[o++] = (unsigned char)s;
                    code = len = 0;
                }
            }
        if (len > 7 || code != (1u << len) - 1) return -1; /* padding must be the EOS prefix */
        *olen = o;
    }
    *pp = p + n;
    return 0;
}

#define HDYN_SLOTS (4096 / 32 + 1)
static HDynE* hp_dyn_at(HDyn* d, uint32_t i) { return &d->e[(d->head + i) % HDYN_SLOTS]; }

static void hp_dyn_evict(HDyn* d, uint32_t max) {
    while (d->n && d->size > max) {
        HDynE* e = hp_dyn_at(d, d->n - 1);
        d->size -= e->nlen + e->vlen + 32;
        free(e->p);
        e->p = NULL;
        d->n--;
    }
}

static int hp_dyn_add(HDyn* d, const unsigned char* n, uint32_t nl, const unsigned char* v, uint32_t vl) {
    uint32_t sz = nl + vl + 32;
    if (sz > d->max) {
        hp_dyn_evict(d, 0);
        return 0;
    }
    hp_dyn_evict(d, d->max - sz);
    unsigned char* p = (unsigned char*)malloc((size_t)nl + vl + 1);
    if (!p) return -1;
    memcpy(p, n, nl);
    memcpy(p + nl, v, vl);
    d->head = (d->head + HDYN_SLOTS - 1) % HDYN_SLOTS;
    HDynE* e = &d->e[d->head];
    e->p = p;
    e->nlen = nl;
    e->vlen = vl;
    d->n++;
    d->size += sz;
    return 0;
}

typedef struct {
    HField* f;
    int32_t nf, fcap;
    unsigned char *out, *str;
    uint32_t used, cap, scap;
    uint32_t listsize, limit;
    int too_large;
} HDec;

static void hp_emit(HDec* o, const unsigned char* n, uint32_t nl, const unsigned char* v, uint32_t vl) {
    o->listsize += nl + vl + 32;
    if (o->too_large || o->listsize > o->limit || o->nf >= o->fcap || o->used + nl + vl > o->cap) {
        o->too_large = 1; /* keep decoding for table synchronization, store nothing more */
        return;
    }
    HField f = {o->used, nl, o->used + nl, vl};
    memcpy(o->out + o->used, n, nl);
    memcpy(o->out + o->used + nl, v, vl);
    o->used += nl + vl;
    o->f[o->nf++] = f;
}

static int hp_entry(HDyn* d, uint32_t idx, const unsigned char** n, uint32_t* nl, const unsigned char** v, uint32_t* vl) {
    if (idx == 0) return -1;
    if (idx <= 61) {
        *n = (const unsigned char*)g_hs[idx - 1][0];
        *nl = (uint32_t)strlen(g_hs[idx - 1][0]);
        *v = (const unsigned char*)g_hs[idx - 1][1];
        *vl = (uint32_t)strlen(g_hs[idx - 1][1]);
        return 0;
    }
    if (idx - 62 >= d->n) return -1;
    HDynE* e = hp_dyn_at(d, idx - 62);
    *n = e->p;
    *nl = e->nlen;
    *v = e->p + e->nlen;
    *vl = e->vlen;
    return 0;
}

static int hp_decode(HDyn* d, const unsigned char* p, uint32_t len, HDec* o) {
    const unsigned char* end = p + len;
    int fields = 0;
    while (p < end) {
        unsigned char b = *p;
        uint32_t idx, nl, vl;
        const unsigned char *n, *v;
        if (b & 0x80) {
            if (hp_int(&p, end, 7, &idx) || hp_entry(d, idx, &n, &nl, &v, &vl)) return -1;
            hp_emit(o, n, nl, v, vl);
            fields++;
        } else if ((b & 0xe0) == 0x20) { /* dynamic table size update: only before the first field */
            if (fields || hp_int(&p, end, 5, &idx) || idx > 4096) return -1;
            d->max = idx;
            hp_dyn_evict(d, idx);
        } else {
            int incr = (b & 0xc0) == 0x40;
            if (hp_int(&p, end, incr ? 6 : 4, &idx)) return -1;
            unsigned char* s = o->str;
            if (idx) {
                if (hp_entry(d, idx, &n, &nl, &v, &vl)) return -1;
                memcpy(s, n, nl);
            } else if (hp_str(&p, end, s, o->scap, &nl)) return -1;
            if (hp_str(&p, end, s + nl, o->scap - nl, &vl)) return -1;
            hp_emit(o, s, nl, s + nl, vl);
            if (incr && hp_dyn_add(d, s, nl, s + nl, vl)) return -1;
            fields++;
        }
    }
    return 0;
}

static uint32_t hp_enc_int(unsigned char* p, uint32_t v, int prefix, unsigned char first) {
    uint32_t mask = (1u << prefix) - 1, o = 0;
    if (v < mask) {
        p[o++] = (unsigned char)(first | v);
        return o;
    }
    p[o++] = (unsigned char)(first | mask);
    v -= mask;
    while (v >= 128) {
        p[o++] = (unsigned char)((v & 0x7f) | 0x80);
        v >>= 7;
    }
    p[o++] = (unsigned char)v;
    return o;
}

static uint32_t hp_enc_str(unsigned char* p, const unsigned char* s, uint32_t n, int lower) {
    uint64_t bits = 0;
    for (uint32_t i = 0; i < n; i++) bits += g_hl[lower ? h_lc(s[i]) : s[i]];
    uint32_t hb = (uint32_t)((bits + 7) / 8), o;
    if (hb < n) {
        o = hp_enc_int(p, hb, 7, 0x80);
        uint64_t acc = 0;
        int nb = 0;
        for (uint32_t i = 0; i < n; i++) {
            unsigned char c = lower ? h_lc(s[i]) : s[i];
            acc = (acc << g_hl[c]) | g_hcode[c];
            nb += g_hl[c];
            while (nb >= 8) {
                nb -= 8;
                p[o++] = (unsigned char)(acc >> nb);
            }
        }
        if (nb) p[o++] = (unsigned char)((acc << (8 - nb)) | (0xffu >> nb));
    } else {
        o = hp_enc_int(p, n, 7, 0);
        for (uint32_t i = 0; i < n; i++) p[o++] = lower ? h_lc(s[i]) : s[i];
    }
    return o;
}

/* Literal without indexing (or a static-table index): the encoder keeps no dynamic state, so
 * frames may be encoded in any order and the peer's table size never matters. */
static uint32_t hp_enc_field(unsigned char* p, const unsigned char* n, uint32_t nl, const unsigned char* v, uint32_t vl) {
    uint32_t si = 0;
    for (uint32_t i = 0; i < 61; i++)
        if (h_ieq(n, nl, g_hs[i][0])) {
            if (!si) si = i + 1;
            if (vl && strlen(g_hs[i][1]) == vl && memcmp(g_hs[i][1], v, vl) == 0) return hp_enc_int(p, i + 1, 7, 0x80);
        }
    uint32_t o;
    if (si) o = hp_enc_int(p, si, 4, 0);
    else {
        p[0] = 0;
        o = 1 + hp_enc_str(p + 1, n, nl, 1);
    }
    return o + hp_enc_str(p + o, v, vl, 0);
}

/* ============================================================================================
 * HTTP/2 (RFC 9113)
 * ========================================================================================== */
enum { F_DATA, F_HEADERS, F_PRIORITY, F_RST, F_SETTINGS, F_PUSH, F_PING, F_GOAWAY, F_WINDOW, F_CONT };
enum { E_NO = 0, E_PROTOCOL = 1, E_INTERNAL = 2, E_FLOW = 3, E_CLOSED = 5, E_FRAME_SIZE = 6, E_REFUSED = 7,
       E_CANCEL = 8, E_COMPRESSION = 9, E_CALM = 11 };
static const char g_preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

static uint32_t h_be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void h_put32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}
static void h2_fhdr(unsigned char* p, uint32_t len, uint8_t type, uint8_t flags, uint32_t sid) {
    p[0] = (unsigned char)(len >> 16); p[1] = (unsigned char)(len >> 8); p[2] = (unsigned char)len;
    p[3] = type; p[4] = flags;
    h_put32(p + 5, sid & 0x7fffffff);
}

static int64_t h2_local_err(int code) {
    return code == E_FLOW ? HE_FLOW : code == E_COMPRESSION ? HE_COMPRESS : HE_PROTOCOL;
}

/* Queues a control frame (lock held). Returns nonzero when the bounded queue is full. */
static int h2_ctl(HConn* c, uint8_t type, uint8_t flags, uint32_t sid, const unsigned char* p, uint32_t n) {
    if (c->cqn + 9 + n > (uint32_t)c->lim[HL_CTLQ]) return -1;
    h2_fhdr(c->cq + c->cqn, n, type, flags, sid);
    if (n) memcpy(c->cq + c->cqn + 9, p, n);
    c->cqn += 9 + n;
    if (A_LOAD(&g_hstats_on) && c->cqn > (uint32_t)A_LOAD(&g_hst[HS_CTL_PEAK])) A_STORE(&g_hst[HS_CTL_PEAK], (int64_t)c->cqn);
    return 0;
}

static void h2_rst(HConn* c, uint32_t sid, uint32_t code) {
    unsigned char b[4];
    h_put32(b, code);
    h2_ctl(c, F_RST, 0, sid, b, 4);
    HST(HS_RESETS, 1);
}

static void h2_window(HConn* c, uint32_t sid, uint32_t inc) {
    unsigned char b[4];
    h_put32(b, inc);
    h2_ctl(c, F_WINDOW, 0, sid, b, 4);
}

static void h_fire(SaWaiter** w) {
    if (*w) {
        sa_waiter_fire(*w);
        *w = NULL;
    }
}

static void h2_wake_all(HConn* c) {
    H2* h = c->h2;
    h_fire(&h->accept_w);
    h_fire(&h->ping_w);
    for (int32_t i = 0; i < h->ntab; i++) h_fire(&h->tab[i]->w);
    SaWaiter* w;
    while ((w = sa_list_pop(&h->sw_h, &h->sw_t)) != NULL) sa_waiter_fire(w);
    while ((w = sa_list_pop(&c->ww_h, &c->ww_t)) != NULL) sa_waiter_fire(w);
}

/* ponytail: linear stream table; bounded by the concurrent-stream limit (100 by default). */
static HEx* h2_find(H2* h, uint32_t sid) {
    if (h->hot && h->hot->sid == sid) return h->hot;
    for (int32_t i = 0; i < h->ntab; i++)
        if (h->tab[i]->sid == sid) return h->hot = h->tab[i];
    return NULL;
}

static int h2_idle(HConn* c, uint32_t sid) {
    H2* h = c->h2;
    if (!(sid & 1)) return 1; /* even streams would be server pushes; never opened */
    return c->client ? sid >= h->next_sid : sid > h->last_peer_sid;
}

static int h2_tab_add(H2* h, HEx* e) {
    if (h->ntab == h->tabcap) {
        int32_t nc = h->tabcap ? h->tabcap * 2 : 16;
        HEx** t = (HEx**)realloc(h->tab, sizeof(HEx*) * (size_t)nc);
        if (!t) return -1;
        h->tab = t;
        h->tabcap = nc;
    }
    sa_obj_ref(&e->obj);
    h->tab[h->ntab++] = e;
    e->in_table = 1;
    return 0;
}

static void h2_aq_remove(H2* h, HEx* e) {
    HEx** pp = &h->aq_h;
    HEx* prev = NULL;
    while (*pp && *pp != e) {
        prev = *pp;
        pp = &(*pp)->aq_next;
    }
    if (!*pp) return;
    *pp = e->aq_next;
    if (h->aq_t == e) h->aq_t = prev;
    h->naq--;
    e->queued = 0;
    sa_obj_unref(&e->obj);
}

/* Removes a closed stream from the table (lock held). May drop the last reference to e. */
static void h2_maybe_close(HConn* c, HEx* e) {
    H2* h = c->h2;
    if (!e->in_table || !(e->reset || (e->local_end && e->remote_end))) return;
    for (int32_t i = 0; i < h->ntab; i++)
        if (h->tab[i] == e) {
            h->tab[i] = h->tab[--h->ntab];
            break;
        }
    e->in_table = 0;
    if (h->hot == e) h->hot = NULL;
    SaWaiter* s = sa_list_pop(&h->sw_h, &h->sw_t);
    if (s) sa_waiter_fire(s);
    sa_obj_unref(&e->obj);
}

/* Resets a stream locally (lock held): RST_STREAM, buffers released, waiter woken. */
static void h2_stream_reset(HConn* c, HEx* e, uint32_t code, int64_t err, int send) {
    if (e->reset) return;
    if (send) h2_rst(c, e->sid, code);
    e->reset = 1;
    e->reset_code = (int32_t)code;
    e->in_err = err;
    e->local_end = e->remote_end = 1;
    h_chunks_free(e);
    h_fire(&e->w);
    if (e->queued) h2_aq_remove(c->h2, e);
    h2_maybe_close(c, e);
}

static int64_t h2_stream_recvwin(HConn* c) {
    int64_t w = c->lim[HL_STREAMWIN];
    return c->h2->settings_acked || w >= 65535 ? w : 65535; /* the peer may use the default until our SETTINGS are acknowledged */
}

/* Consumption credit for a stream (lock held). The connection window is credited on receipt. */
static void h2_consumed(HConn* c, HEx* e, uint32_t n) {
    e->unacked += n;
    if (!e->remote_end && e->in_table && e->unacked >= c->lim[HL_STREAMWIN] / 2) {
        h2_window(c, e->sid, (uint32_t)e->unacked);
        e->recv_win += e->unacked;
        e->unacked = 0;
    }
}

/* Queues received body bytes for the stream's reader. Returns 0, or SA_EOS when no memory is left:
   the caller then resets the stream so no DATA is ever silently dropped. */
/* Test hook (native harness only): the next delivery fails as if out of memory */
static int h_fault_deliver_oom = 0;

static int h2_deliver(HConn* c, HEx* e, unsigned char* p, uint32_t n, HSeg* seg) {
    if (h_fault_deliver_oom) {
        h_fault_deliver_oom = 0;
        return SA_EOS;
    }
    HChunk* t = e->dq_t;
    e->data_total += n;
    if (n < H_SMALL && t && !t->seg && t->p + t->len + n <= (unsigned char*)(t + 1) + t->cap) {
        memcpy(t->p + t->len, p, n);
        t->len += n;
        HSTX(HS_BODY_COPY, n);
    } else {
        HChunk* k;
        if (n < H_SMALL) {
            k = (HChunk*)malloc(sizeof(HChunk) + H_SMALL);
            if (!k) return SA_EOS;
            k->seg = NULL;
            k->p = (unsigned char*)(k + 1);
            k->cap = H_SMALL;
            memcpy(k->p, p, n);
            HSTX(HS_BODY_COPY, n);
        } else {
            k = (HChunk*)malloc(sizeof(HChunk));
            if (!k) return SA_EOS;
            hseg_ref(seg);
            k->seg = seg;
            k->p = p;
            k->cap = n;
        }
        k->len = n;
        k->next = NULL;
        if (t) t->next = k;
        else e->dq_h = k;
        e->dq_t = k;
    }
    h_fire(&e->w);
    return 0;
}

/* Validates a decoded header list (RFC 9113 8.2-8.3) and records the pseudo fields. */
static int h2_check(HConn* c, HDec* d, HEx* e, int trailer) {
    int regular = 0, request = !c->client;
    for (int32_t i = 0; i < d->nf; i++) {
        HField* f = &d->f[i];
        const unsigned char *n = d->out + f->noff, *v = d->out + f->voff;
        if (!f->nlen) return -1;
        /* RFC 9113 8.2.1: uppercase in any field name (pseudo-headers included) is malformed */
        for (uint32_t k = 0; k < f->nlen; k++)
            if (n[k] >= 'A' && n[k] <= 'Z') return -1;
        for (uint32_t k = 0; k < f->vlen; k++)
            if (v[k] == 0 || v[k] == '\r' || v[k] == '\n') return -1;
        if (f->vlen && (v[0] == ' ' || v[0] == '\t' || v[f->vlen - 1] == ' ' || v[f->vlen - 1] == '\t')) return -1;
        if (n[0] == ':') {
            if (trailer || regular) return -1;
            int k = -1;
            if (request) {
                if (h_ieq(n, f->nlen, ":method")) k = PM_METHOD;
                else if (h_ieq(n, f->nlen, ":path")) k = PM_TARGET;
                else if (h_ieq(n, f->nlen, ":authority")) k = PM_AUTH;
                else if (h_ieq(n, f->nlen, ":scheme")) k = PM_SCHEME;
            } else if (h_ieq(n, f->nlen, ":status")) k = PM_STATUS;
            if (k < 0 || e->pm[k].nlen) return -1;
            h_pm(e, k, f->voff, f->voff + f->vlen);
            continue;
        }
        regular = 1;
        for (uint32_t k = 0; k < f->nlen; k++)
            if (!H_TCHAR(n[k]) || (n[k] >= 'A' && n[k] <= 'Z')) return -1;
        if (h_ieq(n, f->nlen, "connection") || h_ieq(n, f->nlen, "keep-alive") || h_ieq(n, f->nlen, "proxy-connection") ||
            h_ieq(n, f->nlen, "transfer-encoding") || h_ieq(n, f->nlen, "upgrade"))
            return -1;
        if (h_ieq(n, f->nlen, "te") && !h_ieq(v, f->vlen, "trailers")) return -1;
        if (!trailer && h_ieq(n, f->nlen, "content-length")) {
            int64_t cl;
            if (!h_digits(v, f->vlen, &cl) || (e->clen >= 0 && e->clen != cl)) return -1;
            e->clen = cl;
        }
    }
    if (trailer) return 0;
    if (request) {
        HField* m = &e->pm[PM_METHOD];
        if (!m->nlen) return -1;
        int connect = m->vlen == 7 && memcmp(d->out + m->voff, "CONNECT", 7) == 0;
        if (connect) {
            if (!e->pm[PM_AUTH].nlen || e->pm[PM_TARGET].nlen || e->pm[PM_SCHEME].nlen) return -1;
            if (!h_valid_connect_authority(d->out + e->pm[PM_AUTH].voff, e->pm[PM_AUTH].vlen)) return -1;
            e->tform = 2;
        } else {
            if (!e->pm[PM_SCHEME].nlen || !e->pm[PM_TARGET].nlen || !e->pm[PM_TARGET].vlen) return -1;
            e->tform = e->pm[PM_TARGET].vlen == 1 && d->out[e->pm[PM_TARGET].voff] == '*' ? 3 : 0;
        }
        e->is_head_req = m->vlen == 4 && memcmp(d->out + m->voff, "HEAD", 4) == 0;
    } else {
        int64_t st;
        if (!e->pm[PM_STATUS].nlen || e->pm[PM_STATUS].vlen != 3 || !h_digits(d->out + e->pm[PM_STATUS].voff, 3, &st))
            return -1;
        e->status = (int32_t)st;
    }
    return 0;
}

/* Copies a decoded list into the exchange: the fields (or the trailers). */
static int h2_store(HDec* d, HEx* e, int trailer) {
    unsigned char* a = (unsigned char*)malloc(d->used ? d->used : 1);
    HField* f = (HField*)malloc(sizeof(HField) * (size_t)(d->nf ? d->nf : 1));
    if (!a || !f) {
        free(a);
        free(f);
        return -1;
    }
    memcpy(a, d->out, d->used);
    HSTX(HS_HDR_COPY, d->used);
    int32_t n = 0;
    for (int32_t i = 0; i < d->nf; i++)
        if (d->out[d->f[i].noff] != ':') f[n++] = d->f[i];
    if (trailer) {
        free(e->tarena);
        free(e->tf);
        e->tarena = a;
        e->tf = f;
        e->ntf = n;
    } else {
        e->arena = e->hbase = a;
        if (e->f != e->fin) free(e->f);
        e->f = f;
        e->nf = e->fcap = n;
    }
    return 0;
}

static int h2_header_block(HConn* c) {
    H2* h = c->h2;
    HDec d;
    memset(&d, 0, sizeof d);
    d.f = h->dfields;
    d.fcap = (int32_t)c->lim[HL_HDRCOUNT] + 8;
    d.out = h->dout;
    d.cap = (uint32_t)c->lim[HL_HLIST];
    d.str = h->dstr;
    d.scap = (uint32_t)c->lim[HL_HDRBYTES] * 8 / 5 + 16;
    d.limit = (uint32_t)c->lim[HL_HLIST];
    if (hp_decode(&h->dyn, h->hb, h->hbn, &d)) return E_COMPRESSION;
    uint32_t sid = h->hb_sid;
    int end = h->hb_end;
    h->hbn = 0;
    HEx* e = h2_find(h, sid);
    if (!e) {
        if (c->client) return h2_idle(c, sid) ? E_PROTOCOL : 0; /* no push: unknown open streams are errors */
        if (!(sid & 1)) return E_PROTOCOL;
        if (sid <= h->last_peer_sid) return E_CLOSED;
        h->last_peer_sid = sid;
        if (h->goaway_sent || c->dead) return 0; /* ignored after GOAWAY */
        if (d.too_large) {
            h2_rst(c, sid, E_CALM);
            return 0;
        }
        if (h->ntab >= c->lim[HL_STREAMS]) {
            h2_rst(c, sid, E_REFUSED);
            return 0;
        }
        e = h_ex_new(c, 1); /* the accept queue's reference */
        if (!e) return E_INTERNAL;
        e->proto = 2;
        e->sid = sid;
        e->version = 20;
        if (h2_check(c, &d, e, 0) || h2_store(&d, e, 0)) {
            sa_obj_unref(&e->obj);
            h2_rst(c, sid, E_PROTOCOL);
            return 0;
        }
        e->head_ready = 1;
        e->keepalive = 1;
        e->send_win = h->peer_initwin;
        e->recv_win = h2_stream_recvwin(c);
        e->remote_end = end;
        e->in_done = end && e->clen <= 0;
        if (end && e->clen > 0) h2_stream_reset(c, e, E_PROTOCOL, HE_PROTOCOL, 1);
        if (h2_tab_add(h, e)) {
            sa_obj_unref(&e->obj);
            return E_INTERNAL;
        }
        e->queued = 1;
        if (h->aq_t) h->aq_t->aq_next = e;
        else h->aq_h = e;
        h->aq_t = e;
        h->naq++;
        c->served++;
        HST(HS_REQUESTS, 1);
        HST(HS_STREAMS, 1);
        h_fire(&h->accept_w);
        return 0;
    }
    if (e->remote_end) {
        h2_stream_reset(c, e, E_CLOSED, HE_PROTOCOL, 1);
        return 0;
    }
    if (d.too_large) {
        h2_stream_reset(c, e, E_CALM, HE_HDRSIZE, 1);
        return 0;
    }
    if (c->client && !e->head_ready) {
        HField keep_pm[PM_COUNT];
        memcpy(keep_pm, e->pm, sizeof keep_pm);
        if (h2_check(c, &d, e, 0)) {
            h2_stream_reset(c, e, E_PROTOCOL, HE_PROTOCOL, 1);
            return 0;
        }
        if (e->status < 200) { /* interim response */
            memcpy(e->pm, keep_pm, sizeof keep_pm);
            if (end) h2_stream_reset(c, e, E_PROTOCOL, HE_PROTOCOL, 1);
            return 0;
        }
        if (h2_store(&d, e, 0)) return E_INTERNAL;
        e->version = 20;
        e->head_ready = 1;
        if (e->mout && strcmp(e->mout, "HEAD") == 0) e->clen = -1;
    } else {
        if (!end || h2_check(c, &d, e, 1) || h2_store(&d, e, 1)) { /* trailers must end the stream */
            h2_stream_reset(c, e, E_PROTOCOL, HE_PROTOCOL, 1);
            return 0;
        }
    }
    if (end) {
        e->remote_end = 1;
        if (e->clen >= 0 && e->data_total != e->clen && !(c->client && e->status == 304)) {
            h2_stream_reset(c, e, E_PROTOCOL, HE_FRAMING, 1);
            return 0;
        }
        e->in_done = 1;
    }
    h_fire(&e->w);
    h2_maybe_close(c, e);
    return 0;
}

static int h2_frame_done(HConn* c) {
    H2* h = c->h2;
    uint32_t len = h->flen, sid = h->fsid;
    uint8_t fl = h->fflags;
    unsigned char* b = h->fbuf;
    switch (h->ftype) {
    case F_DATA: {
        HEx* e = h->fex;
        h->fex = NULL;
        if (!e) return 0;
        if (e->in_table && !e->reset) {
            h2_consumed(c, e, len - h->fdata); /* padding is never read: credit it now */
            if (fl & 1) {
                e->remote_end = 1;
                if (e->clen >= 0 && e->data_total != e->clen) h2_stream_reset(c, e, E_PROTOCOL, HE_FRAMING, 1);
                else {
                    e->in_done = 1;
                    h_fire(&e->w);
                    h2_maybe_close(c, e);
                }
            }
        }
        sa_obj_unref(&e->obj);
        return 0;
    }
    case F_HEADERS:
    case F_CONT: {
        uint32_t off = 0, pad = 0;
        if (h->ftype == F_HEADERS) {
            if (fl & 8) {
                if (len < 1) return E_PROTOCOL;
                pad = b[0];
                off = 1;
            }
            if (fl & 0x20) {
                if (len < off + 5) return E_PROTOCOL;
                if ((h_be32(b + off) & 0x7fffffff) == sid) return E_PROTOCOL; /* depends on itself */
                off += 5;
            }
            if (off + pad > len) return E_PROTOCOL;
            h->hb_sid = sid;
            h->hb_end = fl & 1;
            h->hbn = 0;
        }
        uint32_t n = len - off - pad;
        if (h->hbn + n > (uint32_t)c->lim[HL_HDRBYTES]) return E_CALM; /* header block too large */
        memcpy(h->hb + h->hbn, b + off, n);
        h->hbn += n;
        if (!(fl & 4)) {
            h->hb_on = 1;
            return 0;
        }
        h->hb_on = 0;
        return h2_header_block(c);
    }
    case F_PRIORITY:
        if (len != 5) h2_rst(c, sid, E_FRAME_SIZE);
        else if ((h_be32(b) & 0x7fffffff) == sid) h2_rst(c, sid, E_PROTOCOL);
        return 0; /* priority signals are validated and otherwise ignored (RFC 9113 5.3.2) */
    case F_RST: {
        if (h2_idle(c, sid)) return E_PROTOCOL;
        if (++h->peer_resets > c->lim[HL_RESETS]) return E_CALM; /* rapid-reset abuse */
        HEx* e = h2_find(h, sid);
        if (e) {
            h2_stream_reset(c, e, h_be32(b), HE_RESET, 0);
            HST(HS_RESETS, 1);
        }
        return 0;
    }
    case F_SETTINGS:
        if (fl & 1) {
            h->settings_acked = 1;
            return 0;
        }
        for (uint32_t i = 0; i < len; i += 6) {
            uint32_t id = ((uint32_t)b[i] << 8) | b[i + 1], v = h_be32(b + i + 2);
            if (id == 2 && (v > 1 || (c->client && v == 1))) return E_PROTOCOL;
            if (id == 3) h->peer_streams = v;
            if (id == 4) {
                if (v > 0x7fffffff) return E_FLOW;
                int64_t delta = (int64_t)v - h->peer_initwin;
                for (int32_t k = 0; k < h->ntab; k++) {
                    if ((h->tab[k]->send_win += delta) > 0x7fffffff) return E_FLOW;
                    h_fire(&h->tab[k]->w);
                }
                h->peer_initwin = v;
            }
            if (id == 5) {
                if (v < 16384 || v > 16777215) return E_PROTOCOL;
                h->peer_frame = v;
            }
        }
        h->got_settings = 1;
        return h2_ctl(c, F_SETTINGS, 1, 0, NULL, 0) ? E_CALM : 0;
    case F_PING:
        if (fl & 1) {
            uint64_t v = ((uint64_t)h_be32(b) << 32) | h_be32(b + 4);
            if (h->ping_out && v == h->ping_seq) {
                h->ping_rtt = (sa_now_ns() - h->ping_ns) / 1000;
                h->ping_out = 0;
                h_fire(&h->ping_w);
            }
            return 0;
        }
        return h2_ctl(c, F_PING, 1, 0, b, 8) ? E_CALM : 0;
    case F_GOAWAY: {
        h->goaway_recv = 1;
        h->goaway_last = h_be32(b) & 0x7fffffff;
        for (int32_t i = h->ntab - 1; i >= 0; i--) {
            if (i >= h->ntab) continue;
            HEx* e = h->tab[i];
            if (c->client && e->sid > h->goaway_last) h2_stream_reset(c, e, E_REFUSED, HE_REFUSED, 0);
        }
        h_fire(&h->accept_w);
        SaWaiter* w;
        while ((w = sa_list_pop(&h->sw_h, &h->sw_t)) != NULL) sa_waiter_fire(w);
        return 0;
    }
    case F_WINDOW: {
        uint32_t inc = h_be32(b) & 0x7fffffff;
        if (sid == 0) {
            if (!inc) return E_PROTOCOL;
            if (h->conn_send + inc > 0x7fffffff) return E_FLOW;
            h->conn_send += inc;
            for (int32_t k = 0; k < h->ntab; k++) h_fire(&h->tab[k]->w);
            return 0;
        }
        HEx* e = h2_find(h, sid);
        if (!e) return h2_idle(c, sid) ? E_PROTOCOL : 0;
        if (!inc) h2_stream_reset(c, e, E_PROTOCOL, HE_PROTOCOL, 1);
        else if (e->send_win + inc > 0x7fffffff) h2_stream_reset(c, e, E_FLOW, HE_FLOW, 1);
        else {
            e->send_win += inc;
            h_fire(&e->w);
        }
        return 0;
    }
    default:
        return 0; /* unknown frame types are ignored */
    }
}

/* Frame header checks that do not need the payload. */
static int h2_frame_start(HConn* c) {
    H2* h = c->h2;
    unsigned char* f = h->fh;
    h->flen = ((uint32_t)f[0] << 16) | ((uint32_t)f[1] << 8) | f[2];
    h->ftype = f[3];
    h->fflags = f[4];
    h->fsid = h_be32(f + 5) & 0x7fffffff;
    h->fgot = h->pad = h->fdata = 0;
    uint32_t len = h->flen, sid = h->fsid;
    uint8_t t = h->ftype, fl = h->fflags;
    HSTX(HS_FRAMES, 1);
    if (len > (uint32_t)c->lim[HL_FRAME]) return E_FRAME_SIZE;
    if (!h->got_settings && (t != F_SETTINGS || (fl & 1))) return E_PROTOCOL; /* preface: SETTINGS first */
    if (h->hb_on && (t != F_CONT || sid != h->hb_sid)) return E_PROTOCOL;
    switch (t) {
    case F_DATA: {
        if (!sid) return E_PROTOCOL;
        if ((fl & 8) && len < 1) return E_FRAME_SIZE;
        if ((int64_t)len > h->conn_recv) return E_FLOW;
        h->conn_recv -= len;
        if ((h->conn_credit += len) >= c->lim[HL_CONNWIN] / 2) {
            h2_window(c, 0, (uint32_t)h->conn_credit);
            h->conn_recv += h->conn_credit;
            h->conn_credit = 0;
        }
        HEx* e = h2_find(h, sid);
        if (!e) {
            if (h2_idle(c, sid)) return E_PROTOCOL;
            h2_rst(c, sid, E_CLOSED);
        } else if (e->remote_end || e->reset) h2_stream_reset(c, e, E_CLOSED, HE_PROTOCOL, 1);
        else if ((int64_t)len > e->recv_win) h2_stream_reset(c, e, E_FLOW, HE_FLOW, 1);
        else if (e->data_total + len > c->lim[HL_BODY]) h2_stream_reset(c, e, E_CANCEL, HE_BODYSIZE, 1);
        else {
            e->recv_win -= len;
            sa_obj_ref(&e->obj);
            h->fex = e;
        }
        if (len == 0 && !(fl & 1) && ++h->empty_frames > 10000) return E_CALM;
        return 0;
    }
    case F_HEADERS:
    case F_PRIORITY:
        return sid ? 0 : E_PROTOCOL;
    case F_RST:
        return !sid ? E_PROTOCOL : len != 4 ? E_FRAME_SIZE : 0;
    case F_SETTINGS:
        if (sid) return E_PROTOCOL;
        return (fl & 1) ? (len ? E_FRAME_SIZE : 0) : (len % 6 ? E_FRAME_SIZE : 0);
    case F_PUSH:
        return E_PROTOCOL; /* push is disabled (SETTINGS_ENABLE_PUSH = 0) and never sent */
    case F_PING:
        return sid ? E_PROTOCOL : len != 8 ? E_FRAME_SIZE : 0;
    case F_GOAWAY:
        return sid ? E_PROTOCOL : len < 8 ? E_FRAME_SIZE : 0;
    case F_WINDOW:
        return len != 4 ? E_FRAME_SIZE : 0;
    case F_CONT:
        return h->hb_on ? 0 : E_PROTOCOL;
    default:
        return 0;
    }
}

/* Consumes received bytes (lock held). Returns 0 or an HTTP/2 connection error code. */
static int h2_feed(HConn* c, HSeg* seg, unsigned char* p, uint32_t n) {
    H2* h = c->h2;
    while (n) {
        if (h->preface_left) {
            uint32_t at = 24 - (uint32_t)h->preface_left;
            if (*p != (unsigned char)g_preface[at]) return E_PROTOCOL;
            h->preface_left--;
            p++;
            n--;
            continue;
        }
        if (h->fhn < 9) {
            h->fh[h->fhn++] = *p++;
            n--;
            if (h->fhn < 9) continue;
            int r = h2_frame_start(c);
            if (r) return r;
        } else {
            uint32_t m = h->flen - h->fgot;
            if (m > n) m = n;
            if (h->ftype == F_DATA) {
                uint32_t used = 0;
                if ((h->fflags & 8) && h->fgot == 0) {
                    h->pad = p[0];
                    if (h->pad >= h->flen) return E_PROTOCOL;
                    used = 1;
                }
                uint32_t data_end = h->flen - h->pad, pos = h->fgot + used;
                if (pos < data_end && used < m) {
                    uint32_t k = m - used < data_end - pos ? m - used : data_end - pos;
                    if (h->fex && !h->fex->reset && h2_deliver(c, h->fex, p + used, k, seg))
                        h2_stream_reset(c, h->fex, E_INTERNAL, SA_EOS, 1); /* OOM: fail the stream, visibly */
                    h->fdata += k;
                }
            } else if (h->ftype <= F_CONT) memcpy(h->fbuf + h->fgot, p, m);
            h->fgot += m;
            p += m;
            n -= m;
        }
        if (h->fhn == 9 && h->fgot == h->flen) {
            h->fhn = 0;
            int r = h2_frame_done(c);
            if (r) return r;
        }
    }
    return 0;
}

/* ---- write token: serializes socket writes; never held with the lock ------------------------ */
static void h2_flush(HConn* c, int release);
static int64_t h2_token_acquire(HConn* c) {
    sa_mutex_lock(&c->lock);
    while (c->wbusy) {
        if (c->dead) {
            int64_t e = c->dead_err;
            sa_mutex_unlock(&c->lock);
            return e;
        }
        SaWaiter w;
        sa_waiter_init(&w);
        sa_list_push(&c->ww_h, &c->ww_t, &w);
        sa_mutex_unlock(&c->lock);
        int fired = sa_wait(&w, WK_IO);
        sa_mutex_lock(&c->lock);
        if (!A_LOAD(&w.fired)) {
            sa_list_unlink(&c->ww_h, &c->ww_t, &w);
            if (!fired) {
                sa_mutex_unlock(&c->lock);
                return sa_cancelled_here();
            }
        }
    }
    if (c->dead) {
        int64_t e = c->dead_err;
        sa_mutex_unlock(&c->lock);
        return e;
    }
    c->wbusy = 1;
    sa_mutex_unlock(&c->lock);
    h2_flush(c, 0); /* queued control frames (preface, SETTINGS, ACKs) go out first */
    return SA_OK;
}

/* Token held: writes queued control frames; with `release`, also hands the token on. The empty
 * check and the release happen under one lock, so a frame queued meanwhile is never stranded. */
static void h2_flush(HConn* c, int release) {
    for (;;) {
        sa_mutex_lock(&c->lock);
        if (!c->cqn || A_LOAD(&c->timed_out)) {
            if (release) {
                c->wbusy = 0;
                SaWaiter* w = sa_list_pop(&c->ww_h, &c->ww_t);
                if (w) sa_waiter_fire(w);
            }
            sa_mutex_unlock(&c->lock);
            return;
        }
        uint32_t n = c->cqn;
        memcpy(c->cq2, c->cq, n);
        c->cqn = 0;
        sa_mutex_unlock(&c->lock);
        /* Shielded from the caller's cancellation: a cancelled task that frees its stream must
         * still get the RST_STREAM (and anything queued before it) onto the wire. */
        SaTask* self = sa_current_task();
        int32_t was = self ? __atomic_exchange_n(&self->cancel_req, 0, __ATOMIC_ACQ_REL) : 0;
        int64_t wr = h_io(c, 1, c->cq2, n, c->lim[HL_T_WRITE]);
        if (was) A_STORE(&self->cancel_req, 1);
        if (wr < 0) { /* the stream is broken: control frames can no longer be delivered */
            sa_mutex_lock(&c->lock);
            c->cqn = 0;
            if (!c->dead) {
                c->dead = 1;
                c->dead_err = wr;
            }
            h2_wake_all(c);
            sa_mutex_unlock(&c->lock);
            salivo_aio_close(c->sock);
        }
    }
}

static void h2_token_release(HConn* c) { h2_flush(c, 1); }

/* Flushes the control queue if nobody holds the token (the holder flushes otherwise). */
static void h2_kick(HConn* c) {
    sa_mutex_lock(&c->lock);
    int take = !c->wbusy && c->cqn;
    if (take) c->wbusy = 1;
    sa_mutex_unlock(&c->lock);
    if (take) h2_token_release(c);
}

/* Waits on *slot (or a list) with the lock held; returns with the lock held. */
static int64_t h_wait(HConn* c, SaWaiter** slot, SaWaiter** lh, SaWaiter** lt, int64_t ms) {
    SaWaiter w, tw;
    SaTimer tm;
    int armed = 0;
    sa_waiter_init(&w);
    if (slot) *slot = &w;
    else sa_list_push(lh, lt, &w);
    sa_mutex_unlock(&c->lock);
    if (ms > 0) {
        sa_waiter_init(&tw);
        armed = sa_timer_arm(c->obj.rt, &tm, &tw, ms);
    }
    int fired = sa_wait2(&w, armed ? &tw : NULL, WK_IO, 1);
    if (armed) sa_timer_disarm(c->obj.rt, &tm);
    sa_mutex_lock(&c->lock);
    if (A_LOAD(&w.fired)) return SA_OK;
    if (slot) {
        if (*slot == &w) *slot = NULL;
    } else sa_list_unlink(lh, lt, &w);
    if (armed && A_LOAD(&tw.fired)) return SA_ETIMEOUT;
    return fired ? SA_OK : sa_cancelled_here();
}

static int64_t h2_reader(int64_t arg) {
    HConn* c = (HConn*)(intptr_t)arg;
    H2* h = c->h2;
    int code = 0;
    int64_t err = SA_ECLOSED;
    h2_kick(c);
    for (;;) {
        if (c->rfill > c->rpos) {
            sa_mutex_lock(&c->lock);
            code = c->dead ? -1 : h2_feed(c, c->rseg, c->rseg->data + c->rpos, c->rfill - c->rpos);
            sa_mutex_unlock(&c->lock);
            c->rpos = c->rfill;
            if (code) break;
            h2_kick(c);
        }
        sa_mutex_lock(&c->lock);
        int64_t ms = h->ntab == 0 && h->naq == 0 ? c->lim[HL_T_IDLE] : 0;
        sa_mutex_unlock(&c->lock);
        int64_t r = h_fill(c, c->rpos, ms);
        if (r < 0) {
            err = r == SA_EEOF ? SA_ECLOSED : r;
            break;
        }
    }
    if (code > 0) {
        A_STORE(&c->reader_closes, 1); /* the reader flushes GOAWAY, lingers, then closes */
        h_die_ex(c, h2_local_err(code), code, 1);
    }
    else h_die(c, err, -1);
    sa_obj_unref(&c->obj);
    return 0;
}

/* Starts the HTTP/2 engine: queues our preface/SETTINGS and spawns the reader task. */
static int64_t h2_start(HConn* c) {
    H2* h = (H2*)calloc(1, sizeof(H2));
    if (!h) return SA_EOS;
    c->h2 = h;
    hp_huff_init();
    h->peer_frame = 16384;
    h->peer_initwin = 65535;
    h->peer_streams = 100;
    h->conn_send = 65535;
    h->conn_recv = 65535;
    h->next_sid = 1;
    h->dyn.max = 4096;
    h->preface_left = c->client ? 0 : 24;
    c->cq = (unsigned char*)malloc((size_t)c->lim[HL_CTLQ]);
    c->cq2 = (unsigned char*)malloc((size_t)c->lim[HL_CTLQ]);
    h->fbuf = (unsigned char*)malloc((size_t)c->lim[HL_FRAME]);
    h->hb = (unsigned char*)malloc((size_t)c->lim[HL_HDRBYTES]);
    h->dout = (unsigned char*)malloc((size_t)c->lim[HL_HLIST]);
    h->dstr = (unsigned char*)malloc((size_t)c->lim[HL_HDRBYTES] * 8 / 5 + 16);
    h->dfields = (HField*)malloc(sizeof(HField) * (size_t)(c->lim[HL_HDRCOUNT] + 8));
    if (!c->cq || !c->cq2 || !h->fbuf || !h->hb || !h->dout || !h->dstr || !h->dfields) return SA_EOS;
    sa_mutex_lock(&c->lock);
    if (c->client) {
        memcpy(c->cq, g_preface, 24);
        c->cqn = 24;
    }
    unsigned char s[36];
    uint32_t vals[6][2] = {{1, 4096}, {2, 0}, {3, (uint32_t)c->lim[HL_STREAMS]}, {4, (uint32_t)c->lim[HL_STREAMWIN]},
                           {5, (uint32_t)c->lim[HL_FRAME]}, {6, (uint32_t)c->lim[HL_HLIST]}};
    for (int i = 0; i < 6; i++) {
        s[i * 6] = 0;
        s[i * 6 + 1] = (unsigned char)vals[i][0];
        h_put32(s + i * 6 + 2, vals[i][1]);
    }
    h2_ctl(c, F_SETTINGS, 0, 0, s, 36);
    if (c->lim[HL_CONNWIN] > 65535) {
        h2_window(c, 0, (uint32_t)(c->lim[HL_CONNWIN] - 65535));
        h->conn_recv = c->lim[HL_CONNWIN];
    }
    sa_mutex_unlock(&c->lock);
    sa_obj_ref(&c->obj); /* the reader's reference */
    int64_t t = sa_spawn_in(c->obj.rt, (int64_t)(intptr_t)h2_reader, (int64_t)(intptr_t)c);
    if (t < 0) {
        sa_obj_unref(&c->obj);
        return t;
    }
    salivo_aio_release(t);
    return SA_OK;
}

/* Concurrent first requests on a client connection start the engine exactly once. */
static int64_t h2_start_once(HConn* c) {
    int32_t z = 0;
    if (A_CAS(&c->h2_starting, &z, 1)) {
        int64_t r = h2_start(c);
        if (r) h_die(c, r, -1);
        A_STORE(&c->h2_ready, 1);
        return r;
    }
    while (!A_LOAD(&c->h2_ready)) salivo_aio_yield();
    return c->dead ? c->dead_err : SA_OK;
}

static int64_t h2_next(HConn* c) {
    H2* h = c->h2;
    sa_mutex_lock(&c->lock);
    for (;;) {
        if (h->aq_h) {
            HEx* e = h->aq_h;
            h->aq_h = e->aq_next;
            if (!h->aq_h) h->aq_t = NULL;
            h->naq--;
            e->queued = 0;
            sa_mutex_unlock(&c->lock);
            int64_t hd = sa_tab_insert(c->obj.rt, &e->obj); /* the queue's reference becomes the handle's */
            if (!hd) sa_obj_unref(&e->obj);
            return hd ? hd : SA_EOS;
        }
        if (c->dead || h->goaway_sent || h->goaway_recv) {
            int64_t r = c->dead && c->dead_err != SA_ECLOSED && c->dead_err != SA_EEOF && !h->goaway_sent ? c->dead_err : SA_EEOF;
            sa_mutex_unlock(&c->lock);
            return r;
        }
        if (h->accept_w) {
            sa_mutex_unlock(&c->lock);
            return SA_EINVAL; /* one acceptor per connection */
        }
        int64_t r = h_wait(c, &h->accept_w, NULL, NULL, 0);
        if (r) {
            sa_mutex_unlock(&c->lock);
            return r;
        }
    }
}

/* Encodes the header block of `e` into wb2 (token held). */
static int64_t h2_encode(HConn* c, HEx* e, int64_t status, int64_t len, uint32_t* out) {
    size_t cap = (size_t)e->ohn + 16 * 8 + 64 + (e->mout ? strlen(e->mout) + strlen(e->tout) : 0) + 16 * (e->ohn / 2 + 1);
    if (cap > c->wb2cap) {
        unsigned char* p = (unsigned char*)realloc(c->wb2, cap);
        if (!p) return SA_EOS;
        c->wb2 = p;
        c->wb2cap = (uint32_t)cap;
    }
    unsigned char* p = c->wb2;
    uint32_t o = 0;
    const unsigned char* authority = (const unsigned char*)c->authority; /* a "host" field overrides */
    uint32_t alen = c->authority ? (uint32_t)strlen(c->authority) : 0;
    for (uint32_t i = 0; i < e->ohn;) {
        const unsigned char* nm = e->oh + i;
        uint32_t nl = (uint32_t)strlen((const char*)nm);
        if (h_ieq(nm, nl, "host")) {
            authority = nm + nl + 1;
            alen = (uint32_t)strlen((const char*)authority);
        }
        i += nl + 1 + (uint32_t)strlen((const char*)nm + nl + 1) + 1;
    }
    char num[24];
    if (e->client && strcmp(e->mout, "CONNECT") == 0) {
        /* RFC 9113 8.5: CONNECT carries :method and :authority (host:port, the request target) only */
        const char* t = e->tout && e->tout[0] && e->tout[0] != '/' ? e->tout : (const char*)authority;
        uint32_t tl = t == (const char*)authority ? alen : (uint32_t)strlen(t);
        if (!t || !tl || !h_valid_connect_authority((const unsigned char*)t, tl)) return SA_EINVAL;
        o += hp_enc_field(p + o, (const unsigned char*)":method", 7, (const unsigned char*)"CONNECT", 7);
        o += hp_enc_field(p + o, (const unsigned char*)":authority", 10, (const unsigned char*)t, tl);
    } else if (e->client) {
        o += hp_enc_field(p + o, (const unsigned char*)":method", 7, (const unsigned char*)e->mout, (uint32_t)strlen(e->mout));
        o += hp_enc_field(p + o, (const unsigned char*)":scheme", 7, (const unsigned char*)"http", 4);
        if (authority) o += hp_enc_field(p + o, (const unsigned char*)":authority", 10, authority, alen);
        o += hp_enc_field(p + o, (const unsigned char*)":path", 5, (const unsigned char*)e->tout, (uint32_t)strlen(e->tout));
    } else {
        snprintf(num, sizeof num, "%d", (int)status);
        o += hp_enc_field(p + o, (const unsigned char*)":status", 7, (const unsigned char*)num, 3);
    }
    for (uint32_t i = 0; i < e->ohn;) {
        const unsigned char* nm = e->oh + i;
        uint32_t nl = (uint32_t)strlen((const char*)nm);
        const unsigned char* v = nm + nl + 1;
        uint32_t vl = (uint32_t)strlen((const char*)v);
        i += nl + vl + 2;
        if (h_ieq(nm, nl, "host") || h_ieq(nm, nl, "keep-alive") || h_ieq(nm, nl, "proxy-connection") ||
            h_ieq(nm, nl, "upgrade") || (h_ieq(nm, nl, "te") && !h_ieq(v, vl, "trailers")))
            continue; /* connection-specific fields do not exist in HTTP/2 */
        o += hp_enc_field(p + o, nm, nl, v, vl);
    }
    if (len >= 0 && status != 204 && status != 205 && status != 304 && (len > 0 || !e->client)) {
        snprintf(num, sizeof num, "%lld", (long long)len);
        o += hp_enc_field(p + o, (const unsigned char*)"content-length", 14, (const unsigned char*)num, (uint32_t)strlen(num));
    }
    *out = o;
    return SA_OK;
}

static int64_t h2_err_of(HConn* c, HEx* e) {
    if (e->reset) return e->in_err ? e->in_err : HE_RESET;
    return c->dead_err ? c->dead_err : SA_ECLOSED;
}

static int64_t h2_send(HConn* c, HEx* e, int64_t status, int64_t len) {
    H2* h = c->h2;
    int interim = !e->client && status < 200, end;
    if (interim && status == 101) return SA_EINVAL;
    if (!e->client) e->out_nobody = e->is_head_req || status == 204 || status == 205 || status == 304;
    end = !interim && (e->out_nobody || len == 0);
    int64_t r;
    for (;;) {
        sa_mutex_lock(&c->lock);
        if (e->client) {
            while (!c->dead && !h->goaway_recv && h->ntab >= h->peer_streams) {
                r = h_wait(c, NULL, &h->sw_h, &h->sw_t, c->lim[HL_T_STREAM]);
                if (r) {
                    sa_mutex_unlock(&c->lock);
                    return r;
                }
            }
            if (c->dead || h->goaway_recv) {
                r = c->dead ? c->dead_err : HE_GOAWAY;
                sa_mutex_unlock(&c->lock);
                return r;
            }
        } else if (e->reset || !e->in_table || c->dead) {
            r = h2_err_of(c, e);
            sa_mutex_unlock(&c->lock);
            return r;
        }
        sa_mutex_unlock(&c->lock);
        if ((r = h2_token_acquire(c)) != SA_OK) return r;
        if (!e->client) break;
        /* stream ids go out in token order; the slot is re-checked now that it is ours */
        sa_mutex_lock(&c->lock);
        if (!c->dead && !h->goaway_recv && h->ntab < h->peer_streams) {
            if (h2_tab_add(h, e)) {
                sa_mutex_unlock(&c->lock);
                h2_token_release(c);
                return SA_EOS;
            }
            e->sid = h->next_sid;
            h->next_sid += 2;
            e->send_win = h->peer_initwin;
            e->recv_win = h2_stream_recvwin(c);
            HST(HS_STREAMS, 1);
            sa_mutex_unlock(&c->lock);
            break;
        }
        sa_mutex_unlock(&c->lock);
        h2_token_release(c);
    }
    if (end) {
        sa_mutex_lock(&c->lock);
        e->local_end = 1;
        h2_maybe_close(c, e);
        sa_mutex_unlock(&c->lock);
    }
    uint32_t blen = 0;
    r = h2_encode(c, e, status, interim ? -1 : len, &blen);
    if (!r) {
        uint32_t fmax = (uint32_t)h->peer_frame, nfr = blen / fmax + 1;
        unsigned char* w = h_wb(c, (size_t)blen + 9 * (size_t)nfr);
        if (!w) r = SA_EOS;
        else {
            uint32_t o = 0, done = 0;
            do {
                uint32_t k = blen - done < fmax ? blen - done : fmax;
                int first = done == 0, last = done + k == blen;
                h2_fhdr(w + o, k, first ? F_HEADERS : F_CONT, (uint8_t)((last ? 4 : 0) | (first && end ? 1 : 0)), e->sid);
                memcpy(w + o + 9, c->wb2 + done, k);
                o += 9 + k;
                done += k;
            } while (done < blen);
            HSTX(HS_SER_COPY, o);
            if (!e->client && !interim && !end && len > 0 && (e->ph = (unsigned char*)malloc(o)) != NULL) {
                memcpy(e->ph, w, o); /* goes out with the first DATA frame: one system call */
                e->phn = o;
            } else r = h_io(c, 1, w, o, c->lim[HL_T_WRITE]);
        }
    }
    h2_token_release(c);
    if (r < 0) {
        h_die(c, r, -1);
        return r;
    }
    if (!interim) {
        sa_mutex_lock(&c->lock);
        e->out_state = len == 0 ? 2 : 1;
        e->out_len = len;
        sa_mutex_unlock(&c->lock);
    }
    return SA_OK;
}

static int64_t h2_write(HConn* c, HEx* e, const unsigned char* p, int64_t n, int end) {
    H2* h = c->h2;
    do {
        sa_mutex_lock(&c->lock);
        int64_t avail;
        for (;;) {
            if (c->dead || e->reset) {
                int64_t r = h2_err_of(c, e);
                sa_mutex_unlock(&c->lock);
                return r;
            }
            if (e->local_end) {
                sa_mutex_unlock(&c->lock);
                return SA_EINVAL;
            }
            avail = h->conn_send < e->send_win ? h->conn_send : e->send_win;
            if (avail > h->peer_frame) avail = h->peer_frame;
            if (n == 0 || avail > 0) break; /* an empty END_STREAM frame needs no window */
            int64_t r = h_wait(c, &e->w, NULL, NULL, c->lim[HL_T_STREAM]);
            if (r) {
                sa_mutex_unlock(&c->lock);
                return r;
            }
        }
        /* An empty END_STREAM consumes no credit; never derive its length from a (possibly negative) window */
        uint32_t m = n == 0 ? 0 : (uint32_t)(n < avail ? n : avail);
        h->conn_send -= m;
        e->send_win -= m;
        int last = end && m == n;
        uint32_t sid = e->sid;
        if (last) { /* closed on our side once END_STREAM is sent: frees the slot for the peer */
            e->local_end = 1;
            h2_maybe_close(c, e);
        }
        sa_mutex_unlock(&c->lock);
        int64_t r = h2_token_acquire(c);
        if (r) return r;
        uint32_t hn = e->phn;
        unsigned char* w = h_wb(c, hn + 9 + (m <= H_SMALL ? m : 0));
        if (!w) r = SA_EOS;
        else {
            if (hn) { /* held-back HEADERS */
                memcpy(w, e->ph, hn);
                free(e->ph);
                e->ph = NULL;
                e->phn = 0;
            }
            h2_fhdr(w + hn, m, F_DATA, (uint8_t)(last ? 1 : 0), sid);
            if (m <= H_SMALL) {
                memcpy(w + hn + 9, p, m);
                HSTX(HS_SER_COPY, m);
                r = h_io(c, 1, w, hn + 9 + m, c->lim[HL_T_WRITE]);
            } else {
                HSTX(HS_WRITE_DIRECT, m);
                r = h_io2(c, w, hn + 9, p, m, c->lim[HL_T_WRITE]);
            }
        }
        h2_token_release(c);
        if (r < 0) {
            h_die(c, r, -1);
            return r;
        }
        p += m;
        n -= m;
    } while (n > 0);
    return SA_OK;
}

/* Next inbound DATA piece: copy (buf != NULL) or view (out). Returns bytes, SA_EEOF at the end. */
static int64_t h2_body(HConn* c, HEx* e, unsigned char* buf, int64_t cap, HView* out) {
    sa_mutex_lock(&c->lock);
    for (;;) {
        HChunk* k = e->dq_h;
        if (k) {
            uint32_t n;
            if (buf) {
                n = (uint32_t)(k->len < cap ? k->len : cap);
                memcpy(buf, k->p, n);
                HSTX(HS_BODY_COPY, n);
                k->p += n;
                k->len -= n;
                if (k->len == 0) {
                    e->dq_h = k->next;
                    if (!e->dq_h) e->dq_t = NULL;
                    hseg_unref(k->seg);
                    free(k);
                }
            } else {
                n = k->len;
                e->dq_h = k->next;
                if (!e->dq_h) e->dq_t = NULL;
                out->seg = k->seg;
                out->p = k->p;
                out->len = n;
                if (k->seg) free(k);
                else out->own = k;
                HSTX(HS_BODY_VIEW, n);
            }
            h2_consumed(c, e, n);
            sa_mutex_unlock(&c->lock);
            h2_kick(c);
            return n;
        }
        if (e->reset) {
            int64_t r = h2_err_of(c, e);
            sa_mutex_unlock(&c->lock);
            return r;
        }
        if (e->in_done || e->remote_end) {
            sa_mutex_unlock(&c->lock);
            return SA_EEOF;
        }
        if (c->dead) {
            int64_t r = c->dead_err ? c->dead_err : SA_ECLOSED;
            sa_mutex_unlock(&c->lock);
            return r;
        }
        int64_t r = h_wait(c, &e->w, NULL, NULL, c->lim[HL_T_STREAM]);
        if (r) {
            sa_mutex_unlock(&c->lock);
            return r;
        }
    }
}

/* ============================================================================================
 * Public ABI
 * ========================================================================================== */
static HConn* h_conn_get(int64_t h) { return (HConn*)sa_lookup(h, OT_HTTP_CONN); }
static HEx* h_ex_get(int64_t h) { return (HEx*)sa_lookup(h, OT_HTTP_EX); }

static int64_t h_conn_new(int64_t sock, int64_t proto, int client) {
    if (proto < 0 || proto > 2 || (client && proto == 0)) return SA_EINVAL;
    SaObj* so = sa_lookup(sock, OT_TCP_STREAM);
    if (!so) return SA_EHANDLE;
    SaRuntime* rt = so->rt;
    sa_obj_unref(so);
    if (HST(HS_CONNS, 1) > A_LOAD(&g_hmax_conns)) {
        HST(HS_CONNS, -1);
        return HE_REFUSED;
    }
    HConn* c = (HConn*)calloc(1, sizeof(HConn));
    if (!c) {
        HST(HS_CONNS, -1);
        return SA_EOS;
    }
    h_ctype_init();
    sa_obj_init(rt, &c->obj, OT_HTTP_CONN, 1, h_conn_destroy);
    sa_mutex_init(&c->lock);
    for (int i = 0; i < HL_COUNT; i++) c->lim[i] = A_LOAD(&g_hlim[i]);
    c->proto = (int32_t)proto;
    c->client = client;
    int64_t h = sa_tab_insert(rt, &c->obj);
    if (!h) {
        sa_obj_unref(&c->obj); /* sock is still 0: the caller keeps the stream */
        return SA_EOS;
    }
    c->self = h;
    c->sock = sock;
    return h;
}

int64_t salivo_aio_http_server(int64_t sock, int64_t proto) { return h_conn_new(sock, proto, 0); }
int64_t salivo_aio_http_client(int64_t sock, int64_t proto) { return h_conn_new(sock, proto, 1); }

int64_t salivo_aio_http_set(int64_t conn, int64_t key, int64_t v) {
    if (conn == 0 && key == 100) {
        if (v < 1) return SA_EINVAL;
        A_STORE(&g_hmax_conns, v);
        return SA_OK;
    }
    if (conn == 0 && key == 101) { /* instrumentation mode: copy/view/frame counters */
        if (v != 0 && v != 1) return SA_EINVAL;
        A_STORE(&g_hstats_on, (int32_t)v);
        return SA_OK;
    }
    if (key < 0 || key >= HL_COUNT || v < 0 || v > (1LL << 40)) return SA_EINVAL;
    if (key < HL_T_HEADER || key > HL_T_STREAM) {
        if (v < 1) return SA_EINVAL;
    }
    if (key == HL_FRAME && (v < 16384 || v > 16777215)) return SA_EINVAL;
    if ((key == HL_STREAMWIN || key == HL_CONNWIN) && v > 0x7fffffff) return SA_EINVAL;
    if (key == HL_CONNWIN && v < 65535) return SA_EINVAL;
    if (key == HL_CTLQ && (v < 1024 || v > (1 << 20))) return SA_EINVAL;
    if (key == HL_HDRBYTES && v > (1 << 24)) return SA_EINVAL;
    if (conn == 0) {
        A_STORE(&g_hlim[key], v);
        return SA_OK;
    }
    HConn* c = h_conn_get(conn);
    if (!c) return SA_EHANDLE;
    int64_t r = SA_OK;
    /* Keys that size connection buffers or are advertised in SETTINGS are fixed once HTTP/2
     * started; deadlines, body limits, drain and reset budgets apply from the next operation. */
    int fixed = key == HL_HDRCOUNT || key == HL_HDRBYTES || key == HL_FRAME || key == HL_HLIST ||
                key == HL_STREAMS || key == HL_STREAMWIN || key == HL_CONNWIN || key == HL_CTLQ;
    if (fixed && A_LOAD(&c->h2_starting)) r = SA_EINVAL;
    else A_STORE(&c->lim[key], v);
    sa_obj_unref(&c->obj);
    return r;
}

/* Server protocol detection: the HTTP/2 client preface, or HTTP/1.1. */
static int64_t h_detect(HConn* c) {
    for (;;) {
        uint32_t have = c->rfill - c->rpos;
        const unsigned char* b = c->rseg ? c->rseg->data + c->rpos : NULL;
        uint32_t k = 0;
        while (k < have && k < 24 && b[k] == (unsigned char)g_preface[k]) k++;
        if (k < have && k < 24) {
            c->proto = 1;
            return SA_OK;
        }
        if (k == 24) {
            c->proto = 2;
            return h2_start_once(c);
        }
        int64_t r = h_fill(c, c->rpos, c->lim[HL_T_IDLE]);
        if (r < 0) {
            h_die(c, r, -1);
            return r;
        }
    }
}

int64_t salivo_aio_http_next(int64_t conn) {
    HConn* c = h_conn_get(conn);
    if (!c) return SA_EHANDLE;
    int64_t r = sa_io_check(&c->obj);
    if (!r && c->client) r = SA_EINVAL;
    if (!r && c->proto != 2) {
        int32_t z = 0;
        if (!A_CAS(&c->busy, &z, 1)) r = SA_EINVAL; /* HTTP/1.1: one task drives a connection */
        else {
            if (c->dead) r = c->dead_err == SA_ECLOSED || c->dead_err == SA_EEOF ? SA_EEOF : c->dead_err;
            else if (c->proto == 0) r = h_detect(c);
            if (!r && c->proto == 1) r = h1_next(c);
            A_STORE(&c->busy, 0);
        }
    }
    if (!r && c->proto == 2 && !A_LOAD(&c->h2_ready)) r = h2_start_once(c);
    if (!r && c->proto == 2) r = h2_next(c);
    sa_obj_unref(&c->obj);
    return r;
}

int64_t salivo_aio_http_request(int64_t conn, const char* method, const char* target) {
    if (!method || !target || !*method || !*target) return SA_EINVAL;
    for (const char* p = method; *p; p++)
        if (!H_TCHAR(*p)) return SA_EINVAL;
    for (const char* p = target; *p; p++)
        if ((unsigned char)*p < 0x21 || (unsigned char)*p > 0x7e) return SA_EINVAL;
    HConn* c = h_conn_get(conn);
    if (!c) return SA_EHANDLE;
    int64_t r = SA_OK;
    if (!c->client || c->dead) r = c->dead ? c->dead_err : SA_EINVAL;
    else if (c->proto == 1 && c->cur && !(c->cur->in_done && c->cur->out_state == 2 && c->cur->head_ready)) r = SA_EINVAL;
    else if (c->proto == 1 && c->h1_close) r = SA_ECLOSED;
    /* HTTP/1.1 CONNECT tunnels are not implemented (the 2xx would switch the connection to raw bytes) */
    else if (c->proto == 1 && strcmp(method, "CONNECT") == 0) r = SA_EUNSUPPORTED;
    /* CONNECT needs an authority-form host:port target; reject it before a stream id is spent */
    else if (strcmp(method, "CONNECT") == 0 && !h_valid_connect_authority((const unsigned char*)target, (uint32_t)strlen(target))) r = SA_EINVAL;
    else if (c->proto == 2 && !A_LOAD(&c->h2_ready)) r = h2_start_once(c);
    if (r) {
        sa_obj_unref(&c->obj);
        return r;
    }
    HEx* e = h_ex_new(c, c->proto == 1 ? 2 : 1);
    if (!e) {
        sa_obj_unref(&c->obj);
        return SA_EOS;
    }
    e->mout = h_strdup((const unsigned char*)method, (uint32_t)strlen(method));
    e->tout = h_strdup((const unsigned char*)target, (uint32_t)strlen(target));
    e->is_head_req = strcmp(method, "HEAD") == 0;
    e->version = c->proto == 2 ? 20 : 11;
    if (c->proto == 1) {
        if (c->cur) sa_obj_unref(&c->cur->obj);
        c->cur = e;
    }
    int64_t h = sa_tab_insert(c->obj.rt, &e->obj);
    sa_obj_unref(&c->obj);
    if (!h) {
        sa_obj_unref(&e->obj);
        return SA_EOS;
    }
    return h;
}

int64_t salivo_aio_http_add_header(int64_t ex, const char* name, const char* value) {
    if (!name || !value || !*name) return SA_EINVAL;
    size_t nl = strlen(name), vl = strlen(value);
    for (size_t i = 0; i < nl; i++)
        if (!H_TCHAR(name[i])) return SA_EINVAL;
    for (size_t i = 0; i < vl; i++) /* CR / LF / NUL: header and response-splitting injection */
        if (value[i] == '\r' || value[i] == '\n' || ((unsigned char)value[i] < 0x20 && value[i] != '\t') || value[i] == 0x7f)
            return SA_EINVAL;
    if (h_ieq((const unsigned char*)name, (uint32_t)nl, "content-length") ||
        h_ieq((const unsigned char*)name, (uint32_t)nl, "transfer-encoding"))
        return SA_EINVAL; /* framing is owned by the transport (see rtaiohttpsend) */
    HEx* e = h_ex_get(ex);
    if (!e) return SA_EHANDLE;
    int64_t r = SA_OK;
    if (e->out_state) r = SA_EINVAL;
    else if (h_ieq((const unsigned char*)name, (uint32_t)nl, "connection")) {
        int close = 0, keep = 0;
        h_conn_tokens((const unsigned char*)value, (uint32_t)vl, &close, &keep);
        if (close) e->out_close = 1;
    } else if (e->ohn + nl + vl + 2 > (size_t)g_hlim[HL_HDRBYTES]) r = HE_HDRSIZE;
    else {
        if (e->ohn + nl + vl + 2 > e->ohcap) {
            uint32_t nc = (uint32_t)(e->ohcap * 2 + nl + vl + 64);
            unsigned char* p = (unsigned char*)realloc(e->oh, nc);
            if (!p) r = SA_EOS;
            else {
                e->oh = p;
                e->ohcap = nc;
            }
        }
        if (!r) {
            memcpy(e->oh + e->ohn, name, nl + 1); /* HTTP/2: hp_enc_str lowercases names on the wire */
            memcpy(e->oh + e->ohn + nl + 1, value, vl + 1);
            e->ohn += (uint32_t)(nl + vl + 2);
        }
    }
    sa_obj_unref(&e->obj);
    return r;
}

/* Runs `fn` with the exchange and its connection; maps a vanished connection to Closed. */
#define H_WITH(ex, body)                                        \
    HEx* e = h_ex_get(ex);                                      \
    if (!e) return SA_EHANDLE;                                  \
    HConn* c = h_conn_get(e->conn_h);                           \
    int64_t r;                                                  \
    if (!c) r = e->reset ? (e->in_err ? e->in_err : HE_RESET) : SA_ECLOSED; \
    else if ((r = sa_io_check(&c->obj)) == SA_OK) {             \
        body                                                    \
    }                                                           \
    if (c) sa_obj_unref(&c->obj);                               \
    sa_obj_unref(&e->obj);                                      \
    return r;

static int h1_mine(HConn* c, HEx* e) { return c->proto == 1 && c->cur == e && !c->dead; }

int64_t salivo_aio_http_send(int64_t ex, int64_t status, int64_t len) {
    if (len < -1) return SA_EINVAL;
    H_WITH(ex, {
        if (e->out_state || (e->client ? status != 0 : (status < 100 || status > 599))) r = SA_EINVAL;
        else if (e->proto == 2) r = h2_send(c, e, status, len);
        else if (!h1_mine(c, e)) r = c->dead ? c->dead_err : SA_EINVAL;
        else r = h1_send(c, e, status, len);
    })
}

static int64_t h_write(HConn* c, HEx* e, const unsigned char* p, int64_t n) {
    if (e->out_state != 1 || n < 0) return SA_EINVAL;
    if (e->out_nobody) return SA_OK; /* HEAD / 204 / 205 / 304: the body is not sent */
    if (e->out_len >= 0 && e->out_sent + n > e->out_len) return SA_EINVAL;
    if (n == 0) return SA_OK;
    int64_t r;
    if (e->proto == 2) r = h2_write(c, e, p, n, e->out_len >= 0 && e->out_sent + n == e->out_len);
    else r = h1_mine(c, e) ? h1_write(c, e, p, n) : (c->dead ? c->dead_err : SA_EINVAL);
    if (!r) e->out_sent += n;
    return r;
}

int64_t salivo_aio_http_write(int64_t ex, int64_t buf, int64_t len) {
    if (len < 0 || (len > 0 && buf == 0)) return SA_EINVAL;
    H_WITH(ex, { r = h_write(c, e, (const unsigned char*)(intptr_t)buf, len); })
}

int64_t salivo_aio_http_write_text(int64_t ex, const char* text) {
    if (!text) return SA_EINVAL;
    return salivo_aio_http_write(ex, (int64_t)(intptr_t)text, (int64_t)strlen(text));
}

int64_t salivo_aio_http_finish(int64_t ex) {
    H_WITH(ex, {
        if (e->out_state != 1) r = e->out_state == 2 ? SA_OK : SA_EINVAL;
        else if (e->proto == 2) {
            if (e->local_end) {
                e->out_state = 2;
                r = SA_OK;
            } else if (e->out_len >= 0 && e->out_sent != e->out_len) {
                sa_mutex_lock(&c->lock);
                h2_stream_reset(c, e, E_INTERNAL, HE_FRAMING, 1);
                sa_mutex_unlock(&c->lock);
                h2_kick(c);
                r = HE_FRAMING;
            } else {
                r = h2_write(c, e, NULL, 0, 1);
                if (!r) e->out_state = 2;
            }
        } else r = h1_mine(c, e) ? h1_finish(c, e) : (c->dead ? c->dead_err : SA_EINVAL);
    })
}

int64_t salivo_aio_http_response(int64_t ex) {
    H_WITH(ex, {
        if (!e->client || !e->out_state) r = SA_EINVAL;
        else if (e->head_ready) r = e->status;
        else if (e->proto == 1) r = h1_mine(c, e) ? h1_response(c, e) : (c->dead ? c->dead_err : SA_EINVAL);
        else {
            sa_mutex_lock(&c->lock);
            for (;;) {
                if (e->head_ready) {
                    r = e->status;
                    break;
                }
                if (e->reset || c->dead) {
                    r = h2_err_of(c, e);
                    break;
                }
                if ((r = h_wait(c, &e->w, NULL, NULL, c->lim[HL_T_STREAM])) != SA_OK) break;
            }
            sa_mutex_unlock(&c->lock);
        }
    })
}

static int64_t h_body_common(HConn* c, HEx* e, unsigned char* buf, int64_t cap, HView* out) {
    if (!e->head_ready || (e->client && !e->out_state)) return SA_EINVAL;
    if (e->proto == 2) return h2_body(c, e, buf, cap, out);
    if (!h1_mine(c, e)) return c->dead ? c->dead_err : SA_EINVAL;
    if (!e->client && e->expect100 && !e->out_state) { /* the client waits for this before sending */
        e->expect100 = 0;
        int64_t r = h_io(c, 1, (unsigned char*)"HTTP/1.1 100 Continue\r\n\r\n", 25, c->lim[HL_T_WRITE]);
        if (r < 0) return r;
    }
    HView v;
    memset(&v, 0, sizeof v);
    int64_t r = h1_body(c, e, (uint32_t)(cap < 0x7fffffff ? cap : 0x7fffffff), &v);
    if (r < 0) {
        e->in_err = r;
        c->h1_close = 1;
        return r;
    }
    if (r == 0) return SA_EEOF;
    if (buf) {
        memcpy(buf, v.p, v.len);
        HSTX(HS_BODY_COPY, v.len);
        h_view_drop(&v);
    } else {
        *out = v;
        HSTX(HS_BODY_VIEW, v.len);
    }
    return r;
}

int64_t salivo_aio_http_read(int64_t ex, int64_t buf, int64_t cap) {
    if (buf == 0 || cap <= 0) return SA_EINVAL;
    H_WITH(ex, { r = h_body_common(c, e, (unsigned char*)(intptr_t)buf, cap, NULL); })
}

int64_t salivo_aio_http_next_chunk(int64_t ex) {
    H_WITH(ex, {
        h_view_drop(&e->cur);
        r = h_body_common(c, e, NULL, 1 << 30, &e->cur);
    })
}

int64_t salivo_aio_http_chunk_byte(int64_t ex, int64_t i) {
    HEx* e = h_ex_get(ex);
    if (!e) return SA_EHANDLE;
    int64_t r = i >= 0 && i < e->cur.len ? e->cur.p[i] : SA_EINVAL;
    sa_obj_unref(&e->obj);
    return r;
}

static char* h_empty(void) { return h_strdup((const unsigned char*)"", 0); }

char* salivo_aio_http_text_str(int64_t ex, int64_t key) {
    HEx* e = h_ex_get(ex);
    if (!e) return h_empty();
    const unsigned char* p = NULL;
    uint32_t n = 0;
    const char* lit = NULL;
    int pk = key == 0 ? PM_METHOD : key <= 3 ? PM_TARGET : key == 4 ? PM_AUTH : key == 5 ? PM_SCHEME : key == 6 ? PM_REASON : -1;
    if (e->client && key <= 3) {
        const char* s = key == 0 ? e->mout : e->tout;
        p = (const unsigned char*)s;
        n = s ? (uint32_t)strlen(s) : 0;
    } else if (pk >= 0 && e->pm[pk].nlen) {
        p = e->hbase + e->pm[pk].voff;
        n = e->pm[pk].vlen;
    } else if (key == 4 && e->head_ready) {
        HField* f = h_find(e, (const unsigned char*)"host", 4, 0);
        if (f) {
            p = e->hbase + f->voff;
            n = f->vlen;
        }
    } else if (key == 5) lit = "http";
    if (key == 7) lit = e->version == 20 ? "HTTP/2" : e->version == 10 ? "HTTP/1.0" : "HTTP/1.1";
    if ((key == 2 || key == 3) && p && e->tform == 1) { /* absolute-form: drop scheme://authority */
        const unsigned char* s = (const unsigned char*)memchr(p, ':', n) + 3;
        const unsigned char* slash = (const unsigned char*)memchr(s, '/', n - (uint32_t)(s - p));
        if (slash) {
            n -= (uint32_t)(slash - p);
            p = slash;
        } else if (key == 2) lit = "/";
        else n = 0;
    }
    if (!lit && (key == 2 || key == 3)) {
        const unsigned char* q = p ? (const unsigned char*)memchr(p, '?', n) : NULL;
        if (key == 2 && q) n = (uint32_t)(q - p);
        if (key == 3) {
            if (q) {
                n -= (uint32_t)(q + 1 - p);
                p = q + 1;
            } else n = 0;
        }
    }
    char* s = lit ? h_strdup((const unsigned char*)lit, (uint32_t)strlen(lit)) : h_strdup(p, p ? n : 0);
    sa_obj_unref(&e->obj);
    return s;
}

char* salivo_aio_http_header_str(int64_t ex, const char* name, int64_t idx) {
    HEx* e = h_ex_get(ex);
    if (!e) return h_empty();
    HField* f = name && e->head_ready ? h_find(e, (const unsigned char*)name, (uint32_t)strlen(name), (int32_t)idx) : NULL;
    char* s = f ? h_strdup(e->hbase + f->voff, f->vlen) : h_empty();
    sa_obj_unref(&e->obj);
    return s;
}

int64_t salivo_aio_http_header_count(int64_t ex, const char* name) {
    HEx* e = h_ex_get(ex);
    if (!e) return SA_EHANDLE;
    int64_t n = 0;
    if (!e->head_ready) n = 0;
    else if (!name || !*name) n = e->nf;
    else
        while (h_find(e, (const unsigned char*)name, (uint32_t)strlen(name), (int32_t)n)) n++;
    sa_obj_unref(&e->obj);
    return n;
}

char* salivo_aio_http_header_at_str(int64_t ex, int64_t i, int64_t part) {
    HEx* e = h_ex_get(ex);
    if (!e) return h_empty();
    char* s;
    if (!e->head_ready || i < 0 || i >= e->nf) s = h_empty();
    else {
        HField* f = &e->f[i];
        s = part ? h_strdup(e->hbase + f->voff, f->vlen) : h_strdup(e->hbase + f->noff, f->nlen);
    }
    sa_obj_unref(&e->obj);
    return s;
}

char* salivo_aio_http_trailer_str(int64_t ex, const char* name) {
    HEx* e = h_ex_get(ex);
    if (!e) return h_empty();
    char* s = NULL;
    for (int32_t i = 0; name && e->in_done && i < e->ntf && !s; i++) {
        HField* f = &e->tf[i];
        const unsigned char* a = e->tarena + f->noff;
        uint32_t k = 0, n = (uint32_t)strlen(name);
        if (f->nlen != n) continue;
        while (k < n && h_lc(a[k]) == h_lc((unsigned char)name[k])) k++;
        if (k == n) s = h_strdup(e->tarena + f->voff, f->vlen);
    }
    sa_obj_unref(&e->obj);
    return s ? s : h_empty();
}

int64_t salivo_aio_http_info(int64_t h, int64_t key) {
    SaObj* o = sa_lookup(h, 0);
    if (!o) return SA_EHANDLE;
    int64_t r = SA_EINVAL;
    if (o->type == OT_HTTP_EX) {
        HEx* e = (HEx*)o;
        switch (key) {
        case 0: r = e->version; break;
        case 1: r = e->tform; break;
        case 2: r = e->status; break;
        case 3: r = e->clen; break;
        case 4: r = e->head_ready ? e->nf : 0; break;
        case 5: r = e->keepalive; break;
        case 6: r = e->sid; break;
        case 7: r = e->in_done; break;
        case 8: r = e->proto == 2 ? e->data_total : e->btotal; break;
        case 9: r = e->send_win; break;
        case 10: r = e->reset_code; break;
        case 11: r = e->in_done ? e->ntf : 0; break;
        case 12: r = e->proto; break;
        case 13: r = e->out_state; break;
        }
    } else if (o->type == OT_HTTP_CONN) {
        HConn* c = (HConn*)o;
        H2* h2 = c->h2;
        switch (key) {
        case 0: r = c->proto; break;
        case 1: r = h2 ? h2->ntab : (c->cur && c->cur->out_state != 2 && !c->dead); break;
        case 2: r = c->served; break;
        case 3: r = h2 ? h2->peer_streams : 1; break;
        case 4: r = h2 ? h2->goaway_recv : 0; break;
        case 5: r = c->dead ? c->dead_err : 0; break;
        case 6: r = h2 ? h2->conn_send : 0; break;
        case 7: r = h2 ? h2->conn_recv : 0; break;
        case 8: r = h2 ? h2->ping_rtt : 0; break;
        case 9: r = h2 ? h2->goaway_sent : 0; break;
        case 10: r = h2 ? h2->naq : 0; break;
        case 11: r = h2 ? h2->peer_initwin : 0; break;
        case 12: r = h2 ? h2->peer_frame : 0; break;
        }
    }
    sa_obj_unref(o);
    return r;
}

int64_t salivo_aio_http_reset(int64_t ex, int64_t code) {
    if (code < 0 || code > 0xffffffffLL) return SA_EINVAL;
    H_WITH(ex, {
        r = SA_OK;
        if (e->proto == 2) {
            sa_mutex_lock(&c->lock);
            if (e->in_table) h2_stream_reset(c, e, (uint32_t)code, SA_ECANCELLED, 1);
            else e->reset = 1;
            sa_mutex_unlock(&c->lock);
            h2_kick(c);
        } else if (c->proto == 1 && c->cur == e) h_die(c, SA_ECANCELLED, -1); /* HTTP/1.1: abort = close */
    })
}

int64_t salivo_aio_http_free(int64_t ex) {
    HEx* e = (HEx*)sa_tab_remove(ex, OT_HTTP_EX);
    if (!e) return SA_EHANDLE;
    HConn* c = h_conn_get(e->conn_h);
    if (c) {
        if (e->proto == 2 && c->h2) {
            sa_mutex_lock(&c->lock);
            if (e->in_table) h2_stream_reset(c, e, e->local_end ? E_NO : E_CANCEL, SA_ECANCELLED, 1);
            sa_mutex_unlock(&c->lock);
            h2_kick(c);
        } else if (c->proto == 1 && c->cur == e && (e->out_state == 1 || (e->client && e->out_state && !e->in_done)))
            h_die(c, SA_ECANCELLED, -1); /* an unfinished HTTP/1.1 message leaves the stream unusable */
        sa_obj_unref(&c->obj);
    }
    sa_obj_unref(&e->obj);
    return SA_OK;
}

int64_t salivo_aio_http_goaway(int64_t conn, int64_t code) {
    if (code < 0 || code > 0xffffffffLL) return SA_EINVAL;
    HConn* c = h_conn_get(conn);
    if (!c) return SA_EHANDLE;
    int64_t r = SA_OK;
    if (!c->h2) {
        c->h1_close = 1; /* HTTP/1.1: finish the current exchange, then end the connection */
    } else {
        unsigned char g[8];
        sa_mutex_lock(&c->lock);
        if (!c->h2->goaway_sent && !c->dead) {
            h_put32(g, c->client ? 0 : c->h2->last_peer_sid);
            h_put32(g + 4, (uint32_t)code);
            h2_ctl(c, F_GOAWAY, 0, 0, g, 8);
            c->h2->goaway_sent = 1;
            h_fire(&c->h2->accept_w);
        }
        sa_mutex_unlock(&c->lock);
        h2_kick(c);
    }
    sa_obj_unref(&c->obj);
    return r;
}

int64_t salivo_aio_http_ping(int64_t conn) {
    HConn* c = h_conn_get(conn);
    if (!c) return SA_EHANDLE;
    int64_t r = sa_io_check(&c->obj);
    if (!r && c->client && c->proto == 2 && !A_LOAD(&c->h2_ready)) r = h2_start_once(c);
    if (!r && !c->h2) r = SA_EUNSUPPORTED;
    if (!r) {
        H2* h = c->h2;
        sa_mutex_lock(&c->lock);
        if (h->ping_out || h->ping_w || c->dead) r = c->dead ? c->dead_err : SA_EINVAL;
        else {
            unsigned char b[8];
            h->ping_seq++;
            h_put32(b, (uint32_t)(h->ping_seq >> 32));
            h_put32(b + 4, (uint32_t)h->ping_seq);
            h->ping_ns = sa_now_ns();
            h->ping_out = 1;
            if (h2_ctl(c, F_PING, 0, 0, b, 8)) r = SA_EFULL;
        }
        sa_mutex_unlock(&c->lock);
        if (!r) {
            h2_kick(c);
            sa_mutex_lock(&c->lock);
            while (h->ping_out && !c->dead && !r) r = h_wait(c, &h->ping_w, NULL, NULL, c->lim[HL_T_STREAM]);
            if (!r) r = c->dead ? c->dead_err : h->ping_rtt;
            h->ping_out = 0;
            sa_mutex_unlock(&c->lock);
        }
    }
    sa_obj_unref(&c->obj);
    return r;
}

int64_t salivo_aio_http_close(int64_t conn) {
    HConn* c = (HConn*)sa_tab_remove(conn, OT_HTTP_CONN);
    if (!c) return SA_EHANDLE;
    if (c->h2) h2_kick(c); /* best effort: flush a queued GOAWAY */
    if (!A_LOAD(&c->reader_closes)) h_die(c, SA_ECLOSED, -1);
    sa_obj_unref(&c->obj);
    return SA_OK;
}

int64_t salivo_aio_http_stat(int64_t key) {
    if (key < 0 || key >= HS_COUNT) return SA_EINVAL;
    return A_LOAD(&g_hst[key]);
}

/* Default authority of a client connection: the Host of HTTP/1.1 requests and the :authority of
 * HTTP/2 requests that carry no "host" field. */
int64_t salivo_aio_http_authority(int64_t conn, const char* text) {
    if (!text || !*text) return SA_EINVAL;
    for (const char* p = text; *p; p++)
        if ((unsigned char)*p <= 0x20 || (unsigned char)*p >= 0x7f) return SA_EINVAL;
    HConn* c = h_conn_get(conn);
    if (!c) return SA_EHANDLE;
    int64_t r = SA_OK;
    char* copy = c->client ? h_strdup((const unsigned char*)text, (uint32_t)strlen(text)) : NULL;
    if (!copy) r = c->client ? SA_EOS : SA_EINVAL;
    else {
        sa_mutex_lock(&c->lock);
        char* old = c->authority;
        c->authority = copy;
        sa_mutex_unlock(&c->lock);
        free(old);
    }
    sa_obj_unref(&c->obj);
    return r;
}

/* Releases a handle owned by a task that was cancelled before it ever ran (rtaiospawnowning). */
static void sa_release_owned(int64_t h) {
    SaObj* o = sa_lookup(h, 0);
    if (!o) return;
    int type = o->type;
    sa_obj_unref(o);
    if (type == OT_HTTP_EX) salivo_aio_http_free(h);
    else if (type == OT_HTTP_CONN) salivo_aio_http_close(h);
    else if (type == OT_TASK) {
        salivo_aio_cancel(h);
        salivo_aio_release(h);
    } else salivo_aio_close(h);
}

/* Stage 36.3: textual IP address ("127.0.0.1", "::1") of the peer of an exchange's or a
 * connection's socket, for rate limiting and logging; "" when unknown. */
char* salivo_aio_http_peer_str(int64_t h) {
    char text[SA_PEER_TEXT] = "";
    SaObj* o = sa_lookup(h, 0);
    if (o) {
        int64_t ch = o->type == OT_HTTP_EX ? ((HEx*)o)->conn_h : (o->type == OT_HTTP_CONN ? h : 0);
        HConn* c = ch ? (HConn*)sa_lookup(ch, OT_HTTP_CONN) : NULL;
        SaSock* k = c && c->sock ? (SaSock*)sa_lookup(c->sock, OT_TCP_STREAM) : NULL;
        if (k) {
            struct sockaddr_storage ss;
            memset(&ss, 0, sizeof(ss));
#if defined(_WIN32)
            int sl = (int)sizeof(ss);
            int got = !k->closed && getpeername(k->s, (struct sockaddr*)&ss, &sl) == 0;
#else
            socklen_t sl = (socklen_t)sizeof(ss);
            int got = !k->closed && getpeername(k->fd, (struct sockaddr*)&ss, &sl) == 0;
#endif
            if (got && ss.ss_family == AF_INET6)
                inet_ntop(AF_INET6, (void*)&((struct sockaddr_in6*)&ss)->sin6_addr, text, sizeof(text));
            else if (got && ss.ss_family == AF_INET)
                inet_ntop(AF_INET, (void*)&((struct sockaddr_in*)&ss)->sin_addr, text, sizeof(text));
            sa_obj_unref(&k->obj);
        }
        if (c) sa_obj_unref(&c->obj);
        sa_obj_unref(o);
    }
    return h_strdup((const unsigned char*)text, strlen(text));
}
