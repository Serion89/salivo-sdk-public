#define _CRT_SECURE_NO_WARNINGS
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Diagnostics only with SALIVO_SQLITE_TRACE=1, on stderr: SQL text never reaches stdout by default */
static int sqlite_trace_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("SALIVO_SQLITE_TRACE"); v = e && e[0] == '1'; }
    return v;
}
#define SQLITE_TRACE(...) do { if (sqlite_trace_on()) fprintf(stderr, __VA_ARGS__); } while (0)

#define MAX_DB_HANDLES 128
#define MAX_STMT_HANDLES 512

typedef struct {
    sqlite3* db;
    int is_active;
} SalivoDbSlot;

typedef struct {
    sqlite3_stmt* stmt;
    int is_active;
} SalivoStmtSlot;

static SalivoDbSlot g_db_table[MAX_DB_HANDLES + 1];
static SalivoStmtSlot g_stmt_table[MAX_STMT_HANDLES + 1];

static long long alloc_db_handle(sqlite3* db) {
    for (int i = 1; i <= MAX_DB_HANDLES; i++) {
        if (!g_db_table[i].is_active) {
            g_db_table[i].db = db;
            g_db_table[i].is_active = 1;
            return (long long)i;
        }
    }
    return -1;
}

static sqlite3* get_db_by_handle(long long handle) {
    if (handle < 1 || handle > MAX_DB_HANDLES || !g_db_table[handle].is_active) {
        return NULL;
    }
    return g_db_table[handle].db;
}

static long long alloc_stmt_handle(sqlite3_stmt* stmt) {
    for (int i = 1; i <= MAX_STMT_HANDLES; i++) {
        if (!g_stmt_table[i].is_active) {
            g_stmt_table[i].stmt = stmt;
            g_stmt_table[i].is_active = 1;
            return (long long)i;
        }
    }
    return -1;
}

static sqlite3_stmt* get_stmt_by_handle(long long handle) {
    if (handle < 1 || handle > MAX_STMT_HANDLES || !g_stmt_table[handle].is_active) {
        return NULL;
    }
    return g_stmt_table[handle].stmt;
}

long long salivo_sqlite_open(const char* path) {
    if (!path || strlen(path) == 0) path = ":memory:";
    sqlite3* db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        SQLITE_TRACE("[SQLITE_RT] Failed to open DB '%s': %s\n", path, sqlite3_errmsg(db));
        fflush(stdout);
        if (db) sqlite3_close(db);
        return -1;
    }
    long long handle = alloc_db_handle(db);
    if (handle < 0) {
        SQLITE_TRACE("[SQLITE_RT] Failed to allocate handle slot for DB '%s'\n", path);
        fflush(stdout);
        sqlite3_close(db);
        return -1;
    }
    SQLITE_TRACE("[SQLITE_RT] Opened database '%s' -> handle %lld\n", path, handle);
    fflush(stdout);
    return handle;
}

long long salivo_sqlite_close(long long db_handle) {
    sqlite3* db = get_db_by_handle(db_handle);
    if (!db) {
        SQLITE_TRACE("[SQLITE_RT] Close warning: invalid db handle %lld\n", db_handle);
        fflush(stdout);
        return -1;
    }
    int rc = sqlite3_close(db);
    if (rc == SQLITE_OK || rc == SQLITE_DONE) {
        g_db_table[db_handle].is_active = 0;
        g_db_table[db_handle].db = NULL;
        SQLITE_TRACE("[SQLITE_RT] Closed db handle %lld\n", db_handle);
    } else {
        SQLITE_TRACE("[SQLITE_RT] sqlite3_close returned error code %d for handle %lld\n", rc, db_handle);
    }
    fflush(stdout);
    return (rc == SQLITE_OK || rc == SQLITE_DONE) ? 0 : -1;
}

long long salivo_sqlite_execute(long long db_handle, const char* sql) {
    sqlite3* db = get_db_by_handle(db_handle);
    if (!db || !sql) return -1;
    char* err_msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        SQLITE_TRACE("[SQLITE_RT] Execute error on SQL '%s': %s\n", sql, err_msg ? err_msg : "unknown error");
        fflush(stdout);
        if (err_msg) sqlite3_free(err_msg);
        return -1;
    }
    int changes = sqlite3_changes(db);
    SQLITE_TRACE("[SQLITE_RT] Execute SQL '%s' -> affected rows: %d\n", sql, changes);
    fflush(stdout);
    return (long long)changes;
}

long long salivo_sqlite_prepare(long long db_handle, const char* sql) {
    sqlite3* db = get_db_by_handle(db_handle);
    if (!db || !sql) return -1;
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        SQLITE_TRACE("[SQLITE_RT] Prepare error on SQL '%s': %s\n", sql, sqlite3_errmsg(db));
        fflush(stdout);
        return -1;
    }
    long long stmt_handle = alloc_stmt_handle(stmt);
    if (stmt_handle < 0) {
        SQLITE_TRACE("[SQLITE_RT] Failed to allocate statement handle slot\n");
        fflush(stdout);
        sqlite3_finalize(stmt);
        return -1;
    }
    SQLITE_TRACE("[SQLITE_RT] Prepared statement -> handle %lld\n", stmt_handle);
    fflush(stdout);
    return stmt_handle;
}

long long salivo_sqlite_step(long long stmt_handle) {
    sqlite3_stmt* stmt = get_stmt_by_handle(stmt_handle);
    if (!stmt) return -1;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) return 1;
    if (rc == SQLITE_DONE) return 0;
    SQLITE_TRACE("[SQLITE_RT] Step error on stmt %lld: code %d\n", stmt_handle, rc);
    fflush(stdout);
    return -1;
}

long long salivo_sqlite_column_count(long long stmt_handle) {
    sqlite3_stmt* stmt = get_stmt_by_handle(stmt_handle);
    if (!stmt) return 0;
    return (long long)sqlite3_column_count(stmt);
}

char* salivo_sqlite_column_name(long long stmt_handle, long long col_idx) {
    sqlite3_stmt* stmt = get_stmt_by_handle(stmt_handle);
    if (!stmt) return "";
    const char* name = sqlite3_column_name(stmt, (int)col_idx);
    if (!name) return "";
    char* res = (char*)malloc(strlen(name) + 1);
    strcpy(res, name);
    return res;
}

char* salivo_sqlite_column_text(long long stmt_handle, long long col_idx) {
    sqlite3_stmt* stmt = get_stmt_by_handle(stmt_handle);
    if (!stmt) return "";
    const char* text = (const char*)sqlite3_column_text(stmt, (int)col_idx);
    if (!text) return "";
    char* res = (char*)malloc(strlen(text) + 1);
    strcpy(res, text);
    return res;
}

long long salivo_sqlite_column_int(long long stmt_handle, long long col_idx) {
    sqlite3_stmt* stmt = get_stmt_by_handle(stmt_handle);
    if (!stmt) return 0;
    return (long long)sqlite3_column_int(stmt, (int)col_idx);
}

long long salivo_sqlite_rows_affected(long long db_handle) {
    sqlite3* db = get_db_by_handle(db_handle);
    if (!db) return 0;
    return (long long)sqlite3_changes(db);
}

long long salivo_sqlite_last_insert_id(long long db_handle) {
    sqlite3* db = get_db_by_handle(db_handle);
    if (!db) return 0;
    return (long long)sqlite3_last_insert_rowid(db);
}

long long salivo_sqlite_begin(long long db_handle) {
    return salivo_sqlite_execute(db_handle, "BEGIN TRANSACTION;");
}

long long salivo_sqlite_commit(long long db_handle) {
    return salivo_sqlite_execute(db_handle, "COMMIT;");
}

long long salivo_sqlite_rollback(long long db_handle) {
    return salivo_sqlite_execute(db_handle, "ROLLBACK;");
}

long long salivo_sqlite_finalize(long long stmt_handle) {
    sqlite3_stmt* stmt = get_stmt_by_handle(stmt_handle);
    if (!stmt) return -1;
    sqlite3_finalize(stmt);
    g_stmt_table[stmt_handle].is_active = 0;
    g_stmt_table[stmt_handle].stmt = NULL;
    SQLITE_TRACE("[SQLITE_RT] Finalized stmt handle %lld\n", stmt_handle);
    fflush(stdout);
    return 0;
}

/* Growable JSON output buffer; `failed` latches on allocation failure */
typedef struct { char* data; size_t len; size_t cap; int failed; } JsonBuf;

static void jb_put(JsonBuf* b, const char* s, size_t n) {
    if (b->failed) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->len + n + 1 > cap) cap *= 2;
        char* d = (char*)realloc(b->data, cap);
        if (!d) { b->failed = 1; return; }
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

/* Appends s as a quoted, escaped JSON string (NULL -> "") */
static void jb_str(JsonBuf* b, const char* s) {
    jb_put(b, "\"", 1);
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; p++) {
        char esc[8];
        switch (*p) {
            case '"': jb_put(b, "\\\"", 2); break;
            case '\\': jb_put(b, "\\\\", 2); break;
            case '\n': jb_put(b, "\\n", 2); break;
            case '\r': jb_put(b, "\\r", 2); break;
            case '\t': jb_put(b, "\\t", 2); break;
            default:
                if (*p < 0x20) {
                    snprintf(esc, sizeof(esc), "\\u%04x", *p);
                    jb_put(b, esc, 6);
                } else {
                    jb_put(b, (const char*)p, 1);
                }
        }
    }
    jb_put(b, "\"", 1);
}

char* salivo_sqlite_query_json(long long db_handle, const char* sql) {
    sqlite3* db = get_db_by_handle(db_handle);
    if (!db || !sql) {
        char* empty = (char*)malloc(3);
        strcpy(empty, "[]");
        return empty;
    }
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        char* empty = (char*)malloc(3);
        strcpy(empty, "[]");
        return empty;
    }
    int col_count = sqlite3_column_count(stmt);
    JsonBuf jb = {NULL, 0, 0, 0};
    jb_put(&jb, "[", 1);
    int first_row = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first_row) jb_put(&jb, ", ", 2);
        first_row = 0;
        jb_put(&jb, "{", 1);
        for (int i = 0; i < col_count; i++) {
            if (i > 0) jb_put(&jb, ", ", 2);
            jb_str(&jb, sqlite3_column_name(stmt, i));
            jb_put(&jb, ": ", 2);
            jb_str(&jb, (const char*)sqlite3_column_text(stmt, i));
        }
        jb_put(&jb, "}", 1);
    }
    jb_put(&jb, "]", 1);
    sqlite3_finalize(stmt);
    if (jb.failed) {
        free(jb.data);
        char* empty = (char*)malloc(3);
        if (empty) strcpy(empty, "[]");
        return empty;
    }
    char* res = jb.data;
    return res;
}

