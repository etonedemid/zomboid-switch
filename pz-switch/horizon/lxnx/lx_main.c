// Process entry and syscall dispatch. main() builds what a Linux kernel would hand a static executable
// (argv/envp/auxv with program headers describing the TLS segment) and starts the Java side on this thread,
// on a big stack of its own.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <elf.h>
#include <sys/stat.h>
#include <unistd.h>
#include "lx.h"

FILE *lx_log;
uintptr_t lx_main_stack_lo, lx_main_stack_hi;
static Mutex log_lock;

// Lines collect in memory and go to the file in one open/append/close (the debug service can read it while we run: an
// open file is locked). The watchdog flushes every 100 ms and before it traps, the fatal paths flush; a hard crash
// loses at most the last 100 ms. Opening the file per line cost ~6 ms a line, a third of startup with chatty mods.
static char log_buf[64 << 10];
static size_t log_len;
static void log_flush_locked(void) {
    if (!log_len) return;
    FILE *f = fopen(LX_ROOT "/lx.log", "ab");
    if (f) { fwrite(log_buf, 1, log_len, f); fclose(f); }
    log_len = 0;
}
// never block for long: a thread that dies or overflows its stack while logging would otherwise freeze every logger,
// the watchdog and its hang detector included (seen with Minecraft's server thread)
static int log_lock_try(void) {
    for (int i = 0; i < 200; i++) { if (mutexTryLock(&log_lock)) return 1; svcSleepThread(1000000); }
    return 0;
}
void lx_log_flush(void) {
    if (!lx_log || !log_lock_try()) return;
    log_flush_locked();
    mutexUnlock(&log_lock);
}
void lx_log_write(const void *buf, size_t n) {
    if (!lx_log || !log_lock_try()) return;
    if (log_len + n > sizeof log_buf) log_flush_locked();
    if (n > sizeof log_buf) { FILE *f = fopen(LX_ROOT "/lx.log", "ab"); if (f) { fwrite(buf, 1, n, f); fclose(f); } }
    else { memcpy(log_buf + log_len, buf, n); log_len += n; }
    mutexUnlock(&log_lock);
}

void lx_logf(const char *fmt, ...) {
    if (!lx_log) return;
    char line[1024];
    va_list ap; va_start(ap, fmt);
    int k = snprintf(line, sizeof line, "[%8.3f] ", armTicksToNs(armGetSystemTick()) / 1e9);
    vsnprintf(line + k, sizeof line - k, fmt, ap);
    va_end(ap);
    lx_log_write(line, strlen(line));
}

_Noreturn void lx_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char line[1024];
    vsnprintf(line, sizeof line, fmt, ap);
    lx_log_write("FATAL: ", 7); lx_log_write(line, strlen(line));
    va_end(ap);
    lx_log_flush();
    exit(1);
}

extern char __start__[];
// newlib's abort() (Mesa asserts, libstdc++) quits silently; log who called it, then trap for a crash report with a stack
_Noreturn void abort(void) {
    lx_logf("abort() from pz+%#lx\n", (uintptr_t)__builtin_return_address(0) - (uintptr_t)__start__);
    lx_log_flush();
    __builtin_trap();
}

// Linked with --wrap=fatalThrow,--wrap=diagAbortWithResult: Mesa's Switch winsys calls these when the display queue
// fails, which takes the whole console down until a reboot. Log the result and caller, then crash just this app so
// Atmosphere writes a report with every thread's stack.
static _Noreturn void app_fatal(const char *what, Result rc, void *caller) {
    lx_logf("%s(%#x = %u-%04u) from pz+%#lx\n", what, rc, 2000 + R_MODULE(rc), R_DESCRIPTION(rc), (uintptr_t)caller - (uintptr_t)__start__);
    lx_log_flush();
    svcBreak(BreakReason_Panic, 0, 0);
    for (;;) svcSleepThread(1000000000ll);
}
void __wrap_fatalThrow(Result rc) { app_fatal("fatalThrow", rc, __builtin_return_address(0)); }
void __wrap_diagAbortWithResult(Result rc) { app_fatal("diagAbortWithResult", rc, __builtin_return_address(0)); }

