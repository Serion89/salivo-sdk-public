/* Stage 38.1 native desktop windowing / canvas and Stage 38.2 game rendering (Win32 + OpenGL).
 *
 * Included at the end of salivo_sys_runtime.c, so it shares that file's string helpers, error
 * codes and link step. ABI rows: the `rtsysgfx*` / `rtsyswin*` / ... lines of salivo_sys_abi.def.
 * Public Salivo API: std/desktop.sal (38.1) and std/game.sal (38.2).
 *
 * Model
 *  - One platform thread: the first thread that creates a window owns every window, the event
 *    queue and every graphics call for the life of the process (Win32 message queues and
 *    OpenGL contexts are thread-bound). Calls from any other thread return -13.
 *  - Every object (window, texture, mesh, shader) is a generation handle: (generation << 16) |
 *    (slot + 1). Destroying an object bumps its slot generation, so an old handle can never reach
 *    a newer object in the same slot (-10).
 *  - Each window owns one OpenGL context. Textures, meshes and shaders belong to the window they
 *    were created for (-14 when used with another window) and are destroyed with it.
 *  - The window procedure never calls into Salivo: it only appends events to a bounded queue that
 *    eventPoll drains. Destroying a window removes its queued events, so no event or callback can
 *    reach a destroyed window.
 *  - Coordinates and sizes are physical client-area pixels, origin top-left, y down. The process
 *    is per-monitor DPI aware; windowScale reports the DPI scale (x1000).
 *  - Colors are 0xRRGGBBAA. Angles are degrees. 3D positions and scales are milli-units (x1000).
 *
 * Error codes: -7 invalid argument, -8 OS failure, -10 invalid or stale handle, -11 table full,
 * -12 unsupported, -13 wrong thread, -14 resource of another window, -15 invalid state,
 * -16 asset not found, -17 invalid asset, -18 shader compile/link failure, -19 graphics failure.
 */

#define GFX_WRONG_THREAD (-13)
#define GFX_WRONG_WINDOW (-14)
#define GFX_STATE (-15)
#define GFX_NOT_FOUND (-16)
#define GFX_BAD_ASSET (-17)
#define GFX_SHADER (-18)
#define GFX_GRAPHICS (-19)

#ifdef _WIN32

#include <math.h>
#include <GL/gl.h>
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

/* ---------- OpenGL 1.5 / 2.0 entry points (wglGetProcAddress) ---------- */

typedef char GLchar_;
typedef ptrdiff_t GLsizeiptr_;
#define GL_ARRAY_BUFFER_ 0x8892
#define GL_STATIC_DRAW_ 0x88E4
#define GL_FRAGMENT_SHADER_ 0x8B30
#define GL_VERTEX_SHADER_ 0x8B31
#define GL_COMPILE_STATUS_ 0x8B81
#define GL_LINK_STATUS_ 0x8B82
#define GL_CLAMP_TO_EDGE_ 0x812F

typedef void(APIENTRY* PGenBuffers)(GLsizei, GLuint*);
typedef void(APIENTRY* PDeleteBuffers)(GLsizei, const GLuint*);
typedef void(APIENTRY* PBindBuffer)(GLenum, GLuint);
typedef void(APIENTRY* PBufferData)(GLenum, GLsizeiptr_, const void*, GLenum);
typedef GLuint(APIENTRY* PCreateShader)(GLenum);
typedef void(APIENTRY* PShaderSource)(GLuint, GLsizei, const GLchar_* const*, const GLint*);
typedef void(APIENTRY* PCompileShader)(GLuint);
typedef void(APIENTRY* PGetShaderiv)(GLuint, GLenum, GLint*);
typedef void(APIENTRY* PGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar_*);
typedef void(APIENTRY* PDeleteShader)(GLuint);
typedef GLuint(APIENTRY* PCreateProgram)(void);
typedef void(APIENTRY* PAttachShader)(GLuint, GLuint);
typedef void(APIENTRY* PLinkProgram)(GLuint);
typedef void(APIENTRY* PGetProgramiv)(GLuint, GLenum, GLint*);
typedef void(APIENTRY* PGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar_*);
typedef void(APIENTRY* PDeleteProgram)(GLuint);
typedef void(APIENTRY* PUseProgram)(GLuint);
typedef GLint(APIENTRY* PGetUniformLocation)(GLuint, const GLchar_*);
typedef void(APIENTRY* PUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void(APIENTRY* PUniform1i)(GLint, GLint);
typedef BOOL(WINAPI* PSwapInterval)(int);

static struct {
    int loaded;
    PGenBuffers GenBuffers; PDeleteBuffers DeleteBuffers; PBindBuffer BindBuffer; PBufferData BufferData;
    PCreateShader CreateShader; PShaderSource ShaderSource; PCompileShader CompileShader;
    PGetShaderiv GetShaderiv; PGetShaderInfoLog GetShaderInfoLog; PDeleteShader DeleteShader;
    PCreateProgram CreateProgram; PAttachShader AttachShader; PLinkProgram LinkProgram;
    PGetProgramiv GetProgramiv; PGetProgramInfoLog GetProgramInfoLog; PDeleteProgram DeleteProgram;
    PUseProgram UseProgram; PGetUniformLocation GetUniformLocation; PUniform4f Uniform4f; PUniform1i Uniform1i;
    PSwapInterval SwapInterval;
    GLint max_tex;
} gl;

static void* gl_proc(const char* name) {
    void* p = (void*)wglGetProcAddress(name);
    /* some drivers return small sentinels instead of NULL for missing functions */
    if (p == (void*)1 || p == (void*)2 || p == (void*)3 || p == (void*)-1) return 0;
    return p;
}

static void gl_load(void) {
    if (gl.loaded) return;
    gl.loaded = 1;
    gl.GenBuffers = (PGenBuffers)gl_proc("glGenBuffers");
    gl.DeleteBuffers = (PDeleteBuffers)gl_proc("glDeleteBuffers");
    gl.BindBuffer = (PBindBuffer)gl_proc("glBindBuffer");
    gl.BufferData = (PBufferData)gl_proc("glBufferData");
    gl.CreateShader = (PCreateShader)gl_proc("glCreateShader");
    gl.ShaderSource = (PShaderSource)gl_proc("glShaderSource");
    gl.CompileShader = (PCompileShader)gl_proc("glCompileShader");
    gl.GetShaderiv = (PGetShaderiv)gl_proc("glGetShaderiv");
    gl.GetShaderInfoLog = (PGetShaderInfoLog)gl_proc("glGetShaderInfoLog");
    gl.DeleteShader = (PDeleteShader)gl_proc("glDeleteShader");
    gl.CreateProgram = (PCreateProgram)gl_proc("glCreateProgram");
    gl.AttachShader = (PAttachShader)gl_proc("glAttachShader");
    gl.LinkProgram = (PLinkProgram)gl_proc("glLinkProgram");
    gl.GetProgramiv = (PGetProgramiv)gl_proc("glGetProgramiv");
    gl.GetProgramInfoLog = (PGetProgramInfoLog)gl_proc("glGetProgramInfoLog");
    gl.DeleteProgram = (PDeleteProgram)gl_proc("glDeleteProgram");
    gl.UseProgram = (PUseProgram)gl_proc("glUseProgram");
    gl.GetUniformLocation = (PGetUniformLocation)gl_proc("glGetUniformLocation");
    gl.Uniform4f = (PUniform4f)gl_proc("glUniform4f");
    gl.Uniform1i = (PUniform1i)gl_proc("glUniform1i");
    gl.SwapInterval = (PSwapInterval)gl_proc("wglSwapIntervalEXT");
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl.max_tex);
}

