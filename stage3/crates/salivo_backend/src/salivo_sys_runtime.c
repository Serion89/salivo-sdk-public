/* Stage 38 salivo.std.sys: Windows system telemetry for Salivo programs.
 *
 * Process snapshots (NtQuerySystemInformation), performance counters (PDH, English counter
 * paths so they work on any display language), services, registry values, executable version
 * strings and icons (written as PNG), hardware facts (CPU topology, SMBIOS memory, disks, GPUs,
 * the default-route network adapter) and process termination.
 *
 * ABI: crates/salivo_backend/src/salivo_sys_abi.def. Every value is a 64-bit integer or a
 * NUL-terminated UTF-8 string; returned strings are new allocations owned by the caller.
 * Snapshot and query handles are small integers into per-module tables. Calls are not
 * thread-safe: use one thread (or one async worker) per program. Other platforms get stubs
 * that report -12 (Unsupported) and empty strings.
 */

#define _CRT_SECURE_NO_WARNINGS
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* posix_openpt, ptsname, sethostname */
#endif
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>

void* salivo_rt_smalloc(long long n);

static char* out_str(const char* s, size_t n) {
    char* r = (char*)salivo_rt_smalloc((long long)n + 1);
    if (!r) return 0;
    if (n) memcpy(r, s, n);
    r[n] = 0;
    return r;
}
static char* out_cstr(const char* s) { return out_str(s ? s : "", s ? strlen(s) : 0); }

#define SYS_UNSUPPORTED (-12)
#define SYS_INVALID_ARG (-7)
#define SYS_INVALID_HANDLE (-10)
#define SYS_OS (-8)

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winioctl.h>
#include <iphlpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <shellapi.h>
#include <psapi.h>

/* ---------- UTF-8 <-> UTF-16 ---------- */

static wchar_t* widen(const char* s) {
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, 0, 0);
    wchar_t* w = (wchar_t*)malloc(sizeof(wchar_t) * (n > 0 ? n : 1));
    if (!w) return 0;
    if (n <= 0) { w[0] = 0; return w; }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

/* New caller-owned UTF-8 string from n UTF-16 units (n < 0: NUL-terminated). */
static char* out_wide(const wchar_t* w, int n) {
    if (!w) return out_cstr("");
    int bytes = WideCharToMultiByte(CP_UTF8, 0, w, n, 0, 0, 0, 0);
    if (bytes <= 0) return out_cstr("");
    char* tmp = (char*)malloc((size_t)bytes + 1);
    if (!tmp) return out_cstr("");
    WideCharToMultiByte(CP_UTF8, 0, w, n, tmp, bytes, 0, 0);
    size_t len = (n < 0) ? strlen(tmp) : (size_t)bytes;
    char* r = out_str(tmp, len);
    free(tmp);
    return r;
}

/* UTF-8 copy into a malloc'd buffer (internal use). */
static char* utf8_dup(const wchar_t* w) {
    int bytes = WideCharToMultiByte(CP_UTF8, 0, w ? w : L"", -1, 0, 0, 0, 0);
    char* r = (char*)malloc(bytes > 0 ? (size_t)bytes : 1);
    if (!r) return 0;
    if (bytes <= 0) { r[0] = 0; return r; }
    WideCharToMultiByte(CP_UTF8, 0, w ? w : L"", -1, r, bytes, 0, 0);
    return r;
}

/* ---------- Process snapshots ---------- */

typedef struct {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    USHORT NameLength;
    USHORT NameMaximumLength;
    PWSTR NameBuffer;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
} SysProcInfo;

typedef struct {
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG WaitTime;
    PVOID StartAddress;
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
    LONG Priority;
    LONG BasePriority;
    ULONG ContextSwitches;
    ULONG ThreadState;
    ULONG WaitReason;
} SysThreadInfo;

typedef LONG(WINAPI* NtQsiFn)(ULONG, PVOID, ULONG, PULONG);

typedef struct {
    long long pid, ppid, threads, handles, session, ws, priv, cpu100ns, created, state, haswin;
    long long ioread, iowrite, ioother;
    char* name;
    char* title;
} SysProc;

typedef struct {
    int live;
    SysProc* items;
    long long count;
} SysProcSnap;

#define SYS_MAX_SNAPS 16
static SysProcSnap g_snaps[SYS_MAX_SNAPS];

typedef struct {
    DWORD pid;
    HWND hwnd;
} WinFind;

typedef struct {
    DWORD* pids;
    HWND* hwnds;
    int n, cap;
} WinList;

/* Main window per process, chosen like .NET's Process.MainWindowHandle: the first visible,
 * unowned top-level window. */
static BOOL CALLBACK collect_windows(HWND hwnd, LPARAM lp) {
    WinList* wl = (WinList*)lp;
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    for (int i = 0; i < wl->n; i++) if (wl->pids[i] == pid) return TRUE;
    if (wl->n == wl->cap) {
        int cap = wl->cap ? wl->cap * 2 : 128;
        DWORD* p = (DWORD*)realloc(wl->pids, sizeof(DWORD) * cap);
        HWND* h = (HWND*)realloc(wl->hwnds, sizeof(HWND) * cap);
        if (!p || !h) { free(p ? p : wl->pids); free(h ? h : wl->hwnds); wl->pids = 0; wl->hwnds = 0; wl->n = wl->cap = 0; return FALSE; }
        wl->pids = p; wl->hwnds = h; wl->cap = cap;
    }
    wl->pids[wl->n] = pid;
    wl->hwnds[wl->n] = hwnd;
    wl->n++;
    return TRUE;
}

static long long sys_unlocked_proc_snap(void) {
    static NtQsiFn qsi = 0;
    if (!qsi) qsi = (NtQsiFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    if (!qsi) return SYS_UNSUPPORTED;
    int slot = -1;
    for (int i = 0; i < SYS_MAX_SNAPS; i++) if (!g_snaps[i].live) { slot = i; break; }
    if (slot < 0) return -11;

    ULONG size = 1 << 20;
    BYTE* buf = 0;
    for (int tries = 0; tries < 8; tries++) {
        buf = (BYTE*)malloc(size);
        if (!buf) return SYS_OS;
        ULONG need = 0;
        LONG st = qsi(5 /* SystemProcessInformation */, buf, size, &need);
        if (st == 0) break;
        free(buf);
        buf = 0;
        if (st != (LONG)0xC0000004L /* STATUS_INFO_LENGTH_MISMATCH */) return SYS_OS;
        size = (need > size ? need : size) + (1 << 16);
    }
    if (!buf) return SYS_OS;

    WinList wl = {0};
    EnumWindows(collect_windows, (LPARAM)&wl);

    long long count = 0;
    for (BYTE* p = buf;;) {
        SysProcInfo* pi = (SysProcInfo*)p;
        count++;
        if (!pi->NextEntryOffset) break;
        p += pi->NextEntryOffset;
    }
    SysProc* items = (SysProc*)calloc((size_t)count, sizeof(SysProc));
    if (!items) { free(buf); free(wl.pids); free(wl.hwnds); return SYS_OS; }

    long long i = 0;
    for (BYTE* p = buf;; i++) {
        SysProcInfo* pi = (SysProcInfo*)p;
        SysProc* o = &items[i];
        o->pid = (long long)(ULONG_PTR)pi->UniqueProcessId;
        o->ppid = (long long)(ULONG_PTR)pi->InheritedFromUniqueProcessId;
        o->threads = pi->NumberOfThreads;
        o->handles = pi->HandleCount;
        o->session = pi->SessionId;
        o->ws = (long long)pi->WorkingSetSize;
        o->priv = (long long)pi->PrivatePageCount;
        o->cpu100ns = pi->UserTime.QuadPart + pi->KernelTime.QuadPart;
        o->created = pi->CreateTime.QuadPart;
        o->ioread = pi->ReadTransferCount.QuadPart;
        o->iowrite = pi->WriteTransferCount.QuadPart;
        o->ioother = pi->OtherTransferCount.QuadPart;
        if (pi->NameBuffer && pi->NameLength) {
            int wn = pi->NameLength / 2;
            wchar_t* tmp = (wchar_t*)malloc(sizeof(wchar_t) * (wn + 1));
            if (tmp) { memcpy(tmp, pi->NameBuffer, sizeof(wchar_t) * wn); tmp[wn] = 0; o->name = utf8_dup(tmp); free(tmp); }
        } else {
            o->name = utf8_dup(o->pid == 0 ? L"System Idle Process" : L"");
        }
        /* Every thread waiting with reason Suspended (5) means the process is suspended. */
        SysThreadInfo* th = (SysThreadInfo*)(pi + 1);
        int suspended = pi->NumberOfThreads > 0;
        for (ULONG t = 0; t < pi->NumberOfThreads; t++) {
            if (th[t].ThreadState != 5 || th[t].WaitReason != 5) { suspended = 0; break; }
        }
        o->state = suspended ? 1 : 0;
        for (int w = 0; w < wl.n; w++) {
            if ((long long)wl.pids[w] == o->pid) {
                o->haswin = 1;
                wchar_t title[512];
                int tn = GetWindowTextW(wl.hwnds[w], title, 512);
                title[tn > 0 ? tn : 0] = 0;
                o->title = utf8_dup(title);
                if (!suspended && IsHungAppWindow(wl.hwnds[w])) o->state = 2;
                break;
            }
        }
        if (!o->title) o->title = utf8_dup(L"");
        if (!pi->NextEntryOffset) break;
        p += pi->NextEntryOffset;
    }
    free(buf);
    free(wl.pids);
    free(wl.hwnds);
    g_snaps[slot].live = 1;
    g_snaps[slot].items = items;
    g_snaps[slot].count = count;
    return slot + 1;
}

static SysProcSnap* snap_at(long long h) {
    if (h < 1 || h > SYS_MAX_SNAPS || !g_snaps[h - 1].live) return 0;
    return &g_snaps[h - 1];
}

static long long sys_unlocked_proc_count(long long h) {
    SysProcSnap* s = snap_at(h);
    return s ? s->count : SYS_INVALID_HANDLE;
}

static long long sys_unlocked_proc_int(long long h, long long i, long long field) {
    SysProcSnap* s = snap_at(h);
    if (!s) return SYS_INVALID_HANDLE;
    if (i < 0 || i >= s->count) return SYS_INVALID_ARG;
    SysProc* p = &s->items[i];
    switch (field) {
    case 0: return p->pid;
    case 1: return p->ppid;
    case 2: return p->threads;
    case 3: return p->handles;
    case 4: return p->session;
    case 5: return p->ws;
    case 6: return p->priv;
    case 7: return p->cpu100ns;
    case 8: return p->created;
    case 9: return p->state;
    case 10: return p->haswin;
    case 11: return p->ioread;
    case 12: return p->iowrite;
    case 13: return p->ioother;
    default: return SYS_INVALID_ARG;
    }
}

static char* sys_unlocked_proc_text(long long h, long long i, long long field) {
    SysProcSnap* s = snap_at(h);
    if (!s || i < 0 || i >= s->count) return out_cstr("");
    return out_cstr(field == 1 ? s->items[i].title : s->items[i].name);
}

static long long sys_unlocked_proc_free(long long h) {
    SysProcSnap* s = snap_at(h);
    if (!s) return SYS_INVALID_HANDLE;
    for (long long i = 0; i < s->count; i++) { free(s->items[i].name); free(s->items[i].title); }
    free(s->items);
    s->items = 0;
    s->count = 0;
    s->live = 0;
    return 0;
}

char* salivo_sys_proc_path(long long pid) {
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!ph) return out_cstr("");
    wchar_t path[MAX_PATH * 4];
    DWORD n = (DWORD)(sizeof(path) / sizeof(path[0]));
    char* r = QueryFullProcessImageNameW(ph, 0, path, &n) ? out_wide(path, (int)n) : out_cstr("");
    CloseHandle(ph);
    return r;
}

