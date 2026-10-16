// mmap family. Horizon gives this process all of its memory as one heap up front and has no lazy commit for us
// (no system resource size -> no svcMapPhysicalMemory), so every mapping is a page-aligned block from that heap.
// Per page we track "live" (still mapped) and "clean" (known zero); PROT_NONE reservations skip the memset and get
// zeroed when first made accessible.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "lx.h"

int lx_arena_init(size_t size);
int lx_arena_owns(uintptr_t p);
void *lx_arena_alloc(size_t len, size_t align);
void lx_arena_free(uintptr_t p, size_t len);
__attribute__((weak)) void nouveau_bo_cache_trim(void);   // libdrm_nouveau (GL builds)

#define PG 4096ul

typedef struct {
    uintptr_t base, size;
    uint8_t *live, *clean;  // bitmaps, 1 bit per page
    void *backing;          // low mappings: the heap block aliased at base (svcMapMemory), else NULL
    int lazy;               // big PROT_NONE reservation: address space only, committed in granules (see below)
    struct LazyMap { uintptr_t dst; void *src; Handle h; } *maps;   // lazy: one code-memory alias per committed granule
    int nmaps, capmaps;
    uintptr_t phantom;      // tail bytes inside [base, base+size) that are not ours (GC chunk reservations, see do_mmap)
} Region;

static Region *regions;
static int nregions, capregions;
static Mutex mem_lock;

static int bit(const uint8_t *m, size_t i) { return m[i >> 3] >> (i & 7) & 1; }
static void setbit(uint8_t *m, size_t i, int v) { if (v) m[i >> 3] |= 1 << (i & 7); else m[i >> 3] &= ~(1 << (i & 7)); }

static Region *find(uintptr_t a, uintptr_t len) {
    for (int i = 0; i < nregions; i++)
        if (a >= regions[i].base && a + len <= regions[i].base + regions[i].size) return &regions[i];
    return NULL;
}

static Region *add_region(uintptr_t base, uintptr_t size) {
    if (nregions == capregions) {
        capregions = capregions ? capregions * 2 : 64;
        regions = realloc(regions, capregions * sizeof *regions);
    }
    Region *r = &regions[nregions++];
    size_t pages = size / PG, bytes = (pages + 7) / 8;
    r->base = base; r->size = size; r->backing = NULL; r->lazy = 0; r->maps = NULL; r->nmaps = r->capmaps = 0;
    r->live = malloc(bytes); r->clean = calloc(1, bytes);
    memset(r->live, 0xff, bytes);
    return r;
}

// Page protection on heap-backed regions (svcSetMemoryPermission) is off: every protected page splits a kernel memory
// block, and once the process runs out of them (0xce01, after a few minutes of play) a page can't be made accessible
// again. Everything stays read-write; set enforce_none to make PROT_NONE fault like on Linux while debugging overflows.
// (box64 write-protects pages holding translated x86 code and expects SIGSEGV, which can't be delivered here anyway.)
static int enforce_none = 0;
static void set_perm(Region *r, uintptr_t a, uintptr_t len, int prot) {
    if (!enforce_none || r->backing || r->lazy || !len) return;   // aliased mappings keep RW
    u32 perm = prot ? Perm_Rw : Perm_None;
    // the kernel only changes a range whose current attributes are uniform: go block by block
    for (uintptr_t p = a, end = a + len; p < end;) {
        MemoryInfo mi; u32 pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, p))) break;
        uintptr_t stop = mi.addr + mi.size < end ? mi.addr + mi.size : end;
        if (mi.perm != perm) {
            Result rc = svcSetMemoryPermission((void *)p, stop - p, perm);
            if (R_FAILED(rc))
                lx_logf("mem: svcSetMemoryPermission(%#lx, %#lx, %u) %#x; mem type %u attr %u perm %u\n", p, stop - p, perm, rc, mi.type, mi.attr, mi.perm);
        }
        p = stop;
    }
}

