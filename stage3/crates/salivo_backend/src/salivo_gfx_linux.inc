/* Stage 38.1 desktop canvas and Stage 38.2 2D game rendering on Linux (Salivo OS).
 *
 * Included by salivo_sys_runtime.c after salivo_gfx_runtime.inc (shares its error codes and string
 * helpers). Same ABI rows and semantics as the Win32 backend, with these platform differences:
 *  - The process owns the whole screen: KMS through a DRM dumb buffer (/dev/dri/card0), or the
 *    fbdev framebuffer (/dev/fb0) when KMS is unavailable. One window at a time; its client size is
 *    the display mode, and resize / move report -15.
 *  - Rendering is software into a cached back buffer; present copies it to the scanout buffer and
 *    draws the mouse cursor on top (SALIVO_CURSOR=0 hides it).
 *  - Input is read from every /dev/input/event* device, grabbed exclusively (keys never reach the
 *    text console). Keys map to the Win32 virtual-key codes of std/desktop.sal; text events use a
 *    US layout. Devices plugged in later are picked up within a second.
 *  - Fonts come from .ttf/.otf files (stb_truetype) or a family name searched under
 *    /usr/share/fonts; text is full Unicode. Images: PNG, JPEG, BMP, GIF, TGA (stb_image).
 *  - 3D meshes, shaders and sound report -12 for now. Text ignores the 2D camera.
 */

#if defined(__linux__)

#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <linux/input.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define STBI_STATIC
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* ---------- KMS (uapi structs: stable kernel ABI, defined here so no libdrm headers are needed) ---------- */

typedef struct { uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
                 uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
                 uint32_t min_width, max_width, min_height, max_height; } LxCardRes;
typedef struct { uint32_t clock; uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
                 uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
                 uint32_t vrefresh, flags, type; char name[32]; } LxModeInfo;
typedef struct { uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
                 uint32_t count_modes, count_props, count_encoders;
                 uint32_t encoder_id, connector_id, connector_type, connector_type_id;
                 uint32_t connection, mm_width, mm_height, subpixel, pad; } LxConnector;
typedef struct { uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones; } LxEncoder;
typedef struct { uint64_t set_connectors_ptr; uint32_t count_connectors, crtc_id, fb_id, x, y,
                 gamma_size, mode_valid; LxModeInfo mode; } LxCrtc;
typedef struct { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; } LxCreateDumb;
typedef struct { uint32_t handle, pad; uint64_t offset; } LxMapDumb;
typedef struct { uint32_t fb_id, width, height, pitch, bpp, depth, handle; } LxFbCmd;
typedef struct { uint32_t fb_id, flags, color, num_clips; uint64_t clips_ptr; } LxDirty;

#define LX_DRM_IOWR(nr, type) _IOWR('d', nr, type)
#define LX_GETRESOURCES LX_DRM_IOWR(0xA0, LxCardRes)
#define LX_GETCRTC LX_DRM_IOWR(0xA1, LxCrtc)
#define LX_SETCRTC LX_DRM_IOWR(0xA2, LxCrtc)
#define LX_GETENCODER LX_DRM_IOWR(0xA6, LxEncoder)
#define LX_GETCONNECTOR LX_DRM_IOWR(0xA7, LxConnector)
#define LX_ADDFB LX_DRM_IOWR(0xAE, LxFbCmd)
#define LX_DIRTYFB LX_DRM_IOWR(0xB1, LxDirty)
#define LX_CREATE_DUMB LX_DRM_IOWR(0xB2, LxCreateDumb)
#define LX_MAP_DUMB LX_DRM_IOWR(0xB3, LxMapDumb)
#define LX_SET_MASTER _IO('d', 0x1e)

/* ---------- display ---------- */

static struct {
    int mode;            /* 0 closed, 1 KMS, 2 fbdev */
    int fd;
    int w, h, hz;
    uint8_t* front;      /* scanout memory */
    size_t front_size;
    int pitch;           /* bytes per scanout row */
    int bpp;             /* fbdev: 16 or 32 */
    uint32_t fb_id;
    uint32_t* bb;        /* back buffer, 0x00RRGGBB */
    char name[64];
    LxCrtc saved;        /* CRTC state before we took the screen */
    int have_saved;
} g_lx;

static int lx_kms_open(void) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 0;
    ioctl(fd, LX_SET_MASTER, 0);
    LxCardRes res; memset(&res, 0, sizeof res);
    if (ioctl(fd, LX_GETRESOURCES, &res) != 0 || !res.count_connectors) { close(fd); return 0; }
    uint32_t conns[32], crtcs[32], encs[32];
    if (res.count_connectors > 32) res.count_connectors = 32;
    if (res.count_crtcs > 32) res.count_crtcs = 32;
    if (res.count_encoders > 32) res.count_encoders = 32;
    res.count_fbs = 0; res.fb_id_ptr = 0;
    res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)encs;
    if (ioctl(fd, LX_GETRESOURCES, &res) != 0) { close(fd); return 0; }
    for (uint32_t i = 0; i < res.count_connectors; i++) {
        LxConnector c; memset(&c, 0, sizeof c);
        c.connector_id = conns[i];
        if (ioctl(fd, LX_GETCONNECTOR, &c) != 0 || c.connection != 1 || !c.count_modes) continue;
        LxModeInfo* modes = (LxModeInfo*)calloc(c.count_modes, sizeof(LxModeInfo));
        uint32_t* cencs = (uint32_t*)calloc(c.count_encoders ? c.count_encoders : 1, sizeof(uint32_t));
        if (!modes || !cencs) { free(modes); free(cencs); continue; }
        c.modes_ptr = (uint64_t)(uintptr_t)modes;
        c.encoders_ptr = (uint64_t)(uintptr_t)cencs;
        c.count_props = 0; c.props_ptr = 0; c.prop_values_ptr = 0;
        if (ioctl(fd, LX_GETCONNECTOR, &c) != 0) { free(modes); free(cencs); continue; }
        LxModeInfo mode = modes[0];
        for (uint32_t m = 0; m < c.count_modes; m++) if (modes[m].type & 8) { mode = modes[m]; break; } /* DRM_MODE_TYPE_PREFERRED */
        /* pick a CRTC: the encoder's current one, else the first the encoder allows */
        uint32_t crtc_id = 0;
        for (uint32_t e = 0; e < c.count_encoders && !crtc_id; e++) {
            LxEncoder enc; memset(&enc, 0, sizeof enc);
            enc.encoder_id = cencs[e];
            if (ioctl(fd, LX_GETENCODER, &enc) != 0) continue;
            if (enc.crtc_id) { crtc_id = enc.crtc_id; break; }
            for (uint32_t k = 0; k < res.count_crtcs; k++) if (enc.possible_crtcs & (1u << k)) { crtc_id = crtcs[k]; break; }
        }
        free(modes); free(cencs);
        if (!crtc_id) continue;
        LxCreateDumb cd; memset(&cd, 0, sizeof cd);
        cd.width = mode.hdisplay; cd.height = mode.vdisplay; cd.bpp = 32;
        if (ioctl(fd, LX_CREATE_DUMB, &cd) != 0) continue;
        LxFbCmd fb; memset(&fb, 0, sizeof fb);
        fb.width = cd.width; fb.height = cd.height; fb.pitch = cd.pitch; fb.bpp = 32; fb.depth = 24; fb.handle = cd.handle;
        if (ioctl(fd, LX_ADDFB, &fb) != 0) continue;
        LxMapDumb md; memset(&md, 0, sizeof md);
        md.handle = cd.handle;
        if (ioctl(fd, LX_MAP_DUMB, &md) != 0) continue;
        void* mem = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)md.offset);
        if (mem == MAP_FAILED) continue;
        memset(&g_lx.saved, 0, sizeof g_lx.saved);
        g_lx.saved.crtc_id = crtc_id;
        g_lx.have_saved = ioctl(fd, LX_GETCRTC, &g_lx.saved) == 0;
        LxCrtc set; memset(&set, 0, sizeof set);
        set.crtc_id = crtc_id; set.fb_id = fb.fb_id; set.set_connectors_ptr = (uint64_t)(uintptr_t)&c.connector_id;
        set.count_connectors = 1; set.mode = mode; set.mode_valid = 1;
        if (ioctl(fd, LX_SETCRTC, &set) != 0) { munmap(mem, cd.size); continue; }
        g_lx.mode = 1; g_lx.fd = fd; g_lx.w = cd.width; g_lx.h = cd.height;
        g_lx.hz = mode.vrefresh ? (int)mode.vrefresh : 60;
        g_lx.front = (uint8_t*)mem; g_lx.front_size = cd.size; g_lx.pitch = (int)cd.pitch; g_lx.bpp = 32;
        g_lx.fb_id = fb.fb_id;
        snprintf(g_lx.name, sizeof g_lx.name, "KMS %dx%d@%d", g_lx.w, g_lx.h, g_lx.hz);
        return 1;
    }
    close(fd);
    return 0;
}

