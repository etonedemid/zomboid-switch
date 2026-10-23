// GLFW 3.4 (LWJGL's flavour) on libnx + Mesa EGL, resolved by the Java side through lxh_dlsym().
// One window = the default NWindow; desktop GL 4.3 compatibility context (what the game's GLSL 110-430 mix needs).
// Input: controller -> GLFW gamepad/joystick 0, touchscreen -> mouse, USB keyboard -> keys/chars.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "lx.h"

typedef void (*GLFWfn)(void);
typedef struct { int width, height, redBits, greenBits, blueBits, refreshRate; } GLFWvidmode;

enum {
    GLFW_PRESS = 1, GLFW_RELEASE = 0, GLFW_FOCUSED = 0x20001, GLFW_ICONIFIED = 0x20002, GLFW_VISIBLE = 0x20004,
    GLFW_HOVERED = 0x2000B, GLFW_CLIENT_API = 0x22001, GLFW_CONTEXT_VERSION_MAJOR = 0x22002,
    GLFW_CONTEXT_VERSION_MINOR = 0x22003, GLFW_CURSOR = 0x33001, GLFW_CURSOR_NORMAL = 0x34001, GLFW_CURSOR_DISABLED = 0x34003,
    GLFW_PLATFORM_NULL = 0x60005,
};

static int W = 1280, H = 720;

// --- EGL window/context ---
typedef struct { EGLSurface surf; EGLContext ctx; int should_close; void *user; } Win;
static EGLDisplay dpy;
static Win win;
static int inited;

static struct {
    void (*key)(Win *, int, int, int, int);
    void (*chr)(Win *, unsigned);
    void (*button)(Win *, int, int, int);
    void (*cursor)(Win *, double, double);
    void (*scroll)(Win *, double, double);
    void (*size)(Win *, int, int);
    void (*fbsize)(Win *, int, int);
    void (*focus)(Win *, int);
    void (*close)(Win *);
    void (*joystick)(int, int);
    void (*error)(int, const char *);
} cb;

static int padkbm;   // see pad_kbm()
static int glfwInit(void) {
    if (inited) return 1;
    padConfigureInput(4, HidNpadStyleSet_NpadStandard);   // NPADS
    FILE *pk = fopen(LX_ROOT "/padkbm", "r");
    if (pk) { fclose(pk); padkbm = 1; }
    hidInitializeTouchScreen();
    hidInitializeKeyboard();
    inited = 1;
    lx_logf("glfw: init\n");
    return 1;
}
static void glfwTerminate(void) {
    if (dpy) { eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglTerminate(dpy); dpy = NULL; }
    inited = 0;
}

