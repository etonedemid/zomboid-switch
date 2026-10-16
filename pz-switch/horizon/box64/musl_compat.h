/* glibc-isms box64 relies on, for building it against musl */
#pragma once
#include <math.h>
#include <stdint.h>
#ifndef isnanf
#define isnanf(x) isnan(x)
#endif
#ifndef isinff
#define isinff(x) isinf(x)
#endif
typedef uint8_t __uint8_t;
typedef uint16_t __uint16_t;
typedef uint32_t __uint32_t;
typedef uint64_t __uint64_t;
typedef int8_t __int8_t;
typedef int16_t __int16_t;
typedef int32_t __int32_t;
typedef int64_t __int64_t;
typedef unsigned int uint;
#include <dlfcn.h>
#define dlvsym(h, s, v) dlsym(h, s)
#ifndef RTLD_DL_SYMENT
#define RTLD_DL_SYMENT 1
#define RTLD_DL_LINKMAP 2
#endif
/* musl dropped the LFS64 names. Functions become function-like macros so box64's generated GO(name64, ...) wrapper
 * tables (bare names, no parentheses) keep their x86-visible names while calls reach the plain musl functions. */
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <glob.h>
#include <ftw.h>
struct stat64 {
	dev_t st_dev; ino_t st_ino; mode_t st_mode; nlink_t st_nlink; uid_t st_uid; gid_t st_gid; dev_t st_rdev;
	unsigned long __pad; off_t st_size; blksize_t st_blksize; int __pad2; blkcnt_t st_blocks;
	struct timespec st_atim, st_mtim, st_ctim; unsigned __unused_[2];
};
struct statfs64 {
	unsigned long f_type, f_bsize; fsblkcnt_t f_blocks, f_bfree, f_bavail; fsfilcnt_t f_files, f_ffree; fsid_t f_fsid;
	unsigned long f_namelen, f_frsize, f_flags, f_spare[4];
};
#define off64_t off_t
#define ino64_t ino_t
#define blkcnt64_t blkcnt_t
#define fsblkcnt64_t fsblkcnt_t
#define fsfilcnt64_t fsfilcnt_t
#define fpos64_t fpos_t
#define dirent64 dirent
#define rlimit64 rlimit
#define glob64_t glob_t
#define statvfs64 statvfs
#define stat64(p, b) stat(p, (struct stat *)(b))
#define fstat64(f, b) fstat(f, (struct stat *)(b))
#define lstat64(p, b) lstat(p, (struct stat *)(b))
#define fstatat64(d, p, b, fl) fstatat(d, p, (struct stat *)(b), fl)
#define statfs64(p, b) statfs(p, (struct statfs *)(b))
#define fstatfs64(f, b) fstatfs(f, (struct statfs *)(b))
#define fstatvfs64(...) fstatvfs(__VA_ARGS__)
#define fopen64(...) fopen(__VA_ARGS__)
#define freopen64(...) freopen(__VA_ARGS__)
#define tmpfile64(...) tmpfile(__VA_ARGS__)
#define fseeko64(...) fseeko(__VA_ARGS__)
#define ftello64(...) ftello(__VA_ARGS__)
#define fgetpos64(...) fgetpos(__VA_ARGS__)
#define fsetpos64(...) fsetpos(__VA_ARGS__)
#define lseek64(...) lseek(__VA_ARGS__)
#define pread64(...) pread(__VA_ARGS__)
#define pwrite64(...) pwrite(__VA_ARGS__)
#define preadv64(...) preadv(__VA_ARGS__)
#define pwritev64(...) pwritev(__VA_ARGS__)
#define readdir64(...) readdir(__VA_ARGS__)
#define ftruncate64(...) ftruncate(__VA_ARGS__)
#define truncate64(...) truncate(__VA_ARGS__)
#define getrlimit64(...) getrlimit(__VA_ARGS__)
#define setrlimit64(...) setrlimit(__VA_ARGS__)
#define prlimit64(...) prlimit(__VA_ARGS__)
#define glob64(...) glob(__VA_ARGS__)
#define globfree64(...) globfree(__VA_ARGS__)
#define fallocate64(...) fallocate(__VA_ARGS__)
#define posix_fallocate64(...) posix_fallocate(__VA_ARGS__)
#define posix_fadvise64(...) posix_fadvise(__VA_ARGS__)
#define sendfile64(...) sendfile(__VA_ARGS__)
#define scandir64(...) scandir(__VA_ARGS__)
#define alphasort64(...) alphasort(__VA_ARGS__)
#define versionsort64(...) versionsort(__VA_ARGS__)
#define open64(...) open(__VA_ARGS__)
#define openat64(...) openat(__VA_ARGS__)
#define creat64(...) creat(__VA_ARGS__)
#define ftw64(...) ftw(__VA_ARGS__)
#define nftw64(...) nftw(__VA_ARGS__)
#define lockf64(...) lockf(__VA_ARGS__)
#define mkstemp64(...) mkstemp(__VA_ARGS__)
#define mkostemp64(...) mkostemp(__VA_ARGS__)
#define mkstemps64(...) mkstemps(__VA_ARGS__)
#define mkostemps64(...) mkostemps(__VA_ARGS__)
#define fcntl64(...) fcntl(__VA_ARGS__)
#define mmap64(...) mmap(__VA_ARGS__)
#include <pthread.h>
#ifndef PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP PTHREAD_MUTEX_INITIALIZER
#endif
#include <dirent.h>
static inline int scandirat(int fd, const char *d, struct dirent ***n, int (*s)(const struct dirent *), int (*c)(const struct dirent **, const struct dirent **))
{ (void)fd; return scandir(d, n, s, c); }
struct mallinfo { int arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };
