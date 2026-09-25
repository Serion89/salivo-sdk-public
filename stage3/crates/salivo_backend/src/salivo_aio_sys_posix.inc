/* Salivo Stage 36.1 async runtime - POSIX system primitives (Linux, macOS, BSD).
 *
 * Everything the portable core (salivo_aio_runtime.c) needs from the OS apart from I/O:
 * pthread locks/condition variables/threads, a monotonic clock, ucontext fibers on mmap'd stacks
 * with a guard page, and thread-local slots. I/O lives in salivo_aio_io_posix.inc.
 */
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700 /* ucontext on macOS */
#define _DARWIN_C_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/epoll.h>
#endif

typedef pthread_mutex_t sa_mutex;
typedef pthread_rwlock_t sa_rwlock;
typedef pthread_cond_t sa_cond;
typedef pthread_t sa_thread;

#define SA_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define sa_mutex_init(m) pthread_mutex_init((m), NULL)
#define sa_mutex_lock(m) pthread_mutex_lock(m)
#define sa_mutex_unlock(m) pthread_mutex_unlock(m)
#define sa_mutex_destroy(m) pthread_mutex_destroy(m)
#define sa_rw_init(m) pthread_rwlock_init((m), NULL)
#define sa_rw_rdlock(m) pthread_rwlock_rdlock(m)
#define sa_rw_rdunlock(m) pthread_rwlock_unlock(m)
#define sa_rw_wrlock(m) pthread_rwlock_wrlock(m)
#define sa_rw_wrunlock(m) pthread_rwlock_unlock(m)
#define sa_rw_destroy(m) pthread_rwlock_destroy(m)
#define sa_cond_signal(c) pthread_cond_signal(c)
#define sa_cond_broadcast(c) pthread_cond_broadcast(c)
#define sa_cond_wait(c, m) pthread_cond_wait((c), (m))
#define sa_cond_destroy(c) pthread_cond_destroy(c)

/* Timed waits use the monotonic clock where the platform allows it, so wall-clock jumps never
 * stretch or shorten a timeout. */
static void sa_cond_init(sa_cond* c) {
#if defined(__APPLE__)
    pthread_cond_init(c, NULL);
#else
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
#endif
}

/* Returns 0 when the wait timed out. */
static int sa_cond_wait_ms(sa_cond* c, sa_mutex* m, int64_t ms) {
    if (ms < 0) {
        pthread_cond_wait(c, m);
        return 1;
    }
#if defined(__APPLE__)
    struct timespec rel;
    rel.tv_sec = (time_t)(ms / 1000);
    rel.tv_nsec = (long)((ms % 1000) * 1000000);
    return pthread_cond_timedwait_relative_np(c, m, &rel) == ETIMEDOUT ? 0 : 1;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += (time_t)(ms / 1000);
    ts.tv_nsec += (long)((ms % 1000) * 1000000);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(c, m, &ts) == ETIMEDOUT ? 0 : 1;
#endif
}

#define SA_THREAD_FN(name) static void* name(void* arg)
typedef void* (*sa_thread_fn)(void*);

static int sa_thread_start(sa_thread* t, sa_thread_fn fn, void* arg) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 1 << 20);
    int ok = pthread_create(t, &a, fn, arg) == 0;
    pthread_attr_destroy(&a);
    return ok;
}

static void sa_thread_join(sa_thread t) { pthread_join(t, NULL); }

static void sa_thread_yield_os(void) { sched_yield(); }

static int64_t sa_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int sa_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : (int)n;
}

static int64_t sa_process_cpu_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Process memory (bytes): which = 0 peak resident set, 1 current resident set. */
static int64_t sa_process_mem(int which) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    int64_t peak = (int64_t)ru.ru_maxrss; /* bytes on macOS */
#else
    int64_t peak = (int64_t)ru.ru_maxrss * 1024; /* KiB on Linux/BSD */
#endif
    if (which == 0) return peak;
#if defined(__linux__)
    FILE* f = fopen("/proc/self/statm", "r");
    if (f) {
        long size = 0, resident = 0;
        int ok = fscanf(f, "%ld %ld", &size, &resident) == 2;
        fclose(f);
        if (ok) return (int64_t)resident * (int64_t)sysconf(_SC_PAGESIZE);
    }
#endif
    return peak;
}

/* ---- Fibers (ucontext) -------------------------------------------------------------------
 * A fiber may be resumed on any worker thread, as long as it never runs on two threads at once
 * (the scheduler guarantees that). Each thread tracks the fiber it is currently running so that
 * sa_fiber_switch knows where to save the outgoing context. */
#define SA_FIBER_CALL
typedef void (*sa_fiber_entry)(void*);

typedef struct SaFiberCtx {
    ucontext_t uc;
    void* stack; /* mmap'd region including the guard page; NULL for a thread's own context */
    size_t size;
    sa_fiber_entry entry;
} SaFiberCtx;

#define SA_NOINLINE __attribute__((noinline))
#define SA_TLS __thread

static SA_TLS SaFiberCtx* t_cur_fiber;
SA_NOINLINE static SaFiberCtx* sa_cur_fiber(void) { return t_cur_fiber; }
SA_NOINLINE static void sa_set_cur_fiber(SaFiberCtx* f) { t_cur_fiber = f; }

static void* sa_fiber_thread_enter(void) {
    SaFiberCtx* f = (SaFiberCtx*)calloc(1, sizeof(SaFiberCtx));
    sa_set_cur_fiber(f);
    return f;
}

static void sa_fiber_thread_leave(void* self) {
    sa_set_cur_fiber(NULL);
    free(self);
}

static void sa_fiber_trampoline(void) {
    SaFiberCtx* f = sa_cur_fiber();
    f->entry(NULL); /* never returns: the fiber body loops forever */
    abort();
}

static void* sa_fiber_new(size_t reserve, sa_fiber_entry entry) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t size = (reserve + page - 1) / page * page + page; /* + guard page */
    void* mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON
#ifdef MAP_NORESERVE
                     | MAP_NORESERVE
#endif
                     , -1, 0);
    if (mem == MAP_FAILED) return NULL;
    mprotect(mem, page, PROT_NONE); /* stack overflow faults instead of corrupting memory */
    SaFiberCtx* volatile f = (SaFiberCtx*)calloc(1, sizeof(SaFiberCtx)); /* getcontext returns twice */
    if (!f) {
        munmap(mem, size);
        return NULL;
    }
    f->stack = mem;
    f->size = size;
    f->entry = entry;
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp = (char*)mem + page;
    f->uc.uc_stack.ss_size = size - page;
    f->uc.uc_link = NULL;
    makecontext(&f->uc, sa_fiber_trampoline, 0);
    return f;
}

static void sa_fiber_free(void* p) {
    SaFiberCtx* f = (SaFiberCtx*)p;
    if (f->stack) munmap(f->stack, f->size);
    free(f);
}

SA_NOINLINE static void sa_fiber_switch(void* to) {
    SaFiberCtx* from = sa_cur_fiber();
    sa_set_cur_fiber((SaFiberCtx*)to);
    swapcontext(&from->uc, &((SaFiberCtx*)to)->uc);
    /* May now run on a different thread: nothing thread-local is used after the swap here. */
}
