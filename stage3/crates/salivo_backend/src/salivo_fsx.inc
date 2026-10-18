/*
 * salivo_fsx.inc - Process and Extended File-System Runtime for Salivo
 * Windows (Win32 W APIs) and Linux (POSIX).
 * Thread-safe, no memory leaks, all return strings allocated via rt_out_str / rt_out_cstr.
 */

#ifndef SALIVO_FSX_INC
#define SALIVO_FSX_INC

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <winioctl.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <sys/inotify.h>
#include <errno.h>
extern char **environ;
#endif

/* Included at the end of salivo_task_runtime.c: rt_out_str / rt_out_cstr come from there. */

/* --- Windows UTF-8 <-> UTF-16 Helpers --- */

#if defined(_WIN32)
static wchar_t* fsx_utf8_to_w(const char *utf8) {
    if (!utf8) return NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (len <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)len * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, len);
    return w;
}

static char* fsx_w_to_utf8(const wchar_t *w, size_t wlen, size_t *out_utf8_len) {
    if (!w) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, w, (int)wlen, NULL, 0, NULL, NULL);
    if (len <= 0) return NULL;
    char *utf8 = (char *)malloc((size_t)len + 1);
    if (utf8) {
        WideCharToMultiByte(CP_UTF8, 0, w, (int)wlen, utf8, len, NULL, NULL);
        utf8[len] = '\0';
        if (out_utf8_len) *out_utf8_len = (size_t)len;
    }
    return utf8;
}

#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK (0xA000000CL)
#endif
#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT (0xA0000003L)
#endif
#ifndef FSCTL_GET_REPARSE_POINT
#define FSCTL_GET_REPARSE_POINT 0x900a8
#endif

typedef struct _FSX_REPARSE_DATA_BUFFER {
    ULONG  ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union {
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            ULONG  Flags;
            WCHAR  PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            WCHAR  PathBuffer[1];
        } MountPointReparseBuffer;
        struct {
            UCHAR  DataBuffer[1];
        } GenericReparseBuffer;
    };
} FSX_REPARSE_DATA_BUFFER;

#endif

/* --- 1. Environment Variables --- */

char* salivo_env_list_str(void) {
#if defined(_WIN32)
    wchar_t *env_block = GetEnvironmentStringsW();
    if (!env_block) return rt_out_cstr("");

    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        FreeEnvironmentStringsW(env_block);
        return rt_out_cstr("");
    }

    const wchar_t *cur = env_block;
    while (*cur) {
        size_t wlen = wcslen(cur);
        /* Skip internal cmd variables like "=C:" */
        if (wlen > 0 && cur[0] != L'=') {
            size_t ulen = 0;
            char *utf8 = fsx_w_to_utf8(cur, wlen, &ulen);
            if (utf8) {
                while (len + ulen + 2 > cap) {
                    cap *= 2;
                    char *nb = (char *)realloc(buf, cap);
                    if (!nb) {
                        free(utf8);
                        free(buf);
                        FreeEnvironmentStringsW(env_block);
                        return rt_out_cstr("");
                    }
                    buf = nb;
                }
                memcpy(buf + len, utf8, ulen);
                len += ulen;
                buf[len++] = '\n';
                free(utf8);
            }
        }
        cur += wlen + 1;
    }
    FreeEnvironmentStringsW(env_block);
    buf[len] = '\0';
    char *res = rt_out_str(buf, len);
    free(buf);
    return res;
#else
    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return rt_out_cstr("");

    for (char **e = environ; *e; e++) {
        size_t elen = strlen(*e);
        while (len + elen + 2 > cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return rt_out_cstr(""); }
            buf = nb;
        }
        memcpy(buf + len, *e, elen);
        len += elen;
        buf[len++] = '\n';
    }
    buf[len] = '\0';
    char *res = rt_out_str(buf, len);
    free(buf);
    return res;
#endif
}

long long salivo_env_set(const char* k, const char* v) {
    if (!k || !v) return -1;
#if defined(_WIN32)
    wchar_t *wk = fsx_utf8_to_w(k);
    wchar_t *wv = fsx_utf8_to_w(v);
    if (!wk || !wv) {
        if (wk) free(wk);
        if (wv) free(wv);
        return -1;
    }
    /* _wputenv_s updates the CRT copy that getenv (rtgetenv) reads and the OS block children inherit */
    int rc = _wputenv_s(wk, wv);
    free(wk);
    free(wv);
    return rc == 0 ? 0 : -1;
#else
    return setenv(k, v, 1) == 0 ? 0 : -1;
#endif
}

long long salivo_env_unset(const char* k) {
    if (!k) return -1;
#if defined(_WIN32)
    wchar_t *wk = fsx_utf8_to_w(k);
    if (!wk) return -1;
    int rc = _wputenv_s(wk, L""); /* an empty value removes the variable */
    free(wk);
    return rc == 0 ? 0 : -1;
#else
    return unsetenv(k) == 0 ? 0 : -1;
#endif
}

/* --- 2. Temporary Directories & Files --- */

char* salivo_temp_dir_str(void) {
#if defined(_WIN32)
    wchar_t wbuf[MAX_PATH + 1];
    DWORD dw = GetTempPathW(MAX_PATH, wbuf);
    if (dw == 0 || dw > MAX_PATH) return rt_out_cstr("");
    size_t ulen = 0;
    char *utf8 = fsx_w_to_utf8(wbuf, (size_t)dw, &ulen);
    if (!utf8) return rt_out_cstr("");
    char *res = rt_out_str(utf8, ulen);
    free(utf8);
    return res;
#else
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = getenv("TMP");
    if (!tmp || !*tmp) tmp = getenv("TEMP");
    if (!tmp || !*tmp) tmp = "/tmp";
    return rt_out_cstr(tmp);
#endif
}

