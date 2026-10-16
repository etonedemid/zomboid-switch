/* dlopen/dlsym for the static Java side. Every "shared library" the game loads is linked in; a handle names a library
 * only so JNI_OnLoad can be told apart (built as JNI_OnLoad_<lib>). Graphics/windowing libraries live on the Horizon
 * side and are resolved through lxh_dlsym(). Overrides musl's static-only dl* stubs. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct lx_sym { const char *name; void *addr; };
extern const struct lx_sym lx_dltab[];
extern const size_t lx_dltab_len;
void *lxh_dlsym(const char *lib, const char *name);  /* Horizon side */
/* box64 bridge (x86/lx_x86.c); weak, as only PZ links box64 */
__attribute__((weak)) void *lx_x86_dlopen(const char *path);
__attribute__((weak)) void *lx_x86_dlsym(void *h, const char *name);

typedef struct { char name[64]; int forward; void *x86; } Lib;
static Lib libs[64];
static int nlibs;
static Lib global = { "", 0 };
static const char *err;

/* Libraries that exist here. Anything else fails, which matters for box64: a "host" library that loads but has no
 * symbols would make it call NULL instead of falling back to the x86 copy (or the x86 code skipping it, e.g. FMOD's
 * PulseAudio/ALSA probing). */
static const char *known[] = {
	/* statically linked Java-side natives */
	"liblwjgl.so", "liblwjgl_opengl.so", "liblwjgl_stb.so",
	/* game libraries Java loads by name (JNI resolves through the table / box64) */
	"libfmod.so", "libfmodstudio.so", "libimgui-java64.so",
	"libpzexe_jni64.so", "libPZXInitThreads64.so", "libRakNet64.so", "libsteam_api.so", "libsqlitejdbc.so",
	/* host libraries box64's wrapped libc family dlopens (resolved from musl in the table) */
	"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1", "libutil.so.1", "libresolv.so.2",
	"libcrypt.so.1", "libzomdroidlinker.so",
	/* JDK libraries linked statically (Minecraft's ImageIO/Java2D use) */
	"libawt.so", "libawt_headless.so", "libmlib_image.so", "libjavajpeg.so", "liblcms.so",
	/* openal-soft, OSS backend on the emulated /dev/dsp (Minecraft via LWJGL) */
	"libopenal.so", "libopenal.so.1", NULL };
/* the game's closed x86-64 JNI libraries, run in box64 */
static const char *emulated[] = { "libPZClipper64.so", "libPZBullet64.so", "libPZBulletNoOpenGL64.so", "libPZPopMan64.so",
	"libLighting64.so", "libPZPathFind64.so", "libfmodintegration64.so", "libZNetNoSteam64.so", "libZNetJNI64.so",
	"libjassimp64.so", "libbink64.so", "libBink2x64.so", NULL };
static const char *forwarded[] = { "libGL", "libEGL", "libGLES", "libglfw", "libvulkan", "libasound", NULL };

static int cmp(const void *k, const void *e) { return strcmp(k, ((const struct lx_sym *)e)->name); }
static void *lookup(const char *name)
{
	const struct lx_sym *s = bsearch(name, lx_dltab, lx_dltab_len, sizeof *lx_dltab, cmp);
	return s ? s->addr : NULL;
}

