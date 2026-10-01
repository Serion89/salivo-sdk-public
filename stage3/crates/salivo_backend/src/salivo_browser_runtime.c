/* Stage 37.2: browser DOM + JavaScript FFI bridge for wasm32 modules (docs/STAGE_37_2_BROWSER.md).
 *
 * Linked into every wasm32 module next to salivo_wasm_runtime.c. wasm-ld drops the functions a
 * program does not call, together with their imports, so a program that uses no browser API keeps
 * the Stage 37.1 import set (salivo.write, salivo.exit) and still runs under scripts/wasm_host.mjs.
 *
 * Every `browser.*` import below is implemented by scripts/salivo_browser.mjs. JavaScript values
 * live only in that host's handle table; linear memory holds handles (index + generation, a
 * positive i32), never JS objects. Strings cross as (pointer, byte length) of UTF-8. A string the
 * host returns is staged on the host side: the import returns its byte length (or a negative error
 * code) and browser.take copies it into a fresh compiler-owned string (salivo_str_alloc). The intrinsic signatures (all i64
 * slots, strings as addresses) are in salivo_browser_abi.def.
 */

typedef unsigned long uptr;
typedef long long i64;
extern void* malloc(long long n);
extern void free(void* p);
extern void* memcpy(void* d, const void* s, uptr n);
extern long long strlen(const char* s);
/* The compiler's string allocator (Stage 3 IR): strings returned to Salivo come from it, so the
 * compiler frees them like any other string temporary it owns. */
extern char* salivo_str_alloc(long long n);

#define IMPORT(name) __attribute__((import_module("browser"), import_name(#name)))
IMPORT(window) int br_window(void);
IMPORT(document) int br_document(void);
IMPORT(release) int br_release(int h);
IMPORT(stat) int br_stat(int kind);
IMPORT(same) int br_same(int a, int b);
IMPORT(error) int br_error(void);
IMPORT(take) void br_take(char* dst);
IMPORT(query) int br_query(int root, const char* s, int n);
IMPORT(byid) int br_by_id(const char* s, int n);
IMPORT(create) int br_create(const char* s, int n);
IMPORT(createtext) int br_create_text(const char* s, int n);
IMPORT(insert) int br_insert(int parent, int child, int before);
IMPORT(remove) int br_remove(int h);
IMPORT(parent) int br_parent(int h);
IMPORT(childcount) int br_child_count(int h);
IMPORT(child) int br_child(int h, int i);
IMPORT(settext) int br_set_text(int h, const char* s, int n);
IMPORT(text) int br_text(int h);
IMPORT(setattr) int br_set_attr(int h, const char* k, int kn, const char* v, int vn);
IMPORT(attr) int br_attr(int h, const char* k, int kn);
IMPORT(hasattr) int br_has_attr(int h, const char* k, int kn);
IMPORT(removeattr) int br_remove_attr(int h, const char* k, int kn);
IMPORT(tag) int br_tag(int h);
IMPORT(typeof) int br_typeof(int h, const char* k, int kn);
IMPORT(getstr) int br_get_str(int h, const char* k, int kn);
IMPORT(setstr) int br_set_str(int h, const char* k, int kn, const char* v, int vn);
IMPORT(getint) int br_get_int(int h, const char* k, int kn, i64* out);
IMPORT(setint) int br_set_int(int h, const char* k, int kn, i64 v);
IMPORT(getbool) int br_get_bool(int h, const char* k, int kn);
IMPORT(setbool) int br_set_bool(int h, const char* k, int kn, int v);
IMPORT(getobj) int br_get_obj(int h, const char* k, int kn);
IMPORT(call) int br_call(int h, const char* k, int kn);
IMPORT(callstr) int br_call_str(int h, const char* k, int kn, const char* v, int vn);
IMPORT(listen) int br_listen(int h, const char* t, int tn, int fn, i64 ctx);
IMPORT(timer) int br_timer(int ms, int repeat, int fn, i64 ctx);
IMPORT(fetch) int br_fetch(const char* m, int mn, const char* u, int un, const char* hd, int hn, const char* b, int bn, int fn, i64 ctx);

#define E_ARG (-7)
#define E_HANDLE (-10)

