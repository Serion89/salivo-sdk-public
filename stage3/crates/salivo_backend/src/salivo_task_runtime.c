

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#ifndef LLONG_MAX
#define LLONG_MAX 9223372036854775807LL
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <direct.h>
#include <io.h>
#include <intrin.h>
#include <sys/types.h>
#include <sys/stat.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#else
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#endif

// Debug logging: only active when compiled with -DSALIVO_DEBUG
#ifdef SALIVO_DEBUG
#define SALIVO_LOG(...) do { printf(__VA_ARGS__); fflush(stdout); } while(0)
#else
#define SALIVO_LOG(...) do {} while(0)
#endif

#define MAX_TASKS 65536
#define READY_QUEUE_CAP 65536
#define MAX_CHANNELS 256
#define CHANNEL_CAP 256
#define MAX_SCOPES 256
#define MAX_SCOPE_TASKS 16384
#define MAX_TOKENS 256

long long salivo_alloc(long long size_bytes, long long alignment);
void salivo_deallocate(long long addr, long long size_bytes, long long alignment);
void salivo_free_format_temp(void* ptr);
long long salivo_get_format_temp_free_count(void);
long long salivo_checked_add_len(long long a, long long b);
long long salivo_alloc_format_buffer(long long total_len);
long long salivo_get_active_allocations(void);
long long salivo_channel_send_i64(long long channel_handle, long long val);
long long salivo_channel_recv_i64(long long channel_handle);


/* Set by the Stage 36.1 async runtime while it runs: legacy blocking primitives report themselves
 * so a blocking call made from an async worker is detected (see salivo_aio_runtime.c). */
void (*salivo_aio_blocking_hook)(const char* what) = 0;
/* Allocation counters are updated from several async workers at once. This file is also built
 * with MSVC (cl) for the JIT, which lacks the GCC __atomic builtins. */
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define SALIVO_COUNTER_ADD(var, n) _InterlockedExchangeAdd64((volatile __int64*)&(var), (n))
#else
#define SALIVO_COUNTER_ADD(var, n) __atomic_add_fetch(&(var), (n), __ATOMIC_RELAXED)
#endif
#define SALIVO_BLOCKING(what) do { if (salivo_aio_blocking_hook) salivo_aio_blocking_hook(what); } while (0)

void rtpanic(const char* msg) {
    printf("panic: %s\n", msg ? msg : "Assertion failed");
    fflush(NULL);
    /* _Exit, not exit: async worker threads may still be running; CRT teardown would race them */
    _Exit(1);
}

#ifdef _WIN32
static __declspec(thread) long long g_current_executing_task_id = 0;
#else
static __thread long long g_current_executing_task_id = 0;
#endif

// --- FORWARD DECLARATIONS ---
long long salivo_task_enqueue_ready(int task_id);
long long salivo_time_now_ms(void);
void salivo_sleep_ms(long long ms);
long long salivo_thread_spawn(long long fn_ptr, long long arg_ptr);
long long salivo_thread_join(long long thread_handle);
long long salivo_thread_current_id(void);
void salivo_thread_yield(void);
void salivo_thread_sleep_ms(long long ms);


/* Allocation statistics, printed at exit when SALIVO_ALLOC_STATS is set (diagnostics only) */
static long long salivo_stat_bytes[5];
static long long salivo_stat_calls[5];
static const char* salivo_stat_names[5] = {"rtconcat", "rtsubstr", "rtintstr", "salivo_alloc", "rtupper/trim"};
static int salivo_stats_registered = 0;
static void salivo_print_alloc_stats(void) {
    for (int i = 0; i < 5; i++) {
        fprintf(stderr, "ALLOCSTAT %s calls=%lld bytes=%lld\n", salivo_stat_names[i], salivo_stat_calls[i], salivo_stat_bytes[i]);
    }
}
static void salivo_stat(int kind, long long bytes) {
    if (!salivo_stats_registered) {
        salivo_stats_registered = 1;
        if (getenv("SALIVO_ALLOC_STATS")) atexit(salivo_print_alloc_stats);
    }
    salivo_stat_calls[kind]++;
    salivo_stat_bytes[kind] += bytes;
}

/* Frees a heap string produced by rtconcat/rtintstr; the static "" returned on allocation failure is skipped */
void rtfreestr(char* s) {
    if (s && s[0] != 0) free(s);
}

char* rtconcat(const char* a, const char* b) {
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a);
    size_t lb = strlen(b);
    salivo_stat(0, (long long)(la + lb + 1));
    char* res = (char*)malloc(la + lb + 1);
    if (!res) return "";
    memcpy(res, a, la);
    memcpy(res + la, b, lb);
    res[la + lb] = '\0';
    return res;
}

char* salivo_concat_str_len(const char* a, size_t la, const char* b, size_t lb) {
    char* res = (char*)malloc(la + lb + 1);
    if (!res) return "";
    if (la > 0 && a) memcpy(res, a, la);
    if (lb > 0 && b) memcpy(res + la, b, lb);
    res[la + lb] = '\0';
    return res;
}

static long long salivo_unicode_toupper(long long cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    // Latin-1 Supplement
    if (cp >= 0xE0 && cp <= 0xF6) return cp - 32; // à-ö -> À-Ö
    if (cp >= 0xF8 && cp <= 0xFE) return cp - 32; // ø-þ -> Ø-Þ
    if (cp == 0xFF) return 0x0178; // ÿ -> Ÿ
    // Latin Extended-A
    if (cp >= 0x0100 && cp <= 0x0177) {
        if ((cp % 2) == 1) return cp - 1;
        return cp;
    }
    if (cp >= 0x0179 && cp <= 0x017E) {
        if ((cp % 2) == 0) return cp - 1;
        return cp;
    }
    // Greek (α-ω -> Α-Ω)
    if (cp >= 0x03B1 && cp <= 0x03C9) return cp - 32;
    if (cp == 0x03C2) return 0x03A3; // final sigma ς -> Σ
    if (cp == 0x03AC) return 0x0386;
    if (cp == 0x03AD) return 0x0388;
    if (cp == 0x03AE) return 0x0389;
    if (cp == 0x03AF) return 0x038A;
    if (cp == 0x03CC) return 0x038C;
    if (cp == 0x03CD) return 0x038E;
    if (cp == 0x03CE) return 0x038F;
    // Cyrillic (а-я -> А-Я)
    if (cp >= 0x0430 && cp <= 0x044F) return cp - 32;
    if (cp >= 0x0450 && cp <= 0x045F) return cp - 80;
    if (cp == 0x0491) return 0x0490;
    // Fullwidth ASCII (FF41-FF5A -> FF21-FF3A)
    if (cp >= 0xFF41 && cp <= 0xFF5A) return cp - 32;
    return cp;
}

static long long salivo_unicode_tolower(long long cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    // Latin-1 Supplement
    if (cp >= 0xC0 && cp <= 0xD6) return cp + 32; // À-Ö -> à-ö
    if (cp >= 0xD8 && cp <= 0xDE) return cp + 32; // Ø-Þ -> ø-þ
    if (cp == 0x0178) return 0xFF; // Ÿ -> ÿ
    // Latin Extended-A
    if (cp >= 0x0100 && cp <= 0x0177) {
        if ((cp % 2) == 0) return cp + 1;
        return cp;
    }
    if (cp >= 0x0179 && cp <= 0x017E) {
        if ((cp % 2) == 1) return cp + 1;
        return cp;
    }
    // Greek (Α-Ω -> α-ω)
    if (cp >= 0x0391 && cp <= 0x03A9 && cp != 0x03A2) return cp + 32;
    if (cp == 0x0386) return 0x03AC;
    if (cp == 0x0388) return 0x03AD;
    if (cp == 0x0389) return 0x03AE;
    if (cp == 0x038A) return 0x03AF;
    if (cp == 0x038C) return 0x03CC;
    if (cp == 0x038E) return 0x03CD;
    if (cp == 0x038F) return 0x03CE;
    // Cyrillic (А-Я -> а-я)
    if (cp >= 0x0410 && cp <= 0x042F) return cp + 32;
    if (cp >= 0x0400 && cp <= 0x040F) return cp + 80;
    if (cp == 0x0490) return 0x0491;
    // Fullwidth ASCII (FF21-FF3A -> FF41-FF5A)
    if (cp >= 0xFF21 && cp <= 0xFF3A) return cp + 32;
    return cp;
}

static int salivo_utf8_encode_cp(long long cp, char* out) {
    if (cp < 0 || cp > 0x10FFFF) return 0;
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | ((cp >> 6) & 0x1F));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | ((cp >> 12) & 0x0F));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | ((cp >> 18) & 0x07));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

char* rtupper(const char* s) {
    if (!s) return "";
    long long slen = (long long)strlen(s);
    char* res = (char*)malloc((size_t)(slen * 4 + 4));
    if (!res) return "";
    long long offset = 0;
    long long out_len = 0;
    while (offset < slen) {
        long long prev = offset;
        const unsigned char* p = (const unsigned char*)s + offset;
        const unsigned char* end = (const unsigned char*)s + slen;
        unsigned char c = *p;
        long long cp = 0;
        int seq_len = 0;
        if (c <= 0x7F) {
            cp = c;
            seq_len = 1;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (p + 1 < end) {
                cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
                seq_len = 2;
            }
        } else if (c >= 0xE0 && c <= 0xEF) {
            if (p + 2 < end) {
                cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
                seq_len = 3;
            }
        } else if (c >= 0xF0 && c <= 0xF4) {
            if (p + 3 < end) {
                cp = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
                seq_len = 4;
            }
        }
        if (seq_len == 0) {
            res[out_len++] = s[prev];
            offset = prev + 1;
        } else {
            offset += seq_len;
            long long up = salivo_unicode_toupper(cp);
            out_len += salivo_utf8_encode_cp(up, res + out_len);
        }
    }
    res[out_len] = '\0';
    return res;
}

char* rtlower(const char* s) {
    if (!s) return "";
    long long slen = (long long)strlen(s);
    char* res = (char*)malloc((size_t)(slen * 4 + 4));
    if (!res) return "";
    long long offset = 0;
    long long out_len = 0;
    while (offset < slen) {
        long long prev = offset;
        const unsigned char* p = (const unsigned char*)s + offset;
        const unsigned char* end = (const unsigned char*)s + slen;
        unsigned char c = *p;
        long long cp = 0;
        int seq_len = 0;
        if (c <= 0x7F) {
            cp = c;
            seq_len = 1;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (p + 1 < end) {
                cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
                seq_len = 2;
            }
        } else if (c >= 0xE0 && c <= 0xEF) {
            if (p + 2 < end) {
                cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
                seq_len = 3;
            }
        } else if (c >= 0xF0 && c <= 0xF4) {
            if (p + 3 < end) {
                cp = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
                seq_len = 4;
            }
        }
        if (seq_len == 0) {
            res[out_len++] = s[prev];
            offset = prev + 1;
        } else {
            offset += seq_len;
            long long low = salivo_unicode_tolower(cp);
            out_len += salivo_utf8_encode_cp(low, res + out_len);
        }
    }
    res[out_len] = '\0';
    return res;
}

char* rtstrupper(const char* s) {
    return rtupper(s);
}

char* rtstrlower(const char* s) {
    return rtlower(s);
}

long long rtstrlen(const char* s) {
    if (!s) return 0;
    return (long long)strlen(s);
}

char* rtsubstr(const char* s, long long start, long long len) {
    /* Always a fresh heap string: callers may free a result they are done with */
    long long total_len = s ? (long long)strlen(s) : 0;
    if (start < 0) start = 0;
    if (start >= total_len) { char* empty = (char*)malloc(1); if (empty) empty[0] = 0; return empty ? empty : ""; }
    if (len < 0) len = 0;
    if (start + len > total_len) len = total_len - start;
    salivo_stat(1, len + 1);
    char* res = (char*)malloc((size_t)len + 1);
    if (!res) return "";
    memcpy(res, s + start, (size_t)len);
    res[len] = '\0';
    return res;
}

char* rtdebugstr(const char* s) {
    if (!s) {
        char* empty = (char*)(uintptr_t)salivo_alloc(3, 1);
        if (empty) {
            empty[0] = '"';
            empty[1] = '"';
            empty[2] = '\0';
        }
        return empty ? empty : "\"\"";
    }
    size_t len = strlen(s);
    char* res = (char*)(uintptr_t)salivo_alloc(len * 2 + 3, 1);
    if (!res) return "\"\"";
    size_t out = 0;
    res[out++] = '"';
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\n') {
            res[out++] = '\\';
            res[out++] = 'n';
        } else if (c == '\t') {
            res[out++] = '\\';
            res[out++] = 't';
        } else if (c == '\r') {
            res[out++] = '\\';
            res[out++] = 'r';
        } else if (c == '\\') {
            res[out++] = '\\';
            res[out++] = '\\';
        } else if (c == '"') {
            res[out++] = '\\';
            res[out++] = '"';
        } else {
            res[out++] = c;
        }
    }
    res[out++] = '"';
    res[out] = '\0';
    return res;
}

char* rttrim(const char* s) {
    if (!s) return "";
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
        s++;
    }
    if (*s == '\0') {
        char* empty = (char*)malloc(1);
        if (empty) empty[0] = '\0';
        return empty ? empty : "";
    }
    const char* end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        end--;
    }
    size_t len = (size_t)(end - s + 1);
    char* res = (char*)malloc(len + 1);
    if (!res) return "";
    memcpy(res, s, len);
    res[len] = '\0';
    return res;
}

char* rtgetenv(const char* key) {
    if (!key) return "";
    const char* val = getenv(key);
    if (!val) return "";
    size_t len = strlen(val);
    char* res = (char*)malloc(len + 1);
    if (!res) return "";
    memcpy(res, val, len + 1);
    return res;
}

// --- STAGE 35.2: UTF-8 & RICH STRING PRIMITIVES (EXPLICIT-LENGTH & ZERO-COPY) ---

typedef struct {
    char* ptr;
    long long len;
    long long cap;
} SalivoString;

typedef struct {
    const char* ptr;
    long long len;
} SalivoStrSlice;

SalivoString salivo_string_new(const char* s, long long len) {
    SalivoString res;
    if (!s || len <= 0) {
        res.ptr = (char*)calloc(1, 1);
        res.len = 0;
        res.cap = 0;
        return res;
    }
    long long cap = len < 16 ? 16 : len;
    res.ptr = (char*)malloc((size_t)cap + 1);
    if (!res.ptr) {
        res.ptr = (char*)calloc(1, 1);
        res.len = 0;
        res.cap = 0;
        return res;
    }
    memcpy(res.ptr, s, (size_t)len);
    res.ptr[len] = '\0';
    res.len = len;
    res.cap = cap;
    return res;
}

void salivo_string_copy_to_ptr(long long dest_ptr, const char* s, long long len) {
    if (dest_ptr != 0 && s != NULL && len > 0) {
        memcpy((void*)(uintptr_t)dest_ptr, s, (size_t)len);
        ((char*)(uintptr_t)dest_ptr)[len] = '\0';
    }
}

const char* salivo_ptr_to_str(long long ptr) {
    if (ptr == 0) return "";
    return (const char*)(uintptr_t)ptr;
}

int rtutf8valid(const char* s, long long len) {
    if (!s) return (len == 0) ? 1 : 0;
    if (len < 0) return 0;
    const unsigned char* p = (const unsigned char*)s;
    const unsigned char* end = p + len;

    while (p < end) {
        unsigned char c = *p++;
        if (c <= 0x7F) {
            // 1-byte ASCII (0x00 - 0x7F)
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            // 2-byte sequence (0xC2..0xDF 0x80..0xBF)
            if (p >= end) return 0;
            unsigned char c2 = *p++;
            if ((c2 & 0xC0) != 0x80) return 0;
        } else if (c >= 0xE0 && c <= 0xEF) {
            // 3-byte sequence
            if (p + 1 >= end) return 0;
            unsigned char c2 = *p++;
            unsigned char c3 = *p++;
            if ((c3 & 0xC0) != 0x80) return 0;
            if (c == 0xE0) {
                if (c2 < 0xA0 || c2 > 0xBF) return 0; // Reject overlong
            } else if (c == 0xED) {
                if (c2 < 0x80 || c2 > 0x9F) return 0; // Reject UTF-16 surrogates (0xD800..0xDFFF)
            } else {
                if ((c2 & 0xC0) != 0x80) return 0;
            }
        } else if (c >= 0xF0 && c <= 0xF4) {
            // 4-byte sequence
            if (p + 2 >= end) return 0;
            unsigned char c2 = *p++;
            unsigned char c3 = *p++;
            unsigned char c4 = *p++;
            if ((c3 & 0xC0) != 0x80 || (c4 & 0xC0) != 0x80) return 0;
            if (c == 0xF0) {
                if (c2 < 0x90 || c2 > 0xBF) return 0; // Reject overlong
            } else if (c == 0xF4) {
                if (c2 < 0x80 || c2 > 0x8F) return 0; // Reject > U+10FFFF
            } else {
                if ((c2 & 0xC0) != 0x80) return 0;
            }
        } else {
            // Invalid leading byte (0x80..0xC1, 0xF5..0xFF)
            return 0;
        }
    }
    return 1;
}

int rtutf8bound(const char* s, long long len, long long offset) {
    if (!s || len < 0) return 0;
    if (offset < 0 || offset > len) return 0;
    if (offset == 0 || offset == len) return 1;
    unsigned char c = (unsigned char)s[offset];
    return ((c & 0xC0) != 0x80) ? 1 : 0;
}

long long rtutf8charat(const char* s, long long char_index) {
    if (!s || char_index < 0) return -1;
    const unsigned char* p = (const unsigned char*)s;
    long long current_index = 0;

    while (*p) {
        unsigned char c = *p;
        long long codepoint = 0;
        int seq_len = 0;

        if (c <= 0x7F) {
            codepoint = c;
            seq_len = 1;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (!p[1]) return -1;
            codepoint = ((c & 0x1F) << 6) | (p[1] & 0x3F);
            seq_len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            if (!p[1] || !p[2]) return -1;
            codepoint = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            seq_len = 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            if (!p[1] || !p[2] || !p[3]) return -1;
            codepoint = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            seq_len = 4;
        } else {
            return -1; // Invalid UTF-8 sequence
        }

        if (current_index == char_index) {
            return codepoint;
        }

        current_index++;
        p += seq_len;
    }
    return -1;
}