static int lx_fbdev_open(void) {
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 0;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) != 0 || ioctl(fd, FBIOGET_FSCREENINFO, &f) != 0 ||
        (v.bits_per_pixel != 32 && v.bits_per_pixel != 16)) { close(fd); return 0; }
    size_t size = (size_t)f.line_length * v.yres_virtual;
    void* mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) { close(fd); return 0; }
    g_lx.mode = 2; g_lx.fd = fd; g_lx.w = (int)v.xres; g_lx.h = (int)v.yres; g_lx.hz = 60;
    g_lx.front = (uint8_t*)mem; g_lx.front_size = size; g_lx.pitch = (int)f.line_length; g_lx.bpp = (int)v.bits_per_pixel;
    snprintf(g_lx.name, sizeof g_lx.name, "fbdev %dx%d", g_lx.w, g_lx.h);
    return 1;
}

static void lx_display_close(void) {
    if (!g_lx.mode) return;
    if (g_lx.mode == 1 && g_lx.have_saved && g_lx.saved.fb_id) {
        /* give the screen back to the console */
        LxCrtc c = g_lx.saved;
        c.set_connectors_ptr = 0; c.count_connectors = 0;
        ioctl(g_lx.fd, LX_SETCRTC, &c);
    }
    munmap(g_lx.front, g_lx.front_size);
    close(g_lx.fd);
    free(g_lx.bb);
    memset(&g_lx, 0, sizeof g_lx);
}

/* ---------- objects: one window, textures and fonts (generation handles like Win32) ---------- */

typedef struct { int used, gen; uint8_t* bmp; int w, h, xoff, yoff; float adv; int cp; } LxGlyph;
typedef struct {
    stbtt_fontinfo info;
    unsigned char* data;
    float scale;
    int ascent, line;            /* pixels */
    LxGlyph* cache;              /* open addressing on the code point */
    int cap, count;
} LxFont;
typedef struct { int used, gen; uint32_t* px; int w, h; LxFont* font; size_t mapped; int opaque; } LxTex;   /* px: 0xAARRGGBB; mapped: bytes of a shared file */

#define LX_TEX_MAX 4096
static LxTex g_ltex[LX_TEX_MAX];
static int g_ltex_live;

static struct {
    int used, gen, shown, in_frame, vsync;
    char title[256];
    int cam;                     /* 2D camera active */
    float cx, cy, zoom, cs, sn;  /* center, zoom, cos/sin of -rotation */
    int clip_x0, clip_y0, clip_x1, clip_y1;
    long long last_present_us;
} g_lw;

static long long g_ldraws;

static long long lx_handle(int gen, int slot) { return ((long long)gen << 16) | (slot + 1); }

static int lx_win_ok(long long win) {
    return g_lw.used && win == lx_handle(g_lw.gen, 0);
}

static LxTex* lx_tex_get(long long h) {
    long long slot = (h & 0xFFFF) - 1;
    if (h <= 0 || slot < 0 || slot >= LX_TEX_MAX) return 0;
    LxTex* t = &g_ltex[slot];
    return t->used && t->gen == (int)(h >> 16) ? t : 0;
}

static long long lx_tex_new(int w, int h, LxTex** out) {
    for (int i = 0; i < LX_TEX_MAX; i++) {
        if (g_ltex[i].used) continue;
        uint32_t* px = (uint32_t*)calloc((size_t)w * (size_t)h, 4);
        if (!px) return SYS_OS;
        LxTex* t = &g_ltex[i];
        t->used = 1; t->gen = (t->gen + 1) & 0x7FFF; if (!t->gen) t->gen = 1;
        t->px = px; t->w = w; t->h = h; t->font = 0; t->mapped = 0; t->opaque = 0;
        g_ltex_live++;
        *out = t;
        return lx_handle(t->gen, i);
    }
    return -11;
}

static void lx_font_free(LxFont* f) {
    if (!f) return;
    for (int i = 0; i < f->cap; i++) free(f->cache[i].bmp);
    free(f->cache);
    free(f->data);
    free(f);
}

static void lx_tex_free(LxTex* t) {
    if (t->mapped) munmap(t->px, t->mapped); else free(t->px);
    t->mapped = 0;
    lx_font_free(t->font);
    t->px = 0; t->font = 0; t->used = 0;
    g_ltex_live--;
}

/* ---------- time ---------- */

long long salivo_sys_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ---------- input ---------- */

#define LX_EVQ 1024
typedef struct { long long type, win, a, b, c, x, y; } LxEvent;
static LxEvent g_levq[LX_EVQ];
static int g_levhead, g_levcount;
static LxEvent g_levcur;
static long long g_ldropped;

static void lx_push(long long type, long long a, long long b, long long c, long long x, long long y) {
    if (!g_lw.used) return;
    if (g_levcount == LX_EVQ) { g_ldropped++; return; }
    LxEvent* e = &g_levq[(g_levhead + g_levcount) % LX_EVQ];
    e->type = type; e->win = lx_handle(g_lw.gen, 0); e->a = a; e->b = b; e->c = c; e->x = x; e->y = y;
    g_levcount++;
}

#define LX_DEV_MAX 32
typedef struct { int fd; char path[64]; int absx_min, absx_max, absy_min, absy_max, is_abs; } LxDev;
static LxDev g_ldev[LX_DEV_MAX];
static int g_ldevn;
static long long g_lscan_us;
static unsigned char g_lkeys[256];
static long long g_lmx, g_lmy, g_lbuttons;
static int g_lshift, g_lctrl, g_lalt, g_lcaps;

static void lx_scan_devices(void) {
    g_lscan_us = salivo_sys_time_us();
    DIR* d = opendir("/dev/input");
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) && g_ldevn < LX_DEV_MAX) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[64];
        snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
        int known = 0;
        for (int i = 0; i < g_ldevn; i++) if (!strcmp(g_ldev[i].path, path)) known = 1;
        if (known) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        ioctl(fd, EVIOCGRAB, 1);
        LxDev* v = &g_ldev[g_ldevn++];
        memset(v, 0, sizeof *v);
        v->fd = fd;
        snprintf(v->path, sizeof v->path, "%s", path);
        struct input_absinfo ax, ay;
        if (ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0 && ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0 && ax.maximum > ax.minimum) {
            v->is_abs = 1;
            v->absx_min = ax.minimum; v->absx_max = ax.maximum;
            v->absy_min = ay.minimum; v->absy_max = ay.maximum;
        }
    }
    closedir(d);
}

static void lx_close_devices(void) {
    for (int i = 0; i < g_ldevn; i++) close(g_ldev[i].fd);
    g_ldevn = 0;
}