long long salivo_sys_terminate(long long pid) {
    if (pid <= 4) return SYS_INVALID_ARG;
    HANDLE ph = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!ph) return -(long long)GetLastError();
    BOOL ok = TerminateProcess(ph, 1);
    long long err = ok ? 0 : -(long long)GetLastError();
    CloseHandle(ph);
    return err;
}

/* ---------- Performance counters (PDH) ---------- */

typedef struct {
    int live;
    int query;
    PDH_HCOUNTER counter;
    PDH_FMT_COUNTERVALUE_ITEM_W* items;
    DWORD count;
} SysCounter;

#define SYS_MAX_QUERIES 16
#define SYS_MAX_COUNTERS 256
static PDH_HQUERY g_queries[SYS_MAX_QUERIES];
static SysCounter g_counters[SYS_MAX_COUNTERS];

static long long sys_unlocked_pdh_open(void) {
    for (int i = 0; i < SYS_MAX_QUERIES; i++) {
        if (!g_queries[i]) {
            if (PdhOpenQueryW(0, 0, &g_queries[i]) != ERROR_SUCCESS) { g_queries[i] = 0; return SYS_OS; }
            return i + 1;
        }
    }
    return -11;
}

static long long sys_unlocked_pdh_add(long long q, const char* path) {
    if (q < 1 || q > SYS_MAX_QUERIES || !g_queries[q - 1]) return SYS_INVALID_HANDLE;
    int slot = -1;
    for (int i = 0; i < SYS_MAX_COUNTERS; i++) if (!g_counters[i].live) { slot = i; break; }
    if (slot < 0) return -11;
    wchar_t* w = widen(path);
    if (!w) return SYS_OS;
    PDH_HCOUNTER c = 0;
    PDH_STATUS st = PdhAddEnglishCounterW(g_queries[q - 1], w, 0, &c);
    free(w);
    if (st != ERROR_SUCCESS) return SYS_INVALID_ARG;
    g_counters[slot].live = 1;
    g_counters[slot].query = (int)q;
    g_counters[slot].counter = c;
    g_counters[slot].items = 0;
    g_counters[slot].count = 0;
    return slot + 1;
}

/* Samples every counter of the query and caches their formatted values. Rate counters need two
 * collects; the first one reads as zero. */
static long long sys_unlocked_pdh_collect(long long q) {
    if (q < 1 || q > SYS_MAX_QUERIES || !g_queries[q - 1]) return SYS_INVALID_HANDLE;
    PDH_STATUS st = PdhCollectQueryData(g_queries[q - 1]);
    for (int i = 0; i < SYS_MAX_COUNTERS; i++) {
        SysCounter* c = &g_counters[i];
        if (!c->live || c->query != (int)q) continue;
        free(c->items);
        c->items = 0;
        c->count = 0;
        DWORD bytes = 0, n = 0;
        PDH_STATUS s = PdhGetFormattedCounterArrayW(c->counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &n, 0);
        if (s != (PDH_STATUS)PDH_MORE_DATA || !bytes) continue;
        c->items = (PDH_FMT_COUNTERVALUE_ITEM_W*)malloc(bytes);
        if (!c->items) continue;
        if (PdhGetFormattedCounterArrayW(c->counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &n, c->items) == ERROR_SUCCESS) {
            c->count = n;
        } else {
            free(c->items);
            c->items = 0;
        }
    }
    return st == ERROR_SUCCESS ? 0 : SYS_OS;
}

static SysCounter* counter_at(long long c) {
    if (c < 1 || c > SYS_MAX_COUNTERS || !g_counters[c - 1].live) return 0;
    return &g_counters[c - 1];
}

static long long sys_unlocked_pdh_count(long long c) {
    SysCounter* k = counter_at(c);
    return k ? (long long)k->count : SYS_INVALID_HANDLE;
}

static char* sys_unlocked_pdh_name(long long c, long long i) {
    SysCounter* k = counter_at(c);
    if (!k || i < 0 || i >= (long long)k->count) return out_cstr("");
    return out_wide(k->items[i].szName, -1);
}

/* Value in thousandths (12.5 % reads 12500) so fractions survive the integer ABI. */
static long long sys_unlocked_pdh_value(long long c, long long i) {
    SysCounter* k = counter_at(c);
    if (!k || i < 0 || i >= (long long)k->count) return 0;
    PDH_FMT_COUNTERVALUE* v = &k->items[i].FmtValue;
    if (v->CStatus != PDH_CSTATUS_VALID_DATA && v->CStatus != PDH_CSTATUS_NEW_DATA) return 0;
    double d = v->doubleValue * 1000.0;
    if (d > 9.0e18) d = 9.0e18;
    if (d < -9.0e18) d = -9.0e18;
    return (long long)(d < 0 ? d - 0.5 : d + 0.5);
}

static long long sys_unlocked_pdh_close(long long q) {
    if (q < 1 || q > SYS_MAX_QUERIES || !g_queries[q - 1]) return SYS_INVALID_HANDLE;
    for (int i = 0; i < SYS_MAX_COUNTERS; i++) {
        if (g_counters[i].live && g_counters[i].query == (int)q) {
            free(g_counters[i].items);
            memset(&g_counters[i], 0, sizeof(g_counters[i]));
        }
    }
    PdhCloseQuery(g_queries[q - 1]);
    g_queries[q - 1] = 0;
    return 0;
}

/* ---------- Memory ---------- */

long long salivo_sys_mem(long long field) {
    if (field <= 1) {
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        if (!GlobalMemoryStatusEx(&ms)) return SYS_OS;
        return field == 0 ? (long long)ms.ullTotalPhys : (long long)ms.ullAvailPhys;
    }
    if (field == 2) {
        ULONGLONG kb = 0;
        return GetPhysicallyInstalledSystemMemory(&kb) ? (long long)kb * 1024 : SYS_OS;
    }
    PERFORMANCE_INFORMATION pi;
    pi.cb = sizeof(pi);
    if (!K32GetPerformanceInfo(&pi, sizeof(pi))) return SYS_OS;
    long long page = (long long)pi.PageSize;
    switch (field) {
    case 3: return (long long)pi.CommitTotal * page;
    case 4: return (long long)pi.CommitLimit * page;
    case 5: return (long long)pi.SystemCache * page;
    case 6: return (long long)pi.KernelPaged * page;
    case 7: return (long long)pi.KernelNonpaged * page;
    case 8: return (long long)pi.HandleCount;
    case 9: return (long long)pi.ProcessCount;
    case 10: return (long long)pi.ThreadCount;
    default: return SYS_INVALID_ARG;
    }
}

long long salivo_sys_uptime_ms(void) { return (long long)GetTickCount64(); }