long long rtutf8next(const char* s, long long len, long long* inout_offset) {
    if (!s || !inout_offset || *inout_offset < 0 || *inout_offset >= len) {
        return -1;
    }
    const unsigned char* p = (const unsigned char*)s + *inout_offset;
    const unsigned char* end = (const unsigned char*)s + len;
    if (p >= end) return -1;

    unsigned char c = *p;
    long long codepoint = 0;
    int seq_len = 0;

    if (c <= 0x7F) {
        codepoint = c;
        seq_len = 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        if (p + 1 >= end) return -1;
        codepoint = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        seq_len = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        if (p + 2 >= end) return -1;
        codepoint = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        seq_len = 3;
    } else if (c >= 0xF0 && c <= 0xF4) {
        if (p + 3 >= end) return -1;
        codepoint = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        seq_len = 4;
    } else {
        return -1;
    }

    *inout_offset += seq_len;
    return codepoint;
}

long long rtutf8count(const char* s) {
    if (!s) return 0;
    long long count = 0;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        if ((*p & 0xC0) != 0x80) {
            count++;
        }
        p++;
    }
    return count;
}

int salivo_str_contains_raw(const char* s, long long slen, const char* sub, long long sublen) {
    if (!s || !sub || sublen < 0 || slen < sublen) return 0;
    if (sublen == 0) return 1;
    for (long long i = 0; i <= slen - sublen; i++) {
        if (memcmp(s + i, sub, (size_t)sublen) == 0) return 1;
    }
    return 0;
}

int rtcontains(const char* s, const char* sub) {
    if (!s || !sub) return 0;
    long long slen = (long long)strlen(s);
    long long sublen = (long long)strlen(sub);
    return salivo_str_contains_raw(s, slen, sub, sublen);
}

int salivo_str_startswith_raw(const char* s, long long slen, const char* prefix, long long plen) {
    if (!s || !prefix || plen < 0 || slen < plen) return 0;
    if (plen == 0) return 1;
    return memcmp(s, prefix, (size_t)plen) == 0 ? 1 : 0;
}

int rtstartsw(const char* s, const char* prefix) {
    if (!s || !prefix) return 0;
    long long slen = (long long)strlen(s);
    long long plen = (long long)strlen(prefix);
    return salivo_str_startswith_raw(s, slen, prefix, plen);
}

int salivo_str_endswith_raw(const char* s, long long slen, const char* suffix, long long suflen) {
    if (!s || !suffix || suflen < 0 || slen < suflen) return 0;
    if (suflen == 0) return 1;
    return memcmp(s + slen - suflen, suffix, (size_t)suflen) == 0 ? 1 : 0;
}

int rtendsw(const char* s, const char* suffix) {
    if (!s || !suffix) return 0;
    long long slen = (long long)strlen(s);
    long long suflen = (long long)strlen(suffix);
    return salivo_str_endswith_raw(s, slen, suffix, suflen);
}

long long salivo_str_find_raw(const char* s, long long slen, const char* sub, long long sublen) {
    if (!s || !sub || sublen < 0 || slen < sublen) return -1;
    if (sublen == 0) return 0;
    for (long long i = 0; i <= slen - sublen; i++) {
        if (memcmp(s + i, sub, (size_t)sublen) == 0) return i;
    }
    return -1;
}

long long rtfind(const char* s, const char* sub) {
    if (!s || !sub) return -1;
    long long slen = (long long)strlen(s);
    long long sublen = (long long)strlen(sub);
    return salivo_str_find_raw(s, slen, sub, sublen);
}

long long salivo_str_rfind_raw(const char* s, long long slen, const char* sub, long long sublen) {
    if (!s || !sub || sublen < 0 || slen < sublen) return -1;
    if (sublen == 0) return slen;
    for (long long i = slen - sublen; i >= 0; i--) {
        if (memcmp(s + i, sub, (size_t)sublen) == 0) return i;
    }
    return -1;
}

long long rtrfind(const char* s, const char* sub) {
    if (!s || !sub) return -1;
    long long slen = (long long)strlen(s);
    long long sublen = (long long)strlen(sub);
    return salivo_str_rfind_raw(s, slen, sub, sublen);
}

char* rtreplace(const char* s, const char* from, const char* to) {
    if (!s) return "";
    long long slen = (long long)strlen(s);
    if (!from || !to) {
        char* dup = (char*)malloc((size_t)slen + 1);
        if (dup) memcpy(dup, s, (size_t)slen + 1);
        return dup ? dup : "";
    }

    long long from_len = (long long)strlen(from);
    long long to_len = (long long)strlen(to);
    if (from_len == 0) {
        char* dup = (char*)malloc((size_t)slen + 1);
        if (dup) memcpy(dup, s, (size_t)slen + 1);
        return dup ? dup : "";
    }

    // Count occurrences
    long long count = 0;
    for (long long i = 0; i <= slen - from_len; ) {
        if (memcmp(s + i, from, (size_t)from_len) == 0) {
            count++;
            i += from_len;
        } else {
            i++;
        }
    }

    if (count == 0) {
        char* dup = (char*)malloc((size_t)slen + 1);
        if (dup) memcpy(dup, s, (size_t)slen + 1);
        return dup ? dup : "";
    }

    long long new_len = slen + count * (to_len - from_len);
    char* res = (char*)malloc((size_t)new_len + 1);
    if (!res) return "";

    char* dst = res;
    long long i = 0;
    while (i < slen) {
        if (i <= slen - from_len && memcmp(s + i, from, (size_t)from_len) == 0) {
            if (to_len > 0) {
                memcpy(dst, to, (size_t)to_len);
                dst += to_len;
            }
            i += from_len;
        } else {
            *dst++ = s[i++];
        }
    }
    *dst = '\0';
    return res;
}

char* rtutf8slice(const char* s, long long start, long long end) {
    if (!s) return "";
    long long total_len = (long long)strlen(s);
    if (start < 0 || end < start || end > total_len) {
        fprintf(stderr, "Salivo Runtime Error: String slice [%lld..%lld] out of bounds [0..%lld]\n", start, end, total_len);
        fflush(stderr);
        exit(1);
    }
    if (!rtutf8bound(s, total_len, start) || !rtutf8bound(s, total_len, end)) {
        fprintf(stderr, "Salivo Runtime Error: String slice [%lld..%lld] splits a UTF-8 multi-byte code point\n", start, end);
        fflush(stderr);
        exit(1);
    }
    long long slice_len = end - start;
    char* res = (char*)malloc((size_t)slice_len + 1);
    if (!res) return "";
    if (slice_len > 0) {
        memcpy(res, s + start, (size_t)slice_len);
    }
    res[slice_len] = '\0';
    return res;
}

SalivoStrSlice salivo_str_slice_borrow(const char* s, long long total_len, long long start, long long end) {
    SalivoStrSlice slice;
    slice.ptr = "";
    slice.len = 0;
    if (!s) return slice;
    if (start < 0 || end < start || end > total_len) {
        fprintf(stderr, "Salivo Runtime Error: String slice [%lld..%lld] out of bounds [0..%lld]\n", start, end, total_len);
        fflush(stderr);
        exit(1);
    }
    if (!rtutf8bound(s, total_len, start) || !rtutf8bound(s, total_len, end)) {
        fprintf(stderr, "Salivo Runtime Error: String slice [%lld..%lld] splits a UTF-8 multi-byte code point\n", start, end);
        fflush(stderr);
        exit(1);
    }
    slice.ptr = s + start;
    slice.len = end - start;
    return slice;
}

char* rtpush(const char* s, const char* append_str) {
    if (!s) s = "";
    if (!append_str) append_str = "";
    size_t slen = strlen(s);
    size_t applen = strlen(append_str);
    char* res = (char*)malloc(slen + applen + 1);
    if (!res) return "";
    memcpy(res, s, slen);
    memcpy(res + slen, append_str, applen);
    res[slen + applen] = '\0';
    return res;
}

char* rtpushchar(const char* s, long long codepoint) {
    if (!s) s = "";
    size_t slen = strlen(s);
    unsigned char buf[5] = {0};
    int clen = 0;

    if (codepoint >= 0 && codepoint <= 0x7F) {
        buf[0] = (unsigned char)codepoint;
        clen = 1;
    } else if (codepoint <= 0x7FF) {
        buf[0] = (unsigned char)(0xC0 | ((codepoint >> 6) & 0x1F));
        buf[1] = (unsigned char)(0x80 | (codepoint & 0x3F));
        clen = 2;
    } else if (codepoint <= 0xFFFF) {
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            buf[0] = 0xEF; buf[1] = 0xBF; buf[2] = 0xBD;
            clen = 3;
        } else {
            buf[0] = (unsigned char)(0xE0 | ((codepoint >> 12) & 0x0F));
            buf[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
            buf[2] = (unsigned char)(0x80 | (codepoint & 0x3F));
            clen = 3;
        }
    } else if (codepoint <= 0x10FFFF) {
        buf[0] = (unsigned char)(0xF0 | ((codepoint >> 18) & 0x07));
        buf[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3F));
        buf[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        buf[3] = (unsigned char)(0x80 | (codepoint & 0x3F));
        clen = 4;
    } else {
        buf[0] = 0xEF; buf[1] = 0xBF; buf[2] = 0xBD;
        clen = 3;
    }

    char* res = (char*)malloc(slen + clen + 1);
    if (!res) return "";
    memcpy(res, s, slen);
    memcpy(res + slen, buf, clen);
    res[slen + clen] = '\0';
    return res;
}

char* rtreserve(const char* s, long long cap) {
    if (!s) s = "";
    size_t slen = strlen(s);
    size_t alloc_size = (size_t)cap > slen ? (size_t)cap + 1 : slen + 1;
    char* res = (char*)malloc(alloc_size);
    if (!res) return "";
    memcpy(res, s, slen + 1);
    return res;
}

char* rtclear(const char* s) {
    (void)s;
    char* res = (char*)malloc(1);
    if (res) res[0] = '\0';
    return res ? res : "";
}

char* rtrepeat(const char* s, long long count) {
    if (!s || count <= 0) {
        char* empty = (char*)malloc(1);
        if (empty) empty[0] = '\0';
        return empty ? empty : "";
    }
    size_t slen = strlen(s);
    size_t total = slen * (size_t)count;
    char* res = (char*)malloc(total + 1);
    if (!res) return "";
    for (long long i = 0; i < count; i++) {
        memcpy(res + i * slen, s, slen);
    }
    res[total] = '\0';
    return res;
}

char* rtpadstart(const char* s, long long target_len, const char* pad) {
    if (!s) s = "";
    if (!pad || *pad == '\0') pad = " ";
    long long current_len = (long long)strlen(s);
    if (current_len >= target_len) {
        size_t slen = strlen(s);
        char* dup = (char*)malloc(slen + 1);
        if (dup) memcpy(dup, s, slen + 1);
        return dup ? dup : "";
    }
    long long pad_needed = target_len - current_len;
    size_t pad_len = strlen(pad);
    char* res = (char*)malloc((size_t)target_len + 1);
    if (!res) return "";
    for (long long i = 0; i < pad_needed; i++) {
        res[i] = pad[i % pad_len];
    }
    memcpy(res + pad_needed, s, (size_t)current_len);
    res[target_len] = '\0';
    return res;
}

char* rtpadend(const char* s, long long target_len, const char* pad) {
    if (!s) s = "";
    if (!pad || *pad == '\0') pad = " ";
    long long current_len = (long long)strlen(s);
    if (current_len >= target_len) {
        size_t slen = strlen(s);
        char* dup = (char*)malloc(slen + 1);
        if (dup) memcpy(dup, s, slen + 1);
        return dup ? dup : "";
    }
    long long pad_needed = target_len - current_len;
    size_t pad_len = strlen(pad);
    char* res = (char*)malloc((size_t)target_len + 1);
    if (!res) return "";
    memcpy(res, s, (size_t)current_len);
    for (long long i = 0; i < pad_needed; i++) {
        res[current_len + i] = pad[i % pad_len];
    }
    res[target_len] = '\0';
    return res;
}

char* rtfmtdebug(const char* s) {
    if (!s) {
        char* null_str = (char*)malloc(7);
        if (null_str) strcpy(null_str, "\"null\"");
        return null_str ? null_str : "\"null\"";
    }
    size_t slen = strlen(s);
    char* res = (char*)malloc(slen * 2 + 3);
    if (!res) return "";
    char* dst = res;
    *dst++ = '"';
    for (size_t i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') {
            *dst++ = '\\'; *dst++ = '"';
        } else if (c == '\\') {
            *dst++ = '\\'; *dst++ = '\\';
        } else if (c == '\n') {
            *dst++ = '\\'; *dst++ = 'n';
        } else if (c == '\t') {
            *dst++ = '\\'; *dst++ = 't';
        } else if (c == '\r') {
            *dst++ = '\\'; *dst++ = 'r';
        } else if (c == '\0') {
            *dst++ = '\\'; *dst++ = '0';
        } else {
            *dst++ = (char)c;
        }
    }
    *dst++ = '"';
    *dst = '\0';
    return res;
}

void rtflushstdout(void) {
    fflush(stdout);
}

// --- TASK RUNTIME ENGINE ---

typedef long long (*salivo_task_fn_t)(long long arg);

typedef struct {
    int id;
    int state; // 0: Created, 1: Ready, 2: Running, 3: Pending, 4: Completed, 5: Destroyed, 6: Failed
    int is_active;
    int is_running; // 1 while a thread is actively executing fn_ptr
    salivo_task_fn_t fn_ptr;
    long long arg_val;
    long long result_val;
    int scope_id;
    int is_cancelled;
    int is_failed;
    char error_msg[256];
} SalivoTaskSlot;

typedef struct {
    int id;
    int is_active;
    int task_count;
    int completed_count;
    int is_cancelled;
    long long task_ids[MAX_SCOPE_TASKS];
} SalivoScopeSlot;

typedef struct {
    int id;
    int is_cancelled;
    int is_active;
} SalivoTokenSlot;

/* The task and scope tables are large (about 20 MB and 32 MB), so they are allocated on first use:
   a process that never spawns a task or opens a scope does not commit them. */
static void* volatile salivo_task_table_mem;
static void* volatile salivo_scope_table_mem;

static void* salivo_lazy_table(void* volatile* slot, size_t bytes) {
#ifdef _WIN32
    void* t = InterlockedCompareExchangePointer((PVOID volatile*)slot, NULL, NULL);
#else
    void* t = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
#endif
    if (t) return t;
    void* fresh = calloc(1, bytes);
    if (!fresh) {
        fprintf(stderr, "out of memory allocating the task runtime tables\n");
        exit(1);
    }
#ifdef _WIN32
    void* prev = InterlockedCompareExchangePointer((PVOID volatile*)slot, fresh, NULL);
#else
    void* prev = __sync_val_compare_and_swap(slot, NULL, fresh);
#endif
    if (prev) {
        free(fresh);
        return prev;
    }
    return fresh;
}
#define g_task_table ((SalivoTaskSlot*)salivo_lazy_table(&salivo_task_table_mem, sizeof(SalivoTaskSlot) * (MAX_TASKS + 1)))
#define g_scope_table ((SalivoScopeSlot*)salivo_lazy_table(&salivo_scope_table_mem, sizeof(SalivoScopeSlot) * (MAX_SCOPES + 1)))
static SalivoTokenSlot g_token_table[MAX_TOKENS + 1];

static int g_ready_queue[READY_QUEUE_CAP];
static int g_queue_head = 0;
static int g_queue_tail = 0;
static int g_queue_count = 0;
static int g_next_task_id = 1;
static long long g_last_poll_result = 0;

// --- THREAD SYNCHRONIZATION MUTEX ENGINE ---

#ifdef _WIN32
static CRITICAL_SECTION g_task_rt_cs;
static CRITICAL_SECTION g_channel_rt_cs;
static CRITICAL_SECTION g_mem_rt_cs;
static CONDITION_VARIABLE g_task_rt_cv;
static volatile LONG g_rt_mutex_inited = 0;

static void salivo_rt_init_mutex(void) {
    if (InterlockedCompareExchange(&g_rt_mutex_inited, 1, 0) == 0) {
        InitializeCriticalSection(&g_task_rt_cs);
        InitializeCriticalSection(&g_channel_rt_cs);
        InitializeCriticalSection(&g_mem_rt_cs);
        InitializeConditionVariable(&g_task_rt_cv);
    }
}

static inline void salivo_task_lock(void) {
    salivo_rt_init_mutex();
    EnterCriticalSection(&g_task_rt_cs);
}
static inline void salivo_task_unlock(void) {
    LeaveCriticalSection(&g_task_rt_cs);
}
static inline void salivo_task_wait(void) {
    salivo_rt_init_mutex();
    SleepConditionVariableCS(&g_task_rt_cv, &g_task_rt_cs, INFINITE);
}
static inline void salivo_task_notify_all(void) {
    WakeAllConditionVariable(&g_task_rt_cv);
}

static inline void salivo_channel_lock(void) {
    salivo_rt_init_mutex();
    EnterCriticalSection(&g_channel_rt_cs);
}
static inline void salivo_channel_unlock(void) {
    LeaveCriticalSection(&g_channel_rt_cs);
}

static inline void salivo_mem_lock(void) {
    salivo_rt_init_mutex();
    EnterCriticalSection(&g_mem_rt_cs);
}
static inline void salivo_mem_unlock(void) {
    LeaveCriticalSection(&g_mem_rt_cs);
}

#else
#include <pthread.h>
static pthread_mutex_t g_task_rt_cs = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_channel_rt_cs = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_mem_rt_cs = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_task_rt_cv = PTHREAD_COND_INITIALIZER;

static inline void salivo_task_lock(void) { pthread_mutex_lock(&g_task_rt_cs); }
static inline void salivo_task_unlock(void) { pthread_mutex_unlock(&g_task_rt_cs); }
static inline void salivo_task_wait(void) { pthread_cond_wait(&g_task_rt_cv, &g_task_rt_cs); }
static inline void salivo_task_notify_all(void) { pthread_cond_broadcast(&g_task_rt_cv); }

static inline void salivo_channel_lock(void) { pthread_mutex_lock(&g_channel_rt_cs); }
static inline void salivo_channel_unlock(void) { pthread_mutex_unlock(&g_channel_rt_cs); }

