/* The game's closed x86-64 JNI libraries (TIS natives, FMOD integration) run in box64, as in zomdroid.
 * lx_dl routes dlopen/dlsym for those names here. A JNI symbol resolves to one of the fixed trampolines in
 * lx_x86_tramp.S whose context holds the x86 entry point and the Java method descriptor (known at build time from
 * gen-natives.py); lx_x86_call() moves the AAPCS64 arguments into the x86-64 SysV registers/stack, swaps JNIEnv*
 * for libjniwrapper's x86-side wrapped env, runs the function and returns RAX or XMM0. */
#define _GNU_SOURCE
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "box64context.h"
#include "x64emu.h"
#include "librarian.h"
#include "library.h"
#include "callback.h"
#include "dynarec.h"
#include "elfloader.h"
#include "box64stack.h"
#include "env.h"
#include "regs.h"
#include "x64emu_private.h"
#include "x64run_private.h"

#define NTRAMP 4096
typedef struct { uintptr_t fn; const char *desc; } Ctx;
extern Ctx lx_x86_ctx[NTRAMP];
extern char lx_x86_tramps[];
static int nctx;

struct jni_sig { const char *name, *desc; };
extern const struct jni_sig lx_jni_sigs[];
extern const size_t lx_jni_sigs_len;

JavaVM *g_zomdroid_jvm;
__thread JNIEnv *g_zomdroid_jni_env;
uint64_t g_wrapped_jni_env, g_wrapped_jvm;
#define wrapped_env g_wrapped_jni_env
#define wrapped_jvm g_wrapped_jvm
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int ready = -1;  /* -1 not tried, 0 failed, 1 up */

extern box64context_t *my_context;
extern int box64_pagesize;

static int load(const char **names, int n)
{
	needed_libs_t *nl = new_neededlib(n);
	for (int i = 0; i < n; i++) nl->names[i] = strdup(names[i]);
	int rc = AddNeededLib(my_context->maplib, 0, 0, 0, nl, NULL, my_context, thread_get_emu());
	if (rc) RemoveNeededLib(my_context->maplib, 0, nl, my_context, thread_get_emu());
	free_neededlib(nl);
	return rc;
}

static int init(void)
{
	if (ready >= 0) return ready;
	ready = 0;
	jsize n = 0;
	if (JNI_GetCreatedJavaVMs(&g_zomdroid_jvm, 1, &n) != JNI_OK || !n) { fprintf(stderr, "x86: no JavaVM\n"); return 0; }
	setenv("BOX64_LD_LIBRARY_PATH", "/game/natives:/game/x86libs", 0);
	setenv("BOX64_NOBANNER", "1", 0);
	LoadEnvVariables();
	box64_pagesize = 4096;
	box64context_t *ctx = NewBox64Context(0);
	ctx->argv[0] = strdup("java");
	PrependList(&ctx->box64_ld_lib, getenv("BOX64_LD_LIBRARY_PATH"), 1);
	// zomdroid's box64 wraps FMOD 13 with Android's ARM build; there is none here, so build 41's x86 FMOD is emulated
	AddPath("libfmod.so.13", &ctx->box64_emulated_libs, 0);
	AddPath("libfmodstudio.so.13", &ctx->box64_emulated_libs, 0);
	if (CalcStackSize(ctx)) { fprintf(stderr, "x86: CalcStackSize failed\n"); return 0; }
	thread_set_emu(NewX64Emu(ctx, ctx->ep, (uintptr_t)ctx->stack, ctx->stacksz, 0));
	/* gnu libc next to box64's wrapped libc (zomdroid: SONAMEs patched so box64 doesn't swap them for wrapped ones) */
	/* libpthread.so.0 too: glibc 2.34+ builds (Bullet, PopMan) take pthread_* from libc.so.6, which this box64 only
	 * wraps in its libpthread */
	const char *libc[] = { "libc.so.6", "libpthread.so.0", "_ld-linux-x86-64.so.2", "_libc.so.6" };
	if (load(libc, 4)) { fprintf(stderr, "x86: loading x86 libc failed\n"); return 0; }
	const char *jw[] = { "libjniwrapper.so" };
	if (load(jw, 1)) { fprintf(stderr, "x86: loading libjniwrapper.so failed\n"); return 0; }
	uintptr_t f = FindGlobalSymbol(my_context->maplib, "jni_wrapper_init", -1, NULL, 0);
	wrapped_env = FindGlobalSymbol(my_context->maplib, "g_wrapped_jni_env", -1, NULL, 0);
	wrapped_jvm = FindGlobalSymbol(my_context->maplib, "g_wrapped_jvm", -1, NULL, 0);
	if (!f || !wrapped_env || !wrapped_jvm) { fprintf(stderr, "x86: jniwrapper symbols missing\n"); return 0; }
	RunFunction(f, 0);
	fprintf(stderr, "x86: box64 up\n");
	return ready = 1;
}

