#include "internal.h"
#include <stdlib.h>
#include <string.h>

/* Every public arena owns its wrapper; only buffer ownership varies. */
static bishfpm_arena_t *arena_wrap(void *buffer, size_t size, bool owned,
                                   bishfpm_memory_budget_t *budget) {
  if (!buffer || !size || size > PTRDIFF_MAX)
    return NULL;
  bishfpm_arena_t *a = memory_malloc(budget, sizeof(*a));
  if (!a)
    return NULL;
  a->base = a->beg = buffer;
  a->end = a->base + size;
  a->owned = owned;
  a->budget = budget;
  return a;
}

bishfpm_arena_t *bishfpm_arena_new(size_t size) {
  return arena_new_budget(size, NULL);
}

bishfpm_arena_t *arena_new_budget(size_t size,
                                  bishfpm_memory_budget_t *budget) {
  if (!size || size > PTRDIFF_MAX)
    return NULL;
  void *buffer = memory_malloc(budget, size);
  if (!buffer)
    return NULL;
  bishfpm_arena_t *a = arena_wrap(buffer, size, true, budget);
  if (!a)
    memory_free(budget, buffer, size);
  return a;
}

bishfpm_arena_t *bishfpm_arena_new_buffer(void *buffer, size_t size) {
  return arena_wrap(buffer, size, false, NULL);
}

void bishfpm_arena_free(bishfpm_arena_t *a) {
  if (!a)
    return;
  if (a->owned)
    memory_free(a->budget, a->base, (size_t)(a->end - a->base));
  memory_free(a->budget, a, sizeof(*a));
}

size_t bishfpm_arena_used(const bishfpm_arena_t *a) {
  return a && a->base ? (size_t)(a->beg - a->base) : 0;
}

size_t bishfpm_arena_capacity(const bishfpm_arena_t *a) {
  return a && a->base ? (size_t)(a->end - a->base) : 0;
}

/* reset arena to initial state */
void bishfpm_arena_reset(bishfpm_arena_t *a) {
  if (a) {
    a->beg = a->base;
  }
}

/* uninitialized allocation */
void *bishfpm_alloc_uninit(bishfpm_arena_t *a, ptrdiff_t size, ptrdiff_t align,
                           ptrdiff_t count) {
  if (!a || !a->beg || !a->end || size <= 0 || count < 0) {
    return NULL;
  }
  if (align < 1 || (align & (align - 1)) != 0)
    return NULL;
  ptrdiff_t padding = -(uintptr_t)a->beg & (align - 1);
  ptrdiff_t available = a->end - a->beg - padding;
  if (available < 0 || count > available / size) {
    return NULL;
  }
  void *p = a->beg + padding;
  a->beg += padding + count * size;
  return p;
}

/* zero-initialized allocation */
void *bishfpm_alloc(bishfpm_arena_t *a, ptrdiff_t size, ptrdiff_t align,
                    ptrdiff_t count) {
  void *p = bishfpm_alloc_uninit(a, size, align, count);
  return p ? memset(p, 0, (size_t)(count * size)) : NULL;
}

/* duplicate string into arena */
char *bishfpm_arena_strdup(bishfpm_arena_t *a, const char *s) {
  if (!s) {
    return NULL;
  }
  size_t len = strlen(s);
  if (len >= PTRDIFF_MAX)
    return NULL;
  char *p = (char *)bishfpm_alloc_uninit(a, 1, 1, (ptrdiff_t)(len + 1));
  if (p) {
    memcpy(p, s, len + 1);
  }
  return p;
}