/* All CPUs since boot in 1/100 s: 0 busy, 1 idle; 2 logical CPUs */
long long salivo_sys_cpu(long long which) {
    if (which == 2) { SYSTEM_INFO si; GetSystemInfo(&si); return (long long)si.dwNumberOfProcessors; }
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return SYS_OS;
    unsigned long long i = ((unsigned long long)idle.dwHighDateTime << 32) | idle.dwLowDateTime;
    unsigned long long k = ((unsigned long long)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    unsigned long long u = ((unsigned long long)user.dwHighDateTime << 32) | user.dwLowDateTime;
    if (which == 0) return (long long)((k + u - i) / 100000);
    if (which == 1) return (long long)(i / 100000);
    return SYS_INVALID_ARG;
}

/* ---------- Registry ---------- */

static HKEY root_key(long long root) {
    return root == 1 ? HKEY_CURRENT_USER : root == 2 ? HKEY_LOCAL_MACHINE : 0;
}

static HKEY open_key(long long root, const char* path) {
    HKEY base = root_key(root);
    if (!base) return 0;
    wchar_t* w = widen(path);
    if (!w) return 0;
    HKEY k = 0;
    LONG st = RegOpenKeyExW(base, w, 0, KEY_READ | KEY_WOW64_64KEY, &k);
    free(w);
    return st == ERROR_SUCCESS ? k : 0;
}

/* Value names of a key, one per line. */
char* salivo_sys_reg_names(long long root, const char* path) {
    HKEY k = open_key(root, path);
    if (!k) return out_cstr("");
    size_t cap = 1024, len = 0;
    char* acc = (char*)malloc(cap);
    if (!acc) { RegCloseKey(k); return out_cstr(""); }
    for (DWORD i = 0;; i++) {
        wchar_t name[16384];
        DWORD n = 16384;
        if (RegEnumValueW(k, i, name, &n, 0, 0, 0, 0) != ERROR_SUCCESS) break;
        char* u = utf8_dup(name);
        if (!u) continue;
        size_t ul = strlen(u);
        if (len + ul + 2 > cap) {
            while (len + ul + 2 > cap) cap *= 2;
            char* bigger = (char*)realloc(acc, cap);
            if (!bigger) { free(u); break; }
            acc = bigger;
        }
        memcpy(acc + len, u, ul);
        len += ul;
        acc[len++] = '\n';
        free(u);
    }
    RegCloseKey(k);
    char* r = out_str(acc, len);
    free(acc);
    return r;
}

/* Subkey names of a key, one per line. */
char* salivo_sys_reg_keys(long long root, const char* path) {
    HKEY k = open_key(root, path);
    if (!k) return out_cstr("");
    size_t cap = 1024, len = 0;
    char* acc = (char*)malloc(cap);
    if (!acc) { RegCloseKey(k); return out_cstr(""); }
    for (DWORD i = 0;; i++) {
        wchar_t name[256];
        DWORD n = 256;
        if (RegEnumKeyExW(k, i, name, &n, 0, 0, 0, 0) != ERROR_SUCCESS) break;
        char* u = utf8_dup(name);
        if (!u) continue;
        size_t ul = strlen(u);
        if (len + ul + 2 > cap) {
            while (len + ul + 2 > cap) cap *= 2;
            char* bigger = (char*)realloc(acc, cap);
            if (!bigger) { free(u); break; }
            acc = bigger;
        }
        memcpy(acc + len, u, ul);
        len += ul;
        acc[len++] = '\n';
        free(u);
    }
    RegCloseKey(k);
    char* r = out_str(acc, len);
    free(acc);
    return r;
}

static BYTE* reg_read(long long root, const char* path, const char* name, DWORD* type, DWORD* size) {
    HKEY k = open_key(root, path);
    if (!k) return 0;
    wchar_t* wn = widen(name);
    BYTE* data = 0;
    *size = 0;
    if (wn && RegQueryValueExW(k, wn, 0, type, 0, size) == ERROR_SUCCESS) {
        data = (BYTE*)calloc(1, *size + 4);
        if (data && RegQueryValueExW(k, wn, 0, type, data, size) != ERROR_SUCCESS) { free(data); data = 0; }
    }
    free(wn);
    RegCloseKey(k);
    return data;
}

/* REG_SZ / REG_EXPAND_SZ (unexpanded) / REG_DWORD / REG_QWORD as text, "" when absent. */
char* salivo_sys_reg_str(long long root, const char* path, const char* name) {
    DWORD type = 0, size = 0;
    BYTE* d = reg_read(root, path, name, &type, &size);
    if (!d) return out_cstr("");
    char* r;
    if (type == REG_SZ || type == REG_EXPAND_SZ) {
        r = out_wide((const wchar_t*)d, -1);
    } else if (type == REG_DWORD && size >= 4) {
        char num[32];
        _ui64toa((unsigned long long)*(DWORD*)d, num, 10);
        r = out_cstr(num);
    } else if (type == REG_QWORD && size >= 8) {
        char num[32];
        _ui64toa(*(unsigned long long*)d, num, 10);
        r = out_cstr(num);
    } else {
        r = out_cstr("");
    }
    free(d);
    return r;
}

/* Byte `index` of a value, or -1. */
long long salivo_sys_reg_byte(long long root, const char* path, const char* name, long long index) {
    DWORD type = 0, size = 0;
    BYTE* d = reg_read(root, path, name, &type, &size);
    if (!d) return -1;
    long long r = (index >= 0 && index < (long long)size) ? d[index] : -1;
    free(d);
    return r;
}

/* ---------- Files ---------- */

/* Version strings never change while a file exists, and reading them costs a disk read, so
 * results (including misses) are cached per path and key. */
typedef struct {
    char* key;
    char* value;
} InfoEntry;
static InfoEntry* g_info;
static size_t g_info_n, g_info_cap;

static char* file_info_uncached(const char* path, const char* key);

/* Version resource string (FileDescription, CompanyName, ProductName, ...) of a file. */
char* salivo_sys_file_info(const char* path, const char* key) {
    if (!path || !key) return out_cstr("");
    size_t pl = strlen(path), kl = strlen(key);
    char* ck = (char*)malloc(pl + kl + 2);
    if (!ck) return out_cstr("");
    memcpy(ck, key, kl);
    ck[kl] = '|';
    for (size_t i = 0; i <= pl; i++) {
        char c = path[i];
        ck[kl + 1 + i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    for (size_t i = 0; i < g_info_n; i++) {
        if (!strcmp(g_info[i].key, ck)) { free(ck); return out_cstr(g_info[i].value); }
    }
    char* v = file_info_uncached(path, key);
    if (g_info_n == g_info_cap) {
        size_t cap = g_info_cap ? g_info_cap * 2 : 256;
        InfoEntry* bigger = (InfoEntry*)realloc(g_info, cap * sizeof(InfoEntry));
        if (!bigger) { free(ck); return v; }
        g_info = bigger;
        g_info_cap = cap;
    }
    char* copy = (char*)malloc(strlen(v) + 1);
    if (!copy) { free(ck); return v; }
    strcpy(copy, v);
    g_info[g_info_n].key = ck;
    g_info[g_info_n].value = copy;
    g_info_n++;
    return v;
}

static char* file_info_uncached(const char* path, const char* key) {
    wchar_t* wp = widen(path);
    if (!wp) return out_cstr("");
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(wp, &dummy);
    if (!size) { free(wp); return out_cstr(""); }
    BYTE* data = (BYTE*)malloc(size);
    char* r = 0;
    if (data && GetFileVersionInfoW(wp, 0, size, data)) {
        struct { WORD lang, cp; }* tr = 0;
        UINT trn = 0;
        if (VerQueryValueW(data, L"\\VarFileInfo\\Translation", (LPVOID*)&tr, &trn) && trn >= 4) {
            wchar_t* wk = widen(key);
            wchar_t q[256];
            wchar_t* val = 0;
            UINT vn = 0;
            for (UINT t = 0; t < trn / 4 && !r && wk; t++) {
                _snwprintf(q, 256, L"\\StringFileInfo\\%04x%04x\\%ls", tr[t].lang, tr[t].cp, wk);
                if (VerQueryValueW(data, q, (LPVOID*)&val, &vn) && vn > 1) r = out_wide(val, -1);
            }
            free(wk);
        }
    }
    free(data);
    free(wp);
    return r ? r : out_cstr("");
}

/* -- PNG writer (stored deflate blocks) for icons -- */

static unsigned long crc_table[256];
static int crc_ready = 0;

static unsigned long crc_update(unsigned long c, const BYTE* b, size_t n) {
    if (!crc_ready) {
        for (unsigned long k = 0; k < 256; k++) {
            unsigned long x = k;
            for (int j = 0; j < 8; j++) x = (x & 1) ? 0xEDB88320UL ^ (x >> 1) : x >> 1;
            crc_table[k] = x;
        }
        crc_ready = 1;
    }
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void put32(BYTE* p, unsigned long v) { p[0] = (BYTE)(v >> 24); p[1] = (BYTE)(v >> 16); p[2] = (BYTE)(v >> 8); p[3] = (BYTE)v; }

static int write_chunk(HANDLE f, const char* type, const BYTE* data, DWORD n) {
    BYTE hdr[8];
    put32(hdr, n);
    memcpy(hdr + 4, type, 4);
    unsigned long crc = crc_update(0xFFFFFFFFUL, (const BYTE*)type, 4);
    crc = crc_update(crc, data, n) ^ 0xFFFFFFFFUL;
    BYTE tail[4];
    put32(tail, crc);
    DWORD w;
    return WriteFile(f, hdr, 8, &w, 0) && (!n || WriteFile(f, data, n, &w, 0)) && WriteFile(f, tail, 4, &w, 0);
}

/* RGBA pixels (top-down) to a PNG file. */
static int write_png(const wchar_t* path, const BYTE* rgba, int wdt, int hgt) {
    size_t raw_len = (size_t)hgt * (1 + (size_t)wdt * 4);
    BYTE* raw = (BYTE*)malloc(raw_len);
    if (!raw) return 0;
    for (int y = 0; y < hgt; y++) {
        raw[y * (1 + wdt * 4)] = 0;
        memcpy(raw + y * (1 + wdt * 4) + 1, rgba + (size_t)y * wdt * 4, (size_t)wdt * 4);
    }
    size_t blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    BYTE* z = (BYTE*)malloc(z_len);
    if (!z) { free(raw); return 0; }
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    unsigned long a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t pos = 0; pos < raw_len;) {
        size_t n = raw_len - pos > 65535 ? 65535 : raw_len - pos;
        z[o++] = (BYTE)(pos + n == raw_len ? 1 : 0);
        z[o++] = (BYTE)(n & 0xFF);
        z[o++] = (BYTE)(n >> 8);
        z[o++] = (BYTE)(~n & 0xFF);
        z[o++] = (BYTE)((~n >> 8) & 0xFF);
        memcpy(z + o, raw + pos, n);
        o += n;
        pos += n;
    }
    put32(z + o, (b << 16) | a);
    o += 4;
    free(raw);

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) { free(z); return 0; }
    static const BYTE sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    BYTE ihdr[13];
    put32(ihdr, (unsigned long)wdt);
    put32(ihdr + 4, (unsigned long)hgt);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    DWORD w;
    int ok = WriteFile(f, sig, 8, &w, 0) && write_chunk(f, "IHDR", ihdr, 13) &&
             write_chunk(f, "IDAT", z, (DWORD)o) && write_chunk(f, "IEND", 0, 0);
    CloseHandle(f);
    free(z);
    return ok;
}

static int render_icon(HICON icon, BYTE* out, int size, UINT flags) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = 0;
    HDC dc = CreateCompatibleDC(0);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, 0, 0);
    if (!dc || !bmp || !bits) { if (bmp) DeleteObject(bmp); if (dc) DeleteDC(dc); return 0; }
    HGDIOBJ old = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)size * size * 4);
    DrawIconEx(dc, 0, 0, icon, size, size, 0, 0, flags);
    GdiFlush();
    memcpy(out, bits, (size_t)size * size * 4);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return 1;
}

/* Writes the 32x32 icon of an executable (or any file) as a PNG. 0 on success. */
static long long sys_unlocked_icon_png(const char* exe, const char* out_path) {
    wchar_t* we = widen(exe);
    wchar_t* wo = widen(out_path);
    if (!we || !wo) { free(we); free(wo); return SYS_OS; }
    HICON big = 0;
    if (ExtractIconExW(we, 0, &big, 0, 1) < 1 || !big) {
        SHFILEINFOW sfi;
        memset(&sfi, 0, sizeof(sfi));
        if (SHGetFileInfoW(we, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON)) big = sfi.hIcon;
    }
    free(we);
    if (!big) { free(wo); return SYS_INVALID_ARG; }
    const int S = 32;
    BYTE* px = (BYTE*)malloc((size_t)S * S * 4);
    BYTE* mask = (BYTE*)malloc((size_t)S * S * 4);
    long long rc = SYS_OS;
    if (px && mask && render_icon(big, px, S, DI_NORMAL)) {
        int has_alpha = 0;
        for (int i = 0; i < S * S; i++) if (px[i * 4 + 3]) { has_alpha = 1; break; }
        /* Icons without an alpha channel: opaque wherever the AND mask is black. */
        if (!has_alpha && render_icon(big, mask, S, DI_MASK)) {
            for (int i = 0; i < S * S; i++) px[i * 4 + 3] = mask[i * 4] ? 0 : 255;
        }
        for (int i = 0; i < S * S; i++) { BYTE t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        rc = write_png(wo, px, S, S) ? 0 : SYS_OS;
    }
    free(px);
    free(mask);
    DestroyIcon(big);
    free(wo);
    return rc;
}

/* ---------- Services ---------- */

typedef struct {
    char* name;
    char* display;
    long long state, pid, start, delayed;
} SysSvc;

typedef struct {
    int live;
    SysSvc* items;
    long long count;
} SysSvcSnap;

#define SYS_MAX_SVC_SNAPS 4
static SysSvcSnap g_svcs[SYS_MAX_SVC_SNAPS];

static long long sys_unlocked_svc_snap(void) {
    int slot = -1;
    for (int i = 0; i < SYS_MAX_SVC_SNAPS; i++) if (!g_svcs[i].live) { slot = i; break; }
    if (slot < 0) return -11;
    SC_HANDLE scm = OpenSCManagerW(0, 0, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return SYS_OS;
    DWORD need = 0, n = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, 0, 0, &need, &n, &resume, 0);
    BYTE* buf = (BYTE*)malloc(need + 4096);
    resume = 0;
    if (!buf || !EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buf, need + 4096, &need, &n, &resume, 0)) {
        free(buf);
        CloseServiceHandle(scm);
        return SYS_OS;
    }
    ENUM_SERVICE_STATUS_PROCESSW* e = (ENUM_SERVICE_STATUS_PROCESSW*)buf;
    SysSvc* items = (SysSvc*)calloc(n ? n : 1, sizeof(SysSvc));
    if (!items) { free(buf); CloseServiceHandle(scm); return SYS_OS; }
    BYTE cfgbuf[8192];
    for (DWORD i = 0; i < n; i++) {
        items[i].name = utf8_dup(e[i].lpServiceName);
        items[i].display = utf8_dup(e[i].lpDisplayName);
        items[i].state = e[i].ServiceStatusProcess.dwCurrentState;
        items[i].pid = e[i].ServiceStatusProcess.dwProcessId;
        items[i].start = -1;
        SC_HANDLE sh = OpenServiceW(scm, e[i].lpServiceName, SERVICE_QUERY_CONFIG);
        if (sh) {
            DWORD cn = 0;
            if (QueryServiceConfigW(sh, (QUERY_SERVICE_CONFIGW*)cfgbuf, sizeof(cfgbuf), &cn)) {
                items[i].start = ((QUERY_SERVICE_CONFIGW*)cfgbuf)->dwStartType;
            }
            SERVICE_DELAYED_AUTO_START_INFO d;
            if (QueryServiceConfig2W(sh, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, (LPBYTE)&d, sizeof(d), &cn)) {
                items[i].delayed = d.fDelayedAutostart ? 1 : 0;
            }
            CloseServiceHandle(sh);
        }
    }
    free(buf);
    CloseServiceHandle(scm);
    g_svcs[slot].live = 1;
    g_svcs[slot].items = items;
    g_svcs[slot].count = n;
    return slot + 1;
}

static SysSvcSnap* svc_at(long long h) {
    if (h < 1 || h > SYS_MAX_SVC_SNAPS || !g_svcs[h - 1].live) return 0;
    return &g_svcs[h - 1];
}

static long long sys_unlocked_svc_count(long long h) {
    SysSvcSnap* s = svc_at(h);
    return s ? s->count : SYS_INVALID_HANDLE;
}

/* field 0 state (1 stopped .. 4 running), 1 pid, 2 start type (2 auto, 3 manual, 4 disabled), 3 delayed */
static long long sys_unlocked_svc_int(long long h, long long i, long long field) {
    SysSvcSnap* s = svc_at(h);
    if (!s) return SYS_INVALID_HANDLE;
    if (i < 0 || i >= s->count) return SYS_INVALID_ARG;
    SysSvc* v = &s->items[i];
    return field == 0 ? v->state : field == 1 ? v->pid : field == 2 ? v->start : field == 3 ? v->delayed : SYS_INVALID_ARG;
}

