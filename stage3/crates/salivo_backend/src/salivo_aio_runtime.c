/* Salivo Stage 36.1 - multi-threaded async I/O runtime.
 *
 * Canonical ABI: salivo_aio_abi.def (every exported function below is listed there; the Stage 36.1
 * gate script checks the two agree). Design notes: docs/STAGE_36_1_RUNTIME.md.
 *
 * Layering
 *   salivo_aio_sys_<os>.inc   locks, threads, time, fibers, TLS      (platform)
 *   this file                 scheduler, tasks, timers, channels,    (portable core)
 *                             blocking pool, handle table, sysmon,
 *                             runtime registry, lifecycle
 *   salivo_aio_io_<os>.inc    reactor, TCP, UDP, files               (platform)
 *
 * Backends: Windows (IOCP + fibers) and POSIX (epoll on Linux, poll(2) elsewhere, ucontext
 * fibers). Any other target compiles salivo_aio_unsupported.inc (every call fails Unsupported).
 *
 * Execution model: every task runs on its own fiber (stackful coroutine). N worker threads run
 * fibers from per-worker queues plus a global injection queue, and idle workers steal. A task that
 * waits (I/O, timer, channel, join, blocking pool) registers a waiter and switches back to its
 * worker's scheduler fiber, so the worker runs other tasks. The waiter's source later wakes it.
 * A per-runtime monitor thread (sysmon) asks a task that has held its worker longer than the time
 * slice to yield at its next safepoint (compiler-inserted at loop back-edges).
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hook the legacy runtime calls before a blocking primitive (sleep, blocking channel, mutex, join).
 * Defined in salivo_task_runtime.c, which is always linked. */
extern void (*salivo_aio_blocking_hook)(const char* what);

#if defined(_WIN32)
#include "salivo_aio_sys_win32.inc"
#define SA_HAVE_BACKEND 1
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include "salivo_aio_sys_posix.inc"
#define SA_HAVE_BACKEND 1
#endif

#ifdef SA_HAVE_BACKEND

/* ============================================================================================
 * Errors (mirror the @errors line of salivo_aio_abi.def)
 * ========================================================================================== */
enum {
    SA_OK = 0,
    SA_EWOULDBLOCK = -1,
    SA_EEOF = -2,
    SA_ERESET = -3,
    SA_ETIMEOUT = -4,
    SA_ECANCELLED = -5,
    SA_ECLOSED = -6,
    SA_EINVAL = -7,
    SA_EOS = -8,
    SA_ESHUTDOWN = -9,
    SA_EHANDLE = -10,
    SA_EFULL = -11,
    SA_EUNSUPPORTED = -12,
    SA_ENOTASK = -13,
    SA_EREFUSED = -14,
    SA_EADDRINUSE = -15,
    SA_ETRUNC = -16,
    SA_EFAILED = -17
};

#define SA_ABI_VERSION 36200

/* ============================================================================================
 * Atomics. Relaxed counters use RELAXED; state machines use ACQ_REL; the two sleep/wake
 * handshakes (idle workers, sysmon) need SEQ_CST on both sides (store-load ordering).
 * ========================================================================================== */
#define A_LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define A_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define A_ADD(p, v) __atomic_add_fetch((p), (v), __ATOMIC_ACQ_REL)
#define A_SUB(p, v) __atomic_sub_fetch((p), (v), __ATOMIC_ACQ_REL)
#define A_INC(p) __atomic_add_fetch((p), 1, __ATOMIC_RELAXED)
#define A_DEC(p) __atomic_sub_fetch((p), 1, __ATOMIC_RELAXED)
#define A_CAS(p, e, d) __atomic_compare_exchange_n((p), (e), (d), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
#define A_LOAD_SC(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define A_STORE_SC(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define A_ADD_SC(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)

/* ============================================================================================
 * Handles
 *
 * Object handle  = inc << 49 | rt_slot << 44 | gen << 20 | (slot + 1)
 *   slot     20 bits  index in the owning runtime's handle table
 *   gen      24 bits  bumped whenever the table slot is reused (stale-handle / ABA protection)
 *   rt_slot   5 bits  runtime registry slot
 *   inc      13 bits  incarnation of that registry slot (a destroyed runtime's handles never
 *                     resolve into a later runtime that reuses the slot)
 * Runtime handle = inc << 16 | rt_slot << 8 | 0xA1
 * ========================================================================================== */
#define SA_MAX_RUNTIMES 32
#define SA_MAX_SLOTS 0xFFFFE
#define H_MAKE(inc, rts, gen, slot) \
    (((int64_t)(inc) << 49) | ((int64_t)(rts) << 44) | ((int64_t)(gen) << 20) | (int64_t)((slot) + 1))
#define H_SLOT(h) ((int32_t)((h) & 0xFFFFF) - 1)
#define H_GEN(h) ((uint32_t)(((h) >> 20) & 0xFFFFFF))
#define H_RTS(h) ((int)(((h) >> 44) & 31))
#define H_INC(h) ((uint32_t)(((h) >> 49) & 0x1FFF))
#define RT_HANDLE(inc, rts) (((int64_t)(inc) << 16) | ((int64_t)(rts) << 8) | 0xA1)

enum { OT_TASK = 1, OT_CHAN, OT_TCP_LISTENER, OT_TCP_STREAM, OT_UDP, OT_FILE, OT_HTTP_CONN, OT_HTTP_EX };

struct SaRuntime;

typedef struct SaObj {
    int32_t type;
    int32_t pad;
    int64_t refs; /* atomic; the object is destroyed when it reaches 0 */
    struct SaRuntime* rt;
    void (*destroy)(struct SaObj*);
} SaObj;

typedef struct {
    SaObj* obj;
    uint32_t gen;
    int32_t next_free;
} SaSlot;

/* ============================================================================================
 * Waiters: the one wake primitive shared by timers, channels, joins, I/O and the blocking pool.
 *
 * A waiter belongs either to a task (woken by rescheduling it) or to a plain OS thread (woken
 * through that thread's parker). Memory rule: every waker fires a waiter while holding the lock
 * that protects it, or while holding a reference to the task, and the waiting side always
 * re-acquires that lock (or waits for `done`) before its frame ends. Therefore no wake ever
 * touches a finished frame or a freed task.
 * ========================================================================================== */
typedef struct SaParker {
    sa_mutex m;
    sa_cond c;
    int32_t inited;
} SaParker;

struct SaTask;

typedef struct SaWaiter {
    struct SaTask* task;
    SaParker* parker;
    int32_t fired; /* atomic */
    int32_t pad;
    int64_t value;
    int64_t status;
    struct SaWaiter* prev;
    struct SaWaiter* next;
} SaWaiter;

/* ============================================================================================
 * Tasks
 * ========================================================================================== */
enum { TS_CREATED = 0, TS_RUNNABLE, TS_RUNNING, TS_WAITING, TS_COMPLETED, TS_CANCELLED, TS_FAILED };
/* Scheduling word: exactly one owner may move a task into a queue (IDLE->QUEUED). */
enum { SW_IDLE = 0, SW_QUEUED, SW_RUNNING, SW_NOTIFIED, SW_DONE };
enum { EXIT_NONE = 0, EXIT_YIELD, EXIT_PARK, EXIT_DONE };
enum { WK_NONE = 0, WK_IO, WK_TIMER, WK_CHANNEL, WK_JOIN, WK_BLOCKING };

typedef int64_t (*sa_task_fn)(int64_t);

#define SA_PEER_TEXT 64

typedef struct SaTask {
    SaObj obj;
    int64_t handle;
    int32_t state;           /* atomic, TS_* */
    int32_t sched;           /* atomic, SW_* */
    int32_t cancel_req;      /* atomic */
    int32_t cancel_observed; /* written by the task itself */
    int32_t exit_reason;
    int32_t wait_kind;
    int32_t started;
    int32_t fail_set;
    int32_t running_on; /* atomic; worker id or -1; proves no two workers run one task */
    int32_t budget;
    int32_t owns_arg; /* arg is a handle the task owns: released if the task never runs */
    sa_task_fn fn;
    int64_t arg;
    int64_t result;
    int64_t fail_code;
    int64_t last_value;
    int64_t last_os_error;
    char* last_text;                 /* owned; handed out by rtaiolasttext */
    char last_peer[SA_PEER_TEXT];    /* text address of the last datagram's sender */
    void* fiber;
    sa_mutex lock;     /* protects joiners and publication of the terminal state */
    SaWaiter* joiners; /* doubly linked */
    struct SaTask* qnext;               /* run-queue link (a task is in at most one queue) */
    struct SaTask *all_prev, *all_next; /* runtime's live-task list */
} SaTask;

#define SA_BUDGET 128 /* ready operations a task may complete before it is forced to yield */

/* ============================================================================================
 * Timers
 * ========================================================================================== */
typedef struct SaTimer {
    int64_t deadline_ns;
    uint64_t seq; /* FIFO order between equal deadlines */
    int64_t idx;  /* heap index, -1 when not armed */
    SaWaiter* w;
    void (*cb)(struct SaTimer*); /* fired instead of w (under the timer lock) when set */
} SaTimer;

/* ============================================================================================
 * Blocking pool jobs
 * ========================================================================================== */
enum { JOB_QUEUED = 0, JOB_RUNNING, JOB_DONE };

typedef struct SaJob {
    int64_t (*run)(struct SaJob*);
    int64_t a, b, c;
    void* p;
    const void* q;
    int64_t result;
    int64_t os_error;
    int32_t state;
    int32_t pad;
    SaWaiter w;
    struct SaJob* next;
} SaJob;

/* ============================================================================================
 * Runtime
 * ========================================================================================== */
#define SA_FIBER_POOL 64

typedef struct SaWorker {
    struct SaRuntime* rt;
    int32_t id;
    sa_thread th;
    void* sched_fiber;
    SaTask* current;
    sa_mutex qlock;
    SaTask *qhead, *qtail;
    int64_t qlen; /* atomic (read without the lock by stealers) */
    void* fibers[SA_FIBER_POOL];
    int32_t nfibers;
    uint32_t tick;
    uint64_t rng;
    int64_t running_since_ns;  /* atomic; 0 while in the scheduler */
    int64_t preempt;           /* atomic; set by sysmon, consumed at the task's next safepoint */
    int64_t preempted_since;   /* sysmon: slice start already asked to yield */
    int64_t reported_since_ns; /* sysmon: slice start already reported as blocking */
    int64_t runs;              /* atomic */
    int64_t busy_ns;           /* atomic */
} SaWorker;

enum { RT_CREATED = 0, RT_STARTED, RT_JOINED };

typedef struct SaRuntime {
    int64_t handle;
    int32_t slot; /* registry slot */
    uint32_t inc; /* registry incarnation */
    int32_t state;
    int32_t nworkers;
    SaWorker* workers;
    size_t stack_size;

    sa_mutex gq_lock; /* global injection queue */
    SaTask *gq_head, *gq_tail;
    int64_t gq_len;

    sa_mutex idle_lock; /* worker sleep/wake handshake */
    sa_cond idle_cond;
    int64_t nidle;    /* atomic */
    int64_t runnable; /* atomic: tasks sitting in any queue */

    sa_mutex tasks_lock; /* live-task list, live count, shutdown flag */
    sa_cond tasks_cond;
    SaTask* all_tasks;
    int64_t live_tasks;    /* atomic */
    int32_t shutting_down; /* atomic */
    int32_t workers_exit;  /* atomic */
    int32_t net_inited;

    sa_rwlock tab_lock;
    SaSlot* slots;
    int32_t tab_cap, tab_used, free_head;

    sa_mutex tm_lock;
    SaTimer** heap;
    int64_t heap_n, heap_cap;
    uint64_t tm_seq;
    int64_t reactor_deadline_ns; /* deadline the reactor currently sleeps until (INT64_MAX: none) */

    void* reactor; /* platform reactor object */
    sa_thread reactor_th;
    int32_t reactor_running; /* atomic */
    int32_t reactor_stop;    /* atomic */

    sa_mutex pool_lock;
    sa_cond pool_cond;
    SaJob *pool_head, *pool_tail;
    int64_t pool_n, pool_cap;
    SaWaiter *pool_slot_head, *pool_slot_tail; /* submitters waiting for queue capacity */
    int32_t pool_nthreads, pool_stop;
    sa_thread* pool_th;

    /* sysmon: preemption and blocking detection */
    sa_mutex mon_lock;
    sa_cond mon_cond;
    sa_thread mon_th;
    int32_t mon_started;
    int32_t mon_stop;       /* atomic */
    int64_t mon_sleeping;   /* atomic, SEQ_CST handshake with workers */
    int64_t slice_ms;       /* preemption time slice, 0 = off */
    int64_t detect_ms;      /* blocking detector threshold, 0 = off */

    /* Statistics - each is maintained at the point the real state changes. */
    int64_t st_created, st_destroyed, st_objects, st_switches, st_steals, st_violations, st_fibers,
        st_running, st_max_running, st_pending_io, st_chan_waiters, st_blocking, st_os_threads,
        st_waiting, st_chans, st_socks, st_files, st_pool_threads, st_timers_fired, st_preemptions;
} SaRuntime;

