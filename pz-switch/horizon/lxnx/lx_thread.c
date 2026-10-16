// Threads, futexes, time, signals and process info.
// A Linux clone() becomes a libnx thread whose entry switches onto the stack musl prepared, sets tpidr_el0 to musl's
// thread pointer (Horizon-side code is built -mtp=soft and never touches tpidr_el0) and calls fn(arg) there.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <time.h>
#include "lx.h"

typedef struct LxThread {
    Thread nx;
    int tid;
    int (*fn)(void *);
    void *arg, *stack, *tls;
    int *ctid;              // CLONE_CHILD_CLEARTID / set_tid_address target
    uintptr_t nx_sp;        // libnx stack pointer saved when switching to musl's stack
    struct LxThread *next;  // dead list
    int is_main;
} LxThread;

static __thread LxThread *cur;
static LxThread main_thread = { .tid = 1, .is_main = 1 };
static int next_tid = 2, next_core;
static LxThread *dead;
static Mutex thr_lock;

// lx_call_on_stack(arg, fn, stack_top, save_sp): saves sp to *save_sp, switches to stack_top, calls fn(arg), then
// lx_thread_returned(result) - never returns to the caller's frame.
__asm__(
    ".global lx_call_on_stack\n"
    ".type lx_call_on_stack, %function\n"
    "lx_call_on_stack:\n"
    "  mov x9, sp\n"
    "  str x9, [x3]\n"
    "  mov sp, x2\n"
    "  mov x29, #0\n"
    "  mov x30, #0\n"
    "  blr x1\n"
    "  b lx_thread_returned\n"
    // lx_switch_and_call(sp, fn, a0, a1): continue on sp with fn(a0, a1); fn must not return
    ".global lx_switch_and_call\n"
    ".type lx_switch_and_call, %function\n"
    "lx_switch_and_call:\n"
    "  mov sp, x0\n"
    "  mov x0, x2\n"
    "  mov x2, x1\n"
    "  mov x1, x3\n"
    "  br x2\n");
_Noreturn void lx_call_on_stack(void *arg, int (*fn)(void *), void *stack_top, uintptr_t *save_sp);
_Noreturn void lx_switch_and_call(uintptr_t sp, void (*fn)(uintptr_t, uintptr_t), uintptr_t a0, uintptr_t a1);

