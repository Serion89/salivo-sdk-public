/* Salivo Stage 36.1 async runtime - Windows system primitives.
 *
 * Everything the portable core (salivo_aio_runtime.c) needs from the OS apart from I/O:
 * locks, condition variables, OS threads, monotonic time, fibers and thread-local slots.
 * I/O (IOCP reactor, sockets, files) lives in salivo_aio_io_win32.inc.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <process.h>

typedef SRWLOCK sa_mutex;
typedef SRWLOCK sa_rwlock;
typedef CONDITION_VARIABLE sa_cond;
typedef HANDLE sa_thread;

#define SA_MUTEX_INIT SRWLOCK_INIT
#define sa_mutex_init(m) InitializeSRWLock(m)
#define sa_mutex_lock(m) AcquireSRWLockExclusive(m)
#define sa_mutex_unlock(m) ReleaseSRWLockExclusive(m)
#define sa_mutex_destroy(m) ((void)(m))
#define sa_rw_destroy(m) ((void)(m))
#define sa_cond_destroy(c) ((void)(c))
#define sa_rw_init(m) InitializeSRWLock(m)
#define sa_rw_rdlock(m) AcquireSRWLockShared(m)
#define sa_rw_rdunlock(m) ReleaseSRWLockShared(m)
#define sa_rw_wrlock(m) AcquireSRWLockExclusive(m)
#define sa_rw_wrunlock(m) ReleaseSRWLockExclusive(m)
#define sa_cond_init(c) InitializeConditionVariable(c)
#define sa_cond_signal(c) WakeConditionVariable(c)
#define sa_cond_broadcast(c) WakeAllConditionVariable(c)
#define sa_cond_wait(c, m) SleepConditionVariableSRW((c), (m), INFINITE, 0)

/* Returns 0 when the wait timed out. */
static int sa_cond_wait_ms(sa_cond* c, sa_mutex* m, int64_t ms) {
    if (ms < 0) {
        SleepConditionVariableSRW(c, m, INFINITE, 0);
        return 1;
    }
    return SleepConditionVariableSRW(c, m, (DWORD)(ms > 0x7FFFFFFE ? 0x7FFFFFFE : ms), 0) ? 1 : 0;
}

#define SA_THREAD_FN(name) static unsigned __stdcall name(void* arg)
typedef unsigned(__stdcall* sa_thread_fn)(void*);

static int sa_thread_start(sa_thread* t, sa_thread_fn fn, void* arg) {
    uintptr_t h = _beginthreadex(NULL, 0, fn, arg, 0, NULL);
    if (h == 0) return 0;
    *t = (HANDLE)h;
    return 1;
}

static void sa_thread_join(sa_thread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static void sa_thread_yield_os(void) { SwitchToThread(); }

static int64_t sa_now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
}

static int sa_cpu_count(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n == 0 ? 1 : (int)n;
}

/* Process CPU time (user + kernel) in nanoseconds; used by the idle-CPU measurement. */
static int64_t sa_process_cpu_ns(void) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    ULARGE_INTEGER kk, uu;
    kk.LowPart = k.dwLowDateTime; kk.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
    return (int64_t)((kk.QuadPart + uu.QuadPart) * 100);
}

/* Process memory (bytes): which = 0 peak working set, 1 current private commit. */
#include <psapi.h>
static int64_t sa_process_mem(int which) {
    PROCESS_MEMORY_COUNTERS_EX m;
    memset(&m, 0, sizeof(m));
    m.cb = sizeof(m);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&m, sizeof(m))) return 0;
    return which == 0 ? (int64_t)m.PeakWorkingSetSize : (int64_t)m.PrivateUsage;
}

/* ---- Fibers ------------------------------------------------------------------------------
 * A Windows fiber may be resumed on any thread that has itself been converted to a fiber, as long
 * as it never runs on two threads at once (the scheduler guarantees that). Tasks therefore migrate
 * freely between workers. */
#define SA_FIBER_CALL __stdcall
typedef void(__stdcall* sa_fiber_entry)(void*);

static void* sa_fiber_thread_enter(void) {
    return ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
}

static void sa_fiber_thread_leave(void* self) {
    (void)self;
    ConvertFiberToThread();
}

static void* sa_fiber_new(size_t reserve, sa_fiber_entry entry) {
    return CreateFiberEx(0, reserve, FIBER_FLAG_FLOAT_SWITCH, (LPFIBER_START_ROUTINE)entry, NULL);
}

static void sa_fiber_free(void* f) { DeleteFiber(f); }

static void sa_fiber_switch(void* to) { SwitchToFiber(to); }

/* ---- Thread-local slots --------------------------------------------------------------------
 * A task can resume on a different OS thread after a fiber switch. The compiler may cache a TLS
 * address within one function, so thread-locals are only ever touched inside these noinline
 * accessors, never across a switch in the same frame. */
#define SA_NOINLINE __declspec(noinline)
#define SA_TLS __declspec(thread)