void *dlopen(const char *path, int flags)
{
	(void)flags;
	if (!path) return &global;
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	/* sqlite-jdbc extracts its library as sqlite-<version>-<uuid>-libsqlitejdbc.so */
	size_t bl = strlen(base);
	if (bl > 16 && !strcmp(base + bl - 16, "libsqlitejdbc.so")) base = "libsqlitejdbc.so";
	int ok = 0;
	for (int i = 0; known[i] && !ok; i++) ok = !strcmp(base, known[i]);
	for (int i = 0; emulated[i] && !ok; i++) ok = !strcmp(base, emulated[i]);
	for (int i = 0; forwarded[i] && !ok; i++) ok = !strncmp(base, forwarded[i], strlen(forwarded[i]));
	if (!ok) { err = "library not available on this platform"; return NULL; }
	for (int i = 0; i < nlibs; i++)
		if (!strcmp(libs[i].name, base)) return &libs[i];
	if (nlibs == 64) { err = "too many libraries"; return NULL; }
	void *x86 = NULL;
	for (int i = 0; emulated[i]; i++)
		if (!strcmp(base, emulated[i]) && !(x86 = lx_x86_dlopen ? lx_x86_dlopen(path) : NULL)) { err = "x86 library failed to load"; return NULL; }
	Lib *l = &libs[nlibs++];
	snprintf(l->name, sizeof l->name, "%s", base);
	l->x86 = x86;
	for (int i = 0; forwarded[i]; i++)
		if (!strncmp(base, forwarded[i], strlen(forwarded[i]))) l->forward = 1;
	return l;
}

/* the ALSA shim lives on the Horizon side (newlib heap); FMOD free()s the hint strings with musl */
static char *(*alsa_get_hint)(const void *, const char *);
static char *get_hint_musl(const void *h, const char *id)
{
	const char *s = alsa_get_hint(h, id);
	return s ? strdup(s) : NULL;
}

void *dlsym(void *h, const char *name)
{
	Lib *l = h && h != RTLD_DEFAULT && h != RTLD_NEXT ? h : &global;
	void *p = NULL;
	if (l->forward) {
		p = lxh_dlsym(l->name, name);
		if (p && !strcmp(name, "snd_device_name_get_hint")) alsa_get_hint = p, p = get_hint_musl;
	}
	else if (l->x86 && (p = lx_x86_dlsym(l->x86, name))) {}
	else if (!strcmp(name, "JNI_OnLoad") || !strcmp(name, "JNI_OnUnload")) {
		/* liblwjgl_opengl.so -> lxonload_lwjgl_opengl (not JNI_OnLoad_<lib>: SVM would take that for a statically
		 * linked library, which must report JNI 1.8+) */
		char sym[128], lib[64];
		snprintf(lib, sizeof lib, "%s", l->name + (strncmp(l->name, "lib", 3) ? 0 : 3));
		char *dot = strstr(lib, ".so");
		if (dot) *dot = 0;
		snprintf(sym, sizeof sym, "%s_%s", strcmp(name, "JNI_OnLoad") ? "lxonunload" : "lxonload", lib);
		p = lookup(sym);
		if (!p) { snprintf(sym, sizeof sym, "%s_%s", name, lib); p = lookup(sym); }   /* static JDK libraries: JNI_OnLoad_awt */
	} else {
		/* the table has a logging stub for every native method; an emulated library's real one wins */
		if (!strncmp(name, "Java_", 5))
			for (int i = 0; i < nlibs && !p; i++)
				if (libs[i].x86) p = lx_x86_dlsym(libs[i].x86, name);
		if (!p) p = lookup(name);
		if (!p && l == &global) p = lxh_dlsym(NULL, name);
	}
	if (!p) err = "symbol not found";
	return p;
}

int dlclose(void *h) { (void)h; return 0; }

char *dlerror(void)
{
	const char *e = err;
	err = NULL;
	return (char *)e;
}

int dladdr(const void *addr, Dl_info *info)
{
	(void)addr;
	memset(info, 0, sizeof *info);
	info->dli_fname = "/app";
	return 1;
}

/* JNI stub hits: log each stubbed native the first time it is called */
extern const char *const lx_jni_stub_names[];
extern const unsigned long lx_jni_stub_count;
static unsigned char *hit;
void lx_jni_stub_hit(unsigned i)
{
	if (!hit) hit = calloc(lx_jni_stub_count, 1);
	if (i < lx_jni_stub_count && !hit[i]) {
		hit[i] = 1;
		fprintf(stderr, "[stub] %s\n", lx_jni_stub_names[i]);
	}
}

/* musl's thread exit calls this (normally in its dlerror.c, which would clash with dlerror above) */
void __dl_thread_cleanup(void) {}