static void at_exit(void) { lx_logf("atexit: process exiting\n"); }

_Noreturn void lx_exit_process(int code) {
    lx_logf("exit(%d)\n", code);
    // libnx's exit returns to the loader with the main thread's saved context; from any other thread (Minecraft exits
    // from its server thread) that jumps into the weeds, so leave directly
    if (threadGetCurHandle() != envGetMainThreadHandle()) svcExitProcess();
    exit(code);
}

volatile int lx_sys_trace;   // debug: log every syscall (but futex/yield/clock) once set
long __lx_syscall(long n, long a, long b, long c, long d, long e, long f) {
    int handled;
    if (lx_sys_trace && n != 98 && n != 124 && n != 113) lx_logf("sys %ld(%lx, %lx, %lx, %lx)\n", n, a, b, c, d);
    long r = lx_sys_fs(n, a, b, c, d, e, f, &handled);
    if (!handled) r = lx_sys_mem(n, a, b, c, d, e, f, &handled);
    if (!handled) r = lx_sys_thread(n, a, b, c, d, e, f, &handled);
    if (!handled) { lx_logf("unimplemented syscall %ld (%lx, %lx, %lx)\n", n, a, b, c); r = -LX_ENOSYS; }
    return r;
}

extern char __tdata_lma[], __tdata_lma_end[], __tls_start[], __tls_end[];
extern const uint64_t __tls_align[];
void lx_fs_init(void);
void lx_thread_init_main(void);
int lx_java_entry(long *block);
_Noreturn void lx_call_on_stack(void *arg, int (*fn)(void *), void *stack_top, uintptr_t *save_sp);

static Elf64_Phdr phdrs[2];
static uint8_t at_random[16];

// Debug aid for hangs: creating sdmc:/pz/crashnow makes us crash on purpose, so Atmosphère writes a report with
// every thread's backtrace.
volatile u64 lx_last_swap;   // hang detector: no frame for a minute once frames started -> crash report with every stack
extern volatile u64 lx_gl_last_tick;
extern volatile int lx_gl_armed;
// The watchdog takes no locks (no malloc, no log, no newlib FILE): a thread stuck holding newlib's heap lock froze
// it once, hang detector included. It breaks first; the statistics line lives in its own thread (statlog).
static void watchdog(void *arg) {
    (void)arg;
    u64 prev = armGetSystemTick();
    for (int n = 0;; n++) {
        svcSleepThread(100000000ll);
        lx_log_flush();
        // The system tick keeps running while the console sleeps (or we sit suspended behind the HOME menu), and all
        // our threads stop. Back from that, the time since the last frame says nothing: start counting again.
        u64 now = armGetSystemTick();
        if ((s64)(now - prev) > (s64)armNsToTicks(2000000000ull)) {
            if (lx_last_swap) lx_last_swap = now;
            lx_gl_last_tick = now;
        }
        prev = now;
        if (lx_gl_armed && (s64)(armGetSystemTick() - lx_gl_last_tick) > (s64)armNsToTicks(400000000ull))
            svcBreak(BreakReason_Panic, 0, 0);   // debug trap armed by the GL tracer
        if (lx_last_swap && (s64)(armGetSystemTick() - lx_last_swap) > (s64)armNsToTicks(180000000000ull))
            svcBreak(BreakReason_Panic, 0, 0);   // no frame for 3 min: crash report with every thread's stack (loading mods on a slow SD card can take over 30 s without one)
        struct stat st;
        if (n % 20 == 0 && stat(LX_ROOT "/crashnow", &st) == 0) { unlink(LX_ROOT "/crashnow"); svcBreak(BreakReason_Panic, 0, 0); }
    }
}