static sa_mutex g_rt_lock = SA_MUTEX_INIT; /* registry writes (create/destroy) */
static SaRuntime* volatile g_rts[SA_MAX_RUNTIMES];
static uint32_t g_rt_inc[SA_MAX_RUNTIMES];
static int32_t g_started_runtimes; /* under g_rt_lock; the blocking hook is installed while > 0 */
static int64_t g_buffers_live;
static int64_t g_last_destroy_leaks;

/* ---- thread-local accessors (never inlined, never cached across a fiber switch) ---------- */
static SA_TLS SaWorker* t_worker;
static SA_TLS int64_t t_last_value;
static SA_TLS int64_t t_last_os_error;
static SA_TLS char* t_last_text;
static SA_TLS char t_last_peer[SA_PEER_TEXT];
static SA_TLS int64_t t_default_rt;
static SA_TLS SaParker t_parker;

SA_NOINLINE static SaWorker* sa_tls_worker(void) { return t_worker; }
SA_NOINLINE static void sa_tls_set_worker(SaWorker* w) { t_worker = w; }
SA_NOINLINE static int64_t* sa_tls_last_value(void) { return &t_last_value; }
SA_NOINLINE static int64_t* sa_tls_last_os_error(void) { return &t_last_os_error; }
SA_NOINLINE static char** sa_tls_last_text(void) { return &t_last_text; }
SA_NOINLINE static char* sa_tls_last_peer(void) { return t_last_peer; }
SA_NOINLINE static int64_t* sa_tls_default_rt(void) { return &t_default_rt; }
SA_NOINLINE static SaParker* sa_tls_parker(void) {
    SaParker* p = &t_parker;
    if (!p->inited) {
        sa_mutex_init(&p->m);
        sa_cond_init(&p->c);
        p->inited = 1;
    }
    return p;
}

SA_NOINLINE static SaTask* sa_current_task(void) {
    SaWorker* w = sa_tls_worker();
    return w ? w->current : NULL;
}

static void sa_set_last(int64_t v) {
    SaTask* t = sa_current_task();
    if (t) t->last_value = v;
    else *sa_tls_last_value() = v;
}

static void sa_set_os_error(int64_t e) {
    SaTask* t = sa_current_task();
    if (t) t->last_os_error = e;
    else *sa_tls_last_os_error() = e;
}

/* Takes ownership of `s` (may be NULL). */
static void sa_set_last_text(char* s) {
    SaTask* t = sa_current_task();
    char** slot = t ? &t->last_text : sa_tls_last_text();
    free(*slot);
    *slot = s;
}

static void sa_set_last_peer(const char* s) {
    SaTask* t = sa_current_task();
    char* dst = t ? t->last_peer : sa_tls_last_peer();
    snprintf(dst, SA_PEER_TEXT, "%s", s);
}

static void sa_violation(SaRuntime* rt, const char* what) {
    A_INC(&rt->st_violations);
    fprintf(stderr, "[salivo-aio] scheduler invariant violated: %s\n", what);
}

/* ============================================================================================
 * Runtime registry and runtime resolution
 * ========================================================================================== */
static SaRuntime* sa_rt_from(int64_t h) {
    if ((h & 0xFF) != 0xA1) return NULL;
    int s = (int)((h >> 8) & 0xFF);
    if (s >= SA_MAX_RUNTIMES) return NULL;
    SaRuntime* rt = g_rts[s];
    return (rt && rt->handle == h) ? rt : NULL;
}

static SaRuntime* sa_rt_of_handle(int64_t h) {
    if (h <= 0) return NULL;
    SaRuntime* rt = g_rts[H_RTS(h)];
    return (rt && rt->inc == H_INC(h)) ? rt : NULL;
}

/* The runtime an operation without a handle applies to: the calling task's runtime, else the
 * thread's runtimeUse() choice, else the only live runtime. */
static SaRuntime* sa_ctx_rt(int64_t* err) {
    SaWorker* w = sa_tls_worker();
    if (w) return w->rt;
    int64_t d = *sa_tls_default_rt();
    if (d) {
        SaRuntime* rt = sa_rt_from(d);
        if (rt) return rt;
    }
    SaRuntime* only = NULL;
    int n = 0;
    sa_mutex_lock(&g_rt_lock);
    for (int i = 0; i < SA_MAX_RUNTIMES; i++) {
        if (g_rts[i]) {
            only = g_rts[i];
            n++;
        }
    }
    sa_mutex_unlock(&g_rt_lock);
    if (n == 1) return only;
    if (err) *err = n == 0 ? SA_ESHUTDOWN : SA_EINVAL; /* several runtimes: runtimeUse() one */
    return NULL;
}

/* ============================================================================================
 * Objects and handle table
 * ========================================================================================== */
static void sa_obj_init(SaRuntime* rt, SaObj* o, int type, int64_t refs, void (*destroy)(SaObj*)) {
    o->type = type;
    o->refs = refs;
    o->rt = rt;
    o->destroy = destroy;
    A_INC(&rt->st_objects);
}

static void sa_obj_ref(SaObj* o) { A_ADD(&o->refs, 1); }

static void sa_obj_unref(SaObj* o) {
    if (A_SUB(&o->refs, 1) == 0) {
        SaRuntime* rt = o->rt;
        o->destroy(o);
        A_DEC(&rt->st_objects);
    }
}

static int64_t sa_tab_insert(SaRuntime* rt, SaObj* o) {
    sa_rw_wrlock(&rt->tab_lock);
    int32_t idx;
    if (rt->free_head >= 0) {
        idx = rt->free_head;
        rt->free_head = rt->slots[idx].next_free;
    } else {
        if (rt->tab_used == rt->tab_cap) {
            int32_t ncap = rt->tab_cap ? rt->tab_cap * 2 : 1024;
            if (ncap > SA_MAX_SLOTS) ncap = SA_MAX_SLOTS;
            if (ncap == rt->tab_cap) {
                sa_rw_wrunlock(&rt->tab_lock);
                return 0;
            }
            SaSlot* ns = (SaSlot*)realloc(rt->slots, sizeof(SaSlot) * (size_t)ncap);
            if (!ns) {
                sa_rw_wrunlock(&rt->tab_lock);
                return 0;
            }
            memset(ns + rt->tab_cap, 0, sizeof(SaSlot) * (size_t)(ncap - rt->tab_cap));
            rt->slots = ns;
            rt->tab_cap = ncap;
        }
        idx = rt->tab_used++;
    }
    SaSlot* s = &rt->slots[idx];
    if (s->gen == 0) s->gen = 1;
    s->obj = o;
    s->next_free = -1;
    int64_t h = H_MAKE(rt->inc, rt->slot, s->gen, idx);
    sa_rw_wrunlock(&rt->tab_lock);
    return h;
}

static int sa_tab_decode(SaRuntime* rt, int64_t h, int32_t* idx) {
    int32_t i = H_SLOT(h);
    if (i < 0 || i >= rt->tab_used) return 0;
    SaSlot* s = &rt->slots[i];
    if (!s->obj || s->gen != H_GEN(h)) return 0;
    *idx = i;
    return 1;
}

/* Returns a new reference, or NULL for a stale/unknown/wrong-typed handle. type 0 = any. */
static SaObj* sa_lookup(int64_t h, int type) {
    SaRuntime* rt = sa_rt_of_handle(h);
    if (!rt) return NULL;
    SaObj* o = NULL;
    int32_t idx;
    sa_rw_rdlock(&rt->tab_lock);
    if (sa_tab_decode(rt, h, &idx)) {
        SaObj* c = rt->slots[idx].obj;
        if (type == 0 || c->type == type) {
            sa_obj_ref(c);
            o = c;
        }
    }
    sa_rw_rdunlock(&rt->tab_lock);
    return o;
}

/* Removes the handle; the caller inherits the table's reference. */
static SaObj* sa_tab_remove(int64_t h, int type) {
    SaRuntime* rt = sa_rt_of_handle(h);
    if (!rt) return NULL;
    SaObj* o = NULL;
    int32_t idx;
    sa_rw_wrlock(&rt->tab_lock);
    if (sa_tab_decode(rt, h, &idx)) {
        SaSlot* s = &rt->slots[idx];
        if (type == 0 || s->obj->type == type) {
            o = s->obj;
            s->obj = NULL;
            s->gen = (s->gen + 1) & 0xFFFFFF;
            if (s->gen == 0) s->gen = 1;
            s->next_free = rt->free_head;
            rt->free_head = idx;
        }
    }
    sa_rw_wrunlock(&rt->tab_lock);
    return o;
}

/* ============================================================================================
 * Scheduler
 * ========================================================================================== */
static void sa_q_push(SaTask** head, SaTask** tail, SaTask* t) {
    t->qnext = NULL;
    if (*tail) (*tail)->qnext = t;
    else *head = t;
    *tail = t;
}

static SaTask* sa_q_pop(SaTask** head, SaTask** tail) {
    SaTask* t = *head;
    if (t) {
        *head = t->qnext;
        if (!*head) *tail = NULL;
        t->qnext = NULL;
    }
    return t;
}

static void sa_notify_idle(SaRuntime* rt) {
    if (A_LOAD_SC(&rt->nidle) > 0) {
        sa_mutex_lock(&rt->idle_lock);
        sa_cond_signal(&rt->idle_cond);
        sa_mutex_unlock(&rt->idle_lock);
    }
}

/* Local queue when called on one of this runtime's workers (cache-warm, no global contention);
 * global otherwise. `to_global` forces the global queue (used by yield so other work gets a turn). */
static void sa_enqueue(SaRuntime* rt, SaTask* t, int to_global) {
    SaWorker* w = sa_tls_worker();
    if (w && w->rt == rt && !to_global) {
        sa_mutex_lock(&w->qlock);
        sa_q_push(&w->qhead, &w->qtail, t);
        A_ADD(&w->qlen, 1);
        sa_mutex_unlock(&w->qlock);
    } else {
        sa_mutex_lock(&rt->gq_lock);
        sa_q_push(&rt->gq_head, &rt->gq_tail, t);
        A_ADD(&rt->gq_len, 1);
        sa_mutex_unlock(&rt->gq_lock);
    }
    /* SEQ_CST pairs with the worker's nidle increment + runnable re-check (no lost wakeup). */
    A_ADD_SC(&rt->runnable, 1);
    sa_notify_idle(rt);
}

static void sa_task_wake(SaTask* t) {
    SaRuntime* rt = t->obj.rt;
    for (;;) {
        int32_t s = A_LOAD(&t->sched);
        if (s == SW_RUNNING) {
            if (A_CAS(&t->sched, &s, SW_NOTIFIED)) return; /* the scheduler re-queues it */
            continue;
        }
        if (s == SW_IDLE) {
            if (A_CAS(&t->sched, &s, SW_QUEUED)) {
                A_STORE(&t->state, TS_RUNNABLE);
                sa_enqueue(rt, t, 0);
                return;
            }
            continue;
        }
        return; /* already queued, already notified, or finished: never a double wake */
    }
}

static SaTask* sa_pop_local(SaWorker* w) {
    if (A_LOAD(&w->qlen) == 0) return NULL;
    sa_mutex_lock(&w->qlock);
    SaTask* t = sa_q_pop(&w->qhead, &w->qtail);
    if (t) A_SUB(&w->qlen, 1);
    sa_mutex_unlock(&w->qlock);
    return t;
}

static SaTask* sa_pop_global(SaRuntime* rt) {
    if (A_LOAD(&rt->gq_len) == 0) return NULL;
    sa_mutex_lock(&rt->gq_lock);
    SaTask* t = sa_q_pop(&rt->gq_head, &rt->gq_tail);
    if (t) A_SUB(&rt->gq_len, 1);
    sa_mutex_unlock(&rt->gq_lock);
    return t;
}