char* salivo_temp_file_str(const char* prefix) {
#if defined(_WIN32)
    wchar_t wtemp_dir[MAX_PATH + 1];
    DWORD dw = GetTempPathW(MAX_PATH, wtemp_dir);
    if (dw == 0 || dw > MAX_PATH) return rt_out_cstr("");

    wchar_t *wprefix = prefix ? fsx_utf8_to_w(prefix) : NULL;
    wchar_t wtemp_file[MAX_PATH + 1];
    UINT u = GetTempFileNameW(wtemp_dir, wprefix ? wprefix : L"sal", 0, wtemp_file);
    if (wprefix) free(wprefix);
    if (u == 0) return rt_out_cstr("");

    size_t ulen = 0;
    char *utf8 = fsx_w_to_utf8(wtemp_file, wcslen(wtemp_file), &ulen);
    if (!utf8) return rt_out_cstr("");
    char *res = rt_out_str(utf8, ulen);
    free(utf8);
    return res;
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    const char *p = (prefix && *prefix) ? prefix : "sal";
    char tmpl[1024];
    snprintf(tmpl, sizeof(tmpl), "%s/%s_XXXXXX", dir, p);
    int fd = mkstemp(tmpl);
    if (fd < 0) return rt_out_cstr("");
    close(fd);
    return rt_out_cstr(tmpl);
#endif
}

/* Creates a new empty directory with a unique name in the temp dir (owner-only on Linux); its path or "" */
char* salivo_temp_mkdir_str(const char* prefix) {
    const char *p = (prefix && *prefix) ? prefix : "sal";
#if defined(_WIN32)
    wchar_t wdir[MAX_PATH + 1];
    DWORD dw = GetTempPathW(MAX_PATH, wdir);
    if (dw == 0 || dw > MAX_PATH) return rt_out_cstr("");
    size_t dlen = 0;
    char *dir = fsx_w_to_utf8(wdir, (size_t)dw, &dlen);
    if (!dir) return rt_out_cstr("");
    char path[MAX_PATH * 4];
    for (int attempt = 0; attempt < 16; attempt++) {
        uint8_t rnd[8];
        if (salivo_crypto_random_bytes((long long)(uintptr_t)rnd, 8) < 0) break;
        int n = snprintf(path, sizeof(path), "%s%s_", dir, p);
        for (int i = 0; i < 8 && n > 0 && (size_t)n + 3 < sizeof(path); i++) n += snprintf(path + n, sizeof(path) - (size_t)n, "%02x", rnd[i]);
        wchar_t *wpath = fsx_utf8_to_w(path);
        if (!wpath) break;
        BOOL ok = CreateDirectoryW(wpath, NULL);
        DWORD err = ok ? 0 : GetLastError();
        free(wpath);
        if (ok) {
            free(dir);
            return rt_out_cstr(path);
        }
        if (err != ERROR_ALREADY_EXISTS) break;
    }
    free(dir);
    return rt_out_cstr("");
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    char tmpl[1024];
    int n = snprintf(tmpl, sizeof(tmpl), "%s/%s_XXXXXX", dir, p);
    if (n < 0 || (size_t)n >= sizeof(tmpl)) return rt_out_cstr("");
    if (!mkdtemp(tmpl)) return rt_out_cstr("");
    return rt_out_cstr(tmpl);
#endif
}

/* --- 3. File Operations: Rename, Chmod, Symlink --- */

long long salivo_fs_rename(const char* from, const char* to) {
    if (!from || !to) return -1;
#if defined(_WIN32)
    wchar_t *wfrom = fsx_utf8_to_w(from);
    wchar_t *wto = fsx_utf8_to_w(to);
    if (!wfrom || !wto) {
        if (wfrom) free(wfrom);
        if (wto) free(wto);
        return -1;
    }
    BOOL ok = MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    free(wfrom);
    free(wto);
    return ok ? 0 : -1;
#else
    return rename(from, to) == 0 ? 0 : -1;
#endif
}

long long salivo_fs_chmod(const char* path, long long mode) {
    if (!path) return -1;
#if defined(_WIN32)
    wchar_t *wpath = fsx_utf8_to_w(path);
    if (!wpath) return -1;
    DWORD attr = GetFileAttributesW(wpath);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        free(wpath);
        return -1;
    }
    /* POSIX mode: owner-write bit is 0200 */
    if ((mode & 0200) == 0) {
        attr |= FILE_ATTRIBUTE_READONLY;
    } else {
        attr &= ~FILE_ATTRIBUTE_READONLY;
    }
    BOOL ok = SetFileAttributesW(wpath, attr);
    free(wpath);
    return ok ? 0 : -1;
#else
    return chmod(path, (mode_t)mode) == 0 ? 0 : -1;
#endif
}

long long salivo_fs_symlink(const char* target, const char* link) {
    if (!target || !link) return -1;
#if defined(_WIN32)
    wchar_t *wtarget = fsx_utf8_to_w(target);
    wchar_t *wlink = fsx_utf8_to_w(link);
    if (!wtarget || !wlink) {
        if (wtarget) free(wtarget);
        if (wlink) free(wlink);
        return -1;
    }
    DWORD flags = 0;
    DWORD target_attr = GetFileAttributesW(wtarget);
    if (target_attr != INVALID_FILE_ATTRIBUTES && (target_attr & FILE_ATTRIBUTE_DIRECTORY)) {
        flags |= 0x1; /* SYMBOLIC_LINK_FLAG_DIRECTORY */
    }
    /* SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE (Windows 10 Creators Update+) */
    flags |= 0x2;

    BOOLEAN ok = CreateSymbolicLinkW(wlink, wtarget, flags);
    if (!ok && (flags & 0x2)) {
        /* Retry without flag 0x2 on older Windows versions */
        ok = CreateSymbolicLinkW(wlink, wtarget, flags & ~0x2);
    }
    free(wtarget);
    free(wlink);
    return ok ? 0 : -1;
#else
    return symlink(target, link) == 0 ? 0 : -1;
#endif
}