static Win *glfwCreateWindow(int w, int h, const char *title, void *monitor, void *share) {
    (void)w; (void)h; (void)monitor; (void)share;
    lx_logf("glfw: create window '%s' %dx%d (using %dx%d)\n", title ? title : "", w, h, W, H);
    NWindow *nw = nwindowGetDefault();
    nwindowSetDimensions(nw, W, H);
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!dpy || !eglInitialize(dpy, NULL, NULL)) { lx_logf("glfw: eglInitialize failed %#x\n", eglGetError()); return NULL; }
    eglBindAPI(EGL_OPENGL_API);
    EGLConfig cfg; EGLint n;
    static const EGLint attrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                    EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    if (!eglChooseConfig(dpy, attrs, &cfg, 1, &n) || !n) { lx_logf("glfw: no EGL config %#x\n", eglGetError()); return NULL; }
    win.surf = eglCreateWindowSurface(dpy, cfg, nw, NULL);
    if (!win.surf) { lx_logf("glfw: eglCreateWindowSurface %#x\n", eglGetError()); return NULL; }
    static const EGLint ctxattrs[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT, EGL_NONE };
    win.ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattrs);
    if (!win.ctx) {  // no explicit compat-profile support: the default desktop context is the highest compat version anyway
        lx_logf("glfw: 4.3 compat context refused (%#x), trying default\n", eglGetError());
        win.ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, NULL);
    }
    if (!win.ctx) { lx_logf("glfw: eglCreateContext %#x\n", eglGetError()); return NULL; }
    win.should_close = 0;
    return &win;
}
static void glfwDestroyWindow(Win *w) {
    if (!w || !dpy) return;
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, w->ctx); eglDestroySurface(dpy, w->surf);
}
static __thread Win *current;
// GL debug output (debug; on when <root>/gldebug exists): Mesa's error messages name the failing call and argument
static void gl_debug_cb(unsigned src, unsigned type, unsigned id, unsigned sev, int len, const char *msg, const void *u) {
    static int n;
    (void)src; (void)id; (void)sev; (void)len; (void)u;
    if (type == 0x824C && n++ < 300) lx_logf("gl error: %s\n", msg);   // GL_DEBUG_TYPE_ERROR
}
static void gl_debug_on(void) {
    static int done;
    if (done++) return;
    FILE *f = fopen(LX_ROOT "/gldebug", "r");
    if (!f) return;
    fclose(f);
    void (*enable)(unsigned) = (void *)eglGetProcAddress("glEnable");
    void (*cb)(void *, const void *) = (void *)eglGetProcAddress("glDebugMessageCallback");
    if (!enable || !cb) return;
    enable(0x92E0); enable(0x8242);   // GL_DEBUG_OUTPUT, GL_DEBUG_OUTPUT_SYNCHRONOUS
    cb((void *)gl_debug_cb, NULL);
    lx_logf("glfw: GL debug output on\n");
}
static void glfwMakeContextCurrent(Win *w) {
    current = w;
    if (!dpy) return;
    if (!eglMakeCurrent(dpy, w ? w->surf : EGL_NO_SURFACE, w ? w->surf : EGL_NO_SURFACE, w ? w->ctx : EGL_NO_CONTEXT))
        lx_logf("glfw: eglMakeCurrent %#x\n", eglGetError());
    else if (w) gl_debug_on();
}
static Win *glfwGetCurrentContext(void) { return current; }
static int tracing, ntrace;   // defined with the GL tracer below
static void glfwSwapBuffers(Win *w) {
    static unsigned swaps;
    extern volatile unsigned long lx_frames, lx_frame_max_us; lx_frames++;
    static u64 last_tick;   // longest frame per stats period: stutter shows up here, not in the average
    u64 now_tick = armGetSystemTick();
    if (last_tick) { unsigned long us = armTicksToNs(now_tick - last_tick) / 1000; if (us > lx_frame_max_us) lx_frame_max_us = us; }
    last_tick = now_tick;
    if (tracing > 0 && (swaps < 5 || swaps % 300 == 0)) lx_logf("glfw: swap %u (%d GL functions traced)\n", swaps, ntrace);
    swaps++;
    extern volatile u64 lx_last_swap; if (swaps > 30) lx_last_swap = armGetSystemTick();   // armed once frames flow
    if (dpy && w) eglSwapBuffers(dpy, w->surf);
}
static void glfwSwapInterval(int i) { if (dpy) eglSwapInterval(dpy, i); }
// GL tracing (debug; on when sdmc:/pz/gltrace exists): every GL function handed out goes through a stub in lx_gltrace.s
// and its first call is logged, so a silent kill still shows the last new GL entry point the game reached.
#define NTRACE 4096
volatile u64 lx_gl_last_tick;
volatile int lx_gl_armed;
extern char lx_gltrace_stubs[];
static int tracing = -1, ntrace;
static const char *trace_name[NTRACE];
static void *trace_fn[NTRACE];
static unsigned char trace_seen[NTRACE];
void *lx_gltrace(long i) {
    static int calls;   // every call for the first 5000, then only first calls
    if (!trace_seen[i] || calls < 5000) { trace_seen[i] = 1; lx_logf("gl: %s\n", trace_name[i]); }
    calls++;
    // debug: after PZ's 256 glGenBuffers the process gets killed; let the watchdog snapshot every thread once GL goes quiet
    static int gens;
    lx_gl_last_tick = armGetSystemTick();
    (void)gens;   // arm with: if (!strcmp(trace_name[i], "glGenBuffers") && ++gens == 256) lx_gl_armed = 1;
    return trace_fn[i];
}
// Texture upload accounting, printed by the watchdog: how much image data goes to the GPU (all of it lives in the
// shared heap here), split into uncompressed and compressed uploads.
typedef unsigned int GLenum_; typedef int GLint_; typedef int GLsizei_;
static void (*real_teximage2d)(GLenum_, GLint_, GLint_, GLsizei_, GLsizei_, GLint_, GLenum_, GLenum_, const void *);
static void (*real_compressed2d)(GLenum_, GLint_, GLenum_, GLsizei_, GLsizei_, GLint_, GLsizei_, const void *);
static volatile unsigned long tex_raw, tex_dxt, tex_count;
static void teximage2d(GLenum_ t, GLint_ lvl, GLint_ ifmt, GLsizei_ w, GLsizei_ h, GLint_ b, GLenum_ fmt, GLenum_ ty, const void *px) {
    static int seen[16], nseen;   // debug: each internal format once
    int k = 0; while (k < nseen && seen[k] != ifmt) k++;
    if (k == nseen && nseen < 16) { seen[nseen++] = ifmt; lx_logf("gl: glTexImage2D internal format %#x (%dx%d)\n", ifmt, w, h); }
    // PZ's "texture compression" asks for generic GL_COMPRESSED_RGB(A), which Mesa stores uncompressed; name the S3TC
    // format instead so the driver really compresses (4x smaller for DXT5) - textures are most of the memory here
    if (ifmt == 0x84EE) ifmt = 0x83F3;        // GL_COMPRESSED_RGBA -> COMPRESSED_RGBA_S3TC_DXT5
    else if (ifmt == 0x84ED) ifmt = 0x83F0;   // GL_COMPRESSED_RGB -> COMPRESSED_RGB_S3TC_DXT1
    // compressed internal formats (S3TC, sRGB S3TC) get compressed by the driver
    int dxt = (ifmt >= 0x83F0 && ifmt <= 0x83F3) || (ifmt >= 0x8C4C && ifmt <= 0x8C4F);
    if (dxt) tex_dxt += (unsigned long)w * h; else tex_raw += (unsigned long)w * h * 4;
    tex_count++;
    real_teximage2d(t, lvl, ifmt, w, h, b, fmt, ty, px);
}
static void compressed2d(GLenum_ t, GLint_ lvl, GLenum_ ifmt, GLsizei_ w, GLsizei_ h, GLint_ b, GLsizei_ size, const void *d) {
    tex_dxt += size; tex_count++;
    real_compressed2d(t, lvl, ifmt, w, h, b, size, d);
}
void lx_gl_stats(char *out, size_t cap) {
    snprintf(out, cap, "%lu uploads, %lu MiB uncompressed, %lu MiB compressed", tex_count, tex_raw >> 20, tex_dxt >> 20);
}