/* Steals half of a victim's local queue; returns one task and keeps the rest locally. */
static SaTask* sa_steal(SaRuntime* rt, SaWorker* w) {
    int n = rt->nworkers;
    if (n < 2) return NULL;
    w->rng ^= w->rng << 13;
    w->rng ^= w->rng >> 7;
    w->rng ^= w->rng << 17;
    int start = (int)(w->rng % (uint64_t)n);
    for (int i = 0; i < n; i++) {
        SaWorker* v = &rt->workers[(start + i) % n];
        if (v == w || A_LOAD(&v->qlen) == 0) continue;
        SaTask *head = NULL, *tail = NULL;
        int64_t took = 0;
        sa_mutex_lock(&v->qlock);
        int64_t want = (A_LOAD(&v->qlen) + 1) / 2;
        while (took < want) {
            SaTask* t = sa_q_pop(&v->qhead, &v->qtail);
            if (!t) break;
            sa_q_push(&head, &tail, t);
            took++;
        }
        if (took) A_SUB(&v->qlen, took);
        sa_mutex_unlock(&v->qlock);
        if (!took) continue;
        A_INC(&rt->st_steals);
        SaTask* first = sa_q_pop(&head, &tail);
        if (head) {
            sa_mutex_lock(&w->qlock);
            while (head) {
                SaTask* t = sa_q_pop(&head, &tail);
                sa_q_push(&w->qhead, &w->qtail, t);
                A_ADD(&w->qlen, 1);
            }
            sa_mutex_unlock(&w->qlock);
        }
        return first;
    }
    return NULL;
}

static SaTask* sa_next_task(SaRuntime* rt, SaWorker* w) {
    SaTask* t = NULL;
    /* Every 31st pick checks the global queue first, so injected and yielded tasks cannot be
     * starved by a worker whose local queue never drains (bounded starvation). */
    if ((++w->tick % 31) == 0) t = sa_pop_global(rt);
    if (!t) t = sa_pop_local(w);
    if (!t) t = sa_pop_global(rt);
    if (!t) t = sa_steal(rt, w);
    if (t) A_SUB(&rt->runnable, 1);
    return t;
}

static void sa_task_finalize(SaRuntime* rt, SaTask* t, int final_state);

SA_NOINLINE static void sa_switch_to_scheduler(SaTask* t) {
    (void)t;
    sa_fiber_switch(sa_tls_worker()->sched_fiber);
}

/* Fiber body. The fiber is reused for successive tasks (fiber pool), so the task is re-read from
 * the worker after every switch. */
static void SA_FIBER_CALL sa_fiber_main(void* unused) {
    (void)unused;
    for (;;) {
        SaTask* t = sa_current_task();
        t->result = t->fn(t->arg);
        t->exit_reason = EXIT_DONE;
        sa_switch_to_scheduler(t);
    }
}

static void* sa_fiber_get(SaRuntime* rt, SaWorker* w) {
    if (w->nfibers > 0) return w->fibers[--w->nfibers];
    void* f = sa_fiber_new(rt->stack_size, sa_fiber_main);
    if (f) A_INC(&rt->st_fibers);
    return f;
}

static void sa_fiber_put(SaRuntime* rt, SaWorker* w, void* f) {
    if (w->nfibers < SA_FIBER_POOL) {
        w->fibers[w->nfibers++] = f;
    } else {
        sa_fiber_free(f);
        A_DEC(&rt->st_fibers);
    }
}

static void sa_mon_kick(SaRuntime* rt);

static void sa_release_owned(int64_t h);

static void sa_run_task(SaRuntime* rt, SaWorker* w, SaTask* t) {
    int32_t expect = SW_QUEUED;
    if (!A_CAS(&t->sched, &expect, SW_RUNNING)) {
        sa_violation(rt, "dequeued task was not in the QUEUED state");
        return;
    }
    if (!t->started && A_LOAD(&t->cancel_req)) {
        /* Cancelled before it ever ran: the body is never entered, so a handle it owned is
         * released here instead of leaking. */
        if (t->owns_arg) sa_release_owned(t->arg);
        sa_task_finalize(rt, t, TS_CANCELLED);
        return;
    }
    int32_t none = -1;
    if (!A_CAS(&t->running_on, &none, w->id)) {
        sa_violation(rt, "task already running on another worker");
        return;
    }
    if (!t->fiber) {
        t->fiber = sa_fiber_get(rt, w);
        if (!t->fiber) {
            A_STORE(&t->running_on, -1);
            t->fail_set = 1;
            t->fail_code = SA_EOS;
            if (t->owns_arg) sa_release_owned(t->arg);
            sa_task_finalize(rt, t, TS_FAILED);
            return;
        }
    }
    t->started = 1;
    t->exit_reason = EXIT_NONE;
    t->budget = SA_BUDGET;
    A_STORE(&t->state, TS_RUNNING);
    int64_t running = A_ADD_SC(&rt->st_running, 1);
    int64_t mx = A_LOAD(&rt->st_max_running);
    while (running > mx && !A_CAS(&rt->st_max_running, &mx, running)) {
    }
    sa_mon_kick(rt);
    w->current = t;
    A_STORE(&w->preempt, 0);
    int64_t t0 = sa_now_ns();
    A_STORE(&w->running_since_ns, t0);
    sa_fiber_switch(t->fiber);
    /* Back on the scheduler fiber: the task has fully switched out, so it is now safe for
     * another worker to resume it. */
    A_STORE(&w->running_since_ns, 0);
    A_ADD(&w->busy_ns, sa_now_ns() - t0);
    A_INC(&w->runs);
    w->current = NULL;
    A_SUB(&rt->st_running, 1);
    A_INC(&rt->st_switches);
    A_STORE(&t->running_on, -1);

    switch (t->exit_reason) {
    case EXIT_YIELD:
        A_STORE(&t->state, TS_RUNNABLE);
        A_STORE(&t->sched, SW_QUEUED);
        sa_enqueue(rt, t, 1);
        break;
    case EXIT_PARK: {
        int32_t running_state = SW_RUNNING;
        if (!A_CAS(&t->sched, &running_state, SW_IDLE)) {
            /* A wake arrived while the task was switching out (SW_NOTIFIED): run it again. */
            A_STORE(&t->state, TS_RUNNABLE);
            A_STORE(&t->sched, SW_QUEUED);
            sa_enqueue(rt, t, 0);
        }
        break;
    }
    case EXIT_DONE: {
        sa_fiber_put(rt, w, t->fiber);
        t->fiber = NULL;
        int final_state = t->fail_set ? TS_FAILED : (t->cancel_observed ? TS_CANCELLED : TS_COMPLETED);
        sa_task_finalize(rt, t, final_state);
        break;
    }
    default:
        sa_violation(rt, "task switched out without an exit reason");
        break;
    }
}

/* Parks the calling task until something wakes it. Must be called on the task's fiber. */
static void sa_park_task(SaTask* t, int kind) {
    SaRuntime* rt = t->obj.rt;
    t->wait_kind = kind;
    A_STORE(&t->state, TS_WAITING);
    A_INC(&rt->st_waiting);
    t->exit_reason = EXIT_PARK;
    sa_switch_to_scheduler(t);
    A_DEC(&rt->st_waiting);
    t->wait_kind = WK_NONE;
}

/* ============================================================================================
 * Waiter helpers
 * ========================================================================================== */
static void sa_waiter_init(SaWaiter* w) {
    memset(w, 0, sizeof(*w));
    w->task = sa_current_task();
    if (!w->task) w->parker = sa_tls_parker();
}

/* Caller holds the lock protecting `w` (or a reference on w->task). */
static void sa_waiter_fire(SaWaiter* w) {
    SaTask* t = w->task;
    if (t) {
        A_STORE(&w->fired, 1);
        sa_task_wake(t);
    } else {
        SaParker* p = w->parker;
        sa_mutex_lock(&p->m);
        A_STORE(&w->fired, 1);
        sa_cond_broadcast(&p->c);
        sa_mutex_unlock(&p->m);
    }
}

/* Waits until `a` (or `b`) fires. Returns 1 if fired, 0 if the calling task was cancelled first.
 * OS threads (non-tasks) are not cancellable and wait until fired. */
static int sa_wait2(SaWaiter* a, SaWaiter* b, int kind, int cancellable) {
    SaTask* t = a->task;
    if (t) {
        for (;;) {
            if (A_LOAD(&a->fired) || (b && A_LOAD(&b->fired))) return 1;
            if (cancellable && A_LOAD(&t->cancel_req)) return 0;
            sa_park_task(t, kind);
        }
    }
    SaParker* p = a->parker;
    sa_mutex_lock(&p->m);
    while (!A_LOAD(&a->fired) && !(b && A_LOAD(&b->fired))) sa_cond_wait(&p->c, &p->m);
    sa_mutex_unlock(&p->m);
    return 1;
}

static int sa_wait(SaWaiter* w, int kind) { return sa_wait2(w, NULL, kind, 1); }

static void sa_list_push(SaWaiter** head, SaWaiter** tail, SaWaiter* w) {
    w->next = NULL;
    w->prev = *tail;
    if (*tail) (*tail)->next = w;
    else *head = w;
    *tail = w;
}

static void sa_list_unlink(SaWaiter** head, SaWaiter** tail, SaWaiter* w) {
    if (w->prev) w->prev->next = w->next;
    else *head = w->next;
    if (w->next) w->next->prev = w->prev;
    else *tail = w->prev;
    w->prev = w->next = NULL;
}

static SaWaiter* sa_list_pop(SaWaiter** head, SaWaiter** tail) {
    SaWaiter* w = *head;
    if (w) sa_list_unlink(head, tail, w);
    return w;
}

static int64_t sa_cancelled_here(void) {
    SaTask* t = sa_current_task();
    if (t) t->cancel_observed = 1;
    return SA_ECANCELLED;
}

/* Cooperative budget: a task that keeps completing ready operations yields periodically. */
int64_t salivo_aio_yield(void);
static void sa_coop(void) {
    SaTask* t = sa_current_task();
    if (t && --t->budget <= 0) salivo_aio_yield();
}

/* ============================================================================================
 * Timers (min-heap ordered by deadline, then arming order). Serviced by the reactor thread.
 * ========================================================================================== */
static int sa_tm_less(SaTimer* a, SaTimer* b) {
    return a->deadline_ns < b->deadline_ns || (a->deadline_ns == b->deadline_ns && a->seq < b->seq);
}

static void sa_heap_set(SaRuntime* rt, int64_t i, SaTimer* t) {
    rt->heap[i] = t;
    t->idx = i;
}

static void sa_heap_up(SaRuntime* rt, int64_t i) {
    SaTimer* t = rt->heap[i];
    while (i > 0) {
        int64_t p = (i - 1) / 2;
        if (!sa_tm_less(t, rt->heap[p])) break;
        sa_heap_set(rt, i, rt->heap[p]);
        i = p;
    }
    sa_heap_set(rt, i, t);
}

static void sa_heap_down(SaRuntime* rt, int64_t i) {
    SaTimer* t = rt->heap[i];
    for (;;) {
        int64_t l = 2 * i + 1, r = l + 1, m = i;
        SaTimer* best = t;
        if (l < rt->heap_n && sa_tm_less(rt->heap[l], best)) { m = l; best = rt->heap[l]; }
        if (r < rt->heap_n && sa_tm_less(rt->heap[r], best)) { m = r; best = rt->heap[r]; }
        if (m == i) break;
        sa_heap_set(rt, i, rt->heap[m]);
        i = m;
    }
    sa_heap_set(rt, i, t);
}

static void sa_heap_remove(SaRuntime* rt, int64_t i) {
    SaTimer* gone = rt->heap[i];
    rt->heap_n--;
    if (i != rt->heap_n) {
        sa_heap_set(rt, i, rt->heap[rt->heap_n]);
        sa_heap_down(rt, i);
        sa_heap_up(rt, rt->heap[i]->idx);
    }
    gone->idx = -1;
}

static void sa_reactor_notify(SaRuntime* rt);

static int sa_timer_arm_ex(SaRuntime* rt, SaTimer* tm, SaWaiter* w, int64_t ms, void (*cb)(SaTimer*)) {
    int notify = 0;
    tm->w = w;
    tm->cb = cb;
    sa_mutex_lock(&rt->tm_lock);
    if (rt->heap_n == rt->heap_cap) {
        int64_t ncap = rt->heap_cap ? rt->heap_cap * 2 : 256;
        SaTimer** nh = (SaTimer**)realloc(rt->heap, sizeof(SaTimer*) * (size_t)ncap);
        if (!nh) {
            sa_mutex_unlock(&rt->tm_lock);
            tm->idx = -1;
            return 0;
        }
        rt->heap = nh;
        rt->heap_cap = ncap;
    }
    tm->deadline_ns = sa_now_ns() + ms * 1000000;
    tm->seq = rt->tm_seq++;
    rt->heap_n++;
    sa_heap_set(rt, rt->heap_n - 1, tm);
    sa_heap_up(rt, rt->heap_n - 1);
    if (tm->idx == 0 && tm->deadline_ns < rt->reactor_deadline_ns) notify = 1;
    sa_mutex_unlock(&rt->tm_lock);
    if (notify) sa_reactor_notify(rt);
    return 1;
}