static inline void salivo_mem_lock(void) { pthread_mutex_lock(&g_mem_rt_cs); }
static inline void salivo_mem_unlock(void) { pthread_mutex_unlock(&g_mem_rt_cs); }
#endif

static int alloc_task_slot(void) {
    for (int i = 1; i <= MAX_TASKS; ++i) {
        int id = ((g_next_task_id + i - 1) % MAX_TASKS) + 1;
        if (!g_task_table[id].is_active) {
            g_next_task_id = id;
            return id;
        }
    }
    return -1;
}

long long salivo_task_spawn(long long task_id) {
    salivo_task_lock();
    if (g_queue_count >= READY_QUEUE_CAP) {
        salivo_task_unlock();
        return -1;
    }
    int id = alloc_task_slot();
    if (id < 0) {
        salivo_task_unlock();
        return -1;
    }
    g_task_table[id].id = id;
    g_task_table[id].state = 1; // Ready
    g_task_table[id].is_active = 1;
    g_task_table[id].fn_ptr = NULL;
    g_task_table[id].arg_val = 0;
    g_task_table[id].result_val = task_id;
    g_task_table[id].scope_id = 0;
    g_task_table[id].is_cancelled = 0;
    g_task_table[id].is_failed = 0;
    g_task_table[id].error_msg[0] = '\0';

    g_ready_queue[g_queue_tail] = id;
    g_queue_tail = (g_queue_tail + 1) % READY_QUEUE_CAP;
    g_queue_count++;
    salivo_task_unlock();

    SALIVO_LOG("[TASK_RT] Spawned task_id=%d -> host_task_id=%d\n", task_id, id);
    return (long long)id;
}

long long salivo_task_spawn_val(long long res_val) {
    salivo_task_lock();
    if (g_queue_count >= READY_QUEUE_CAP) {
        salivo_task_unlock();
        return -1;
    }
    int id = alloc_task_slot();
    if (id < 0) {
        salivo_task_unlock();
        return -1;
    }
    g_task_table[id].id = id;
    g_task_table[id].state = 1; // Ready
    g_task_table[id].is_active = 1;
    g_task_table[id].fn_ptr = NULL;
    g_task_table[id].arg_val = 0;
    g_task_table[id].result_val = res_val;
    g_task_table[id].scope_id = 0;
    g_task_table[id].is_cancelled = 0;
    g_task_table[id].is_failed = 0;
    g_task_table[id].error_msg[0] = '\0';

    g_ready_queue[g_queue_tail] = id;
    g_queue_tail = (g_queue_tail + 1) % READY_QUEUE_CAP;
    g_queue_count++;
    salivo_task_unlock();

    SALIVO_LOG("[TASK_RT] Spawned task with val=%lld -> host_task_id=%d\n", res_val, id);
    return (long long)id;
}

long long salivo_task_spawn_fn(salivo_task_fn_t fn_ptr, long long arg_val) {
    salivo_task_lock();
    if (g_queue_count >= READY_QUEUE_CAP) {
        salivo_task_unlock();
        return -1;
    }
    int id = alloc_task_slot();
    if (id < 0) {
        salivo_task_unlock();
        return -1;
    }
    g_task_table[id].id = id;
    g_task_table[id].state = 1; // Ready
    g_task_table[id].is_active = 1;
    g_task_table[id].fn_ptr = fn_ptr;
    g_task_table[id].arg_val = arg_val;
    g_task_table[id].result_val = 0;
    g_task_table[id].scope_id = 0;
    g_task_table[id].is_cancelled = 0;
    g_task_table[id].is_failed = 0;
    g_task_table[id].error_msg[0] = '\0';

    g_ready_queue[g_queue_tail] = id;
    g_queue_tail = (g_queue_tail + 1) % READY_QUEUE_CAP;
    g_queue_count++;
    salivo_task_unlock();

    SALIVO_LOG("[TASK_RT] Spawned task with fn_ptr=%p, arg=%lld -> host_task_id=%d\n", (void*)fn_ptr, arg_val, id);
    return (long long)id;
}

long long salivo_task_fail(long long task_id, const char* msg) {
    salivo_task_lock();
    if (task_id < 1 || task_id > MAX_TASKS || !g_task_table[task_id].is_active) {
        if (g_current_executing_task_id >= 1 && g_current_executing_task_id <= MAX_TASKS && g_task_table[g_current_executing_task_id].is_active) {
            task_id = g_current_executing_task_id;
        } else {
            salivo_task_unlock();
            return -1;
        }
    }
    if (g_task_table[task_id].is_cancelled || g_task_table[task_id].state >= 4) {
        salivo_task_unlock();
        return -1;
    }
    SalivoTaskSlot* slot = &g_task_table[task_id];
    slot->state = 6; // Failed
    slot->is_failed = 1;
    if (msg) {
        strncpy(slot->error_msg, msg, 255);
        slot->error_msg[255] = '\0';
    } else {
        strcpy(slot->error_msg, "Task failed");
    }
    salivo_task_unlock();

    SALIVO_LOG("[TASK_RT] Task %lld failed\n", task_id);
    return 0;
}

long long salivo_task_is_failed(long long task_id) {
    salivo_task_lock();
    if (task_id < 1 || task_id > MAX_TASKS) {
        salivo_task_unlock();
        return 0;
    }
    long long res = g_task_table[task_id].is_failed || (g_task_table[task_id].state == 6);
    salivo_task_unlock();
    return res;
}

long long salivo_task_enqueue_ready(int task_id) {
    salivo_task_lock();
    if (task_id < 1 || task_id > MAX_TASKS || 
        !g_task_table[task_id].is_active ||
        g_task_table[task_id].is_cancelled ||
        g_task_table[task_id].state >= 4 ||
        g_task_table[task_id].is_running) {
        salivo_task_unlock();
        return -1;
    }
    if (g_queue_count >= READY_QUEUE_CAP) {
        salivo_task_unlock();
        return -1;
    }
    g_ready_queue[g_queue_tail] = task_id;
    g_queue_tail = (g_queue_tail + 1) % READY_QUEUE_CAP;
    g_queue_count++;
    salivo_task_unlock();
    return 0;
}

long long salivo_task_dequeue_ready(void) {
    salivo_task_lock();
    if (g_queue_count <= 0) {
        salivo_task_unlock();
        return 0;
    }
    int task_id = g_ready_queue[g_queue_head];
    g_queue_head = (g_queue_head + 1) % READY_QUEUE_CAP;
    g_queue_count--;
    salivo_task_unlock();
    return (long long)task_id;
}

long long salivo_task_ready_queue_empty(void) {
    salivo_task_lock();
    long long res = (g_queue_count == 0 ? 1 : 0);
    salivo_task_unlock();
    return res;
}

long long salivo_task_ready_queue_size(void) {
    salivo_task_lock();
    long long res = (long long)g_queue_count;
    salivo_task_unlock();
    return res;
}

long long salivo_task_get_state(int task_id) {
    salivo_task_lock();
    if (task_id < 1 || task_id > MAX_TASKS || !g_task_table[task_id].is_active) {
        salivo_task_unlock();
        return 0; // Created
    }
    long long st = (long long)g_task_table[task_id].state;
    salivo_task_unlock();
    return st;
}

long long salivo_task_set_state(int task_id, int state) {
    salivo_task_lock();
    if (task_id < 1 || task_id > MAX_TASKS || !g_task_table[task_id].is_active) {
        salivo_task_unlock();
        return -1;
    }
    if (state < 0 || state > 6) {
        salivo_task_unlock();
        return -1;
    }
    int current = g_task_table[task_id].state;
    int valid = 0;
    if (current == state) valid = 1;
    else if (current == 0 && state == 1) valid = 1; // Created -> Ready
    else if (current == 1 && state == 2) valid = 1; // Ready -> Running
    else if (current == 2 && state == 3) valid = 1; // Running -> Pending
    else if (current == 2 && state == 4) valid = 1; // Running -> Completed
    else if (current == 2 && state == 6) valid = 1; // Running -> Failed
    else if (current == 3 && state == 1) valid = 1; // Pending -> Ready
    else if (state == 5) valid = 1; // Any -> Destroyed
    
    if (!valid) {
        salivo_task_unlock();
        return -1;
    }

    g_task_table[task_id].state = state;
    if (state == 5) {
        g_task_table[task_id].is_active = 0;
    }
    salivo_task_notify_all();
    salivo_task_unlock();
    return 0;
}

long long salivo_task_destroy(int task_id) {
    salivo_task_lock();
    if (task_id < 1 || task_id > MAX_TASKS) {
        salivo_task_unlock();
        return -1;
    }
    g_task_table[task_id].is_cancelled = 1;
    g_task_table[task_id].state = 5; // Destroyed
    if (!g_task_table[task_id].is_running) {
        g_task_table[task_id].is_active = 0;
    }
    salivo_task_notify_all();
    salivo_task_unlock();
    return 0;
}

long long salivo_task_execute(long long task_id) {
    salivo_task_lock();
    if (task_id < 1 || task_id > MAX_TASKS || !g_task_table[task_id].is_active) {
        salivo_task_unlock();
        return 0;
    }
    SalivoTaskSlot* slot = &g_task_table[task_id];

    // If another thread is actively running this task, wait on condition variable
    while (slot->is_active && slot->state == 2 && slot->is_running) {
        salivo_task_wait();
        if (task_id < 1 || task_id > MAX_TASKS || !g_task_table[task_id].is_active) {
            salivo_task_unlock();
            return 0;
        }
    }

    if (slot->is_cancelled || slot->state == 5) {
        slot->state = 5; // Destroyed
        if (!slot->is_running) slot->is_active = 0;
        salivo_task_unlock();
        return 0;
    }
    if (slot->state == 4) {
        long long res = slot->result_val;
        salivo_task_unlock();
        return res;
    }
    if (slot->state == 6 || slot->is_failed) {
        salivo_task_unlock();
        return 0;
    }

    // Atomic state transition: only READY(1), CREATED(0), or PENDING(3) transitions to RUNNING(2)
    slot->state = 2; // Running
    slot->is_running = 1;
    salivo_task_fn_t fn_ptr = slot->fn_ptr;
    long long arg_val = slot->arg_val;
    long long default_res = slot->result_val;
    salivo_task_unlock();

    long long res = default_res;
    if (fn_ptr != NULL) {
        g_current_executing_task_id = task_id;
        res = fn_ptr(arg_val);
        g_current_executing_task_id = 0;
    }

    salivo_task_lock();
    slot->is_running = 0;
    if (slot->is_cancelled || slot->state == 5) {
        slot->state = 5;
        slot->is_active = 0; // Execution finished for cancelled task; slot can now be recycled
    } else if (slot->is_active && !slot->is_failed && slot->state != 6) {
        slot->result_val = res;
        slot->state = 4; // Completed
        if (slot->scope_id > 0 && slot->scope_id <= MAX_SCOPES) {
            g_scope_table[slot->scope_id].completed_count++;
        }
    }
    g_last_poll_result = slot->result_val;
    long long final_res = slot->result_val;
    salivo_task_notify_all();
    salivo_task_unlock();

    return final_res;
}

long long salivo_task_get_result(long long task_id) {
    salivo_task_lock();
    if (task_id < 1 || task_id > MAX_TASKS) {
        salivo_task_unlock();
        return 0;
    }
    g_last_poll_result = g_task_table[task_id].result_val;
    long long res = g_task_table[task_id].result_val;
    salivo_task_unlock();
    return res;
}

long long salivo_task_get_last_poll_result(void) {
    salivo_task_lock();
    long long res = g_last_poll_result;
    salivo_task_unlock();
    return res;
}

long long salivo_task_set_last_poll_result(long long val) {
    salivo_task_lock();
    g_last_poll_result = val;
    salivo_task_unlock();
    return 0;
}

long long salivo_task_set_result(long long task_id, long long val) {
    salivo_task_lock();
    if (task_id < 1 || task_id > MAX_TASKS || 
        !g_task_table[task_id].is_active || 
        g_task_table[task_id].is_running || 
        g_task_table[task_id].is_cancelled || 
        g_task_table[task_id].state == 5) {
        salivo_task_unlock();
        return -1;
    }
    g_task_table[task_id].result_val = val;
    salivo_task_unlock();
    return 0;
}

long long salivo_task_cancel(long long task_id) {
    salivo_task_lock();
    if (task_id < 1 || task_id > MAX_TASKS) {
        salivo_task_unlock();
        return -1;
    }
    g_task_table[task_id].is_cancelled = 1;
    g_task_table[task_id].state = 5; // Destroyed
    if (!g_task_table[task_id].is_running) {
        g_task_table[task_id].is_active = 0;
    }
    salivo_task_notify_all();
    salivo_task_unlock();
    SALIVO_LOG("[TASK_RT] Cancelled task_id=%lld\n", task_id);
    return 0;
}

long long salivo_task_is_cancelled(long long task_id) {
    salivo_task_lock();
    if (task_id < 1 || task_id > MAX_TASKS) {
        salivo_task_unlock();
        return 1;
    }
    long long res = g_task_table[task_id].is_cancelled || (g_task_table[task_id].state == 5);
    salivo_task_unlock();
    return res;
}

// --- STRUCTURED CONCURRENCY SCOPE PRIMITIVES ---

long long salivo_scope_create(void) {
    salivo_task_lock();
    for (int i = 1; i <= MAX_SCOPES; i++) {
        if (!g_scope_table[i].is_active) {
            g_scope_table[i].id = i;
            g_scope_table[i].is_active = 1;
            g_scope_table[i].task_count = 0;
            g_scope_table[i].completed_count = 0;
            g_scope_table[i].is_cancelled = 0;
            salivo_task_unlock();
            SALIVO_LOG("[SCOPE_RT] Created scope_id=%d\n", i);
            return (long long)i;
        }
    }
    salivo_task_unlock();
    return -1;
}

long long salivo_scope_spawn(long long scope_id, long long res_val) {
    salivo_task_lock();
    if (scope_id < 1 || scope_id > MAX_SCOPES || !g_scope_table[scope_id].is_active) {
        salivo_task_unlock();
        return -1;
    }
    SalivoScopeSlot* scope = &g_scope_table[scope_id];
    if (scope->is_cancelled) {
        salivo_task_unlock();
        return -1;
    }

    if (g_queue_count >= READY_QUEUE_CAP || scope->task_count >= MAX_SCOPE_TASKS) {
        salivo_task_unlock();
        return -1;
    }

    int id = alloc_task_slot();
    if (id < 0) {
        salivo_task_unlock();
        return -1;
    }

    g_task_table[id].id = id;
    g_task_table[id].state = 1; // Ready
    g_task_table[id].is_active = 1;
    g_task_table[id].fn_ptr = NULL;
    g_task_table[id].arg_val = 0;
    g_task_table[id].result_val = res_val;
    g_task_table[id].scope_id = (int)scope_id;
    g_task_table[id].is_cancelled = 0;
    g_task_table[id].is_failed = 0;
    g_task_table[id].error_msg[0] = '\0';

    scope->task_ids[scope->task_count++] = id;

    g_ready_queue[g_queue_tail] = id;
    g_queue_tail = (g_queue_tail + 1) % READY_QUEUE_CAP;
    g_queue_count++;

    salivo_task_unlock();
    SALIVO_LOG("[SCOPE_RT] Scope %lld spawned task_id=%d\n", scope_id, id);
    return (long long)id;
}

long long salivo_scope_join(long long scope_id, long long task_id) {
    if (task_id < 1 || task_id > MAX_TASKS) return -1;
    salivo_task_lock();
    if (g_task_table[task_id].is_cancelled || (scope_id >= 1 && scope_id <= MAX_SCOPES && g_scope_table[scope_id].is_cancelled)) {
        salivo_task_unlock();
        return -1;
    }
    salivo_task_unlock();
    return salivo_task_execute(task_id);
}

long long salivo_scope_cancel(long long scope_id) {
    salivo_task_lock();
    if (scope_id < 1 || scope_id > MAX_SCOPES || !g_scope_table[scope_id].is_active) {
        salivo_task_unlock();
        return -1;
    }
    SalivoScopeSlot* scope = &g_scope_table[scope_id];
    scope->is_cancelled = 1;
    int count = scope->task_count;
    if (count > MAX_SCOPE_TASKS) count = MAX_SCOPE_TASKS;
    long long* tids = NULL;
    if (count > 0) {
        tids = (long long*)malloc((size_t)count * sizeof(long long));
        if (tids) {
            for (int i = 0; i < count; i++) {
                tids[i] = scope->task_ids[i];
            }
        }
    }
    salivo_task_unlock();

    if (tids) {
        for (int i = 0; i < count; i++) {
            salivo_task_cancel(tids[i]);
        }
        free(tids);
    }
    return 0;
}

long long salivo_scope_wait(long long scope_id) {
    salivo_task_lock();
    if (scope_id < 1 || scope_id > MAX_SCOPES || !g_scope_table[scope_id].is_active) {
        salivo_task_unlock();
        return -1;
    }
    SalivoScopeSlot* scope = &g_scope_table[scope_id];
    int count = scope->task_count;
    if (count > MAX_SCOPE_TASKS) count = MAX_SCOPE_TASKS;
    long long* tids = NULL;
    if (count > 0) {
        tids = (long long*)malloc((size_t)count * sizeof(long long));
        if (tids) {
            for (int i = 0; i < count; i++) {
                tids[i] = scope->task_ids[i];
            }
        }
    }
    salivo_task_unlock();

    if (tids) {
        for (int i = 0; i < count; i++) {
            salivo_task_execute(tids[i]);
        }
        free(tids);
    }

    return 0;
}

long long salivo_scope_join_all(long long scope_id) {
    salivo_task_lock();
    if (scope_id < 1 || scope_id > MAX_SCOPES || !g_scope_table[scope_id].is_active) {
        salivo_task_unlock();
        return 0;
    }
    SalivoScopeSlot* scope = &g_scope_table[scope_id];
    int count = scope->task_count;
    if (count > MAX_SCOPE_TASKS) count = MAX_SCOPE_TASKS;
    long long* tids = NULL;
    if (count > 0) {
        tids = (long long*)malloc((size_t)count * sizeof(long long));
        if (tids) {
            for (int i = 0; i < count; i++) {
                tids[i] = scope->task_ids[i];
            }
        }
    }
    salivo_task_unlock();

    long long total_sum = 0;
    if (tids) {
        for (int i = 0; i < count; i++) {
            total_sum += salivo_task_execute(tids[i]);
        }
        free(tids);
    }
    return total_sum;
}