static void *gl_proc(const char *n) {
    void *p = (void *)eglGetProcAddress(n);
    if (p && !strcmp(n, "glTexImage2D")) { real_teximage2d = p; p = (void *)teximage2d; }
    if (p && !strcmp(n, "glCompressedTexImage2D")) { real_compressed2d = p; p = (void *)compressed2d; }
    if (tracing < 0) { FILE *f = fopen(LX_ROOT "/gltrace", "r"); tracing = f != NULL; if (f) fclose(f); }
    if (!tracing || !p || ntrace == NTRACE) return p;
    trace_name[ntrace] = strdup(n); trace_fn[ntrace] = p;
    return lx_gltrace_stubs + 8 * ntrace++;
}
static void *glfwGetProcAddress(const char *n) { return gl_proc(n); }
static int glfwExtensionSupported(const char *n) { (void)n; return 0; }

static int glfwWindowShouldClose(Win *w) { return w ? w->should_close : 0; }
static void glfwSetWindowShouldClose(Win *w, int v) { if (w) w->should_close = v; }
static void glfwGetWindowSize(Win *w, int *x, int *y) { (void)w; if (x) *x = W; if (y) *y = H; }
static void glfwGetFramebufferSize(Win *w, int *x, int *y) { (void)w; if (x) *x = W; if (y) *y = H; }
static void glfwGetWindowPos(Win *w, int *x, int *y) { (void)w; if (x) *x = 0; if (y) *y = 0; }
static void glfwGetWindowFrameSize(Win *w, int *l, int *t, int *r, int *b) { (void)w; if (l) *l = 0; if (t) *t = 0; if (r) *r = 0; if (b) *b = 0; }
static void glfwGetWindowContentScale(Win *w, float *x, float *y) { (void)w; if (x) *x = 1; if (y) *y = 1; }
static int glfwGetWindowAttrib(Win *w, int a) { (void)w; return a == GLFW_FOCUSED || a == GLFW_VISIBLE || a == GLFW_HOVERED; }
static void glfwSetWindowUserPointer(Win *w, void *p) { if (w) w->user = p; }
static void *glfwGetWindowUserPointer(Win *w) { return w ? w->user : NULL; }
static float glfwGetWindowOpacity(Win *w) { (void)w; return 1; }
static const char *glfwGetWindowTitle(Win *w) { (void)w; return "Project Zomboid"; }
static void *glfwGetWindowMonitor(Win *w) { (void)w; return NULL; }

// --- monitors ---
static int the_monitor;
static void *monitors[1] = { &the_monitor };
static GLFWvidmode mode = { 1280, 720, 8, 8, 8, 60 };
static void **glfwGetMonitors(int *n) { *n = 1; return monitors; }
static void *glfwGetPrimaryMonitor(void) { return &the_monitor; }
static const GLFWvidmode *glfwGetVideoMode(void *m) { (void)m; return &mode; }
static const GLFWvidmode *glfwGetVideoModes(void *m, int *n) { (void)m; *n = 1; return &mode; }
static void glfwGetMonitorPos(void *m, int *x, int *y) { (void)m; if (x) *x = 0; if (y) *y = 0; }
static void glfwGetMonitorWorkarea(void *m, int *x, int *y, int *w, int *h) { (void)m; if (x) *x = 0; if (y) *y = 0; if (w) *w = W; if (h) *h = H; }
static void glfwGetMonitorPhysicalSize(void *m, int *w, int *h) { (void)m; if (w) *w = 155; if (h) *h = 87; }
static void glfwGetMonitorContentScale(void *m, float *x, float *y) { (void)m; if (x) *x = 1; if (y) *y = 1; }
static const char *glfwGetMonitorName(void *m) { (void)m; return "Switch"; }

// --- time ---
static u64 t0;
static double glfwGetTime(void) { return armTicksToNs(armGetSystemTick() - t0) / 1e9; }
static void glfwSetTime(double t) { t0 = armGetSystemTick() - armNsToTicks((u64)(t * 1e9)); }
static u64 glfwGetTimerValue(void) { return armGetSystemTick(); }
static u64 glfwGetTimerFrequency(void) { return armGetSystemTickFreq(); }