/* evdev key code -> Win32 virtual key */
static int lx_vk(int code) {
    static const unsigned char row1[] = { '1','2','3','4','5','6','7','8','9','0', 0xBD, 0xBB };   /* 2..13 */
    static const char row2[] = "QWERTYUIOP";      /* 16..25 */
    static const char row3[] = "ASDFGHJKL";       /* 30..38 */
    static const char row4[] = "ZXCVBNM";         /* 44..50 */
    if (code >= 2 && code <= 13) return row1[code - 2];
    if (code >= 16 && code <= 25) return row2[code - 16];
    if (code >= 30 && code <= 38) return row3[code - 30];
    if (code >= 44 && code <= 50) return row4[code - 44];
    if (code >= KEY_F1 && code <= KEY_F10) return 112 + code - KEY_F1;
    switch (code) {
    case KEY_ESC: return 27; case KEY_BACKSPACE: return 8; case KEY_TAB: return 9;
    case KEY_LEFTBRACE: return 0xDB; case KEY_RIGHTBRACE: return 0xDD;
    case KEY_ENTER: case KEY_KPENTER: return 13;
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: return 17;
    case KEY_SEMICOLON: return 0xBA; case KEY_APOSTROPHE: return 0xDE; case KEY_GRAVE: return 0xC0;
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: return 16;
    case KEY_BACKSLASH: return 0xDC; case KEY_COMMA: return 0xBC; case KEY_DOT: return 0xBE; case KEY_SLASH: return 0xBF;
    case KEY_KPASTERISK: return 0x6A; case KEY_LEFTALT: case KEY_RIGHTALT: return 18;
    case KEY_SPACE: return 32; case KEY_CAPSLOCK: return 20;
    case KEY_NUMLOCK: return 0x90; case KEY_SCROLLLOCK: return 0x91;
    case KEY_KP7: return 0x67; case KEY_KP8: return 0x68; case KEY_KP9: return 0x69; case KEY_KPMINUS: return 0x6D;
    case KEY_KP4: return 0x64; case KEY_KP5: return 0x65; case KEY_KP6: return 0x66; case KEY_KPPLUS: return 0x6B;
    case KEY_KP1: return 0x61; case KEY_KP2: return 0x62; case KEY_KP3: return 0x63; case KEY_KP0: return 0x60;
    case KEY_KPDOT: return 0x6E; case KEY_KPSLASH: return 0x6F;
    case KEY_F11: return 122; case KEY_F12: return 123; case KEY_SYSRQ: return 0x2C;
    case KEY_HOME: return 36; case KEY_UP: return 38; case KEY_PAGEUP: return 33; case KEY_LEFT: return 37;
    case KEY_RIGHT: return 39; case KEY_END: return 35; case KEY_DOWN: return 40; case KEY_PAGEDOWN: return 34;
    case KEY_INSERT: return 45; case KEY_DELETE: return 46;
    case KEY_LEFTMETA: return 0x5B; case KEY_RIGHTMETA: return 0x5C; case KEY_COMPOSE: return 0x5D;
    default: return 0;
    }
}

/* typed character for a key press (US layout), 0 when the key types nothing */
static int lx_char(int code) {
    static const char lo1[] = "1234567890-=", hi1[] = "!@#$%^&*()_+";   /* 2..13 */
    static const char lo2[] = "qwertyuiop[]", hi2[] = "QWERTYUIOP{}";   /* 16..27 */
    static const char lo3[] = "asdfghjkl;'`", hi3[] = "ASDFGHJKL:\"~";  /* 30..41 */
    static const char lo4[] = "\\zxcvbnm,./", hi4[] = "|ZXCVBNM<>?";    /* 43..53 */
    int c = 0;
    if (code >= 2 && code <= 13) c = g_lshift ? hi1[code - 2] : lo1[code - 2];
    else if (code >= 16 && code <= 27) c = g_lshift ? hi2[code - 16] : lo2[code - 16];
    else if (code >= 30 && code <= 41) c = g_lshift ? hi3[code - 30] : lo3[code - 30];
    else if (code >= 43 && code <= 53) c = g_lshift ? hi4[code - 43] : lo4[code - 43];
    else if (code == KEY_SPACE) c = ' ';
    else if (code == KEY_ENTER || code == KEY_KPENTER) return 13;
    else if (code == KEY_BACKSPACE) return 8;
    else if (code == KEY_TAB) return 9;
    else if (code == KEY_ESC) return 27;
    if (c >= 'a' && c <= 'z' && g_lcaps) c -= 32;
    else if (c >= 'A' && c <= 'Z' && g_lcaps) c += 32;
    if (g_lctrl && c) {
        if (c >= 'a' && c <= 'z') return c - 'a' + 1;
        if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
        return 0;
    }
    return c;
}

static void lx_clamp_mouse(void) {
    if (g_lmx < 0) g_lmx = 0;
    if (g_lmy < 0) g_lmy = 0;
    if (g_lmx >= g_lx.w) g_lmx = g_lx.w - 1;
    if (g_lmy >= g_lx.h) g_lmy = g_lx.h - 1;
}

static void lx_read_input(void) {
    if (salivo_sys_time_us() - g_lscan_us > 1000000) lx_scan_devices();
    for (int i = 0; i < g_ldevn; i++) {
        struct input_event ev[64];
        long long rx = 0, ry = 0;
        int moved = 0;
        for (;;) {
            ssize_t n = read(g_ldev[i].fd, ev, sizeof ev);
            if (n <= 0) {
                if (n < 0 && errno == ENODEV) {      /* unplugged */
                    close(g_ldev[i].fd);
                    g_ldev[i] = g_ldev[--g_ldevn];
                    i--;
                }
                break;
            }
            for (size_t k = 0; k < (size_t)n / sizeof ev[0]; k++) {
                struct input_event* e = &ev[k];
                if (e->type == EV_KEY) {
                    int code = e->code;
                    if (code == BTN_LEFT || code == BTN_RIGHT || code == BTN_MIDDLE || code == BTN_TOUCH) {
                        int b = code == BTN_RIGHT ? 2 : code == BTN_MIDDLE ? 3 : 1;
                        if (e->value) g_lbuttons |= 1LL << b; else g_lbuttons &= ~(1LL << b);
                        lx_push(e->value ? 9 : 10, b, 0, 0, g_lmx, g_lmy);
                        continue;
                    }
                    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) g_lshift = e->value != 0;
                    if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) g_lctrl = e->value != 0;
                    if (code == KEY_LEFTALT || code == KEY_RIGHTALT) g_lalt = e->value != 0;
                    if (code == KEY_CAPSLOCK && e->value == 1) g_lcaps = !g_lcaps;
                    int vk = lx_vk(code);
                    if (!vk) continue;
                    if (e->value) {
                        g_lkeys[vk] = 1;
                        lx_push(5, vk, code, e->value == 2, 0, 0);
                        int ch = lx_char(code);
                        if (ch && !g_lalt) lx_push(7, ch, 0, 0, 0, 0);
                    } else {
                        g_lkeys[vk] = 0;
                        lx_push(6, vk, code, 0, 0, 0);
                    }
                } else if (e->type == EV_REL) {
                    if (e->code == REL_X) { rx += e->value; moved = 1; }
                    else if (e->code == REL_Y) { ry += e->value; moved = 1; }
                    else if (e->code == REL_WHEEL) lx_push(11, e->value * 120, 0, 0, g_lmx, g_lmy);
                } else if (e->type == EV_ABS && g_ldev[i].is_abs) {
                    LxDev* v = &g_ldev[i];
                    if (e->code == ABS_X) { g_lmx = (long long)(e->value - v->absx_min) * (g_lx.w - 1) / (v->absx_max - v->absx_min); moved = 1; }
                    else if (e->code == ABS_Y) { g_lmy = (long long)(e->value - v->absy_min) * (g_lx.h - 1) / (v->absy_max - v->absy_min); moved = 1; }
                } else if (e->type == EV_SYN && moved) {
                    g_lmx += rx; g_lmy += ry; rx = ry = 0;
                    lx_clamp_mouse();
                    lx_push(8, 0, 0, 0, g_lmx, g_lmy);
                    moved = 0;
                }
            }
        }
    }
}

long long salivo_sys_ev_poll(void) {
    if (!g_lw.used) return 0;
    lx_read_input();
    if (!g_levcount) return 0;
    g_levcur = g_levq[g_levhead];
    g_levhead = (g_levhead + 1) % LX_EVQ;
    g_levcount--;
    return g_levcur.type;
}

long long salivo_sys_ev_wait(long long timeout_ms) {
    if (!g_lw.used) return 0;
    long long t = salivo_sys_ev_poll();
    if (t || timeout_ms <= 0) return t;
    struct pollfd p[LX_DEV_MAX + SYS_PTYS];
    int ptyOf[LX_DEV_MAX + SYS_PTYS];
    int n = 0;
    for (int i = 0; i < g_ldevn; i++) { p[n].fd = g_ldev[i].fd; p[n].events = POLLIN; p[n].revents = 0; ptyOf[n++] = 0; }
    /* programs on pseudo-terminals (terminals, the browser engine) wake the loop when they write;
       one that hung up is left out until it is closed, so it cannot spin the loop */
    for (int i = 1; i <= SYS_PTYS; i++) {
        if (g_pty[i].fd <= 0 || g_pty[i].hup) continue;
        p[n].fd = g_pty[i].fd; p[n].events = POLLIN; p[n].revents = 0; ptyOf[n++] = i;
    }
    /* wake at least once a second to pick up newly plugged devices */
    poll(p, (nfds_t)n, (int)(timeout_ms > 1000 ? 1000 : timeout_ms));
    for (int i = 0; i < n; i++) if (ptyOf[i] && (p[i].revents & (POLLHUP | POLLERR)) && !(p[i].revents & POLLIN)) g_pty[ptyOf[i]].hup = 1;
    return salivo_sys_ev_poll();
}

