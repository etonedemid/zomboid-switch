/* minimal fts declarations (box64 only forwards these for x86 callers; no implementation on this platform) */
#pragma once
#include <sys/types.h>
#include <sys/stat.h>
typedef struct _ftsent { struct _ftsent *fts_cycle, *fts_parent, *fts_link; long fts_number; void *fts_pointer;
  char *fts_accpath, *fts_path; int fts_errno, fts_symfd; unsigned short fts_pathlen, fts_namelen; ino_t fts_ino; dev_t fts_dev;
  nlink_t fts_nlink; short fts_level, fts_info; unsigned short fts_flags, fts_instr; struct stat *fts_statp; char fts_name[1]; } FTSENT;
typedef struct { FTSENT *fts_cur, *fts_child, **fts_array; dev_t fts_dev; char *fts_path; int fts_rfd, fts_pathlen, fts_nitems;
  int (*fts_compar)(const void *, const void *); int fts_options; } FTS;
static inline FTS *fts_open(char *const *p, int o, int (*c)(const FTSENT **, const FTSENT **)) { (void)p; (void)o; (void)c; return 0; }
static inline FTSENT *fts_read(FTS *f) { (void)f; return 0; }
static inline FTSENT *fts_children(FTS *f, int o) { (void)f; (void)o; return 0; }
static inline int fts_set(FTS *f, FTSENT *e, int o) { (void)f; (void)e; (void)o; return -1; }
static inline int fts_close(FTS *f) { (void)f; return 0; }