static int gl_has_vbo(void) { return gl.GenBuffers && gl.DeleteBuffers && gl.BindBuffer && gl.BufferData; }
static int gl_has_glsl(void) {
    return gl.CreateShader && gl.ShaderSource && gl.CompileShader && gl.GetShaderiv && gl.GetShaderInfoLog &&
           gl.DeleteShader && gl.CreateProgram && gl.AttachShader && gl.LinkProgram && gl.GetProgramiv &&
           gl.GetProgramInfoLog && gl.DeleteProgram && gl.UseProgram && gl.GetUniformLocation && gl.Uniform4f && gl.Uniform1i;
}

/* ---------- generation handle table ---------- */

enum { K_FREE = 0, K_WIN, K_TEX, K_MESH, K_SHADER };
#define GFX_SLOTS 4096

typedef struct { int kind; unsigned gen; long long owner; void* p; } GfxSlot;
static GfxSlot g_slots[GFX_SLOTS];
static int g_live[5];
static DWORD g_gfx_thread;

static long long slot_new(int kind, void* p, long long owner) {
    for (int i = 0; i < GFX_SLOTS; i++) {
        if (g_slots[i].kind != K_FREE) continue;
        if (g_slots[i].gen == 0 || g_slots[i].gen >= 0x7FFFFFFF) g_slots[i].gen = 1;
        g_slots[i].kind = kind;
        g_slots[i].p = p;
        g_slots[i].owner = owner;
        g_live[kind]++;
        return ((long long)g_slots[i].gen << 16) | (i + 1);
    }
    return SYS_INVALID_HANDLE - 1; /* -11 table full */
}

static GfxSlot* slot_get(long long h, int kind) {
    if (h <= 0) return 0;
    long long idx = (h & 0xFFFF) - 1;
    if (idx < 0 || idx >= GFX_SLOTS) return 0;
    GfxSlot* s = &g_slots[idx];
    if (s->kind != kind || (long long)s->gen != (h >> 16)) return 0;
    return s;
}

static void slot_release(GfxSlot* s) {
    g_live[s->kind]--;
    s->kind = K_FREE;
    s->p = 0;
    s->owner = 0;
    s->gen++;
}

static int gfx_thread_ok(void) {
    DWORD t = GetCurrentThreadId();
    return g_gfx_thread == 0 || g_gfx_thread == t;
}

/* ---------- windows ---------- */

typedef struct {
    HWND hwnd;
    HDC dc;
    HGLRC rc;
    long long handle;
    int w, h;
    int shown, fullscreen, minimized;
    int in_frame, mode; /* mode 1 = 2D pixel projection, 2 = 3D camera */
    WINDOWPLACEMENT place;
    LONG style;
} GfxWin;

static GfxWin* g_current; /* window whose context is current */

static GfxWin* win_get(long long h, long long* err) {
    if (!gfx_thread_ok()) { *err = GFX_WRONG_THREAD; return 0; }
    GfxSlot* s = slot_get(h, K_WIN);
    if (!s) { *err = SYS_INVALID_HANDLE; return 0; }
    return (GfxWin*)s->p;
}

static int make_current(GfxWin* w) {
    if (g_current == w) return 1;
    if (!wglMakeCurrent(w->dc, w->rc)) return 0;
    g_current = w;
    return 1;
}

/* ---------- event queue ---------- */

typedef struct { long long type, win, a, b, c, x, y; } GfxEvent;
#define GFX_QUEUE 1024
static GfxEvent g_q[GFX_QUEUE];
static int g_qhead, g_qlen;
static long long g_dropped;
static GfxEvent g_ev;
static unsigned char g_keys[256];
static long long g_mx, g_my, g_mbuttons, g_mwin;
static WCHAR g_high_surrogate;

static void ev_push(long long type, long long win, long long a, long long b, long long c, long long x, long long y) {
    if (g_qlen > 0) { /* coalesce consecutive mouse moves of one window */
        GfxEvent* last = &g_q[(g_qhead + g_qlen - 1) % GFX_QUEUE];
        if (type == 8 && last->type == 8 && last->win == win) { last->x = x; last->y = y; return; }
    }
    if (g_qlen == GFX_QUEUE) { g_dropped++; return; }
    GfxEvent* e = &g_q[(g_qhead + g_qlen) % GFX_QUEUE];
    e->type = type; e->win = win; e->a = a; e->b = b; e->c = c; e->x = x; e->y = y;
    g_qlen++;
}

/* Removes every queued event of a window (it is being destroyed). */
static void ev_purge(long long win) {
    int n = g_qlen, out = 0;
    for (int i = 0; i < n; i++) {
        GfxEvent e = g_q[(g_qhead + i) % GFX_QUEUE];
        if (e.win != win) g_q[(g_qhead + out++) % GFX_QUEUE] = e;
    }
    g_qlen = out;
    if (g_ev.win == win) memset(&g_ev, 0, sizeof g_ev);
}

/* Focus loss: release every held key and mouse button so nothing stays stuck. */
static void release_input(long long win) {
    for (int k = 0; k < 256; k++) {
        if (g_keys[k]) { g_keys[k] = 0; ev_push(6, win, k, 0, 0, 0, 0); }
    }
    for (int b = 1; b <= 3; b++) {
        if (g_mbuttons & (1LL << b)) ev_push(10, win, b, 0, 0, g_mx, g_my);
    }
    g_mbuttons = 0;
    g_high_surrogate = 0;
}