// --- input ---
#define NPADS 4                      // GLFW joysticks 0..3 = controllers 1..4 (split screen); 0 is also the handheld
static PadState pads[NPADS];
static int pad_ready, pad_present[NPADS];
static unsigned char gp_buttons[NPADS][15];
static float gp_axes[NPADS][6];
static unsigned char gp_hat[NPADS][1];   // the D-pad as joystick hat 0 too, like GLFW (lwjglx reads its POV from it)
static double mouse_x, mouse_y;
static int mouse_btn = -1;          // touch: button held (0 = one finger, 1 = two fingers = right click), -1 none
static int pad_buttons[3];          // mouse buttons from the controller (pad->kbm mode)
static int cursor_mode = GLFW_CURSOR_NORMAL;
static int padkbm;                  // controller as keyboard + mouse (sdmc:/<app>/padkbm), for games without pad support
static unsigned char keys[512];       // what the game sees: USB keyboard OR controller
static unsigned char kb_keys[512], pad_keys[512];   // each source on its own, so one can't release the other's keys
// a source changed key k: report a press/release only when the combined state changes
static void key_update(int k, int scancode, int mods) {
    int down = kb_keys[k] | pad_keys[k];
    if (keys[k] == down) return;
    keys[k] = down;
    if (cb.key) cb.key(&win, k, scancode, down ? GLFW_PRESS : GLFW_RELEASE, mods);
}

static void update_pad(void) {
    if (!pad_ready) {
        padInitializeDefault(&pads[0]);   // controller 1 and the handheld controls
        for (int i = 1; i < NPADS; i++) padInitialize(&pads[i], HidNpadIdType_No1 + i);
        pad_ready = 1;
    }
    // GLFW gamepad slots A B X Y by label, Nintendo-style: the button marked A confirms (GLFW A), B goes back. With
    // sdmc:/<app>/padpos, by position instead (Xbox-style: the bottom button, Switch B, is GLFW A).
    static const u64 by_label[15] = { HidNpadButton_A, HidNpadButton_B, HidNpadButton_X, HidNpadButton_Y, HidNpadButton_L,
        HidNpadButton_R, HidNpadButton_Minus, HidNpadButton_Plus, 0, HidNpadButton_StickL, HidNpadButton_StickR,
        HidNpadButton_Up, HidNpadButton_Right, HidNpadButton_Down, HidNpadButton_Left };
    static const u64 by_position[15] = { HidNpadButton_B, HidNpadButton_A, HidNpadButton_Y, HidNpadButton_X, HidNpadButton_L,
        HidNpadButton_R, HidNpadButton_Minus, HidNpadButton_Plus, 0, HidNpadButton_StickL, HidNpadButton_StickR,
        HidNpadButton_Up, HidNpadButton_Right, HidNpadButton_Down, HidNpadButton_Left };
    static int positional = -1;
    if (positional < 0) { FILE *f = fopen(LX_ROOT "/padpos", "r"); positional = f != NULL; if (f) fclose(f); }
    const u64 *map = positional ? by_position : by_label;
    for (int i = 0; i < NPADS; i++) {
        padUpdate(&pads[i]);
        int was = pad_present[i];
        pad_present[i] = i == 0 || padIsConnected(&pads[i]);
        if (was != pad_present[i]) { lx_logf("pad %d %s\n", i + 1, pad_present[i] ? "connected" : "disconnected"); if (cb.joystick) cb.joystick(i, pad_present[i] ? 0x40001 : 0x40002); }
        u64 pb = pad_present[i] ? padGetButtons(&pads[i]) : 0;
        for (int k = 0; k < 15; k++) gp_buttons[i][k] = map[k] && (pb & map[k]) ? 1 : 0;
        HidAnalogStickState l = padGetStickPos(&pads[i], 0), r = padGetStickPos(&pads[i], 1);
        gp_axes[i][0] = l.x / 32767.f; gp_axes[i][1] = -l.y / 32767.f;
        gp_axes[i][2] = r.x / 32767.f; gp_axes[i][3] = -r.y / 32767.f;
        gp_axes[i][4] = (pb & HidNpadButton_ZL) ? 1.f : -1.f;
        gp_axes[i][5] = (pb & HidNpadButton_ZR) ? 1.f : -1.f;
        gp_hat[i][0] = ((pb & HidNpadButton_Up) ? 1 : 0) | ((pb & HidNpadButton_Right) ? 2 : 0) |   // GLFW_HAT_UP/RIGHT/DOWN/LEFT
                       ((pb & HidNpadButton_Down) ? 4 : 0) | ((pb & HidNpadButton_Left) ? 8 : 0);
    }
    u64 b = padGetButtons(&pads[0]);
    // Minus + Plus together: on-screen keyboard. Its text replaces the field's: End, Backspaces, then the text, one key
    // per frame (the game takes one key event per frame and drops the rest)
    static u32 kb_text[512]; static int kb_len, kb_pos = -1, kb_back;
    const u64 both = HidNpadButton_Minus | HidNpadButton_Plus;
    if (kb_pos < 0 && (b & both) == both && (padGetButtonsDown(&pads[0]) & both) && cb.chr) {
        SwkbdConfig kbd; char out[512] = "";
        if (R_SUCCEEDED(swkbdCreate(&kbd, 0))) {
            swkbdConfigMakePresetDefault(&kbd);
            if (R_SUCCEEDED(swkbdShow(&kbd, out, sizeof out))) {
                kb_len = 0;
                for (const unsigned char *p = (const unsigned char *)out; *p && kb_len < 512;) {   // UTF-8 -> code points
                    u32 c; ssize_t n = decode_utf8(&c, p);
                    if (n <= 0) break;
                    kb_text[kb_len++] = c; p += n;
                }
                kb_pos = 0; kb_back = 0;
            }
            swkbdClose(&kbd);
        }
    } else if (kb_pos >= 0) {
        if (kb_back <= 64) {   // End, then 64 Backspaces
            int k = kb_back++ ? 259 : 269;   // GLFW_KEY_BACKSPACE, GLFW_KEY_END
            pad_keys[k] = 1; key_update(k, 0, 0); pad_keys[k] = 0; key_update(k, 0, 0);
        } else if (kb_pos < kb_len) cb.chr(&win, kb_text[kb_pos++]);
        else kb_pos = -1;
    }
}