char* salivo_fs_readlink_str(const char* path) {
    if (!path) return rt_out_cstr("");
#if defined(_WIN32)
    wchar_t *wpath = fsx_utf8_to_w(path);
    if (!wpath) return rt_out_cstr("");

    HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) return rt_out_cstr("");

    BYTE buf[16384];
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, buf, sizeof(buf), &bytes, NULL);
    CloseHandle(h);
    if (!ok || bytes < 8) return rt_out_cstr("");

    FSX_REPARSE_DATA_BUFFER *rdb = (FSX_REPARSE_DATA_BUFFER *)buf;
    if (rdb->ReparseTag == IO_REPARSE_TAG_SYMLINK) {
        wchar_t *name = NULL;
        size_t nlen = 0;
        if (rdb->SymbolicLinkReparseBuffer.PrintNameLength > 0) {
            name = (wchar_t *)((char *)rdb->SymbolicLinkReparseBuffer.PathBuffer +
                               rdb->SymbolicLinkReparseBuffer.PrintNameOffset);
            nlen = rdb->SymbolicLinkReparseBuffer.PrintNameLength / sizeof(wchar_t);
        } else {
            name = (wchar_t *)((char *)rdb->SymbolicLinkReparseBuffer.PathBuffer +
                               rdb->SymbolicLinkReparseBuffer.SubstituteNameOffset);
            nlen = rdb->SymbolicLinkReparseBuffer.SubstituteNameLength / sizeof(wchar_t);
            if (nlen >= 4 && name[0] == L'\\' && name[1] == L'?' && name[2] == L'?' && name[3] == L'\\') {
                name += 4;
                nlen -= 4;
            }
        }
        size_t ulen = 0;
        char *utf8 = fsx_w_to_utf8(name, nlen, &ulen);
        if (!utf8) return rt_out_cstr("");
        char *res = rt_out_str(utf8, ulen);
        free(utf8);
        return res;
    } else if (rdb->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
        wchar_t *name = NULL;
        size_t nlen = 0;
        if (rdb->MountPointReparseBuffer.PrintNameLength > 0) {
            name = (wchar_t *)((char *)rdb->MountPointReparseBuffer.PathBuffer +
                               rdb->MountPointReparseBuffer.PrintNameOffset);
            nlen = rdb->MountPointReparseBuffer.PrintNameLength / sizeof(wchar_t);
        } else {
            name = (wchar_t *)((char *)rdb->MountPointReparseBuffer.PathBuffer +
                               rdb->MountPointReparseBuffer.SubstituteNameOffset);
            nlen = rdb->MountPointReparseBuffer.SubstituteNameLength / sizeof(wchar_t);
            if (nlen >= 4 && name[0] == L'\\' && name[1] == L'?' && name[2] == L'?' && name[3] == L'\\') {
                name += 4;
                nlen -= 4;
            }
        }
        size_t ulen = 0;
        char *utf8 = fsx_w_to_utf8(name, nlen, &ulen);
        if (!utf8) return rt_out_cstr("");
        char *res = rt_out_str(utf8, ulen);
        free(utf8);
        return res;
    }
    return rt_out_cstr("");
#else
    char buf[4096];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n < 0) return rt_out_cstr("");
    buf[n] = '\0';
    return rt_out_str(buf, (size_t)n);
#endif
}

long long salivo_fs_is_symlink(const char* path) {
    if (!path) return 0;
#if defined(_WIN32)
    wchar_t *wpath = fsx_utf8_to_w(path);
    if (!wpath) return 0;
    DWORD attr = GetFileAttributesW(wpath);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        free(wpath);
        return 0;
    }
    HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
        return 1;
    }
    BYTE buf[16384];
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, buf, sizeof(buf), &bytes, NULL);
    CloseHandle(h);
    if (!ok || bytes < 8) return 0;
    FSX_REPARSE_DATA_BUFFER *rdb = (FSX_REPARSE_DATA_BUFFER *)buf;
    if (rdb->ReparseTag == IO_REPARSE_TAG_SYMLINK || rdb->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
        return 1;
    }
    return 0;
#else
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    return S_ISLNK(st.st_mode) ? 1 : 0;
#endif
}

/* --- 4. File Watching (up to 64 watches, lock-guarded; names relative to the watched dir, not recursive) --- */

#define FSX_MAX_WATCHES 64

#if defined(_WIN32)
typedef struct {
    int active;
    HANDLE hDir;
    OVERLAPPED ov;
    DWORD buffer[2048]; /* FILE_NOTIFY_INFORMATION records, DWORD aligned */
    DWORD bytes_returned;
    DWORD cur_offset;
    int pending;
} fsx_watch_t;

static fsx_watch_t fsx_watches[FSX_MAX_WATCHES];
static SRWLOCK fsx_watch_lock = SRWLOCK_INIT;

/* Starts the next ReadDirectoryChangesW on w (lock held) */
static void fsx_watch_arm(fsx_watch_t *w) {
    w->bytes_returned = 0;
    w->cur_offset = 0;
    ResetEvent(w->ov.hEvent);
    DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                   FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                   FILE_NOTIFY_CHANGE_LAST_WRITE;
    w->pending = ReadDirectoryChangesW(w->hDir, w->buffer, sizeof(w->buffer), FALSE, filter, NULL, &w->ov, NULL) ? 1 : 0;
}