static char* sys_unlocked_svc_text(long long h, long long i, long long field) {
    SysSvcSnap* s = svc_at(h);
    if (!s || i < 0 || i >= s->count) return out_cstr("");
    return out_cstr(field == 1 ? s->items[i].display : s->items[i].name);
}

static long long sys_unlocked_svc_free(long long h) {
    SysSvcSnap* s = svc_at(h);
    if (!s) return SYS_INVALID_HANDLE;
    for (long long i = 0; i < s->count; i++) { free(s->items[i].name); free(s->items[i].display); }
    free(s->items);
    memset(s, 0, sizeof(*s));
    return 0;
}

/* ---------- Hardware facts ---------- */

static char g_cpu_name[256];
static long long g_cpu_mhz, g_sockets, g_cores, g_logical, g_l2, g_l3;
static int g_cpu_ready;

static void read_cpu(void) {
    if (g_cpu_ready) return;
    g_cpu_ready = 1;
    DWORD type = 0, size = 0;
    BYTE* d = reg_read(2, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", &type, &size);
    if (d) {
        char* u = utf8_dup((const wchar_t*)d);
        if (u) {
            char* s = u;
            while (*s == ' ') s++;
            strncpy(g_cpu_name, s, sizeof(g_cpu_name) - 1);
            size_t n = strlen(g_cpu_name);
            while (n && g_cpu_name[n - 1] == ' ') g_cpu_name[--n] = 0;
            free(u);
        }
        free(d);
    }
    d = reg_read(2, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "~MHz", &type, &size);
    if (d) { g_cpu_mhz = *(DWORD*)d; free(d); }
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, 0, &len);
    BYTE* buf = (BYTE*)malloc(len ? len : 1);
    if (buf && GetLogicalProcessorInformationEx(RelationAll, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        for (DWORD off = 0; off < len;) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX x = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            if (x->Relationship == RelationProcessorPackage) g_sockets++;
            if (x->Relationship == RelationProcessorCore) {
                g_cores++;
                for (WORD gi = 0; gi < x->Processor.GroupCount; gi++) {
                    KAFFINITY m = x->Processor.GroupMask[gi].Mask;
                    while (m) { g_logical += (long long)(m & 1); m >>= 1; }
                }
            }
            if (x->Relationship == RelationCache) {
                if (x->Cache.Level == 2) g_l2 += x->Cache.CacheSize;
                if (x->Cache.Level == 3) g_l3 += x->Cache.CacheSize;
            }
            off += x->Size;
        }
    }
    free(buf);
}

static long long g_mem_speed, g_mem_slots, g_mem_used_slots, g_mem_form;
static int g_smbios_ready;

/* Memory devices (SMBIOS type 17): slots, populated slots, configured speed, form factor. */
static void read_smbios(void) {
    if (g_smbios_ready) return;
    g_smbios_ready = 1;
    UINT size = GetSystemFirmwareTable('RSMB', 0, 0, 0);
    if (!size) return;
    BYTE* buf = (BYTE*)malloc(size);
    if (!buf || GetSystemFirmwareTable('RSMB', 0, buf, size) != size) { free(buf); return; }
    DWORD tlen = *(DWORD*)(buf + 4);
    BYTE* p = buf + 8;
    BYTE* end = buf + 8 + tlen;
    if (end > buf + size) end = buf + size;
    while (p + 4 <= end) {
        BYTE type = p[0], len = p[1];
        if (len < 4 || p + len > end) break;
        if (type == 17 && len >= 0x15) {
            g_mem_slots++;
            WORD sz = *(WORD*)(p + 0x0C);
            if (sz != 0 && sz != 0xFFFF) {
                g_mem_used_slots++;
                if (!g_mem_form) g_mem_form = p[0x0E];
                WORD speed = len >= 0x22 ? *(WORD*)(p + 0x20) : 0;
                if (!speed) speed = *(WORD*)(p + 0x15);
                if (speed && speed != 0xFFFF && !g_mem_speed) g_mem_speed = speed;
            }
        }
        if (type == 127) break;
        BYTE* s = p + len;
        while (s + 1 < end && !(s[0] == 0 && s[1] == 0)) s++;
        p = s + 2;
    }
    free(buf);
}

static char g_disk_model[256];
static long long g_disk_size_gb, g_disk_ssd = -1, g_disk_count;
static int g_disk_ready;

static void read_disks(void) {
    if (g_disk_ready) return;
    g_disk_ready = 1;
    for (int i = 0; i < 16; i++) {
        wchar_t dev[64];
        _snwprintf(dev, 64, L"\\\\.\\PhysicalDrive%d", i);
        HANDLE h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        g_disk_count++;
        DWORD got = 0;
        DISK_GEOMETRY_EX geo;
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, 0, 0, &geo, sizeof(geo), &got, 0)) {
            g_disk_size_gb += geo.DiskSize.QuadPart / 1000000000LL;
        }
        if (!g_disk_model[0]) {
            STORAGE_PROPERTY_QUERY q;
            memset(&q, 0, sizeof(q));
            q.PropertyId = StorageDeviceProperty;
            q.QueryType = PropertyStandardQuery;
            BYTE out[1024];
            memset(out, 0, sizeof(out));
            if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), out, sizeof(out), &got, 0)) {
                STORAGE_DEVICE_DESCRIPTOR* dd = (STORAGE_DEVICE_DESCRIPTOR*)out;
                if (dd->ProductIdOffset && dd->ProductIdOffset < sizeof(out)) {
                    const char* s = (const char*)out + dd->ProductIdOffset;
                    while (*s == ' ') s++;
                    strncpy(g_disk_model, s, sizeof(g_disk_model) - 1);
                    size_t n = strlen(g_disk_model);
                    while (n && g_disk_model[n - 1] == ' ') g_disk_model[--n] = 0;
                }
            }
            q.PropertyId = StorageDeviceSeekPenaltyProperty;
            DEVICE_SEEK_PENALTY_DESCRIPTOR sp;
            if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &sp, sizeof(sp), &got, 0)) {
                g_disk_ssd = sp.IncursSeekPenalty ? 0 : 1;
            }
        }
        CloseHandle(h);
    }
}

typedef struct {
    char* names;
    char* drivers;
    long long total_mb;
} GpuInfo;
static GpuInfo g_gpu;
static int g_gpu_ready;

static void append_line(char** acc, const char* s) {
    size_t a = *acc ? strlen(*acc) : 0, b = strlen(s);
    char* r = (char*)realloc(*acc, a + b + 2);
    if (!r) return;
    memcpy(r + a, s, b);
    r[a + b] = '\n';
    r[a + b + 1] = 0;
    *acc = r;
}

static void read_gpus(void) {
    if (g_gpu_ready) return;
    g_gpu_ready = 1;
    const char* cls = "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    for (int i = 0; i < 32; i++) {
        char key[256];
        _snprintf(key, sizeof(key), "%s\\%04d", cls, i);
        DWORD type = 0, size = 0;
        BYTE* desc = reg_read(2, key, "DriverDesc", &type, &size);
        if (!desc) continue;
        char* name = utf8_dup((const wchar_t*)desc);
        free(desc);
        BYTE* ver = reg_read(2, key, "DriverVersion", &type, &size);
        char* vs = ver ? utf8_dup((const wchar_t*)ver) : 0;
        free(ver);
        BYTE* mem = reg_read(2, key, "HardwareInformation.qwMemorySize", &type, &size);
        if (mem && size >= 8) g_gpu.total_mb += (long long)(*(unsigned long long*)mem / (1024 * 1024));
        else {
            free(mem);
            mem = reg_read(2, key, "HardwareInformation.MemorySize", &type, &size);
            if (mem && size >= 4) g_gpu.total_mb += (long long)(*(DWORD*)mem / (1024 * 1024));
        }
        free(mem);
        if (name) append_line(&g_gpu.names, name);
        append_line(&g_gpu.drivers, vs ? vs : "");
        free(name);
        free(vs);
    }
}

static char g_net_name[256], g_net_desc[256], g_net_v4[64], g_net_v6[64];
static long long g_net_link;
static int g_net_ready;

/* The adapter Windows would use to reach the internet (best route to 8.8.8.8). */
static void read_net(void) {
    if (g_net_ready) return;
    g_net_ready = 1;
    DWORD best = 0;
    IPAddr dest = htonl(0x08080808);
    if (GetBestInterface(dest, &best) != NO_ERROR) best = 0;
    ULONG size = 1 << 16;
    IP_ADAPTER_ADDRESSES* aa = (IP_ADAPTER_ADDRESSES*)malloc(size);
    if (!aa) return;
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, 0, aa, &size) == ERROR_BUFFER_OVERFLOW) {
        free(aa);
        aa = (IP_ADAPTER_ADDRESSES*)malloc(size);
        if (!aa || GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, 0, aa, &size) != NO_ERROR) { free(aa); return; }
    }
    for (IP_ADAPTER_ADDRESSES* a = aa; a; a = a->Next) {
        if (a->IfIndex != best) continue;
        char* s = utf8_dup(a->FriendlyName);
        if (s) { strncpy(g_net_name, s, sizeof(g_net_name) - 1); free(s); }
        s = utf8_dup(a->Description);
        if (s) { strncpy(g_net_desc, s, sizeof(g_net_desc) - 1); free(s); }
        g_net_link = (long long)a->ReceiveLinkSpeed;
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
            wchar_t txt[64];
            DWORD tn = 64;
            if (WSAAddressToStringW(u->Address.lpSockaddr, u->Address.iSockaddrLength, 0, txt, &tn) != 0) continue;
            char* t = utf8_dup(txt);
            if (!t) continue;
            if (u->Address.lpSockaddr->sa_family == AF_INET && !g_net_v4[0]) strncpy(g_net_v4, t, sizeof(g_net_v4) - 1);
            if (u->Address.lpSockaddr->sa_family == AF_INET6 && !g_net_v6[0]) strncpy(g_net_v6, t, sizeof(g_net_v6) - 1);
            free(t);
        }
        break;
    }
    free(aa);
}

/* Text facts: cpu_name computer user gpu_names gpu_drivers disk_model net_name net_desc
 * net_ipv4 net_ipv6 (lists are one item per line). Unknown keys read as "". */
static char* sys_unlocked_info(const char* key) {
    if (!key) return out_cstr("");
    if (!strcmp(key, "cpu_name")) { read_cpu(); return out_cstr(g_cpu_name); }
    if (!strcmp(key, "computer")) {
        wchar_t w[256];
        DWORD n = 256;
        return GetComputerNameW(w, &n) ? out_wide(w, -1) : out_cstr("");
    }
    if (!strcmp(key, "user")) {
        wchar_t w[256];
        DWORD n = 256;
        return GetUserNameW(w, &n) ? out_wide(w, -1) : out_cstr("");
    }
    if (!strcmp(key, "gpu_names")) { read_gpus(); return out_cstr(g_gpu.names); }
    if (!strcmp(key, "gpu_drivers")) { read_gpus(); return out_cstr(g_gpu.drivers); }
    if (!strcmp(key, "disk_model")) { read_disks(); return out_cstr(g_disk_model); }
    if (!strcmp(key, "net_name")) { read_net(); return out_cstr(g_net_name); }
    if (!strcmp(key, "net_desc")) { read_net(); return out_cstr(g_net_desc); }
    if (!strcmp(key, "net_ipv4")) { read_net(); return out_cstr(g_net_v4); }
    if (!strcmp(key, "net_ipv6")) { read_net(); return out_cstr(g_net_v6); }
    return out_cstr("");
}

/* Numeric facts: cpu_mhz cpu_sockets cpu_cores cpu_logical cpu_l2_kb cpu_l3_kb cpu_virt
 * mem_speed_mts mem_slots mem_slots_used mem_form (SMBIOS: 9 DIMM, 13 SODIMM) gpu_total_mb
 * disk_size_gb disk_ssd (1, 0, -1 unknown) disk_count net_link_bps session pid.
 * Unknown keys read as -7. */