static int sa_timer_arm(SaRuntime* rt, SaTimer* tm, SaWaiter* w, int64_t ms) { return sa_timer_arm_ex(rt, tm, w, ms, NULL); }
static int sa_timer_arm_cb(SaRuntime* rt, SaTimer* tm, int64_t ms, void (*cb)(SaTimer*)) { return sa_timer_arm_ex(rt, tm, NULL, ms, cb); }

/* Always takes the timer lock, even if the timer already fired: this orders the task's return
 * after the reactor's wake (see the waiter memory rule). */
static void sa_timer_disarm(SaRuntime* rt, SaTimer* tm) {
    sa_mutex_lock(&rt->tm_lock);
    if (tm->idx >= 0) sa_heap_remove(rt, tm->idx);
    sa_mutex_unlock(&rt->tm_lock);
}

/* Reactor side: fires due timers, returns the next deadline (INT64_MAX when none). */
static int64_t sa_timers_run(SaRuntime* rt) {
    sa_mutex_lock(&rt->tm_lock);
    int64_t now = sa_now_ns();
    while (rt->heap_n > 0 && rt->heap[0]->deadline_ns <= now) {
        SaTimer* tm = rt->heap[0];
        sa_heap_remove(rt, 0);
        A_INC(&rt->st_timers_fired);
        if (tm->cb) tm->cb(tm);
        else sa_waiter_fire(tm->w);
    }
    int64_t next = rt->heap_n > 0 ? rt->heap[0]->deadline_ns : INT64_MAX;
    rt->reactor_deadline_ns = next;
    sa_mutex_unlock(&rt->tm_lock);
    return next;
}

/* ============================================================================================
 * Blocking pool: bounded queue, fixed threads. Keeps inherently blocking OS calls (file open,
 * flush, close, name resolution, POSIX file data, user blocking functions) off the workers.
 * ========================================================================================== */
SA_THREAD_FN(sa_pool_main) {
    SaRuntime* rt = (SaRuntime*)arg;
    A_INC(&rt->st_os_threads);
    *sa_tls_default_rt() = rt->handle; /* runtime calls made by blocking jobs target this runtime */
    sa_mutex_lock(&rt->pool_lock);
    for (;;) {
        while (!rt->pool_head && !rt->pool_stop) sa_cond_wait(&rt->pool_cond, &rt->pool_lock);
        if (!rt->pool_head) break; /* stop requested and queue drained */
        SaJob* j = rt->pool_head;
        rt->pool_head = j->next;
        if (!rt->pool_head) rt->pool_tail = NULL;
        rt->pool_n--;
        j->state = JOB_RUNNING;
        SaWaiter* s = sa_list_pop(&rt->pool_slot_head, &rt->pool_slot_tail);
        if (s) sa_waiter_fire(s); /* capacity freed: admit one waiting submitter */
        sa_mutex_unlock(&rt->pool_lock);
        int64_t r = j->run(j);
        sa_mutex_lock(&rt->pool_lock);
        j->result = r;
        j->state = JOB_DONE;
        sa_waiter_fire(&j->w);
    }
    sa_mutex_unlock(&rt->pool_lock);
    *sa_tls_default_rt() = 0;
    A_DEC(&rt->st_os_threads);
    return 0;
}

/* Runs `j` on the pool and waits. A job still queued when the caller is cancelled is withdrawn
 * (Cancelled); a running job cannot be interrupted, so its real result is returned. */
static int64_t sa_pool_run_ex(SaRuntime* rt, SaJob* j, int cancellable) {
    if (!A_LOAD(&rt->reactor_running)) return SA_ESHUTDOWN;
    sa_waiter_init(&j->w);
    j->next = NULL;
    sa_mutex_lock(&rt->pool_lock);
    while (rt->pool_n >= rt->pool_cap) { /* backpressure: wait for capacity, never grow */
        SaWaiter sw;
        sa_waiter_init(&sw);
        sa_list_push(&rt->pool_slot_head, &rt->pool_slot_tail, &sw);
        sa_mutex_unlock(&rt->pool_lock);
        sa_wait2(&sw, NULL, WK_BLOCKING, cancellable);
        sa_mutex_lock(&rt->pool_lock);
        if (!A_LOAD(&sw.fired)) {
            sa_list_unlink(&rt->pool_slot_head, &rt->pool_slot_tail, &sw);
            sa_mutex_unlock(&rt->pool_lock);
            return sa_cancelled_here();
        }
    }
    if (rt->pool_stop) {
        sa_mutex_unlock(&rt->pool_lock);
        return SA_ESHUTDOWN;
    }
    j->state = JOB_QUEUED;
    if (rt->pool_tail) rt->pool_tail->next = j;
    else rt->pool_head = j;
    rt->pool_tail = j;
    rt->pool_n++;
    sa_cond_signal(&rt->pool_cond);
    sa_mutex_unlock(&rt->pool_lock);

    sa_wait2(&j->w, NULL, WK_BLOCKING, cancellable);
    sa_mutex_lock(&rt->pool_lock);
    if (!A_LOAD(&j->w.fired)) {
        if (j->state == JOB_QUEUED) {
            SaJob** pp = &rt->pool_head;
            SaJob* prev = NULL;
            while (*pp && *pp != j) { prev = *pp; pp = &(*pp)->next; }
            if (*pp) {
                *pp = j->next;
                if (rt->pool_tail == j) rt->pool_tail = prev;
                rt->pool_n--;
            }
            SaWaiter* s = sa_list_pop(&rt->pool_slot_head, &rt->pool_slot_tail);
            if (s) sa_waiter_fire(s);
            sa_mutex_unlock(&rt->pool_lock);
            return sa_cancelled_here();
        }
        sa_mutex_unlock(&rt->pool_lock);
        sa_wait2(&j->w, NULL, WK_BLOCKING, 0);
        sa_mutex_lock(&rt->pool_lock);
    }
    sa_mutex_unlock(&rt->pool_lock);
    return j->result;
}

static int64_t sa_pool_run(SaRuntime* rt, SaJob* j) { return sa_pool_run_ex(rt, j, 1); }

static int64_t sa_job_user_fn(SaJob* j) { return ((sa_task_fn)(intptr_t)j->a)(j->b); }

/* ============================================================================================
 * Platform reactor and I/O (needs the core above)
 * ========================================================================================== */
static int sa_reactor_init(SaRuntime* rt);
static void sa_reactor_fini(SaRuntime* rt);
static int sa_reactor_poll(SaRuntime* rt, int64_t timeout_ms); /* returns 0 when told to stop */
static int sa_net_init(void);
static void sa_net_fini(void);

/* ============================================================================================
 * Tasks: spawn, finalize, await, cancel
 * ========================================================================================== */
static void sa_task_destroy(SaObj* o) {
    SaTask* t = (SaTask*)o;
    A_INC(&o->rt->st_destroyed);
    free(t->last_text);
    sa_mutex_destroy(&t->lock);
    free(t);
}

static void sa_task_finalize(SaRuntime* rt, SaTask* t, int final_state) {
    sa_mutex_lock(&t->lock);
    A_STORE(&t->state, final_state);
    A_STORE(&t->sched, SW_DONE);
    while (t->joiners) { /* head-linked, doubly linked list */
        SaWaiter* j = t->joiners;
        t->joiners = j->next;
        if (t->joiners) t->joiners->prev = NULL;
        j->next = j->prev = NULL;
        sa_waiter_fire(j);
    }
    sa_mutex_unlock(&t->lock);

    sa_mutex_lock(&rt->tasks_lock);
    if (t->all_prev) t->all_prev->all_next = t->all_next;
    else rt->all_tasks = t->all_next;
    if (t->all_next) t->all_next->all_prev = t->all_prev;
    t->all_prev = t->all_next = NULL;
    A_SUB(&rt->live_tasks, 1);
    int none_left = A_LOAD(&rt->live_tasks) == 0;
    if (none_left) sa_cond_broadcast(&rt->tasks_cond);
    sa_mutex_unlock(&rt->tasks_lock);
    if (none_left && A_LOAD(&rt->workers_exit)) {
        sa_mutex_lock(&rt->idle_lock);
        sa_cond_broadcast(&rt->idle_cond);
        sa_mutex_unlock(&rt->idle_lock);
    }
    sa_obj_unref(&t->obj); /* the "alive" reference */
}

static int64_t sa_spawn_in_ex(SaRuntime* rt, int64_t fn, int64_t arg, int owns) {
    if (rt->state != RT_STARTED) return SA_ESHUTDOWN;
    if (fn == 0) return SA_EINVAL;
    SaTask* t = (SaTask*)calloc(1, sizeof(SaTask));
    if (!t) return SA_EOS;
    sa_obj_init(rt, &t->obj, OT_TASK, 2 /* handle + alive */, sa_task_destroy);
    t->fn = (sa_task_fn)(intptr_t)fn;
    t->arg = arg;
    t->owns_arg = owns;
    t->running_on = -1;
    t->state = TS_CREATED;
    sa_mutex_init(&t->lock);

    sa_mutex_lock(&rt->tasks_lock);
    if (A_LOAD(&rt->shutting_down)) {
        sa_mutex_unlock(&rt->tasks_lock);
        A_DEC(&rt->st_objects);
        sa_mutex_destroy(&t->lock);
        free(t);
        return SA_ESHUTDOWN;
    }
    A_ADD(&rt->live_tasks, 1);
    t->all_next = rt->all_tasks;
    if (rt->all_tasks) rt->all_tasks->all_prev = t;
    rt->all_tasks = t;
    sa_mutex_unlock(&rt->tasks_lock);

    A_INC(&rt->st_created);
    t->handle = sa_tab_insert(rt, &t->obj);
    if (t->handle == 0) {
        /* Table exhausted: run nothing, finish as failed, drop both references. */
        t->fail_set = 1;
        t->fail_code = SA_EOS;
        t->sched = SW_RUNNING;
        sa_task_finalize(rt, t, TS_FAILED);
        sa_obj_unref(&t->obj);
        return SA_EOS;
    }
    t->sched = SW_QUEUED;
    A_STORE(&t->state, TS_RUNNABLE);
    int64_t h = t->handle;
    sa_enqueue(rt, t, 0);
    return h;
}

static int64_t sa_spawn_in(SaRuntime* rt, int64_t fn, int64_t arg) { return sa_spawn_in_ex(rt, fn, arg, 0); }

/* Like spawn, but the task owns `h` (the argument): if the task is cancelled before it ever runs,
 * the runtime closes/releases h, so a handle handed to a task never leaks. On error the caller
 * still owns h. */
int64_t salivo_aio_spawn_owning(int64_t fn, int64_t h) {
    int64_t err = SA_ESHUTDOWN;
    SaRuntime* rt = sa_ctx_rt(&err);
    if (!rt) return err;
    return sa_spawn_in_ex(rt, fn, h, 1);
}

int64_t salivo_aio_spawn(int64_t fn, int64_t arg) {
    int64_t err = SA_ESHUTDOWN;
    SaRuntime* rt = sa_ctx_rt(&err);
    if (!rt) return err;
    return sa_spawn_in(rt, fn, arg);
}

int64_t salivo_aio_spawn_on(int64_t rth, int64_t fn, int64_t arg) {
    SaRuntime* rt = sa_rt_from(rth);
    if (!rt) return SA_EHANDLE;
    return sa_spawn_in(rt, fn, arg);
}

static int sa_terminal(int s) { return s == TS_COMPLETED || s == TS_CANCELLED || s == TS_FAILED; }

static void sa_task_request_cancel(SaTask* t) {
    A_STORE(&t->cancel_req, 1);
    sa_task_wake(t);
}

/* Waits for `t` to finish, optionally with a timeout. Result value via sa_set_last. */
static int64_t sa_join(SaTask* t, int64_t timeout_ms) {
    SaRuntime* rt = t->obj.rt;
    SaWaiter jw, tw;
    SaTimer tm;
    int armed = 0;
    sa_waiter_init(&jw);
    sa_mutex_lock(&t->lock);
    int done = sa_terminal(A_LOAD(&t->state));
    if (!done) {
        jw.prev = NULL;
        jw.next = t->joiners;
        if (t->joiners) t->joiners->prev = &jw;
        t->joiners = &jw;
    }
    sa_mutex_unlock(&t->lock);

    if (!done) {
        if (timeout_ms >= 0) {
            sa_waiter_init(&tw);
            armed = sa_timer_arm(rt, &tm, &tw, timeout_ms);
        }
        sa_wait2(&jw, armed ? &tw : NULL, WK_JOIN, 1);
        sa_mutex_lock(&t->lock);
        done = A_LOAD(&jw.fired);
        if (!done) {
            if (jw.prev) jw.prev->next = jw.next;
            else t->joiners = jw.next;
            if (jw.next) jw.next->prev = jw.prev;
        }
        sa_mutex_unlock(&t->lock);
        if (armed) sa_timer_disarm(rt, &tm);
        if (!done) {
            if (armed && A_LOAD(&tw.fired)) {
                sa_task_request_cancel(t);
                return SA_ETIMEOUT;
            }
            return sa_cancelled_here();
        }
    }
    switch (A_LOAD(&t->state)) {
    case TS_COMPLETED:
        sa_set_last(t->result);
        return SA_OK;
    case TS_FAILED:
        sa_set_last(t->fail_code);
        return SA_EFAILED;
    default:
        return SA_ECANCELLED;
    }
}