// Lazy reservations: the GC's heap reservation gets address space only, and heap memory just for the granules actually
// committed, aliased in with svcMapMemory. That only works in the stack region, whose largest free hole (~1 GiB, it's
// fragmented by thread stacks and hbloader) bounds the Java heap. Code memory could go anywhere but hits a kernel
// resource limit after ~144 MiB. Backing the whole reservation up front cost more than half of the console's memory.
#define GRANULE (2ul << 20)
static void lazy_commit(Region *r, uintptr_t a, uintptr_t len) {
    uintptr_t g0 = (a - r->base) / GRANULE, g1 = (a + len - r->base + GRANULE - 1) / GRANULE;
    for (uintptr_t g = g0; g < g1; g++) {
        uintptr_t dst = r->base + g * GRANULE, sz = GRANULE;
        if (dst + sz > r->base + r->size) sz = r->base + r->size - dst;
        if (bit(r->live, (dst - r->base) / PG)) continue;   // granule already backed (live bits are per granule here)
        void *src = memalign(PG, sz);
        if (!src) { lx_logf("mem: lazy commit out of memory (%#lx)\n", sz); return; }
        memset(src, 0, sz);
        Result rc = svcMapMemory((void *)dst, src, sz);
        if (R_FAILED(rc)) { lx_logf("mem: lazy map of %#lx +%#lx failed %#x\n", dst, sz, rc); free(src); return; }
        if (r->nmaps == r->capmaps) { r->capmaps = r->capmaps ? r->capmaps * 2 : 256; r->maps = realloc(r->maps, r->capmaps * sizeof *r->maps); }
        r->maps[r->nmaps++] = (struct LazyMap){ dst, src, 0 };
        for (uintptr_t p = dst; p < dst + sz; p += PG) setbit(r->live, (p - r->base) / PG, 1);
    }
}
static void lazy_release(Region *r, uintptr_t a, uintptr_t len) {   // granules entirely inside [a, a+len)
    for (int i = 0; i < r->nmaps; i++) {
        uintptr_t dst = r->maps[i].dst, sz = dst + GRANULE > r->base + r->size ? r->base + r->size - dst : GRANULE;
        if (dst < a || dst + sz > a + len) continue;
        svcUnmapMemory((void *)dst, r->maps[i].src, sz);
        free(r->maps[i].src);
        for (uintptr_t p = dst; p < dst + sz; p += PG) setbit(r->live, (p - r->base) / PG, 0);
        r->maps[i--] = r->maps[--r->nmaps];
    }
}
static void lazy_zero(Region *r, uintptr_t a, uintptr_t len) {   // zero whatever of [a, a+len) is backed
    for (uintptr_t p = a & ~(PG - 1); p < a + len; p += PG)
        if (bit(r->live, (p - r->base) / PG)) memset((void *)p, 0, PG);
}

static int quarantine = 0;   // debug switch: keep unmapped regions inaccessible forever so stale writers fault
static void drop_region(Region *r) {
    if (r->lazy) {
        lazy_release(r, r->base, r->size);
        free(r->maps); free(r->live); free(r->clean);
        *r = regions[--nregions];
        return;
    }
    if (quarantine && !r->backing) {
        set_perm(r, r->base, r->size, 0);
        lx_logf("mem: quarantined %#lx+%#lx\n", r->base, r->size);
    } else {
        set_perm(r, r->base, r->size + (r->backing ? 0 : PG), 3);   // give the block (and its guard page) back writable
        if (r->backing) { svcUnmapMemory((void *)r->base, r->backing, r->size); free(r->backing); }
        else if (lx_arena_owns(r->base)) lx_arena_free(r->base, r->size - r->phantom);
        else free((void *)r->base);
    }
    free(r->live); free(r->clean);
    *r = regions[--nregions];
}

// make [a, a+len) accessible and zero where not known clean
static void commit(Region *r, uintptr_t a, uintptr_t len, int zero_all, int prot) {
    set_perm(r, a, len, 3);
    for (uintptr_t p = a; p < a + len; p += PG) {
        size_t i = (p - r->base) / PG;
        setbit(r->live, i, 1);
        if (zero_all || !bit(r->clean, i)) { memset((void *)p, 0, PG); setbit(r->clean, i, 1); }
    }
    if (prot != 3) set_perm(r, a, len, prot);
}

