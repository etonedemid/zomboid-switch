// /game served read-only straight out of the uploaded tar parts (sdmc:/pz/xfer/game.tar.NN, as written by xfer.sh).
// Extracting ~47k files onto the SD card takes many hours (directory creation cost grows with directory size); the tar
// needs one index pass, cached in game.idx next to the parts. Paths not in the archive fall through to the SD card.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "lx.h"
#include "lx_tarfs.h"

#define XFER LX_ROOT "/xfer/game.tar."
#define IDX LX_ROOT "/xfer/game.idx"
#define MAXPARTS 16

static int parts[MAXPARTS], nparts;
static uint64_t partsz;              // size of every part but the last (split -b)
static Mutex io_lock;                // ponytail: one lock for all archive reads; per-part fds if it ever contends
static TarNode *root;
static TarNode **table;              // open-addressed hash on full path
static size_t tabcap;

static uint64_t hash(const char *s) { uint64_t h = 1469598103934665603ull; for (; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull; return h; }

static TarNode *find(const char *path) {
    if (!table) return NULL;
    for (size_t i = hash(path) & (tabcap - 1);; i = (i + 1) & (tabcap - 1)) {
        if (!table[i]) return NULL;
        if (!strcmp(table[i]->path, path)) return table[i];
    }
}

static void insert(TarNode *n) {
    size_t i = hash(n->path) & (tabcap - 1);
    while (table[i]) i = (i + 1) & (tabcap - 1);
    table[i] = n;
}

// node for path (relative, no leading slash), creating it and its parents as directories if needed
static TarNode *node(const char *path, int is_dir) {
    TarNode *n = find(path);
    if (n) { if (!is_dir) n->is_dir = 0; return n; }
    n = calloc(1, sizeof *n);
    n->path = strdup(path);
    const char *slash = strrchr(n->path, '/');
    n->name = slash ? slash + 1 : n->path;
    n->is_dir = is_dir;
    insert(n);
    TarNode *parent = root;
    if (slash) { char *pp = strndup(path, slash - n->path); parent = node(pp, 1); free(pp); }
    n->next = parent->child; parent->child = n;
    return n;
}

ssize_t lx_tarfs_pread(const TarNode *n, void *buf, size_t len, uint64_t off) {
    if (off >= n->size) return 0;
    if (len > n->size - off) len = n->size - off;
    uint64_t g = n->off + off;
    size_t done = 0;
    mutexLock(&io_lock);
    while (done < len) {
        int p = g / partsz;
        uint64_t po = g % partsz;
        size_t chunk = len - done;
        if (p < nparts - 1 && po + chunk > partsz) chunk = partsz - po;   // file straddles two parts
        if (p >= nparts || lseek(parts[p], po, SEEK_SET) < 0) break;
        ssize_t r = read(parts[p], (char *)buf + done, chunk);
        if (r <= 0) break;
        done += r; g += r;
    }
    mutexUnlock(&io_lock);
    return done;
}

static uint64_t octal(const char *s, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n && s[i]; i++) if (s[i] >= '0' && s[i] <= '7') v = v * 8 + (s[i] - '0');
    return v;
}

// raw read across parts at a global offset (index building)
static int raw(void *buf, size_t len, uint64_t g) {
    TarNode t = { .off = 0, .size = ~0ull };
    return lx_tarfs_pread(&t, buf, len, g) == (ssize_t)len;
}

static void build_index(FILE *out) {
    static char hdr[512], longname[4096];
    uint64_t g = 0, files = 0;
    longname[0] = 0;
    while (raw(hdr, 512, g) && hdr[0]) {
        uint64_t size = octal(hdr + 124, 12);
        char type = hdr[156];
        g += 512;
        if (type == 'L') {
            size_t n = size < sizeof longname - 1 ? size : sizeof longname - 1;
            raw(longname, n, g); longname[n] = 0;
            g += (size + 511) & ~511ull;
            continue;
        }
        char name[4200];
        if (longname[0]) { snprintf(name, sizeof name, "%s", longname); longname[0] = 0; }
        else snprintf(name, sizeof name, "%.155s%s%.100s", hdr + 345, hdr[345] ? "/" : "", hdr);
        size_t l = strlen(name);
        while (l && name[l - 1] == '/') name[--l] = 0;
        int is_dir = type == '5';
        if (l && (is_dir || type == '0' || type == 0)) {
            TarNode *n = node(name, is_dir);
            n->off = g; n->size = is_dir ? 0 : size;
            if (out) { uint16_t pl = l; uint8_t d = is_dir; fwrite(&g, 8, 1, out); fwrite(&n->size, 8, 1, out); fwrite(&d, 1, 1, out); fwrite(&pl, 2, 1, out); fwrite(name, 1, l, out); }
            files++;
        }
        g += (size + 511) & ~511ull;
    }
    lx_logf("tarfs: indexed %llu entries\n", (unsigned long long)files);
}

static int load_index(void) {
    FILE *f = fopen(IDX, "rb");
    if (!f) return 0;
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    uint64_t off, size; uint8_t d; uint16_t pl; char name[4200]; size_t n = 0;
    while (fread(&off, 8, 1, f) == 1 && fread(&size, 8, 1, f) == 1 && fread(&d, 1, 1, f) == 1 && fread(&pl, 2, 1, f) == 1
           && pl < sizeof name && fread(name, 1, pl, f) == pl) {
        name[pl] = 0;
        TarNode *t = node(name, d);
        t->off = off; t->size = size;
        n++;
    }
    fclose(f);
    lx_logf("tarfs: loaded index, %zu entries\n", n);
    return n > 0;
}

void lx_tarfs_init(void) {
    char name[64];
    for (nparts = 0; nparts < MAXPARTS; nparts++) {
        snprintf(name, sizeof name, XFER "%02d", nparts);
        if ((parts[nparts] = open(name, O_RDONLY)) < 0) break;
    }
    if (!nparts) return;
    struct stat st;
    fstat(parts[0], &st);
    partsz = st.st_size;
    tabcap = 1 << 17;   // > 2x the ~47k entries
    table = calloc(tabcap, sizeof *table);
    root = calloc(1, sizeof *root);
    root->path = ""; root->name = ""; root->is_dir = 1;
    if (!load_index()) {
        FILE *out = fopen(IDX ".tmp", "wb");
        if (out) setvbuf(out, NULL, _IOFBF, 1 << 20);
        build_index(out);
        if (out) { fclose(out); rename(IDX ".tmp", IDX); }
    }
}

// Linux path -> node, for paths under /game present in the archive
TarNode *lx_tarfs_lookup(const char *lp) {
    if (!root || strncmp(lp, "/game", 5) || (lp[5] && lp[5] != '/')) return NULL;
    if (!lp[5] || !lp[6]) return root;
    return find(lp + 6);
}
