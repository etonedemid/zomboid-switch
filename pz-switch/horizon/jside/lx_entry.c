/* Java-side (musl) entry. The Horizon side calls lx_java_entry() on the thread that should become Java's main thread;
 * we start musl the way a Linux kernel would, with argv/envp/auxv on a block it gives us. */
#include <elf.h>
#include <stddef.h>

int __libc_start_main(int (*)(int, char **, char **), int, char **);
int lx_svm_main(int, char **, char **); /* SVM's generated main(), renamed when pre-linking */

/* argv block layout: argc args, NULL, envs, NULL, aux pairs, AT_NULL */
int lx_java_entry(long *block)
{
	return __libc_start_main(lx_svm_main, (int)block[0], (char **)(block + 1));
}