static long long sys_unlocked_info_int(const char* key) {
    if (!key) return SYS_INVALID_ARG;
    if (!strncmp(key, "cpu_", 4)) {
        read_cpu();
        if (!strcmp(key, "cpu_mhz")) return g_cpu_mhz;
        if (!strcmp(key, "cpu_sockets")) return g_sockets;
        if (!strcmp(key, "cpu_cores")) return g_cores;
        if (!strcmp(key, "cpu_logical")) return g_logical;
        if (!strcmp(key, "cpu_l2_kb")) return g_l2 / 1024;
        if (!strcmp(key, "cpu_l3_kb")) return g_l3 / 1024;
        if (!strcmp(key, "cpu_virt")) return IsProcessorFeaturePresent(21 /* PF_VIRT_FIRMWARE_ENABLED */) ? 1 : 0;
    }
    if (!strncmp(key, "mem_", 4)) {
        read_smbios();
        if (!strcmp(key, "mem_speed_mts")) return g_mem_speed;
        if (!strcmp(key, "mem_slots")) return g_mem_slots;
        if (!strcmp(key, "mem_slots_used")) return g_mem_used_slots;
        if (!strcmp(key, "mem_form")) return g_mem_form;
    }
    if (!strcmp(key, "gpu_total_mb")) { read_gpus(); return g_gpu.total_mb; }
    if (!strcmp(key, "disk_size_gb")) { read_disks(); return g_disk_size_gb; }
    if (!strcmp(key, "disk_ssd")) { read_disks(); return g_disk_ssd; }
    if (!strcmp(key, "disk_count")) { read_disks(); return g_disk_count; }
    if (!strcmp(key, "net_link_bps")) { read_net(); return g_net_link; }
    if (!strcmp(key, "session")) {
        DWORD s = 0;
        return ProcessIdToSessionId(GetCurrentProcessId(), &s) ? (long long)s : SYS_OS;
    }
    if (!strcmp(key, "pid")) return (long long)GetCurrentProcessId();
    return SYS_INVALID_ARG;
}

/* Busy blocks of this process's heaps (HeapWalk) whose size is in [min_size, max_size]:
 * which = 0 counts blocks, 1 sums their bytes. For finding what a long-running program keeps. */
long long salivo_sys_heap_blocks(long long min_size, long long max_size, long long which) {
    HANDLE heaps[256];
    DWORD n = GetProcessHeaps(256, heaps);
    long long count = 0, bytes = 0;
    for (DWORD i = 0; i < n && i < 256; i++) {
        if (!HeapLock(heaps[i])) continue;
        PROCESS_HEAP_ENTRY e;
        e.lpData = 0;
        while (HeapWalk(heaps[i], &e)) {
            if (!(e.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
            long long sz = (long long)e.cbData;
            if (sz >= min_size && sz <= max_size) { count++; bytes += sz; }
        }
        HeapUnlock(heaps[i]);
    }
    return which == 1 ? bytes : count;
}

/* 1 while the process exists. */
long long salivo_sys_alive(long long pid) {
    HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!ph) return GetLastError() == ERROR_ACCESS_DENIED ? 1 : 0;
    DWORD r = WaitForSingleObject(ph, 0);
    CloseHandle(ph);
    return r == WAIT_TIMEOUT ? 1 : 0;
}

/* Opens a URL or file with its default handler (ShellExecute "open"). */
long long salivo_sys_open(const char* target) {
    wchar_t* w = widen(target);
    if (!w) return SYS_OS;
    INT_PTR r = (INT_PTR)ShellExecuteW(0, L"open", w, 0, 0, SW_SHOWNORMAL);
    free(w);
    return r > 32 ? 0 : SYS_OS;
}

/* Opens Explorer with the file selected. */
long long salivo_sys_reveal(const char* path) {
    wchar_t* w = widen(path);
    if (!w) return SYS_OS;
    if (GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) { free(w); return SYS_INVALID_ARG; }
    size_t n = wcslen(w) + 16;
    wchar_t* args = (wchar_t*)malloc(sizeof(wchar_t) * n);
    if (!args) { free(w); return SYS_OS; }
    _snwprintf(args, n, L"/select,\"%ls\"", w);
    INT_PTR r = (INT_PTR)ShellExecuteW(0, L"open", L"explorer.exe", args, 0, SW_SHOWNORMAL);
    free(args);
    free(w);
    return r > 32 ? 0 : SYS_OS;
}

#else /* !_WIN32 */

#include <errno.h>

#if defined(__linux__)
/* ---------- Linux: processes, memory, CPU time from /proc (no strings for the caller to keep) ---------- */
#include <dirent.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int used;
    long long n;
    long long* ints;   /* SYS_PROC_INTS per process */
    char (*names)[64];
} SysLxSnap;
#define SYS_PROC_INTS 15
#define SYS_MAX_SNAPS 16
static SysLxSnap g_lx_snaps[SYS_MAX_SNAPS + 1];

static long long lx_ticks_per_sec(void) { long t = sysconf(_SC_CLK_TCK); return t > 0 ? t : 100; }

/* Reads a small text file into buf; returns the length (0 when unreadable) */
static size_t lx_slurp(const char* path, char* buf, size_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) { buf[0] = 0; return 0; }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return n;
}

static long long sys_unlocked_proc_snap(void) {
    long long h = 0;
    for (int i = 1; i <= SYS_MAX_SNAPS; i++) if (!g_lx_snaps[i].used) { h = i; break; }
    if (!h) return -11;
    DIR* d = opendir("/proc");
    if (!d) return SYS_OS;
    size_t cap = 256, n = 0;
    long long* ints = (long long*)calloc(cap * SYS_PROC_INTS, sizeof(long long));
    char (*names)[64] = calloc(cap, 64);
    if (!ints || !names) { free(ints); free(names); closedir(d); return SYS_OS; }
    long long hz = lx_ticks_per_sec();
    long long page = sysconf(_SC_PAGESIZE);
    struct dirent* de;
    char path[64], buf[1024];
    while ((de = readdir(d))) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        snprintf(path, sizeof path, "/proc/%s/stat", de->d_name);
        if (!lx_slurp(path, buf, sizeof buf)) continue;
        char* open = strchr(buf, '(');
        char* close = strrchr(buf, ')');
        if (!open || !close || close < open) continue;
        if (n == cap) {
            size_t nc = cap * 2;
            long long* ni = (long long*)realloc(ints, nc * SYS_PROC_INTS * sizeof(long long));
            if (!ni) break;
            ints = ni;
            char (*nn)[64] = realloc(names, nc * 64);
            if (!nn) break;
            names = nn;
            cap = nc;
        }
        long long* v = ints + n * SYS_PROC_INTS;
        memset(v, 0, SYS_PROC_INTS * sizeof(long long));
        size_t len = (size_t)(close - open - 1);
        if (len > 63) len = 63;
        memcpy(names[n], open + 1, len);
        names[n][len] = 0;
        /* fields after ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt
           utime stime cutime cstime priority nice threads itreal starttime vsize rss */
        char state = 0;
        long long ppid = 0, pgrp = 0, session = 0, utime = 0, stime = 0, threads = 0, start = 0, rss = 0;
        long long skip;
        sscanf(close + 2, "%c %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld",
               &state, &ppid, &pgrp, &session, &skip, &skip, &skip, &skip, &skip, &skip, &skip, &utime, &stime,
               &skip, &skip, &skip, &skip, &threads, &skip, &start, &skip, &rss);
        v[0] = atoll(de->d_name);
        v[1] = ppid;
        v[2] = threads;
        v[4] = session;
        v[5] = rss * page;
        v[6] = rss * page;
        v[7] = (utime + stime) * (10000000 / hz);   /* 100 ns units, like Windows */
        v[8] = start * (10000000 / hz);
        v[9] = state == 'T' || state == 't' ? 1 : 0;
        v[14] = (unsigned char)state;
        /* storage traffic (needs task I/O accounting; stays 0 without it) */
        snprintf(path, sizeof path, "/proc/%s/io", de->d_name);
        if (lx_slurp(path, buf, sizeof buf)) {
            char* r = strstr(buf, "\nread_bytes:");
            char* w = strstr(buf, "\nwrite_bytes:");
            if (r) v[11] = atoll(r + 12);
            if (w) v[12] = atoll(w + 13);
        }
        n++;
    }
    closedir(d);
    g_lx_snaps[h].used = 1;
    g_lx_snaps[h].n = (long long)n;
    g_lx_snaps[h].ints = ints;
    g_lx_snaps[h].names = names;
    return h;
}

static SysLxSnap* lx_snap(long long h) { return h >= 1 && h <= SYS_MAX_SNAPS && g_lx_snaps[h].used ? &g_lx_snaps[h] : NULL; }

static long long sys_unlocked_proc_count(long long h) { SysLxSnap* s = lx_snap(h); return s ? s->n : SYS_INVALID_HANDLE; }

static long long sys_unlocked_proc_int(long long h, long long i, long long f) {
    SysLxSnap* s = lx_snap(h);
    if (!s) return SYS_INVALID_HANDLE;
    if (i < 0 || i >= s->n || f < 0 || f >= SYS_PROC_INTS) return SYS_INVALID_ARG;
    return s->ints[i * SYS_PROC_INTS + f];
}

static char* sys_unlocked_proc_text(long long h, long long i, long long f) {
    SysLxSnap* s = lx_snap(h);
    if (!s || i < 0 || i >= s->n || f != 0) return out_cstr("");
    return out_cstr(s->names[i]);
}

static long long sys_unlocked_proc_free(long long h) {
    SysLxSnap* s = lx_snap(h);
    if (!s) return SYS_INVALID_HANDLE;
    free(s->ints);
    free(s->names);
    memset(s, 0, sizeof *s);
    return 0;
}

char* salivo_sys_proc_path(long long pid) {
    char link[64], target[1024];
    snprintf(link, sizeof link, "/proc/%lld/exe", pid);
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n < 0) n = 0;
    target[n] = 0;
    return out_cstr(target);
}

static long long lx_meminfo(const char* key) {
    char buf[8192];
    if (!lx_slurp("/proc/meminfo", buf, sizeof buf)) return 0;
    char* p = strstr(buf, key);
    if (!p) return 0;
    return atoll(p + strlen(key)) * 1024;
}

/* 0 physical 1 available 2 installed 3 committed 4 commit limit 5 page cache (bytes) 9 processes 10 threads */
long long salivo_sys_mem(long long f) {
    switch (f) {
    case 0: case 2: return lx_meminfo("MemTotal:");
    case 1: return lx_meminfo("MemAvailable:");
    case 3: return lx_meminfo("Committed_AS:");
    case 4: return lx_meminfo("CommitLimit:");
    case 5: return lx_meminfo("Cached:");
    case 6: case 7: case 8: return 0;
    case 9: case 10: {
        char buf[128];
        if (!lx_slurp("/proc/loadavg", buf, sizeof buf)) return 0;
        char* slash = strchr(buf, '/');
        return slash ? atoll(slash + 1) : 0;
    }
    default: return SYS_INVALID_ARG;
    }
}

long long salivo_sys_uptime_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* All CPUs since boot in 1/100 s: 0 busy, 1 idle; 2 logical CPUs */
long long salivo_sys_cpu(long long which) {
    if (which == 2) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? n : 1; }
    char buf[512];
    if (!lx_slurp("/proc/stat", buf, sizeof buf)) return SYS_OS;
    long long v[8] = { 0 };
    sscanf(buf, "cpu %lld %lld %lld %lld %lld %lld %lld %lld", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    long long hz = lx_ticks_per_sec();
    long long idle = v[3] + v[4];
    long long busy = v[0] + v[1] + v[2] + v[5] + v[6] + v[7];
    if (which == 0) return busy * 100 / hz;
    if (which == 1) return idle * 100 / hz;
    return SYS_INVALID_ARG;
}