volatile unsigned long lx_frames, lx_frame_max_us;   // eglSwapBuffers calls and the longest frame, for the stats line
static void statlog(void *arg) {
    (void)arg;
    unsigned long last_frames = 0;
    for (;;) {
        svcSleepThread(2000000000ll);
        unsigned long fr = lx_frames;
        lx_logf("fps %.1f, longest frame %lu ms\n", (fr - last_frames) / 2.0, lx_frame_max_us / 1000);
        lx_frame_max_us = 0;
        last_frames = fr;
        u64 used = 0, total = 0;
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        struct mallinfo mi = mallinfo();
        char gl[128]; void lx_gl_stats(char *, size_t); lx_gl_stats(gl, sizeof gl);
        extern unsigned long lx_audio_appended, lx_audio_released, lx_audio_dropped;
        lx_logf("watchdog: memory %lu of %lu MiB, newlib heap %lu MiB in use; textures: %s; audio buffers %lu queued %lu back, %lu frames dropped\n",
                used >> 20, total >> 20, (unsigned long)mi.uordblks >> 20, gl, lx_audio_appended, lx_audio_released, lx_audio_dropped);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    lx_fs_init();
    appletSetAutoSleepDisabled(true);   // long installs / play sessions without input must not suspend us
    appletSetMediaPlaybackState(true);
    lx_log = fopen(LX_ROOT "/lx.log", "w");   // truncate; lx_log stays as the "logging on" marker
    if (lx_log) fclose(lx_log);
    s64 sd_free = 0, sd_total = 0;
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    if (sd) { fsFsGetFreeSpace(sd, "/", &sd_free); fsFsGetTotalSpace(sd, "/", &sd_total); }
    freopen(LX_ROOT "/h.log", "w", stderr);   // Horizon-side stderr (Mesa)
    setvbuf(stderr, NULL, _IONBF, 0);
    atexit(at_exit);
    lx_logf("lxnx start, SD %lld of %lld MiB free\n", (long long)(sd_free >> 20), (long long)(sd_total >> 20));

    // Java checks that a library file exists before loading it; the libraries themselves are linked in (lx_dl)
    static const char *libs[] = { "liblwjgl.so", "liblwjgl_opengl.so", "liblwjgl_stb.so", "libglfw.so", "libopenal.so", "libBink2x64.so",
        "libbink64.so", "libfmod.so", "libfmodintegration64.so", "libfmodstudio.so", "libimgui-java64.so", "libjassimp64.so",
        "libLighting64.so", "libPZBullet64.so", "libPZBulletNoOpenGL64.so", "libPZClipper64.so", "libpzexe_jni64.so",
        "libPZPathFind64.so", "libPZPopMan64.so", "libPZXInitThreads64.so", "libRakNet64.so", "libZNetJNI64.so",
        "libZNetNoSteam64.so", "libsteam_api.so", NULL };
    mkdir(LX_ROOT "/game", 0777); mkdir(LX_ROOT "/game/natives", 0777);
    mkdir(LX_ROOT "/tmp", 0777); mkdir(LX_ROOT "/home", 0777);
    for (int k = 0; libs[k]; k++) {
        char p[256]; snprintf(p, sizeof p, LX_ROOT "/game/natives/%s", libs[k]);
        FILE *pf = fopen(p, "ab"); if (pf) fclose(pf);
    }
    // JDK libraries, looked up next to the executable (/app)
    static const char *jdk[] = { "libawt.so", "libawt_headless.so", "libmlib_image.so", "libjavajpeg.so", "liblcms.so", NULL };
    for (int k = 0; jdk[k]; k++) {
        char p[256]; snprintf(p, sizeof p, LX_ROOT "/%s", jdk[k]);
        FILE *pf = fopen(p, "ab"); if (pf) fclose(pf);
    }

    // arguments: one per line from args.txt (argv[0] included)
    static char *args[256];
    int nargs = 0;
    FILE *af = fopen(LX_ROOT "/args.txt", "r");
    static char line[1024];
    while (af && nargs < 255 && fgets(line, sizeof line, af)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "@cwd=", 5)) { int h; lx_sys_fs(LX_chdir, (long)(line + 5), 0, 0, 0, 0, 0, &h); continue; }
        if (*line && *line != '#') args[nargs++] = strdup(line);
    }
    if (af) fclose(af);
    if (!nargs) args[nargs++] = "/app";
    // BOX64_CPU*: box64 would popen lscpu for them (no processes here)
    static char *envs[] = { "HOME=/home", "PATH=/bin", "LANG=en_US.UTF-8", "USER=switch", "TZ=UTC", "BOX64_CPUNAME=Cortex-A57", "BOX64_CPUMHZ=1785", NULL };

    phdrs[0] = (Elf64_Phdr){ .p_type = PT_PHDR, .p_vaddr = (uintptr_t)phdrs, .p_memsz = sizeof phdrs };
    phdrs[1] = (Elf64_Phdr){ .p_type = PT_TLS, .p_vaddr = (uintptr_t)__tdata_lma,
        .p_filesz = __tdata_lma_end - __tdata_lma, .p_memsz = __tls_end - __tls_start, .p_align = *__tls_align };
    randomGet(at_random, sizeof at_random);

    // block: argc, argv..., NULL, envp..., NULL, auxv pairs - at the top of the Java main stack, like a kernel
    // would put it (musl takes the main thread's stack top from the auxv address)
    void *stack = memalign(4096, LX_MAIN_STACK);
    lx_main_stack_lo = (uintptr_t)stack;
    lx_main_stack_hi = lx_main_stack_lo + LX_MAIN_STACK;
    long *block = (long *)(lx_main_stack_hi - 4096);
    int i = 0;
    block[i++] = nargs;
    for (int k = 0; k < nargs; k++) block[i++] = (long)args[k];
    block[i++] = 0;
    for (int k = 0; envs[k]; k++) block[i++] = (long)envs[k];
    block[i++] = 0;
    long aux[][2] = {
        { AT_PHDR, (long)phdrs }, { AT_PHENT, sizeof(Elf64_Phdr) }, { AT_PHNUM, 2 }, { AT_PAGESZ, 4096 },
        { AT_HWCAP, 0xff },  // fp asimd evtstrm aes pmull sha1 sha2 crc32 (Cortex-A57)
        { AT_CLKTCK, 100 }, { AT_UID, 0 }, { AT_EUID, 0 }, { AT_GID, 0 }, { AT_EGID, 0 }, { AT_SECURE, 0 },
        { AT_RANDOM, (long)at_random }, { AT_EXECFN, (long)args[0] }, { AT_NULL, 0 },
    };
    for (size_t k = 0; k < sizeof aux / sizeof *aux; k++) { block[i++] = aux[k][0]; block[i++] = aux[k][1]; }

    lx_logf("java main: %d args, stack %p, tls %p+%#lx/%#lx align %lu\n", nargs, stack, __tdata_lma,
            (long)phdrs[1].p_filesz, (long)phdrs[1].p_memsz, (unsigned long)phdrs[1].p_align);
    static Thread wd;
    if (R_SUCCEEDED(threadCreate(&wd, watchdog, NULL, NULL, 0x4000, 0x2C, -2))) threadStart(&wd);
    static Thread sl;
    if (R_SUCCEEDED(threadCreate(&sl, statlog, NULL, NULL, 0x4000, 0x2C, -2))) threadStart(&sl);
    lx_thread_init_main();
    void lx_prof_start(void); lx_prof_start();
    static uintptr_t saved_sp;
    lx_call_on_stack(block, (int (*)(void *))lx_java_entry, (void *)block, &saved_sp);
}
