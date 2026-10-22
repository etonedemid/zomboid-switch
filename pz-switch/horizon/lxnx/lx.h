// lxnx: a Linux (aarch64, asm-generic) syscall personality on top of libnx, for the musl-linked Java side.
// Everything here is Horizon-side code (newlib); Linux ABI values are spelled out because <linux/*> can't coexist with newlib.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <switch.h>

// --- syscall numbers (asm-generic, aarch64) ---
enum {
    LX_eventfd2 = 19, LX_epoll_create1 = 20, LX_epoll_ctl = 21, LX_epoll_pwait = 22, LX_getcwd = 17, LX_fcntl = 25, LX_ioctl = 29, LX_mkdirat = 34, LX_unlinkat = 35, LX_renameat = 38,
    LX_statfs = 43, LX_fstatfs = 44, LX_ftruncate = 46, LX_faccessat = 48, LX_chdir = 49, LX_fchmod = 52, LX_fchmodat = 53, LX_fchownat = 54, LX_fchown = 55,
    LX_openat = 56, LX_close = 57, LX_pipe2 = 59, LX_getdents64 = 61, LX_lseek = 62, LX_read = 63, LX_write = 64,
    LX_readv = 65, LX_writev = 66, LX_pread64 = 67, LX_pwrite64 = 68, LX_pselect6 = 72, LX_ppoll = 73, LX_readlinkat = 78,
    LX_newfstatat = 79, LX_fstat = 80, LX_fsync = 82, LX_fdatasync = 83, LX_utimensat = 88,
    LX_exit = 93, LX_exit_group = 94, LX_set_tid_address = 96, LX_futex = 98, LX_set_robust_list = 99, LX_nanosleep = 101,
    LX_clock_gettime = 113, LX_clock_getres = 114, LX_clock_nanosleep = 115, LX_sched_setaffinity = 122,
    LX_sched_getaffinity = 123, LX_sched_yield = 124, LX_kill = 129, LX_tkill = 130, LX_tgkill = 131,
    LX_sigaltstack = 132, LX_rt_sigaction = 134, LX_rt_sigprocmask = 135, LX_times = 153, LX_uname = 160,
    LX_getrlimit = 163, LX_getrusage = 165, LX_umask = 166, LX_prctl = 167, LX_gettimeofday = 169,
    LX_getpid = 172, LX_getppid = 173, LX_getuid = 174, LX_geteuid = 175, LX_getgid = 176, LX_getegid = 177,
    LX_gettid = 178, LX_sysinfo = 179, LX_socket = 198, LX_brk = 214, LX_munmap = 215, LX_mremap = 216,
    LX_mmap = 222, LX_mprotect = 226, LX_msync = 227, LX_mlock = 228, LX_madvise = 233, LX_prlimit64 = 261,
    LX_renameat2 = 276, LX_getrandom = 278, LX_membarrier = 283, LX_statx = 291, LX_rseq = 293,
    LX_dup = 23, LX_dup3 = 24, LX_mincore = 232,
};

// --- errno (asm-generic) ---
enum {
    LX_EPERM = 1, LX_ENOENT = 2, LX_EINTR = 4, LX_EIO = 5, LX_EBADF = 9, LX_EAGAIN = 11, LX_ENOMEM = 12,
    LX_EACCES = 13, LX_EFAULT = 14, LX_EEXIST = 17, LX_ENOTDIR = 20, LX_EISDIR = 21, LX_EINVAL = 22,
    LX_EMFILE = 24, LX_ENOTTY = 25, LX_ENOSPC = 28, LX_ESPIPE = 29, LX_ERANGE = 34, LX_ENOSYS = 38,
    LX_ENOTEMPTY = 39, LX_ETIMEDOUT = 110, LX_EAFNOSUPPORT = 97, LX_EOPNOTSUPP = 95,
};

// --- flags ---
#define LX_O_ACCMODE 03
#define LX_O_CREAT 0100
#define LX_O_EXCL 0200
#define LX_O_TRUNC 01000
#define LX_O_APPEND 02000
#define LX_O_NONBLOCK 04000
#define LX_O_DIRECTORY 040000
#define LX_O_CLOEXEC 02000000
#define LX_AT_FDCWD (-100)
#define LX_AT_REMOVEDIR 0x200
#define LX_AT_SYMLINK_NOFOLLOW 0x100
#define LX_AT_EMPTY_PATH 0x1000

#define LX_PROT_NONE 0
#define LX_MAP_SHARED 0x01
#define LX_MAP_PRIVATE 0x02
#define LX_MAP_FIXED 0x10
#define LX_MAP_ANONYMOUS 0x20
#define LX_MAP_NORESERVE 0x4000
#define LX_MAP_FIXED_NOREPLACE 0x100000
#define LX_MREMAP_MAYMOVE 1
#define LX_MADV_DONTNEED 4

#define LX_FUTEX_WAIT 0
#define LX_FUTEX_WAKE 1
#define LX_FUTEX_REQUEUE 3
#define LX_FUTEX_CMP_REQUEUE 4
#define LX_FUTEX_WAIT_BITSET 9
#define LX_FUTEX_WAKE_BITSET 10
#define LX_FUTEX_CMD_MASK 0x7f  // strips PRIVATE (128) and CLOCK_REALTIME (256)

#define LX_CLONE_THREAD 0x00010000
#define LX_CLONE_SETTLS 0x00080000
#define LX_CLONE_PARENT_SETTID 0x00100000
#define LX_CLONE_CHILD_CLEARTID 0x00200000

#define LX_S_IFDIR 0040000
#define LX_S_IFREG 0100000

// aarch64 generic struct stat (128 bytes)
struct lx_stat {
    uint64_t st_dev, st_ino;
    uint32_t st_mode, st_nlink, st_uid, st_gid;
    uint64_t st_rdev, __pad1;
    int64_t st_size;
    int32_t st_blksize, __pad2;
    int64_t st_blocks;
    int64_t st_atime_sec, st_atime_nsec, st_mtime_sec, st_mtime_nsec, st_ctime_sec, st_ctime_nsec;
    uint32_t unused_[2];
};

struct lx_timespec { int64_t tv_sec, tv_nsec; };

// --- shared state / helpers ---
#ifndef LX_ROOT
#define LX_ROOT "sdmc:/pz"           // Linux "/" lives here on the SD card (per app: -DLX_ROOT=...)
#endif
#define LX_MAIN_STACK (64u << 20)     // Java main thread stack

extern FILE *lx_log;
void lx_logf(const char *fmt, ...);
void lx_log_write(const void *buf, size_t n);
void lx_log_flush(void);
_Noreturn void lx_fatal(const char *fmt, ...);

long lx_sys_mem(long n, long a, long b, long c, long d, long e, long f, int *handled);
long lx_sys_fs(long n, long a, long b, long c, long d, long e, long f, int *handled);
long lx_sys_thread(long n, long a, long b, long c, long d, long e, long f, int *handled);

// main thread stack bounds (for musl's mremap probe of the main stack)
extern uintptr_t lx_main_stack_lo, lx_main_stack_hi;
// lx_main.c: leave Java (exit_group from any thread)
_Noreturn void lx_exit_process(int code);