#else
static long long sys_unlocked_proc_snap(void) { return SYS_UNSUPPORTED; }
static long long sys_unlocked_proc_count(long long h) { (void)h; return SYS_UNSUPPORTED; }
static long long sys_unlocked_proc_int(long long h, long long i, long long f) { (void)h; (void)i; (void)f; return SYS_UNSUPPORTED; }
static char* sys_unlocked_proc_text(long long h, long long i, long long f) { (void)h; (void)i; (void)f; return out_cstr(""); }
static long long sys_unlocked_proc_free(long long h) { (void)h; return SYS_UNSUPPORTED; }
char* salivo_sys_proc_path(long long pid) { (void)pid; return out_cstr(""); }
long long salivo_sys_mem(long long f) { (void)f; return SYS_UNSUPPORTED; }
long long salivo_sys_uptime_ms(void) { return SYS_UNSUPPORTED; }
long long salivo_sys_cpu(long long which) { (void)which; return SYS_UNSUPPORTED; }
#endif
#if defined(__linux__)
#include <signal.h>
/* 1 while the process exists and has not exited (a zombie waiting for its parent counts as ended) */
static int lx_running(long long pid) {
    char path[64], buf[256];
    snprintf(path, sizeof path, "/proc/%lld/stat", pid);
    if (!lx_slurp(path, buf, sizeof buf)) return 0;
    char* close = strrchr(buf, ')');
    return close && close[1] && close[2] != 'Z' && close[2] != 'X';
}

/* Like TerminateProcess: SIGTERM first; a program that ignores it (an interactive shell) gets SIGKILL
   after 300 ms */
long long salivo_sys_terminate(long long pid) {
    if (pid <= 1) return SYS_INVALID_ARG;
    if (kill((pid_t)pid, SIGTERM) != 0) return -errno;
    for (int i = 0; i < 30; i++) {
        struct timespec ts = { 0, 10000000 };
        nanosleep(&ts, 0);
        if (!lx_running(pid)) return 0;
    }
    return kill((pid_t)pid, SIGKILL) == 0 || errno == ESRCH ? 0 : -errno;
}
long long salivo_sys_alive(long long pid) { return pid > 0 && (kill((pid_t)pid, 0) == 0 || errno == EPERM) ? 1 : 0; }
#else
long long salivo_sys_terminate(long long pid) { (void)pid; return SYS_UNSUPPORTED; }
long long salivo_sys_alive(long long pid) { (void)pid; return SYS_UNSUPPORTED; }
#endif
static long long sys_unlocked_pdh_open(void) { return SYS_UNSUPPORTED; }
static long long sys_unlocked_pdh_add(long long q, const char* p) { (void)q; (void)p; return SYS_UNSUPPORTED; }
static long long sys_unlocked_pdh_collect(long long q) { (void)q; return SYS_UNSUPPORTED; }
static long long sys_unlocked_pdh_count(long long c) { (void)c; return SYS_UNSUPPORTED; }
static char* sys_unlocked_pdh_name(long long c, long long i) { (void)c; (void)i; return out_cstr(""); }
static long long sys_unlocked_pdh_value(long long c, long long i) { (void)c; (void)i; return 0; }
static long long sys_unlocked_pdh_close(long long q) { (void)q; return SYS_UNSUPPORTED; }
char* salivo_sys_reg_names(long long r, const char* p) { (void)r; (void)p; return out_cstr(""); }
char* salivo_sys_reg_keys(long long r, const char* p) { (void)r; (void)p; return out_cstr(""); }
char* salivo_sys_reg_str(long long r, const char* p, const char* n) { (void)r; (void)p; (void)n; return out_cstr(""); }
long long salivo_sys_reg_byte(long long r, const char* p, const char* n, long long i) { (void)r; (void)p; (void)n; (void)i; return -1; }
char* salivo_sys_file_info(const char* p, const char* k) { (void)p; (void)k; return out_cstr(""); }
static long long sys_unlocked_icon_png(const char* e, const char* o) { (void)e; (void)o; return SYS_UNSUPPORTED; }
static long long sys_unlocked_svc_snap(void) { return SYS_UNSUPPORTED; }
static long long sys_unlocked_svc_count(long long h) { (void)h; return SYS_UNSUPPORTED; }
static long long sys_unlocked_svc_int(long long h, long long i, long long f) { (void)h; (void)i; (void)f; return SYS_UNSUPPORTED; }
static char* sys_unlocked_svc_text(long long h, long long i, long long f) { (void)h; (void)i; (void)f; return out_cstr(""); }
static long long sys_unlocked_svc_free(long long h) { (void)h; return SYS_UNSUPPORTED; }
static char* sys_unlocked_info(const char* k) { (void)k; return out_cstr(""); }
static long long sys_unlocked_info_int(const char* k) { (void)k; return SYS_UNSUPPORTED; }
long long salivo_sys_heap_blocks(long long a, long long b, long long w) { (void)a; (void)b; (void)w; return SYS_UNSUPPORTED; }
long long salivo_sys_open(const char* t) { (void)t; return SYS_UNSUPPORTED; }
long long salivo_sys_reveal(const char* p) { (void)p; return SYS_UNSUPPORTED; }

#endif

/* ---------- Linux system primitives (Salivo OS init and services) ---------- */

#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

long long salivo_sys_mount(const char* src, const char* target, const char* type, long long flags) {
    return mount(src, target, type, (unsigned long)flags, NULL) == 0 ? 0 : -errno;
}

long long salivo_sys_spawn(const char* path, const char* args, const char* tty) {
    if (!path || !*path) return SYS_INVALID_ARG;
    /* argv lives in the parent's memory until exec copies it */
    size_t n = args ? strlen(args) : 0;
    char* buf = (char*)malloc(n + 1);
    if (!buf) return SYS_OS;
    memcpy(buf, args ? args : "", n + 1);
    size_t count = 2;
    for (size_t i = 0; i < n; i++) if (buf[i] == '\n') count++;
    char** argv = (char**)calloc(count + 1, sizeof(char*));
    if (!argv) { free(buf); return SYS_OS; }
    size_t argc = 0;
    if (n == 0) argv[argc++] = (char*)path;
    else for (char* t = buf; t; ) { char* nl = strchr(t, '\n'); if (nl) *nl = 0; argv[argc++] = t; t = nl ? nl + 1 : NULL; }
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (tty && *tty) {
            int fd = open(tty, O_RDWR);
            if (fd >= 0) {
                ioctl(fd, TIOCSCTTY, 1);
                dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
                if (fd > 2) close(fd);
            }
        }
        execv(path, argv);
        _exit(127);
    }
    long long r = pid > 0 ? (long long)pid : -errno;
    free(argv);
    free(buf);
    return r;
}

long long salivo_sys_reap(long long block) {
    int st = 0;
    pid_t pid = waitpid(-1, &st, block ? 0 : WNOHANG);
    if (pid < 0) return errno == ECHILD ? SYS_INVALID_HANDLE : -errno;
    return (long long)pid;
}

long long salivo_sys_power(long long mode) {
    sync();
    return reboot(mode == 1 ? RB_AUTOBOOT : RB_POWER_OFF) == 0 ? 0 : -errno;
}

long long salivo_sys_hostname(const char* name) {
    if (!name) return SYS_INVALID_ARG;
    return sethostname(name, strlen(name)) == 0 ? 0 : -errno;
}

/* Pseudo-terminals: a terminal emulator written in Salivo drives a shell through these. */
#include <termios.h>
#include <signal.h>
typedef struct { int fd; pid_t pid; int alive; int hup; } SysPty;
#define SYS_PTYS 16
static SysPty g_pty[SYS_PTYS + 1];

static SysPty* pty_get(long long h) { return h >= 1 && h <= SYS_PTYS && g_pty[h].fd > 0 ? &g_pty[h] : NULL; }

long long salivo_sys_pty_open(const char* path, const char* args, long long cols, long long rows) {
    if (!path || !*path || cols < 1 || rows < 1) return SYS_INVALID_ARG;
    long long h = 0;
    for (int i = 1; i <= SYS_PTYS; i++) if (g_pty[i].fd <= 0) { h = i; break; }
    if (!h) return -11;
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (m < 0) return -errno;
    if (grantpt(m) != 0 || unlockpt(m) != 0) { int e = errno; close(m); return -e; }
    char* sname = ptsname(m);
    if (!sname) { int e = errno; close(m); return -e; }
    char slave[128];
    snprintf(slave, sizeof slave, "%s", sname);
    struct winsize ws; memset(&ws, 0, sizeof ws);
    ws.ws_col = (unsigned short)cols; ws.ws_row = (unsigned short)rows;
    size_t n = args ? strlen(args) : 0;
    char* buf = (char*)malloc(n + 1);
    char** argv = (char**)calloc(n + 2, sizeof(char*));
    if (!buf || !argv) { free(buf); free(argv); close(m); return SYS_OS; }
    memcpy(buf, args ? args : "", n + 1);
    size_t argc = 0;
    if (n == 0) argv[argc++] = (char*)path;
    else for (char* t = buf; t; ) { char* nl = strchr(t, '\n'); if (nl) *nl = 0; argv[argc++] = t; t = nl ? nl + 1 : NULL; }
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int sfd = open(slave, O_RDWR);
        if (sfd < 0) _exit(127);
        ioctl(sfd, TIOCSCTTY, 0);
        ioctl(sfd, TIOCSWINSZ, &ws);
        dup2(sfd, 0); dup2(sfd, 1); dup2(sfd, 2);
        if (sfd > 2) close(sfd);
        setenv("TERM", "xterm-256color", 1);
        execv(path, argv);
        _exit(127);
    }
    free(argv); free(buf);
    if (pid < 0) { int e = errno; close(m); return -e; }
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);
    ioctl(m, TIOCSWINSZ, &ws);
    g_pty[h].fd = m; g_pty[h].pid = pid; g_pty[h].alive = 1;
    return h;
}

char* salivo_sys_pty_read(long long h) {
    SysPty* p = pty_get(h);
    if (!p) return out_cstr("");
    size_t cap = 4096, len = 0;
    char* buf = (char*)malloc(cap + 1);
    if (!buf) return out_cstr("");
    for (;;) {
        if (len == cap) { char* nb = (char*)realloc(buf, cap * 2 + 1); if (!nb) break; buf = nb; cap *= 2; }
        ssize_t r = read(p->fd, buf + len, cap - len);
        if (r <= 0) break;
        len += (size_t)r;
        if (len >= 1 << 20) break;   /* at most 1 MiB per read: the caller draws between reads */
    }
    /* Salivo strings end at NUL: drop stray zero bytes */
    size_t w = 0;
    for (size_t i = 0; i < len; i++) if (buf[i]) buf[w++] = buf[i];
    buf[w] = 0;
    char* out = out_cstr(buf);
    free(buf);
    return out;
}

/* 1 when output is waiting or the program hung up (a read will not block), 0 otherwise */
long long salivo_sys_pty_ready(long long h) {
    SysPty* p = pty_get(h);
    if (!p) return SYS_INVALID_HANDLE;
    struct pollfd pf = { p->fd, POLLIN, 0 };
    return poll(&pf, 1, 0) > 0 ? 1 : 0;
}

long long salivo_sys_pty_write(long long h, const char* text) {
    SysPty* p = pty_get(h);
    if (!p) return SYS_INVALID_HANDLE;
    if (!text) return 0;
    size_t n = strlen(text), done = 0;
    while (done < n) {
        ssize_t r = write(p->fd, text + done, n - done);
        if (r < 0) {
            if (errno == EAGAIN) { struct pollfd pf = { p->fd, POLLOUT, 0 }; poll(&pf, 1, 100); continue; }
            return done ? (long long)done : -errno;
        }
        done += (size_t)r;
    }
    return (long long)done;
}