long long salivo_scope_wait_any(long long scope_id) {
    salivo_task_lock();
    if (scope_id < 1 || scope_id > MAX_SCOPES || !g_scope_table[scope_id].is_active) {
        salivo_task_unlock();
        return 0;
    }
    SalivoScopeSlot* scope = &g_scope_table[scope_id];
    if (scope->task_count <= 0) {
        salivo_task_unlock();
        return 0;
    }
    int count = scope->task_count;
    if (count > MAX_SCOPE_TASKS) count = MAX_SCOPE_TASKS;

    while (1) {
        int all_terminal = 1;
        for (int i = 0; i < count; i++) {
            int tid = (int)scope->task_ids[i];
            if (tid >= 1 && tid <= MAX_TASKS && g_task_table[tid].is_active) {
                if (g_task_table[tid].state == 4 || g_task_table[tid].state == 6) {
                    long long res = g_task_table[tid].result_val;
                    salivo_task_unlock();
                    return res;
                }
                all_terminal = 0; // Still active and not terminal
            }
        }
        if (all_terminal) {
            salivo_task_unlock();
            return 0;
        }
        salivo_task_wait();
    }
}

long long salivo_scope_wait_all(long long scope_id) {
    return salivo_scope_wait(scope_id);
}

// --- CANCELLATION TOKEN PRIMITIVES ---

long long salivo_token_create(void) {
    salivo_task_lock();
    for (int i = 1; i <= MAX_TOKENS; i++) {
        if (!g_token_table[i].is_active) {
            g_token_table[i].id = i;
            g_token_table[i].is_active = 1;
            g_token_table[i].is_cancelled = 0;
            salivo_task_unlock();
            SALIVO_LOG("[TOKEN_RT] Created token_id=%d\n", i);
            return (long long)i;
        }
    }
    salivo_task_unlock();
    return -1;
}

long long salivo_token_cancel(long long token_id) {
    salivo_task_lock();
    if (token_id < 1 || token_id > MAX_TOKENS || !g_token_table[token_id].is_active) {
        salivo_task_unlock();
        return -1;
    }
    g_token_table[token_id].is_cancelled = 1;
    salivo_task_unlock();
    SALIVO_LOG("[TOKEN_RT] Cancelled token_id=%lld\n", token_id);
    return 0;
}

long long salivo_token_is_cancelled(long long token_id) {
    salivo_task_lock();
    if (token_id < 1 || token_id > MAX_TOKENS || !g_token_table[token_id].is_active) {
        salivo_task_unlock();
        return 1;
    }
    long long res = (long long)g_token_table[token_id].is_cancelled;
    salivo_task_unlock();
    return res;
}

// --- GENUINE MPSC CHANNEL RUNTIME ENGINE ---

typedef struct {
    char* msgs[CHANNEL_CAP];
    long long int_msgs[CHANNEL_CAP];
    int msg_types[CHANNEL_CAP]; // 0 = string, 1 = int64
    int head;
    int tail;
    int count;
    int is_active;
} SalivoChannelSlot;

static SalivoChannelSlot g_channel_table[MAX_CHANNELS + 1];

long long salivo_channel_create(void) {
    salivo_channel_lock();
    for (int i = 1; i <= MAX_CHANNELS; i++) {
        if (!g_channel_table[i].is_active) {
            g_channel_table[i].head = 0;
            g_channel_table[i].tail = 0;
            g_channel_table[i].count = 0;
            g_channel_table[i].is_active = 1;
            salivo_channel_unlock();
            SALIVO_LOG("[CHANNEL_RT] Created channel_handle=%d\n", i);
            return (long long)i;
        }
    }
    salivo_channel_unlock();
    return -1;
}

long long salivo_channel_send(long long channel_handle, const char* msg) {
    salivo_channel_lock();
    if (channel_handle < 1 || channel_handle > MAX_CHANNELS || !g_channel_table[channel_handle].is_active) {
        salivo_channel_unlock();
        return -1;
    }
    SalivoChannelSlot* chan = &g_channel_table[channel_handle];
    if (chan->count >= CHANNEL_CAP) {
        salivo_channel_unlock();
        return -1;
    }
    if (!msg) msg = "";
    char* copy = (char*)malloc(strlen(msg) + 1);
    strcpy(copy, msg);

    chan->msgs[chan->tail] = copy;
    chan->int_msgs[chan->tail] = 0;
    chan->msg_types[chan->tail] = 0;
    chan->tail = (chan->tail + 1) % CHANNEL_CAP;
    chan->count++;
    salivo_channel_unlock();

    SALIVO_LOG("[CHANNEL_RT] Channel %lld sent msg: '%s'\n", channel_handle, msg);
    return 0;
}

long long salivo_channel_send_i64(long long channel_handle, long long val) {
    salivo_channel_lock();
    if (channel_handle < 1 || channel_handle > MAX_CHANNELS || !g_channel_table[channel_handle].is_active) {
        salivo_channel_unlock();
        return -1;
    }
    SalivoChannelSlot* chan = &g_channel_table[channel_handle];
    if (chan->count >= CHANNEL_CAP) {
        salivo_channel_unlock();
        return -1;
    }
    chan->msgs[chan->tail] = NULL;
    chan->int_msgs[chan->tail] = val;
    chan->msg_types[chan->tail] = 1;
    chan->tail = (chan->tail + 1) % CHANNEL_CAP;
    chan->count++;
    salivo_channel_unlock();

    SALIVO_LOG("[CHANNEL_RT] Channel %lld sent i64: %lld\n", channel_handle, val);
    return 0;
}

char* salivo_channel_recv(long long channel_handle) {
    SALIVO_BLOCKING("channel_recv");
    salivo_channel_lock();
    if (channel_handle < 1 || channel_handle > MAX_CHANNELS || !g_channel_table[channel_handle].is_active) {
        salivo_channel_unlock();
        char* empty = (char*)malloc(1);
        empty[0] = '\0';
        return empty;
    }
    SalivoChannelSlot* chan = &g_channel_table[channel_handle];
    if (chan->count <= 0) {
        salivo_channel_unlock();
        char* empty = (char*)malloc(1);
        empty[0] = '\0';
        return empty;
    }
    char* msg = NULL;
    if (chan->msg_types[chan->head] == 1) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", chan->int_msgs[chan->head]);
        msg = (char*)malloc(strlen(buf) + 1);
        strcpy(msg, buf);
    } else {
        msg = chan->msgs[chan->head];
        chan->msgs[chan->head] = NULL;
    }
    chan->head = (chan->head + 1) % CHANNEL_CAP;
    chan->count--;
    salivo_channel_unlock();
    SALIVO_LOG("[CHANNEL_RT] Channel %lld recv msg: '%s'\n", channel_handle, msg ? msg : "null");
    return msg;
}

long long salivo_channel_recv_i64(long long channel_handle) {
    SALIVO_BLOCKING("channel_recv");
    salivo_channel_lock();
    if (channel_handle < 1 || channel_handle > MAX_CHANNELS || !g_channel_table[channel_handle].is_active) {
        salivo_channel_unlock();
        return 0;
    }
    SalivoChannelSlot* chan = &g_channel_table[channel_handle];
    if (chan->count <= 0) {
        salivo_channel_unlock();
        return 0;
    }
    long long val = 0;
    if (chan->msg_types[chan->head] == 1) {
        val = chan->int_msgs[chan->head];
    } else if (chan->msgs[chan->head]) {
        val = atoll(chan->msgs[chan->head]);
        free(chan->msgs[chan->head]);
        chan->msgs[chan->head] = NULL;
    }
    chan->head = (chan->head + 1) % CHANNEL_CAP;
    chan->count--;
    salivo_channel_unlock();
    SALIVO_LOG("[CHANNEL_RT] Channel %lld recv i64: %lld\n", channel_handle, val);
    return val;
}

char* salivo_channel_peek(long long channel_handle) {
    salivo_channel_lock();
    if (channel_handle < 1 || channel_handle > MAX_CHANNELS || !g_channel_table[channel_handle].is_active) {
        salivo_channel_unlock();
        char* empty = (char*)malloc(1);
        empty[0] = '\0';
        return empty;
    }
    SalivoChannelSlot* chan = &g_channel_table[channel_handle];
    if (chan->count <= 0) {
        salivo_channel_unlock();
        char* empty = (char*)malloc(1);
        empty[0] = '\0';
        return empty;
    }
    if (chan->msg_types[chan->head] == 1) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", chan->int_msgs[chan->head]);
        salivo_channel_unlock();
        char* copy = (char*)malloc(strlen(buf) + 1);
        strcpy(copy, buf);
        return copy;
    }
    char* msg = chan->msgs[chan->head];
    salivo_channel_unlock();
    if (!msg) {
        char* empty = (char*)malloc(1);
        empty[0] = '\0';
        return empty;
    }
    char* copy = (char*)malloc(strlen(msg) + 1);
    strcpy(copy, msg);
    return copy;
}

long long salivo_channel_size(long long channel_handle) {
    salivo_channel_lock();
    if (channel_handle < 1 || channel_handle > MAX_CHANNELS || !g_channel_table[channel_handle].is_active) {
        salivo_channel_unlock();
        return 0;
    }
    long long res = (long long)g_channel_table[channel_handle].count;
    salivo_channel_unlock();
    return res;
}

long long salivo_channel_close(long long channel_handle) {
    salivo_channel_lock();
    if (channel_handle < 1 || channel_handle > MAX_CHANNELS || !g_channel_table[channel_handle].is_active) {
        salivo_channel_unlock();
        return -1;
    }
    SalivoChannelSlot* chan = &g_channel_table[channel_handle];
    while (chan->count > 0) {
        if (chan->msg_types[chan->head] == 0 && chan->msgs[chan->head]) {
            free(chan->msgs[chan->head]);
            chan->msgs[chan->head] = NULL;
        }
        chan->head = (chan->head + 1) % CHANNEL_CAP;
        chan->count--;
    }
    chan->is_active = 0;
    salivo_channel_unlock();
    SALIVO_LOG("[CHANNEL_RT] Closed channel %lld\n", channel_handle);
    return 0;
}

// --- REAL FILESYSTEM I/O RUNTIME ENGINE ---

#define MAX_FILES 4096

typedef struct {
    int id;
    int is_active;
    FILE* fp;
    char path[256];
} SalivoFileSlot;

static int g_next_file_id = 1;
static SalivoFileSlot g_file_table[MAX_FILES + 1];

long long salivo_c_file_open(const char* path, const char* mode) {
    if (!path) path = "";
    if (!mode) mode = "rb";
    for (int k = 0; k < MAX_FILES; k++) {
        int i = ((g_next_file_id + k) % MAX_FILES) + 1;
        if (!g_file_table[i].is_active) {
            g_next_file_id = i;
            FILE* f = fopen(path, mode);
            if (!f) return -1;
            g_file_table[i].id = i;
            g_file_table[i].is_active = 1;
            g_file_table[i].fp = f;
            strncpy(g_file_table[i].path, path, 255);
            g_file_table[i].path[255] = '\0';
            return (long long)i;
        }
    }
    return -1;
}

long long salivo_c_file_close(long long handle) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active) {
        return -1;
    }
    if (g_file_table[handle].fp) {
        fclose(g_file_table[handle].fp);
        g_file_table[handle].fp = NULL;
    }
    g_file_table[handle].is_active = 0;
    return 0;
}

long long salivo_c_file_is_open(long long handle) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) {
        return 0;
    }
    return 1;
}

long long salivo_c_file_read(long long handle, char* buf, long long max_bytes) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) {
        return -1;
    }
    if (!buf || max_bytes <= 0) return 0;
    size_t read_bytes = fread(buf, 1, (size_t)max_bytes, g_file_table[handle].fp);
    return (long long)read_bytes;
}

long long salivo_c_file_write(long long handle, const char* buf, long long count_bytes) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) {
        return -1;
    }
    if (!buf || count_bytes <= 0) return 0;
    size_t written = fwrite(buf, 1, (size_t)count_bytes, g_file_table[handle].fp);
    fflush(g_file_table[handle].fp);
    return (long long)written;
}

long long salivo_c_file_seek(long long handle, long long offset, long long whence) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) return -1;
    int origin = SEEK_SET;
    if (whence == 1) origin = SEEK_CUR;
    else if (whence == 2) origin = SEEK_END;
    return (fseek(g_file_table[handle].fp, (long)offset, origin) == 0) ? 0 : -1;
}

long long salivo_c_file_tell(long long handle) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) return -1;
    return (long long)ftell(g_file_table[handle].fp);
}

long long salivo_c_file_read_byte(long long handle) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) {
        return -1;
    }
    unsigned char b = 0;
    size_t r = fread(&b, 1, 1, g_file_table[handle].fp);
    if (r != 1) return -1;
    return (long long)b;
}

long long salivo_c_file_write_byte(long long handle, long long byte_val) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) {
        return -1;
    }
    unsigned char b = (unsigned char)(byte_val & 0xFF);
    size_t written = fwrite(&b, 1, 1, g_file_table[handle].fp);
    return (long long)written;
}

long long salivo_c_file_eof(long long handle) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) return 1;
    if (feof(g_file_table[handle].fp)) return 1;
    long cur = ftell(g_file_table[handle].fp);
    fseek(g_file_table[handle].fp, 0, SEEK_END);
    long end = ftell(g_file_table[handle].fp);
    fseek(g_file_table[handle].fp, cur, SEEK_SET);
    return (cur >= end) ? 1 : 0;
}

long long salivo_c_file_copy(const char* src_path, const char* dest_path) {
    if (!src_path || !dest_path) return -1;
    FILE* src = fopen(src_path, "rb");
    if (!src) return -1;
    FILE* dest = fopen(dest_path, "wb");
    if (!dest) { fclose(src); return -1; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, n, dest);
    }
    fclose(src);
    fclose(dest);
    return 0;
}

long long salivo_system(const char* cmd) {
    if (!cmd) return -1;
    return (long long)system(cmd);
}

/* ---- Parallel native compilation: split one LLVM module, run clang on every core ---- */
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

static long long shim_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (long long)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
#endif
}

