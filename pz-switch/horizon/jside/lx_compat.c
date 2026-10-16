/* glibc-only symbols referenced by the JDK's static libraries (built against glibc). On aarch64 glibc and musl share
 * the kernel's generic struct layouts (stat, dirent, statvfs, rlimit), so these map straight onto musl. */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <glob.h>
#include <ftw.h>
#include <sys/statfs.h>
#include <sys/uio.h>
#include <math.h>

FILE *fopen64(const char *p, const char *m) { return fopen(p, m); }
int open64(const char *p, int f, ...) { va_list ap; va_start(ap, f); int mode = va_arg(ap, int); va_end(ap); return open(p, f, mode); }
off_t lseek64(int fd, off_t o, int w) { return lseek(fd, o, w); }
ssize_t pread64(int fd, void *b, size_t n, off_t o) { return pread(fd, b, n, o); }
ssize_t pwrite64(int fd, const void *b, size_t n, off_t o) { return pwrite(fd, b, n, o); }
int ftruncate64(int fd, off_t l) { return ftruncate(fd, l); }
void *mmap64(void *a, size_t l, int p, int f, int fd, off_t o) { return mmap(a, l, p, f, fd, o); }
struct dirent *readdir64(DIR *d) { return readdir(d); }
int statvfs64(const char *p, struct statvfs *s) { return statvfs(p, s); }
int fstatvfs64(int fd, struct statvfs *s) { return fstatvfs(fd, s); }
int getrlimit64(int r, struct rlimit *l) { return getrlimit(r, l); }
ssize_t sendfile64(int o, int i, off_t *off, size_t n) { return sendfile(o, i, off, n); }
int __xstat64(int ver, const char *p, struct stat *s) { (void)ver; return stat(p, s); }
int __lxstat64(int ver, const char *p, struct stat *s) { (void)ver; return lstat(p, s); }
int __fxstat64(int ver, int fd, struct stat *s) { (void)ver; return fstat(fd, s); }
int posix_fadvise64(int fd, off_t o, off_t l, int adv) { return posix_fadvise(fd, o, l, adv); }
char *__strtok_r(char *s, const char *d, char **p) { return strtok_r(s, d, p); }
char *__strdup(const char *s) { return strdup(s); }
cpu_set_t *__sched_cpualloc(size_t n) { return calloc(1, CPU_ALLOC_SIZE(n)); }
void __sched_cpufree(cpu_set_t *s) { free(s); }

/* the rest of the LFS64 family, exported so box64's wrapped libc finds host versions instead of falling back to the
 * emulated x86 glibc's (which then gets handed musl FILE*s) */
int fseeko64(FILE *f, off_t o, int w) { return fseeko(f, o, w); }
off_t ftello64(FILE *f) { return ftello(f); }
int fgetpos64(FILE *f, fpos_t *p) { return fgetpos(f, p); }
int fsetpos64(FILE *f, const fpos_t *p) { return fsetpos(f, p); }
FILE *freopen64(const char *p, const char *m, FILE *f) { return freopen(p, m, f); }
FILE *tmpfile64(void) { return tmpfile(); }
int stat64(const char *p, struct stat *s) { return stat(p, s); }
int fstat64(int fd, struct stat *s) { return fstat(fd, s); }
int lstat64(const char *p, struct stat *s) { return lstat(p, s); }
int fstatat64(int d, const char *p, struct stat *s, int f) { return fstatat(d, p, s, f); }
int statfs64(const char *p, struct statfs *s) { return statfs(p, s); }
int fstatfs64(int fd, struct statfs *s) { return fstatfs(fd, s); }
int truncate64(const char *p, off_t l) { return truncate(p, l); }
int creat64(const char *p, mode_t m) { return creat(p, m); }
int openat64(int d, const char *p, int f, ...) { va_list ap; va_start(ap, f); int m = va_arg(ap, int); va_end(ap); return openat(d, p, f, m); }
ssize_t preadv64(int fd, const struct iovec *v, int n, off_t o) { return preadv(fd, v, n, o); }
ssize_t pwritev64(int fd, const struct iovec *v, int n, off_t o) { return pwritev(fd, v, n, o); }
int posix_fallocate64(int fd, off_t o, off_t l) { return posix_fallocate(fd, o, l); }
int fallocate64(int fd, int m, off_t o, off_t l) { return fallocate(fd, m, o, l); }
int setrlimit64(int r, const struct rlimit *l) { return setrlimit(r, l); }
int prlimit64(pid_t p, int r, const struct rlimit *n, struct rlimit *o) { return prlimit(p, r, n, o); }
int lockf64(int fd, int c, off_t l) { return lockf(fd, c, l); }
int mkstemp64(char *t) { return mkstemp(t); }
int mkostemp64(char *t, int f) { return mkostemp(t, f); }
int mkstemps64(char *t, int s) { return mkstemps(t, s); }
int mkostemps64(char *t, int s, int f) { return mkostemps(t, s, f); }
int glob64(const char *p, int f, int (*e)(const char *, int), glob_t *g) { return glob(p, f, e, g); }
void globfree64(glob_t *g) { globfree(g); }
int scandir64(const char *d, struct dirent ***n, int (*s)(const struct dirent *), int (*c)(const struct dirent **, const struct dirent **)) { return scandir(d, n, s, c); }
int alphasort64(const struct dirent **a, const struct dirent **b) { return alphasort(a, b); }
int versionsort64(const struct dirent **a, const struct dirent **b) { return versionsort(a, b); }
int ftw64(const char *p, int (*f)(const char *, const struct stat *, int), int n) { return ftw(p, f, n); }
int nftw64(const char *p, int (*f)(const char *, const struct stat *, int, struct FTW *), int n, int fl) { return nftw(p, f, n, fl); }
int fcntl64(int fd, int c, ...) { va_list ap; va_start(ap, c); long a = va_arg(ap, long); va_end(ap); return fcntl(fd, c, a); }
/* glibc's classification functions, called by static JDK libraries built against glibc (lcms) */
int __isinf(double x) { return isinf(x); }
int __isnan(double x) { return isnan(x); }
int __isinff(float x) { return isinf(x); }
int __isnanf(float x) { return isnan(x); }
int __finite(double x) { return isfinite(x); }