static i64 status;
static int current_event;

static i64 rc(i64 r) {
    status = r < 0 ? r : 0;
    return r;
}

/* Handles are positive i32 values; anything else is rejected before it reaches the host. */
static int bad(i64 h) { return h <= 0 || h > 0x7fffffff; }
static int bad0(i64 h) { return h < 0 || h > 0x7fffffff; }

static const char* S(i64 s) { return s ? (const char*)(uptr)s : ""; }
static int L(i64 s) { return s ? (int)strlen((const char*)(uptr)s) : 0; }

/* Copies the string the host staged (n bytes) into a fresh NUL-terminated buffer. */
static i64 take(int n) {
    rc(n);
    char* p = salivo_str_alloc(n < 0 ? 1 : (i64)n + 1);
    if (n > 0) br_take(p);
    p[n < 0 ? 0 : n] = 0;
    return (i64)(uptr)p;
}

/* A Salivo function value is its wasm table index; it must fit the 32-bit address space. */
static int fnok(i64 fn) { return fn > 0 && fn <= 0xffffffffLL; }

/* Host entry point for every callback: listener events, timer ticks and fetch completions.
 * `ev` is the borrowed event / response handle (0 for timers). Nested dispatch (a callback that
 * makes the browser fire another event synchronously) saves and restores the current event. */
__attribute__((export_name("salivo_br_invoke"))) i64 salivo_br_invoke(int fn, i64 ctx, int ev) {
    int saved = current_event;
    current_event = ev;
    i64 r = ((i64(*)(i64))(uptr)(unsigned)fn)(ctx);
    current_event = saved;
    return r;
}

i64 salivo_br_window(void) { return rc(br_window()); }
i64 salivo_br_document(void) { return rc(br_document()); }
i64 salivo_br_release(i64 h) { return rc(bad(h) ? E_HANDLE : br_release((int)h)); }
i64 salivo_br_status(void) { return status; }
i64 salivo_br_error_str(void) {
    i64 saved = status;
    i64 s = take(br_error());
    status = saved;
    return s;
}
i64 salivo_br_stat(i64 k) { return rc(k < 0 || k > 5 ? E_ARG : br_stat((int)k)); }
i64 salivo_br_same(i64 a, i64 b) { return rc(bad(a) || bad(b) ? E_HANDLE : br_same((int)a, (int)b)); }

i64 salivo_br_query(i64 root, i64 sel) { return rc(bad(root) ? E_HANDLE : br_query((int)root, S(sel), L(sel))); }
i64 salivo_br_by_id(i64 id) { return rc(br_by_id(S(id), L(id))); }
i64 salivo_br_create(i64 tag) { return rc(br_create(S(tag), L(tag))); }
i64 salivo_br_create_text(i64 s) { return rc(br_create_text(S(s), L(s))); }
i64 salivo_br_insert(i64 p, i64 c, i64 before) {
    return rc(bad(p) || bad(c) || bad0(before) ? E_HANDLE : br_insert((int)p, (int)c, (int)before));
}
i64 salivo_br_remove(i64 h) { return rc(bad(h) ? E_HANDLE : br_remove((int)h)); }
i64 salivo_br_parent(i64 h) { return rc(bad(h) ? E_HANDLE : br_parent((int)h)); }
i64 salivo_br_child_count(i64 h) { return rc(bad(h) ? E_HANDLE : br_child_count((int)h)); }
i64 salivo_br_child(i64 h, i64 i) {
    if (bad(h)) return rc(E_HANDLE);
    return rc(i < 0 || i > 0x7fffffff ? 0 : br_child((int)h, (int)i));
}
i64 salivo_br_set_text(i64 h, i64 s) { return rc(bad(h) ? E_HANDLE : br_set_text((int)h, S(s), L(s))); }
i64 salivo_br_text_str(i64 h) { return take(bad(h) ? E_HANDLE : br_text((int)h)); }
i64 salivo_br_set_attr(i64 h, i64 k, i64 v) {
    return rc(bad(h) ? E_HANDLE : br_set_attr((int)h, S(k), L(k), S(v), L(v)));
}
i64 salivo_br_attr_str(i64 h, i64 k) { return take(bad(h) ? E_HANDLE : br_attr((int)h, S(k), L(k))); }
i64 salivo_br_has_attr(i64 h, i64 k) { return rc(bad(h) ? E_HANDLE : br_has_attr((int)h, S(k), L(k))); }
i64 salivo_br_remove_attr(i64 h, i64 k) { return rc(bad(h) ? E_HANDLE : br_remove_attr((int)h, S(k), L(k))); }
i64 salivo_br_tag_str(i64 h) { return take(bad(h) ? E_HANDLE : br_tag((int)h)); }