/* Name of the record at w->cur_offset as a program string; advances, re-arming after the last record (lock held) */
static char* fsx_watch_take(fsx_watch_t *w) {
    FILE_NOTIFY_INFORMATION *fni = (FILE_NOTIFY_INFORMATION *)((uint8_t *)w->buffer + w->cur_offset);
    size_t ulen = 0;
    char *utf8 = fsx_w_to_utf8(fni->FileName, fni->FileNameLength / sizeof(wchar_t), &ulen);
    if (fni->NextEntryOffset != 0) w->cur_offset += fni->NextEntryOffset;
    else fsx_watch_arm(w);
    if (!utf8) return rt_out_cstr("");
    char *res = rt_out_str(utf8, ulen);
    free(utf8);
    return res;
}
#else
typedef struct {
    int active;
    int inotify_fd;
    int wd;
    int off;
    int n;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
} fsx_watch_t;

static fsx_watch_t fsx_watches[FSX_MAX_WATCHES];
static pthread_mutex_t fsx_watch_mtx = PTHREAD_MUTEX_INITIALIZER;
#endif

long long salivo_watch_open(const char* dir) {
    if (!dir) return -1;
#if defined(_WIN32)
    wchar_t *wdir = fsx_utf8_to_w(dir);
    if (!wdir) return -1;
    HANDLE h = CreateFileW(wdir, FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    free(wdir);
    if (h == INVALID_HANDLE_VALUE) return -1;
    HANDLE hEv = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!hEv) {
        CloseHandle(h);
        return -1;
    }

    AcquireSRWLockExclusive(&fsx_watch_lock);
    int slot = -1;
    for (int i = 0; i < FSX_MAX_WATCHES; i++) {
        if (!fsx_watches[i].active) { slot = i; break; }
    }
    if (slot >= 0) {
        fsx_watch_t *w = &fsx_watches[slot];
        memset(w, 0, sizeof(*w));
        w->hDir = h;
        w->ov.hEvent = hEv;
        fsx_watch_arm(w);
        if (w->pending) w->active = 1;
        else slot = -1;
    }
    ReleaseSRWLockExclusive(&fsx_watch_lock);
    if (slot < 0) {
        CloseHandle(hEv);
        CloseHandle(h);
        return -1;
    }
    return (long long)(slot + 1);
#else
    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) return -1;
    int wd = inotify_add_watch(ifd, dir, IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO);
    if (wd < 0) {
        close(ifd);
        return -1;
    }
    pthread_mutex_lock(&fsx_watch_mtx);
    int slot = -1;
    for (int i = 0; i < FSX_MAX_WATCHES; i++) {
        if (!fsx_watches[i].active) { slot = i; break; }
    }
    if (slot >= 0) {
        fsx_watches[slot].active = 1;
        fsx_watches[slot].inotify_fd = ifd;
        fsx_watches[slot].wd = wd;
        fsx_watches[slot].off = 0;
        fsx_watches[slot].n = 0;
    }
    pthread_mutex_unlock(&fsx_watch_mtx);
    if (slot < 0) {
        close(ifd);
        return -1;
    }
    return (long long)(slot + 1);
#endif
}

/* Next changed name, or "" when nothing changed within timeout_ms (< 0 waits forever) */
char* salivo_watch_next_str(long long h, long long timeout_ms) {
    if (h < 1 || h > FSX_MAX_WATCHES) return rt_out_cstr("");
    int slot = (int)(h - 1);
#if defined(_WIN32)
    fsx_watch_t *w = &fsx_watches[slot];
    AcquireSRWLockExclusive(&fsx_watch_lock);
    if (!w->active) {
        ReleaseSRWLockExclusive(&fsx_watch_lock);
        return rt_out_cstr("");
    }
    if (w->cur_offset < w->bytes_returned) {
        char *res = fsx_watch_take(w);
        ReleaseSRWLockExclusive(&fsx_watch_lock);
        return res;
    }
    if (!w->pending) fsx_watch_arm(w);
    HANDLE hEv = w->ov.hEvent;
    ReleaseSRWLockExclusive(&fsx_watch_lock);

    DWORD ms = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    if (WaitForSingleObject(hEv, ms) != WAIT_OBJECT_0) return rt_out_cstr("");

    AcquireSRWLockExclusive(&fsx_watch_lock);
    char *res = NULL;
    DWORD bytes = 0;
    if (w->active && w->ov.hEvent == hEv && w->pending) {
        if (GetOverlappedResult(w->hDir, &w->ov, &bytes, FALSE) && bytes >= sizeof(FILE_NOTIFY_INFORMATION)) {
            w->pending = 0;
            w->bytes_returned = bytes;
            w->cur_offset = 0;
            res = fsx_watch_take(w);
        } else {
            fsx_watch_arm(w); /* buffer overflow (0 bytes) or error: start over */
        }
    } else if (w->active && w->ov.hEvent == hEv && w->cur_offset < w->bytes_returned) {
        res = fsx_watch_take(w); /* another caller collected this batch */
    }
    ReleaseSRWLockExclusive(&fsx_watch_lock);
    return res ? res : rt_out_cstr("");
#else
    fsx_watch_t *w = &fsx_watches[slot];
    for (;;) {
        pthread_mutex_lock(&fsx_watch_mtx);
        if (!w->active) {
            pthread_mutex_unlock(&fsx_watch_mtx);
            return rt_out_cstr("");
        }
        while (w->off < w->n) {
            struct inotify_event *ev = (struct inotify_event *)(w->buf + w->off);
            w->off += (int)(sizeof(struct inotify_event) + ev->len);
            if (ev->len > 0 && ev->name[0] != '\0') {
                char *res = rt_out_cstr(ev->name);
                pthread_mutex_unlock(&fsx_watch_mtx);
                return res;
            }
        }
        int ifd = w->inotify_fd;
        pthread_mutex_unlock(&fsx_watch_mtx);

        struct pollfd pfd;
        pfd.fd = ifd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, timeout_ms < 0 ? -1 : (int)timeout_ms) <= 0) return rt_out_cstr("");

        pthread_mutex_lock(&fsx_watch_mtx);
        ssize_t n = (w->active && w->inotify_fd == ifd && w->off >= w->n) ? read(ifd, w->buf, sizeof(w->buf)) : 0;
        if (n > 0) {
            w->off = 0;
            w->n = (int)n;
        }
        pthread_mutex_unlock(&fsx_watch_mtx);
        if (n < 0 && errno != EAGAIN) return rt_out_cstr("");
    }