// ---- sampling profiler (sdmc:/<app>/profile exists): pauses every thread ~200 times a second, reads its PC
// (svcSetThreadActivity + svcGetThreadContext3 work on one's own threads), and every 10 s writes the hottest PCs as
// offsets into the binary to prof.txt - symbolize with addr2line -f on the .elf.
#define PROF_MAX_THREADS 128
#define PROF_SLOTS (1 << 16)
static Handle prof_threads[PROF_MAX_THREADS];
static int prof_tid[PROF_MAX_THREADS];
static Mutex prof_lock;
static void prof_add(Handle h, int tid) {
    mutexLock(&prof_lock);
    for (int i = 0; i < PROF_MAX_THREADS; i++) if (!prof_threads[i]) { prof_threads[i] = h; prof_tid[i] = tid; break; }
    mutexUnlock(&prof_lock);
}
static void prof_remove(Handle h) {
    mutexLock(&prof_lock);
    for (int i = 0; i < PROF_MAX_THREADS; i++) if (prof_threads[i] == h) prof_threads[i] = 0;
    mutexUnlock(&prof_lock);
}
// one entry per distinct (thread, call stack): pc plus up to PROF_DEPTH-1 return addresses from the frame chain
#define PROF_DEPTH 8
typedef struct { uint64_t a[PROF_DEPTH]; uint32_t tid, n; } ProfEnt;
static ProfEnt prof_tab[PROF_SLOTS];
static unsigned long prof_samples, prof_idle;
static void prof_hit(const uint64_t *a, int tid) {
    uint64_t h = tid;
    for (int k = 0; k < PROF_DEPTH; k++) h = (h ^ a[k]) * 1099511628211ull;
    for (uint32_t i = h & (PROF_SLOTS - 1), probes = 0; probes < 64; i = (i + 1) & (PROF_SLOTS - 1), probes++) {
        ProfEnt *e = &prof_tab[i];
        if (e->n && (e->tid != (uint32_t)tid || memcmp(e->a, a, sizeof e->a))) continue;
        if (!e->n) { memcpy(e->a, a, sizeof e->a); e->tid = tid; }
        e->n++;
        return;
    }
}
static int prof_cmp(const void *a, const void *b) { return (int)(*(ProfEnt *const *)b)->n - (int)(*(ProfEnt *const *)a)->n; }
void lx_prof_start(void);
static void prof_dump(void) {
    static ProfEnt *top[PROF_SLOTS];
    int n = 0;
    for (int i = 0; i < PROF_SLOTS; i++) if (prof_tab[i].n) top[n++] = &prof_tab[i];
    qsort(top, n, sizeof top[0], prof_cmp);
    FILE *f = fopen(LX_ROOT "/prof.txt", "w");
    if (!f) return;
    // the anchor gives the load address: base = anchor - (link address of lx_prof_start)
    fprintf(f, "# %lu samples, %lu waiting in svc; anchor lx_prof_start=%p; count tid pc callers...\n",
            prof_samples, prof_idle, (void *)lx_prof_start);
    for (int i = 0; i < n && i < 600; i++) {
        fprintf(f, "%u %u", top[i]->n, top[i]->tid);
        for (int k = 0; k < PROF_DEPTH && top[i]->a[k]; k++) fprintf(f, " %lx", (unsigned long)top[i]->a[k]);
        fputc('\n', f);
    }
    fclose(f);
}
static int readable(uint64_t p) {   // a frame record the paused thread's fp points at: mapped, readable, 16 bytes
    if (p & 7 || p < 0x8000000 || p >= 0x8000000000ull) return 0;
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, p))) return 0;
    return mi.type != MemType_Unmapped && (mi.perm & Perm_R) && p + 16 <= mi.addr + mi.size;
}
static void profiler(void *arg) {
    (void)arg;
    u64 last = armGetSystemTick();
    for (;;) {
        svcSleepThread(5000000);
        mutexLock(&prof_lock);
        for (int i = 0; i < PROF_MAX_THREADS; i++) {
            Handle h = prof_threads[i];
            if (!h || R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) continue;
            ThreadContext ctx;
            Result rc = svcGetThreadContext3(&ctx, h);
            uint64_t a[PROF_DEPTH] = { 0 };
            if (R_SUCCEEDED(rc)) {
                a[0] = ctx.pc.x; a[1] = ctx.lr;
                uint64_t fp = ctx.fp;   // walk the frame chain while the thread is still paused
                for (int k = 2; k < PROF_DEPTH && readable(fp); k++) { a[k] = ((uint64_t *)fp)[1]; fp = ((uint64_t *)fp)[0]; }
            }
            svcSetThreadActivity(h, ThreadActivity_Runnable);
            if (R_FAILED(rc)) continue;
            prof_samples++;
            uint32_t at = *(volatile uint32_t *)ctx.pc.x, before = *(volatile uint32_t *)(ctx.pc.x - 4);
            if ((at & 0xffe0001f) == 0xd4000001 || (before & 0xffe0001f) == 0xd4000001) prof_idle++;
            prof_hit(a, prof_tid[i]);
        }
        mutexUnlock(&prof_lock);
        if (armTicksToNs(armGetSystemTick() - last) > 10000000000ull) { prof_dump(); last = armGetSystemTick(); }
    }
}
void lx_prof_start(void) {
    FILE *f = fopen(LX_ROOT "/profile", "r");
    if (!f) return;
    fclose(f);
    prof_add(envGetMainThreadHandle(), 1);
    static Thread t;
    if (R_SUCCEEDED(threadCreate(&t, profiler, NULL, NULL, 0x8000, 0x2B, -2))) { threadStart(&t); lx_logf("profiler on\n"); }
}

