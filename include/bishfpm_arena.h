#ifndef BISHFPM_ARENA_H
#define BISHFPM_ARENA_H
#include <stddef.h>

#ifdef __cplusplus
#define BISHFPM_ALIGNOF alignof
extern "C" {
#else
#define BISHFPM_ALIGNOF _Alignof
#endif

typedef struct bishfpm_arena bishfpm_arena_t;

/* Both constructors return owned wrappers, released with arena_free.
 * new owns its buffer; new_buffer borrows the buffer until arena_free.
 * A borrowed buffer must outlive the arena and all allocations made in it. */
bishfpm_arena_t *bishfpm_arena_new(size_t size);
bishfpm_arena_t *bishfpm_arena_new_buffer(void *buffer, size_t size);
void bishfpm_arena_free(bishfpm_arena_t *a);
/* Reset invalidates prior allocations without freeing the buffer. */
void bishfpm_arena_reset(bishfpm_arena_t *a);
size_t bishfpm_arena_used(const bishfpm_arena_t *a);
size_t bishfpm_arena_capacity(const bishfpm_arena_t *a);
void *bishfpm_alloc_uninit(bishfpm_arena_t *a, ptrdiff_t size, ptrdiff_t align,
                           ptrdiff_t count);
void *bishfpm_alloc(bishfpm_arena_t *a, ptrdiff_t size, ptrdiff_t align,
                    ptrdiff_t count);
char *bishfpm_arena_strdup(bishfpm_arena_t *a, const char *s);

#define bishfpm_alloc_array(a, type, count)                                    \
  (type *)bishfpm_alloc((a), sizeof(type), BISHFPM_ALIGNOF(type),              \
                        (ptrdiff_t)(count))
#ifdef __cplusplus
}
#endif
#endif