#endif
}

long long salivo_watch_close(long long h) {
    if (h < 1 || h > FSX_MAX_WATCHES) return -1;
    int slot = (int)(h - 1);
#if defined(_WIN32)
    fsx_watch_t *w = &fsx_watches[slot];
    AcquireSRWLockExclusive(&fsx_watch_lock);
    if (!w->active) {
        ReleaseSRWLockExclusive(&fsx_watch_lock);
        return -1;
    }
    if (w->pending) {
        DWORD bytes = 0;
        CancelIo(w->hDir);
        GetOverlappedResult(w->hDir, &w->ov, &bytes, TRUE); /* the kernel no longer writes into buffer */
    }
    /* Like closing a file: the caller must not close a watch another thread is waiting on */
    CloseHandle(w->hDir);
    CloseHandle(w->ov.hEvent);
    memset(w, 0, sizeof(*w));
    ReleaseSRWLockExclusive(&fsx_watch_lock);
    return 0;
#else
    pthread_mutex_lock(&fsx_watch_mtx);
    if (!fsx_watches[slot].active) {
        pthread_mutex_unlock(&fsx_watch_mtx);
        return -1;
    }
    close(fsx_watches[slot].inotify_fd);
    memset(&fsx_watches[slot], 0, sizeof(fsx_watch_t));
    pthread_mutex_unlock(&fsx_watch_mtx);
    return 0;
#endif
}

/* --- 5. Memory-Mapped Files (up to 256 mappings, mutex-guarded) --- */

#define FSX_MAX_MMAPS 256

#if defined(_WIN32)
typedef struct {
    int active;
    HANDLE hFile;
    HANDLE hMap;
    void *addr;
    size_t len;
} fsx_mmap_t;

static fsx_mmap_t fsx_mmaps[FSX_MAX_MMAPS];
static SRWLOCK fsx_mmap_lock = SRWLOCK_INIT;
#else
typedef struct {
    int active;
    int fd;
    void *addr;
    size_t len;
} fsx_mmap_t;

static fsx_mmap_t fsx_mmaps[FSX_MAX_MMAPS];
static pthread_mutex_t fsx_mmap_mtx = PTHREAD_MUTEX_INITIALIZER;
#endif

long long salivo_mmap_open(const char* path, long long writable) {
    if (!path) return -1;

#if defined(_WIN32)
    AcquireSRWLockExclusive(&fsx_mmap_lock);

    int slot = -1;
    for (int i = 0; i < FSX_MAX_MMAPS; i++) {
        if (!fsx_mmaps[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        ReleaseSRWLockExclusive(&fsx_mmap_lock);
        return -1;
    }

    wchar_t *wpath = fsx_utf8_to_w(path);
    if (!wpath) {
        ReleaseSRWLockExclusive(&fsx_mmap_lock);
        return -1;
    }

    DWORD access = GENERIC_READ | (writable ? GENERIC_WRITE : 0);
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE hFile = CreateFileW(wpath, access, share, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wpath);
    if (hFile == INVALID_HANDLE_VALUE) {
        ReleaseSRWLockExclusive(&fsx_mmap_lock);
        return -1;
    }

    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li) || li.QuadPart <= 0) {
        CloseHandle(hFile);
        ReleaseSRWLockExclusive(&fsx_mmap_lock);
        return -1;
    }

    DWORD page_prot = writable ? PAGE_READWRITE : PAGE_READONLY;
    HANDLE hMap = CreateFileMappingW(hFile, NULL, page_prot, 0, 0, NULL);
    if (!hMap) {
        CloseHandle(hFile);
        ReleaseSRWLockExclusive(&fsx_mmap_lock);
        return -1;
    }

    DWORD map_access = writable ? FILE_MAP_WRITE : FILE_MAP_READ;
    void *addr = MapViewOfFile(hMap, map_access, 0, 0, 0);
    if (!addr) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        ReleaseSRWLockExclusive(&fsx_mmap_lock);
        return -1;
    }

    fsx_mmaps[slot].active = 1;
    fsx_mmaps[slot].hFile = hFile;
    fsx_mmaps[slot].hMap = hMap;
    fsx_mmaps[slot].addr = addr;
    fsx_mmaps[slot].len = (size_t)li.QuadPart;

    ReleaseSRWLockExclusive(&fsx_mmap_lock);
    return (long long)(slot + 1);
#else
    pthread_mutex_lock(&fsx_mmap_mtx);
    int slot = -1;
    for (int i = 0; i < FSX_MAX_MMAPS; i++) {
        if (!fsx_mmaps[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&fsx_mmap_mtx);
        return -1;
    }

    int flags = writable ? O_RDWR : O_RDONLY;
    int fd = open(path, flags);
    if (fd < 0) {
        pthread_mutex_unlock(&fsx_mmap_mtx);
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        pthread_mutex_unlock(&fsx_mmap_mtx);
        return -1;
    }

    int prot = PROT_READ | (writable ? PROT_WRITE : 0);
    void *addr = mmap(NULL, (size_t)st.st_size, prot, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        pthread_mutex_unlock(&fsx_mmap_mtx);
        return -1;
    }

    fsx_mmaps[slot].active = 1;
    fsx_mmaps[slot].fd = fd;
    fsx_mmaps[slot].addr = addr;
    fsx_mmaps[slot].len = (size_t)st.st_size;

    pthread_mutex_unlock(&fsx_mmap_mtx);
    return (long long)(slot + 1);
#endif
}