long lx_fd_pread(int fd, void *buf, size_t n, int64_t off);

// Low addresses (MAP_32BIT, or hints/MAP_FIXED below 4 GiB - box64 keeps its bridges there): Horizon lets us alias heap
// pages anywhere free in the stack region, which is 128 MiB..2 GiB, with svcMapMemory.
static uintptr_t stack_lo, stack_hi;
static int low_free(uintptr_t a, uintptr_t len) {
    if (a < stack_lo || a + len > stack_hi || a + len < a) return 0;
    for (int i = 0; i < nregions; i++)
        if (a < regions[i].base + regions[i].size && regions[i].base < a + len) return 0;
    MemoryInfo mi; u32 pi;
    for (uintptr_t p = a; p < a + len; p = mi.addr + mi.size) {
        if (R_FAILED(svcQueryMemory(&mi, &pi, p)) || mi.type != MemType_Unmapped) return 0;
    }
    return 1;
}
static long map_low(uintptr_t a, uintptr_t len) {
    void *src = memalign(PG, len);
    if (!src) return -LX_ENOMEM;
    memset(src, 0, len);
    Result rc = svcMapMemory((void *)a, src, len);
    if (R_FAILED(rc)) { free(src); lx_logf("mmap: svcMapMemory(%#lx, %#lx) %#x\n", a, len, rc); return -LX_ENOMEM; }
    Region *r = add_region(a, len);
    r->backing = src;
    memset(r->clean, 0xff, (len / PG + 7) / 8);
    return a;
}
static long mmap_low(uintptr_t hint, uintptr_t len) {
    if (hint && low_free(hint, len)) return map_low(hint, len);
    for (uintptr_t a = stack_lo + 0x10000000; a + len <= stack_hi; a += 0x100000)   // first fit, 1 MiB steps
        if (low_free(a, len)) return map_low(a, len);
    return -LX_ENOMEM;
}

// /proc/self/maps as box64 needs it: only the free part of the stack region is shown as available
size_t lx_proc_maps(char *buf, size_t cap) {
    size_t n = 0;
    mutexLock(&mem_lock);
    n += snprintf(buf + n, cap - n, "00000000-%08lx ---p 00000000 00:00 0\n", stack_lo);
    for (int i = 0; i < nregions && n < cap - 128; i++)
        if (regions[i].backing || regions[i].lazy)
            n += snprintf(buf + n, cap - n, "%08lx-%08lx rw-p 00000000 00:00 0\n", regions[i].base, regions[i].base + regions[i].size);
    n += snprintf(buf + n, cap - n, "%08lx-8000000000 ---p 00000000 00:00 0\n", stack_hi);
    mutexUnlock(&mem_lock);
    return n;
}

// ponytail: fixed split of the ~2.9 GiB heap, Java side 1.9 GiB (heap -Xmx and everything else it maps) and the rest
// for Mesa and libnx; mappings past the arena fall back to the shared heap. Tune with -Xmx if either side runs short.
#define ARENA_SIZE (1920ul << 20)