void *lx_x86_dlopen(const char *path)
{
	pthread_mutex_lock(&lock);
	void *h = NULL;
	if (init()) {
		const char *base = strrchr(path, '/');
		base = base ? base + 1 : path;
		needed_libs_t *nl = new_neededlib(1);
		nl->names[0] = strdup(base);
		if (AddNeededLib(NULL, 1, 0, 1, nl, NULL, my_context, thread_get_emu()) == 0) {
			h = nl->libs[0];
			/* run the new library's constructors now (zomdroid does the same dance) */
			int od = my_context->deferredInit; elfheader_t **ol = my_context->deferredInitList;
			int os = my_context->deferredInitSz, oc = my_context->deferredInitCap;
			my_context->deferredInit = 1; my_context->deferredInitList = NULL;
			my_context->deferredInitSz = my_context->deferredInitCap = 0;
			RunDeferredElfInit(thread_get_emu());
			my_context->deferredInit = od; my_context->deferredInitList = ol;
			my_context->deferredInitSz = os; my_context->deferredInitCap = oc;
			fprintf(stderr, "x86: loaded %s\n", base);
		} else {
			RemoveNeededLib(NULL, 1, nl, my_context, thread_get_emu());
			fprintf(stderr, "x86: failed to load %s\n", base);
		}
		free_neededlib(nl);
	}
	pthread_mutex_unlock(&lock);
	return h;
}

static int sigcmp(const void *k, const void *e) { return strcmp(k, ((const struct jni_sig *)e)->name); }

void *lx_x86_dlsym(void *h, const char *name)
{
	if (strncmp(name, "Java_", 5)) return NULL;
	char shortname[512];
	snprintf(shortname, sizeof shortname, "%s", name);
	char *dd = strstr(shortname + 5, "__");  /* long form: strip the mangled signature */
	if (dd) *dd = 0;
	const struct jni_sig *s = bsearch(shortname, lx_jni_sigs, lx_jni_sigs_len, sizeof *lx_jni_sigs, sigcmp);
	if (!s) return NULL;
	pthread_mutex_lock(&lock);
	uintptr_t fn = FindGlobalSymbol(GetMaplib(h), shortname, -1, NULL, 0);
	if (!fn) fn = FindGlobalSymbol(GetMaplib(h), name, -1, NULL, 0);
	void *t = NULL;
	if (fn && nctx < NTRAMP) {
		lx_x86_ctx[nctx] = (Ctx){ fn, s->desc };
		t = lx_x86_tramps + nctx * 32;
		nctx++;
	}
	pthread_mutex_unlock(&lock);
	return t;
}

/* skip one JVM type in a descriptor */
static const char *next_type(const char *p)
{
	while (*p == '[') p++;
	if (*p == 'L') { while (*p && *p != ';') p++; }
	return *p ? p + 1 : p;
}

uint64_t lx_x86_call(const Ctx *c, const uint64_t *gp, const double *fp, const uint64_t *stk, double *fpret)
{
	static const int xgp[6] = { _DI, _SI, _DX, _CX, _R8, _R9 };
	x64emu_t *emu = thread_get_emu();
	uint64_t ia[64]; double fa[64]; char kind[64]; int n = 0;  /* argument values in order, kind i/f/d */
	int ngp = 0, nfp = 0, nstk = 0;
	/* JNIEnv*, jobject/jclass */
	ia[n] = wrapped_env; kind[n++] = 'i'; ngp++;
	ia[n] = gp[ngp++]; kind[n++] = 'i';
	const char *p = c->desc + 1;
	while (*p && *p != ')' && n < 64) {
		char t = *p;
		if (t == 'F' || t == 'D') {
			uint64_t raw = nfp < 8 ? *(const uint64_t *)&fp[nfp++] : stk[nstk++];
			memcpy(&fa[n], &raw, 8);
			kind[n++] = t == 'F' ? 'f' : 'd';
		} else {
			ia[n] = ngp < 8 ? gp[ngp++] : stk[nstk++];
			/* AAPCS64 leaves the upper bits of narrow integer args unspecified; SysV callees expect them extended */
			switch (t) {
			case 'Z': ia[n] = (uint8_t)ia[n]; break;
			case 'B': ia[n] = (uint64_t)(int64_t)(int8_t)ia[n]; break;
			case 'C': ia[n] = (uint16_t)ia[n]; break;
			case 'S': ia[n] = (uint64_t)(int64_t)(int16_t)ia[n]; break;
			case 'I': ia[n] = (uint64_t)(int64_t)(int32_t)ia[n]; break;
			}
			kind[n++] = 'i';
		}
		p = next_type(p);
	}
	char ret = p[0] == ')' ? p[1] : 'V';

	/* x86-64 SysV placement */
	int xi = 0, xf = 0, nst = 0;
	uint64_t st[64];
	for (int k = 0; k < n; k++) {
		if (kind[k] == 'i') { if (xi < 6) emu->regs[xgp[xi++]].q[0] = ia[k]; else st[nst++] = ia[k]; }
		else {
			uint64_t raw; memcpy(&raw, &fa[k], 8);
			if (xf < 8) { emu->xmm[xf].q[0] = raw; emu->xmm[xf].q[1] = 0; xf++; } else st[nst++] = raw;
		}
	}
	Push64(emu, R_RBP);
	R_RBP = R_RSP;
	R_RSP -= ((nst + 1) & ~1) * 8;   /* keep 16-byte alignment at the call */
	uint64_t *sp = (uint64_t *)R_RSP;
	for (int k = 0; k < nst; k++) sp[k] = st[k];
	R_RAX = xf;   /* varargs convention; harmless otherwise */
	uintptr_t oldip = R_RIP;
	DynaCall(emu, c->fn);
	if (oldip == R_RIP) { R_RSP = R_RBP; R_RBP = Pop64(emu); }
	if (ret == 'F') { float f = emu->xmm[0].f[0]; *fpret = 0; memcpy(fpret, &f, 4); return 0; }
	if (ret == 'D') { *fpret = emu->xmm[0].d[0]; return 0; }
	return R_RAX;
}