long long salivo_mmap_addr(long long h) {
    if (h < 1 || h > FSX_MAX_MMAPS) return 0;
    int slot = (int)(h - 1);
#if defined(_WIN32)
    AcquireSRWLockExclusive(&fsx_mmap_lock);
    long long addr = fsx_mmaps[slot].active ? (long long)(uintptr_t)fsx_mmaps[slot].addr : 0;
    ReleaseSRWLockExclusive(&fsx_mmap_lock);
    return addr;
#else
    pthread_mutex_lock(&fsx_mmap_mtx);
    long long addr = fsx_mmaps[slot].active ? (long long)(uintptr_t)fsx_mmaps[slot].addr : 0;
    pthread_mutex_unlock(&fsx_mmap_mtx);
    return addr;
#endif
}

long long salivo_mmap_len(long long h) {
    if (h < 1 || h > FSX_MAX_MMAPS) return 0;
    int slot = (int)(h - 1);
#if defined(_WIN32)
    AcquireSRWLockExclusive(&fsx_mmap_lock);
    long long len = fsx_mmaps[slot].active ? (long long)fsx_mmaps[slot].len : 0;
    ReleaseSRWLockExclusive(&fsx_mmap_lock);
    return len;
#else
    pthread_mutex_lock(&fsx_mmap_mtx);
    long long len = fsx_mmaps[slot].active ? (long long)fsx_mmaps[slot].len : 0;
    pthread_mutex_unlock(&fsx_mmap_mtx);
    return len;
#endif
}

long long salivo_mmap_close(long long h) {
    if (h < 1 || h > FSX_MAX_MMAPS) return -1;
    int slot = (int)(h - 1);
#if defined(_WIN32)
    AcquireSRWLockExclusive(&fsx_mmap_lock);
    if (!fsx_mmaps[slot].active) {
        ReleaseSRWLockExclusive(&fsx_mmap_lock);
        return -1;
    }
    if (fsx_mmaps[slot].addr) {
        FlushViewOfFile(fsx_mmaps[slot].addr, 0);
        UnmapViewOfFile(fsx_mmaps[slot].addr);
    }
    if (fsx_mmaps[slot].hMap) CloseHandle(fsx_mmaps[slot].hMap);
    if (fsx_mmaps[slot].hFile != INVALID_HANDLE_VALUE) CloseHandle(fsx_mmaps[slot].hFile);
    memset(&fsx_mmaps[slot], 0, sizeof(fsx_mmap_t));
    ReleaseSRWLockExclusive(&fsx_mmap_lock);
    return 0;
#else
    pthread_mutex_lock(&fsx_mmap_mtx);
    if (!fsx_mmaps[slot].active) {
        pthread_mutex_unlock(&fsx_mmap_mtx);
        return -1;
    }
    if (fsx_mmaps[slot].addr && fsx_mmaps[slot].addr != MAP_FAILED) {
        msync(fsx_mmaps[slot].addr, fsx_mmaps[slot].len, MS_SYNC);
        munmap(fsx_mmaps[slot].addr, fsx_mmaps[slot].len);
    }
    if (fsx_mmaps[slot].fd >= 0) close(fsx_mmaps[slot].fd);
    memset(&fsx_mmaps[slot], 0, sizeof(fsx_mmap_t));
    pthread_mutex_unlock(&fsx_mmap_mtx);
    return 0;
#endif
}

/* --- 6. Child processes with streaming pipes (up to 64, lock-guarded) ---
 * The command runs through the system shell (cmd.exe /d /s /c, or /bin/sh -c) with its stdin,
 * stdout and stderr on pipes. Reads return what has arrived (waiting up to timeout_ms for the first
 * bytes); "" means nothing came in time or the stream ended (salivo_child_eof tells them apart). */

#define FSX_MAX_CHILDREN 64

#if defined(_WIN32)
typedef struct {
    int active;
    HANDLE proc;
    HANDLE in_w;    /* our end of the child's stdin, NULL once closed */
    HANDLE out_r[2]; /* stdout, stderr */
    int eof[2];
} fsx_child_t;
static SRWLOCK fsx_child_lock = SRWLOCK_INIT;
#else
#include <signal.h>
#include <sys/wait.h>
typedef struct {
    int active;
    pid_t pid;
    int in_w;
    int out_r[2];
    int eof[2];
    int reaped;
    long long code;
} fsx_child_t;
static pthread_mutex_t fsx_child_mtx = PTHREAD_MUTEX_INITIALIZER;
#endif
static fsx_child_t fsx_children[FSX_MAX_CHILDREN];

static fsx_child_t* fsx_child_get(long long h) {
    return (h >= 1 && h <= FSX_MAX_CHILDREN && fsx_children[h - 1].active) ? &fsx_children[h - 1] : NULL;
}

#if defined(_WIN32)
#define FSX_CHILD_LOCK() AcquireSRWLockExclusive(&fsx_child_lock)
#define FSX_CHILD_UNLOCK() ReleaseSRWLockExclusive(&fsx_child_lock)
#else
#define FSX_CHILD_LOCK() pthread_mutex_lock(&fsx_child_mtx)
#define FSX_CHILD_UNLOCK() pthread_mutex_unlock(&fsx_child_mtx)
#endif