long long salivo_sys_pty_resize(long long h, long long cols, long long rows) {
    SysPty* p = pty_get(h);
    if (!p) return SYS_INVALID_HANDLE;
    if (cols < 1 || rows < 1) return SYS_INVALID_ARG;
    struct winsize ws; memset(&ws, 0, sizeof ws);
    ws.ws_col = (unsigned short)cols; ws.ws_row = (unsigned short)rows;
    return ioctl(p->fd, TIOCSWINSZ, &ws) == 0 ? 0 : -errno;
}

long long salivo_sys_pty_state(long long h) {
    SysPty* p = pty_get(h);
    if (!p) return SYS_INVALID_HANDLE;
    if (p->alive) {
        int st;
        pid_t r = waitpid(p->pid, &st, WNOHANG);
        if (r == p->pid || (r < 0 && errno == ECHILD)) p->alive = 0;
    }
    return p->alive;
}

long long salivo_sys_pty_close(long long h) {
    SysPty* p = pty_get(h);
    if (!p) return SYS_INVALID_HANDLE;
    if (p->alive) { kill(-p->pid, SIGHUP); kill(p->pid, SIGHUP); waitpid(p->pid, NULL, WNOHANG); }
    close(p->fd);
    memset(p, 0, sizeof *p);
    return 0;
}

#else

long long salivo_sys_mount(const char* s, const char* t, const char* y, long long f) { (void)s; (void)t; (void)y; (void)f; return SYS_UNSUPPORTED; }
long long salivo_sys_spawn(const char* p, const char* a, const char* t) { (void)p; (void)a; (void)t; return SYS_UNSUPPORTED; }
long long salivo_sys_reap(long long b) { (void)b; return SYS_UNSUPPORTED; }
long long salivo_sys_power(long long m) { (void)m; return SYS_UNSUPPORTED; }
long long salivo_sys_hostname(const char* n) { (void)n; return SYS_UNSUPPORTED; }
long long salivo_sys_pty_open(const char* p, const char* a, long long c, long long r) { (void)p; (void)a; (void)c; (void)r; return SYS_UNSUPPORTED; }
char* salivo_sys_pty_read(long long h) { (void)h; return out_cstr(""); }
long long salivo_sys_pty_ready(long long h) { (void)h; return SYS_UNSUPPORTED; }
long long salivo_sys_pty_write(long long h, const char* t) { (void)h; (void)t; return SYS_UNSUPPORTED; }
long long salivo_sys_pty_resize(long long h, long long c, long long r) { (void)h; (void)c; (void)r; return SYS_UNSUPPORTED; }
long long salivo_sys_pty_state(long long h) { (void)h; return SYS_UNSUPPORTED; }
long long salivo_sys_pty_close(long long h) { (void)h; return SYS_UNSUPPORTED; }

#endif

/* ---------- Text store (all platforms) ----------
 * Numbered slots holding copies of strings in runtime memory. A program whose own string memory
 * is reclaimed per request or per task (salivo.std.web) keeps long-lived text here; writing a
 * slot frees its previous value. */

#define SYS_TEXT_SLOTS 65536
static char** g_text;

static long long sys_unlocked_text_set(long long slot, const char* text) {
    if (slot < 0 || slot >= SYS_TEXT_SLOTS) return SYS_INVALID_ARG;
    if (!g_text) {
        g_text = (char**)calloc(SYS_TEXT_SLOTS, sizeof(char*));
        if (!g_text) return SYS_OS;
    }
    size_t n = text ? strlen(text) : 0;
    char* copy = (char*)malloc(n + 1);
    if (!copy) return SYS_OS;
    if (n) memcpy(copy, text, n);
    copy[n] = 0;
    free(g_text[slot]);
    g_text[slot] = copy;
    return 0;
}

static char* sys_unlocked_text_get(long long slot) {
    if (!g_text || slot < 0 || slot >= SYS_TEXT_SLOTS) return out_cstr("");
    return out_cstr(g_text[slot]);
}

/* ---------- Command line and child processes (all platforms) ----------
 * argv: the program's arguments (index 0 is the program). run: executes a command through the
 * system shell (cmd.exe /d /s /c, or /bin/sh -c), optionally feeding stdin text, and captures
 * stdout and stderr separately. Redirection goes through temporary files, so a child writing a
 * lot to both streams cannot deadlock against us. Output containing a 0 byte is cut there
 * (Salivo strings are NUL-terminated). Run handles are guarded by a lock; any thread may use them. */

#ifdef _WIN32
#include <io.h>
static SRWLOCK g_run_mu = SRWLOCK_INIT;
static void run_lock(void) { AcquireSRWLockExclusive(&g_run_mu); }
static void run_unlock(void) { ReleaseSRWLockExclusive(&g_run_mu); }
#else
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
static pthread_mutex_t g_run_mu = PTHREAD_MUTEX_INITIALIZER;
static void run_lock(void) { pthread_mutex_lock(&g_run_mu); }
static void run_unlock(void) { pthread_mutex_unlock(&g_run_mu); }
#endif

static int g_argc = -1;
static char** g_argv;

static void args_load(void) {
    if (g_argc >= 0) return;
    g_argc = 0;
#ifdef _WIN32
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) return;
    g_argv = (char**)calloc((size_t)n + 1, sizeof(char*));
    for (int i = 0; g_argv && i < n; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
        g_argv[i] = (char*)malloc(len > 0 ? (size_t)len : 1);
        if (g_argv[i]) { if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, g_argv[i], len, NULL, NULL); else g_argv[i][0] = 0; }
    }
    LocalFree(w);
    if (g_argv) g_argc = n;
#elif defined(__APPLE__)
    extern int* _NSGetArgc(void);
    extern char*** _NSGetArgv(void);
    g_argc = *_NSGetArgc();
    g_argv = *_NSGetArgv();
#else
    /* /proc/self/cmdline: arguments separated by NUL bytes */
    FILE* f = fopen("/proc/self/cmdline", "rb");
    if (!f) return;
    size_t cap = 4096, n = 0;
    char* buf = (char*)malloc(cap);
    int c;
    while (buf && (c = fgetc(f)) != EOF) {
        if (n + 1 >= cap) { char* nb = (char*)realloc(buf, cap *= 2); if (!nb) { free(buf); buf = NULL; break; } buf = nb; }
        buf[n++] = (char)c;
    }
    fclose(f);
    if (!buf) return;
    buf[n] = 0;
    int count = 0;
    for (size_t i = 0; i < n; i++) if (buf[i] == 0) count++;
    g_argv = (char**)calloc((size_t)count + 1, sizeof(char*));
    if (!g_argv) return;
    size_t at = 0;
    for (int i = 0; i < count; i++) { g_argv[i] = buf + at; at += strlen(buf + at) + 1; }
    g_argc = count;
#endif
}