static LRESULT CALLBACK gfx_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    GfxWin* w = (GfxWin*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    long long h = w->handle;
    switch (msg) {
    case WM_CLOSE: ev_push(1, h, 0, 0, 0, 0, 0); return 0; /* the program decides; nothing is destroyed here */
    case WM_ERASEBKGND: return 1;
    case WM_SYSCOMMAND: /* a lone Alt / F10 must not enter the modal menu loop (it would stall the frame loop) */
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
        break;
    case WM_SIZE: {
        w->minimized = wp == SIZE_MINIMIZED;
        if (!w->minimized) { w->w = LOWORD(lp); w->h = HIWORD(lp); }
        ev_push(2, h, w->w, w->h, wp == SIZE_MINIMIZED ? 1 : wp == SIZE_MAXIMIZED ? 2 : 0, 0, 0);
        return 0;
    }
    case WM_MOVE: ev_push(3, h, 0, 0, 0, (short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_SETFOCUS: ev_push(4, h, 1, 0, 0, 0, 0); return 0;
    case WM_KILLFOCUS: release_input(h); ev_push(4, h, 0, 0, 0, 0, 0); return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        int vk = (int)(wp & 0xFF);
        g_keys[vk] = 1;
        ev_push(5, h, vk, (lp >> 16) & 0x1FF, (lp >> 30) & 1, 0, 0);
        if (msg == WM_SYSKEYDOWN) break; /* Alt+F4 and the system menu still work */
        return 0;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        int vk = (int)(wp & 0xFF);
        g_keys[vk] = 0;
        ev_push(6, h, vk, (lp >> 16) & 0x1FF, 0, 0, 0);
        if (msg == WM_SYSKEYUP) break;
        return 0;
    }
    case WM_CHAR: {
        WCHAR c = (WCHAR)wp;
        if (c >= 0xD800 && c <= 0xDBFF) { g_high_surrogate = c; return 0; }
        long long cp = c;
        if (c >= 0xDC00 && c <= 0xDFFF) {
            if (!g_high_surrogate) return 0;
            cp = 0x10000 + (((long long)g_high_surrogate - 0xD800) << 10) + (c - 0xDC00);
        }
        g_high_surrogate = 0;
        ev_push(7, h, cp, 0, 0, 0, 0);
        return 0;
    }
    case WM_MOUSEMOVE:
        g_mx = (short)LOWORD(lp); g_my = (short)HIWORD(lp); g_mwin = h;
        ev_push(8, h, 0, 0, 0, g_mx, g_my);
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        int b = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 1 : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 2 : 3;
        int down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
        g_mx = (short)LOWORD(lp); g_my = (short)HIWORD(lp); g_mwin = h;
        if (down) { g_mbuttons |= 1LL << b; SetCapture(hwnd); }
        else { g_mbuttons &= ~(1LL << b); if (!g_mbuttons) ReleaseCapture(); }
        ev_push(down ? 9 : 10, h, b, 0, 0, g_mx, g_my);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(hwnd, &p);
        ev_push(11, h, (short)HIWORD(wp), 0, 0, p.x, p.y);
        return 0;
    }
    case 0x02E0: { /* WM_DPICHANGED: take the suggested rectangle */
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, 0, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void pump(void) {
    MSG m;
    while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

static int g_class_ready;
static const wchar_t* GFX_CLASS = L"SalivoDesktopWindow";

static void dpi_aware(void) {
    static int done;
    if (done) return;
    done = 1;
    typedef BOOL(WINAPI * SetCtx)(HANDLE);
    SetCtx f = (SetCtx)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (f) f((HANDLE)(INT_PTR)-4); /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
}

static void destroy_resources_of(long long win);

static void win_free(GfxWin* w) {
    if (w->rc) {
        if (g_current == w) { wglMakeCurrent(0, 0); g_current = 0; }
        wglDeleteContext(w->rc);
    }
    if (w->dc) ReleaseDC(w->hwnd, w->dc);
    if (w->hwnd) {
        SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(w->hwnd);
    }
    free(w);
}

long long salivo_sys_win_create(const char* title, long long width, long long height) {
    if (!gfx_thread_ok()) return GFX_WRONG_THREAD;
    if (!title || strlen(title) > 4096 || width < 1 || height < 1 || width > 16384 || height > 16384) return SYS_INVALID_ARG;
    dpi_aware();
    HINSTANCE inst = GetModuleHandleW(0);
    if (!g_class_ready) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = gfx_wndproc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(0, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = GFX_CLASS;
        if (!RegisterClassExW(&wc)) return SYS_OS;
        g_class_ready = 1;
    }
    GfxWin* w = (GfxWin*)calloc(1, sizeof(GfxWin));
    if (!w) return SYS_OS;
    w->style = WS_OVERLAPPEDWINDOW;
    RECT r = { 0, 0, (LONG)width, (LONG)height };
    AdjustWindowRectEx(&r, (DWORD)w->style, FALSE, 0);
    wchar_t* wt = widen(title);
    w->hwnd = CreateWindowExW(0, GFX_CLASS, wt ? wt : L"", (DWORD)w->style, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, 0, 0, inst, 0);
    free(wt);
    if (!w->hwnd) { free(w); return SYS_OS; }
    w->dc = GetDC(w->hwnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1 };
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int pf = w->dc ? ChoosePixelFormat(w->dc, &pfd) : 0;
    if (!pf || !SetPixelFormat(w->dc, pf, &pfd) || !(w->rc = wglCreateContext(w->dc))) {
        win_free(w);
        return GFX_GRAPHICS;
    }
    long long h = slot_new(K_WIN, w, 0);
    if (h < 0) { win_free(w); return h; }
    w->handle = h;
    g_slots[(h & 0xFFFF) - 1].owner = h;
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
    if (!make_current(w)) { slot_release(&g_slots[(h & 0xFFFF) - 1]); win_free(w); return GFX_GRAPHICS; }
    gl_load();
    if (gl.SwapInterval) gl.SwapInterval(1); /* vsync on by default */
    RECT cr;
    GetClientRect(w->hwnd, &cr);
    w->w = cr.right;
    w->h = cr.bottom;
    g_gfx_thread = GetCurrentThreadId();
    return h;
}

long long salivo_sys_win_destroy(long long win) {
    long long err = 0;
    GfxWin* w = win_get(win, &err);
    if (!w) return err;
    destroy_resources_of(win); /* textures, meshes, shaders first: they need the context */
    ev_purge(win);
    if (g_mwin == win) { g_mwin = 0; g_mbuttons = 0; }
    slot_release(&g_slots[(win & 0xFFFF) - 1]);
    win_free(w);
    pump(); /* deliver WM_DESTROY-time messages now; the window no longer maps to a handle */
    return 0;
}

/* mode: 0 hide, 1 show, 2 minimize, 3 maximize, 4 restore, 5 focus */
long long salivo_sys_win_show(long long win, long long mode) {
    long long err = 0;
    GfxWin* w = win_get(win, &err);
    if (!w) return err;
    static const int cmds[] = { SW_HIDE, SW_SHOW, SW_MINIMIZE, SW_MAXIMIZE, SW_RESTORE };
    if (mode < 0 || mode > 5) return SYS_INVALID_ARG;
    if (mode == 5) {
        if (!w->shown) return GFX_STATE;
        SetForegroundWindow(w->hwnd);
        SetFocus(w->hwnd);
        return 0;
    }
    ShowWindow(w->hwnd, cmds[mode]);
    w->shown = mode != 0;
    if (mode == 1) { UpdateWindow(w->hwnd); SetForegroundWindow(w->hwnd); }
    pump();
    return 0;
}

long long salivo_sys_win_title(long long win, const char* title) {
    long long err = 0;
    GfxWin* w = win_get(win, &err);
    if (!w) return err;
    if (!title || strlen(title) > 4096) return SYS_INVALID_ARG;
    wchar_t* wt = widen(title);
    BOOL ok = wt && SetWindowTextW(w->hwnd, wt);
    free(wt);
    return ok ? 0 : SYS_OS;
}

/* Client-area size in physical pixels. */
long long salivo_sys_win_resize(long long win, long long width, long long height) {
    long long err = 0;
    GfxWin* w = win_get(win, &err);
    if (!w) return err;
    if (width < 1 || height < 1 || width > 16384 || height > 16384) return SYS_INVALID_ARG;
    if (w->fullscreen) return GFX_STATE;
    RECT r = { 0, 0, (LONG)width, (LONG)height };
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(w->hwnd, GWL_STYLE), FALSE, 0);
    if (!SetWindowPos(w->hwnd, 0, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) return SYS_OS;
    pump(); /* WM_SIZE updates w->w / w->h and queues the resize event */
    return 0;
}

long long salivo_sys_win_move(long long win, long long x, long long y) {
    long long err = 0;
    GfxWin* w = win_get(win, &err);
    if (!w) return err;
    if (w->fullscreen) return GFX_STATE;
    if (x < -32768 || x > 32767 || y < -32768 || y > 32767) return SYS_INVALID_ARG;
    if (!SetWindowPos(w->hwnd, 0, (int)x, (int)y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) return SYS_OS;
    pump();
    return 0;
}

/* which: 0 client width, 1 client height, 2 x, 3 y (outer, screen), 4 shown, 5 fullscreen,
 * 6 minimized, 7 focused, 8 DPI scale x1000, 9 in frame */
long long salivo_sys_win_int(long long win, long long which) {
    long long err = 0;
    GfxWin* w = win_get(win, &err);
    if (!w) return err;
    RECT r;
    switch (which) {
    case 0: return w->w;
    case 1: return w->h;
    case 2: GetWindowRect(w->hwnd, &r); return r.left;
    case 3: GetWindowRect(w->hwnd, &r); return r.top;
    case 4: return w->shown;
    case 5: return w->fullscreen;
    case 6: return IsIconic(w->hwnd) ? 1 : 0;
    case 7: return GetForegroundWindow() == w->hwnd ? 1 : 0;
    case 8: {
        typedef UINT(WINAPI * GetDpi)(HWND);
        GetDpi f = (GetDpi)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        return f ? (long long)f(w->hwnd) * 1000 / 96 : 1000;
    }
    case 9: return w->in_frame;
    }
    return SYS_INVALID_ARG;
}

/* Borderless fullscreen on the window's monitor (no display mode change); 0 restores. */
long long salivo_sys_win_fullscreen(long long win, long long on) {
    long long err = 0;
    GfxWin* w = win_get(win, &err);
    if (!w) return err;
    if (on != 0 && on != 1) return SYS_INVALID_ARG;
    if (on == w->fullscreen) return 0;
    if (on) {
        MONITORINFO mi = { sizeof mi };
        w->place.length = sizeof w->place;
        if (!GetWindowPlacement(w->hwnd, &w->place) || !GetMonitorInfoW(MonitorFromWindow(w->hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return SYS_OS;
        SetWindowLongW(w->hwnd, GWL_STYLE, (LONG)(w->style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | (w->shown ? WS_VISIBLE : 0));
        SetWindowPos(w->hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(w->hwnd, GWL_STYLE, w->style | (w->shown ? WS_VISIBLE : 0));
        SetWindowPlacement(w->hwnd, &w->place);
        SetWindowPos(w->hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    w->fullscreen = (int)on;
    pump();
    return 0;
}

long long salivo_sys_win_vsync(long long win, long long on) {
    long long err = 0;
    GfxWin* w = win_get(win, &err);
    if (!w) return err;
    if (on != 0 && on != 1) return SYS_INVALID_ARG;
    if (!make_current(w)) return GFX_GRAPHICS;
    if (!gl.SwapInterval) return SYS_UNSUPPORTED;
    return gl.SwapInterval((int)on) ? 0 : GFX_GRAPHICS;
}

/* ---------- events and input ---------- */

/* Next event type (0 when the queue is empty). Pumps the OS queue first. */
long long salivo_sys_ev_poll(void) {
    if (!gfx_thread_ok()) return GFX_WRONG_THREAD;
    pump();
    if (g_qlen == 0) { memset(&g_ev, 0, sizeof g_ev); return 0; }
    g_ev = g_q[g_qhead];
    g_qhead = (g_qhead + 1) % GFX_QUEUE;
    g_qlen--;
    return g_ev.type;
}

/* Blocks up to timeout ms (no CPU spin) for an OS message or a queued event, then polls. */
long long salivo_sys_ev_wait(long long timeout_ms) {
    if (!gfx_thread_ok()) return GFX_WRONG_THREAD;
    if (timeout_ms < 0 || timeout_ms > 86400000) return SYS_INVALID_ARG;
    if (g_qlen == 0) {
        pump();
        if (g_qlen == 0) MsgWaitForMultipleObjects(0, 0, FALSE, (DWORD)timeout_ms, QS_ALLINPUT);
    }
    return salivo_sys_ev_poll();
}

/* field of the current event: 0 window, 1 a, 2 b, 3 c, 4 x, 5 y */
long long salivo_sys_ev_field(long long field) {
    switch (field) {
    case 0: return g_ev.win;
    case 1: return g_ev.a;
    case 2: return g_ev.b;
    case 3: return g_ev.c;
    case 4: return g_ev.x;
    case 5: return g_ev.y;
    }
    return SYS_INVALID_ARG;
}

long long salivo_sys_key_down(long long vk) {
    if (vk < 0 || vk > 255) return SYS_INVALID_ARG;
    return g_keys[vk];
}

/* which: 0 x, 1 y, 2 button bitmask (bit 1 left, 2 right, 3 middle), 3 window under the mouse */
long long salivo_sys_mouse(long long which) {
    switch (which) {
    case 0: return g_mx;
    case 1: return g_my;
    case 2: return g_mbuttons;
    case 3: return g_mwin;
    }
    return SYS_INVALID_ARG;
}

/* ---------- frames and the 2D canvas ---------- */

static void set_color(long long rgba) {
    glColor4ub((GLubyte)(rgba >> 24), (GLubyte)(rgba >> 16), (GLubyte)(rgba >> 8), (GLubyte)rgba);
}

static void pixel_projection(GfxWin* w) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w->w, w->h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    w->mode = 1;
}

static GfxWin* frame_win(long long win, long long* err) {
    GfxWin* w = win_get(win, err);
    if (!w) return 0;
    if (!w->in_frame) { *err = GFX_STATE; return 0; }
    if (!make_current(w)) { *err = GFX_GRAPHICS; return 0; }
    return w;
}

static long long g_draws;

long long salivo_sys_frame_begin(long long win) {
    long long err = 0;
    GfxWin* w = win_get(win, &err);
    if (!w) return err;
    if (w->in_frame) return GFX_STATE;
    if (!make_current(w)) return GFX_GRAPHICS;
    RECT cr; /* the viewport always matches the current client area */
    GetClientRect(w->hwnd, &cr);
    if (cr.right > 0 && cr.bottom > 0) { w->w = cr.right; w->h = cr.bottom; }
    glViewport(0, 0, w->w, w->h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    pixel_projection(w);
    w->in_frame = 1;
    return 0;
}

long long salivo_sys_frame_clear(long long win, long long rgba) {
    long long err = 0;
    GfxWin* w = frame_win(win, &err);
    if (!w) return err;
    glClearColor(((rgba >> 24) & 255) / 255.0f, ((rgba >> 16) & 255) / 255.0f, ((rgba >> 8) & 255) / 255.0f, (rgba & 255) / 255.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return glGetError() == GL_NO_ERROR ? 0 : GFX_GRAPHICS;
}

long long salivo_sys_frame_rect(long long win, long long x, long long y, long long wd, long long ht, long long rgba) {
    long long err = 0;
    GfxWin* w = frame_win(win, &err);
    if (!w) return err;
    if (wd < 0 || ht < 0 || wd > 1 << 20 || ht > 1 << 20) return SYS_INVALID_ARG;
    if (w->mode != 1) pixel_projection(w);
    glDisable(GL_TEXTURE_2D);
    set_color(rgba);
    glBegin(GL_QUADS);
    glVertex2f((float)x, (float)y);
    glVertex2f((float)(x + wd), (float)y);
    glVertex2f((float)(x + wd), (float)(y + ht));
    glVertex2f((float)x, (float)(y + ht));
    glEnd();
    g_draws++;
    return glGetError() == GL_NO_ERROR ? 0 : GFX_GRAPHICS;
}

/* Reads one pixel of the frame being drawn (back buffer) as 0xRRGGBBAA. */
long long salivo_sys_frame_pixel(long long win, long long x, long long y) {
    long long err = 0;
    GfxWin* w = frame_win(win, &err);
    if (!w) return err;
    if (x < 0 || y < 0 || x >= w->w || y >= w->h) return SYS_INVALID_ARG;
    unsigned char p[4] = { 0 };
    glFinish();
    glReadBuffer(GL_BACK);
    glReadPixels((GLint)x, (GLint)(w->h - 1 - y), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    if (glGetError() != GL_NO_ERROR) return GFX_GRAPHICS;
    return ((long long)p[0] << 24) | ((long long)p[1] << 16) | ((long long)p[2] << 8) | 255;
}

long long salivo_sys_frame_present(long long win) {
    long long err = 0;
    GfxWin* w = frame_win(win, &err);
    if (!w) return err;
    w->in_frame = 0;
    return SwapBuffers(w->dc) ? 0 : GFX_GRAPHICS;
}

/* ---------- 38.2: time, cameras, textures, meshes, shaders ---------- */

long long salivo_sys_time_us(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (long long)(c.QuadPart / f.QuadPart * 1000000 + (c.QuadPart % f.QuadPart) * 1000000 / f.QuadPart);
}

/* 2D camera: (cx, cy) world point at the window center, zoom x1000, rotation degrees. */
long long salivo_sys_cam2d(long long win, long long cx, long long cy, long long zoom, long long rot) {
    long long err = 0;
    GfxWin* w = frame_win(win, &err);
    if (!w) return err;
    if (zoom <= 0 || zoom > 1000000) return SYS_INVALID_ARG;
    pixel_projection(w);
    glTranslatef(w->w / 2.0f, w->h / 2.0f, 0);
    glScalef(zoom / 1000.0f, zoom / 1000.0f, 1);
    glRotatef((float)-rot, 0, 0, 1);
    glTranslatef((float)-cx, (float)-cy, 0);
    return 0;
}

typedef struct { GLuint id; int w, h, dirty; unsigned char* px; } GfxTex;
typedef struct { float x, y, z, u, v; unsigned char rgba[4]; } GfxVertex;
typedef struct { GLuint vbo; int count, cap, dirty; GfxVertex* v; } GfxMesh;
typedef struct { GLuint prog; GLint tint, tex, use_tex; } GfxShader;

static GfxSlot* res_get(long long h, int kind, long long* err) {
    if (!gfx_thread_ok()) { *err = GFX_WRONG_THREAD; return 0; }
    GfxSlot* s = slot_get(h, kind);
    if (!s) { *err = SYS_INVALID_HANDLE; return 0; }
    GfxSlot* ws = slot_get(s->owner, K_WIN);
    if (!ws || !make_current((GfxWin*)ws->p)) { *err = GFX_GRAPHICS; return 0; }
    return s;
}

/* A resource argument of a draw call: 0 means none; otherwise it must belong to the window. */
static long long check_res(long long h, int kind, long long win, void** out) {
    *out = 0;
    if (h == 0) return 0;
    GfxSlot* s = slot_get(h, kind);
    if (!s) return SYS_INVALID_HANDLE;
    if (s->owner != win) return GFX_WRONG_WINDOW;
    *out = s->p;
    return 0;
}

static void tex_free(GfxTex* t) { glDeleteTextures(1, &t->id); free(t->px); free(t); }
static void mesh_free(GfxMesh* m) { if (m->vbo) gl.DeleteBuffers(1, &m->vbo); free(m->v); free(m); }
static void shader_free(GfxShader* s) { gl.DeleteProgram(s->prog); free(s); }

static void destroy_resources_of(long long win) {
    GfxSlot* ws = slot_get(win, K_WIN);
    if (!ws || !make_current((GfxWin*)ws->p)) return;
    for (int i = 0; i < GFX_SLOTS; i++) {
        GfxSlot* s = &g_slots[i];
        if (s->kind == K_FREE || s->kind == K_WIN || s->owner != win) continue;
        if (s->kind == K_TEX) tex_free((GfxTex*)s->p);
        else if (s->kind == K_MESH) mesh_free((GfxMesh*)s->p);
        else shader_free((GfxShader*)s->p);
        slot_release(s);
    }
}

static long long tex_new(long long win, int wd, int ht, unsigned char* px) {
    long long err = 0;
    GfxWin* w = win_get(win, &err);
    if (!w) { free(px); return err; }
    if (!make_current(w)) { free(px); return GFX_GRAPHICS; }
    GfxTex* t = (GfxTex*)calloc(1, sizeof(GfxTex));
    if (!t) { free(px); return SYS_OS; }
    t->w = wd; t->h = ht; t->px = px;
    while (glGetError() != GL_NO_ERROR) {}
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, wd, ht, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (glGetError() != GL_NO_ERROR) { tex_free(t); return GFX_GRAPHICS; } /* e.g. out of video memory */
    long long h = slot_new(K_TEX, t, win);
    if (h < 0) tex_free(t);
    return h;
}

static int tex_dims_ok(long long wd, long long ht) {
    return wd >= 1 && ht >= 1 && wd <= gl.max_tex && ht <= gl.max_tex && wd * ht <= (1LL << 26);
}

/* Blank (transparent black) texture; fill it with texPixel. */
long long salivo_sys_tex_create(long long win, long long wd, long long ht) {
    long long err = 0;
    if (!win_get(win, &err)) return err;
    if (!tex_dims_ok(wd, ht)) return SYS_INVALID_ARG;
    unsigned char* px = (unsigned char*)calloc((size_t)(wd * ht), 4);
    if (!px) return SYS_OS;
    return tex_new(win, (int)wd, (int)ht, px);
}

static unsigned rd16(const unsigned char* p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

/* Uncompressed 24- or 32-bit BMP file. */
long long salivo_sys_tex_load(long long win, const char* path) {
    long long err = 0;
    if (!win_get(win, &err)) return err;
    if (!path || !*path) return SYS_INVALID_ARG;
    wchar_t* wp = widen(path);
    FILE* f = wp ? _wfopen(wp, L"rb") : 0;
    free(wp);
    if (!f) return GFX_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 54 || size > (1L << 28)) { fclose(f); return GFX_BAD_ASSET; }
    unsigned char* b = (unsigned char*)malloc((size_t)size);
    if (!b) { fclose(f); return SYS_OS; }
    size_t got = fread(b, 1, (size_t)size, f);
    fclose(f);
    long long rc = GFX_BAD_ASSET;
    if (got == (size_t)size && b[0] == 'B' && b[1] == 'M') {
        unsigned off = rd32(b + 10), hdr = rd32(b + 14), comp = rd32(b + 30), bpp = rd16(b + 28);
        long long wd = (int)rd32(b + 18), ht = (int)rd32(b + 22);
        int bottom_up = ht > 0;
        if (ht < 0) ht = -ht;
        long long stride = ((wd * bpp + 31) / 32) * 4;
        if (hdr >= 40 && (bpp == 24 || bpp == 32) && (comp == 0 || (comp == 3 && bpp == 32)) && tex_dims_ok(wd, ht) &&
            off <= (unsigned)size && stride * ht <= (long long)size - off) {
            unsigned char* px = (unsigned char*)malloc((size_t)(wd * ht * 4));
            if (!px) { free(b); return SYS_OS; }
            for (long long y = 0; y < ht; y++) {
                const unsigned char* row = b + off + (bottom_up ? ht - 1 - y : y) * stride;
                for (long long x = 0; x < wd; x++) {
                    const unsigned char* s = row + x * (bpp / 8);
                    unsigned char* d = px + (y * wd + x) * 4;
                    d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = bpp == 32 ? s[3] : 255;
                }
            }
            rc = tex_new(win, (int)wd, (int)ht, px);
        }
    }
    free(b);
    return rc;
}

long long salivo_sys_tex_pixel(long long tex, long long x, long long y, long long rgba) {
    if (!gfx_thread_ok()) return GFX_WRONG_THREAD;
    GfxSlot* s = slot_get(tex, K_TEX);
    if (!s) return SYS_INVALID_HANDLE;
    GfxTex* t = (GfxTex*)s->p;
    if (x < 0 || y < 0 || x >= t->w || y >= t->h) return SYS_INVALID_ARG;
    unsigned char* d = t->px + (y * t->w + x) * 4;
    d[0] = (unsigned char)(rgba >> 24); d[1] = (unsigned char)(rgba >> 16); d[2] = (unsigned char)(rgba >> 8); d[3] = (unsigned char)rgba;
    t->dirty = 1;
    return 0;
}

/* which: 0 width, 1 height */
long long salivo_sys_tex_size(long long tex, long long which) {
    if (!gfx_thread_ok()) return GFX_WRONG_THREAD;
    GfxSlot* s = slot_get(tex, K_TEX);
    if (!s) return SYS_INVALID_HANDLE;
    if (which != 0 && which != 1) return SYS_INVALID_ARG;
    return which ? ((GfxTex*)s->p)->h : ((GfxTex*)s->p)->w;
}

long long salivo_sys_tex_destroy(long long tex) {
    long long err = 0;
    GfxSlot* s = res_get(tex, K_TEX, &err);
    if (!s) return err;
    tex_free((GfxTex*)s->p);
    slot_release(s);
    return 0;
}

static void bind_tex(GfxTex* t) {
    if (!t) { glDisable(GL_TEXTURE_2D); return; }
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, t->id);
    if (t->dirty) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, t->px);
        t->dirty = 0;
    }
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

/* Quad centered on (x, y), w x h, rotated rot degrees clockwise (y down), tinted rgba, texture or 0.
 * Model transform = translate(x, y) * rotate(rot) * scale(w, h) on the unit quad [-0.5, 0.5]. */
long long salivo_sys_draw_quad(long long win, long long x, long long y, long long wd, long long ht, long long rot, long long rgba, long long tex) {
    long long err = 0;
    GfxWin* w = frame_win(win, &err);
    if (!w) return err;
    if (w->mode != 1) return GFX_STATE; /* a 3D camera is active: switch back with frameBegin/camera2d */
    if (wd < 0 || ht < 0 || wd > 1 << 20 || ht > 1 << 20) return SYS_INVALID_ARG;
    void* tp;
    long long rc = check_res(tex, K_TEX, win, &tp);
    if (rc) return rc;
    glPushMatrix();
    glTranslatef((float)x, (float)y, 0);
    glRotatef((float)rot, 0, 0, 1);
    glScalef((float)wd, (float)ht, 1);
    bind_tex((GfxTex*)tp);
    set_color(rgba);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-0.5f, -0.5f);
    glTexCoord2f(1, 0); glVertex2f(0.5f, -0.5f);
    glTexCoord2f(1, 1); glVertex2f(0.5f, 0.5f);
    glTexCoord2f(0, 1); glVertex2f(-0.5f, 0.5f);
    glEnd();
    glPopMatrix();
    glDisable(GL_TEXTURE_2D);
    g_draws++;
    return glGetError() == GL_NO_ERROR ? 0 : GFX_GRAPHICS;
}

long long salivo_sys_mesh_create(long long win) {
    long long err = 0;
    GfxWin* w = win_get(win, &err);
    if (!w) return err;
    if (!make_current(w)) return GFX_GRAPHICS;
    if (!gl_has_vbo()) return SYS_UNSUPPORTED;
    GfxMesh* m = (GfxMesh*)calloc(1, sizeof(GfxMesh));
    if (!m) return SYS_OS;
    long long h = slot_new(K_MESH, m, win);
    if (h < 0) free(m);
    return h;
}

/* Appends one vertex (triangle list): position milli-units, color, texture coordinates x1000. */
long long salivo_sys_mesh_vertex(long long mesh, long long x, long long y, long long z, long long rgba, long long u, long long v) {
    if (!gfx_thread_ok()) return GFX_WRONG_THREAD;
    GfxSlot* s = slot_get(mesh, K_MESH);
    if (!s) return SYS_INVALID_HANDLE;
    GfxMesh* m = (GfxMesh*)s->p;
    if (m->count >= (1 << 22)) return SYS_INVALID_ARG;
    if (m->count == m->cap) {
        int cap = m->cap ? m->cap * 2 : 64;
        GfxVertex* nv = (GfxVertex*)realloc(m->v, sizeof(GfxVertex) * (size_t)cap);
        if (!nv) return SYS_OS;
        m->v = nv;
        m->cap = cap;
    }
    GfxVertex* d = &m->v[m->count++];
    d->x = x / 1000.0f; d->y = y / 1000.0f; d->z = z / 1000.0f; d->u = u / 1000.0f; d->v = v / 1000.0f;
    d->rgba[0] = (unsigned char)(rgba >> 24); d->rgba[1] = (unsigned char)(rgba >> 16); d->rgba[2] = (unsigned char)(rgba >> 8); d->rgba[3] = (unsigned char)rgba;
    m->dirty = 1;
    return m->count;
}

long long salivo_sys_mesh_count(long long mesh) {
    if (!gfx_thread_ok()) return GFX_WRONG_THREAD;
    GfxSlot* s = slot_get(mesh, K_MESH);
    return s ? ((GfxMesh*)s->p)->count : SYS_INVALID_HANDLE;
}

long long salivo_sys_mesh_destroy(long long mesh) {
    long long err = 0;
    GfxSlot* s = res_get(mesh, K_MESH, &err);
    if (!s) return err;
    mesh_free((GfxMesh*)s->p);
    slot_release(s);
    return 0;
}

static GLuint compile_stage(GLenum type, const char* src, char* log, int cap) {
    GLuint sh = gl.CreateShader(type);
    if (!sh) { snprintf(log, (size_t)cap, "glCreateShader failed"); return 0; }
    gl.ShaderSource(sh, 1, &src, 0);
    gl.CompileShader(sh);
    GLint ok = 0;
    gl.GetShaderiv(sh, GL_COMPILE_STATUS_, &ok);
    if (!ok) {
        int n = (int)strlen(log);
        n += snprintf(log + n, (size_t)(cap - n), type == GL_VERTEX_SHADER_ ? "vertex: " : "fragment: ");
        gl.GetShaderInfoLog(sh, cap - n, 0, log + n);
        gl.DeleteShader(sh);
        return 0;
    }
    return sh;
}

static char g_shader_log[4096];

/* GLSL program; vertex inputs gl_Vertex / gl_Color / gl_MultiTexCoord0 and the fixed-function
 * matrices (GLSL 1.10/1.20 compatibility). Optional uniforms: vec4 tint, sampler2D tex, int useTex. */
long long salivo_sys_shader_create(long long win, const char* vs, const char* fs) {
    long long err = 0;
    GfxWin* w = win_get(win, &err);
    if (!w) return err;
    if (!vs || !fs || !*vs || !*fs || strlen(vs) > (1 << 20) || strlen(fs) > (1 << 20)) return SYS_INVALID_ARG;
    if (!make_current(w)) return GFX_GRAPHICS;
    if (!gl_has_glsl()) return SYS_UNSUPPORTED;
    g_shader_log[0] = 0;
    GLuint v = compile_stage(GL_VERTEX_SHADER_, vs, g_shader_log, sizeof g_shader_log);
    GLuint f = compile_stage(GL_FRAGMENT_SHADER_, fs, g_shader_log, sizeof g_shader_log);
    if (!v || !f) {
        if (v) gl.DeleteShader(v);
        if (f) gl.DeleteShader(f);
        return GFX_SHADER;
    }
    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, v);
    gl.AttachShader(p, f);
    gl.LinkProgram(p);
    gl.DeleteShader(v); /* flagged; freed with the program */
    gl.DeleteShader(f);
    GLint ok = 0;
    gl.GetProgramiv(p, GL_LINK_STATUS_, &ok);
    if (!ok) {
        int n = snprintf(g_shader_log, sizeof g_shader_log, "link: ");
        gl.GetProgramInfoLog(p, (GLsizei)(sizeof g_shader_log - n), 0, g_shader_log + n);
        gl.DeleteProgram(p);
        return GFX_SHADER;
    }
    GfxShader* s = (GfxShader*)calloc(1, sizeof(GfxShader));
    if (!s) { gl.DeleteProgram(p); return SYS_OS; }
    s->prog = p;
    s->tint = gl.GetUniformLocation(p, "tint");
    s->tex = gl.GetUniformLocation(p, "tex");
    s->use_tex = gl.GetUniformLocation(p, "useTex");
    long long h = slot_new(K_SHADER, s, win);
    if (h < 0) shader_free(s);
    return h;
}

char* salivo_sys_shader_log(void) { return out_cstr(g_shader_log); }

long long salivo_sys_shader_destroy(long long sh) {
    long long err = 0;
    GfxSlot* s = res_get(sh, K_SHADER, &err);
    if (!s) return err;
    shader_free((GfxShader*)s->p);
    slot_release(s);
    return 0;
}

/* 3D camera: eye and target in milli-units (up = +Y), vertical field of view in degrees,
 * near/far planes in milli-units. Enables depth testing until the next frameBegin/camera2d. */
long long salivo_sys_cam3d(long long win, long long ex, long long ey, long long ez, long long tx, long long ty, long long tz,
                           long long fov, long long znear, long long zfar) {
    long long err = 0;
    GfxWin* w = frame_win(win, &err);
    if (!w) return err;
    if (fov < 1 || fov > 179 || znear <= 0 || zfar <= znear) return SYS_INVALID_ARG;
    double e[3] = { ex / 1000.0, ey / 1000.0, ez / 1000.0 }, t[3] = { tx / 1000.0, ty / 1000.0, tz / 1000.0 };
    double f[3] = { t[0] - e[0], t[1] - e[1], t[2] - e[2] };
    double fl = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-9) return SYS_INVALID_ARG;
    for (int i = 0; i < 3; i++) f[i] /= fl;
    double s[3] = { f[1] * 0 - f[2] * 1, f[2] * 0 - f[0] * 0, f[0] * 1 - f[1] * 0 }; /* f x up(0,1,0) */
    double sl = sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (sl < 1e-9) return SYS_INVALID_ARG; /* looking straight up or down */
    for (int i = 0; i < 3; i++) s[i] /= sl;
    double u[3] = { s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0] };
    float view[16] = { (float)s[0], (float)u[0], (float)-f[0], 0, (float)s[1], (float)u[1], (float)-f[1], 0,
                       (float)s[2], (float)u[2], (float)-f[2], 0,
                       (float)-(s[0] * e[0] + s[1] * e[1] + s[2] * e[2]), (float)-(u[0] * e[0] + u[1] * e[1] + u[2] * e[2]),
                       (float)(f[0] * e[0] + f[1] * e[1] + f[2] * e[2]), 1 };
    double n = znear / 1000.0, fa = zfar / 1000.0, aspect = w->h > 0 ? (double)w->w / w->h : 1.0;
    double c = 1.0 / tan(fov * 3.14159265358979323846 / 360.0);
    float proj[16] = { (float)(c / aspect), 0, 0, 0, 0, (float)c, 0, 0, 0, 0, (float)((fa + n) / (n - fa)), -1,
                       0, 0, (float)(2 * fa * n / (n - fa)), 0 };
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(proj);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(view);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    w->mode = 2;
    return 0;
}

/* Draws a mesh under the 3D camera. Model = translate(x, y, z) * rotY * rotX * rotZ * scale,
 * position and scale milli-units, rotations degrees. shader 0 = fixed function (vertex colors x
 * texture); otherwise the program gets tint (rgba), tex (unit 0) and useTex. */
long long salivo_sys_draw_mesh(long long win, long long mesh, long long shader, long long x, long long y, long long z,
                               long long rx, long long ry, long long rz, long long scale, long long rgba, long long tex) {
    long long err = 0;
    GfxWin* w = frame_win(win, &err);
    if (!w) return err;
    if (w->mode != 2) return GFX_STATE; /* camera3d first */
    if (scale <= 0 || scale > 1000000000) return SYS_INVALID_ARG;
    void *mp, *sp, *tp;
    long long rc = check_res(mesh, K_MESH, win, &mp);
    if (!rc && !mp) rc = SYS_INVALID_HANDLE;
    if (!rc) rc = check_res(shader, K_SHADER, win, &sp);
    if (!rc) rc = check_res(tex, K_TEX, win, &tp);
    if (rc) return rc;
    GfxMesh* m = (GfxMesh*)mp;
    if (m->count < 3) return GFX_STATE;
    if (!m->vbo) gl.GenBuffers(1, &m->vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER_, m->vbo);
    if (m->dirty) {
        gl.BufferData(GL_ARRAY_BUFFER_, (GLsizeiptr_)(sizeof(GfxVertex) * (size_t)m->count), m->v, GL_STATIC_DRAW_);
        m->dirty = 0;
    }
    glPushMatrix();
    glTranslatef(x / 1000.0f, y / 1000.0f, z / 1000.0f);
    glRotatef((float)ry, 0, 1, 0);
    glRotatef((float)rx, 1, 0, 0);
    glRotatef((float)rz, 0, 0, 1);
    glScalef(scale / 1000.0f, scale / 1000.0f, scale / 1000.0f);
    bind_tex((GfxTex*)tp);
    GfxShader* s = (GfxShader*)sp;
    if (s) {
        gl.UseProgram(s->prog);
        if (s->tint >= 0) gl.Uniform4f(s->tint, ((rgba >> 24) & 255) / 255.0f, ((rgba >> 16) & 255) / 255.0f, ((rgba >> 8) & 255) / 255.0f, (rgba & 255) / 255.0f);
        if (s->tex >= 0) gl.Uniform1i(s->tex, 0);
        if (s->use_tex >= 0) gl.Uniform1i(s->use_tex, tp ? 1 : 0);
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(GfxVertex), (void*)0);
    glTexCoordPointer(2, GL_FLOAT, sizeof(GfxVertex), (void*)(sizeof(float) * 3));
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(GfxVertex), (void*)(sizeof(float) * 5));
    glDrawArrays(GL_TRIANGLES, 0, m->count);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    gl.BindBuffer(GL_ARRAY_BUFFER_, 0);
    if (s) gl.UseProgram(0);
    glDisable(GL_TEXTURE_2D);
    glPopMatrix();
    g_draws++;
    return glGetError() == GL_NO_ERROR ? 0 : GFX_GRAPHICS;
}

/* OpenGL driver strings of the window's context: 0 vendor, 1 renderer, 2 version */
char* salivo_sys_gfx_info(long long win, long long which) {
    long long err = 0;
    GfxWin* w = win_get(win, &err);
    if (!w || which < 0 || which > 2 || !make_current(w)) return out_cstr("");
    static const GLenum names[] = { GL_VENDOR, GL_RENDERER, GL_VERSION };
    return out_cstr((const char*)glGetString(names[which]));
}

/* which: 0 live windows, 1 textures, 2 meshes, 3 shaders, 4 draw calls so far, 5 dropped events,
 * 6 queued events */
long long salivo_sys_gfx_stat(long long which) {
    switch (which) {
    case 0: return g_live[K_WIN];
    case 1: return g_live[K_TEX];
    case 2: return g_live[K_MESH];
    case 3: return g_live[K_SHADER];
    case 4: return g_draws;
    case 5: return g_dropped;
    case 6: return g_qlen;
    }
    return SYS_INVALID_ARG;
}

#else /* !_WIN32: the desktop platform is Windows-only for now */

long long salivo_sys_win_create(const char* t, long long w, long long h) { (void)t; (void)w; (void)h; return SYS_UNSUPPORTED; }
long long salivo_sys_win_destroy(long long w) { (void)w; return SYS_UNSUPPORTED; }
long long salivo_sys_win_show(long long w, long long m) { (void)w; (void)m; return SYS_UNSUPPORTED; }
long long salivo_sys_win_title(long long w, const char* t) { (void)w; (void)t; return SYS_UNSUPPORTED; }
long long salivo_sys_win_resize(long long w, long long a, long long b) { (void)w; (void)a; (void)b; return SYS_UNSUPPORTED; }
long long salivo_sys_win_move(long long w, long long a, long long b) { (void)w; (void)a; (void)b; return SYS_UNSUPPORTED; }
long long salivo_sys_win_int(long long w, long long a) { (void)w; (void)a; return SYS_UNSUPPORTED; }
long long salivo_sys_win_fullscreen(long long w, long long a) { (void)w; (void)a; return SYS_UNSUPPORTED; }
long long salivo_sys_win_vsync(long long w, long long a) { (void)w; (void)a; return SYS_UNSUPPORTED; }
long long salivo_sys_ev_poll(void) { return SYS_UNSUPPORTED; }
long long salivo_sys_ev_wait(long long t) { (void)t; return SYS_UNSUPPORTED; }
long long salivo_sys_ev_field(long long f) { (void)f; return SYS_UNSUPPORTED; }
long long salivo_sys_key_down(long long k) { (void)k; return SYS_UNSUPPORTED; }
long long salivo_sys_mouse(long long w) { (void)w; return SYS_UNSUPPORTED; }
long long salivo_sys_frame_begin(long long w) { (void)w; return SYS_UNSUPPORTED; }
long long salivo_sys_frame_clear(long long w, long long c) { (void)w; (void)c; return SYS_UNSUPPORTED; }
long long salivo_sys_frame_rect(long long w, long long x, long long y, long long a, long long b, long long c) { (void)w; (void)x; (void)y; (void)a; (void)b; (void)c; return SYS_UNSUPPORTED; }
long long salivo_sys_frame_pixel(long long w, long long x, long long y) { (void)w; (void)x; (void)y; return SYS_UNSUPPORTED; }
long long salivo_sys_frame_present(long long w) { (void)w; return SYS_UNSUPPORTED; }
long long salivo_sys_time_us(void) { return SYS_UNSUPPORTED; }
long long salivo_sys_cam2d(long long w, long long a, long long b, long long c, long long d) { (void)w; (void)a; (void)b; (void)c; (void)d; return SYS_UNSUPPORTED; }
long long salivo_sys_tex_create(long long w, long long a, long long b) { (void)w; (void)a; (void)b; return SYS_UNSUPPORTED; }
long long salivo_sys_tex_load(long long w, const char* p) { (void)w; (void)p; return SYS_UNSUPPORTED; }
long long salivo_sys_tex_pixel(long long t, long long x, long long y, long long c) { (void)t; (void)x; (void)y; (void)c; return SYS_UNSUPPORTED; }
long long salivo_sys_tex_size(long long t, long long w) { (void)t; (void)w; return SYS_UNSUPPORTED; }
long long salivo_sys_tex_destroy(long long t) { (void)t; return SYS_UNSUPPORTED; }
long long salivo_sys_draw_quad(long long w, long long x, long long y, long long a, long long b, long long r, long long c, long long t) { (void)w; (void)x; (void)y; (void)a; (void)b; (void)r; (void)c; (void)t; return SYS_UNSUPPORTED; }
long long salivo_sys_mesh_create(long long w) { (void)w; return SYS_UNSUPPORTED; }
long long salivo_sys_mesh_vertex(long long m, long long x, long long y, long long z, long long c, long long u, long long v) { (void)m; (void)x; (void)y; (void)z; (void)c; (void)u; (void)v; return SYS_UNSUPPORTED; }
long long salivo_sys_mesh_count(long long m) { (void)m; return SYS_UNSUPPORTED; }
long long salivo_sys_mesh_destroy(long long m) { (void)m; return SYS_UNSUPPORTED; }
long long salivo_sys_shader_create(long long w, const char* v, const char* f) { (void)w; (void)v; (void)f; return SYS_UNSUPPORTED; }
char* salivo_sys_shader_log(void) { return out_cstr(""); }
long long salivo_sys_shader_destroy(long long s) { (void)s; return SYS_UNSUPPORTED; }
long long salivo_sys_cam3d(long long w, long long a, long long b, long long c, long long d, long long e, long long f, long long g, long long h, long long i) { (void)w; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; return SYS_UNSUPPORTED; }
long long salivo_sys_draw_mesh(long long w, long long m, long long s, long long x, long long y, long long z, long long a, long long b, long long c, long long k, long long r, long long t) { (void)w; (void)m; (void)s; (void)x; (void)y; (void)z; (void)a; (void)b; (void)c; (void)k; (void)r; (void)t; return SYS_UNSUPPORTED; }
long long salivo_sys_gfx_stat(long long w) { (void)w; return SYS_UNSUPPORTED; }
char* salivo_sys_gfx_info(long long w, long long k) { (void)w; (void)k; return out_cstr(""); }

#endif