/* Starts cmd; a handle (> 0) or -1 */
long long salivo_child_open(const char* cmd) {
    if (!cmd || !*cmd) return -1;
    fsx_child_t c;
    memset(&c, 0, sizeof c);
#if defined(_WIN32)
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL, err_r = NULL, err_w = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0)) goto win_fail;
    /* our ends stay in this process */
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    {
        int wl = MultiByteToWideChar(CP_UTF8, 0, cmd, -1, NULL, 0);
        wchar_t *wcmd = wl > 0 ? (wchar_t *)malloc(((size_t)wl + 32) * sizeof(wchar_t)) : NULL;
        if (!wcmd) goto win_fail;
        wcscpy(wcmd, L"cmd.exe /d /s /c \"");
        MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd + wcslen(wcmd), wl);
        wcscat(wcmd, L"\"");
        /* inherit exactly the child's three pipe ends, so a child started at the same time by another
           thread cannot keep our pipes open (which would hide end of stream) */
        HANDLE inherit[3] = { in_r, out_w, err_w };
        SIZE_T attr_size = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
        LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
        BOOL ok = attrs && InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
        if (ok) ok = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL, NULL);
        STARTUPINFOEXW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si);
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = in_r;
        si.StartupInfo.hStdOutput = out_w;
        si.StartupInfo.hStdError = err_w;
        si.lpAttributeList = attrs;
        if (ok) ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi);
        if (attrs) {
            DeleteProcThreadAttributeList(attrs);
            free(attrs);
        }
        free(wcmd);
        if (!ok) goto win_fail;
        CloseHandle(pi.hThread);
        c.proc = pi.hProcess;
    }
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(err_w);
    c.in_w = in_w;
    c.out_r[0] = out_r;
    c.out_r[1] = err_r;
    goto have_child;
win_fail:
    if (in_r) CloseHandle(in_r);
    if (in_w) CloseHandle(in_w);
    if (out_r) CloseHandle(out_r);
    if (out_w) CloseHandle(out_w);
    if (err_r) CloseHandle(err_r);
    if (err_w) CloseHandle(err_w);
    return -1;
#else
    int pin[2] = { -1, -1 }, pout[2] = { -1, -1 }, perr[2] = { -1, -1 };
    if (pipe2(pin, O_CLOEXEC) || pipe2(pout, O_CLOEXEC) || pipe2(perr, O_CLOEXEC)) goto posix_fail;
    pid_t pid = fork();
    if (pid < 0) goto posix_fail;
    if (pid == 0) {
        /* dup2 clears close-on-exec on 0, 1 and 2; every other pipe end closes at exec */
        dup2(pin[0], 0);
        dup2(pout[1], 1);
        dup2(perr[1], 2);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pin[0]);
    close(pout[1]);
    close(perr[1]);
    /* writing to a child that exited must fail with EPIPE, not kill this process */
    signal(SIGPIPE, SIG_IGN);
    c.pid = pid;
    c.in_w = pin[1];
    c.out_r[0] = pout[0];
    c.out_r[1] = perr[0];
    goto have_child;
posix_fail:
    for (int i = 0; i < 2; i++) {
        if (pin[i] >= 0) close(pin[i]);
        if (pout[i] >= 0) close(pout[i]);
        if (perr[i] >= 0) close(perr[i]);
    }
    return -1;
#endif
have_child:
    c.active = 1;
    FSX_CHILD_LOCK();
    int slot = -1;
    for (int i = 0; i < FSX_MAX_CHILDREN; i++) {
        if (!fsx_children[i].active) { slot = i; fsx_children[i] = c; break; }
    }
    FSX_CHILD_UNLOCK();
    if (slot < 0) {
        /* table full: stop the child and release everything */
#if defined(_WIN32)
        TerminateProcess(c.proc, 1);
        CloseHandle(c.proc);
        CloseHandle(c.in_w);
        CloseHandle(c.out_r[0]);
        CloseHandle(c.out_r[1]);
#else
        kill(c.pid, SIGKILL);
        waitpid(c.pid, NULL, 0);
        close(c.in_w);
        close(c.out_r[0]);
        close(c.out_r[1]);
#endif
        return -1;
    }
    return (long long)(slot + 1);
}

/* Writes text to the child's stdin; bytes written, or -1 (closed input, or the child is gone) */
long long salivo_child_write(long long h, const char* s) {
    if (!s) return -1;
    size_t n = strlen(s);
    FSX_CHILD_LOCK();
    fsx_child_t *c = fsx_child_get(h);
#if defined(_WIN32)
    HANDLE w = c ? c->in_w : NULL;
    FSX_CHILD_UNLOCK();
    if (!w) return -1;
    size_t done = 0;
    while (done < n) {
        DWORD k = 0;
        DWORD chunk = (DWORD)((n - done) > 0x40000000u ? 0x40000000u : (n - done));
        if (!WriteFile(w, s + done, chunk, &k, NULL)) return done ? (long long)done : -1;
        done += k;
    }
#else
    int w = c ? c->in_w : -1;
    FSX_CHILD_UNLOCK();
    if (w < 0) return -1;
    size_t done = 0;
    while (done < n) {
        ssize_t k = write(w, s + done, n - done);
        if (k < 0) {
            if (errno == EINTR) continue;
            return done ? (long long)done : -1;
        }
        done += (size_t)k;
    }
#endif
    return (long long)n;
}

/* Closes the child's stdin (it then sees end of input); 0 or -1 */
long long salivo_child_close_input(long long h) {
    FSX_CHILD_LOCK();
    fsx_child_t *c = fsx_child_get(h);
    long long r = -1;
#if defined(_WIN32)
    if (c && c->in_w) { CloseHandle(c->in_w); c->in_w = NULL; r = 0; }
#else
    if (c && c->in_w >= 0) { close(c->in_w); c->in_w = -1; r = 0; }
#endif
    FSX_CHILD_UNLOCK();
    return r;
}

/* Output of stream (0 stdout, 1 stderr) that has arrived, waiting up to timeout_ms (< 0 forever) for
   the first bytes; "" on timeout or end of stream */