#include <sys/stat.h>
#include <errno.h>
/* 0 size, 1 modified time (Unix seconds), 2 directory flag; -errno when the path is missing */
long long salivo_sys_file_stat(const char* path, long long which) {
    if (!path || !*path) return SYS_INVALID_ARG;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return -errno;
    int dir = (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return -errno;
    int dir = S_ISDIR(st.st_mode);
#endif
    if (which == 0) return (long long)st.st_size;
    if (which == 1) return (long long)st.st_mtime;
    if (which == 2) return dir;
    return SYS_INVALID_ARG;
}

/* Whole-machine counters for a task manager (Linux; -12 elsewhere):
   0 disk bytes read 1 disk bytes written 2 disk busy time (ms) 3 disk capacity (bytes)
   4 network bytes received 5 network bytes sent 6 fastest link (Mbps; 0 unknown, -1 no link up)
   7 CPU clock (MHz, average of the cores; 0 unknown) 8 GPU busy (%; -1 unknown)
   9 battery charge (%; -1 no battery) 10 on mains power (1/0; -1 unknown) 11 wireless link up (1/0)
   12 GPU temperature (milli degrees C; -1 unknown) 13 physical disks */
#if defined(__linux__)
static long long lx_read_ll(const char* path, long long fallback) {
    char buf[64];
    if (!lx_slurp(path, buf, sizeof buf)) return fallback;
    char* end = 0;
    long long v = strtoll(buf, &end, 10);
    return end == buf ? fallback : v;
}

static int lx_entry_ok(const char* name) { return name[0] != '.'; }

long long salivo_sys_stat(long long f) {
    char path[512], buf[512];
    struct dirent* de;
    DIR* d;
    long long total = 0, found = 0, best = -1;
    if ((f >= 0 && f <= 3) || f == 13) {
        /* whole disks have a device link; loop, ram and zram devices do not */
        if (!(d = opendir("/sys/block"))) return 0;
        while ((de = readdir(d))) {
            if (!lx_entry_ok(de->d_name) || strncmp(de->d_name, "sr", 2) == 0) continue;
            snprintf(path, sizeof path, "/sys/block/%s/device", de->d_name);
            if (access(path, F_OK) != 0) continue;
            found++;
            if (f == 3) {
                snprintf(path, sizeof path, "/sys/block/%s/size", de->d_name);
                total += lx_read_ll(path, 0) * 512;
                continue;
            }
            snprintf(path, sizeof path, "/sys/block/%s/stat", de->d_name);
            if (!lx_slurp(path, buf, sizeof buf)) continue;
            long long v[10] = { 0 };
            sscanf(buf, "%lld %lld %lld %lld %lld %lld %lld %lld %lld %lld",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
            if (f == 0) total += v[2] * 512;
            else if (f == 1) total += v[6] * 512;
            else if (f == 2) total += v[9];
        }
        closedir(d);
        return f == 13 ? found : total;
    }
    if ((f >= 4 && f <= 6) || f == 11) {
        if (!(d = opendir("/sys/class/net"))) return f == 6 ? -1 : 0;
        while ((de = readdir(d))) {
            if (!lx_entry_ok(de->d_name) || strcmp(de->d_name, "lo") == 0) continue;
            if (f == 4 || f == 5) {
                snprintf(path, sizeof path, "/sys/class/net/%s/statistics/%s", de->d_name, f == 4 ? "rx_bytes" : "tx_bytes");
                total += lx_read_ll(path, 0);
                continue;
            }
            snprintf(path, sizeof path, "/sys/class/net/%s/operstate", de->d_name);
            lx_slurp(path, buf, sizeof buf);
            if (strncmp(buf, "up", 2) != 0) continue;
            if (f == 11) {
                snprintf(path, sizeof path, "/sys/class/net/%s/wireless", de->d_name);
                if (access(path, F_OK) == 0) found = 1;
                continue;
            }
            snprintf(path, sizeof path, "/sys/class/net/%s/speed", de->d_name);
            long long speed = lx_read_ll(path, 0);
            if (speed < 0) speed = 0;
            if (speed > best) best = speed;
        }
        closedir(d);
        if (f == 6) return best;
        if (f == 11) return found;
        return total;
    }
    if (f == 7) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        for (long c = 0; c < n && c < 4096; c++) {
            snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/cpufreq/scaling_cur_freq", c);
            long long khz = lx_read_ll(path, -1);
            if (khz > 0) { total += khz; found++; }
        }
        if (found) return total / found / 1000;
        /* no cpufreq driver (virtual machines): the clock the kernel measured at boot */
        char info[4096];
        if (!lx_slurp("/proc/cpuinfo", info, sizeof info)) return 0;
        char* p = strstr(info, "cpu MHz");
        if (!p || !(p = strchr(p, ':'))) return 0;
        return atoll(p + 1);
    }
    if (f == 8 || f == 12) {
        for (int c = 0; c < 8; c++) {
            if (f == 8) {
                snprintf(path, sizeof path, "/sys/class/drm/card%d/device/gpu_busy_percent", c);
                long long v = lx_read_ll(path, -1);
                if (v >= 0) return v;
                continue;
            }
            snprintf(path, sizeof path, "/sys/class/drm/card%d/device/hwmon", c);
            if (!(d = opendir(path))) continue;
            while ((de = readdir(d))) {
                if (strncmp(de->d_name, "hwmon", 5) != 0) continue;
                snprintf(buf, sizeof buf, "%s/%s/temp1_input", path, de->d_name);
                best = lx_read_ll(buf, -1);
                if (best >= 0) break;
            }
            closedir(d);
            if (best >= 0) return best;
        }
        return -1;
    }
    if (f == 9 || f == 10) {
        if (!(d = opendir("/sys/class/power_supply"))) return -1;
        while ((de = readdir(d))) {
            if (!lx_entry_ok(de->d_name)) continue;
            snprintf(path, sizeof path, "/sys/class/power_supply/%s/type", de->d_name);
            lx_slurp(path, buf, sizeof buf);
            if (f == 9 && strncmp(buf, "Battery", 7) == 0) {
                snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity", de->d_name);
                best = lx_read_ll(path, -1);
                if (best >= 0) break;
            }
            if (f == 10 && strncmp(buf, "Mains", 5) == 0) {
                snprintf(path, sizeof path, "/sys/class/power_supply/%s/online", de->d_name);
                best = lx_read_ll(path, -1);
                if (best >= 0) break;
            }
        }
        closedir(d);
        return best;
    }
    return SYS_INVALID_ARG;
}
#else
long long salivo_sys_stat(long long f) { (void)f; return SYS_UNSUPPORTED; }
#endif

char* salivo_sys_byte(long long b) { char c[2] = { (char)b, 0 }; return out_cstr(b >= 1 && b <= 255 ? c : ""); }

long long salivo_sys_argc(void) { args_load(); return g_argc; }

char* salivo_sys_argv(long long i) {
    args_load();
    if (i < 0 || i >= g_argc || !g_argv[i]) return out_cstr("");
    return out_cstr(g_argv[i]);
}

typedef struct { int used; long long code; char* out; char* err; } SysRun;
#define SYS_RUNS 64
static SysRun g_runs[SYS_RUNS + 1];

static char* slurp(FILE* f) {
    size_t cap = 4096, n = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return NULL;
    size_t r;
    rewind(f);
    while ((r = fread(buf + n, 1, cap - n - 1, f)) > 0) {
        n += r;
        if (n + 1 >= cap) { char* nb = (char*)realloc(buf, cap *= 2); if (!nb) { free(buf); return NULL; } buf = nb; }
    }
    buf[n] = 0;
    return buf;
}

/* Runs cmd with stdin text `input` ("" = empty stdin); a handle (> 0) to read the result from */
long long salivo_sys_run(const char* cmd, const char* input) {
    if (!cmd || !*cmd) return SYS_INVALID_ARG;
    FILE* fin = tmpfile();
    FILE* fout = tmpfile();
    FILE* ferr = tmpfile();
    long long code = SYS_OS;
    if (!fin || !fout || !ferr) goto done;
    if (input && *input) { fputs(input, fin); fflush(fin); }
    rewind(fin);
#ifdef _WIN32
    {
        HANDLE hin = (HANDLE)_get_osfhandle(_fileno(fin));
        HANDLE hout = (HANDLE)_get_osfhandle(_fileno(fout));
        HANDLE herr = (HANDLE)_get_osfhandle(_fileno(ferr));
        HANDLE dup_in, dup_out, dup_err, me = GetCurrentProcess();
        if (!DuplicateHandle(me, hin, me, &dup_in, 0, TRUE, DUPLICATE_SAME_ACCESS)) goto done;
        if (!DuplicateHandle(me, hout, me, &dup_out, 0, TRUE, DUPLICATE_SAME_ACCESS)) { CloseHandle(dup_in); goto done; }
        if (!DuplicateHandle(me, herr, me, &dup_err, 0, TRUE, DUPLICATE_SAME_ACCESS)) { CloseHandle(dup_in); CloseHandle(dup_out); goto done; }
        int wl = MultiByteToWideChar(CP_UTF8, 0, cmd, -1, NULL, 0);
        wchar_t* wcmd = (wchar_t*)malloc(((size_t)wl + 32) * sizeof(wchar_t));
        if (wcmd) {
            wcscpy(wcmd, L"cmd.exe /d /s /c \"");
            MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd + wcslen(wcmd), wl);
            wcscat(wcmd, L"\"");
            STARTUPINFOW si;
            PROCESS_INFORMATION pi;
            memset(&si, 0, sizeof si);
            si.cb = sizeof si;
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = dup_in;
            si.hStdOutput = dup_out;
            si.hStdError = dup_err;
            if (CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, INFINITE);
                DWORD ec = 1;
                GetExitCodeProcess(pi.hProcess, &ec);
                code = (long long)ec;
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            free(wcmd);
        }
        CloseHandle(dup_in); CloseHandle(dup_out); CloseHandle(dup_err);
    }
#else
    {
        fflush(NULL);
        pid_t pid = fork();
        if (pid == 0) {
            dup2(fileno(fin), 0);
            dup2(fileno(fout), 1);
            dup2(fileno(ferr), 2);
            execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
            _exit(127);
        }
        if (pid > 0) {
            int st = 0;
            pid_t w;
            while ((w = waitpid(pid, &st, 0)) < 0 && errno == EINTR) {}
            /* ECHILD: someone else (sys.reap) collected it; the exit status is lost */
            code = w < 0 ? 1 : WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        }
    }
#endif
done:;
    long long h = code == SYS_OS ? SYS_OS : SYS_INVALID_HANDLE;
    if (code != SYS_OS) {
        char* out = slurp(fout);
        char* err = slurp(ferr);
        run_lock();
        for (int i = 1; i <= SYS_RUNS; i++) {
            if (!g_runs[i].used) { g_runs[i].used = 1; g_runs[i].code = code; g_runs[i].out = out; g_runs[i].err = err; h = i; break; }
        }
        run_unlock();
        if (h < 0) { free(out); free(err); h = -11; }
    }
    if (fin) fclose(fin);
    if (fout) fclose(fout);
    if (ferr) fclose(ferr);
    return h;
}

static SysRun* run_get(long long h) { return h >= 1 && h <= SYS_RUNS && g_runs[h].used ? &g_runs[h] : NULL; }

long long salivo_sys_run_code(long long h) { run_lock(); SysRun* r = run_get(h); long long c = r ? r->code : SYS_INVALID_HANDLE; run_unlock(); return c; }
char* salivo_sys_run_out(long long h) { run_lock(); SysRun* r = run_get(h); char* s = out_cstr(r ? r->out : ""); run_unlock(); return s; }
char* salivo_sys_run_err(long long h) { run_lock(); SysRun* r = run_get(h); char* s = out_cstr(r ? r->err : ""); run_unlock(); return s; }
long long salivo_sys_run_free(long long h) {
    run_lock();
    SysRun* r = run_get(h);
    if (r) { free(r->out); free(r->err); memset(r, 0, sizeof *r); }
    run_unlock();
    return r ? 0 : SYS_INVALID_HANDLE;
}

/* Stage 38.1 desktop windowing / canvas and Stage 38.2 game rendering */
#include "salivo_gfx_runtime.inc"
#include "salivo_gfx_linux.inc"

/* ---- Thread safety: the snapshot, counter, service, info, icon and text-store calls share tables
 * and caches, so each public entry point holds one re-entrant lock (an entry point that calls
 * another keeps it). Generated from the 'one thread' rows of salivo_sys_abi.def. */
#ifdef _WIN32
static SRWLOCK g_sys_mu = SRWLOCK_INIT;
static __declspec(thread) char g_sys_tl;
static void sys_mu_lock(void) { AcquireSRWLockExclusive(&g_sys_mu); }
static void sys_mu_unlock(void) { ReleaseSRWLockExclusive(&g_sys_mu); }
#else
#include <pthread.h>
static pthread_mutex_t g_sys_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread char g_sys_tl;
static void sys_mu_lock(void) { pthread_mutex_lock(&g_sys_mu); }
static void sys_mu_unlock(void) { pthread_mutex_unlock(&g_sys_mu); }
#endif
static char* volatile g_sys_owner;
static int g_sys_depth;
static void sys_lock(void) {
    if (__atomic_load_n(&g_sys_owner, __ATOMIC_ACQUIRE) == &g_sys_tl) { g_sys_depth++; return; }
    sys_mu_lock();
    __atomic_store_n(&g_sys_owner, &g_sys_tl, __ATOMIC_RELEASE);
    g_sys_depth = 1;
}
static void sys_unlock(void) {
    if (--g_sys_depth == 0) {
        __atomic_store_n(&g_sys_owner, (char*)0, __ATOMIC_RELEASE);
        sys_mu_unlock();
    }
}

long long salivo_sys_proc_snap(void) { sys_lock(); long long r = sys_unlocked_proc_snap(); sys_unlock(); return r; }
long long salivo_sys_proc_count(long long h) { sys_lock(); long long r = sys_unlocked_proc_count(h); sys_unlock(); return r; }
long long salivo_sys_proc_int(long long h, long long i, long long field) { sys_lock(); long long r = sys_unlocked_proc_int(h, i, field); sys_unlock(); return r; }
char* salivo_sys_proc_text(long long h, long long i, long long field) { sys_lock(); char* r = sys_unlocked_proc_text(h, i, field); sys_unlock(); return r; }
long long salivo_sys_proc_free(long long h) { sys_lock(); long long r = sys_unlocked_proc_free(h); sys_unlock(); return r; }
long long salivo_sys_pdh_open(void) { sys_lock(); long long r = sys_unlocked_pdh_open(); sys_unlock(); return r; }
long long salivo_sys_pdh_add(long long q, const char* path) { sys_lock(); long long r = sys_unlocked_pdh_add(q, path); sys_unlock(); return r; }
long long salivo_sys_pdh_collect(long long q) { sys_lock(); long long r = sys_unlocked_pdh_collect(q); sys_unlock(); return r; }
long long salivo_sys_pdh_count(long long c) { sys_lock(); long long r = sys_unlocked_pdh_count(c); sys_unlock(); return r; }
char* salivo_sys_pdh_name(long long c, long long i) { sys_lock(); char* r = sys_unlocked_pdh_name(c, i); sys_unlock(); return r; }
long long salivo_sys_pdh_value(long long c, long long i) { sys_lock(); long long r = sys_unlocked_pdh_value(c, i); sys_unlock(); return r; }
long long salivo_sys_pdh_close(long long q) { sys_lock(); long long r = sys_unlocked_pdh_close(q); sys_unlock(); return r; }
long long salivo_sys_icon_png(const char* exe, const char* out_path) { sys_lock(); long long r = sys_unlocked_icon_png(exe, out_path); sys_unlock(); return r; }
long long salivo_sys_svc_snap(void) { sys_lock(); long long r = sys_unlocked_svc_snap(); sys_unlock(); return r; }
long long salivo_sys_svc_count(long long h) { sys_lock(); long long r = sys_unlocked_svc_count(h); sys_unlock(); return r; }
long long salivo_sys_svc_int(long long h, long long i, long long field) { sys_lock(); long long r = sys_unlocked_svc_int(h, i, field); sys_unlock(); return r; }
char* salivo_sys_svc_text(long long h, long long i, long long field) { sys_lock(); char* r = sys_unlocked_svc_text(h, i, field); sys_unlock(); return r; }
long long salivo_sys_svc_free(long long h) { sys_lock(); long long r = sys_unlocked_svc_free(h); sys_unlock(); return r; }
char* salivo_sys_info(const char* key) { sys_lock(); char* r = sys_unlocked_info(key); sys_unlock(); return r; }
long long salivo_sys_info_int(const char* key) { sys_lock(); long long r = sys_unlocked_info_int(key); sys_unlock(); return r; }
long long salivo_sys_text_set(long long slot, const char* text) { sys_lock(); long long r = sys_unlocked_text_set(slot, text); sys_unlock(); return r; }
char* salivo_sys_text_get(long long slot) { sys_lock(); char* r = sys_unlocked_text_get(slot); sys_unlock(); return r; }