static int shim_starts(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

/* Writes "declare <ret> @name(<param types>)" for a "define ... {" line. */
static void shim_write_decl(FILE* f, const char* def) {
    const char* at = strchr(def, '@');
    const char* lp = at ? strchr(at, '(') : NULL;
    if (!at || !lp) return;
    fputs("declare ", f);
    fwrite(def + 7, 1, (size_t)(lp + 1 - (def + 7)), f);
    const char* p = lp + 1;
    const char* start = p;
    int depth = 0, first = 1;
    for (;; p++) {
        char c = *p;
        if (c == '(' || c == '[' || c == '{' || c == '<') { depth++; continue; }
        if ((c == ')' || c == ']' || c == '}' || c == '>') && depth > 0) { depth--; continue; }
        if ((c == ',' || c == ')') && depth == 0 || c == 0) {
            const char* e = p;
            const char* b = start;
            while (b < e && *b == ' ') b++;
            while (e > b && e[-1] == ' ') e--;
            if (e > b) {
                /* drop a trailing "%name" parameter name */
                const char* last = e;
                while (last > b && last[-1] != ' ') last--;
                const char* te = (*last == '%' && last > b) ? last : e;
                while (te > b && te[-1] == ' ') te--;
                if (!first) fputs(", ", f);
                fwrite(b, 1, (size_t)(te - b), f);
                first = 0;
            }
            if (c != ',') break;
            start = p + 1;
        }
    }
    fputs(")\n", f);
}

/* Writes "@x = external hidden [thread_local] global <type>" for a global definition line. */
static void shim_write_extern(FILE* f, const char* g) {
    const char* eq = strstr(g, " = ");
    const char* kg = strstr(g, " global ");
    const char* kc = strstr(g, " constant ");
    const char* k = (kg && (!kc || kg < kc)) ? kg + 8 : (kc ? kc + 10 : NULL);
    if (!eq || !k) return;
    const char* e = k;
    int depth = 0;
    for (; *e; e++) {
        if (*e == '[' || *e == '{' || *e == '<') depth++;
        else if (*e == ']' || *e == '}' || *e == '>') { if (--depth == 0) { e++; break; } }
        else if (*e == ' ' && depth == 0) break;
    }
    const char* tl = strstr(g, "thread_local");
    fwrite(g, 1, (size_t)(eq - g), f);
    fputs(tl && tl < k ? " = external hidden thread_local global " : " = external hidden global ", f);
    fwrite(k, 1, (size_t)(e - k), f);
    fputc('\n', f);
}

/* Writes a whole-program module to <path>.p0.ll with every function except main internal. The
   optimizer then deletes the unused runtime-preamble functions up front instead of optimizing all
   of them, and inlines freely. Safe because the C runtime objects never reference IR-defined symbols. */
static int shim_internalize_ir(const char* path, const char* buf) {
    char name[4096];
    snprintf(name, sizeof name, "%s.p0.ll", path);
    FILE* out = fopen(name, "wb");
    if (!out) return 0;
    static const char* linkages[] = {"internal ", "private ", "linkonce", "weak", "available_externally ", "external "};
    for (const char* s = buf; *s;) {
        const char* nl = strchr(s, '\n');
        size_t n = nl ? (size_t)(nl - s + 1) : strlen(s);
        if (strncmp(s, "define ", 7) == 0) {
            int keep = 0;
            for (size_t i = 0; i < sizeof linkages / sizeof *linkages; i++)
                if (strncmp(s + 7, linkages[i], strlen(linkages[i])) == 0) keep = 1;
            const char* at = memchr(s, '@', n);
            if (at && (size_t)(s + n - at) > 6 && strncmp(at, "@main(", 6) == 0) keep = 1;
            if (!keep) fputs("define internal ", out), s += 7, n -= 7;
        }
        fwrite(s, 1, n, out);
        s += n;
    }
    return fclose(out) == 0;
}

/* Splits <path> into <path>.p<k>.ll parts for parallel compilation and returns the part count
   (0 = not split; parts <= 0 means one per CPU). Globals stay defined in part 0 with hidden
   linkage and are declared external elsewhere. Function bodies are balanced by size; each part
   declares the functions it does not own. Output is deterministic for identical input.
   A module kept whole (under 2 MB per part, where a part's clang process and ThinLTO work cost
   more than they save) becomes one internalized part instead, and the count is 1. */
long long salivo_split_ir_module(const char* path, long long parts) {
    if (parts <= 0) parts = shim_cpu_count();
    if (parts > 64) parts = 64;
    FILE* in = fopen(path, "rb");
    if (!in) return 0;
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    if (parts > size / (2 << 20)) parts = size / (2 << 20);
    fseek(in, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf || fread(buf, 1, (size_t)size, in) != (size_t)size) { fclose(in); free(buf); return 0; }
    fclose(in);
    buf[size] = 0;
    if (parts < 2) {
        int ok = shim_internalize_ir(path, buf);
        free(buf);
        return ok ? 1 : 0;
    }

    long long cap = 1;
    for (long i = 0; i < size; i++) if (buf[i] == '\n') cap++;
    char** lines = (char**)malloc(sizeof(char*) * (size_t)cap);
    long long n = 0;
    char* s = buf;
    for (long i = 0; i <= size; i++) {
        if (i == size || buf[i] == '\n') {
            buf[i] = 0;
            if (i > 0 && buf[i - 1] == '\r') buf[i - 1] = 0;
            lines[n++] = s;
            s = buf + i + 1;
        }
    }

    long long* fn_start = (long long*)malloc(sizeof(long long) * (size_t)n);
    long long* fn_end = (long long*)malloc(sizeof(long long) * (size_t)n);
    long long* fn_bytes = (long long*)malloc(sizeof(long long) * (size_t)n);
    int* owner = (int*)malloc(sizeof(int) * (size_t)n);
    long long nf = 0;
    for (long long i = 0; i < n; i++) {
        if (shim_starts(lines[i], "define ")) {
            long long j = i, bytes = 0;
            while (j < n && strcmp(lines[j], "}") != 0) { bytes += (long long)strlen(lines[j]) + 1; j++; }
            fn_start[nf] = i; fn_end[nf] = j; fn_bytes[nf] = bytes; nf++;
            i = j;
        }
    }
    /* Greedy balance: biggest function first onto the lightest part (stable, so deterministic) */
    long long* order = (long long*)malloc(sizeof(long long) * (size_t)(nf + 1));
    for (long long i = 0; i < nf; i++) order[i] = i;
    for (long long i = 1; i < nf; i++) {
        long long v = order[i], j = i;
        while (j > 0 && fn_bytes[order[j - 1]] < fn_bytes[v]) { order[j] = order[j - 1]; j--; }
        order[j] = v;
    }
    long long* load = (long long*)calloc((size_t)parts, sizeof(long long));
    for (long long i = 0; i < nf; i++) {
        long long best = 0;
        for (long long p = 1; p < parts; p++) if (load[p] < load[best]) best = p;
        owner[order[i]] = (int)best;
        load[best] += fn_bytes[order[i]];
    }

    char* name = (char*)malloc(strlen(path) + 32);
    long long ok = parts;
    for (long long p = 0; p < parts && ok; p++) {
        sprintf(name, "%s.p%lld.ll", path, p);
        FILE* f = fopen(name, "wb");
        if (!f) { ok = 0; break; }
        long long fi = 0;
        for (long long i = 0; i < n; i++) {
            const char* l = lines[i];
            if (fi < nf && i == fn_start[fi]) {
                if (owner[fi] == p) {
                    for (long long j = fn_start[fi]; j <= fn_end[fi] && j < n; j++) { fputs(lines[j], f); fputc('\n', f); }
                } else {
                    shim_write_decl(f, l);
                }
                i = fn_end[fi];
                fi++;
                continue;
            }
            if (l[0] == '@') {
                const char* eq = strstr(l, " = ");
                if (p != 0) {
                    shim_write_extern(f, l);
                } else if (eq && (shim_starts(eq + 3, "private ") || shim_starts(eq + 3, "internal "))) {
                    fwrite(l, 1, (size_t)(eq + 3 - l), f);
                    fputs("hidden ", f);
                    fputs(strchr(eq + 3, ' ') + 1, f);
                    fputc('\n', f);
                } else {
                    fputs(l, f);
                    fputc('\n', f);
                }
                continue;
            }
            fputs(l, f);
            fputc('\n', f);
        }
        fclose(f);
    }
    free(name); free(load); free(order); free(owner); free(fn_bytes); free(fn_end); free(fn_start); free(lines); free(buf);
    return ok;
}

/* Runs newline-separated commands concurrently; 0 when all succeed, else the first failing exit code. */
long long salivo_run_parallel(const char* cmds) {
    if (!cmds) return 0;
    char* all = strdup(cmds);
    char* list[256];
    long long count = 0;
    for (char* t = strtok(all, "\n"); t && count < 256; t = strtok(NULL, "\n")) if (*t) list[count++] = t;
    long long rc = 0;
#ifdef _WIN32
    HANDLE hs[256];
    for (long long i = 0; i < count; i++) {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        hs[i] = NULL;
        if (CreateProcessA(NULL, list[i], NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            hs[i] = pi.hProcess;
        } else if (rc == 0) {
            rc = 1;
        }
    }
    for (long long i = 0; i < count; i++) {
        if (!hs[i]) continue;
        DWORD code = 1;
        WaitForSingleObject(hs[i], INFINITE);
        GetExitCodeProcess(hs[i], &code);
        CloseHandle(hs[i]);
        if (code != 0 && rc == 0) rc = (long long)code;
    }
#else
    pid_t pids[256];
    for (long long i = 0; i < count; i++) {
        pids[i] = fork();
        if (pids[i] == 0) { execl("/bin/sh", "sh", "-c", list[i], (char*)NULL); _exit(127); }
    }
    for (long long i = 0; i < count; i++) {
        int st = 0;
        if (pids[i] > 0) waitpid(pids[i], &st, 0);
        long long code = (pids[i] > 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : 1;
        if (code != 0 && rc == 0) rc = code;
    }
#endif
    free(all);
    return rc;
}

char* salivo_c_file_read_string(long long handle) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) {
        char* empty = (char*)malloc(1);
        empty[0] = '\0';
        return empty;
    }
    FILE* fp = g_file_table[handle].fp;
    long current_pos = ftell(fp);
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp) - current_pos;
    if (size <= 0) size = 0;
    fseek(fp, current_pos, SEEK_SET);

    char* buf = (char*)malloc(size + 1);
    if (!buf) {
        char* empty = (char*)malloc(1);
        empty[0] = '\0';
        return empty;
    }
    size_t read_bytes = 0;
    if (size > 0) {
        read_bytes = fread(buf, 1, (size_t)size, fp);
    }
    buf[read_bytes] = '\0';
    return buf;
}

long long salivo_c_file_write_buf(long long handle, const char* buf, long long count_bytes) {
    return salivo_c_file_write(handle, buf, count_bytes);
}

long long salivo_c_file_sync(long long handle) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) return -1;
    FILE* fp = g_file_table[handle].fp;
    fflush(fp);
#ifdef _WIN32
    int fd = _fileno(fp);
    return (_commit(fd) == 0) ? 0 : -1;
#else
    int fd = fileno(fp);
    return (fsync(fd) == 0) ? 0 : -1;
#endif
}

long long salivo_c_file_truncate(long long handle, long long size) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) return -1;
    FILE* fp = g_file_table[handle].fp;
    fflush(fp);
#ifdef _WIN32
    int fd = _fileno(fp);
    return (_chsize(fd, (long)size) == 0) ? 0 : -1;
#else
    int fd = fileno(fp);
    return (ftruncate(fd, (off_t)size) == 0) ? 0 : -1;
#endif
}

long long salivo_c_file_metadata_size(long long handle) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) return 0;
    FILE* fp = g_file_table[handle].fp;
#ifdef _WIN32
    int fd = _fileno(fp);
    struct _stat64 st;
    if (_fstat64(fd, &st) == 0) return (long long)st.st_size;
#else
    int fd = fileno(fp);
    struct stat st;
    if (fstat(fd, &st) == 0) return (long long)st.st_size;
#endif
    return 0;
}

long long salivo_c_file_metadata_mtime(long long handle) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) return 0;
    FILE* fp = g_file_table[handle].fp;
#ifdef _WIN32
    int fd = _fileno(fp);
    struct _stat64 st;
    if (_fstat64(fd, &st) == 0) return (long long)st.st_mtime;
#else
    int fd = fileno(fp);
    struct stat st;
    if (fstat(fd, &st) == 0) return (long long)st.st_mtime;
#endif
    return 0;
}

long long salivo_c_file_metadata_ctime(long long handle) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) return 0;
    FILE* fp = g_file_table[handle].fp;
#ifdef _WIN32
    int fd = _fileno(fp);
    struct _stat64 st;
    if (_fstat64(fd, &st) == 0) return (long long)st.st_ctime;
#else
    int fd = fileno(fp);
    struct stat st;
    if (fstat(fd, &st) == 0) return (long long)st.st_ctime;
#endif
    return 0;
}

long long salivo_c_file_metadata_permissions(long long handle) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) return 0;
    FILE* fp = g_file_table[handle].fp;
#ifdef _WIN32
    int fd = _fileno(fp);
    struct _stat64 st;
    if (_fstat64(fd, &st) == 0) return (long long)(st.st_mode & 0777);
#else
    int fd = fileno(fp);
    struct stat st;
    if (fstat(fd, &st) == 0) return (long long)(st.st_mode & 0777);
#endif
    return 0;
}

long long salivo_c_file_metadata_type(long long handle) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) return 0;
    FILE* fp = g_file_table[handle].fp;
#ifdef _WIN32
    int fd = _fileno(fp);
    struct _stat64 st;
    if (_fstat64(fd, &st) == 0) {
        if ((st.st_mode & _S_IFDIR) == _S_IFDIR) return 2;
        if ((st.st_mode & _S_IFREG) == _S_IFREG) return 1;
    }
#else
    int fd = fileno(fp);
    struct stat st;
    if (fstat(fd, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 2;
        if (S_ISREG(st.st_mode)) return 1;
    }
#endif
    return 0;
}

long long salivo_c_file_metadata_is_dir(long long handle) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) return 0;
    FILE* fp = g_file_table[handle].fp;
#ifdef _WIN32
    int fd = _fileno(fp);
    struct _stat64 st;
    if (_fstat64(fd, &st) == 0) return (st.st_mode & _S_IFDIR) ? 1 : 0;
#else
    int fd = fileno(fp);
    struct stat st;
    if (fstat(fd, &st) == 0) return S_ISDIR(st.st_mode) ? 1 : 0;
#endif
    return 0;
}

long long salivo_c_dir_list(const char* path) {
    if (!path) return 0;
    int count = 0;
#ifdef _WIN32
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search_path, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            count++;
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
#else
    DIR* d = opendir(path);
    if (d) {
        struct dirent* dir;
        while ((dir = readdir(d)) != NULL) {
            count++;
        }
        closedir(d);
    }
#endif
    return (long long)count;
}

char* salivo_c_dir_list_string(const char* path) {
    if (!path || path[0] == '\0') path = ".";
    size_t cap = 1024;
    size_t len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return "";
    buf[0] = '\0';

#ifdef _WIN32
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search_path, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") != 0 && strcmp(fd.cFileName, "..") != 0) {
                size_t nlen = strlen(fd.cFileName);
                while (len + nlen + 2 > cap) {
                    cap *= 2;
                    buf = (char*)realloc(buf, cap);
                }
                if (len > 0) {
                    buf[len++] = '\n';
                }
                memcpy(buf + len, fd.cFileName, nlen);
                len += nlen;
                buf[len] = '\0';
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
#else
    DIR* d = opendir(path);
    if (d) {
        struct dirent* dir;
        while ((dir = readdir(d)) != NULL) {
            if (strcmp(dir->d_name, ".") != 0 && strcmp(dir->d_name, "..") != 0) {
                size_t nlen = strlen(dir->d_name);
                while (len + nlen + 2 > cap) {
                    cap *= 2;
                    buf = (char*)realloc(buf, cap);
                }
                if (len > 0) {
                    buf[len++] = '\n';
                }
                memcpy(buf + len, dir->d_name, nlen);
                len += nlen;
                buf[len] = '\0';
            }
        }
        closedir(d);
    }
#endif
    return buf;
}

long long salivo_channel_is_closed(long long channel_handle) {
    if (channel_handle < 1 || channel_handle > MAX_CHANNELS || !g_channel_table[channel_handle].is_active) {
        return 1;
    }
    return 0;
}
// --- REAL MEMORY ALLOCATOR & MEMORY RUNTIME (WITH BOUNDS VALIDATION) ---

#define SALIVO_MEM_MAGIC 0x53414C49564F4D45ULL // "SALIVOME"

typedef struct {
    void* raw_ptr;
    void* user_ptr;
    size_t size;
    size_t alignment;
    unsigned long long magic;
    unsigned long long reserved; // Struct size = 48 bytes (16-byte aligned)
} SalivoMemHeader;

static inline SalivoMemHeader* salivo_get_valid_header(long long addr) {
    if (addr == 0 || (uintptr_t)addr < sizeof(SalivoMemHeader)) return NULL;
    SalivoMemHeader* hdr = (SalivoMemHeader*)((uintptr_t)addr - sizeof(SalivoMemHeader));
    if (hdr->magic == SALIVO_MEM_MAGIC && hdr->user_ptr == (void*)(uintptr_t)addr) {
        return hdr;
    }
    return NULL;
}

static volatile long long salivo_active_allocations = 0;

long long salivo_get_active_allocations(void) {
    return salivo_active_allocations;
}

long long salivo_alloc(long long size_bytes, long long alignment) {
    salivo_stat(3, size_bytes);
    if (size_bytes <= 0) {
        return 0;
    }
    if (alignment < 8) {
        alignment = 8;
    }
    size_t header_size = sizeof(SalivoMemHeader);

    // Fast path: standard alignment (<= 16 bytes)
    // On 64-bit systems, malloc returns 16-byte aligned memory.
    if (alignment <= 16) {
        void* raw = malloc(header_size + (size_t)size_bytes);
        if (!raw) return 0;
        uintptr_t user_addr = (uintptr_t)raw + header_size;
        SalivoMemHeader* hdr = (SalivoMemHeader*)raw;
        hdr->raw_ptr = raw;
        hdr->user_ptr = (void*)user_addr;
        hdr->size = (size_t)size_bytes;
        hdr->alignment = (size_t)alignment;
        hdr->magic = SALIVO_MEM_MAGIC;
        SALIVO_COUNTER_ADD(salivo_active_allocations, 1);
        return (long long)user_addr;
    }

    // Extended path: alignment > 16 (e.g. 32, 64-byte AVX/cacheline alignment)
    size_t align = (size_t)alignment;
    if ((align & (align - 1)) != 0) {
        size_t a = 1;
        while (a < align) a <<= 1;
        align = a;
    }
    if ((size_t)size_bytes > ((size_t)-1) - (header_size + align + 64)) {
        return 0;
    }
    size_t total = header_size + align + (size_t)size_bytes;
    void* raw = malloc(total);
    if (!raw) {
        return 0;
    }
    uintptr_t raw_addr = (uintptr_t)raw;
    uintptr_t user_addr = (raw_addr + header_size + align - 1) & ~((uintptr_t)align - 1);
    
    SalivoMemHeader* hdr = (SalivoMemHeader*)(user_addr - header_size);
    hdr->raw_ptr = raw;
    hdr->user_ptr = (void*)user_addr;
    hdr->size = (size_t)size_bytes;
    hdr->alignment = align;
    hdr->magic = SALIVO_MEM_MAGIC;
    
    SALIVO_COUNTER_ADD(salivo_active_allocations, 1);
    return (long long)user_addr;
}

long long salivo_alloc_zeroed(long long size_bytes, long long alignment) {
    long long addr = salivo_alloc(size_bytes, alignment);
    if (addr != 0) {
        memset((void*)(uintptr_t)addr, 0, (size_t)size_bytes);
    }
    return addr;
}

void salivo_deallocate(long long addr, long long size_bytes, long long alignment) {
    (void)size_bytes;
    (void)alignment;
    if (addr == 0) {
        return;
    }
    SalivoMemHeader* hdr = salivo_get_valid_header(addr);
    if (hdr) {
        hdr->magic = 0; // Invalidate magic on deallocation
        free(hdr->raw_ptr);
        SALIVO_COUNTER_ADD(salivo_active_allocations, -1);
    }
}

static volatile long long salivo_fmt_temp_free_counter = 0;

void salivo_free_format_temp(void* ptr) {
    if (!ptr) {
        return;
    }
    SalivoMemHeader* hdr = salivo_get_valid_header((long long)(uintptr_t)ptr);
    if (hdr) {
        hdr->magic = 0;
        free(hdr->raw_ptr);
        salivo_fmt_temp_free_counter++;
    }
}

long long salivo_get_format_temp_free_count(void) {
    return salivo_fmt_temp_free_counter;
}

long long salivo_checked_add_len(long long a, long long b) {
    if (b < 0 || a < 0) {
        rtpanic("runtime panic: formatted string length negative");
    }
    unsigned long long ua = (unsigned long long)a;
    unsigned long long ub = (unsigned long long)b;
    unsigned long long sum = ua + ub;
    if (sum < ua || sum > (unsigned long long)LLONG_MAX - 1) {
        rtpanic("runtime panic: formatted string allocation overflow");
    }
    return (long long)sum;
}

long long salivo_alloc_format_buffer(long long total_len) {
    if (total_len < 0 || (unsigned long long)total_len >= (unsigned long long)LLONG_MAX - 1) {
        rtpanic("runtime panic: formatted string allocation overflow");
    }
    return salivo_alloc(total_len + 1, 1);
}

long long salivo_is_allocated(long long addr) {
    SalivoMemHeader* hdr = salivo_get_valid_header(addr);
    return hdr != NULL ? 1 : 0;
}

