/* obstack is a glibc extension; box64 wraps it for x86 callers. Declarations only, implementation absent. */
#pragma once
#include <stddef.h>
struct _obstack_chunk { char *limit; struct _obstack_chunk *prev; char contents[4]; };
struct obstack { size_t chunk_size; struct _obstack_chunk *chunk; char *object_base, *next_free, *chunk_limit;
  union { size_t i; void *p; } temp; size_t alignment_mask; void *(*chunkfun)(void *, size_t); void (*freefun)(void *, void *);
  void *extra_arg; unsigned use_extra_arg:1, maybe_empty_object:1, alloc_failed:1; };
extern void (*obstack_alloc_failed_handler)(void);
int _obstack_begin(struct obstack *, size_t, size_t, void *(*)(size_t), void (*)(void *));
int _obstack_begin_1(struct obstack *, size_t, size_t, void *(*)(void *, size_t), void (*)(void *, void *), void *);
void _obstack_newchunk(struct obstack *, size_t);
void _obstack_free(struct obstack *, void *);
#define obstack_free _obstack_free