char* salivo_child_read_str(long long h, long long stream, long long timeout_ms) {
    if (stream != 0 && stream != 1) return rt_out_cstr("");
    FSX_CHILD_LOCK();
    fsx_child_t *c = fsx_child_get(h);
    if (!c || c->eof[stream]) {
        FSX_CHILD_UNLOCK();
        return rt_out_cstr("");
    }
#if defined(_WIN32)
    HANDLE r = c->out_r[stream];
    FSX_CHILD_UNLOCK();
    /* ponytail: anonymous pipes have no overlapped I/O, so waiting polls every 5 ms; a named-pipe
       pair with overlapped reads would remove the latency floor if it ever matters */
    ULONGLONG start = GetTickCount64();
    DWORD avail = 0;
    for (;;) {
        if (!PeekNamedPipe(r, NULL, 0, NULL, &avail, NULL)) {
            FSX_CHILD_LOCK();
            if (fsx_child_get(h) == c) c->eof[stream] = 1; /* broken pipe: the child closed it */
            FSX_CHILD_UNLOCK();
            return rt_out_cstr("");
        }
        if (avail > 0) break;
        if (timeout_ms >= 0 && GetTickCount64() - start >= (ULONGLONG)timeout_ms) return rt_out_cstr("");
        Sleep(5);
    }
    if (avail > 65536) avail = 65536;
    char buf[65536];
    DWORD got = 0;
    if (!ReadFile(r, buf, avail, &got, NULL)) got = 0;
    return rt_out_str(buf, got);
#else
    int fd = c->out_r[stream];
    FSX_CHILD_UNLOCK();
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr;
    while ((pr = poll(&pfd, 1, timeout_ms < 0 ? -1 : (int)timeout_ms)) < 0 && errno == EINTR) {}
    if (pr <= 0) return rt_out_cstr("");
    char buf[65536];
    ssize_t got;
    while ((got = read(fd, buf, sizeof buf)) < 0 && errno == EINTR) {}
    if (got <= 0) {
        FSX_CHILD_LOCK();
        if (fsx_child_get(h) == c) c->eof[stream] = 1;
        FSX_CHILD_UNLOCK();
        return rt_out_cstr("");
    }
    return rt_out_str(buf, (size_t)got);
#endif
}

/* 1 once the stream (0 stdout, 1 stderr) has ended, else 0 */
long long salivo_child_eof(long long h, long long stream) {
    if (stream != 0 && stream != 1) return 1;
    FSX_CHILD_LOCK();
    fsx_child_t *c = fsx_child_get(h);
    long long r = c ? c->eof[stream] : 1;
    FSX_CHILD_UNLOCK();
    return r;
}

/* Waits up to timeout_ms (< 0 forever) for the child to exit: its exit code, or -1 while it still runs */
long long salivo_child_wait(long long h, long long timeout_ms) {
    FSX_CHILD_LOCK();
    fsx_child_t *c = fsx_child_get(h);
    if (!c) {
        FSX_CHILD_UNLOCK();
        return -1;
    }
#if defined(_WIN32)
    HANDLE p = c->proc;
    FSX_CHILD_UNLOCK();
    if (WaitForSingleObject(p, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms) != WAIT_OBJECT_0) return -1;
    DWORD ec = 1;
    GetExitCodeProcess(p, &ec);
    return (long long)ec;
#else
    if (c->reaped) {
        long long code = c->code;
        FSX_CHILD_UNLOCK();
        return code;
    }
    pid_t pid = c->pid;
    FSX_CHILD_UNLOCK();
    long long waited = 0;
    for (;;) {
        int st = 0;
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid || (w < 0 && errno != EINTR)) {
            long long code = w < 0 ? 1 : WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
            FSX_CHILD_LOCK();
            if (fsx_child_get(h) == c && !c->reaped) { c->reaped = 1; c->code = code; }
            if (fsx_child_get(h) == c) code = c->code;
            FSX_CHILD_UNLOCK();
            return code;
        }
        if (timeout_ms >= 0 && waited >= timeout_ms) return -1;
        /* ponytail: polls every 5 ms (no pidfd before Linux 5.3); a pidfd + poll would wake at once */
        usleep(5000);
        waited += 5;
    }
#endif
}

/* Stops the child (TerminateProcess / SIGKILL); 0 or -1 */
long long salivo_child_kill(long long h) {
    FSX_CHILD_LOCK();
    fsx_child_t *c = fsx_child_get(h);
    long long r = -1;
#if defined(_WIN32)
    if (c && TerminateProcess(c->proc, 1)) r = 0;
#else
    if (c && !c->reaped && kill(c->pid, SIGKILL) == 0) r = 0;
#endif
    FSX_CHILD_UNLOCK();
    return r;
}

/* Releases the handle and its pipes; a child still running is killed and reaped first */
long long salivo_child_free(long long h) {
    FSX_CHILD_LOCK();
    fsx_child_t *c = fsx_child_get(h);
    if (!c) {
        FSX_CHILD_UNLOCK();
        return -1;
    }
    fsx_child_t copy = *c;
    memset(c, 0, sizeof *c);
    FSX_CHILD_UNLOCK();
#if defined(_WIN32)
    if (WaitForSingleObject(copy.proc, 0) != WAIT_OBJECT_0) TerminateProcess(copy.proc, 1);
    CloseHandle(copy.proc);
    if (copy.in_w) CloseHandle(copy.in_w);
    CloseHandle(copy.out_r[0]);
    CloseHandle(copy.out_r[1]);
#else
    if (!copy.reaped) {
        if (waitpid(copy.pid, NULL, WNOHANG) == 0) {
            kill(copy.pid, SIGKILL);
            while (waitpid(copy.pid, NULL, 0) < 0 && errno == EINTR) {}
        }
    }
    if (copy.in_w >= 0) close(copy.in_w);
    close(copy.out_r[0]);
    close(copy.out_r[1]);
#endif
    return 0;
}

#endif /* SALIVO_FSX_INC */