long long salivo_reallocate(long long addr, long long old_size, long long new_size, long long alignment) {
    if (addr == 0) {
        return salivo_alloc(new_size, alignment);
    }
    if (new_size <= 0) {
        salivo_deallocate(addr, old_size, alignment);
        return 0;
    }
    SalivoMemHeader* old_hdr = salivo_get_valid_header(addr);
    if (old_hdr && old_hdr->size >= (size_t)new_size) {
        // In-place buffer reuse: avoid any heap allocation or copy
        old_hdr->size = (size_t)new_size;
        return addr;
    }

    size_t actual_old_size = old_hdr ? old_hdr->size : (size_t)old_size;
    long long new_addr = salivo_alloc(new_size, alignment);
    if (new_addr == 0) {
        return 0;
    }
    size_t copy_size = (actual_old_size < (size_t)new_size) ? actual_old_size : (size_t)new_size;
    if (copy_size > 0) {
        memcpy((void*)(uintptr_t)new_addr, (void*)(uintptr_t)addr, copy_size);
    }
    salivo_deallocate(addr, old_size, alignment);
    return new_addr;
}

long long salivo_mem_read_byte(long long addr, long long offset) {
    if (!addr || offset < 0) return 0;
    SalivoMemHeader* hdr = salivo_get_valid_header(addr);
    if (hdr && (size_t)offset + 1 > hdr->size) {
        SALIVO_LOG("[MEM_SAFETY] Read byte out of bounds: offset=%lld, alloc_size=%zu\n", offset, hdr->size);
        return 0;
    }
    const unsigned char* ptr = (const unsigned char*)(uintptr_t)addr;
    return (long long)ptr[offset];
}

void salivo_mem_write_byte(long long addr, long long offset, long long val) {
    if (!addr || offset < 0) return;
    SalivoMemHeader* hdr = salivo_get_valid_header(addr);
    if (hdr && (size_t)offset + 1 > hdr->size) {
        SALIVO_LOG("[MEM_SAFETY] Write byte out of bounds: offset=%lld, alloc_size=%zu\n", offset, hdr->size);
        return;
    }
    unsigned char* ptr = (unsigned char*)(uintptr_t)addr;
    ptr[offset] = (unsigned char)val;
}

long long salivo_hash_bytes(long long addr, long long len) {
    if (!addr || len <= 0) return 0;
    const unsigned char* ptr = (const unsigned char*)(uintptr_t)addr;
    uint64_t h = 14695981039346656037ULL;
    for (long long i = 0; i < len; i++) {
        h ^= (uint64_t)ptr[i];
        h *= 1099511628211ULL;
    }
    long long res = (long long)(h & 0x7FFFFFFFFFFFFFFFULL);
    return res;
}

long long salivo_mem_read_i64(long long addr, long long offset) {
    if (!addr || offset < 0) return 0;
    SalivoMemHeader* hdr = salivo_get_valid_header(addr);
    if (hdr && (size_t)offset + 8 > hdr->size) {
        SALIVO_LOG("[MEM_SAFETY] Read i64 out of bounds: offset=%lld, alloc_size=%zu\n", offset, hdr->size);
        return 0;
    }
    const long long* ptr = (const long long*)((const unsigned char*)(uintptr_t)addr + offset);
    return *ptr;
}

void salivo_mem_write_i64(long long addr, long long offset, long long val) {
    if (!addr || offset < 0) return;
    SalivoMemHeader* hdr = salivo_get_valid_header(addr);
    if (hdr && (size_t)offset + 8 > hdr->size) {
        SALIVO_LOG("[MEM_SAFETY] Write i64 out of bounds: offset=%lld, alloc_size=%zu\n", offset, hdr->size);
        return;
    }
    long long* ptr = (long long*)((unsigned char*)(uintptr_t)addr + offset);
    *ptr = val;
}

void rtmemcopy(long long src_addr, long long dest_addr, long long count_bytes) {
    if (!src_addr || !dest_addr || count_bytes <= 0) return;
    memcpy((void*)(uintptr_t)dest_addr, (void*)(uintptr_t)src_addr, (size_t)count_bytes);
}

void rtmemmove(long long src_addr, long long dest_addr, long long count_bytes) {
    if (!src_addr || !dest_addr || count_bytes <= 0) return;
    memmove((void*)(uintptr_t)dest_addr, (void*)(uintptr_t)src_addr, (size_t)count_bytes);
}

void rtmemfill(long long dest_addr, long long val, long long count_bytes) {
    if (!dest_addr || count_bytes <= 0) return;
    memset((void*)(uintptr_t)dest_addr, (int)(val & 0xFF), (size_t)count_bytes);
}

void salivo_free(void* ptr) {
    if (ptr) {
        salivo_deallocate((long long)(uintptr_t)ptr, 0, 8);
    }
}

void salivo_arc_retain(void* ptr) {
    if (!ptr) return;
    long long* counts = (long long*)ptr;
#ifdef _WIN32
    InterlockedIncrement64(counts);
#else
    __atomic_add_fetch(counts, 1, __ATOMIC_SEQ_CST);
#endif
}

void salivo_arc_release(void* ptr) {
    if (!ptr) return;
    long long* counts = (long long*)ptr;
#ifdef _WIN32
    long long remaining = InterlockedDecrement64(counts);
#else
    long long remaining = __atomic_sub_fetch(counts, 1, __ATOMIC_SEQ_CST);
#endif
    if (remaining <= 0) {
        salivo_deallocate((long long)(uintptr_t)ptr, 0, 8);
    }
}

/* Forward declaration — defined below, needed for PRNG auto-seeding */
long long salivo_clock_nanos(void);

static unsigned long long g_salivo_prng_seed = 0;
static int g_salivo_prng_seeded = 0;

long long salivo_random_int(void) {
    if (!g_salivo_prng_seeded) {
        /* Seed from high-resolution clock + process ID for per-process entropy */
        unsigned long long t = (unsigned long long)salivo_clock_nanos();
#ifdef _WIN32
        unsigned long long pid = (unsigned long long)GetCurrentProcessId();
#else
        unsigned long long pid = (unsigned long long)getpid();
#endif
        g_salivo_prng_seed = (t ^ (pid << 16)) | 1ULL; /* ensure non-zero */
        g_salivo_prng_seeded = 1;
    }
    g_salivo_prng_seed = (g_salivo_prng_seed * 1103515245ULL + 12345ULL) & 0x7FFFFFFFULL;
    return (long long)g_salivo_prng_seed;
}

long long salivo_clock_nanos(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (long long)((count.QuadPart * 1000000000ULL) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

void salivo_sleep_nanos(long long nanos) {
    SALIVO_BLOCKING("sleep");
    if (nanos <= 0) return;
#ifdef _WIN32
    DWORD ms = (DWORD)(nanos / 1000000LL);
    if (ms == 0) ms = 1;
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = nanos / 1000000000LL;
    ts.tv_nsec = nanos % 1000000000LL;
    nanosleep(&ts, NULL);
#endif
}

char* salivo_process_cwd(void) {
    char* buf = (char*)malloc(1024);
    if (!buf) return "";
#ifdef _WIN32
    if (_getcwd(buf, 1024)) return buf;
#else
    if (getcwd(buf, 1024)) return buf;
#endif
    buf[0] = '\0';
    return buf;
}

long long salivo_c_file_exists(const char* path) {
    if (!path || strlen(path) == 0) return 0;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) == 0) {
        return (st.st_mode & _S_IFREG) ? 1 : 0;
    }
#else
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISREG(st.st_mode) ? 1 : 0;
    }
#endif
    return 0;
}

typedef struct {
    long long id;
    int is_active;
    long long expire_time_ms;
} SalivoTimerSlot;

static SalivoTimerSlot g_timer_table[1024];
static long long g_timer_count = 0;

long long salivo_timer_create(long long delay_ms) {
    long long id = ++g_timer_count;
    if (id < 1024) {
        g_timer_table[id].id = id;
        g_timer_table[id].is_active = 1;
        g_timer_table[id].expire_time_ms = salivo_time_now_ms() + delay_ms;
    }
    return id;
}

long long salivo_timer_is_active(long long timer_id) {
    if (timer_id < 1 || timer_id >= 1024) return 0;
    return g_timer_table[timer_id].is_active ? 1 : 0;
}

long long salivo_timer_cancel(long long timer_id) {
    if (timer_id < 1 || timer_id >= 1024) return 0;
    if (!g_timer_table[timer_id].is_active) return 0;
    g_timer_table[timer_id].is_active = 0;
    return 1;
}

long long salivo_runtime_process_timers(void) {
    long long min_expire = 0;
    int min_idx = -1;
    for (int i = 1; i <= g_timer_count && i < 1024; i++) {
        if (g_timer_table[i].is_active) {
            if (min_idx == -1 || g_timer_table[i].expire_time_ms < min_expire) {
                min_expire = g_timer_table[i].expire_time_ms;
                min_idx = i;
            }
        }
    }

    if (min_idx != -1) {
        long long now = salivo_time_now_ms();
        while (min_expire + 10 > now) {
            long long sleep_amt = min_expire + 10 - now;
            salivo_sleep_ms((int)sleep_amt);
            now = salivo_time_now_ms();
        }
        g_timer_table[min_idx].is_active = 0;
        return 1;
    }
    return 0;
}

long long salivo_c_dir_exists(const char* path) {
    if (!path || strlen(path) == 0) return 0;
#ifdef _WIN32
    DWORD dwAttrib = GetFileAttributesA(path);
    long long res = 0;
    if (dwAttrib != INVALID_FILE_ATTRIBUTES && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
        res = 1;
    }
    SALIVO_LOG("[FS_RT] dir_exists path='%s' dwAttrib=0x%lx res=%lld\n", path, dwAttrib, res);
    return res;
#else
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 1 : 0;
    }
    return 0;
#endif
}

long long salivo_c_dir_create(const char* path) {
    if (!path || strlen(path) == 0) return -1;
#ifdef _WIN32
    SALIVO_LOG("[FS_RT] dir_create path='%s'\n", path);
    if (CreateDirectoryA(path, NULL)) return 0;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    return _mkdir(path) == 0 ? 0 : -1;
#else
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return 0;
    return -1;
#endif
}

long long salivo_c_dir_remove(const char* path) {
    if (!path || strlen(path) == 0) return -1;
#ifdef _WIN32
    return RemoveDirectoryA(path) ? 0 : (_rmdir(path) == 0 ? 0 : -1);
#else
    return rmdir(path) == 0 ? 0 : -1;
#endif
}

long long salivo_c_file_remove(const char* path) {
    if (!path || strlen(path) == 0) return -1;
    return remove(path) == 0 ? 0 : -1;
}

char* rtintstr(long long val) {
    salivo_stat(2, 32);
    char* buf = (char*)malloc(32);
    if (!buf) return "";
    snprintf(buf, 32, "%lld", val);
    return buf;
}

long long rtstrint(const char* s) {
    if (!s) return 0;
    return atoll(s);
}

double rtstrfloat(const char* s) {
    if (!s) return 0.0;
    return atof(s);
}

char* rtfloatstr(double val) {
    char* buf = (char*)malloc(64);
    if (!buf) return "";
    snprintf(buf, 64, "%f", val);
    return buf;
}

char* rtboolstr(long long val) {
    char* buf = (char*)malloc(6);
    if (!buf) return "";
    strcpy(buf, val ? "true" : "false");
    return buf;
}

