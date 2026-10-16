#pragma once
#include <stdint.h>
#include <sys/types.h>

typedef struct TarNode {
    char *path;              // relative to /game
    const char *name;        // last component
    int is_dir;
    uint64_t off, size;      // data offset in the concatenated parts
    struct TarNode *child, *next;
} TarNode;

void lx_tarfs_init(void);
TarNode *lx_tarfs_lookup(const char *linux_path);
ssize_t lx_tarfs_pread(const TarNode *n, void *buf, size_t len, uint64_t off);
