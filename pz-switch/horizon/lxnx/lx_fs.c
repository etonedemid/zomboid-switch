// Files: Linux fds over newlib fds / DIR handles. Linux "/" is LX_ROOT on the SD card; /proc and /sys don't exist
// (SVM and the JDK fall back when they're missing); /dev/null and /dev/(u)random are emulated.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include "lx.h"
#include "lx_tarfs.h"

enum { FD_FREE, FD_FILE, FD_DIR, FD_LOG, FD_NULL, FD_RANDOM, FD_TAR, FD_TARDIR, FD_MEM, FD_DSP, FD_EVENT, FD_PIPER, FD_PIPEW, FD_EPOLL };

typedef struct {
    int kind, nfd, flags;
    DIR *dir;
    char *path;                 // Linux path (dirs: for *at() lookups)
    struct dirent *pending;     // entry that didn't fit in the last getdents64 buffer
    TarNode *tn, *titer;        // FD_TAR / FD_TARDIR: archive node, next child to list
    uint64_t pos;               // FD_TAR / FD_MEM position
    char *mem; size_t memlen;   // FD_MEM: generated file contents (/proc/self/maps)
    void *dsp; int dsp_channels; // FD_DSP: audout playback (lx_alsa.c)
    struct PollObj *po;         // FD_EVENT / FD_PIPER / FD_PIPEW / FD_EPOLL: shared state (see the poll section)
    Mutex lock;                 // pread/pwrite = lseek + read under this
    struct Shared *sh;          // FD_FILE: native handle shared with other fds (see do_open)
} Fd;
// Horizon refuses a second open of a file that is open for writing; Linux allows it (SQLite opens a database once per
// connection). Such opens share the first fd's native handle: one refcount and one lock for the lseek+read/write pairs.
struct Shared { int refs; Mutex lock; };
#define FDLOCK(f) ((f)->sh ? &(f)->sh->lock : &(f)->lock)

#define MAXFD 1024
static Fd fds[MAXFD];
static char cwd[1024] = "/";
static Mutex fd_lock;

static int lx_err(void) {  // newlib errno -> Linux errno
    switch (errno) {
    case ENOENT: return -LX_ENOENT; case EEXIST: return -LX_EEXIST; case ENOTDIR: return -LX_ENOTDIR;
    case EISDIR: return -LX_EISDIR; case EACCES: return -LX_EACCES; case ENOSPC: return -LX_ENOSPC;
    case ENOTEMPTY: return -LX_ENOTEMPTY; case EINVAL: return -LX_EINVAL; case EBADF: return -LX_EBADF;
    case ENOMEM: return -LX_ENOMEM; default: return -LX_EIO;
    }
}

void lx_fs_init(void) {
    for (int i = 0; i < 3; i++) fds[i].kind = i == 0 ? FD_NULL : FD_LOG;
    mkdir(LX_ROOT, 0777);
    lx_tarfs_init();
}

static int alloc_fd(int kind) {
    mutexLock(&fd_lock);
    for (int i = 3; i < MAXFD; i++)
        if (fds[i].kind == FD_FREE) { memset(&fds[i], 0, sizeof fds[i]); fds[i].kind = kind; mutexUnlock(&fd_lock); return i; }
    mutexUnlock(&fd_lock);
    return -LX_EMFILE;
}

static Fd *get(long fd) { return fd >= 0 && fd < MAXFD && fds[fd].kind != FD_FREE ? &fds[fd] : NULL; }

// Linux path (relative to dirfd) -> normalized absolute Linux path in out
static int resolve(long dirfd, const char *p, char *out, size_t cap) {
    char tmp[2048];
    if (p[0] == '/') snprintf(tmp, sizeof tmp, "%s", p);
    else {
        const char *base = cwd;
        if (dirfd != LX_AT_FDCWD) { Fd *d = get(dirfd); if (!d || !d->path) return -LX_EBADF; base = d->path; }
        snprintf(tmp, sizeof tmp, "%s/%s", base, p);
    }
    // normalize: split on '/', handle . and ..
    size_t n = 0; out[0] = 0;
    char *save;
    for (char *s = strtok_r(tmp, "/", &save); s; s = strtok_r(NULL, "/", &save)) {
        if (!strcmp(s, ".")) continue;
        if (!strcmp(s, "..")) { while (n && out[n - 1] != '/') n--; if (n) n--; out[n] = 0; continue; }
        if (n + strlen(s) + 2 > cap) return -LX_ERANGE;
        out[n++] = '/'; strcpy(out + n, s); n += strlen(s);
    }
    if (!n) strcpy(out, "/");
    return 0;
}

static void host(const char *lp, char *out, size_t cap) { snprintf(out, cap, "%s%s", LX_ROOT, strcmp(lp, "/") ? lp : ""); }