typedef struct { unsigned char buttons[15]; float axes[6]; } GLFWgamepadstate;
#define JOK(jid) ((jid) >= 0 && (jid) < NPADS && pad_present[jid])
static int glfwGetGamepadState(int jid, GLFWgamepadstate *s) {
    if (!JOK(jid)) return 0;
    memcpy(s->buttons, gp_buttons[jid], 15); memcpy(s->axes, gp_axes[jid], sizeof gp_axes[jid]);
    return 1;
}
static int glfwJoystickPresent(int jid) { return JOK(jid); }
static int glfwJoystickIsGamepad(int jid) { return JOK(jid); }
static const float *glfwGetJoystickAxes(int jid, int *n) { *n = JOK(jid) ? 6 : 0; return JOK(jid) ? gp_axes[jid] : NULL; }
static const unsigned char *glfwGetJoystickButtons(int jid, int *n) { *n = JOK(jid) ? 15 : 0; return JOK(jid) ? gp_buttons[jid] : NULL; }
static const unsigned char *glfwGetJoystickHats(int jid, int *n) { *n = JOK(jid) ? 1 : 0; return JOK(jid) ? gp_hat[jid] : NULL; }
static const char *glfwGetJoystickName(int jid) { return JOK(jid) ? "Nintendo Switch Controller" : NULL; }
static const char *glfwGetGamepadName(int jid) { return JOK(jid) ? "Nintendo Switch Controller" : NULL; }
// the GUID differs per slot so the game keeps each player's bindings apart
static const char *glfwGetJoystickGUID(int jid) {
    static const char *g[NPADS] = { "030000007e0500000920000000000000", "030000007e0500000920000000000001",
                                    "030000007e0500000920000000000002", "030000007e0500000920000000000003" };
    return JOK(jid) ? g[jid] : NULL;
}
static int glfwUpdateGamepadMappings(const char *s) { (void)s; return 1; }

// USB keyboard: HID usage -> GLFW key
static int hid_to_glfw(int u) {
    if (u >= 0x04 && u <= 0x1d) return 65 + (u - 0x04);           // A-Z
    if (u >= 0x1e && u <= 0x26) return 49 + (u - 0x1e);           // 1-9
    if (u == 0x27) return 48;                                     // 0
    if (u >= 0x3a && u <= 0x45) return 290 + (u - 0x3a);          // F1-F12
    switch (u) {
    case 0x28: return 257; case 0x29: return 256; case 0x2a: return 259; case 0x2b: return 258; case 0x2c: return 32;
    case 0x2d: return 45; case 0x2e: return 61; case 0x2f: return 91; case 0x30: return 93; case 0x31: return 92;
    case 0x33: return 59; case 0x34: return 39; case 0x35: return 96; case 0x36: return 44; case 0x37: return 46;
    case 0x38: return 47; case 0x39: return 280; case 0x49: return 260; case 0x4a: return 268; case 0x4b: return 266;
    case 0x4c: return 261; case 0x4d: return 269; case 0x4e: return 267; case 0x4f: return 262; case 0x50: return 263;
    case 0x51: return 264; case 0x52: return 265; case 0xe0: return 341; case 0xe1: return 340; case 0xe2: return 342;
    case 0xe4: return 345; case 0xe5: return 344; case 0xe6: return 346;
    }
    return -1;
}
static int glfw_to_char(int k, int shift) {
    if (k >= 65 && k <= 90) return shift ? k : k + 32;
    if (k >= 48 && k <= 57) return shift ? ")!@#$%^&*("[k - 48] : k;
    if (k == 32) return 32;
    static const char *lo = "-=[]\\;'`,./", *hi = "_+{}|:\"~<>?";
    static const int ks[] = { 45, 61, 91, 93, 92, 59, 39, 96, 44, 46, 47 };
    for (int i = 0; i < 11; i++) if (ks[i] == k) return shift ? hi[i] : lo[i];
    return 0;
}

// pad->kbm: the controller drives keys and the mouse, for games that only know keyboard and mouse (Minecraft 1.12).
// Laid out by button label, Nintendo-style.
// In game (cursor disabled): left stick WASD, right stick looks, A jump, B sneak, ZR/ZL attack/use, X inventory, Y drop,
// L/R and D-pad left/right hotbar, + pause, - debug screen (F3), left-stick click sprint, right-stick click pick block,
// D-pad up camera view.
// In menus (cursor shown): sticks move the pointer, A (or ZR) clicks, Y right-clicks (split a stack), ZL held is Shift
// (A then quick-moves a stack), B is Escape, X closes the inventory.
enum { K_SPACE = 32, K_A = 65, K_D = 68, K_E = 69, K_Q = 81, K_S = 83, K_W = 87, K_ESC = 256, K_F3 = 292, K_F5 = 294,
       K_LSHIFT = 340, K_LCTRL = 341 };
static void key_event(int k, int down) {   // from the controller
    if (pad_keys[k] == down) return;
    pad_keys[k] = down;
    key_update(k, 0, 0);
}
static void button_event(int b, int down) {
    if (pad_buttons[b] == down) return;
    pad_buttons[b] = down;
    if (cb.button) cb.button(&win, b, down, 0);
}
static void pad_kbm(void) {
    static u64 last_tick;
    u64 now = armGetSystemTick();
    double dt = last_tick ? armTicksToNs(now - last_tick) / 1e9 : 0;
    if (dt > 0.1) dt = 0.1;
    last_tick = now;
    u64 b = padGetButtons(&pads[0]), down = padGetButtonsDown(&pads[0]);
    HidAnalogStickState l = padGetStickPos(&pads[0], 0), r = padGetStickPos(&pads[0], 1);
    double lx = l.x / 32767.0, ly = l.y / 32767.0, rx = r.x / 32767.0, ry = r.y / 32767.0;
    int ingame = cursor_mode == GLFW_CURSOR_DISABLED;
    if (ingame) {
        key_event(K_W, ly > 0.4); key_event(K_S, ly < -0.4); key_event(K_A, lx < -0.4); key_event(K_D, lx > 0.4);
        key_event(K_SPACE, !!(b & HidNpadButton_A)); key_event(K_LSHIFT, !!(b & HidNpadButton_B));
        key_event(K_Q, !!(b & HidNpadButton_Y)); key_event(K_LCTRL, !!(b & HidNpadButton_StickL));
        key_event(K_F5, !!(b & HidNpadButton_Up));
        key_event(K_F3, !!(b & HidNpadButton_Minus));
        button_event(0, !!(b & HidNpadButton_ZR)); button_event(1, !!(b & HidNpadButton_ZL));
        button_event(2, !!(b & HidNpadButton_StickR));
        if (cb.scroll && (down & (HidNpadButton_L | HidNpadButton_Left))) cb.scroll(&win, 0, 1);
        if (cb.scroll && (down & (HidNpadButton_R | HidNpadButton_Right))) cb.scroll(&win, 0, -1);
        double dz = 0.12, speed = 900 * dt;   // look: pixels/s at full tilt, squared response for fine aim
        double mx = fabs(rx) > dz ? rx * fabs(rx) : 0, my = fabs(ry) > dz ? ry * fabs(ry) : 0;
        if (mx || my) { mouse_x += mx * speed; mouse_y -= my * speed; if (cb.cursor) cb.cursor(&win, mouse_x, mouse_y); }
    } else {
        // nothing stays held into a menu, except keys that menus read too (re-pressing E while X is still down would
        // close the inventory X just opened, over and over)
        for (int k = 0; k < 512; k++) if (pad_keys[k] && k != K_ESC && k != K_LSHIFT && k != K_E) key_event(k, 0);
        button_event(2, 0);
        double dz = 0.15, speed = 700 * dt;
        double mx = fabs(lx) > dz ? lx : fabs(rx) > dz ? rx : 0, my = fabs(ly) > dz ? ly : fabs(ry) > dz ? ry : 0;
        if (mx || my) {
            mouse_x += mx * speed; mouse_y -= my * speed;
            if (mouse_x < 0) mouse_x = 0; if (mouse_x > W - 1) mouse_x = W - 1;
            if (mouse_y < 0) mouse_y = 0; if (mouse_y > H - 1) mouse_y = H - 1;
            if (cb.cursor) cb.cursor(&win, mouse_x, mouse_y);
        }
        button_event(0, !!(b & (HidNpadButton_A | HidNpadButton_ZR)));
        button_event(1, !!(b & HidNpadButton_Y));
        key_event(K_LSHIFT, !!(b & HidNpadButton_ZL));
        key_event(K_ESC, !!(b & HidNpadButton_B));
    }
    key_event(K_E, !!(b & HidNpadButton_X));
    if (ingame) key_event(K_ESC, !!(b & HidNpadButton_Plus));
}

