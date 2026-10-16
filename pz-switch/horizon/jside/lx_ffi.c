/* libffi closures without executable memory (Horizon has no RWX and no file-backed code remaps).
 * A fixed pool of trampolines lives in .text; trampoline i loads lx_ffi_closures[i] into x17 and jumps to the
 * address libffi's aarch64 ffi_prep_closure_loc stores at closure->tramp + 16 (ffi_closure_SYSV or _V). Those
 * entry points expect x17 = the closure, exactly what libffi's own dynamic trampoline provides. libffi's code
 * bytes still get written into closure->tramp (plain heap); they're just never executed.
 * These definitions replace libffi's closures.c. */
#include <pthread.h>
#include <stdlib.h>
#include <stddef.h>

#define NTRAMP 1024
#define TRAMP_SIZE 32

void *lx_ffi_closures[NTRAMP];
extern char lx_ffi_tramps[];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

__asm__(
    ".text\n"
    ".balign 32\n"
    ".global lx_ffi_tramps\n"
    "lx_ffi_tramps:\n"
    ".set i, 0\n"
    ".rept 1024\n"
    "  adrp x17, lx_ffi_closures\n"
    "  add x17, x17, :lo12:lx_ffi_closures\n"
    "  ldr x17, [x17, #(i * 8)]\n"
    "  ldr x16, [x17, #16]\n"
    "  br x16\n"
    "  .balign 32\n"
    "  .set i, i + 1\n"
    ".endr\n");

void *ffi_closure_alloc(size_t size, void **code)
{
	void *c = NULL;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < NTRAMP; i++)
		if (!lx_ffi_closures[i]) {
			if (!(c = calloc(1, size))) break;
			lx_ffi_closures[i] = c;
			*code = lx_ffi_tramps + i * TRAMP_SIZE;
			break;
		}
	pthread_mutex_unlock(&lock);
	return c;  /* NULL when the pool is exhausted: LWJGL reports a callback allocation failure */
}

void ffi_closure_free(void *ptr)
{
	pthread_mutex_lock(&lock);
	for (int i = 0; i < NTRAMP; i++)
		if (lx_ffi_closures[i] == ptr) { lx_ffi_closures[i] = NULL; break; }
	pthread_mutex_unlock(&lock);
	free(ptr);
}

void *ffi_data_to_code_pointer(void *data) { return data; }
int ffi_tramp_is_present(void *closure) { (void)closure; return 0; }