static void reap(void) {
    while (dead) {
        LxThread *t = dead; dead = t->next;
        threadWaitForExit(&t->nx);
        threadClose(&t->nx);
        free(t);
    }
}

static _Noreturn void thread_exit(int code) {
    LxThread *t = cur;
    if (!t || t->is_main) lx_exit_process(code);
    if (t->ctid) {
        __atomic_store_n(t->ctid, 0, __ATOMIC_SEQ_CST);
        svcSignalToAddress(t->ctid, SignalType_Signal, 0, -1);
    }
    prof_remove(t->nx.handle);
    mutexLock(&thr_lock);
    t->next = dead; dead = t;   // freed by a later clone(); the libnx stack we may be on lives until then
    mutexUnlock(&thr_lock);
    threadExit();
}

_Noreturn void lx_thread_returned(long code) { thread_exit((int)code); }

static void unmap_and_exit(uintptr_t base, uintptr_t size) {
    int h;
    lx_sys_mem(LX_munmap, base, size, 0, 0, 0, 0, &h);
    thread_exit(0);
}

// musl: a detached thread frees its own stack, then exits. Get off that stack first.
_Noreturn void __lx_unmapself(void *base, size_t size) {
    lx_switch_and_call(cur->nx_sp, (void (*)(uintptr_t, uintptr_t))unmap_and_exit, (uintptr_t)base, size);
}

static void entry(void *p) {
    LxThread *t = p;
    cur = t;
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(t->tls));
    lx_call_on_stack(t->arg, t->fn, t->stack, &t->nx_sp);
}

int __lx_clone(int (*fn)(void *), void *stack, int flags, void *arg, int *ptid, void *tls, int *ctid) {
    // no processes: fork/vfork (posix_spawn, popen) fail cleanly instead of running a child on our fd table
    if (!(flags & LX_CLONE_THREAD)) return -LX_ENOSYS;
    LxThread *t = calloc(1, sizeof *t);
    mutexLock(&thr_lock);
    reap();
    t->tid = next_tid++;
    int core = next_core++ % 3;
    mutexUnlock(&thr_lock);
    t->fn = fn; t->arg = arg; t->stack = stack;
    t->tls = (flags & LX_CLONE_SETTLS) ? tls : NULL;
    t->ctid = (flags & LX_CLONE_CHILD_CLEARTID) ? ctid : NULL;
    if (flags & LX_CLONE_PARENT_SETTID) *ptid = t->tid;
    // the libnx stack only runs entry() and the exit path; Java code runs on musl's stack
    Result rc = threadCreate(&t->nx, entry, t, NULL, 0x10000, 0x2C, core);
    if (R_FAILED(rc)) { lx_logf("clone: threadCreate %#x\n", rc); free(t); return -LX_EAGAIN; }
    svcSetThreadCoreMask(t->nx.handle, core, 0x7);   // any of the three application cores
    prof_add(t->nx.handle, t->tid);
    rc = threadStart(&t->nx);
    if (R_FAILED(rc)) { lx_logf("clone: threadStart %#x\n", rc); prof_remove(t->nx.handle); threadClose(&t->nx); free(t); return -LX_EAGAIN; }
    return t->tid;
}

void lx_thread_init_main(void) { cur = &main_thread; }

static LxThread *self(void) {
    if (!cur) {  // a Horizon-side thread calling into Java code
        cur = calloc(1, sizeof *cur);
        mutexLock(&thr_lock); cur->tid = next_tid++; mutexUnlock(&thr_lock);
    }
    return cur;
}

// --- time ---
static int64_t mono_ns(void) { return armTicksToNs(armGetSystemTick()); }
static int64_t real_ns(void) {
    static int64_t offset;  // wall clock at boot of this process, refined once
    if (!offset) offset = (int64_t)time(NULL) * 1000000000ll - mono_ns();
    return offset + mono_ns();
}
static int64_t ts_ns(const struct lx_timespec *t) { return t->tv_sec * 1000000000ll + t->tv_nsec; }
static void ns_ts(int64_t ns, struct lx_timespec *t) { t->tv_sec = ns / 1000000000ll; t->tv_nsec = ns % 1000000000ll; }