long long salivo_sys_ev_field(long long field) {
    switch (field) {
    case 0: return g_levcur.win; case 1: return g_levcur.a; case 2: return g_levcur.b;
    case 3: return g_levcur.c; case 4: return g_levcur.x; case 5: return g_levcur.y;
    default: return SYS_INVALID_ARG;
    }
}

long long salivo_sys_key_down(long long vk) {
    if (vk < 0 || vk > 255) return SYS_INVALID_ARG;
    return g_lkeys[vk];
}

long long salivo_sys_mouse(long long which) {
    switch (which) {
    case 0: return g_lmx; case 1: return g_lmy; case 2: return g_lbuttons;
    case 3: return g_lw.used ? lx_handle(g_lw.gen, 0) : 0;
    default: return SYS_INVALID_ARG;
    }
}

/* ---------- windows ---------- */

static void lx_atexit(void) {
    lx_close_devices();
    lx_display_close();
}

long long salivo_sys_win_create(const char* title, long long width, long long height) {
    if (width < 1 || height < 1 || width > 16384 || height > 16384) return SYS_INVALID_ARG;
    if (g_lw.used) return -11;   /* the process owns the screen: one window */
    if (!g_lx.mode && !lx_kms_open() && !lx_fbdev_open()) return SYS_UNSUPPORTED;
    if (!g_lx.bb) {
        g_lx.bb = (uint32_t*)calloc((size_t)g_lx.w * (size_t)g_lx.h, 4);
        if (!g_lx.bb) return SYS_OS;
        static int registered;
        if (!registered) { atexit(lx_atexit); registered = 1; }
    }
    g_lw.used = 1; g_lw.gen = (g_lw.gen + 1) & 0x7FFF; if (!g_lw.gen) g_lw.gen = 1;
    g_lw.shown = 0; g_lw.in_frame = 0; g_lw.vsync = 1; g_lw.cam = 0;
    snprintf(g_lw.title, sizeof g_lw.title, "%s", title ? title : "");
    g_lmx = g_lx.w / 2; g_lmy = g_lx.h / 2;
    lx_scan_devices();
    return lx_handle(g_lw.gen, 0);
}

long long salivo_sys_win_destroy(long long win) {
    if (!lx_win_ok(win)) return SYS_INVALID_HANDLE;
    for (int i = 0; i < LX_TEX_MAX; i++) if (g_ltex[i].used) lx_tex_free(&g_ltex[i]);
    g_lw.used = 0;
    g_levcount = 0;
    lx_close_devices();
    lx_display_close();
    return 0;
}

long long salivo_sys_win_show(long long win, long long mode) {
    if (!lx_win_ok(win)) return SYS_INVALID_HANDLE;
    if (mode < 0 || mode > 5) return SYS_INVALID_ARG;
    g_lw.shown = mode != 0;
    if (mode == 1 || mode == 5) lx_push(4, 1, 0, 0, 0, 0);
    return 0;
}

long long salivo_sys_win_title(long long win, const char* title) {
    if (!lx_win_ok(win)) return SYS_INVALID_HANDLE;
    snprintf(g_lw.title, sizeof g_lw.title, "%s", title ? title : "");
    return 0;
}

long long salivo_sys_win_resize(long long win, long long a, long long b) { (void)a; (void)b; return lx_win_ok(win) ? GFX_STATE : SYS_INVALID_HANDLE; }
long long salivo_sys_win_move(long long win, long long a, long long b) { (void)a; (void)b; return lx_win_ok(win) ? GFX_STATE : SYS_INVALID_HANDLE; }
long long salivo_sys_win_fullscreen(long long win, long long on) { (void)on; return lx_win_ok(win) ? 0 : SYS_INVALID_HANDLE; }

long long salivo_sys_win_vsync(long long win, long long on) {
    if (!lx_win_ok(win)) return SYS_INVALID_HANDLE;
    g_lw.vsync = on != 0;
    return 0;
}

long long salivo_sys_win_int(long long win, long long which) {
    if (!lx_win_ok(win)) return SYS_INVALID_HANDLE;
    switch (which) {
    case 0: return g_lx.w; case 1: return g_lx.h; case 2: return 0; case 3: return 0;
    case 4: return g_lw.shown; case 5: return 1; case 6: return 0; case 7: return 1;
    case 8: return 1000; case 9: return g_lw.in_frame;
    default: return SYS_INVALID_ARG;
    }
}

/* ---------- software rasterizer ---------- */

static inline void lx_blend(uint32_t* d, uint32_t rgb, int a) {
    if (a >= 255) { *d = rgb; return; }
    if (a <= 0) return;
    uint32_t s = *d;
    uint32_t rb = ((((rgb & 0xFF00FF) * (uint32_t)a) + ((s & 0xFF00FF) * (uint32_t)(255 - a))) >> 8) & 0xFF00FF;
    uint32_t g = ((((rgb & 0x00FF00) * (uint32_t)a) + ((s & 0x00FF00) * (uint32_t)(255 - a))) >> 8) & 0x00FF00;
    *d = rb | g;
}

static int lx_frame(long long win, long long* err) {
    if (!lx_win_ok(win)) { *err = SYS_INVALID_HANDLE; return 0; }
    if (!g_lw.in_frame) { *err = GFX_STATE; return 0; }
    return 1;
}

/* screen-space axis-aligned fill, clipped */
static void lx_fill(int x0, int y0, int x1, int y1, uint32_t rgb, int a) {
    if (x0 < g_lw.clip_x0) x0 = g_lw.clip_x0;
    if (y0 < g_lw.clip_y0) y0 = g_lw.clip_y0;
    if (x1 > g_lw.clip_x1) x1 = g_lw.clip_x1;
    if (y1 > g_lw.clip_y1) y1 = g_lw.clip_y1;
    if (x0 >= x1 || y0 >= y1 || a <= 0) return;
    for (int y = y0; y < y1; y++) {
        uint32_t* row = g_lx.bb + (size_t)y * g_lx.w;
        if (a >= 255) { for (int x = x0; x < x1; x++) row[x] = rgb; }
        else { for (int x = x0; x < x1; x++) lx_blend(&row[x], rgb, a); }
    }
}

static void lx_world_to_screen(float wx, float wy, float* sx, float* sy) {
    if (!g_lw.cam) { *sx = wx; *sy = wy; return; }
    float dx = (wx - g_lw.cx) * g_lw.zoom, dy = (wy - g_lw.cy) * g_lw.zoom;
    *sx = g_lx.w / 2.0f + dx * g_lw.cs - dy * g_lw.sn;
    *sy = g_lx.h / 2.0f + dx * g_lw.sn + dy * g_lw.cs;
}

/* Draws a w x h quad centered on (cx, cy) in world space, rotated rot degrees, tinted rgba, sampling
 * texture t's rectangle (sx, sy, sw, sh) with nearest filtering (no texture: solid tint). */
