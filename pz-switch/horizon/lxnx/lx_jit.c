// JIT arena for box64's dynarec: Horizon code memory maps the same pages twice, RW for writing generated code and RX
// for running it (no page is ever writable and executable at once).
#include "lx.h"

void *lx_jit_reserve(size_t size, intptr_t *delta) {
    static Jit j;
    Result rc = jitCreate(&j, size);
    if (R_FAILED(rc)) { lx_logf("jit: jitCreate(%#zx) failed %#x\n", size, rc); return NULL; }
    jitTransitionToExecutable(&j);   // no-op for code memory; keeps the older permission-flipping type usable
    *delta = (intptr_t)jitGetRxAddr(&j) - (intptr_t)jitGetRwAddr(&j);
    lx_logf("jit: %zu MiB arena, rw %p rx %p (type %d)\n", size >> 20, jitGetRwAddr(&j), jitGetRxAddr(&j), j.type);
    return jitGetRwAddr(&j);
}