char* rtcharstr(long long val) {
    char* buf = (char*)malloc(5);
    if (!buf) return "";
    if (val < 0x80) {
        buf[0] = (char)val;
        buf[1] = '\0';
    } else if (val < 0x800) {
        buf[0] = (char)(0xC0 | (val >> 6));
        buf[1] = (char)(0x80 | (val & 0x3F));
        buf[2] = '\0';
    } else if (val < 0x10000) {
        buf[0] = (char)(0xE0 | (val >> 12));
        buf[1] = (char)(0x80 | ((val >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (val & 0x3F));
        buf[3] = '\0';
    } else {
        buf[0] = (char)(0xF0 | (val >> 18));
        buf[1] = (char)(0x80 | ((val >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((val >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (val & 0x3F));
        buf[4] = '\0';
    }
    return buf;
}

char* rtdebugchar(long long val) {
    char* s = rtcharstr(val);
    char* buf = (char*)malloc(strlen(s) + 3);
    if (!buf) return "''";
    snprintf(buf, strlen(s) + 3, "'%s'", s);
    free(s);
    return buf;
}

char* rtidentitystr(const char* s) {
    if (!s) return "";
    size_t len = strlen(s);
    char* buf = (char*)malloc(len + 1);
    if (!buf) return "";
    memcpy(buf, s, len + 1);
    return buf;
}

char* rtreadline(void) {
    fflush(stdout);
    char temp[2048];
    if (fgets(temp, sizeof(temp), stdin) == NULL) {
        char* empty = (char*)malloc(1);
        if (empty) empty[0] = '\0';
        return empty ? empty : "";
    }
    size_t len = strlen(temp);
    while (len > 0 && (temp[len - 1] == '\n' || temp[len - 1] == '\r')) {
        temp[len - 1] = '\0';
        len--;
    }
    char* res = (char*)malloc(len + 1);
    if (!res) return "";
    strcpy(res, temp);
    return res;
}

long long rtreadint(void) {
    char* line = rtreadline();
    long long val = rtstrint(line);
    free(line);
    return val;
}

double rtreadfloat(void) {
    char* line = rtreadline();
    double val = atof(line);
    free(line);
    return val;
}

long long salivo_c_file_flush(long long handle) {
    if (handle < 1 || handle > MAX_FILES || !g_file_table[handle].is_active || !g_file_table[handle].fp) {
        return -1;
    }
    return (fflush(g_file_table[handle].fp) == 0) ? 0 : -1;
}

long long salivo_c_dir_change_directory(const char* path) {
    if (!path || strlen(path) == 0) return -1;
#ifdef _WIN32
    return SetCurrentDirectoryA(path) ? 0 : -1;
#else
    return chdir(path) == 0 ? 0 : -1;
#endif
}

long long salivo_c_file_size(const char* path) {
    if (!path || strlen(path) == 0) return 0;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) == 0) return (long long)st.st_size;
#else
    struct stat st;
    if (stat(path, &st) == 0) return (long long)st.st_size;
#endif
    return 0;
}



#define MAX_MUTEXES 1024
#define MAX_ATOMICS 1024

#ifdef _WIN32
typedef CRITICAL_SECTION salivo_mutex_t;
#else
#include <pthread.h>
typedef pthread_mutex_t salivo_mutex_t;
#endif

typedef struct {
    salivo_mutex_t handle;
    int is_active;
    int is_locked;
    long long owner_thread_id;
} SalivoMutexSlot;

static SalivoMutexSlot g_mutex_table[MAX_MUTEXES + 1];

long long salivo_mutex_create(void) {
    for (int i = 1; i <= MAX_MUTEXES; i++) {
        if (!g_mutex_table[i].is_active) {
#ifdef _WIN32
            InitializeCriticalSection(&g_mutex_table[i].handle);
#else
            pthread_mutex_init(&g_mutex_table[i].handle, NULL);
#endif
            g_mutex_table[i].is_active = 1;
            g_mutex_table[i].is_locked = 0;
            g_mutex_table[i].owner_thread_id = 0;
            return (long long)i;
        }
    }
    return -1;
}

long long salivo_mutex_lock(long long handle) {
    SALIVO_BLOCKING("mutex_lock");
    if (handle < 1 || handle > MAX_MUTEXES || !g_mutex_table[handle].is_active) return -1;
#ifdef _WIN32
    EnterCriticalSection(&g_mutex_table[handle].handle);
    g_mutex_table[handle].is_locked = 1;
    g_mutex_table[handle].owner_thread_id = (long long)GetCurrentThreadId();
#else
    pthread_mutex_lock(&g_mutex_table[handle].handle);
    g_mutex_table[handle].is_locked = 1;
    g_mutex_table[handle].owner_thread_id = (long long)pthread_self();
#endif
    return 0;
}

long long salivo_mutex_try_lock(long long handle) {
    if (handle < 1 || handle > MAX_MUTEXES || !g_mutex_table[handle].is_active) return 0;
    if (g_mutex_table[handle].is_locked) return 0;
#ifdef _WIN32
    if (TryEnterCriticalSection(&g_mutex_table[handle].handle)) {
        g_mutex_table[handle].is_locked = 1;
        g_mutex_table[handle].owner_thread_id = (long long)GetCurrentThreadId();
        return 1;
    }
    return 0;
#else
    if (pthread_mutex_trylock(&g_mutex_table[handle].handle) == 0) {
        g_mutex_table[handle].is_locked = 1;
        g_mutex_table[handle].owner_thread_id = (long long)pthread_self();
        return 1;
    }
    return 0;
#endif
}

long long salivo_mutex_unlock(long long handle) {
    if (handle < 1 || handle > MAX_MUTEXES || !g_mutex_table[handle].is_active) return -1;
    g_mutex_table[handle].is_locked = 0;
    g_mutex_table[handle].owner_thread_id = 0;
#ifdef _WIN32
    LeaveCriticalSection(&g_mutex_table[handle].handle);
#else
    pthread_mutex_unlock(&g_mutex_table[handle].handle);
#endif
    return 0;
}

long long salivo_mutex_is_locked(long long handle) {
    if (handle < 1 || handle > MAX_MUTEXES || !g_mutex_table[handle].is_active) return 0;
    return (long long)g_mutex_table[handle].is_locked;
}

long long salivo_mutex_owner(long long handle) {
    if (handle < 1 || handle > MAX_MUTEXES || !g_mutex_table[handle].is_active) return 0;
    return g_mutex_table[handle].owner_thread_id;
}

typedef struct {
    long long value;
    int is_active;
} SalivoAtomicSlot;

static SalivoAtomicSlot g_atomic_table[MAX_ATOMICS + 1];

long long salivo_atomic_int_create(long long initial) {
    for (int i = 1; i <= MAX_ATOMICS; i++) {
        if (!g_atomic_table[i].is_active) {
            g_atomic_table[i].value = initial;
            g_atomic_table[i].is_active = 1;
            return (long long)i;
        }
    }
    return -1;
}

long long salivo_atomic_int_load(long long handle) {
    if (handle < 1 || handle > MAX_ATOMICS || !g_atomic_table[handle].is_active) return 0;
#ifdef _MSC_VER
    return _InterlockedCompareExchange64(&g_atomic_table[handle].value, 0, 0);
#else
    return (long long)__atomic_load_n(&g_atomic_table[handle].value, __ATOMIC_SEQ_CST);
#endif
}

void salivo_atomic_int_store(long long handle, long long val) {
    if (handle < 1 || handle > MAX_ATOMICS || !g_atomic_table[handle].is_active) return;
#ifdef _MSC_VER
    _InterlockedExchange64(&g_atomic_table[handle].value, val);
#else
    __atomic_store_n(&g_atomic_table[handle].value, val, __ATOMIC_SEQ_CST);
#endif
}

long long salivo_atomic_int_fetch_add(long long handle, long long val) {
    if (handle < 1 || handle > MAX_ATOMICS || !g_atomic_table[handle].is_active) return 0;
#ifdef _MSC_VER
    return (long long)_InterlockedExchangeAdd64((__int64 volatile*)&g_atomic_table[handle].value, (__int64)val);
#else
    return (long long)__atomic_fetch_add(&g_atomic_table[handle].value, val, __ATOMIC_SEQ_CST);
#endif
}

long long salivo_atomic_int_swap(long long handle, long long val) {
    if (handle < 1 || handle > MAX_ATOMICS || !g_atomic_table[handle].is_active) return 0;
#ifdef _MSC_VER
    return _InterlockedExchange64(&g_atomic_table[handle].value, val);
#else
    return (long long)__atomic_exchange_n(&g_atomic_table[handle].value, val, __ATOMIC_SEQ_CST);
#endif
}

long long salivo_atomic_int_compare_exchange(long long handle, long long expected, long long desired) {
    if (handle < 1 || handle > MAX_ATOMICS || !g_atomic_table[handle].is_active) return 0;
#ifdef _MSC_VER
    long long old = _InterlockedCompareExchange64(&g_atomic_table[handle].value, desired, expected);
    return (old == expected) ? 1 : 0;
#else
    long long exp_copy = expected;
    int success = __atomic_compare_exchange_n(
        &g_atomic_table[handle].value, &exp_copy, desired,
        0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return success ? 1 : 0;
#endif
}

long long salivo_atomic_int_fetch_sub(long long handle, long long val) {
    if (handle < 1 || handle > MAX_ATOMICS || !g_atomic_table[handle].is_active) return 0;
#ifdef _MSC_VER
    return (long long)_InterlockedExchangeAdd64((__int64 volatile*)&g_atomic_table[handle].value, (__int64)(-val));
#else
    return (long long)__atomic_fetch_sub(&g_atomic_table[handle].value, val, __ATOMIC_SEQ_CST);
#endif
}

long long salivo_atomic_bool_create(long long initial) {
    return salivo_atomic_int_create(initial ? 1 : 0);
}

long long salivo_atomic_bool_load(long long handle) {
    return salivo_atomic_int_load(handle) ? 1 : 0;
}

void salivo_atomic_bool_store(long long handle, long long val) {
    salivo_atomic_int_store(handle, val ? 1 : 0);
}

long long salivo_atomic_bool_swap(long long handle, long long val) {
    return salivo_atomic_int_swap(handle, val ? 1 : 0) ? 1 : 0;
}

char* salivo_channel_try_recv(long long channel_handle) {
    SALIVO_LOG("[CHANNEL_RT] salivo_channel_try_recv called with handle=%lld\n", channel_handle);
    salivo_channel_lock();
    if (channel_handle < 1 || channel_handle > MAX_CHANNELS || !g_channel_table[channel_handle].is_active) {
        salivo_channel_unlock();
        char* empty = (char*)malloc(1);
        empty[0] = '\0';
        return empty;
    }
    SalivoChannelSlot* chan = &g_channel_table[channel_handle];
    SALIVO_LOG("[CHANNEL_RT] Channel %lld count=%d head=%d tail=%d\n", channel_handle, chan->count, chan->head, chan->tail);
    if (chan->count <= 0) {
        salivo_channel_unlock();
        char* empty = (char*)malloc(1);
        empty[0] = '\0';
        return empty;
    }
    char* msg = chan->msgs[chan->head];
    chan->head = (chan->head + 1) % CHANNEL_CAP;
    chan->count--;
    salivo_channel_unlock();
    SALIVO_LOG("[CHANNEL_RT] Channel %lld try_recv msg: '%s' (remaining count=%d)\n", channel_handle, msg, chan->count);
    return msg;
}

// =============================================================================
// CRYPTO ENGINE (SHA-256 NIST FIPS 180-4, AES-256 FIPS 197, OS Secure Random)
// =============================================================================

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} SalivoSha256;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHA256_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_SIG0(x) (SHA256_ROTR(x, 2) ^ SHA256_ROTR(x, 13) ^ SHA256_ROTR(x, 22))
#define SHA256_SIG1(x) (SHA256_ROTR(x, 6) ^ SHA256_ROTR(x, 11) ^ SHA256_ROTR(x, 25))
#define SHA256_sig0(x) (SHA256_ROTR(x, 7) ^ SHA256_ROTR(x, 18) ^ ((x) >> 3))
#define SHA256_sig1(x) (SHA256_ROTR(x, 17) ^ SHA256_ROTR(x, 19) ^ ((x) >> 10))

static void salivo_sha256_transform(SalivoSha256* ctx, const uint8_t* data) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) | ((uint32_t)data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        w[i] = SHA256_sig1(w[i - 2]) + w[i - 7] + SHA256_sig0(w[i - 15]) + w[i - 16];
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + SHA256_SIG1(e) + SHA256_CH(e, f, g) + K256[i] + w[i];
        uint32_t t2 = SHA256_SIG0(a) + SHA256_MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void salivo_sha256_init(SalivoSha256* ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

void salivo_sha256_update(SalivoSha256* ctx, const uint8_t* data, size_t len) {
    size_t buffer_idx = (size_t)(ctx->count & 63);
    ctx->count += len;
    size_t i = 0;
    if (buffer_idx > 0) {
        size_t needed = 64 - buffer_idx;
        if (len < needed) {
            memcpy(&ctx->buffer[buffer_idx], data, len);
            return;
        }
        memcpy(&ctx->buffer[buffer_idx], data, needed);
        salivo_sha256_transform(ctx, ctx->buffer);
        i += needed;
    }
    for (; i + 64 <= len; i += 64) {
        salivo_sha256_transform(ctx, &data[i]);
    }
    if (i < len) {
        memcpy(ctx->buffer, &data[i], len - i);
    }
}

void salivo_sha256_final(SalivoSha256* ctx, uint8_t* digest) {
    uint8_t pad[64];
    memset(pad, 0, 64);
    pad[0] = 0x80;
    uint64_t total_bits = ctx->count * 8;
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++) {
        len_bytes[7 - i] = (uint8_t)(total_bits >> (i * 8));
    }
    size_t buffer_idx = (size_t)(ctx->count & 63);
    size_t pad_len = (buffer_idx < 56) ? (56 - buffer_idx) : (120 - buffer_idx);
    salivo_sha256_update(ctx, pad, pad_len);
    salivo_sha256_update(ctx, len_bytes, 8);
    for (int i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

char* salivo_crypto_sha256_str(const char* s) {
    if (!s) s = "";
    SalivoSha256 ctx;
    salivo_sha256_init(&ctx);
    salivo_sha256_update(&ctx, (const uint8_t*)s, strlen(s));
    uint8_t digest[32];
    salivo_sha256_final(&ctx, digest);
    char* hex = (char*)malloc(65);
    if (!hex) return "";
    static const char hex_digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[i * 2]     = hex_digits[(digest[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = hex_digits[digest[i] & 0x0F];
    }
    hex[64] = '\0';
    return hex;
}

long long salivo_crypto_sha256_buf(long long in_addr, long long in_len, long long out_addr) {
    if (in_addr == 0 || out_addr == 0 || in_len < 0) return -1;
    SalivoSha256 ctx;
    salivo_sha256_init(&ctx);
    salivo_sha256_update(&ctx, (const uint8_t*)(uintptr_t)in_addr, (size_t)in_len);
    salivo_sha256_final(&ctx, (uint8_t*)(uintptr_t)out_addr);
    return 0;
}

long long salivo_crypto_sha256_state(const char* s, int state_idx) {
    if (!s) s = "";
    SalivoSha256 ctx;
    salivo_sha256_init(&ctx);
    salivo_sha256_update(&ctx, (const uint8_t*)s, strlen(s));
    uint8_t digest[32];
    salivo_sha256_final(&ctx, digest);
    if (state_idx < 0 || state_idx >= 8) return 0;
    return (long long)ctx.state[state_idx];
}

long long salivo_crypto_random_bytes(long long out_addr, long long count_bytes) {
    if (out_addr == 0 || count_bytes <= 0) return -1;
    void* buf = (void*)(uintptr_t)out_addr;
#ifdef _WIN32
    HCRYPTPROV hProv = 0;
    if (CryptAcquireContextA(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        BOOL ok = CryptGenRandom(hProv, (DWORD)count_bytes, (BYTE*)buf);
        CryptReleaseContext(hProv, 0);
        if (ok) return count_bytes;
    }
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t n = fread(buf, 1, (size_t)count_bytes, f);
        fclose(f);
        if (n == (size_t)count_bytes) return count_bytes;
    }
#endif
    unsigned char* p = (unsigned char*)buf;
    for (long long i = 0; i < count_bytes; i++) {
        p[i] = (unsigned char)(salivo_random_int() & 0xFF);
    }
    return count_bytes;
}

static const uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static const uint8_t rsbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};

static const uint8_t rcon[15] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36, 0x6c, 0xd8, 0xab, 0x4d
};

static uint8_t aes_xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ (((x >> 7) & 1) * 0x1b));
}

static uint8_t aes_multiply(uint8_t x, uint8_t y) {
    return (uint8_t)(((y & 1) * x) ^
           ((y >> 1 & 1) * aes_xtime(x)) ^
           ((y >> 2 & 1) * aes_xtime(aes_xtime(x))) ^
           ((y >> 3 & 1) * aes_xtime(aes_xtime(aes_xtime(x)))) ^
           ((y >> 4 & 1) * aes_xtime(aes_xtime(aes_xtime(aes_xtime(x))))));
}

static void aes256_key_expansion(const uint8_t* key, uint8_t* round_keys) {
    memcpy(round_keys, key, 32);
    for (int i = 8; i < 60; i++) {
        uint8_t temp[4];
        memcpy(temp, &round_keys[(i - 1) * 4], 4);
        if (i % 8 == 0) {
            uint8_t t = temp[0];
            temp[0] = sbox[temp[1]] ^ rcon[i / 8];
            temp[1] = sbox[temp[2]];
            temp[2] = sbox[temp[3]];
            temp[3] = sbox[t];
        } else if (i % 8 == 4) {
            temp[0] = sbox[temp[0]];
            temp[1] = sbox[temp[1]];
            temp[2] = sbox[temp[2]];
            temp[3] = sbox[temp[3]];
        }
        for (int j = 0; j < 4; j++) {
            round_keys[i * 4 + j] = round_keys[(i - 8) * 4 + j] ^ temp[j];
        }
    }
}

static void aes256_encrypt_block(const uint8_t* in, uint8_t* out, const uint8_t* round_keys) {
    uint8_t state[16];
    memcpy(state, in, 16);
    for (int i = 0; i < 16; i++) state[i] ^= round_keys[i];
    for (int round = 1; round < 14; round++) {
        for (int i = 0; i < 16; i++) state[i] = sbox[state[i]];
        uint8_t temp[16];
        temp[0] = state[0];  temp[1] = state[5];  temp[2] = state[10]; temp[3] = state[15];
        temp[4] = state[4];  temp[5] = state[9];  temp[6] = state[14]; temp[7] = state[3];
        temp[8] = state[8];  temp[9] = state[13]; temp[10] = state[2];  temp[11] = state[7];
        temp[12] = state[12]; temp[13] = state[1]; temp[14] = state[6];  temp[15] = state[11];
        for (int c = 0; c < 4; c++) {
            uint8_t a = temp[c * 4], b = temp[c * 4 + 1], d = temp[c * 4 + 2], e = temp[c * 4 + 3];
            state[c * 4]     = aes_xtime(a) ^ aes_xtime(b) ^ b ^ d ^ e;
            state[c * 4 + 1] = a ^ aes_xtime(b) ^ aes_xtime(d) ^ d ^ e;
            state[c * 4 + 2] = a ^ b ^ aes_xtime(d) ^ aes_xtime(e) ^ e;
            state[c * 4 + 3] = aes_xtime(a) ^ a ^ b ^ d ^ aes_xtime(e);
        }
        for (int i = 0; i < 16; i++) state[i] ^= round_keys[round * 16 + i];
    }
    for (int i = 0; i < 16; i++) state[i] = sbox[state[i]];
    uint8_t temp[16];
    temp[0] = state[0];  temp[1] = state[5];  temp[2] = state[10]; temp[3] = state[15];
    temp[4] = state[4];  temp[5] = state[9];  temp[6] = state[14]; temp[7] = state[3];
    temp[8] = state[8];  temp[9] = state[13]; temp[10] = state[2];  temp[11] = state[7];
    temp[12] = state[12]; temp[13] = state[1]; temp[14] = state[6];  temp[15] = state[11];
    for (int i = 0; i < 16; i++) out[i] = temp[i] ^ round_keys[14 * 16 + i];
}

static void aes256_decrypt_block(const uint8_t* in, uint8_t* out, const uint8_t* round_keys) {
    uint8_t state[16];
    memcpy(state, in, 16);
    for (int i = 0; i < 16; i++) state[i] ^= round_keys[14 * 16 + i];
    for (int round = 13; round > 0; round--) {
        uint8_t temp[16];
        temp[0] = state[0];  temp[1] = state[13]; temp[2] = state[10]; temp[3] = state[7];
        temp[4] = state[4];  temp[5] = state[1];  temp[6] = state[14]; temp[7] = state[11];
        temp[8] = state[8];  temp[9] = state[5];  temp[10] = state[2];  temp[11] = state[15];
        temp[12] = state[12]; temp[13] = state[9]; temp[14] = state[6];  temp[15] = state[3];
        for (int i = 0; i < 16; i++) state[i] = rsbox[temp[i]];
        for (int i = 0; i < 16; i++) state[i] ^= round_keys[round * 16 + i];
        for (int c = 0; c < 4; c++) {
            uint8_t a = state[c * 4], b = state[c * 4 + 1], d = state[c * 4 + 2], e = state[c * 4 + 3];
            temp[c * 4]     = aes_multiply(a, 0x0e) ^ aes_multiply(b, 0x0b) ^ aes_multiply(d, 0x0d) ^ aes_multiply(e, 0x09);
            temp[c * 4 + 1] = aes_multiply(a, 0x09) ^ aes_multiply(b, 0x0e) ^ aes_multiply(d, 0x0b) ^ aes_multiply(e, 0x0d);
            temp[c * 4 + 2] = aes_multiply(a, 0x0d) ^ aes_multiply(b, 0x09) ^ aes_multiply(d, 0x0e) ^ aes_multiply(e, 0x0b);
            temp[c * 4 + 3] = aes_multiply(a, 0x0b) ^ aes_multiply(b, 0x0d) ^ aes_multiply(d, 0x09) ^ aes_multiply(e, 0x0e);
        }
        memcpy(state, temp, 16);
    }
    uint8_t temp[16];
    temp[0] = state[0];  temp[1] = state[13]; temp[2] = state[10]; temp[3] = state[7];
    temp[4] = state[4];  temp[5] = state[1];  temp[6] = state[14]; temp[7] = state[11];
    temp[8] = state[8];  temp[9] = state[5];  temp[10] = state[2];  temp[11] = state[15];
    temp[12] = state[12]; temp[13] = state[9]; temp[14] = state[6];  temp[15] = state[3];
    for (int i = 0; i < 16; i++) out[i] = rsbox[temp[i]] ^ round_keys[i];
}

long long salivo_crypto_aes256_encrypt(long long key_addr, long long plain_addr, long long len, long long out_addr) {
    if (key_addr == 0 || plain_addr == 0 || out_addr == 0 || len <= 0) return -1;
    uint8_t round_keys[240];
    aes256_key_expansion((const uint8_t*)(uintptr_t)key_addr, round_keys);
    const uint8_t* p = (const uint8_t*)(uintptr_t)plain_addr;
    uint8_t* o = (uint8_t*)(uintptr_t)out_addr;
    long long offset = 0;
    while (offset < len) {
        uint8_t block[16];
        memset(block, 0, 16);
        long long chunk = (len - offset < 16) ? (len - offset) : 16;
        memcpy(block, p + offset, (size_t)chunk);
        aes256_encrypt_block(block, o + offset, round_keys);
        offset += 16;
    }
    return offset;
}

long long salivo_crypto_aes256_decrypt(long long key_addr, long long cipher_addr, long long len, long long out_addr) {
    if (key_addr == 0 || cipher_addr == 0 || out_addr == 0 || len <= 0) return -1;
    uint8_t round_keys[240];
    aes256_key_expansion((const uint8_t*)(uintptr_t)key_addr, round_keys);
    const uint8_t* c = (const uint8_t*)(uintptr_t)cipher_addr;
    uint8_t* o = (uint8_t*)(uintptr_t)out_addr;
    long long offset = 0;
    while (offset < len) {
        uint8_t block[16];
        memset(block, 0, 16);
        long long chunk = (len - offset < 16) ? (len - offset) : 16;
        memcpy(block, c + offset, (size_t)chunk);
        aes256_decrypt_block(block, o + offset, round_keys);
        offset += 16;
    }
    return offset;
}

// =============================================================================
// REAL OS CONDVAR & RWLOCK THREAD SYNCHRONIZATION
// =============================================================================

#define MAX_CONDVARS 1024
#define MAX_RWLOCKS 1024

#ifdef _WIN32
typedef CONDITION_VARIABLE salivo_cv_t;
typedef SRWLOCK salivo_rwlock_t;
#else
typedef pthread_cond_t salivo_cv_t;
typedef pthread_rwlock_t salivo_rwlock_t;
#endif

typedef struct {
    salivo_cv_t handle;
    int is_active;
    int waiter_count;
} SalivoCvSlot;

typedef struct {
    salivo_rwlock_t handle;
    int is_active;
    int reader_count;
    int is_write_locked;
} SalivoRwLockSlot;

static SalivoCvSlot g_cv_table[MAX_CONDVARS + 1];
static SalivoRwLockSlot g_rwlock_table[MAX_RWLOCKS + 1];

long long salivo_condvar_create(void) {
    for (int i = 1; i <= MAX_CONDVARS; i++) {
        if (!g_cv_table[i].is_active) {
#ifdef _WIN32
            InitializeConditionVariable(&g_cv_table[i].handle);
#else
            pthread_cond_init(&g_cv_table[i].handle, NULL);
#endif
            g_cv_table[i].is_active = 1;
            g_cv_table[i].waiter_count = 0;
            return (long long)i;
        }
    }
    return -1;
}

long long salivo_condvar_wait(long long cv_handle, long long mutex_handle) {
    SALIVO_BLOCKING("condvar_wait");
    if (cv_handle < 1 || cv_handle > MAX_CONDVARS || !g_cv_table[cv_handle].is_active) return -1;
    if (mutex_handle < 1 || mutex_handle > MAX_MUTEXES || !g_mutex_table[mutex_handle].is_active) return -1;
    g_cv_table[cv_handle].waiter_count++;
#ifdef _WIN32
    SleepConditionVariableCS(&g_cv_table[cv_handle].handle, &g_mutex_table[mutex_handle].handle, INFINITE);
#else
    pthread_cond_wait(&g_cv_table[cv_handle].handle, &g_mutex_table[mutex_handle].handle);
#endif
    g_cv_table[cv_handle].waiter_count--;
    return 0;
}

long long salivo_condvar_notify_one(long long cv_handle) {
    if (cv_handle < 1 || cv_handle > MAX_CONDVARS || !g_cv_table[cv_handle].is_active) return -1;
#ifdef _WIN32
    WakeConditionVariable(&g_cv_table[cv_handle].handle);
#else
    pthread_cond_signal(&g_cv_table[cv_handle].handle);
#endif
    return 0;
}

long long salivo_condvar_notify_all(long long cv_handle) {
    if (cv_handle < 1 || cv_handle > MAX_CONDVARS || !g_cv_table[cv_handle].is_active) return -1;
#ifdef _WIN32
    WakeAllConditionVariable(&g_cv_table[cv_handle].handle);
#else
    pthread_cond_broadcast(&g_cv_table[cv_handle].handle);
#endif
    return 0;
}

long long salivo_condvar_waiter_count(long long cv_handle) {
    if (cv_handle < 1 || cv_handle > MAX_CONDVARS || !g_cv_table[cv_handle].is_active) return 0;
    return (long long)g_cv_table[cv_handle].waiter_count;
}

long long salivo_rwlock_create(void) {
    for (int i = 1; i <= MAX_RWLOCKS; i++) {
        if (!g_rwlock_table[i].is_active) {
#ifdef _WIN32
            InitializeSRWLock(&g_rwlock_table[i].handle);
#else
            pthread_rwlock_init(&g_rwlock_table[i].handle, NULL);
#endif
            g_rwlock_table[i].is_active = 1;
            g_rwlock_table[i].reader_count = 0;
            g_rwlock_table[i].is_write_locked = 0;
            return (long long)i;
        }
    }
    return -1;
}

long long salivo_rwlock_read(long long handle) {
    if (handle < 1 || handle > MAX_RWLOCKS || !g_rwlock_table[handle].is_active) return -1;
#ifdef _WIN32
    AcquireSRWLockShared(&g_rwlock_table[handle].handle);
#else
    pthread_rwlock_rdlock(&g_rwlock_table[handle].handle);
#endif
    g_rwlock_table[handle].reader_count++;
    return 0;
}

long long salivo_rwlock_try_read(long long handle) {
    if (handle < 1 || handle > MAX_RWLOCKS || !g_rwlock_table[handle].is_active) return 0;
    if (g_rwlock_table[handle].is_write_locked) return 0;
#ifdef _WIN32
    if (TryAcquireSRWLockShared(&g_rwlock_table[handle].handle)) {
        g_rwlock_table[handle].reader_count++;
        return 1;
    }
    return 0;
#else
    if (pthread_rwlock_tryrdlock(&g_rwlock_table[handle].handle) == 0) {
        g_rwlock_table[handle].reader_count++;
        return 1;
    }
    return 0;
#endif
}

long long salivo_rwlock_write(long long handle) {
    if (handle < 1 || handle > MAX_RWLOCKS || !g_rwlock_table[handle].is_active) return -1;
#ifdef _WIN32
    AcquireSRWLockExclusive(&g_rwlock_table[handle].handle);
#else
    pthread_rwlock_wrlock(&g_rwlock_table[handle].handle);
#endif
    g_rwlock_table[handle].is_write_locked = 1;
    return 0;
}

long long salivo_rwlock_try_write(long long handle) {
    if (handle < 1 || handle > MAX_RWLOCKS || !g_rwlock_table[handle].is_active) return 0;
    if (g_rwlock_table[handle].reader_count > 0 || g_rwlock_table[handle].is_write_locked) return 0;
#ifdef _WIN32
    if (TryAcquireSRWLockExclusive(&g_rwlock_table[handle].handle)) {
        g_rwlock_table[handle].is_write_locked = 1;
        return 1;
    }
    return 0;
#else
    if (pthread_rwlock_trywrlock(&g_rwlock_table[handle].handle) == 0) {
        g_rwlock_table[handle].is_write_locked = 1;
        return 1;
    }
    return 0;
#endif
}

long long salivo_rwlock_unlock_read(long long handle) {
    if (handle < 1 || handle > MAX_RWLOCKS || !g_rwlock_table[handle].is_active) return -1;
    if (g_rwlock_table[handle].reader_count > 0) {
        g_rwlock_table[handle].reader_count--;
    }
#ifdef _WIN32
    ReleaseSRWLockShared(&g_rwlock_table[handle].handle);
#else
    pthread_rwlock_unlock(&g_rwlock_table[handle].handle);
#endif
    return 0;
}

long long salivo_rwlock_unlock_write(long long handle) {
    if (handle < 1 || handle > MAX_RWLOCKS || !g_rwlock_table[handle].is_active) return -1;
    g_rwlock_table[handle].is_write_locked = 0;
#ifdef _WIN32
    ReleaseSRWLockExclusive(&g_rwlock_table[handle].handle);
#else
    pthread_rwlock_unlock(&g_rwlock_table[handle].handle);
#endif
    return 0;
}

long long salivo_rwlock_reader_count(long long handle) {
    if (handle < 1 || handle > MAX_RWLOCKS || !g_rwlock_table[handle].is_active) return 0;
    return (long long)g_rwlock_table[handle].reader_count;
}

long long salivo_rwlock_is_write_locked(long long handle) {
    if (handle < 1 || handle > MAX_RWLOCKS || !g_rwlock_table[handle].is_active) return 0;
    return (long long)g_rwlock_table[handle].is_write_locked;
}

// =============================================================================
// REAL OS TIME & SLEEP ENGINE
// =============================================================================

long long salivo_time_now_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER li;
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    return (long long)((li.QuadPart - 116444736000000000ULL) / 10000ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + (ts.tv_nsec / 1000000LL);
#endif
}

void salivo_sleep_ms(long long ms) {
    SALIVO_BLOCKING("sleep");
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000LL;
    ts.tv_nsec = (ms % 1000LL) * 1000000LL;
    nanosleep(&ts, NULL);
#endif
}

// Native Networking is handled by salivo_net_runtime.c



// =============================================================================
// Native GUI Runtime (Win32 / Headless fallback)
// =============================================================================

#define MAX_GUI_WINDOWS 64

typedef struct {
    int is_active;
    int width;
    int height;
    int is_open;
    char title[256];
#ifdef _WIN32
    HWND hwnd;
#else
    int dummy_handle;
#endif
} SalivoGuiWindow;

static SalivoGuiWindow g_gui_windows[MAX_GUI_WINDOWS];
static int g_gui_init = 0;
static int g_gui_next_id = 1;

static void salivo_gui_ensure_init(void) {
    if (g_gui_init) return;
    memset(g_gui_windows, 0, sizeof(g_gui_windows));
    g_gui_init = 1;
}

#ifdef _WIN32
static LRESULT CALLBACK SalivoWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            return 0;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

static int g_wndclass_registered = 0;

static void salivo_gui_register_class(void) {
    if (g_wndclass_registered) return;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = SalivoWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "SalivoWindowClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&wc);
    g_wndclass_registered = 1;
}
#endif

long long salivo_gui_create_window(const char* title, long long width, long long height) {
    salivo_gui_ensure_init();
    if (g_gui_next_id >= MAX_GUI_WINDOWS) return -1;
    int id = g_gui_next_id++;
    SalivoGuiWindow* win = &g_gui_windows[id];
    win->is_active = 1;
    win->width = (int)width;
    win->height = (int)height;
    win->is_open = 1;
    if (title) {
        strncpy(win->title, title, 255);
        win->title[255] = '\0';
    }
#ifdef _WIN32
    salivo_gui_register_class();
    win->hwnd = CreateWindowExA(
        0, "SalivoWindowClass", title ? title : "Salivo",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        (int)width, (int)height,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );
    if (!win->hwnd) {
        win->is_active = 0;
        return -1;
    }
#endif
    return (long long)id;
}

long long salivo_gui_show_window(long long win_id) {
    salivo_gui_ensure_init();
    if (win_id < 1 || win_id >= MAX_GUI_WINDOWS) return -1;
    SalivoGuiWindow* win = &g_gui_windows[win_id];
    if (!win->is_active) return -1;
#ifdef _WIN32
    ShowWindow(win->hwnd, SW_SHOW);
    UpdateWindow(win->hwnd);
#endif
    return 0;
}

long long salivo_gui_close_window(long long win_id) {
    salivo_gui_ensure_init();
    if (win_id < 1 || win_id >= MAX_GUI_WINDOWS) return -1;
    SalivoGuiWindow* win = &g_gui_windows[win_id];
    if (!win->is_active) return -1;
    win->is_open = 0;
#ifdef _WIN32
    if (win->hwnd) {
        DestroyWindow(win->hwnd);
        win->hwnd = NULL;
    }
#endif
    win->is_active = 0;
    return 0;
}

long long salivo_gui_poll_events(long long win_id) {
    salivo_gui_ensure_init();
    if (win_id < 1 || win_id >= MAX_GUI_WINDOWS) return -1;
    SalivoGuiWindow* win = &g_gui_windows[win_id];
    if (!win->is_active || !win->is_open) return -1;
#ifdef _WIN32
    MSG msg;
    while (PeekMessageA(&msg, win->hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (msg.message == WM_QUIT || msg.message == WM_CLOSE) {
            win->is_open = 0;
            return -1;
        }
    }
    if (!IsWindow(win->hwnd)) {
        win->is_open = 0;
        return -1;
    }
#endif
    return 0; // 0 = OK, events processed
}

long long salivo_gui_is_window_open(long long win_id) {
    salivo_gui_ensure_init();
    if (win_id < 1 || win_id >= MAX_GUI_WINDOWS) return 0;
    return g_gui_windows[win_id].is_open ? 1 : 0;
}

long long salivo_gui_set_window_title(long long win_id, const char* title) {
    salivo_gui_ensure_init();
    if (win_id < 1 || win_id >= MAX_GUI_WINDOWS) return -1;
    SalivoGuiWindow* win = &g_gui_windows[win_id];
    if (!win->is_active) return -1;
    if (title) {
        strncpy(win->title, title, 255);
        win->title[255] = '\0';
    }
#ifdef _WIN32
    if (win->hwnd) SetWindowTextA(win->hwnd, title ? title : "");
#endif
    return 0;
}

// Clipboard
long long salivo_gui_set_clipboard(const char* text) {
#ifdef _WIN32
    if (!OpenClipboard(NULL)) return -1;
    EmptyClipboard();
    if (text) {
        size_t len = strlen(text);
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len + 1);
        if (hg) {
            char* p = (char*)GlobalLock(hg);
            memcpy(p, text, len + 1);
            GlobalUnlock(hg);
            SetClipboardData(CF_TEXT, hg);
        }
    }
    CloseClipboard();
    return 0;
#else
    (void)text;
    return -1;
#endif
}

char* salivo_gui_get_clipboard(void) {
#ifdef _WIN32
    if (!OpenClipboard(NULL)) return "";
    HANDLE hData = GetClipboardData(CF_TEXT);
    if (!hData) { CloseClipboard(); return ""; }
    char* pData = (char*)GlobalLock(hData);
    if (!pData) { CloseClipboard(); return ""; }
    size_t len = strlen(pData);
    char* result = (char*)malloc(len + 1);
    if (result) { memcpy(result, pData, len + 1); }
    else { result = ""; }
    GlobalUnlock(hData);
    CloseClipboard();
    return result;
#else
    return "";
#endif
}

// Native dialog
long long salivo_gui_message_box(const char* title, const char* message) {
#ifdef _WIN32
    MessageBoxA(NULL, message ? message : "", title ? title : "Salivo", MB_OK);
    return 0;
#else
    (void)title; (void)message;
    return -1;
#endif
}

// Input polling - returns last key code from window message queue
long long salivo_gui_poll_input(long long win_id) {
    salivo_gui_ensure_init();
    if (win_id < 1 || win_id >= MAX_GUI_WINDOWS) return -1;
    SalivoGuiWindow* win = &g_gui_windows[win_id];
    if (!win->is_active || !win->is_open) return -1;
#ifdef _WIN32
    MSG msg;
    if (PeekMessageA(&msg, win->hwnd, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE)) {
        return (long long)msg.wParam; // Virtual key code
    }
    if (PeekMessageA(&msg, win->hwnd, WM_LBUTTONDOWN, WM_LBUTTONDOWN, PM_REMOVE)) {
        return 1000; // Left mouse button
    }
    if (PeekMessageA(&msg, win->hwnd, WM_RBUTTONDOWN, WM_RBUTTONDOWN, PM_REMOVE)) {
        return 1001; // Right mouse button
    }
#endif
    return 0; // No event
}

// =============================================================================
// Native OS Threading Runtime (Win32 CreateThread / POSIX pthread)
// =============================================================================

typedef struct {
    long long fn_ptr;
    long long arg_ptr;
    long long result;
#ifdef _WIN32
    HANDLE handle;
    DWORD thread_id;
#else
    void* handle;
    unsigned long thread_id;
#endif
    int is_finished;
} SalivoOsThreadSlot;

#define MAX_OS_THREADS 256
static SalivoOsThreadSlot g_os_threads[MAX_OS_THREADS + 1];

#ifdef _WIN32
static DWORD WINAPI salivo_os_thread_proc(LPVOID param) {
    long long slot_idx = (long long)(intptr_t)param;
    if (slot_idx < 1 || slot_idx > MAX_OS_THREADS) return 0;
    
    typedef long long (*salivo_fn_arg)(long long);
    typedef long long (*salivo_fn_noarg)(void);

    long long fn_addr = g_os_threads[slot_idx].fn_ptr;
    long long arg = g_os_threads[slot_idx].arg_ptr;
    
    long long res = 0;
    if (arg != 0) {
        salivo_fn_arg f = (salivo_fn_arg)(intptr_t)fn_addr;
        res = f(arg);
    } else {
        salivo_fn_noarg f = (salivo_fn_noarg)(intptr_t)fn_addr;
        res = f();
    }
    
    g_os_threads[slot_idx].result = res;
    g_os_threads[slot_idx].is_finished = 1;
    return (DWORD)res;
}
#else
static void* salivo_os_thread_proc(void* param) {
    long long slot_idx = (long long)(intptr_t)param;
    if (slot_idx < 1 || slot_idx > MAX_OS_THREADS) return NULL;
    
    typedef long long (*salivo_fn_arg)(long long);
    typedef long long (*salivo_fn_noarg)(void);

    long long fn_addr = g_os_threads[slot_idx].fn_ptr;
    long long arg = g_os_threads[slot_idx].arg_ptr;
    
    long long res = 0;
    if (arg != 0) {
        salivo_fn_arg f = (salivo_fn_arg)(intptr_t)fn_addr;
        res = f(arg);
    } else {
        salivo_fn_noarg f = (salivo_fn_noarg)(intptr_t)fn_addr;
        res = f();
    }
    
    g_os_threads[slot_idx].result = res;
    g_os_threads[slot_idx].is_finished = 1;
    return (void*)(intptr_t)res;
}
#endif

long long salivo_thread_spawn(long long fn_ptr, long long arg_ptr) {
    for (int i = 1; i <= MAX_OS_THREADS; i++) {
        if (g_os_threads[i].handle == NULL && !g_os_threads[i].is_finished) {
            g_os_threads[i].fn_ptr = fn_ptr;
            g_os_threads[i].arg_ptr = arg_ptr;
            g_os_threads[i].result = 0;
            g_os_threads[i].is_finished = 0;
#ifdef _WIN32
            DWORD tid = 0;
            HANDLE h = CreateThread(NULL, 0, salivo_os_thread_proc, (LPVOID)(intptr_t)i, 0, &tid);
            if (!h) return -1;
            g_os_threads[i].handle = h;
            g_os_threads[i].thread_id = tid;
#else
            pthread_t pt;
            if (pthread_create(&pt, NULL, salivo_os_thread_proc, (void*)(intptr_t)i) != 0) {
                return -1;
            }
            g_os_threads[i].handle = (void*)pt;
            g_os_threads[i].thread_id = (unsigned long)pt;
#endif
            return (long long)i;
        }
    }
    return -1;
}

long long salivo_thread_join(long long thread_handle) {
    SALIVO_BLOCKING("thread_join");
    if (thread_handle < 1 || thread_handle > MAX_OS_THREADS) return -1;
    if (g_os_threads[thread_handle].handle == NULL && !g_os_threads[thread_handle].is_finished) return -1;
#ifdef _WIN32
    if (g_os_threads[thread_handle].handle != NULL) {
        WaitForSingleObject(g_os_threads[thread_handle].handle, INFINITE);
        CloseHandle(g_os_threads[thread_handle].handle);
        g_os_threads[thread_handle].handle = NULL;
    }
#else
    if (g_os_threads[thread_handle].handle != NULL) {
        pthread_join((pthread_t)g_os_threads[thread_handle].handle, NULL);
        g_os_threads[thread_handle].handle = NULL;
    }
#endif
    long long res = g_os_threads[thread_handle].result;
    g_os_threads[thread_handle].is_finished = 0;
    return res;
}

long long salivo_thread_current_id(void) {
#ifdef _WIN32
    return (long long)GetCurrentThreadId();
#else
    return (long long)pthread_self();
#endif
}

void salivo_thread_yield(void) {
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

void salivo_thread_sleep_ms(long long ms) {
    SALIVO_BLOCKING("sleep");
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)(ms * 1000));
#endif
}