static int64_t sa_await_common(int64_t h, int64_t timeout_ms) {
    SaTask* t = (SaTask*)sa_lookup(h, OT_TASK);
    if (!t) return SA_EHANDLE;
    if (t == sa_current_task()) { /* a task awaiting itself would never wake */
        sa_obj_unref(&t->obj);
        return SA_EINVAL;
    }
    int64_t r = sa_join(t, timeout_ms);
    sa_obj_unref(&t->obj);
    return r;
}

int64_t salivo_aio_await(int64_t h) {
    /* A Task built from a failed spawn carries the spawn's error code as its handle: awaiting it
     * reports that error (e.g. Shutdown) instead of a generic InvalidHandle. */
    if (h < 0 && h >= SA_EFAILED) return h;
    return sa_await_common(h, -1);
}

int64_t salivo_aio_await_timeout(int64_t h, int64_t ms) {
    if (ms < 0) return SA_EINVAL;
    return sa_await_common(h, ms);
}

int64_t salivo_aio_release(int64_t h) {
    SaObj* o = sa_tab_remove(h, OT_TASK);
    if (!o) return SA_EHANDLE;
    sa_obj_unref(o);
    return SA_OK;
}

int64_t salivo_aio_cancel(int64_t h) {
    SaTask* t = (SaTask*)sa_lookup(h, OT_TASK);
    if (!t) return SA_EHANDLE;
    int64_t r = sa_terminal(A_LOAD(&t->state)) ? SA_EINVAL : SA_OK; /* too late to cancel */
    if (r == SA_OK) sa_task_request_cancel(t);
    sa_obj_unref(&t->obj);
    return r;
}

int64_t salivo_aio_is_cancelled(int64_t h) {
    if (h == 0) {
        SaTask* t = sa_current_task();
        if (!t) return SA_ENOTASK;
        if (A_LOAD(&t->cancel_req)) {
            t->cancel_observed = 1;
            return 1;
        }
        return 0;
    }
    SaTask* t = (SaTask*)sa_lookup(h, OT_TASK);
    if (!t) return SA_EHANDLE;
    int64_t r = A_LOAD(&t->cancel_req) ? 1 : 0;
    sa_obj_unref(&t->obj);
    return r;
}

int64_t salivo_aio_task_state(int64_t h) {
    SaTask* t = (SaTask*)sa_lookup(h, OT_TASK);
    if (!t) return SA_EHANDLE;
    int64_t s = A_LOAD(&t->state);
    sa_obj_unref(&t->obj);
    return s;
}

int64_t salivo_aio_yield(void) {
    SaTask* t = sa_current_task();
    if (!t) {
        sa_thread_yield_os();
        return SA_OK;
    }
    t->exit_reason = EXIT_YIELD;
    sa_switch_to_scheduler(t);
    t = sa_current_task();
    t->budget = SA_BUDGET;
    if (A_LOAD(&t->cancel_req)) return sa_cancelled_here();
    return SA_OK;
}

/* Compiler-inserted at loop back-edges of programs that use salivo.std.runtime: yields when the
 * sysmon has asked this worker's task to give up the CPU (its time slice expired). */
int64_t salivo_aio_safepoint(void) {
    SaWorker* w = sa_tls_worker();
    if (!w || !w->current || !A_LOAD(&w->preempt)) return 0;
    A_STORE(&w->preempt, 0);
    A_INC(&w->rt->st_preemptions);
    SaTask* t = w->current;
    t->exit_reason = EXIT_YIELD;
    sa_switch_to_scheduler(t);
    return 1;
}

int64_t salivo_aio_current(void) {
    SaTask* t = sa_current_task();
    return t ? t->handle : 0;
}

int64_t salivo_aio_fail(int64_t code) {
    SaTask* t = sa_current_task();
    if (!t) return SA_ENOTASK;
    t->fail_set = 1;
    t->fail_code = code;
    return SA_OK;
}

int64_t salivo_aio_sleep(int64_t ms) {
    if (ms < 0) return SA_EINVAL;
    int64_t err = SA_ESHUTDOWN;
    SaRuntime* rt = sa_ctx_rt(&err);
    if (!rt) return err;
    if (!A_LOAD(&rt->reactor_running)) return SA_ESHUTDOWN;
    SaTask* self = sa_current_task();
    if (self && A_LOAD(&self->cancel_req)) return sa_cancelled_here();
    if (ms == 0) return salivo_aio_yield();
    SaWaiter w;
    SaTimer tm;
    sa_waiter_init(&w);
    if (!sa_timer_arm(rt, &tm, &w, ms)) return SA_EOS;
    int fired = sa_wait(&w, WK_TIMER);
    sa_timer_disarm(rt, &tm);
    if (!fired && !A_LOAD(&w.fired)) return sa_cancelled_here();
    return SA_OK;
}

int64_t salivo_aio_blocking(int64_t fn, int64_t arg) {
    if (fn == 0) return SA_EINVAL;
    int64_t err = SA_ESHUTDOWN;
    SaRuntime* rt = sa_ctx_rt(&err);
    if (!rt) return err;
    SaJob j;
    memset(&j, 0, sizeof(j));
    j.run = sa_job_user_fn;
    j.a = fn;
    j.b = arg;
    int64_t r = sa_pool_run(rt, &j);
    if (r == SA_ECANCELLED || r == SA_ESHUTDOWN) return r;
    sa_set_last(r);
    return SA_OK;
}

/* ============================================================================================
 * Channels. Bounded channels never hold more than `cap` values: a sender that finds the channel
 * full parks with its value in its own frame until a receiver makes room (backpressure).
 * Text channels carry owned string copies: the channel owns buffered strings, a receiver owns
 * what it received, a sender whose value is not delivered (Closed/Cancelled) frees its copy.
 * ========================================================================================== */
typedef struct SaChan {
    SaObj obj;
    sa_mutex m;
    int64_t cap; /* 0 = unbounded */
    int64_t* buf;
    int64_t bufcap, head, count;
    int32_t closed;
    int32_t text; /* values are owned char* */
    SaWaiter *rq_h, *rq_t; /* waiting receivers (only while empty) */
    SaWaiter *sq_h, *sq_t; /* waiting senders (only while full) */
} SaChan;

static void sa_chan_destroy(SaObj* o) {
    SaChan* c = (SaChan*)o;
    A_DEC(&o->rt->st_chans);
    if (c->text) {
        for (int64_t i = 0; i < c->count; i++) free((char*)(intptr_t)c->buf[(c->head + i) % c->bufcap]);
    }
    free(c->buf);
    sa_mutex_destroy(&c->m);
    free(c);
}

static int sa_chan_push(SaChan* c, int64_t v) {
    if (c->count == c->bufcap) {
        if (c->cap != 0) return 0;
        int64_t ncap = c->bufcap ? c->bufcap * 2 : 16;
        int64_t* nb = (int64_t*)malloc(sizeof(int64_t) * (size_t)ncap);
        if (!nb) return 0;
        for (int64_t i = 0; i < c->count; i++) nb[i] = c->buf[(c->head + i) % c->bufcap];
        free(c->buf);
        c->buf = nb;
        c->bufcap = ncap;
        c->head = 0;
    }
    c->buf[(c->head + c->count) % c->bufcap] = v;
    c->count++;
    return 1;
}

static int64_t sa_chan_pop(SaChan* c) {
    int64_t v = c->buf[c->head];
    c->head = (c->head + 1) % c->bufcap;
    c->count--;
    return v;
}

static int64_t sa_chan_new(int64_t cap, int text) {
    int64_t err = SA_ESHUTDOWN;
    SaRuntime* rt = sa_ctx_rt(&err);
    if (!rt) return err;
    if (cap < 0 || cap > (1LL << 28)) return SA_EINVAL;
    SaChan* c = (SaChan*)calloc(1, sizeof(SaChan));
    if (!c) return SA_EOS;
    if (cap > 0) {
        c->buf = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
        if (!c->buf) {
            free(c);
            return SA_EOS;
        }
        c->bufcap = cap;
    }
    sa_obj_init(rt, &c->obj, OT_CHAN, 1, sa_chan_destroy);
    A_INC(&rt->st_chans);
    c->cap = cap;
    c->text = text;
    sa_mutex_init(&c->m);
    int64_t h = sa_tab_insert(rt, &c->obj);
    if (!h) {
        sa_obj_unref(&c->obj);
        return SA_EOS;
    }
    return h;
}

int64_t salivo_aio_chan_new(int64_t cap) { return sa_chan_new(cap, 0); }
int64_t salivo_aio_chan_new_text(int64_t cap) { return sa_chan_new(cap, 1); }

static int64_t sa_chan_send(SaChan* c, int64_t v, int wait) {
    SaRuntime* rt = c->obj.rt;
    sa_mutex_lock(&c->m);
    if (c->closed) {
        sa_mutex_unlock(&c->m);
        return SA_ECLOSED;
    }
    SaWaiter* r = sa_list_pop(&c->rq_h, &c->rq_t);
    if (r) { /* hand the value straight to a waiting receiver */
        A_DEC(&rt->st_chan_waiters);
        r->value = v;
        r->status = SA_OK;
        sa_waiter_fire(r);
        sa_mutex_unlock(&c->m);
        sa_coop();
        return SA_OK;
    }
    if (c->cap == 0 || c->count < c->cap) {
        int ok = sa_chan_push(c, v);
        sa_mutex_unlock(&c->m);
        if (!ok) return SA_EOS;
        sa_coop();
        return SA_OK;
    }
    if (!wait) {
        sa_mutex_unlock(&c->m);
        return SA_EFULL;
    }
    SaTask* self = sa_current_task();
    if (self && A_LOAD(&self->cancel_req)) {
        sa_mutex_unlock(&c->m);
        return sa_cancelled_here();
    }
    SaWaiter w;
    sa_waiter_init(&w);
    w.value = v;
    sa_list_push(&c->sq_h, &c->sq_t, &w);
    A_INC(&rt->st_chan_waiters);
    sa_mutex_unlock(&c->m);
    sa_wait(&w, WK_CHANNEL);
    sa_mutex_lock(&c->m);
    if (!A_LOAD(&w.fired)) { /* cancelled: the value was never taken, so nothing is lost */
        sa_list_unlink(&c->sq_h, &c->sq_t, &w);
        A_DEC(&rt->st_chan_waiters);
        sa_mutex_unlock(&c->m);
        return sa_cancelled_here();
    }
    sa_mutex_unlock(&c->m);
    return w.status; /* Ok (value accepted) or Closed (value not delivered) */
}

static int64_t sa_chan_recv(SaChan* c, int wait, int64_t* out) {
    SaRuntime* rt = c->obj.rt;
    sa_mutex_lock(&c->m);
    if (c->count > 0) {
        int64_t v = sa_chan_pop(c);
        SaWaiter* s = sa_list_pop(&c->sq_h, &c->sq_t);
        if (s) { /* room appeared: move the oldest blocked sender's value in and release it */
            A_DEC(&rt->st_chan_waiters);
            sa_chan_push(c, s->value);
            s->status = SA_OK;
            sa_waiter_fire(s);
        }
        sa_mutex_unlock(&c->m);
        *out = v;
        sa_coop();
        return SA_OK;
    }
    if (c->closed) {
        sa_mutex_unlock(&c->m);
        return SA_ECLOSED;
    }
    if (!wait) {
        sa_mutex_unlock(&c->m);
        return SA_EWOULDBLOCK;
    }
    SaTask* self = sa_current_task();
    if (self && A_LOAD(&self->cancel_req)) {
        sa_mutex_unlock(&c->m);
        return sa_cancelled_here();
    }
    SaWaiter w;
    sa_waiter_init(&w);
    sa_list_push(&c->rq_h, &c->rq_t, &w);
    A_INC(&rt->st_chan_waiters);
    sa_mutex_unlock(&c->m);
    sa_wait(&w, WK_CHANNEL);
    sa_mutex_lock(&c->m);
    if (!A_LOAD(&w.fired)) {
        sa_list_unlink(&c->rq_h, &c->rq_t, &w);
        A_DEC(&rt->st_chan_waiters);
        sa_mutex_unlock(&c->m);
        return sa_cancelled_here();
    }
    sa_mutex_unlock(&c->m);
    if (w.status == SA_OK) *out = w.value;
    return w.status;
}