static int64_t thread_cpu_ns(void) {
    u64 ticks = 0;
    svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, (u64)-1);
    return armTicksToNs(ticks);
}

static long futex(int *uaddr, int op, int val, const struct lx_timespec *timeout, int *uaddr2, int val3) {
    switch (op & LX_FUTEX_CMD_MASK) {
    case LX_FUTEX_WAIT:
    case LX_FUTEX_WAIT_BITSET: {
        s64 ns = -1;
        if (timeout) {
            ns = ts_ns(timeout);
            if ((op & LX_FUTEX_CMD_MASK) == LX_FUTEX_WAIT_BITSET)  // absolute
                ns -= (op & 256) ? real_ns() : mono_ns();
            if (ns < 0) ns = 0;
        }
        Result rc = svcWaitForAddress(uaddr, ArbitrationType_WaitIfEqual, val, ns);
        if (R_SUCCEEDED(rc)) return 0;
        if (rc == 0xEA01) return -LX_ETIMEDOUT;
        if (rc == 0xFA01) return -LX_EAGAIN;   // value changed before we slept
        return -LX_EINTR;
    }
    case LX_FUTEX_WAKE:
    case LX_FUTEX_WAKE_BITSET:
        svcSignalToAddress(uaddr, SignalType_Signal, 0, val);
        return val;
    case LX_FUTEX_CMP_REQUEUE:
        if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val3) return -LX_EAGAIN;
        // fallthrough
    case LX_FUTEX_REQUEUE:
        // ponytail: wake everyone instead of requeueing; waiters re-check their condition (spurious wakeups are legal)
        svcSignalToAddress(uaddr, SignalType_Signal, 0, -1);
        svcSignalToAddress(uaddr2, SignalType_Signal, 0, -1);
        return val;
    }
    return -LX_ENOSYS;
}

static int fatal_signal(int sig) { return sig == 4 || sig == 6 || sig == 7 || sig == 8 || sig == 9 || sig == 11 || sig == 15; }

