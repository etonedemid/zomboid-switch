/* Linux/qemu test harness for the Java side: stands in for the Horizon side.
 *  - lxh_dlsym: headless GLFW + a no-op desktop GL that answers the queries LWJGL and the game make, so the game can run
 *    past graphics init (Lua, assets, box64 natives) without a GPU
 *  - lx_jit_reserve: dual-mapped JIT arena the way Horizon code memory works (same pages, RW view + RX view) */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

void *lx_jit_reserve(size_t size, intptr_t *delta)
{
	int fd = memfd_create("box64-jit", 0);
	if (fd < 0 || ftruncate(fd, size)) return 0;
	void *rw = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	void *rx = mmap(0, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
	close(fd);
	if (rw == MAP_FAILED || rx == MAP_FAILED) return 0;
	*delta = (intptr_t)rx - (intptr_t)rw;
	return rw;
}

/* --- headless GLFW --- */
static int win;
static int mon;
static void *mons[1] = { &mon };
static struct { int w, h, r, g, b, hz; } mode = { 1280, 720, 8, 8, 8, 60 };
static long g_init(void) { return 1; }
static void *g_create(void) { return &win; }
static void g_fbsize(void *w, int *x, int *y) { (void)w; if (x) *x = 1280; if (y) *y = 720; }
static void *g_cur;
static void g_make(void *w) { g_cur = w; }
static void *g_getcur(void) { return g_cur; }
static double g_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void **g_mons(int *n) { *n = 1; return mons; }
static void *g_prim(void) { return &mon; }
static void *g_mode(void *m) { (void)m; return &mode; }
static void *g_modes(void *m, int *n) { (void)m; *n = 1; return &mode; }
static const char *g_ver(void) { return "3.4.0 headless"; }
static void g_version(int *a, int *b, int *c) { if (a) *a = 3; if (b) *b = 4; if (c) *c = 0; }
static long g_attrib(void *w, int a) { (void)w; return a == 0x20001 || a == 0x20004; }
static void g_scale(void *w, float *x, float *y) { (void)w; if (x) *x = 1; if (y) *y = 1; }
static long g_platform(void) { return 0x60005; }
static long g_zero(void) { return 0; }
static void *g_cbset(void *a, void *b) { (void)a; (void)b; return 0; }
void *lxh_dlsym(const char *lib, const char *name);
static void *g_proc(const char *n) { return lxh_dlsym(NULL, n); }

/* --- no-op GL with the answers callers depend on --- */
static unsigned next_id = 1;
static long gl_noop(void) { return 0; }
static const char *gl_string(unsigned e)
{
	switch (e) {
	case 0x1F00: return "lxnx";                    /* VENDOR */
	case 0x1F01: return "headless";                /* RENDERER */
	case 0x1F02: return "4.3 (Compatibility Profile) headless";
	case 0x8B8C: return "4.30";                    /* SHADING_LANGUAGE_VERSION */
	}
	return "";
}
static const char *gl_stringi(unsigned e, unsigned i) { (void)e; (void)i; return ""; }
static void gl_getintv(unsigned e, int *v)
{
	switch (e) {
	case 0x821B: *v = 4; break;                    /* MAJOR_VERSION */
	case 0x821C: *v = 3; break;                    /* MINOR_VERSION */
	case 0x0D33: case 0x84E8: *v = 8192; break;    /* MAX_TEXTURE_SIZE, MAX_RENDERBUFFER_SIZE */
	case 0x8872: case 0x8B4D: case 0x84E2: case 0x8869: *v = 16; break; /* texture units, vertex attribs */
	case 0x8CDF: *v = 8; break;                    /* MAX_COLOR_ATTACHMENTS */
	case 0x8D57: *v = 4; break;                    /* MAX_SAMPLES */
	case 0x0BA2: v[0] = v[1] = 0; v[2] = 1280; v[3] = 720; break; /* VIEWPORT */
	default: *v = 0;
	}
}
static void gl_getfloatv(unsigned e, float *v) { int i[4] = { 0 }; gl_getintv(e, i); v[0] = i[0]; }
static void gl_gen(int n, unsigned *ids) { for (int i = 0; i < n; i++) ids[i] = next_id++; }
static unsigned gl_create(void) { return next_id++; }
static unsigned gl_createshader(unsigned t) { (void)t; return next_id++; }
static void gl_getiv_ok(unsigned o, unsigned e, int *v) { (void)o; *v = (e == 0x8B81 || e == 0x8B82 || e == 0x8B83) ? 1 : 0; } /* COMPILE/LINK/VALIDATE_STATUS */
static unsigned gl_fbstatus(unsigned t) { (void)t; return 0x8CD5; }
static int gl_location(unsigned p, const char *n) { (void)p; (void)n; return 0; }
static void *mapbuf;
static void *gl_map(void) { if (!mapbuf) mapbuf = calloc(1, 64 << 20); return mapbuf; }   /* ponytail: one shared scratch buffer */

static const struct { const char *name; void *fn; } tab[] = {
	{ "glfwInit", g_init }, { "glfwCreateWindow", g_create }, { "glfwGetFramebufferSize", g_fbsize },
	{ "glfwGetWindowSize", g_fbsize }, { "glfwMakeContextCurrent", g_make }, { "glfwGetCurrentContext", g_getcur },
	{ "glfwGetTime", g_time }, { "glfwGetMonitors", g_mons }, { "glfwGetPrimaryMonitor", g_prim },
	{ "glfwGetVideoMode", g_mode }, { "glfwGetVideoModes", g_modes }, { "glfwGetVersionString", g_ver },
	{ "glfwGetVersion", g_version }, { "glfwGetWindowAttrib", g_attrib }, { "glfwGetWindowContentScale", g_scale },
	{ "glfwGetMonitorContentScale", g_scale }, { "glfwGetPlatform", g_platform },
	{ "glfwGetProcAddress", g_proc }, { "glfwCreateCursor", g_create }, { "glfwCreateStandardCursor", g_create }, { "glfwSetErrorCallback", g_cbset },
	{ "glGetString", gl_string }, { "glGetStringi", gl_stringi }, { "glGetIntegerv", gl_getintv },
	{ "glGetFloatv", gl_getfloatv }, { "glCreateProgram", gl_create }, { "glCreateShader", gl_createshader },
	{ "glGetShaderiv", gl_getiv_ok }, { "glGetProgramiv", gl_getiv_ok }, { "glCheckFramebufferStatus", gl_fbstatus },
	{ "glCheckFramebufferStatusEXT", gl_fbstatus }, { "glGetUniformLocation", gl_location }, { "glGetAttribLocation", gl_location },
	{ "glMapBuffer", gl_map }, { "glMapBufferRange", gl_map },
};

void *lxh_dlsym(const char *lib, const char *name)
{
	(void)lib;
	for (size_t i = 0; i < sizeof tab / sizeof *tab; i++)
		if (!strcmp(tab[i].name, name)) return tab[i].fn;
	if (!strncmp(name, "glGen", 5) && strncmp(name, "glGenerate", 10)) return gl_gen;
	if (!strncmp(name, "glfwSet", 7) && strstr(name, "Callback")) return g_cbset;
	if (!strncmp(name, "glfw", 4)) return g_zero;
	if (!strncmp(name, "gl", 2)) return gl_noop;
	return NULL;
}