static void lx_quad(float cx, float cy, float w, float h, float rot, uint32_t rgba, LxTex* t, int sx, int sy, int sw, int sh) {
    int flipx = w < 0, flipy = h < 0;
    if (flipx) w = -w;
    if (flipy) h = -h;
    float scx, scy;
    lx_world_to_screen(cx, cy, &scx, &scy);
    float zoom = g_lw.cam ? g_lw.zoom : 1.0f;
    float sw_ = w * zoom, sh_ = h * zoom;
    /* total screen rotation: the quad's own plus the camera's */
    float ang = rot * 3.14159265f / 180.0f;
    float ca = cosf(ang), sa = sinf(ang);
    if (g_lw.cam) { float c2 = ca * g_lw.cs - sa * g_lw.sn, s2 = sa * g_lw.cs + ca * g_lw.sn; ca = c2; sa = s2; }
    int tr = (int)(rgba >> 24) & 255, tg = (int)(rgba >> 16) & 255, tb = (int)(rgba >> 8) & 255, ta = (int)rgba & 255;
    if (ta == 0 || sw_ < 0.5f || sh_ < 0.5f) return;
    int axis = fabsf(sa) < 1e-6f && ca > 0;
    if (axis && !t) {
        int x0 = (int)floorf(scx - sw_ / 2 + 0.5f), y0 = (int)floorf(scy - sh_ / 2 + 0.5f);
        lx_fill(x0, y0, x0 + (int)(sw_ + 0.5f), y0 + (int)(sh_ + 0.5f), (uint32_t)((tr << 16) | (tg << 8) | tb), ta);
        return;
    }
    /* bounding box of the rotated quad */
    float hx = sw_ / 2, hy = sh_ / 2;
    float ex = fabsf(ca) * hx + fabsf(sa) * hy, ey = fabsf(sa) * hx + fabsf(ca) * hy;
    int x0 = (int)floorf(scx - ex), x1 = (int)ceilf(scx + ex), y0 = (int)floorf(scy - ey), y1 = (int)ceilf(scy + ey);
    if (x0 < g_lw.clip_x0) x0 = g_lw.clip_x0;
    if (y0 < g_lw.clip_y0) y0 = g_lw.clip_y0;
    if (x1 > g_lw.clip_x1) x1 = g_lw.clip_x1;
    if (y1 > g_lw.clip_y1) y1 = g_lw.clip_y1;
    int plain = tr == 255 && tg == 255 && tb == 255 && ta == 255;
    for (int y = y0; y < y1; y++) {
        uint32_t* row = g_lx.bb + (size_t)y * g_lx.w;
        float py = y + 0.5f - scy;
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f - scx;
            /* screen -> quad-local (inverse rotation) */
            float lx = px * ca + py * sa, ly = -px * sa + py * ca;
            if (lx < -hx || lx >= hx || ly < -hy || ly >= hy) continue;
            if (!t) { lx_blend(&row[x], (uint32_t)((tr << 16) | (tg << 8) | tb), ta); continue; }
            float u = (lx + hx) / sw_, v = (ly + hy) / sh_;
            if (flipx) u = 1 - u;
            if (flipy) v = 1 - v;
            int tx = sx + (int)(u * sw), ty = sy + (int)(v * sh);
            if (tx >= sx + sw) tx = sx + sw - 1;
            if (ty >= sy + sh) ty = sy + sh - 1;
            uint32_t p = t->px[(size_t)ty * t->w + tx];
            int a = (int)(p >> 24);
            if (!a) continue;
            if (plain) { lx_blend(&row[x], p & 0xFFFFFF, a); continue; }
            int r = (int)((p >> 16) & 255) * tr / 255, g = (int)((p >> 8) & 255) * tg / 255, b = (int)(p & 255) * tb / 255;
            lx_blend(&row[x], (uint32_t)((r << 16) | (g << 8) | b), a * ta / 255);
        }
    }
}

long long salivo_sys_frame_begin(long long win) {
    if (!lx_win_ok(win)) return SYS_INVALID_HANDLE;
    if (g_lw.in_frame) return GFX_STATE;
    g_lw.in_frame = 1;
    g_lw.cam = 0;
    g_lw.clip_x0 = 0; g_lw.clip_y0 = 0; g_lw.clip_x1 = g_lx.w; g_lw.clip_y1 = g_lx.h;
    return 0;
}

long long salivo_sys_frame_clear(long long win, long long rgba) {
    long long err = 0;
    if (!lx_frame(win, &err)) return err;
    uint32_t rgb = (uint32_t)(rgba >> 8) & 0xFFFFFF;
    size_t n = (size_t)g_lx.w * g_lx.h;
    for (size_t i = 0; i < n; i++) g_lx.bb[i] = rgb;
    return 0;
}

long long salivo_sys_frame_rect(long long win, long long x, long long y, long long wd, long long ht, long long rgba) {
    long long err = 0;
    if (!lx_frame(win, &err)) return err;
    if (wd < 0 || ht < 0 || wd > 1 << 20 || ht > 1 << 20) return SYS_INVALID_ARG;
    g_ldraws++;
    if (!g_lw.cam) {
        lx_fill((int)x, (int)y, (int)(x + wd), (int)(y + ht), (uint32_t)(rgba >> 8) & 0xFFFFFF, (int)(rgba & 255));
        return 0;
    }
    lx_quad(x + wd / 2.0f, y + ht / 2.0f, (float)wd, (float)ht, 0, (uint32_t)rgba, 0, 0, 0, 0, 0);
    return 0;
}

/* Anti-aliased filled rounded rectangle in screen pixels (ignores the 2D camera). */
long long salivo_sys_frame_round(long long win, long long x, long long y, long long wd, long long ht, long long radius, long long rgba) {
    long long err = 0;
    if (!lx_frame(win, &err)) return err;
    if (wd < 0 || ht < 0 || radius < 0 || wd > 1 << 20 || ht > 1 << 20) return SYS_INVALID_ARG;
    g_ldraws++;
    float r = (float)radius;
    if (r > wd / 2.0f) r = wd / 2.0f;
    if (r > ht / 2.0f) r = ht / 2.0f;
    uint32_t rgb = (uint32_t)(rgba >> 8) & 0xFFFFFF;
    int a = (int)(rgba & 255);
    int ri = (int)ceilf(r);
    /* the cross-shaped interior has full coverage */
    lx_fill((int)x, (int)y + ri, (int)(x + wd), (int)(y + ht) - ri, rgb, a);
    lx_fill((int)x + ri, (int)y, (int)(x + wd) - ri, (int)y + ri, rgb, a);
    lx_fill((int)x + ri, (int)(y + ht) - ri, (int)(x + wd) - ri, (int)(y + ht), rgb, a);
    if (ri <= 0) return 0;
    /* four corners: coverage from the distance to the corner circle */
    for (int corner = 0; corner < 4; corner++) {
        int bx = corner & 1 ? (int)(x + wd) - ri : (int)x;
        int by = corner & 2 ? (int)(y + ht) - ri : (int)y;
        float ccx = corner & 1 ? (float)(x + wd) - r : (float)x + r;
        float ccy = corner & 2 ? (float)(y + ht) - r : (float)y + r;
        for (int py = by; py < by + ri; py++) {
            if (py < g_lw.clip_y0 || py >= g_lw.clip_y1) continue;
            uint32_t* row = g_lx.bb + (size_t)py * g_lx.w;
            for (int px = bx; px < bx + ri; px++) {
                if (px < g_lw.clip_x0 || px >= g_lw.clip_x1) continue;
                float dx = px + 0.5f - ccx, dy = py + 0.5f - ccy;
                /* pixels beyond the circle center along an axis are inside the straight edge */
                if ((corner & 1 ? dx < 0 : dx > 0)) dx = 0;
                if ((corner & 2 ? dy < 0 : dy > 0)) dy = 0;
                float cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
                if (cov <= 0) continue;
                if (cov > 1) cov = 1;
                lx_blend(&row[px], rgb, (int)(a * cov));
            }
        }
    }
    return 0;
}

/* Anti-aliased circle outline (annulus) in screen pixels; thickness >= radius fills the disc. */
long long salivo_sys_frame_ring(long long win, long long cx, long long cy, long long radius, long long thick, long long rgba) {
    long long err = 0;
    if (!lx_frame(win, &err)) return err;
    if (radius < 0 || thick < 1 || radius > 1 << 16) return SYS_INVALID_ARG;
    g_ldraws++;
    float ro = (float)radius, ri = (float)(radius - thick);
    uint32_t rgb = (uint32_t)(rgba >> 8) & 0xFFFFFF;
    int a = (int)(rgba & 255);
    int x0 = (int)(cx - radius - 1), x1 = (int)(cx + radius + 2), y0 = (int)(cy - radius - 1), y1 = (int)(cy + radius + 2);
    if (x0 < g_lw.clip_x0) x0 = g_lw.clip_x0;
    if (y0 < g_lw.clip_y0) y0 = g_lw.clip_y0;
    if (x1 > g_lw.clip_x1) x1 = g_lw.clip_x1;
    if (y1 > g_lw.clip_y1) y1 = g_lw.clip_y1;
    for (int y = y0; y < y1; y++) {
        uint32_t* row = g_lx.bb + (size_t)y * g_lx.w;
        float dy = y + 0.5f - (float)cy;
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - (float)cx;
            float d = sqrtf(dx * dx + dy * dy);
            float outer = ro + 0.5f - d;
            if (outer <= 0) continue;
            if (outer > 1) outer = 1;
            float inner = ri <= 0 ? 1.0f : d - ri + 0.5f;
            if (inner <= 0) continue;
            if (inner > 1) inner = 1;
            lx_blend(&row[x], rgb, (int)(a * outer * inner));
        }
    }
    return 0;
}

/* Clip rectangle in screen pixels for the rest of the frame; a negative width or height clears it. */
long long salivo_sys_frame_clip(long long win, long long x, long long y, long long wd, long long ht) {
    long long err = 0;
    if (!lx_frame(win, &err)) return err;
    if (wd < 0 || ht < 0) { g_lw.clip_x0 = 0; g_lw.clip_y0 = 0; g_lw.clip_x1 = g_lx.w; g_lw.clip_y1 = g_lx.h; return 0; }
    long long x1 = x + wd, y1 = y + ht;
    g_lw.clip_x0 = (int)(x < 0 ? 0 : x > g_lx.w ? g_lx.w : x);
    g_lw.clip_y0 = (int)(y < 0 ? 0 : y > g_lx.h ? g_lx.h : y);
    g_lw.clip_x1 = (int)(x1 < g_lw.clip_x0 ? g_lw.clip_x0 : x1 > g_lx.w ? g_lx.w : x1);
    g_lw.clip_y1 = (int)(y1 < g_lw.clip_y0 ? g_lw.clip_y0 : y1 > g_lx.h ? g_lx.h : y1);
    return 0;
}

long long salivo_sys_frame_pixel(long long win, long long x, long long y) {
    long long err = 0;
    if (!lx_frame(win, &err)) return err;
    if (x < 0 || y < 0 || x >= g_lx.w || y >= g_lx.h) return SYS_INVALID_ARG;
    return ((long long)g_lx.bb[(size_t)y * g_lx.w + x] << 8) | 255;
}

static void lx_cursor(uint8_t* front) {
    static const char* arrow[] = {
        "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X", "X........X",
        "X.....XXXXX", "X..X..X", "X.X X..X", "XX  X..X", "X    X..X", "     X..X", "      XX", 0 };
    for (int r = 0; arrow[r]; r++) {
        int y = (int)g_lmy + r;
        if (y >= g_lx.h) break;
        for (int c = 0; arrow[r][c]; c++) {
            int x = (int)g_lmx + c;
            if (x >= g_lx.w || arrow[r][c] == ' ') continue;
            uint32_t col = arrow[r][c] == 'X' ? 0x000000 : 0xFFFFFF;
            uint8_t* p = front + (size_t)y * g_lx.pitch;
            if (g_lx.bpp == 32) ((uint32_t*)p)[x] = col;
            else ((uint16_t*)p)[x] = (uint16_t)(col ? 0xFFFF : 0);
        }
    }
}

long long salivo_sys_frame_present(long long win) {
    long long err = 0;
    if (!lx_frame(win, &err)) return err;
    g_lw.in_frame = 0;
    if (g_lw.vsync && g_lx.hz > 0) {   /* pace to the display refresh */
        long long period = 1000000 / g_lx.hz, now = salivo_sys_time_us();
        long long wait = g_lw.last_present_us + period - now;
        if (wait > 0 && wait <= period) { struct timespec ts = { 0, (long)(wait * 1000) }; nanosleep(&ts, 0); }
    }
    g_lw.last_present_us = salivo_sys_time_us();
    for (int y = 0; y < g_lx.h; y++) {
        uint32_t* src = g_lx.bb + (size_t)y * g_lx.w;
        uint8_t* dst = g_lx.front + (size_t)y * g_lx.pitch;
        if (g_lx.bpp == 32) memcpy(dst, src, (size_t)g_lx.w * 4);
        else for (int x = 0; x < g_lx.w; x++) {
            uint32_t p = src[x];
            ((uint16_t*)dst)[x] = (uint16_t)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
        }
    }
    const char* cur = getenv("SALIVO_CURSOR");
    if (!cur || strcmp(cur, "0") != 0) lx_cursor(g_lx.front);
    if (g_lx.mode == 1) {
        LxDirty d; memset(&d, 0, sizeof d);
        d.fb_id = g_lx.fb_id;
        ioctl(g_lx.fd, LX_DIRTYFB, &d);   /* virtual GPUs copy on dirty; real ones scan out directly */
    }
    return 0;
}

/* ---------- 2D camera ---------- */

long long salivo_sys_cam2d(long long win, long long cx, long long cy, long long zoom, long long rot) {
    long long err = 0;
    if (!lx_frame(win, &err)) return err;
    if (zoom <= 0 || zoom > 1000000) return SYS_INVALID_ARG;
    float a = (float)-rot * 3.14159265f / 180.0f;
    g_lw.cam = 1; g_lw.cx = (float)cx; g_lw.cy = (float)cy; g_lw.zoom = zoom / 1000.0f;
    g_lw.cs = cosf(a); g_lw.sn = sinf(a);
    return 0;
}

long long salivo_sys_cam_screen(long long win) {
    long long err = 0;
    if (!lx_frame(win, &err)) return err;
    g_lw.cam = 0;
    return 0;
}

/* ---------- textures ---------- */

long long salivo_sys_tex_create(long long win, long long wd, long long ht) {
    if (!lx_win_ok(win)) return SYS_INVALID_HANDLE;
    if (wd < 1 || ht < 1 || wd > 16384 || ht > 16384) return SYS_INVALID_ARG;
    LxTex* t;
    return lx_tex_new((int)wd, (int)ht, &t);
}