long lx_sys_thread(long n, long a, long b, long c, long d, long e, long f, int *handled) {
    (void)f;
    *handled = 1;
    switch (n) {
    case LX_futex: return futex((int *)a, b, c, (const struct lx_timespec *)d, (int *)e, (int)f);
    case LX_exit: thread_exit(a);
    case LX_exit_group: lx_exit_process(a);
    case LX_gettid: return self()->tid;
    case LX_getpid: return 1;
    case LX_getppid: return 0;
    case LX_set_tid_address: self()->ctid = (int *)a; return self()->tid;
    case LX_getuid: case LX_geteuid: case LX_getgid: case LX_getegid: return 0;
    case LX_set_robust_list: case LX_rt_sigaction: case LX_rt_sigprocmask: case LX_sigaltstack: case LX_prctl:
    case LX_sched_setaffinity:
        if (n == LX_rt_sigaction && d == 8 && c) memset((void *)c, 0, 32);  // old action: SIG_DFL
        if (n == LX_rt_sigprocmask && c) memset((void *)c, 0, d);
        return 0;
    case LX_rseq: return -LX_ENOSYS;
    case 118: case 119: case 140: return 0;                  // sched_setparam/setscheduler, setpriority: accepted, ignored
    case 120: return 0;                                     // sched_getscheduler: SCHED_OTHER
    case 121: if (b) *(int *)b = 0; return 0;               // sched_getparam
    case 125: return a == 0 ? 0 : 99;                       // sched_get_priority_max
    case 126: return a == 0 ? 0 : 1;                        // sched_get_priority_min
    case 141: return 20;                                    // getpriority: nice 0
    case LX_kill: case LX_tkill: case LX_tgkill: {
        int sig = n == LX_tgkill ? c : b;
        if (fatal_signal(sig)) lx_fatal("signal %d raised (abort)\n", sig);
        return 0;
    }
    case LX_sched_yield: svcSleepThread(0); return 0;
    case LX_nanosleep: {
        int64_t ns = ts_ns((const struct lx_timespec *)a);
        svcSleepThread(ns > 0 ? ns : 0);
        if (b) memset((void *)b, 0, sizeof(struct lx_timespec));
        return 0;
    }
    case LX_clock_nanosleep: {
        int64_t ns = ts_ns((const struct lx_timespec *)c);
        if (b & 1) ns -= a == 0 ? real_ns() : mono_ns();  // TIMER_ABSTIME
        svcSleepThread(ns > 0 ? ns : 0);
        return 0;
    }
    case LX_clock_gettime: {
        int64_t ns;
        switch (a) {
        case 0: case 5: case 11: ns = real_ns(); break;           // REALTIME(_COARSE), TAI
        case 2: case 3: ns = thread_cpu_ns(); break;              // ponytail: process CPU time = calling thread's
        default: ns = mono_ns(); break;                           // MONOTONIC(_RAW/_COARSE), BOOTTIME
        }
        ns_ts(ns, (struct lx_timespec *)b);
        return 0;
    }
    case LX_clock_getres: if (b) ns_ts(52, (struct lx_timespec *)b); return 0;  // 19.2 MHz tick
    case LX_gettimeofday: {
        int64_t ns = real_ns();
        if (a) { ((int64_t *)a)[0] = ns / 1000000000ll; ((int64_t *)a)[1] = ns % 1000000000ll / 1000; }
        return 0;
    }
    case LX_sched_getaffinity: {
        if (b < 8) return -LX_EINVAL;
        memset((void *)c, 0, b);
        *(uint8_t *)c = 0x7;   // three application cores
        return 8;
    }
    case LX_times: {
        int64_t t = thread_cpu_ns() / 10000000;  // clock ticks at 100 Hz
        if (a) { int64_t *tms = (int64_t *)a; tms[0] = t; tms[1] = tms[2] = tms[3] = 0; }
        return mono_ns() / 10000000;
    }
    case LX_getrusage: {
        memset((void *)b, 0, 144);
        int64_t ns = thread_cpu_ns();
        ((int64_t *)b)[0] = ns / 1000000000; ((int64_t *)b)[1] = ns % 1000000000 / 1000;
        return 0;
    }
    case LX_getrlimit: case LX_prlimit64: {
        int res = n == LX_prlimit64 ? b : a;
        uint64_t *out = (uint64_t *)(n == LX_prlimit64 ? d : b);
        if (!out) return 0;
        uint64_t v = res == 7 ? 1024 : res == 3 ? LX_MAIN_STACK : ~0ull;  // NOFILE, STACK, else unlimited
        out[0] = out[1] = v;
        return 0;
    }
    case LX_sysinfo: {
        uint64_t total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        struct mallinfo mi = mallinfo();
        used = mi.uordblks;
        uint8_t *si = (uint8_t *)a;
        memset(si, 0, 112);
        *(int64_t *)si = mono_ns() / 1000000000;
        *(uint64_t *)(si + 32) = total;                                 // totalram
        *(uint64_t *)(si + 40) = total > used ? total - used : 0;       // freeram
        *(uint16_t *)(si + 80) = 1;                                     // procs
        *(uint32_t *)(si + 104) = 1;                                    // mem_unit
        return 0;
    }
    case LX_uname: {
        char *u = (char *)a;
        memset(u, 0, 65 * 6);
        strcpy(u, "Linux"); strcpy(u + 65, "switch"); strcpy(u + 130, "5.10.0-horizon");
        strcpy(u + 195, "#1 SMP"); strcpy(u + 260, "aarch64");
        return 0;
    }
    case LX_getrandom: randomGet((void *)a, b); return b;
    case LX_umask: return 022;
    case LX_socket: return -LX_EAFNOSUPPORT;  // TODO: map onto libnx BSD sockets for multiplayer
    }
    *handled = 0;
    return 0;
}
