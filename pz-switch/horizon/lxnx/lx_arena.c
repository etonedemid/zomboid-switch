// A contiguous arena for the Java side's anonymous mappings, taken from the libnx heap once at startup and handed out
// page by page. Mixed into the newlib heap, the GC's aligned 512 KiB-4 MiB chunks and Mesa's textures fragmented it
// until aligned requests failed with a gigabyte still free; on their own they pack fine.
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#define APG 0x1000ul
static uintptr_t base;
static size_t npages, hint;
static uint64_t *used;   // one bit per page

static int busy(size_t i) { return used[i / 64] >> (i % 64) & 1; }
static void mark(size_t i, size_t n, int v) {
    for (size_t k = i; k < i + n; k++) {
        if (v) used[k / 64] |= 1ull << (k % 64);
        else used[k / 64] &= ~(1ull << (k % 64));
    }
}

int lx_arena_init(size_t size) {
    base = (uintptr_t)memalign(2ul << 20, size);
    if (!base) return 0;
    npages = size / APG; hint = 0;
    used = calloc((npages + 63) / 64, 8);
    return 1;
}

int lx_arena_owns(uintptr_t p) { return base && p >= base && p < base + npages * APG; }

// first fit at a multiple of align (a power of two, <= 2 MiB, so relative to base works), starting at a rolling hint
void *lx_arena_alloc(size_t len, size_t align) {
    if (!base) return NULL;
    size_t n = (len + APG - 1) / APG, step = align > APG ? align / APG : 1;
    for (int pass = 0; pass < 2; pass++) {
        size_t i = pass ? 0 : (hint + step - 1) / step * step, end = pass ? hint + n : npages;
        if (end > npages) end = npages;
        while (i + n <= end) {
            size_t k = 0;
            while (k < n && !busy(i + k)) k++;
            if (k == n) { mark(i, n, 1); hint = i + n; return (void *)(base + i * APG); }
            i = (i + k + 1 + step - 1) / step * step;   // past the busy page, back on the alignment grid
        }
    }
    return NULL;
}

void lx_arena_free(uintptr_t p, size_t len) {
    size_t i = (p - base) / APG, n = (len + APG - 1) / APG;
    mark(i, n, 0);
    if (i < hint) hint = i;
}

#ifdef LX_ARENA_TEST   // cc -DLX_ARENA_TEST lx_arena.c && ./a.out
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(lx_arena_init(16ul << 20));
    void *a = lx_arena_alloc(0x100000, 0x100000), *b = lx_arena_alloc(0x80000, 0x80000);
    assert(a && b && (uintptr_t)a % 0x100000 == 0 && (uintptr_t)b % 0x80000 == 0 && b != a);
    lx_arena_free((uintptr_t)a + 0x80000, 0x80000);   // GC-style tail trim
    void *c = lx_arena_alloc(0x80000, 0x80000);
    assert(c == (char *)a + 0x80000);                 // the trimmed tail is reused
    void *big = lx_arena_alloc(0x401000, 2ul << 20);
    assert(big && (uintptr_t)big % (2ul << 20) == 0);
    int n = 0;
    while (lx_arena_alloc(0x100000, 0x100000)) n++;   // fills up (then the alignment holes), then says so
    while (lx_arena_alloc(APG, APG)) n++;
    assert(n > 0 && !lx_arena_alloc(APG, APG));
    lx_arena_free((uintptr_t)b, 0x80000);
    assert(lx_arena_alloc(0x80000, 0x80000) == b);     // and refills a hole behind the hint
    assert(lx_arena_owns((uintptr_t)b) && !lx_arena_owns((uintptr_t)&n));
    // the GC's pattern: map 1 MiB at 512 KiB alignment, keep the first 512 KiB; chunks must pack back to back
    assert(lx_arena_init(64ul << 20));
    uintptr_t first = 0, prev = 0;
    for (int i = 0; i < 100; i++) {
        uintptr_t c = (uintptr_t)lx_arena_alloc(0x100000, 0x80000);
        assert(c && c % 0x80000 == 0);
        lx_arena_free(c + 0x80000, 0x80000);
        if (!first) first = c; else assert(c == prev + 0x80000);
        prev = c;
    }
    puts("lx_arena ok");
    return 0;
}
#endif