long long salivo_sys_tex_load(long long win, const char* path) {
    if (!lx_win_ok(win)) return SYS_INVALID_HANDLE;
    if (!path || !*path) return SYS_INVALID_ARG;
    struct stat st;
    if (stat(path, &st) != 0) return GFX_NOT_FOUND;
    int w, h, n;
    unsigned char* rgba = stbi_load(path, &w, &h, &n, 4);
    if (!rgba) return GFX_BAD_ASSET;
    LxTex* t;
    long long hnd = lx_tex_new(w, h, &t);
    if (hnd > 0) {
        for (size_t i = 0; i < (size_t)w * h; i++) {
            unsigned char* p = rgba + i * 4;
            t->px[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
    stbi_image_free(rgba);
    return hnd;
}

long long salivo_sys_tex_pixel(long long tex, long long x, long long y, long long rgba) {
    LxTex* t = lx_tex_get(tex);
    if (!t || t->font) return SYS_INVALID_HANDLE;
    if (t->mapped) return -15;
    if (x < 0 || y < 0 || x >= t->w || y >= t->h) return SYS_INVALID_ARG;
    t->px[(size_t)y * t->w + x] = ((uint32_t)(rgba & 255) << 24) | ((uint32_t)(rgba >> 8) & 0xFFFFFF);
    return 0;
}

/* A texture whose pixels are a shared-memory file another process draws into (wd * ht words,
   0xAARRGGBB, all opaque). Nothing is copied: every draw shows what the file holds now. */
long long salivo_sys_tex_shm(long long win, const char* path, long long wd, long long ht) {
    if (!lx_win_ok(win)) return SYS_INVALID_HANDLE;
    if (!path || !*path || wd < 1 || ht < 1 || wd > 16384 || ht > 16384) return SYS_INVALID_ARG;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -16;
    size_t bytes = (size_t)wd * (size_t)ht * 4;
    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < bytes) { close(fd); return -17; }
    void* m = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return SYS_OS;
    LxTex* t = 0;
    long long h = lx_tex_new(1, 1, &t);
    if (h < 0) { munmap(m, bytes); return h; }
    free(t->px);
    t->px = (uint32_t*)m; t->w = (int)wd; t->h = (int)ht; t->mapped = bytes; t->opaque = 1;
    return h;
}

long long salivo_sys_tex_size(long long tex, long long which) {
    LxTex* t = lx_tex_get(tex);
    if (!t) return SYS_INVALID_HANDLE;
    if (which == 0) return t->w;
    if (which == 1) return t->h;
    return SYS_INVALID_ARG;
}

/* New texture holding tex resized to wd x ht: area average (premultiplied) when shrinking, bilinear when growing. */
long long salivo_sys_tex_scale(long long tex, long long wd, long long ht) {
    LxTex* t = lx_tex_get(tex);
    if (!t) return SYS_INVALID_HANDLE;
    if (t->font || wd < 1 || ht < 1 || wd > 16384 || ht > 16384) return SYS_INVALID_ARG;
    LxTex* o;
    long long h = lx_tex_new((int)wd, (int)ht, &o);
    if (h < 0) return h;
    int shrink = wd <= t->w && ht <= t->h;
    for (long long y = 0; y < ht; y++) {
        for (long long x = 0; x < wd; x++) {
            uint64_t ar = 0, ag = 0, ab = 0, aa = 0, n = 0;
            if (shrink) {
                int x0 = (int)(x * t->w / wd), x1 = (int)((x + 1) * t->w / wd), y0 = (int)(y * t->h / ht), y1 = (int)((y + 1) * t->h / ht);
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                for (int sy = y0; sy < y1; sy++)
                    for (int sx = x0; sx < x1; sx++) {
                        uint32_t p = t->px[(size_t)sy * t->w + sx], a = p >> 24;
                        ar += ((p >> 16) & 255) * a; ag += ((p >> 8) & 255) * a; ab += (p & 255) * a; aa += a; n++;
                    }
            } else {
                float fx = (x + 0.5f) * t->w / wd - 0.5f, fy = (y + 0.5f) * t->h / ht - 0.5f;
                int ix = (int)floorf(fx), iy = (int)floorf(fy);
                float tx = fx - ix, ty = fy - iy;
                for (int k = 0; k < 4; k++) {
                    int sx = ix + (k & 1), sy = iy + (k >> 1);
                    if (sx < 0) sx = 0;
                    if (sy < 0) sy = 0;
                    if (sx >= t->w) sx = t->w - 1;
                    if (sy >= t->h) sy = t->h - 1;
                    float wgt = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
                    uint32_t p = t->px[(size_t)sy * t->w + sx];
                    uint64_t a = p >> 24, wq = (uint64_t)(wgt * 256 + 0.5f);
                    ar += ((p >> 16) & 255) * a * wq; ag += ((p >> 8) & 255) * a * wq; ab += (p & 255) * a * wq; aa += a * wq; n += wq;
                }
            }
            uint32_t out = 0;
            if (aa) out = (uint32_t)(ar / aa) << 16 | (uint32_t)(ag / aa) << 8 | (uint32_t)(ab / aa);
            if (n) out |= (uint32_t)(aa / n) << 24;
            o->px[(size_t)y * wd + x] = out;
        }
    }
    return h;
}

long long salivo_sys_tex_destroy(long long tex) {
    LxTex* t = lx_tex_get(tex);
    if (!t) return SYS_INVALID_HANDLE;
    lx_tex_free(t);
    return 0;
}

long long salivo_sys_draw_quad(long long win, long long x, long long y, long long wd, long long ht, long long rot, long long rgba, long long tex) {
    long long err = 0;
    if (!lx_frame(win, &err)) return err;
    LxTex* t = 0;
    if (tex) { t = lx_tex_get(tex); if (!t || t->font) return SYS_INVALID_HANDLE; }
    g_ldraws++;
    lx_quad((float)x, (float)y, (float)wd, (float)ht, (float)rot, (uint32_t)rgba, t, 0, 0, t ? t->w : 0, t ? t->h : 0);
    return 0;
}

long long salivo_sys_draw_region(long long win, long long x, long long y, long long wd, long long ht, long long rot, long long rgba,
                                 long long tex, long long sx, long long sy, long long sw, long long sh) {
    long long err = 0;
    if (!lx_frame(win, &err)) return err;
    LxTex* t = lx_tex_get(tex);
    if (!t || t->font) return SYS_INVALID_HANDLE;
    if (sx < 0 || sy < 0 || sw < 1 || sh < 1 || sx + sw > t->w || sy + sh > t->h) return SYS_INVALID_ARG;
    g_ldraws++;
    /* unscaled, unrotated, no camera: straight blit */
    if (!g_lw.cam && rot == 0 && wd == sw && ht == sh && rgba == 0xFFFFFFFFLL) {
        int x0 = (int)(x - wd / 2), y0 = (int)(y - ht / 2);
        if (t->opaque) {
            /* every pixel opaque (a page from another process): copy the visible part of each row */
            int c0 = x0 < g_lw.clip_x0 ? g_lw.clip_x0 - x0 : 0;
            int c1 = x0 + (int)sw > g_lw.clip_x1 ? g_lw.clip_x1 - x0 : (int)sw;
            if (c1 <= c0) return 0;
            for (int r = 0; r < sh; r++) {
                int py = y0 + r;
                if (py < g_lw.clip_y0 || py >= g_lw.clip_y1) continue;
                memcpy(g_lx.bb + (size_t)py * g_lx.w + x0 + c0, t->px + (size_t)(sy + r) * t->w + sx + c0, (size_t)(c1 - c0) * 4);
            }
            return 0;
        }
        for (int r = 0; r < sh; r++) {
            int py = y0 + r;
            if (py < g_lw.clip_y0 || py >= g_lw.clip_y1) continue;
            uint32_t* row = g_lx.bb + (size_t)py * g_lx.w;
            const uint32_t* src = t->px + (size_t)(sy + r) * t->w + sx;
            for (int c = 0; c < sw; c++) {
                int px = x0 + c;
                if (px < g_lw.clip_x0 || px >= g_lw.clip_x1) continue;
                uint32_t p = src[c];
                lx_blend(&row[px], p & 0xFFFFFF, (int)(p >> 24));
            }
        }
        return 0;
    }
    lx_quad((float)x, (float)y, (float)wd, (float)ht, (float)rot, (uint32_t)rgba, t, (int)sx, (int)sy, (int)sw, (int)sh);
    return 0;
}

/* ---------- fonts ---------- */

static int lx_norm(const char* s, char* out, int cap) {
    int n = 0;
    for (; *s && n < cap - 1; s++) {
        char c = *s;
        if (c == ' ' || c == '-' || c == '_') continue;
        if (c >= 'A' && c <= 'Z') c += 32;
        out[n++] = c;
    }
    out[n] = 0;
    return n;
}

/* best file under dir whose name starts with the family: "Regular" beats other styles */
static int lx_font_find(const char* dir, const char* fam, char* best, int* score, int depth) {
    DIR* d = opendir(dir);
    if (!d) return 0;
    struct dirent* de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 5) lx_font_find(path, fam, best, score, depth + 1); continue; }
        size_t len = strlen(de->d_name);
        if (len < 5) continue;
        const char* ext = de->d_name + len - 4;
        if (strcasecmp(ext, ".ttf") && strcasecmp(ext, ".otf") && strcasecmp(ext, ".ttc")) continue;
        char name[256];
        lx_norm(de->d_name, name, sizeof name);
        size_t fl = strlen(fam);
        if (strncmp(name, fam, fl) != 0) continue;
        const char* rest = name + fl;
        int s = 1;
        if (!strncmp(rest, "regular", 7) || rest[0] == '.') s = 3;
        else if (strstr(rest, "variable") || rest[0] == '[') s = 2;
        if (s > *score) { *score = s; snprintf(best, 1024, "%s", path); }
    }
    closedir(d);
    return *score > 0;
}