i64 salivo_br_typeof_str(i64 h, i64 k) { return take(bad(h) ? E_HANDLE : br_typeof((int)h, S(k), L(k))); }
i64 salivo_br_get_str(i64 h, i64 k) { return take(bad(h) ? E_HANDLE : br_get_str((int)h, S(k), L(k))); }
i64 salivo_br_set_str(i64 h, i64 k, i64 v) {
    return rc(bad(h) ? E_HANDLE : br_set_str((int)h, S(k), L(k), S(v), L(v)));
}
i64 salivo_br_get_int(i64 h, i64 k) {
    i64 v = 0;
    if (rc(bad(h) ? E_HANDLE : br_get_int((int)h, S(k), L(k), &v)) < 0) return 0;
    return v;
}
i64 salivo_br_set_int(i64 h, i64 k, i64 v) { return rc(bad(h) ? E_HANDLE : br_set_int((int)h, S(k), L(k), v)); }
i64 salivo_br_get_bool(i64 h, i64 k) {
    i64 r = rc(bad(h) ? E_HANDLE : br_get_bool((int)h, S(k), L(k)));
    return r < 0 ? 0 : r;
}
i64 salivo_br_set_bool(i64 h, i64 k, i64 v) { return rc(bad(h) ? E_HANDLE : br_set_bool((int)h, S(k), L(k), v != 0)); }
i64 salivo_br_get_obj(i64 h, i64 k) { return rc(bad(h) ? E_HANDLE : br_get_obj((int)h, S(k), L(k))); }
i64 salivo_br_call(i64 h, i64 k) { return rc(bad(h) ? E_HANDLE : br_call((int)h, S(k), L(k))); }
i64 salivo_br_call_str(i64 h, i64 k, i64 v) {
    return rc(bad(h) ? E_HANDLE : br_call_str((int)h, S(k), L(k), S(v), L(v)));
}

i64 salivo_br_listen(i64 h, i64 type, i64 fn, i64 ctx) {
    if (bad(h)) return rc(E_HANDLE);
    return rc(fnok(fn) ? br_listen((int)h, S(type), L(type), (int)fn, ctx) : E_ARG);
}
i64 salivo_br_event(void) { return rc(current_event); }
i64 salivo_br_timer(i64 ms, i64 repeat, i64 fn, i64 ctx) {
    if (ms < 0 || ms > 0x7fffffff || !fnok(fn)) return rc(E_ARG);
    return rc(br_timer((int)ms, repeat != 0, (int)fn, ctx));
}
i64 salivo_br_fetch(i64 m, i64 u, i64 hd, i64 b, i64 fn, i64 ctx) {
    if (!fnok(fn)) return rc(E_ARG);
    return rc(br_fetch(S(m), L(m), S(u), L(u), S(hd), L(hd), S(b), L(b), (int)fn, ctx));
}
i64 salivo_br_apply(i64 fn, i64 ctx) {
    if (!fnok(fn)) return rc(E_ARG);
    return ((i64(*)(i64))(uptr)fn)(ctx);
}

/* String boxes: UI state that must outlive the compiler-managed string it was made from. */
i64 salivo_br_keep(i64 s) {
    i64 n = L(s);
    char* b = (char*)malloc(n + 1);
    memcpy(b, S(s), (uptr)n);
    b[n] = 0;
    return (i64)(uptr)b;
}
i64 salivo_br_kept_str(i64 b) { return b ? b : (i64)(uptr)""; }
i64 salivo_br_unkeep(i64 b) {
    free((void*)(uptr)b);
    return 0;
}