size_t lx_proc_maps(char *buf, size_t cap);
static int special(const char *lp) {
    if (!strcmp(lp, "/proc/self/maps") || !strcmp(lp, "/proc/cpuinfo")) return FD_MEM;
    if (!strcmp(lp, "/dev/null") || !strcmp(lp, "/dev/zero")) return FD_NULL;
    if (!strcmp(lp, "/dev/urandom") || !strcmp(lp, "/dev/random")) return FD_RANDOM;
    if (!strcmp(lp, "/dev/dsp")) return FD_DSP;   // OSS playback (openal-soft)
    if (!strncmp(lp, "/proc", 5) || !strncmp(lp, "/sys", 4) || !strncmp(lp, "/dev", 4)) return -1;
    return 0;
}

static void to_lx_stat(const struct stat *s, struct lx_stat *o, const char *lp) {
    memset(o, 0, sizeof *o);
    uint64_t h = 1469598103934665603ull;     // stable fake inode from the path
    for (const char *c = lp; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    o->st_dev = 1; o->st_ino = h; o->st_nlink = 1;
    o->st_mode = (S_ISDIR(s->st_mode) ? LX_S_IFDIR : LX_S_IFREG) | 0755;
    o->st_size = s->st_size; o->st_blksize = 4096; o->st_blocks = (s->st_size + 511) / 512;
    o->st_atime_sec = s->st_atime; o->st_mtime_sec = s->st_mtime; o->st_ctime_sec = s->st_ctime;
}

static void tar_stat(const TarNode *t, struct lx_stat *o, const char *lp) {
    struct stat st; memset(&st, 0, sizeof st);
    st.st_mode = t->is_dir ? S_IFDIR : S_IFREG; st.st_size = t->size;
    to_lx_stat(&st, o, lp);
}

// fsdev fails a read at or past the end of file (EIO) where POSIX returns 0
static ssize_t file_read(int nfd, void *buf, size_t n) {
    ssize_t r = read(nfd, buf, n);
    if (r < 0 && errno == EIO) {
        struct stat st;
        if (!fstat(nfd, &st) && lseek(nfd, 0, SEEK_CUR) >= st.st_size) return 0;
        errno = EIO;
    }
    return r;
}

long lx_fd_pread(int fd, void *buf, size_t n, int64_t off) {
    Fd *f = get(fd);
    if (f && f->kind == FD_TAR) return lx_tarfs_pread(f->tn, buf, n, off);
    if (!f || f->kind != FD_FILE) return -LX_EBADF;
    mutexLock(FDLOCK(f));
    off_t old = lseek(f->nfd, 0, SEEK_CUR);
    lseek(f->nfd, off, SEEK_SET);
    ssize_t r = file_read(f->nfd, buf, n);
    int e = errno;
    lseek(f->nfd, old, SEEK_SET);
    mutexUnlock(FDLOCK(f));
    errno = e;
    return r < 0 ? lx_err() : r;
}

static long do_open(long dirfd, const char *p, int flags, int mode) {
    char lp[1024], hp[1100];
    int rc = resolve(dirfd, p, lp, sizeof lp);
    if (rc) return rc;
    int sp = special(lp);
    if (sp < 0) return -LX_ENOENT;
    if (sp == FD_MEM) {
        int fd = alloc_fd(FD_MEM);
        if (fd < 0) return fd;
        fds[fd].mem = malloc(65536);
        if (!strcmp(lp, "/proc/cpuinfo")) {   // Tegra X1: four Cortex-A57; NEON detection (openal-soft, OptiFine)
            size_t n = 0;
            for (int c = 0; c < 4; c++)
                n += snprintf(fds[fd].mem + n, 65536 - n, "processor\t: %d\nBogoMIPS\t: 38.40\nFeatures\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
                              "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x1\nCPU part\t: 0xd07\nCPU revision\t: 1\n\n", c);
            fds[fd].memlen = n;
        } else fds[fd].memlen = lx_proc_maps(fds[fd].mem, 65536);
        return fd;
    }
    if (sp == FD_DSP) {
        void *lx_dsp_open(void);
        void *d = lx_dsp_open();
        if (!d) return -LX_EIO;
        int fd = alloc_fd(FD_DSP);
        if (fd < 0) return fd;
        fds[fd].dsp = d; fds[fd].dsp_channels = 2;
        return fd;
    }
    if (sp) { int fd = alloc_fd(sp); return fd; }
    TarNode *tn = lx_tarfs_lookup(lp);
    if (tn) {
        if ((flags & LX_O_ACCMODE) != 0 || (flags & (LX_O_CREAT | LX_O_TRUNC))) return -LX_EACCES;  // the archive is read-only
        if ((flags & LX_O_DIRECTORY) && !tn->is_dir) return -LX_ENOTDIR;
        int fd = alloc_fd(tn->is_dir ? FD_TARDIR : FD_TAR);
        if (fd < 0) return fd;
        fds[fd].tn = tn; fds[fd].titer = tn->child; fds[fd].path = strdup(lp); fds[fd].flags = flags;
        return fd;
    }
    host(lp, hp, sizeof hp);
    struct stat st;
    int exists = stat(hp, &st) == 0;
    if (exists && (flags & LX_O_CREAT) && (flags & LX_O_EXCL)) return -LX_EEXIST;
    if ((exists && S_ISDIR(st.st_mode)) || (flags & LX_O_DIRECTORY)) {
        if (!exists) return -LX_ENOENT;
        if (!S_ISDIR(st.st_mode)) return -LX_ENOTDIR;
        DIR *d = opendir(hp);
        if (!d) return lx_err();
        int fd = alloc_fd(FD_DIR);
        if (fd < 0) { closedir(d); return fd; }
        fds[fd].dir = d; fds[fd].path = strdup(lp); fds[fd].flags = flags;
        return fd;
    }
    int nf = (flags & LX_O_ACCMODE) == 1 ? O_WRONLY : (flags & LX_O_ACCMODE) == 2 ? O_RDWR : O_RDONLY;
    if (flags & LX_O_CREAT) nf |= O_CREAT;
    if (flags & LX_O_TRUNC) nf |= O_TRUNC;
    if (flags & LX_O_APPEND) nf |= O_APPEND;
    int h = open(hp, nf, mode);
    Fd *same = NULL;
    if (h < 0) {
        mutexLock(&fd_lock);
        for (int i = 3; i < MAXFD && !same; i++)
            if (fds[i].kind == FD_FILE && fds[i].path && !strcmp(fds[i].path, lp)) same = &fds[i];
        if (same) {
            if (!same->sh) { same->sh = calloc(1, sizeof *same->sh); same->sh->refs = 1; }
            same->sh->refs++;
            h = same->nfd;
        }
        mutexUnlock(&fd_lock);
        if (!same) return lx_err();
        if (flags & LX_O_TRUNC) ftruncate(h, 0);
    }
    int fd = alloc_fd(FD_FILE);
    if (fd < 0) { if (same) { mutexLock(&fd_lock); same->sh->refs--; mutexUnlock(&fd_lock); } else close(h); return fd; }
    fds[fd].nfd = h; fds[fd].path = strdup(lp); fds[fd].flags = flags; fds[fd].sh = same ? same->sh : NULL;
    if (same) lx_logf("open %s: shares fd %d's handle\n", lp, (int)(same - fds));
    if (strstr(lp, ".db")) lx_logf("open %s flags %#x -> fd %d (nfd %d)\n", lp, flags, fd, h);   // debug
    return fd;
}

// ---- eventfd, pipes, epoll, ppoll: what Java NIO selectors (netty's event loops, Minecraft's integrated server)
// need. In-process only: readiness of eventfds and pipes is real, every other fd counts as always ready. One lock and
// one condition variable for all of it; any state change wakes every waiter.
#define PIPE_CAP 65536
#define MAX_WATCH 64
typedef struct PollObj {
    int refs;
    uint64_t count;                                     // eventfd
    int semaphore;
    char *buf; size_t head, len; int readers, writers;  // pipe ring
    struct { int fd; uint32_t events; uint64_t data; } w[MAX_WATCH];   // epoll interest list
    int nw;
} PollObj;
static Mutex poll_lock;
static CondVar poll_cv;
static void poll_wake(void) { condvarWakeAll(&poll_cv); }
// wait on the condition with a deadline (ns since boot; 0 = none); returns 0 on timeout
static int poll_wait(u64 deadline) {
    if (!deadline) { condvarWait(&poll_cv, &poll_lock); return 1; }
    u64 now = armTicksToNs(armGetSystemTick());
    if (now >= deadline) return 0;
    condvarWaitTimeout(&poll_cv, &poll_lock, deadline - now);
    return 1;
}
enum { L_POLLIN = 1, L_POLLOUT = 4, L_POLLERR = 8, L_POLLHUP = 0x10, L_POLLNVAL = 0x20 };
static unsigned readiness(int fd) {   // under poll_lock
    Fd *f = fd >= 0 && fd < MAXFD && fds[fd].kind != FD_FREE ? &fds[fd] : NULL;
    if (!f) return L_POLLNVAL;
    switch (f->kind) {
    case FD_EVENT: return (f->po->count ? L_POLLIN : 0) | L_POLLOUT;
    case FD_PIPER: return (f->po->len ? L_POLLIN : 0) | (f->po->writers ? 0 : L_POLLHUP);
    case FD_PIPEW: return (f->po->len < PIPE_CAP ? L_POLLOUT : 0) | (f->po->readers ? 0 : L_POLLERR);
    case FD_EPOLL: return 0;
    default: return L_POLLIN | L_POLLOUT;
    }
}
static PollObj *pobj_new(void) { PollObj *o = calloc(1, sizeof *o); o->refs = 1; return o; }
static void pobj_unref(Fd *f) {   // close of an FD_EVENT/PIPE/EPOLL
    mutexLock(&poll_lock);
    PollObj *o = f->po;
    if (f->kind == FD_PIPER) o->readers--;
    if (f->kind == FD_PIPEW) o->writers--;
    if (--o->refs == 0) { free(o->buf); free(o); }
    f->po = NULL;
    poll_wake();
    mutexUnlock(&poll_lock);
}
static u64 deadline_ms(long ms) { return ms < 0 ? 0 : armTicksToNs(armGetSystemTick()) + (u64)ms * 1000000ull + (ms == 0); }
static long pobj_rw(Fd *f, void *buf, size_t n, int write_) {
    long r;
    int nb = (f->flags & LX_O_NONBLOCK) != 0;
    mutexLock(&poll_lock);
    PollObj *o = f->po;
    if (f->kind == FD_EVENT) {
        if (n < 8) r = -LX_EINVAL;
        else if (write_) { o->count += *(uint64_t *)buf; poll_wake(); r = 8; }
        else {
            while (!o->count && !nb) poll_wait(0);
            if (!o->count) r = -LX_EAGAIN;
            else { uint64_t v = o->semaphore ? 1 : o->count; o->count -= v; *(uint64_t *)buf = v; poll_wake(); r = 8; }
        }
    } else if (f->kind == FD_PIPER && !write_) {
        while (!o->len && o->writers && !nb) poll_wait(0);
        if (!o->len) r = o->writers ? -LX_EAGAIN : 0;
        else {
            size_t k = n < o->len ? n : o->len;
            for (size_t i = 0; i < k; i++) ((char *)buf)[i] = o->buf[(o->head + i) % PIPE_CAP];
            o->head = (o->head + k) % PIPE_CAP; o->len -= k; r = k; poll_wake();
        }
    } else if (f->kind == FD_PIPEW && write_) {
        size_t done = 0;
        r = 0;
        while (done < n) {
            if (!o->readers) { r = done ? (long)done : -32; break; }   // EPIPE
            if (o->len == PIPE_CAP) { if (nb) { r = done ? (long)done : -LX_EAGAIN; break; } poll_wait(0); continue; }
            size_t k = n - done < PIPE_CAP - o->len ? n - done : PIPE_CAP - o->len;
            for (size_t i = 0; i < k; i++) o->buf[(o->head + o->len + i) % PIPE_CAP] = ((const char *)buf)[done + i];
            o->len += k; done += k; r = done; poll_wake();
        }
    } else r = -LX_EBADF;
    mutexUnlock(&poll_lock);
    return r;
}
static long sys_epoll_pwait(int epfd, uint8_t *evs, int max, long timeout) {
    Fd *e = get(epfd);
    if (!e || e->kind != FD_EPOLL || max <= 0) return -LX_EINVAL;
    u64 dl = deadline_ms(timeout);
    long n = 0;
    mutexLock(&poll_lock);
    for (;;) {
        PollObj *o = e->po;
        for (int i = 0; i < o->nw && n < max; i++) {
            unsigned r = readiness(o->w[i].fd) & (o->w[i].events | L_POLLERR | L_POLLHUP);
            if (!r) continue;
            *(uint32_t *)(evs + n * 16) = r; *(uint64_t *)(evs + n * 16 + 8) = o->w[i].data;   // aarch64: 16-byte epoll_event
            n++;
        }
        if (n || timeout == 0 || !poll_wait(dl)) break;
    }
    mutexUnlock(&poll_lock);
    return n;
}
static long sys_ppoll(uint8_t *pfds, long nfds, const int64_t *ts) {
    u64 dl = ts ? armTicksToNs(armGetSystemTick()) + ts[0] * 1000000000ull + ts[1] + 1 : 0;
    long n = 0;
    mutexLock(&poll_lock);
    for (;;) {
        n = 0;
        for (long i = 0; i < nfds; i++) {
            int fd = *(int *)(pfds + i * 8);
            short ev = *(short *)(pfds + i * 8 + 4);
            unsigned r = fd < 0 ? 0 : readiness(fd) & (ev | L_POLLERR | L_POLLHUP | L_POLLNVAL);
            *(short *)(pfds + i * 8 + 6) = r;
            if (r) n++;
        }
        if (n || (ts && !ts[0] && !ts[1]) || !poll_wait(dl)) break;
    }
    mutexUnlock(&poll_lock);
    return n;
}

// OSS ioctls openal-soft's OSS backend issues; the device is fixed at 48 kHz s16 and four 1024-frame fragments
static long dsp_ioctl(Fd *f, unsigned long req, int *arg) {
    void lx_dsp_channels(void *, int);
    switch ((unsigned)req) {   // musl passes the request as an int: it arrives sign-extended
    case 0x5000: return 0;                                    // SNDCTL_DSP_RESET
    case 0xc0045002: *arg = 48000; return 0;                  // SNDCTL_DSP_SPEED
    case 0xc0045005: *arg = 0x10; return 0;                   // SNDCTL_DSP_SETFMT: AFMT_S16_LE only
    case 0xc0045006:                                          // SNDCTL_DSP_CHANNELS
        if (*arg < 1) *arg = 1;
        if (*arg > 2) *arg = 2;
        f->dsp_channels = *arg; lx_dsp_channels(f->dsp, *arg);
        return 0;
    case 0xc004500a: return 0;                                // SNDCTL_DSP_SETFRAGMENT: fixed fragments
    case 0x8010500c:                                          // SNDCTL_DSP_GETOSPACE: audio_buf_info
        arg[0] = 4; arg[1] = 4; arg[2] = 1024 * 2 * f->dsp_channels; arg[3] = arg[0] * arg[2];
        return 0;
    }
    return -LX_ENOTTY;
}

static long do_close(long fd) {
    Fd *f = get(fd);
    if (!f) return -LX_EBADF;
    if (f->path && strstr(f->path, ".db")) lx_logf("close(%ld) %s\n", fd, f->path);   // debug
    if (f->kind == FD_FILE) {
        mutexLock(&fd_lock);
        int last = !f->sh || --f->sh->refs == 0;
        if (f->sh && last) free(f->sh);
        mutexUnlock(&fd_lock);
        if (last) close(f->nfd);
    }
    if (f->kind == FD_DIR) closedir(f->dir);
    if (f->kind == FD_MEM) free(f->mem);
    if (f->kind == FD_DSP) { void lx_dsp_close(void *); lx_dsp_close(f->dsp); }
    if (f->po) pobj_unref(f);
    free(f->path);
    if (fd >= 3) f->kind = FD_FREE;
    return 0;
}

static long do_rw(long fd, void *buf, size_t n, int write_) {
    Fd *f = get(fd);
    if (!f) return -LX_EBADF;
    switch (f->kind) {
    case FD_LOG:
        if (write_) lx_log_write(buf, n);
        return write_ ? (long)n : 0;
    case FD_NULL: if (!write_) memset(buf, 0, n); return write_ ? (long)n : 0;
    case FD_RANDOM: if (!write_) randomGet(buf, n); return n;
    case FD_EVENT: case FD_PIPER: case FD_PIPEW: return pobj_rw(f, buf, n, write_);
    case FD_DSP: { long lx_dsp_write(void *, const void *, size_t); return write_ ? lx_dsp_write(f->dsp, buf, n) : -LX_EINVAL; }
    case FD_DIR: case FD_TARDIR: return -LX_EISDIR;
    case FD_MEM: {
        if (write_) return -LX_EBADF;
        size_t k = f->pos < f->memlen ? f->memlen - f->pos : 0;
        if (k > n) k = n;
        memcpy(buf, f->mem + f->pos, k); f->pos += k;
        return k;
    }
    case FD_TAR: {
        if (write_) return -LX_EBADF;
        ssize_t r = lx_tarfs_pread(f->tn, buf, n, f->pos);
        if (r > 0) f->pos += r;
        return r;
    }
    }
    ssize_t r = write_ ? write(f->nfd, buf, n) : file_read(f->nfd, buf, n);
    return r < 0 ? lx_err() : r;
}

static long do_getdents(long fd, uint8_t *buf, size_t cap) {
    Fd *f = get(fd);
    if (f && f->kind == FD_TARDIR) {
        size_t off = 0;
        for (; f->titer; f->titer = f->titer->next) {
            size_t nl = strlen(f->titer->name), reclen = (19 + nl + 1 + 7) & ~7ul;
            if (off + reclen > cap) { if (!off) return -LX_EINVAL; break; }
            uint8_t *r = buf + off;
            uint64_t ino = 2 + off; int64_t doff = off + reclen;
            memcpy(r, &ino, 8); memcpy(r + 8, &doff, 8);
            r[16] = reclen & 0xff; r[17] = reclen >> 8;
            r[18] = f->titer->is_dir ? 4 : 8;
            memcpy(r + 19, f->titer->name, nl + 1);
            off += reclen;
        }
        return off;
    }
    if (!f || f->kind != FD_DIR) return -LX_EBADF;
    size_t off = 0;
    for (;;) {
        struct dirent *e = f->pending ? f->pending : readdir(f->dir);
        f->pending = NULL;
        if (!e) break;
        size_t nl = strlen(e->d_name), reclen = (19 + nl + 1 + 7) & ~7ul;
        if (off + reclen > cap) { f->pending = e; if (!off) return -LX_EINVAL; break; }
        uint8_t *r = buf + off;
        uint64_t ino = 2 + off; int64_t doff = off + reclen;
        memcpy(r, &ino, 8); memcpy(r + 8, &doff, 8);
        r[16] = reclen & 0xff; r[17] = reclen >> 8;
        r[18] = e->d_type == DT_DIR ? 4 : 8;   // DT_DIR / DT_REG
        memcpy(r + 19, e->d_name, nl + 1);
        off += reclen;
    }
    return off;
}

static long do_stat(long dirfd, const char *p, struct lx_stat *o, int flags) {
    char lp[1024], hp[1100];
    struct stat st;
    if ((flags & LX_AT_EMPTY_PATH) && (!p || !*p)) {
        Fd *f = get(dirfd);
        if (!f) { lx_logf("fstat(%ld): not open\n", dirfd); return -LX_EBADF; }
        if (f->kind == FD_FILE) {
            if (fstat(f->nfd, &st)) {   // seen failing (EIO) on a just-created file; the path still answers
                int e = errno;
                host(f->path, hp, sizeof hp);
                if (stat(hp, &st)) { lx_logf("fstat(%ld) %s: nfd %d errno %d, stat errno %d\n", dirfd, f->path, f->nfd, e, errno); return lx_err(); }
            }
            to_lx_stat(&st, o, f->path);
            if (strstr(f->path, ".db")) lx_logf("fstat(%ld) %s: size %ld mode %o ino %lu\n", dirfd, f->path, (long)o->st_size, o->st_mode, (unsigned long)o->st_ino);   // debug
            return 0;
        }
        if (f->kind == FD_TAR || f->kind == FD_TARDIR) { tar_stat(f->tn, o, f->path); return 0; }
        if (f->kind == FD_DIR) { memset(&st, 0, sizeof st); st.st_mode = S_IFDIR; to_lx_stat(&st, o, f->path); return 0; }
        memset(o, 0, sizeof *o); o->st_mode = 0020666; return 0;   // character device
    }
    int rc = resolve(dirfd, p, lp, sizeof lp);
    if (rc) return rc;
    int sp = special(lp);
    if (sp < 0) return -LX_ENOENT;
    if (sp) { memset(o, 0, sizeof *o); o->st_mode = 0020666; return 0; }
    TarNode *tn = lx_tarfs_lookup(lp);
    if (tn) { tar_stat(tn, o, lp); return 0; }
    host(lp, hp, sizeof hp);
    if (!strcmp(lp, "/")) { memset(&st, 0, sizeof st); st.st_mode = S_IFDIR; to_lx_stat(&st, o, lp); return 0; }
    if (stat(hp, &st)) {   // fsdev opens the file for its size, which Horizon refuses (EIO) while we hold it open for writing
        int e = errno, ok = 0;
        mutexLock(&fd_lock);
        for (int i = 0; i < MAXFD && !ok; i++)
            if (fds[i].kind == FD_FILE && fds[i].path && !strcmp(fds[i].path, lp)) ok = !fstat(fds[i].nfd, &st);
        mutexUnlock(&fd_lock);
        if (!ok) { errno = e; return lx_err(); }
    }
    to_lx_stat(&st, o, lp);
    return 0;
}

// statx through do_stat: the JDK's NIO (Files.exists, newDirectoryStream filters) calls it directly
static long do_statx(long dirfd, const char *p, int flags, uint8_t *o) {
    struct lx_stat st;
    long rc = do_stat(dirfd, p, &st, flags);
    if (rc) return rc;
    memset(o, 0, 256);
    struct { uint32_t mask, blksize; uint64_t attributes; uint32_t nlink, uid, gid; uint16_t mode, pad; uint64_t ino, size, blocks, attributes_mask;
             struct { int64_t sec; uint32_t nsec; int32_t pad; } atime, btime, ctime, mtime; } x = {
        0x7ff, st.st_blksize, 0, st.st_nlink, st.st_uid, st.st_gid, st.st_mode, 0, st.st_ino, st.st_size, st.st_blocks, 0,
        { st.st_atime_sec, st.st_atime_nsec, 0 }, { 0, 0, 0 }, { st.st_ctime_sec, st.st_ctime_nsec, 0 }, { st.st_mtime_sec, st.st_mtime_nsec, 0 } };
    memcpy(o, &x, sizeof x);   // STATX_BASIC_STATS; dev/rdev left 0
    return 0;
}

static long path_op(long n, long dirfd, const char *p, long a2, long a3, long a4) {
    char lp[1024], hp[1100], lp2[1024], hp2[1100];
    int rc = resolve(dirfd, p, lp, sizeof lp);
    if (rc) return rc;
    if (n == LX_readlinkat && !strcmp(lp, "/proc/self/exe")) {   // native-image puts JDK libraries next to the executable
        size_t k = (size_t)a3 < 4 ? (size_t)a3 : 4;
        memcpy((char *)a2, "/app", k);
        return k;
    }
    if (special(lp)) return n == LX_faccessat && special(lp) > 0 ? 0 : -LX_ENOENT;
    TarNode *tn = lx_tarfs_lookup(lp);
    if (tn && n == LX_faccessat) return 0;
    if (tn && n == LX_chdir) { if (!tn->is_dir) return -LX_ENOTDIR; snprintf(cwd, sizeof cwd, "%s", lp); return 0; }
    host(lp, hp, sizeof hp);
    struct stat st;
    switch (n) {
    case LX_faccessat: return stat(hp, &st) ? lx_err() : 0;
    case LX_mkdirat: return mkdir(hp, 0777) ? lx_err() : 0;
    case LX_unlinkat: return ((a2 & LX_AT_REMOVEDIR) ? rmdir(hp) : unlink(hp)) ? lx_err() : 0;
    case LX_renameat: case LX_renameat2:
        if ((rc = resolve(a2, (const char *)a3, lp2, sizeof lp2))) return rc;
        host(lp2, hp2, sizeof hp2);
        if (!stat(hp2, &st) && !S_ISDIR(st.st_mode)) unlink(hp2);   // Linux rename replaces, Horizon's doesn't
        return rename(hp, hp2) ? lx_err() : 0;
    case LX_chdir:
        if (strcmp(lp, "/") && (stat(hp, &st) || !S_ISDIR(st.st_mode))) return -LX_ENOTDIR;
        snprintf(cwd, sizeof cwd, "%s", lp);
        return 0;
    case LX_readlinkat: return -LX_EINVAL;
    }
    (void)a4;
    return -LX_ENOSYS;
}

long lx_sys_fs(long n, long a, long b, long c, long d, long e, long f, int *handled) {
    (void)e; (void)f;
    *handled = 1;
    switch (n) {
    case LX_openat: return do_open(a, (const char *)b, c, d);
    case LX_close: return do_close(a);
    case LX_read: return do_rw(a, (void *)b, c, 0);
    case LX_write: return do_rw(a, (void *)b, c, 1);
    case LX_readv: case LX_writev: {
        long total = 0;
        const struct { void *base; size_t len; } *iov = (void *)b;
        for (int i = 0; i < c; i++) {
            long r = do_rw(a, iov[i].base, iov[i].len, n == LX_writev);
            if (r < 0) return total ? total : r;
            total += r;
            if ((size_t)r < iov[i].len) break;
        }
        return total;
    }
    case LX_pread64: return lx_fd_pread(a, (void *)b, c, d);
    case LX_pwrite64: {
        Fd *fp = get(a);
        if (!fp || fp->kind != FD_FILE) return -LX_EBADF;
        mutexLock(FDLOCK(fp));
        off_t old = lseek(fp->nfd, 0, SEEK_CUR);
        lseek(fp->nfd, d, SEEK_SET);
        ssize_t r = write(fp->nfd, (void *)b, c);
        lseek(fp->nfd, old, SEEK_SET);
        mutexUnlock(FDLOCK(fp));
        return r < 0 ? lx_err() : r;
    }
    case LX_lseek: {
        Fd *fp = get(a);
        if (!fp) return -LX_EBADF;
        if (fp->kind == FD_TAR) {
            int64_t np = c == 0 ? b : c == 1 ? (int64_t)fp->pos + b : (int64_t)fp->tn->size + b;
            if (np < 0) return -LX_EINVAL;
            return fp->pos = np;
        }
        if (fp->kind == FD_TARDIR) { if (b == 0) fp->titer = fp->tn->child; return 0; }
        if (fp->kind != FD_FILE) return fp->kind == FD_DIR ? 0 : -LX_ESPIPE;
        off_t r = lseek(fp->nfd, b, c);
        return r < 0 ? lx_err() : r;
    }
    case LX_getdents64: return do_getdents(a, (uint8_t *)b, c);
    case LX_fstat: return do_stat(a, "", (struct lx_stat *)b, LX_AT_EMPTY_PATH);
    case LX_newfstatat: return do_stat(a, (const char *)b, (struct lx_stat *)c, d);
    case LX_statx: return do_statx(a, (const char *)b, c, (uint8_t *)e);
    case LX_fchown: case LX_fchownat: return 0;   // one user   // musl falls back to fstatat
    case LX_ftruncate: { Fd *fp = get(a); return fp && fp->kind == FD_FILE ? (ftruncate(fp->nfd, b) ? lx_err() : 0) : -LX_EBADF; }
    case LX_fsync: case LX_fdatasync: { Fd *fp = get(a); if (fp && fp->kind == FD_FILE) fsync(fp->nfd); return 0; }
    case LX_fcntl: {
        Fd *fp = get(a);
        if (!fp) return -LX_EBADF;
        switch (b) {
        case 0: case 1030: {  // F_DUPFD(_CLOEXEC)
            int nfd = alloc_fd(fp->kind);
            if (nfd < 0) return nfd;
            Fd *g = &fds[nfd];
            g->flags = fp->flags; g->path = fp->path ? strdup(fp->path) : NULL;
            g->tn = fp->tn; g->titer = fp->titer; g->pos = fp->pos;
            if (fp->kind == FD_FILE) g->nfd = dup(fp->nfd);
            if (fp->kind == FD_DIR) { char hp[1100]; host(fp->path, hp, sizeof hp); g->dir = opendir(hp); }
            if (fp->po) {
                mutexLock(&poll_lock);
                g->po = fp->po; g->po->refs++;
                if (fp->kind == FD_PIPER) g->po->readers++;
                if (fp->kind == FD_PIPEW) g->po->writers++;
                mutexUnlock(&poll_lock);
            }
            return nfd;
        }
        case 3: return fp->flags;   // F_GETFL
        case 4: fp->flags = (fp->flags & ~LX_O_NONBLOCK) | (c & LX_O_NONBLOCK); return 0;   // F_SETFL: O_NONBLOCK
        default: return 0;          // F_GETFD/SETFD, locks: pretend
        }
    }
    case LX_dup: return lx_sys_fs(LX_fcntl, a, 0, 0, 0, 0, 0, handled);
    case LX_ioctl: { Fd *fp = get(a); return fp && fp->kind == FD_DSP ? dsp_ioctl(fp, b, (int *)c) : -LX_ENOTTY; }
    case LX_pselect6: {   // ponytail: /dev/dsp waits for a free buffer; any other fd asked about is "ready"
        int n = 0;
        int lx_dsp_wait(void *, u64);
        u64 limit = e ? ((int64_t *)e)[0] * 1000000000ull + ((int64_t *)e)[1] : 1000000000ull;
        for (long i = 0; i < a; i++) {
            for (int k = 0; k < 2; k++) {
                unsigned long *set = (unsigned long *)(k ? c : b);
                if (!set || !(set[i / 64] >> (i % 64) & 1)) continue;
                Fd *fp = get(i);
                if (k && fp && fp->kind == FD_DSP && !lx_dsp_wait(fp->dsp, limit)) { set[i / 64] &= ~(1ul << (i % 64)); continue; }
                n++;
            }
        }
        if (d) memset((void *)d, 0, (a + 7) / 8);
        return n;
    }
    case LX_faccessat: case LX_mkdirat: case LX_chdir: case LX_readlinkat:
        return path_op(n, n == LX_chdir ? LX_AT_FDCWD : a, (const char *)(n == LX_chdir ? a : b), c, d, e);
    case LX_unlinkat: return path_op(n, a, (const char *)b, c, 0, 0);
    case LX_renameat: case LX_renameat2: return path_op(n, a, (const char *)b, c, d, e);
    case LX_getcwd: {
        size_t l = strlen(cwd) + 1;
        if ((size_t)b < l) return -LX_ERANGE;
        memcpy((void *)a, cwd, l);
        return l;
    }
    case LX_statfs: case LX_fstatfs: {
        if (n == LX_statfs && special((const char *)a) < 0) return -LX_ENOENT;
        uint64_t *s = (uint64_t *)b;
        memset(s, 0, 120);
        s[0] = 0x4d44; s[1] = 32768; s[2] = s[3] = s[4] = 1 << 20; s[5] = s[6] = 1 << 20; s[8] = 255; s[9] = 32768;
        return 0;
    }
    case LX_utimensat: case LX_fchmod: case LX_fchmodat: return 0;
    case LX_ppoll: return sys_ppoll((uint8_t *)a, b, (const int64_t *)c);
    case LX_pipe2: {
        int r = alloc_fd(FD_PIPER);
        if (r < 0) return r;
        int w = alloc_fd(FD_PIPEW);
        if (w < 0) { fds[r].kind = FD_FREE; return w; }
        PollObj *o = pobj_new();
        o->refs = 2; o->readers = 1; o->writers = 1; o->buf = malloc(PIPE_CAP);
        fds[r].po = fds[w].po = o;
        fds[r].flags = fds[w].flags = b & LX_O_NONBLOCK;
        ((int *)a)[0] = r; ((int *)a)[1] = w;
        return 0;
    }
    // inotify: Java's WatchService (the game's DebugFileWatcher) needs one to exist; it never reports a change
    case 26: {   // inotify_init1
        int fd = alloc_fd(FD_EVENT);
        if (fd < 0) return fd;
        fds[fd].po = pobj_new(); fds[fd].flags = a & LX_O_NONBLOCK;   // IN_NONBLOCK == O_NONBLOCK
        return fd;
    }
    case 27: { static int wd; return ++wd; }   // inotify_add_watch
    case 28: return 0;                          // inotify_rm_watch
    case 199: return lx_sys_fs(LX_pipe2, d, b & LX_O_NONBLOCK, 0, 0, 0, 0, handled);   // socketpair: WatchService's wakeup
                                                                                        // channel only needs sv[1] -> sv[0]
    case LX_eventfd2: {
        int fd = alloc_fd(FD_EVENT);
        if (fd < 0) return fd;
        fds[fd].po = pobj_new(); fds[fd].po->count = (unsigned)a; fds[fd].po->semaphore = b & 1;
        fds[fd].flags = b & LX_O_NONBLOCK;   // EFD_NONBLOCK == O_NONBLOCK
        return fd;
    }
    case LX_epoll_create1: {
        int fd = alloc_fd(FD_EPOLL);
        if (fd >= 0) fds[fd].po = pobj_new();
        return fd;
    }
    case LX_epoll_ctl: {
        Fd *e = get(a);
        if (!e || e->kind != FD_EPOLL || !get(c)) return -LX_EBADF;
        mutexLock(&poll_lock);
        PollObj *o = e->po;
        int i = 0;
        while (i < o->nw && o->w[i].fd != c) i++;
        long r = 0;
        uint32_t ev = d ? *(uint32_t *)d : 0; uint64_t data = d ? *(uint64_t *)(d + 8) : 0;
        if (b == 1) {        // EPOLL_CTL_ADD
            if (i < o->nw) r = -LX_EEXIST;
            else if (o->nw == MAX_WATCH) r = -LX_ENOMEM;
            else { o->w[o->nw].fd = c; o->w[o->nw].events = ev; o->w[o->nw].data = data; o->nw++; }
        } else if (i == o->nw) r = -LX_ENOENT;
        else if (b == 2) o->w[i] = o->w[--o->nw];               // EPOLL_CTL_DEL
        else { o->w[i].events = ev; o->w[i].data = data; }     // EPOLL_CTL_MOD
        poll_wake();
        mutexUnlock(&poll_lock);
        return r;
    }
    case LX_epoll_pwait: return sys_epoll_pwait(a, (uint8_t *)b, c, d);
    }
    *handled = 0;
    return 0;
}