long long salivo_sys_font_load(long long win, const char* src, long long size) {
    if (!lx_win_ok(win)) return SYS_INVALID_HANDLE;
    if (!src || !*src || size < 4 || size > 256) return SYS_INVALID_ARG;
    char path[1024];
    struct stat st;
    if (stat(src, &st) == 0) snprintf(path, sizeof path, "%s", src);
    else {
        char fam[256];
        int score = 0;
        lx_norm(src, fam, sizeof fam);
        const char* dirs[] = { "/usr/share/fonts", "/usr/local/share/fonts", 0 };
        int found = 0;
        for (int i = 0; dirs[i] && !found; i++) found = lx_font_find(dirs[i], fam, path, &score, 0);
        if (!found) return GFX_NOT_FOUND;
    }
    FILE* f = fopen(path, "rb");
    if (!f) return GFX_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* data = n > 0 ? (unsigned char*)malloc((size_t)n) : 0;
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(data); return GFX_BAD_ASSET; }
    fclose(f);
    LxFont* font = (LxFont*)calloc(1, sizeof(LxFont));
    if (!font) { free(data); return SYS_OS; }
    font->data = data;
    if (!stbtt_InitFont(&font->info, data, stbtt_GetFontOffsetForIndex(data, 0))) { lx_font_free(font); return GFX_BAD_ASSET; }
    font->scale = stbtt_ScaleForMappingEmToPixels(&font->info, (float)size);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&font->info, &asc, &desc, &gap);
    font->ascent = (int)ceilf(asc * font->scale);
    font->line = (int)ceilf((asc - desc + gap) * font->scale);
    font->cap = 256;
    font->cache = (LxGlyph*)calloc((size_t)font->cap, sizeof(LxGlyph));
    if (!font->cache) { lx_font_free(font); return SYS_OS; }
    LxTex* t;
    long long h = lx_tex_new(1, 1, &t);
    if (h < 0) { lx_font_free(font); return h; }
    t->font = font;
    return h;
}

static LxGlyph* lx_glyph(LxFont* f, int cp) {
    if (f->count * 2 >= f->cap) {   /* grow the table */
        int ncap = f->cap * 2;
        LxGlyph* nc = (LxGlyph*)calloc((size_t)ncap, sizeof(LxGlyph));
        if (!nc) return 0;
        for (int i = 0; i < f->cap; i++) {
            if (!f->cache[i].used) continue;
            int j = (int)((unsigned)f->cache[i].cp * 2654435761u % (unsigned)ncap);
            while (nc[j].used) j = (j + 1) % ncap;
            nc[j] = f->cache[i];
        }
        free(f->cache);
        f->cache = nc;
        f->cap = ncap;
    }
    int j = (int)((unsigned)cp * 2654435761u % (unsigned)f->cap);
    while (f->cache[j].used) {
        if (f->cache[j].cp == cp) return &f->cache[j];
        j = (j + 1) % f->cap;
    }
    LxGlyph* g = &f->cache[j];
    g->used = 1; g->cp = cp;
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&f->info, cp, &adv, &lsb);
    g->adv = adv * f->scale;
    g->bmp = stbtt_GetCodepointBitmap(&f->info, f->scale, f->scale, cp, &g->w, &g->h, &g->xoff, &g->yoff);
    f->count++;
    return g;
}

static int lx_utf8(const unsigned char** s) {
    const unsigned char* p = *s;
    int c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
    else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
    else if (c >= 0xC0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
    *s = p;
    return c;
}

/* Lays out text; draws it when draw is set. Returns the widest line; *lines gets the line count. */
static int lx_text(LxFont* f, const char* text, int x, int y, uint32_t rgba, int draw, int* lines) {
    const unsigned char* s = (const unsigned char*)text;
    float pen = 0;
    int widest = 0, nlines = 1, prev = 0;
    uint32_t rgb = (rgba >> 8) & 0xFFFFFF;
    int a = (int)(rgba & 255);
    while (*s) {
        int cp = lx_utf8(&s);
        if (cp == '\n') {
            if ((int)ceilf(pen) > widest) widest = (int)ceilf(pen);
            pen = 0; prev = 0; nlines++;
            continue;
        }
        if (prev) pen += stbtt_GetCodepointKernAdvance(&f->info, prev, cp) * f->scale;
        LxGlyph* g = lx_glyph(f, cp);
        if (!g) break;
        if (draw && g->bmp) {
            int gx = x + (int)floorf(pen + 0.5f) + g->xoff;
            int gy = y + (nlines - 1) * f->line + f->ascent + g->yoff;
            for (int r = 0; r < g->h; r++) {
                int py = gy + r;
                if (py < g_lw.clip_y0 || py >= g_lw.clip_y1) continue;
                uint32_t* row = g_lx.bb + (size_t)py * g_lx.w;
                const unsigned char* src = g->bmp + (size_t)r * g->w;
                for (int c = 0; c < g->w; c++) {
                    int px = gx + c;
                    if (px < g_lw.clip_x0 || px >= g_lw.clip_x1 || !src[c]) continue;
                    lx_blend(&row[px], rgb, src[c] * a / 255);
                }
            }
        }
        pen += g->adv;
        prev = cp;
    }
    if ((int)ceilf(pen) > widest) widest = (int)ceilf(pen);
    if (lines) *lines = nlines;
    return widest;
}

long long salivo_sys_draw_text(long long win, long long font, const char* text, long long x, long long y, long long rgba) {
    long long err = 0;
    if (!lx_frame(win, &err)) return err;
    LxTex* t = lx_tex_get(font);
    if (!t) return SYS_INVALID_HANDLE;
    if (!t->font) return SYS_INVALID_ARG;
    g_ldraws++;
    return lx_text(t->font, text ? text : "", (int)x, (int)y, (uint32_t)rgba, 1, 0);
}

long long salivo_sys_text_size(long long font, const char* text, long long which) {
    LxTex* t = lx_tex_get(font);
    if (!t) return SYS_INVALID_HANDLE;
    if (!t->font) return SYS_INVALID_ARG;
    int lines = 1;
    int w = lx_text(t->font, text ? text : "", 0, 0, 0, 0, &lines);
    if (which == 0) return w;
    if (which == 1) return (long long)lines * t->font->line;
    return SYS_INVALID_ARG;
}

/* ---------- stats and info ---------- */

long long salivo_sys_gfx_stat(long long which) {
    switch (which) {
    case 0: return g_lw.used; case 1: return g_ltex_live; case 2: return 0; case 3: return 0;
    case 4: return g_ldraws; case 5: return g_ldropped; case 6: return g_levcount; case 7: return 0;
    default: return SYS_INVALID_ARG;
    }
}

char* salivo_sys_gfx_info(long long win, long long which) {
    if (!lx_win_ok(win)) return out_cstr("");
    if (which == 0) return out_cstr("Salivo");
    if (which == 1) return out_cstr(g_lx.name);
    if (which == 2) return out_cstr("software 1.0");
    return out_cstr("");
}

/* ---------- not yet on Linux: 3D and sound ---------- */

long long salivo_sys_mesh_create(long long w) { (void)w; return SYS_UNSUPPORTED; }
long long salivo_sys_mesh_vertex(long long m, long long x, long long y, long long z, long long c, long long u, long long v) { (void)m; (void)x; (void)y; (void)z; (void)c; (void)u; (void)v; return SYS_UNSUPPORTED; }
long long salivo_sys_mesh_count(long long m) { (void)m; return SYS_UNSUPPORTED; }
long long salivo_sys_mesh_destroy(long long m) { (void)m; return SYS_UNSUPPORTED; }
long long salivo_sys_shader_create(long long w, const char* v, const char* f) { (void)w; (void)v; (void)f; return SYS_UNSUPPORTED; }
char* salivo_sys_shader_log(void) { return out_cstr(""); }
long long salivo_sys_shader_destroy(long long s) { (void)s; return SYS_UNSUPPORTED; }
long long salivo_sys_cam3d(long long w, long long a, long long b, long long c, long long d, long long e, long long f, long long g, long long h, long long i) { (void)w; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; return SYS_UNSUPPORTED; }
long long salivo_sys_draw_mesh(long long w, long long m, long long s, long long x, long long y, long long z, long long a, long long b, long long c, long long k, long long r, long long t) { (void)w; (void)m; (void)s; (void)x; (void)y; (void)z; (void)a; (void)b; (void)c; (void)k; (void)r; (void)t; return SYS_UNSUPPORTED; }
long long salivo_sys_sound_load(const char* p) { (void)p; return SYS_UNSUPPORTED; }
long long salivo_sys_sound_play(long long h, long long l) { (void)h; (void)l; return SYS_UNSUPPORTED; }
long long salivo_sys_sound_stop(long long h) { (void)h; return SYS_UNSUPPORTED; }
long long salivo_sys_sound_volume(long long h, long long v) { (void)h; (void)v; return SYS_UNSUPPORTED; }
long long salivo_sys_sound_int(long long h, long long k) { (void)h; (void)k; return SYS_UNSUPPORTED; }
long long salivo_sys_sound_destroy(long long h) { (void)h; return SYS_UNSUPPORTED; }

#endif /* __linux__ */