static int64_t sa_chan_close_locked_fire(SaChan* c) {
    SaRuntime* rt = c->obj.rt;
    if (c->closed) return SA_ECLOSED;
    c->closed = 1;
    SaWaiter* w;
    while ((w = sa_list_pop(&c->rq_h, &c->rq_t)) != NULL) {
        A_DEC(&rt->st_chan_waiters);
        w->status = SA_ECLOSED;
        sa_waiter_fire(w);
    }
    while ((w = sa_list_pop(&c->sq_h, &c->sq_t)) != NULL) {
        A_DEC(&rt->st_chan_waiters);
        w->status = SA_ECLOSED;
        sa_waiter_fire(w);
    }
    return SA_OK;
}

static SaChan* sa_chan_get(int64_t h, int text, int64_t* err) {
    SaChan* c = (SaChan*)sa_lookup(h, OT_CHAN);
    if (!c) {
        *err = SA_EHANDLE;
        return NULL;
    }
    if (c->text != text) { /* int operation on a text channel or vice versa */
        sa_obj_unref(&c->obj);
        *err = SA_EINVAL;
        return NULL;
    }
    return c;
}

static int64_t sa_chan_send_int(int64_t h, int64_t v, int wait) {
    int64_t err;
    SaChan* c = sa_chan_get(h, 0, &err);
    if (!c) return err;
    int64_t r = sa_chan_send(c, v, wait);
    sa_obj_unref(&c->obj);
    return r;
}

static int64_t sa_chan_recv_int(int64_t h, int wait) {
    int64_t err, v = 0;
    SaChan* c = sa_chan_get(h, 0, &err);
    if (!c) return err;
    int64_t r = sa_chan_recv(c, wait, &v);
    sa_obj_unref(&c->obj);
    if (r == SA_OK) sa_set_last(v);
    return r;
}

static int64_t sa_chan_send_text(int64_t h, const char* s, int wait) {
    if (!s) return SA_EINVAL;
    int64_t err;
    SaChan* c = sa_chan_get(h, 1, &err);
    if (!c) return err;
    size_t n = strlen(s);
    char* copy = (char*)malloc(n + 1);
    if (!copy) {
        sa_obj_unref(&c->obj);
        return SA_EOS;
    }
    memcpy(copy, s, n + 1);
    int64_t r = sa_chan_send(c, (int64_t)(intptr_t)copy, wait);
    if (r != SA_OK) free(copy); /* not delivered: the sender still owns its copy */
    sa_obj_unref(&c->obj);
    return r;
}

static int64_t sa_chan_recv_text(int64_t h, int wait) {
    int64_t err, v = 0;
    SaChan* c = sa_chan_get(h, 1, &err);
    if (!c) return err;
    int64_t r = sa_chan_recv(c, wait, &v);
    sa_obj_unref(&c->obj);
    if (r == SA_OK) sa_set_last_text((char*)(intptr_t)v);
    return r;
}

int64_t salivo_aio_chan_send(int64_t h, int64_t v) { return sa_chan_send_int(h, v, 1); }
int64_t salivo_aio_chan_try_send(int64_t h, int64_t v) { return sa_chan_send_int(h, v, 0); }
int64_t salivo_aio_chan_recv(int64_t h) { return sa_chan_recv_int(h, 1); }
int64_t salivo_aio_chan_try_recv(int64_t h) { return sa_chan_recv_int(h, 0); }
int64_t salivo_aio_chan_send_text(int64_t h, const char* s) { return sa_chan_send_text(h, s, 1); }
int64_t salivo_aio_chan_try_send_text(int64_t h, const char* s) { return sa_chan_send_text(h, s, 0); }
int64_t salivo_aio_chan_recv_text(int64_t h) { return sa_chan_recv_text(h, 1); }
int64_t salivo_aio_chan_try_recv_text(int64_t h) { return sa_chan_recv_text(h, 0); }

/* The text received by the caller's last successful text receive; ownership passes to the
 * caller. A second call returns "" (the text is handed out once). */
char* salivo_aio_last_text_str(void) {
    SaTask* t = sa_current_task();
    char** slot = t ? &t->last_text : sa_tls_last_text();
    char* s = *slot;
    *slot = NULL;
    if (!s) {
        s = (char*)malloc(1);
        if (s) s[0] = 0;
    }
    return s;
}

int64_t salivo_aio_chan_close(int64_t h) {
    SaChan* c = (SaChan*)sa_lookup(h, OT_CHAN);
    if (!c) return SA_EHANDLE;
    sa_mutex_lock(&c->m);
    int64_t r = sa_chan_close_locked_fire(c);
    sa_mutex_unlock(&c->m);
    sa_obj_unref(&c->obj);
    return r;
}

int64_t salivo_aio_chan_len(int64_t h) {
    SaChan* c = (SaChan*)sa_lookup(h, OT_CHAN);
    if (!c) return SA_EHANDLE;
    sa_mutex_lock(&c->m);
    int64_t n = c->count;
    sa_mutex_unlock(&c->m);
    sa_obj_unref(&c->obj);
    return n;
}

/* ============================================================================================
 * Blocking detector hook: legacy blocking primitives report themselves when called on a worker
 * ========================================================================================== */
static void sa_blocking_hook(const char* what) {
    SaWorker* w = sa_tls_worker();
    if (!w || !w->current) return;
    A_INC(&w->rt->st_blocking);
    if (w->rt->detect_ms > 0)
        fprintf(stderr, "[salivo-aio] blocking operation '%s' invoked from async worker %d\n", what, w->id);
}

/* ============================================================================================
 * sysmon: one monitor thread per runtime. While any task runs, it wakes every half time slice,
 * asks a task that has held its worker longer than the slice to yield (salivo_aio_safepoint),
 * and reports tasks that exceed the blocking-detector threshold. With no task running it sleeps
 * on a condition variable (no idle CPU).
 * ========================================================================================== */
static void sa_mon_kick(SaRuntime* rt) {
    if (A_LOAD_SC(&rt->mon_sleeping)) {
        sa_mutex_lock(&rt->mon_lock);
        sa_cond_signal(&rt->mon_cond);
        sa_mutex_unlock(&rt->mon_lock);
    }
}

SA_THREAD_FN(sa_mon_main) {
    SaRuntime* rt = (SaRuntime*)arg;
    A_INC(&rt->st_os_threads);
    int64_t tick = rt->slice_ms > 0 ? rt->slice_ms / 2 : rt->detect_ms / 2;
    if (tick < 1) tick = 1;
    sa_mutex_lock(&rt->mon_lock);
    while (!A_LOAD(&rt->mon_stop)) {
        A_STORE_SC(&rt->mon_sleeping, 1);
        if (A_LOAD_SC(&rt->st_running) == 0) {
            sa_cond_wait(&rt->mon_cond, &rt->mon_lock);
            A_STORE_SC(&rt->mon_sleeping, 0);
            continue;
        }
        A_STORE_SC(&rt->mon_sleeping, 0);
        sa_cond_wait_ms(&rt->mon_cond, &rt->mon_lock, tick);
        int64_t now = sa_now_ns();
        for (int i = 0; i < rt->nworkers; i++) {
            SaWorker* w = &rt->workers[i];
            int64_t since = A_LOAD(&w->running_since_ns);
            if (since == 0) continue;
            int64_t ran_ms = (now - since) / 1000000;
            if (rt->slice_ms > 0 && ran_ms >= rt->slice_ms && w->preempted_since != since) {
                w->preempted_since = since;
                A_STORE(&w->preempt, 1);
            }
            if (rt->detect_ms > 0 && ran_ms > rt->detect_ms && w->reported_since_ns != since) {
                w->reported_since_ns = since;
                A_INC(&rt->st_blocking);
                fprintf(stderr, "[salivo-aio] blocking detected: worker %d ran one task for %lld ms without suspending\n",
                        i, (long long)ran_ms);
            }
        }
    }
    sa_mutex_unlock(&rt->mon_lock);
    A_DEC(&rt->st_os_threads);
    return 0;
}

/* ============================================================================================
 * Runtime lifecycle
 * ========================================================================================== */
SA_THREAD_FN(sa_worker_main) {
    SaWorker* w = (SaWorker*)arg;
    SaRuntime* rt = w->rt;
    A_INC(&rt->st_os_threads);
    sa_tls_set_worker(w);
    w->sched_fiber = sa_fiber_thread_enter();
    for (;;) {
        SaTask* t = sa_next_task(rt, w);
        if (t) {
            sa_run_task(rt, w, t);
            continue;
        }
        sa_mutex_lock(&rt->idle_lock);
        A_ADD_SC(&rt->nidle, 1);
        int exit_now = 0;
        for (;;) {
            if (A_LOAD_SC(&rt->runnable) > 0) break;
            if (A_LOAD(&rt->workers_exit) && A_LOAD(&rt->live_tasks) == 0) {
                exit_now = 1;
                break;
            }
            sa_cond_wait(&rt->idle_cond, &rt->idle_lock);
        }
        A_SUB(&rt->nidle, 1);
        sa_mutex_unlock(&rt->idle_lock);
        if (exit_now) break;
    }
    for (int i = 0; i < w->nfibers; i++) {
        sa_fiber_free(w->fibers[i]);
        A_DEC(&rt->st_fibers);
    }
    w->nfibers = 0;
    sa_fiber_thread_leave(w->sched_fiber);
    w->sched_fiber = NULL;
    sa_tls_set_worker(NULL);
    A_DEC(&rt->st_os_threads);
    return 0;
}

SA_THREAD_FN(sa_reactor_main) {
    SaRuntime* rt = (SaRuntime*)arg;
    A_INC(&rt->st_os_threads);
    for (;;) {
        int64_t next = sa_timers_run(rt);
        int64_t timeout = -1;
        if (next != INT64_MAX) {
            int64_t d = next - sa_now_ns();
            timeout = d <= 0 ? 0 : (d + 999999) / 1000000;
        }
        if (!sa_reactor_poll(rt, timeout)) break;
    }
    A_DEC(&rt->st_os_threads);
    return 0;
}

static int64_t sa_env_int(const char* name, int64_t dflt) {
    const char* v = getenv(name);
    if (!v || !*v) return dflt;
    return strtoll(v, NULL, 10);
}

int64_t salivo_aio_abi_version(void) { return SA_ABI_VERSION; }

int64_t salivo_aio_create(int64_t workers) {
    if (workers < 0 || workers > 256) return SA_EINVAL;
    SaRuntime* rt = (SaRuntime*)calloc(1, sizeof(SaRuntime));
    if (!rt) return SA_EOS;
    rt->nworkers = workers == 0 ? sa_cpu_count() : (int32_t)workers;
    rt->workers = (SaWorker*)calloc((size_t)rt->nworkers, sizeof(SaWorker));
    if (!rt->workers) {
        free(rt);
        return SA_EOS;
    }
    int64_t kb = sa_env_int("SALIVO_AIO_STACK_KB", 256);
    rt->stack_size = (size_t)(kb < 64 ? 64 : kb) * 1024;
    rt->pool_nthreads = (int32_t)sa_env_int("SALIVO_AIO_BLOCKING_THREADS", 4);
    if (rt->pool_nthreads < 1) rt->pool_nthreads = 1;
    rt->pool_cap = sa_env_int("SALIVO_AIO_BLOCKING_QUEUE", 256);
    if (rt->pool_cap < 1) rt->pool_cap = 1;
    rt->detect_ms = sa_env_int("SALIVO_AIO_DETECT_BLOCKING", 0);
    rt->slice_ms = sa_env_int("SALIVO_AIO_TIMESLICE_MS", 10);
    if (rt->slice_ms < 0) rt->slice_ms = 0;
    for (int i = 0; i < rt->nworkers; i++) {
        SaWorker* w = &rt->workers[i];
        w->rt = rt;
        w->id = i;
        w->rng = 0x9E3779B97F4A7C15ULL ^ (uint64_t)(i + 1);
        sa_mutex_init(&w->qlock);
    }
    sa_mutex_init(&rt->gq_lock);
    sa_mutex_init(&rt->idle_lock);
    sa_cond_init(&rt->idle_cond);
    sa_mutex_init(&rt->tasks_lock);
    sa_cond_init(&rt->tasks_cond);
    sa_rw_init(&rt->tab_lock);
    rt->free_head = -1;
    sa_mutex_init(&rt->tm_lock);
    rt->reactor_deadline_ns = INT64_MAX;
    sa_mutex_init(&rt->pool_lock);
    sa_cond_init(&rt->pool_cond);
    sa_mutex_init(&rt->mon_lock);
    sa_cond_init(&rt->mon_cond);
    rt->state = RT_CREATED;

    sa_mutex_lock(&g_rt_lock);
    int slot = -1;
    for (int i = 0; i < SA_MAX_RUNTIMES; i++) {
        if (!g_rts[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        sa_mutex_unlock(&g_rt_lock);
        free(rt->workers);
        free(rt);
        return SA_EINVAL; /* registry full: at most 32 live runtimes */
    }
    g_rt_inc[slot] = (g_rt_inc[slot] + 1) & 0x1FFF;
    if (g_rt_inc[slot] == 0) g_rt_inc[slot] = 1;
    rt->slot = slot;
    rt->inc = g_rt_inc[slot];
    rt->handle = RT_HANDLE(rt->inc, slot);
    g_rts[slot] = rt;
    sa_mutex_unlock(&g_rt_lock);
    return rt->handle;
}

int64_t salivo_aio_use(int64_t h) {
    if (h == 0) {
        *sa_tls_default_rt() = 0;
        return SA_OK;
    }
    if (!sa_rt_from(h)) return SA_EHANDLE;
    *sa_tls_default_rt() = h;
    return SA_OK;
}

static void sa_stop_threads(SaRuntime* rt, int nworkers_started) {
    A_STORE(&rt->workers_exit, 1);
    sa_mutex_lock(&rt->idle_lock);
    sa_cond_broadcast(&rt->idle_cond);
    sa_mutex_unlock(&rt->idle_lock);
    for (int i = 0; i < nworkers_started; i++) sa_thread_join(rt->workers[i].th);

    if (rt->mon_started) {
        sa_mutex_lock(&rt->mon_lock);
        A_STORE(&rt->mon_stop, 1);
        sa_cond_broadcast(&rt->mon_cond);
        sa_mutex_unlock(&rt->mon_lock);
        sa_thread_join(rt->mon_th);
        rt->mon_started = 0;
    }

    sa_mutex_lock(&rt->pool_lock);
    rt->pool_stop = 1;
    sa_cond_broadcast(&rt->pool_cond);
    sa_mutex_unlock(&rt->pool_lock);
    for (int i = 0; i < rt->st_pool_threads; i++) sa_thread_join(rt->pool_th[i]);
    rt->st_pool_threads = 0;
    free(rt->pool_th);
    rt->pool_th = NULL;

    A_STORE(&rt->reactor_stop, 1);
    sa_reactor_notify(rt);
    sa_thread_join(rt->reactor_th);
    A_STORE(&rt->reactor_running, 0);
}

static void sa_hook_ref(int delta) {
    sa_mutex_lock(&g_rt_lock);
    g_started_runtimes += delta;
    salivo_aio_blocking_hook = g_started_runtimes > 0 ? sa_blocking_hook : NULL;
    sa_mutex_unlock(&g_rt_lock);
}

int64_t salivo_aio_start(int64_t h) {
    SaRuntime* rt = sa_rt_from(h);
    if (!rt) return SA_EHANDLE;
    if (rt->state != RT_CREATED) return SA_EINVAL;
    if (!sa_net_init()) return SA_EOS;
    if (!sa_reactor_init(rt)) {
        sa_net_fini();
        return SA_EOS;
    }
    rt->net_inited = 1;
    A_STORE(&rt->reactor_running, 1);
    int started = 0;
    if (!sa_thread_start(&rt->reactor_th, sa_reactor_main, rt)) {
        A_STORE(&rt->reactor_running, 0);
        sa_reactor_fini(rt);
        sa_net_fini();
        rt->net_inited = 0;
        return SA_EOS;
    }
    rt->pool_th = (sa_thread*)calloc((size_t)rt->pool_nthreads, sizeof(sa_thread));
    if (!rt->pool_th) goto fail;
    for (int i = 0; i < rt->pool_nthreads; i++) {
        if (!sa_thread_start(&rt->pool_th[i], sa_pool_main, rt)) goto fail;
        rt->st_pool_threads++;
    }
    if (rt->slice_ms > 0 || rt->detect_ms > 0) {
        if (!sa_thread_start(&rt->mon_th, sa_mon_main, rt)) goto fail;
        rt->mon_started = 1;
    }
    for (int i = 0; i < rt->nworkers; i++) {
        if (!sa_thread_start(&rt->workers[i].th, sa_worker_main, &rt->workers[i])) goto fail;
        started++;
    }
    rt->state = RT_STARTED;
    sa_hook_ref(1);
    return SA_OK;

fail:
    sa_stop_threads(rt, started);
    sa_reactor_fini(rt);
    sa_net_fini();
    rt->net_inited = 0;
    return SA_EOS;
}

int64_t salivo_aio_run(int64_t h, int64_t fn, int64_t arg) {
    SaRuntime* rt = sa_rt_from(h);
    if (!rt) return SA_EHANDLE;
    if (sa_tls_worker()) return SA_EINVAL; /* would park a worker thread */
    if (rt->state == RT_CREATED) {
        int64_t s = salivo_aio_start(h);
        if (s != SA_OK) return s;
    }
    int64_t th = sa_spawn_in(rt, fn, arg);
    if (th < 0) return th;
    int64_t r = salivo_aio_await(th);
    salivo_aio_release(th);
    return r;
}

int64_t salivo_aio_shutdown(int64_t h) {
    SaRuntime* rt = sa_rt_from(h);
    if (!rt) return SA_EHANDLE;
    if (rt->state != RT_STARTED) return SA_EINVAL;
    sa_mutex_lock(&rt->tasks_lock);
    A_STORE(&rt->shutting_down, 1);
    /* Tasks stay on the list until finalized under this lock, so each pointer is alive here. */
    for (SaTask* t = rt->all_tasks; t; t = t->all_next) sa_task_request_cancel(t);
    sa_mutex_unlock(&rt->tasks_lock);
    return SA_OK;
}

int64_t salivo_aio_join(int64_t h, int64_t timeout_ms) {
    SaRuntime* rt = sa_rt_from(h);
    if (!rt) return SA_EHANDLE;
    if (sa_tls_worker()) return SA_EINVAL; /* would park a worker thread */
    if (rt->state == RT_JOINED) return SA_EINVAL;
    if (rt->state == RT_CREATED) {
        rt->state = RT_JOINED;
        return SA_OK;
    }
    int64_t deadline = timeout_ms >= 0 ? sa_now_ns() + timeout_ms * 1000000 : INT64_MAX;
    sa_mutex_lock(&rt->tasks_lock);
    while (A_LOAD(&rt->live_tasks) > 0) {
        int64_t left = deadline == INT64_MAX ? -1 : (deadline - sa_now_ns()) / 1000000;
        if (deadline != INT64_MAX && left <= 0) {
            sa_mutex_unlock(&rt->tasks_lock);
            return SA_ETIMEOUT;
        }
        sa_cond_wait_ms(&rt->tasks_cond, &rt->tasks_lock, left);
    }
    A_STORE(&rt->shutting_down, 1); /* no task exists and none can be spawned now */
    sa_mutex_unlock(&rt->tasks_lock);
    sa_stop_threads(rt, rt->nworkers);
    sa_hook_ref(-1);
    rt->state = RT_JOINED;
    return SA_OK;
}

int64_t salivo_aio_destroy(int64_t h) {
    sa_mutex_lock(&g_rt_lock);
    SaRuntime* rt = sa_rt_from(h);
    if (!rt) {
        sa_mutex_unlock(&g_rt_lock);
        return SA_EHANDLE;
    }
    if (rt->state == RT_STARTED) {
        sa_mutex_unlock(&g_rt_lock);
        return SA_EINVAL; /* join first */
    }
    g_rts[rt->slot] = NULL; /* handles of this runtime stop resolving from here on */
    sa_mutex_unlock(&g_rt_lock);
    /* Reclaim every object still reachable through a handle (unreleased tasks, open sockets,
     * files and channels). Pending I/O is impossible here: every task has finished. Each one is
     * a leak: it is counted before reclamation so freeing it cannot hide it. */
    int64_t leaks = 0;
    for (int32_t i = 0; i < rt->tab_used; i++) {
        SaObj* o = rt->slots[i].obj;
        if (o) {
            rt->slots[i].obj = NULL;
            sa_obj_unref(o);
            leaks++;
        }
    }
    leaks += A_LOAD(&rt->st_objects) + A_LOAD(&rt->st_fibers) + rt->heap_n + rt->pool_n;
    if (rt->reactor) sa_reactor_fini(rt);
    if (rt->net_inited) sa_net_fini();
    for (int i = 0; i < rt->nworkers; i++) sa_mutex_destroy(&rt->workers[i].qlock);
    sa_mutex_destroy(&rt->gq_lock);
    sa_mutex_destroy(&rt->idle_lock);
    sa_cond_destroy(&rt->idle_cond);
    sa_mutex_destroy(&rt->tasks_lock);
    sa_cond_destroy(&rt->tasks_cond);
    sa_rw_destroy(&rt->tab_lock);
    sa_mutex_destroy(&rt->tm_lock);
    sa_mutex_destroy(&rt->pool_lock);
    sa_cond_destroy(&rt->pool_cond);
    sa_mutex_destroy(&rt->mon_lock);
    sa_cond_destroy(&rt->mon_cond);
    free(rt->slots);
    free(rt->heap);
    free(rt->workers);
    __atomic_store_n(&g_last_destroy_leaks, leaks, __ATOMIC_RELEASE);
    if (*sa_tls_default_rt() == h) *sa_tls_default_rt() = 0;
    free(rt);
    return leaks == 0 ? SA_OK : SA_EOS;
}

int64_t salivo_aio_stat(int64_t h, int64_t key) {
    if (key == 18) return A_LOAD(&g_buffers_live);
    if (key == 23) return A_LOAD(&g_last_destroy_leaks);
    if (key == 25) return sa_process_cpu_ns() / 1000; /* process CPU microseconds (idle measurement) */
    if (key == 26) return sa_now_ns() / 1000;         /* monotonic microseconds */
    if (key == 27) return sa_process_mem(0);          /* peak resident set, bytes */
    if (key == 28) return sa_process_mem(1);          /* private/resident memory now, bytes */
    if (key == 30) {                                  /* live runtimes in the process */
        int n = 0;
        for (int i = 0; i < SA_MAX_RUNTIMES; i++) n += g_rts[i] != NULL;
        return n;
    }
    SaRuntime* rt = sa_rt_from(h);
    if (!rt) return SA_EHANDLE;
    if (key >= 100 && key < 100 + rt->nworkers) return A_LOAD(&rt->workers[key - 100].runs);
    if (key >= 200 && key < 200 + rt->nworkers) return A_LOAD(&rt->workers[key - 200].busy_ns) / 1000;
    switch (key) {
    case 0: return rt->nworkers;
    case 1: return A_LOAD(&rt->live_tasks);
    case 2: return A_LOAD(&rt->runnable);
    case 3: return A_LOAD(&rt->st_waiting);
    case 4: return A_LOAD(&rt->st_pending_io);
    case 5: {
        sa_mutex_lock(&rt->tm_lock);
        int64_t n = rt->heap_n;
        sa_mutex_unlock(&rt->tm_lock);
        return n;
    }
    case 6: return A_LOAD(&rt->st_chan_waiters);
    case 7: return A_LOAD(&rt->st_created);
    case 8: return A_LOAD(&rt->st_destroyed);
    case 9: return A_LOAD(&rt->st_objects);
    case 10: {
        sa_mutex_lock(&rt->pool_lock);
        int64_t n = rt->pool_n;
        sa_mutex_unlock(&rt->pool_lock);
        return n;
    }
    case 11: return A_LOAD(&rt->st_steals);
    case 12: return A_LOAD(&rt->st_switches);
    case 13: return A_LOAD(&rt->st_violations);
    case 14: return A_LOAD(&rt->st_fibers);
    case 15: return A_LOAD(&rt->st_max_running);
    case 16: return A_LOAD(&rt->st_blocking);
    case 17: return A_LOAD(&rt->st_os_threads);
    case 19: return rt->st_pool_threads;
    case 20: return A_LOAD(&rt->st_chans);
    case 21: return A_LOAD(&rt->st_socks);
    case 22: return A_LOAD(&rt->st_files);
    case 24: return A_LOAD(&rt->st_timers_fired);
    case 29: return A_LOAD(&rt->st_preemptions);
    default: return SA_EINVAL;
    }
}

int64_t salivo_aio_last_value(void) {
    SaTask* t = sa_current_task();
    return t ? t->last_value : *sa_tls_last_value();
}

int64_t salivo_aio_last_os_error(void) {
    SaTask* t = sa_current_task();
    return t ? t->last_os_error : *sa_tls_last_os_error();
}

char* salivo_aio_last_peer_str(void) {
    SaTask* t = sa_current_task();
    const char* src = t ? t->last_peer : sa_tls_last_peer();
    size_t n = strlen(src);
    char* s = (char*)malloc(n + 1);
    if (s) memcpy(s, src, n + 1);
    return s;
}

int64_t salivo_aio_worker_id(void) {
    SaWorker* w = sa_tls_worker();
    return w ? w->id : -1;
}

/* ============================================================================================
 * Byte buffers (caller-owned memory for I/O). A 16-byte header records the capacity so every
 * access is bounds-checked.
 * ========================================================================================== */
#define SA_BUF_MAGIC 0x53414C4956304246LL

static int64_t sa_buf_cap(int64_t buf) {
    if (buf <= 16) return -1;
    int64_t* hdr = (int64_t*)(intptr_t)(buf - 16);
    return hdr[0] == SA_BUF_MAGIC ? hdr[1] : -1;
}

int64_t salivo_aio_buf_new(int64_t cap) {
    if (cap <= 0 || cap > (1LL << 30)) return SA_EINVAL;
    int64_t* hdr = (int64_t*)calloc(1, (size_t)cap + 16);
    if (!hdr) return SA_EOS;
    hdr[0] = SA_BUF_MAGIC;
    hdr[1] = cap;
    A_INC(&g_buffers_live);
    return (int64_t)(intptr_t)hdr + 16;
}

int64_t salivo_aio_buf_free(int64_t buf) {
    if (sa_buf_cap(buf) < 0) return SA_EINVAL;
    int64_t* hdr = (int64_t*)(intptr_t)(buf - 16);
    hdr[0] = 0;
    free(hdr);
    A_DEC(&g_buffers_live);
    return SA_OK;
}

int64_t salivo_aio_buf_get(int64_t buf, int64_t i) {
    int64_t cap = sa_buf_cap(buf);
    if (cap < 0 || i < 0 || i >= cap) return SA_EINVAL;
    return ((unsigned char*)(intptr_t)buf)[i];
}

int64_t salivo_aio_buf_set(int64_t buf, int64_t i, int64_t v) {
    int64_t cap = sa_buf_cap(buf);
    if (cap < 0 || i < 0 || i >= cap || v < 0 || v > 255) return SA_EINVAL;
    ((unsigned char*)(intptr_t)buf)[i] = (unsigned char)v;
    return SA_OK;
}

int64_t salivo_aio_buf_put_text(int64_t buf, int64_t cap, const char* text) {
    int64_t real = sa_buf_cap(buf);
    if (real < 0 || cap < 0 || !text) return SA_EINVAL;
    if (cap > real) cap = real;
    int64_t n = (int64_t)strlen(text);
    if (n > cap) n = cap;
    memcpy((void*)(intptr_t)buf, text, (size_t)n);
    return n;
}

char* salivo_aio_buf_str(int64_t buf, int64_t len) {
    int64_t cap = sa_buf_cap(buf);
    if (cap < 0 || len < 0) len = 0;
    if (len > cap) len = cap;
    char* s = (char*)malloc((size_t)len + 1);
    if (!s) return NULL;
    if (len > 0) memcpy(s, (void*)(intptr_t)buf, (size_t)len);
    s[len] = 0;
    return s;
}

/* ============================================================================================
 * Argument blocks for `async func`: the compiler-generated wrapper packs the call's arguments
 * into a block, spawns the entry thunk with the block, and the thunk unpacks and frees it. Text
 * arguments are copied (the caller's string may not outlive the call); reading one hands the copy
 * to the task.
 * ========================================================================================== */
#define SA_ARG_MAGIC 0x53414C4941524753LL

typedef struct {
    int64_t magic;
    int64_t n;
    int64_t vals[1]; /* n slots, followed by n kind bytes (0 int, 1 owned text) */
} SaArgs;

static unsigned char* sa_arg_kinds(SaArgs* a) { return (unsigned char*)&a->vals[a->n]; }

static SaArgs* sa_args_of(int64_t b, int64_t i) {
    SaArgs* a = (SaArgs*)(intptr_t)b;
    if (!a || a->magic != SA_ARG_MAGIC || i < 0 || i >= a->n) return NULL;
    return a;
}

int64_t salivo_aio_arg_new(int64_t n) {
    if (n < 0 || n > 64) return SA_EINVAL;
    size_t sz = sizeof(SaArgs) + sizeof(int64_t) * (size_t)(n > 0 ? n - 1 : 0) + (size_t)n;
    SaArgs* a = (SaArgs*)calloc(1, sz);
    if (!a) return SA_EOS;
    a->magic = SA_ARG_MAGIC;
    a->n = n;
    return (int64_t)(intptr_t)a;
}

int64_t salivo_aio_arg_set_int(int64_t b, int64_t i, int64_t v) {
    SaArgs* a = sa_args_of(b, i);
    if (!a) return SA_EINVAL;
    if (sa_arg_kinds(a)[i] == 1) free((char*)(intptr_t)a->vals[i]);
    a->vals[i] = v;
    sa_arg_kinds(a)[i] = 0;
    return SA_OK;
}

int64_t salivo_aio_arg_set_text(int64_t b, int64_t i, const char* s) {
    SaArgs* a = sa_args_of(b, i);
    if (!a || !s) return SA_EINVAL;
    size_t n = strlen(s);
    char* copy = (char*)malloc(n + 1);
    if (!copy) return SA_EOS;
    memcpy(copy, s, n + 1);
    if (sa_arg_kinds(a)[i] == 1) free((char*)(intptr_t)a->vals[i]);
    a->vals[i] = (int64_t)(intptr_t)copy;
    sa_arg_kinds(a)[i] = 1;
    return SA_OK;
}

int64_t salivo_aio_arg_int(int64_t b, int64_t i) {
    SaArgs* a = sa_args_of(b, i);
    return a && sa_arg_kinds(a)[i] == 0 ? a->vals[i] : 0;
}

char* salivo_aio_arg_get_str(int64_t b, int64_t i) {
    SaArgs* a = sa_args_of(b, i);
    if (a && sa_arg_kinds(a)[i] == 1) {
        char* s = (char*)(intptr_t)a->vals[i];
        a->vals[i] = 0;
        sa_arg_kinds(a)[i] = 0; /* ownership moves to the task */
        return s;
    }
    char* e = (char*)malloc(1);
    if (e) e[0] = 0;
    return e;
}

int64_t salivo_aio_arg_free(int64_t b) {
    SaArgs* a = (SaArgs*)(intptr_t)b;
    if (!a || a->magic != SA_ARG_MAGIC) return SA_EINVAL;
    for (int64_t i = 0; i < a->n; i++)
        if (sa_arg_kinds(a)[i] == 1) free((char*)(intptr_t)a->vals[i]);
    a->magic = 0;
    free(a);
    return SA_OK;
}

char* salivo_aio_error_name_str(int64_t code) {
    static const char* names[] = {"Ok", "WouldBlock", "Eof", "ConnectionReset", "Timeout", "Cancelled",
                                  "Closed", "InvalidArgument", "Os", "Shutdown", "InvalidHandle", "Full",
                                  "Unsupported", "NotInTask", "ConnectionRefused", "AddressInUse",
                                  "Truncated", "TaskFailed", "BadMessage", "InvalidHeader", "InvalidFraming",
                                  "HeaderTooLarge", "BodyTooLarge", "VersionNotSupported", "ProtocolError",
                                  "StreamReset", "FlowControl", "Compression", "Refused", "GoingAway"};
    if (code > 0) code = 0;
    if (-code >= (int64_t)(sizeof(names) / sizeof(names[0]))) return (char*)"Unknown";
    return (char*)names[-code];
}

/* ============================================================================================
 * Address helpers shared by both I/O backends (IPv4 and IPv6)
 * ========================================================================================== */
typedef struct {
    struct sockaddr_storage ss;
    int len;
} SaAddr;

static int64_t sa_resolve_job(SaJob* j) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    int r = getaddrinfo((const char*)j->q, NULL, &hints, &res);
    if (r != 0 || !res) {
        j->os_error = r;
        return SA_EINVAL;
    }
    SaAddr* a = (SaAddr*)j->p;
    memcpy(&a->ss, res->ai_addr, res->ai_addrlen);
    a->len = (int)res->ai_addrlen;
    freeaddrinfo(res);
    return SA_OK;
}

static void sa_addr_set_port(SaAddr* a, int port) {
    if (a->ss.ss_family == AF_INET6) ((struct sockaddr_in6*)&a->ss)->sin6_port = htons((unsigned short)port);
    else ((struct sockaddr_in*)&a->ss)->sin_port = htons((unsigned short)port);
}

/* Numeric IPv4/IPv6 and "localhost" resolve inline; other names use getaddrinfo on the blocking
 * pool. "" / "0.0.0.0" is the IPv4 wildcard, "::" the IPv6 wildcard. Brackets ("[::1]") are
 * accepted. */
static int64_t sa_resolve(SaRuntime* rt, const char* host, int64_t port, SaAddr* out) {
    if (!host || port < 0 || port > 65535) return SA_EINVAL;
    memset(out, 0, sizeof(*out));
    char buf[256];
    size_t n = strlen(host);
    if (n >= sizeof(buf)) return SA_EINVAL;
    if (n >= 2 && host[0] == '[' && host[n - 1] == ']') {
        memcpy(buf, host + 1, n - 2);
        buf[n - 2] = 0;
    } else {
        memcpy(buf, host, n + 1);
    }
    struct sockaddr_in* v4 = (struct sockaddr_in*)&out->ss;
    struct sockaddr_in6* v6 = (struct sockaddr_in6*)&out->ss;
    if (!buf[0] || strcmp(buf, "0.0.0.0") == 0) {
        v4->sin_family = AF_INET;
        v4->sin_addr.s_addr = htonl(INADDR_ANY);
        out->len = sizeof(*v4);
    } else if (strcmp(buf, "localhost") == 0) {
        v4->sin_family = AF_INET;
        v4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        out->len = sizeof(*v4);
    } else if (inet_pton(AF_INET, buf, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        out->len = sizeof(*v4);
    } else if (inet_pton(AF_INET6, buf, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        out->len = sizeof(*v6);
    } else {
        SaJob j;
        memset(&j, 0, sizeof(j));
        j.run = sa_resolve_job;
        j.q = buf;
        j.p = out;
        int64_t r = sa_pool_run(rt, &j);
        if (r != SA_OK) return r;
    }
    sa_addr_set_port(out, (int)port);
    return SA_OK;
}

static int sa_addr_is_any(const SaAddr* a) {
    if (a->ss.ss_family == AF_INET6) {
        static const unsigned char zero[16];
        return memcmp(&((const struct sockaddr_in6*)&a->ss)->sin6_addr, zero, 16) == 0;
    }
    return ((const struct sockaddr_in*)&a->ss)->sin_addr.s_addr == htonl(INADDR_ANY);
}

/* Records a datagram sender: rtaiolastvalue = ipv4 * 65536 + port (port alone for IPv6),
 * rtaiolastpeer = textual address ("127.0.0.1", "::1"). */
static void sa_record_peer(const struct sockaddr_storage* ss) {
    char text[SA_PEER_TEXT] = "";
    int64_t ep = 0;
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6* a = (const struct sockaddr_in6*)ss;
        inet_ntop(AF_INET6, (void*)&a->sin6_addr, text, sizeof(text));
        ep = ntohs(a->sin6_port);
    } else {
        const struct sockaddr_in* a = (const struct sockaddr_in*)ss;
        inet_ntop(AF_INET, (void*)&a->sin_addr, text, sizeof(text));
        ep = ((int64_t)ntohl(a->sin_addr.s_addr) << 16) | (int64_t)ntohs(a->sin_port);
    }
    sa_set_last(ep);
    sa_set_last_peer(text);
}

static int sa_sockaddr_port(const struct sockaddr_storage* ss) {
    return ss->ss_family == AF_INET6 ? ntohs(((const struct sockaddr_in6*)ss)->sin6_port)
                                     : ntohs(((const struct sockaddr_in*)ss)->sin_port);
}

#if defined(_WIN32)
#include "salivo_aio_io_win32.inc"
#else
#include "salivo_aio_io_posix.inc"
#endif
#include "salivo_aio_http.inc"

#else /* no async backend for this platform */
#include "salivo_aio_unsupported.inc"
#endif