static long do_mmap(uintptr_t addr, uintptr_t len, int prot, int flags, int fd, int64_t off) {
    len = (len + PG - 1) & ~(PG - 1);
    if (!len) return -LX_EINVAL;
    if (!stack_lo) {
        stack_lo = 0; svcGetInfo((u64 *)&stack_lo, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
        u64 sz = 0; svcGetInfo(&sz, InfoType_StackRegionSize, CUR_PROCESS_HANDLE, 0); stack_hi = stack_lo + sz;
        if (!lx_arena_init(ARENA_SIZE)) lx_logf("mem: no room for the %lu MiB arena\n", ARENA_SIZE >> 20);
    }
    // MAP_32BIT, or a hint/fixed address inside the stack region (box64 retries its 32-bit picks with MAP_FIXED)
    int low = (flags & 0x40) || (addr && addr >= stack_lo && addr < stack_hi);
    if (flags & (LX_MAP_FIXED | LX_MAP_FIXED_NOREPLACE)) {
        // inside something we handed out before (SVM commits into its heap reservation this way), or a free low range
        Region *r = find(addr, len);
        if (!r) {
            if (!low || !low_free(addr, len)) return -LX_EEXIST;
            long a = map_low(addr, len);
            if (a > 0 && fd >= 0 && lx_fd_pread(fd, (void *)a, len, off) < 0) return -LX_EIO;
            return a;
        }
        if (r->lazy) {
            if (prot == LX_PROT_NONE) lazy_release(r, addr, len);   // the GC uncommitting
            else lazy_commit(r, addr, len);
            lazy_zero(r, addr, len);
            if (fd >= 0 && lx_fd_pread(fd, (void *)addr, len, off) < 0) return -LX_EIO;
            return addr;
        }
        commit(r, addr, len, 1, 3);
        if (fd >= 0 && lx_fd_pread(fd, (void *)addr, len, off) < 0) return -LX_EIO;
        if (prot != 3) set_perm(r, addr, len, prot);
        return addr;
    }
    if (!addr && prot == LX_PROT_NONE && fd < 0 && len >= (64ul << 20)) {   // a heap reservation: make it lazy
        void *va = NULL;
        for (uintptr_t a = (stack_lo + GRANULE - 1) & ~(GRANULE - 1); a + len <= stack_hi && !va; a += GRANULE)
            if (low_free(a, len)) va = (void *)a;
        if (va) {
            Region *r = add_region((uintptr_t)va, len);
            r->lazy = 1;
            memset(r->live, 0, (len / PG + 7) / 8);
            lx_logf("mem: lazy reservation %p +%lu MiB\n", va, (unsigned long)(len >> 20));
            return (long)va;
        }
        uintptr_t best = 0; MemoryInfo mi; u32 pi;
        for (uintptr_t p = stack_lo; p < stack_hi; p = mi.addr + mi.size) {
            if (R_FAILED(svcQueryMemory(&mi, &pi, p))) break;
            uintptr_t lo = mi.addr > stack_lo ? mi.addr : stack_lo, hi = mi.addr + mi.size < stack_hi ? mi.addr + mi.size : stack_hi;
            if (mi.type == MemType_Unmapped && hi - lo > best) best = hi - lo;
        }
        lx_logf("mem: no room for a lazy %lu MiB reservation (largest stack-region hole %lu MiB: lower -XX:ReservedAddressSpaceSize), backing it fully\n",
                (unsigned long)(len >> 20), (unsigned long)(best >> 20));
    }
    if (low) {
        long a = mmap_low(addr, len);
        if (a > 0 && fd >= 0 && lx_fd_pread(fd, (void *)a, len, off) < 0) return -LX_EIO;
        if (a > 0 || (flags & 0x40)) return a;
    }
    // Aligned to half the size (power of two, up to 2 MiB): the GC maps 2x the chunk it wants and unmaps around a
    // chunk-aligned window, so starting chunk-aligned the window is at the base and only the tail goes (do_munmap hands
    // it back). Aligning to the whole size instead would strand every trimmed tail as a hole no later request fits.
    uintptr_t align = PG;
    while (align < (2ul << 20) && align * 4 <= len) align *= 2;
    void *p = len >= (256ul << 10) ? lx_arena_alloc(len, align) : NULL;   // small maps (stacks) would only splinter it
    // The GC reserves a chunk by mapping twice its size and unmapping all but the aligned first half right away. Freed
    // chunks leave holes of exactly one chunk, which no such request fits, so with the arena full of them it ran dry
    // with 200 MiB free. Then hand out just the half that will be kept; the tail is never touched before the unmap.
    uintptr_t phantom = 0;
    if (!p && len == 2 * align && align >= (256ul << 10) && prot == LX_PROT_NONE && fd < 0 && (p = lx_arena_alloc(align, align)))
        phantom = len - align;
    if (!p) p = memalign(align, len + PG);   // + a guard page: an overrun faults instead of corrupting the next heap block
    if (!p && nouveau_bo_cache_trim) {   // out of memory: the GPU buffer cache is the one thing we can give back
        nouveau_bo_cache_trim();
        p = memalign(align, len + PG);
    }
    if (!p) {
        static int reported;
        lx_logf("mmap: out of memory for %#lx bytes\n", len);
        if (!reported++) {
            uint64_t mapped = 0, live = 0, total = 0, used = 0;
            for (int i = 0; i < nregions; i++) {
                mapped += regions[i].size;
                for (size_t k = 0; k < regions[i].size / PG; k++) live += bit(regions[i].live, k) ? PG : 0;
            }
            struct mallinfo mi = mallinfo();
            svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
            svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
            lx_logf("mem: %d regions, %lu MiB held (%lu MiB still mapped); heap in use %lu MiB of arena %lu MiB; process %lu/%lu MiB\n",
                    nregions, (unsigned long)(mapped >> 20), (unsigned long)(live >> 20), (unsigned long)(mi.uordblks >> 20),
                    (unsigned long)(mi.arena >> 20), (unsigned long)(used >> 20), (unsigned long)(total >> 20));
            // by size: count and MiB per power-of-four bucket from 64 KiB, and how much sits in the arena
            uint64_t bn[8] = {0}, bsz[8] = {0}, in_arena = 0, low = 0;
            uintptr_t shown[8] = {0};
            for (int i = 0; i < nregions; i++) {
                int b = 0;
                for (uintptr_t s = regions[i].size; b < 7 && s > (64ul << 10); s >>= 2) b++;
                bn[b]++; bsz[b] += regions[i].size;
                if (lx_arena_owns(regions[i].base)) in_arena += regions[i].size;
                if (regions[i].backing) low += regions[i].size;
            }
            for (int b = 0; b < 8; b++)
                if (bn[b]) lx_logf("mem:   <= %lu KiB: %lu regions, %lu MiB\n", (64ul << (2 * b)), (unsigned long)bn[b], (unsigned long)(bsz[b] >> 20));
            lx_logf("mem:   %lu MiB in the arena, %lu MiB low (box64)\n", (unsigned long)(in_arena >> 20), (unsigned long)(low >> 20));
            // the most common exact sizes (GC heap chunks are 1 MiB; musl malloc maps every block over ~128 KiB)
            for (int top = 0; top < 8; top++) {
                uintptr_t best = 0; int bestn = 0;
                for (int i = 0; i < nregions; i++) {
                    uintptr_t s = regions[i].size; int n = 0, done = 0;
                    for (int j = 0; j < i && !done; j++) done = regions[j].size == s;
                    if (done) continue;
                    for (int j = i; j < nregions; j++) n += regions[j].size == s;
                    if (n > bestn) {
                        int seen = 0;
                        for (int t = 0; t < top; t++) seen |= shown[t] == s;
                        if (!seen) { best = s; bestn = n; }
                    }
                }
                if (!bestn) break;
                shown[top] = best;
                lx_logf("mem:   %d x %lu KiB = %lu MiB\n", bestn, (unsigned long)(best >> 10), (unsigned long)(((uint64_t)bestn * best) >> 20));
            }
            for (int i = 0; i < nregions; i++)
                if (regions[i].size >= (16u << 20)) lx_logf("mem:   %#lx +%lu MiB%s\n", regions[i].base, (unsigned long)(regions[i].size >> 20), regions[i].backing ? " (low)" : "");
        }
        return -LX_ENOMEM;
    }
    Region *r = add_region((uintptr_t)p, len);
    r->phantom = phantom;
    if (!phantom) set_perm(r, (uintptr_t)p + len, PG, 0);
    if (prot != LX_PROT_NONE || fd >= 0) commit(r, (uintptr_t)p, len, 1, 3);
    if (fd >= 0) {
        long n = lx_fd_pread(fd, p, len, off);  // ponytail: MAP_SHARED writes are never written back; fine for read-only use
        if (n < 0) { drop_region(r); return n; }
    }
    if (prot != 3) set_perm(r, (uintptr_t)p, len, prot);   // incl. PROT_NONE reservations
    return (long)p;
}

static long do_munmap(uintptr_t a, uintptr_t len) {
    len = (len + PG - 1) & ~(PG - 1);
    Region *r = NULL;
    for (int i = 0; i < nregions && !r; i++)   // a phantom tail overlaps whoever really owns that memory: it goes first
        if (regions[i].phantom && a >= regions[i].base + regions[i].size - regions[i].phantom && a + len == regions[i].base + regions[i].size)
            r = &regions[i];
    if (!r) r = find(a, len);
    if (!r) return 0;  // Linux allows unmapping nothing
    if (r->lazy) {
        if (a == r->base && len == r->size) drop_region(r);
        else lazy_release(r, a, len);
        return 0;
    }
    size_t live = 0;
    if (!r->backing && a > r->base && a + len == r->base + r->size) {   // tail trim: give the tail back
        if (lx_arena_owns(r->base)) {
            uintptr_t own_end = r->base + r->size - r->phantom;
            if (a < own_end) lx_arena_free(a, own_end - a);
            r->size = a - r->base; r->phantom = 0;
            return 0;
        }
        void *q = realloc((void *)r->base, a - r->base + PG);
        if (q != (void *)r->base) lx_fatal("mem: in-place shrink of %#lx moved it to %p\n", r->base, q);
        r->size = a - r->base;
        return 0;
    }
    for (uintptr_t p = a; p < a + len; p += PG) setbit(r->live, (p - r->base) / PG, 0);
    set_perm(r, a, len, 0);   // touching unmapped pages faults, as on Linux
    for (size_t i = 0; i < r->size / PG; i++) live += bit(r->live, i);
    // ponytail: unmapped head/interior pages stay allocated until the whole region goes (tails are given back above)
    if (!live) drop_region(r);
    return 0;
}

static int nlog;
long lx_sys_mem(long n, long a, long b, long c, long d, long e, long f, int *handled) {
    long ret = 0;
    if ((n == LX_mmap || n == LX_munmap || n == LX_mremap) && nlog < 400) {   // debug: first mmap calls
        nlog++;
        lx_logf("%s(%#lx, %#lx, %lx, %lx, %ld)\n", n == LX_mmap ? "mmap" : n == LX_munmap ? "munmap" : "mremap", a, b, c, d, e);
    }
    *handled = 1;
    mutexLock(&mem_lock);
    switch (n) {
    case LX_mmap: ret = do_mmap(a, b, c, d, (int)e, f); break;
    case LX_munmap: ret = do_munmap(a, b); break;
    case LX_mprotect: {
        Region *r = find(a, (b + PG - 1) & ~(PG - 1));
        uintptr_t pa = a & ~(PG - 1), pl = (a + b + PG - 1 - pa) & ~(PG - 1);
        if (r && r->lazy) { if (c != LX_PROT_NONE) lazy_commit(r, pa, pl); }
        else if (r && c != LX_PROT_NONE) commit(r, pa, pl, 0, c);
        else if (r) set_perm(r, pa, pl, 0);
        break;
    }
    case LX_madvise:
        if (c == LX_MADV_DONTNEED) {
            Region *r = find(a, b);
            if (r && r->lazy) lazy_zero(r, a, b);
            else if (r) { set_perm(r, a & ~(PG - 1), (b + PG - 1) & ~(PG - 1), 3); memset((void *)a, 0, b); }
        }
        break;
    case LX_mremap:
        if ((uintptr_t)a >= lx_main_stack_lo && (uintptr_t)a < lx_main_stack_hi) { ret = -LX_ENOMEM; break; }  // musl's main-stack probe
        if (!find(a, b) || (uintptr_t)a < lx_main_stack_lo + PG * 0 && (uintptr_t)a + 2 * PG > lx_main_stack_lo) { ret = -LX_EFAULT; break; }
        if (!(d & LX_MREMAP_MAYMOVE)) { ret = -LX_ENOMEM; break; }
        ret = do_mmap(0, c, 3, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, -1, 0);
        if (ret > 0) { memcpy((void *)ret, (void *)a, b < (uintptr_t)c ? b : c); do_munmap(a, b); }
        break;
    case LX_brk: ret = -LX_ENOMEM; break;  // musl falls back to mmap
    case LX_msync: case LX_mlock: case LX_membarrier: break;
    case LX_mincore: ret = -LX_ENOSYS; break;
    default: *handled = 0;
    }
    mutexUnlock(&mem_lock);
    return ret;
}