static void glfwPollEvents(void) {
    if (!appletMainLoop()) { win.should_close = 1; if (cb.close) cb.close(&win); }
    update_pad();
    if (padkbm) pad_kbm();
    // touchscreen -> mouse: one finger = left button, two fingers = right button
    HidTouchScreenState ts;
    if (hidGetTouchScreenStates(&ts, 1)) {
        // A two-finger gesture stays the right button until every finger is up: lifting one finger first must not turn the
        // other into a left click (it dismissed the menu the right click had just opened) or move the cursor off the
        // row that was tapped.
        // Holding one finger still for a second is a right click too (it takes over from the left button and keeps the
        // cursor where it was, like the two-finger gesture).
        static int multi, held, moved;
        static u64 t0; static int x0, y0;
        if (ts.count >= 2) multi = 1;
        if (ts.count == 0) multi = held = moved = 0;
        else if (ts.count == 1 && !t0) { t0 = armGetSystemTick(); x0 = ts.touches[0].x; y0 = ts.touches[0].y; }
        if (ts.count == 0) t0 = 0;
        else if (!multi && !held && !moved) {
            int dx = (int)ts.touches[0].x - x0, dy = (int)ts.touches[0].y - y0;
            if (dx * dx + dy * dy > 20 * 20) moved = 1;
            else if (armTicksToNs(armGetSystemTick() - t0) >= 1000000000ull) held = 1;
        }
        int want = ts.count >= 2 || held ? 1 : multi ? -1 : ts.count - 1;
        if (ts.count && !multi && !held) {
            double x = ts.touches[0].x * W / 1280.0, y = ts.touches[0].y * H / 720.0;
            if (x != mouse_x || y != mouse_y) { mouse_x = x; mouse_y = y; if (cb.cursor) cb.cursor(&win, x, y); }
        }
        // The cursor moves at once but button changes are 3 polls late: the game's popup menus only take a click on the
        // item that was already highlighted, and a click in the same frame as the move needed a second tap.
        static int hist[4] = { -1, -1, -1, -1 };
        memmove(hist, hist + 1, 3 * sizeof hist[0]); hist[3] = want;
        want = hist[0];
        if (want != mouse_btn) {
            if (mouse_btn >= 0 && cb.button) cb.button(&win, mouse_btn, 0, 0);
            mouse_btn = want;
            if (want >= 0 && cb.button) cb.button(&win, want, 1, 0);
        }
    }
    // keyboard
    HidKeyboardState ks;
    if (hidGetKeyboardStates(&ks, 1)) {
        int shift = (ks.modifiers & HidKeyboardModifier_Shift) != 0;
        for (int u = 0; u < 256; u++) {
            int k = hid_to_glfw(u);
            if (k < 0) continue;
            int down = (ks.keys[u / 64] >> (u % 64)) & 1;
            if (down == kb_keys[k]) continue;
            kb_keys[k] = down;
            key_update(k, u, shift);
            int c = glfw_to_char(k, shift);
            if (down && c && cb.chr) cb.chr(&win, c);
        }
    }
}
static void glfwWaitEvents(void) { glfwPollEvents(); }
static void glfwWaitEventsTimeout(double t) { (void)t; glfwPollEvents(); }
static void glfwPostEmptyEvent(void) {}
static int glfwGetKey(Win *w, int k) { (void)w; return k >= 0 && k < 512 ? keys[k] : 0; }
static int glfwGetMouseButton(Win *w, int b) { (void)w; return b >= 0 && b < 3 ? b == mouse_btn || pad_buttons[b] : 0; }
static void glfwGetCursorPos(Win *w, double *x, double *y) { (void)w; if (x) *x = mouse_x; if (y) *y = mouse_y; }
static void glfwSetCursorPos(Win *w, double x, double y) { (void)w; mouse_x = x; mouse_y = y; }
static int glfwGetInputMode(Win *w, int m) { (void)w; return m == GLFW_CURSOR ? cursor_mode : 0; }
static void glfwSetInputMode(Win *w, int m, int v) { (void)w; if (m == GLFW_CURSOR) cursor_mode = v; }
static const char *glfwGetKeyName(int k, int s) { (void)k; (void)s; return NULL; }
static int glfwGetKeyScancode(int k) { return k; }
static int glfwGetPlatform(void) { return GLFW_PLATFORM_NULL; }
static int glfwPlatformSupported(int p) { return p == GLFW_PLATFORM_NULL; }
static void glfwGetVersion(int *a, int *b, int *c) { if (a) *a = 3; if (b) *b = 4; if (c) *c = 0; }
static const char *glfwGetVersionString(void) { return "3.4.0 lxnx (libnx, EGL)"; }
static int glfwGetError(const char **d) { if (d) *d = NULL; return 0; }
static const char *glfwGetClipboardString(Win *w) { (void)w; return ""; }
static int glfwRawMouseMotionSupported(void) { return 0; }

#define SETCB(name, field) static void *name(void *w, void *f) { (void)w; void *o = (void *)cb.field; cb.field = f; return o; }
SETCB(glfwSetKeyCallback, key)
SETCB(glfwSetCharCallback, chr)
SETCB(glfwSetMouseButtonCallback, button)
SETCB(glfwSetCursorPosCallback, cursor)
SETCB(glfwSetScrollCallback, scroll)
SETCB(glfwSetWindowSizeCallback, size)
SETCB(glfwSetFramebufferSizeCallback, fbsize)
SETCB(glfwSetWindowFocusCallback, focus)
SETCB(glfwSetWindowCloseCallback, close)
static void *glfwSetErrorCallback(void *f) { void *o = (void *)cb.error; cb.error = f; return o; }
static void *glfwSetJoystickCallback(void *f) { void *o = (void *)cb.joystick; cb.joystick = f; return o; }

// everything else: accepted and ignored
static long glfw_noop(void) { return 0; }
static int dummy_cursor;
static void *glfwCreateCursor(void) { return &dummy_cursor; }   // no pointer on the Switch; the game only needs a handle

#define F(n) { #n, (GLFWfn)n }
#define N(n) { #n, (GLFWfn)glfw_noop }
static const struct { const char *name; GLFWfn fn; } glfw_table[] = {
    F(glfwInit), F(glfwTerminate), F(glfwCreateWindow), F(glfwDestroyWindow), F(glfwMakeContextCurrent),
    F(glfwGetCurrentContext), F(glfwSwapBuffers), F(glfwSwapInterval), F(glfwGetProcAddress), F(glfwExtensionSupported),
    F(glfwWindowShouldClose), F(glfwSetWindowShouldClose), F(glfwGetWindowSize), F(glfwGetFramebufferSize),
    F(glfwGetWindowPos), F(glfwGetWindowFrameSize), F(glfwGetWindowContentScale), F(glfwGetWindowAttrib),
    F(glfwSetWindowUserPointer), F(glfwGetWindowUserPointer), F(glfwGetWindowOpacity), F(glfwGetWindowTitle),
    F(glfwGetWindowMonitor), F(glfwGetMonitors), F(glfwGetPrimaryMonitor), F(glfwGetVideoMode), F(glfwGetVideoModes),
    F(glfwGetMonitorPos), F(glfwGetMonitorWorkarea), F(glfwGetMonitorPhysicalSize), F(glfwGetMonitorContentScale),
    F(glfwGetMonitorName), F(glfwGetTime), F(glfwSetTime), F(glfwGetTimerValue), F(glfwGetTimerFrequency),
    F(glfwGetGamepadState), F(glfwJoystickPresent), F(glfwJoystickIsGamepad), F(glfwGetJoystickAxes),
    F(glfwGetJoystickButtons), F(glfwGetJoystickHats), F(glfwGetJoystickName), F(glfwGetGamepadName),
    F(glfwGetJoystickGUID), F(glfwUpdateGamepadMappings), F(glfwPollEvents), F(glfwWaitEvents), F(glfwWaitEventsTimeout),
    F(glfwPostEmptyEvent), F(glfwGetKey), F(glfwGetMouseButton), F(glfwGetCursorPos), F(glfwSetCursorPos),
    F(glfwGetInputMode), F(glfwGetKeyName), F(glfwGetKeyScancode), F(glfwGetPlatform), F(glfwPlatformSupported),
    F(glfwGetVersion), F(glfwGetVersionString), F(glfwGetError), F(glfwGetClipboardString), F(glfwRawMouseMotionSupported),
    F(glfwSetKeyCallback), F(glfwSetCharCallback), F(glfwSetMouseButtonCallback), F(glfwSetCursorPosCallback),
    F(glfwSetScrollCallback), F(glfwSetWindowSizeCallback), F(glfwSetFramebufferSizeCallback), F(glfwSetWindowFocusCallback),
    F(glfwSetWindowCloseCallback), F(glfwSetErrorCallback), F(glfwSetJoystickCallback),
    N(glfwInitHint), N(glfwInitAllocator), N(glfwSetMonitorUserPointer), N(glfwGetMonitorUserPointer), N(glfwSetMonitorCallback),
    N(glfwSetGamma), N(glfwGetGammaRamp), N(glfwSetGammaRamp), N(glfwDefaultWindowHints), N(glfwWindowHint),
    N(glfwWindowHintString), N(glfwSetWindowTitle), N(glfwSetWindowIcon), N(glfwSetWindowPos), N(glfwSetWindowSizeLimits),
    N(glfwSetWindowAspectRatio), N(glfwSetWindowSize), N(glfwSetWindowOpacity), N(glfwIconifyWindow), N(glfwRestoreWindow),
    N(glfwMaximizeWindow), N(glfwShowWindow), N(glfwHideWindow), N(glfwFocusWindow), N(glfwRequestWindowAttention),
    N(glfwSetWindowMonitor), N(glfwSetWindowAttrib), N(glfwSetWindowPosCallback), N(glfwSetWindowRefreshCallback),
    N(glfwSetWindowIconifyCallback), N(glfwSetWindowMaximizeCallback), N(glfwSetWindowContentScaleCallback),
    F(glfwSetInputMode), F(glfwCreateCursor), { "glfwCreateStandardCursor", (GLFWfn)glfwCreateCursor }, N(glfwDestroyCursor), N(glfwSetCursor),
    N(glfwGetPreeditCursorRectangle), N(glfwSetPreeditCursorRectangle), N(glfwResetPreeditText), N(glfwGetPreeditCandidate),
    N(glfwSetCharModsCallback), N(glfwSetPreeditCallback), N(glfwSetIMEStatusCallback), N(glfwSetPreeditCandidateCallback),
    N(glfwSetCursorEnterCallback), N(glfwSetDropCallback), N(glfwSetJoystickUserPointer), N(glfwGetJoystickUserPointer),
    N(glfwSetClipboardString),
};

void *lx_alsa_sym(const char *name);

void *lxh_dlsym(const char *lib, const char *name) {
    if (lib && !strncmp(lib, "libasound", 9)) return lx_alsa_sym(name);
    if (!strncmp(name, "glfw", 4)) {
        for (size_t i = 0; i < sizeof glfw_table / sizeof *glfw_table; i++)
            if (!strcmp(glfw_table[i].name, name)) return (void *)glfw_table[i].fn;
        lx_logf("glfw: missing %s\n", name);
        return NULL;
    }
    if (!strncmp(name, "gl", 2)) return gl_proc(name);
    if (!strcmp(name, "eglGetProcAddress")) return (void *)gl_proc;   // LWJGL resolves GL through it (no GLX here)
    if (!strncmp(name, "egl", 3)) return (void *)eglGetProcAddress(name);
    (void)lib;
    return NULL;
}
