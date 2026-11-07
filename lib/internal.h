#ifndef BISHFPM_INTERNAL_H
#define BISHFPM_INTERNAL_H

#include "bishfpm_hash.h"
#include "bishfpm_itemset.h"
#include "state.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline uint64_t effective_time(uint64_t requested, uint64_t watermark) {
  return requested > watermark ? requested : watermark;
}

static inline double decay_since(double count, uint64_t then, uint64_t now,
                                 double half) {
  return now <= then ? count : bishfpm_rate_decay(count, now - then, half);
}

static inline size_t next_pow2(size_t v) {
  if (v > (SIZE_MAX >> 1))
    return 0;
  size_t p = 16;
  while (p < v)
    p <<= 1;
  return p;
}

static inline bool size_add(size_t a, size_t b, size_t *out) {
  if (b > SIZE_MAX - a)
    return false;
  *out = a + b;
  return true;
}

static inline bool size_mul(size_t a, size_t b, size_t *out) {
  if (a && b > SIZE_MAX / a)
    return false;
  *out = a * b;
  return true;
}

/* NULL budgets preserve the standalone and conservative allocation paths.
 * No allocation headers: charges are exactly the requested payload sizes. */
struct bishfpm_memory_budget {
  size_t current, peak, limit;
  uint64_t denials, event_rejections;
};

static inline bool budget_allows(bishfpm_memory_budget_t *b, size_t bytes) {
  if (bytes > b->limit - b->current) {
    b->denials++;
    return false;
  }
  return true;
}
static inline void budget_charge(bishfpm_memory_budget_t *b, size_t bytes) {
  b->current += bytes;
  if (b->current > b->peak)
    b->peak = b->current;
}
static inline void *budget_calloc(bishfpm_memory_budget_t *b, size_t n,
                                  size_t size) {
  size_t bytes;
  if (!size_mul(n, size, &bytes)) {
    b->denials++;
    return NULL;
  }
  if (!budget_allows(b, bytes))
    return NULL;
  void *p = calloc(n, size);
  if (p)
    budget_charge(b, bytes);
  return p;
}
static inline void *budget_malloc(bishfpm_memory_budget_t *b, size_t bytes) {
  if (!budget_allows(b, bytes))
    return NULL;
  void *p = malloc(bytes);
  if (p)
    budget_charge(b, bytes);
  return p;
}
static inline char *budget_strdup(bishfpm_memory_budget_t *b, const char *s) {
  size_t bytes = strlen(s) + 1;
  if (!budget_allows(b, bytes))
    return NULL;
  char *p = strdup(s);
  if (p)
    budget_charge(b, bytes);
  return p;
}
static inline void memory_free(bishfpm_memory_budget_t *b, void *p,
                               size_t bytes) {
  if (!p)
    return;
  if (b) {
    assert(b->current >= bytes);
    b->current -= bytes;
  }
  free(p);
}
#define memory_calloc(b, n, size)                                              \
  ((b) ? budget_calloc((b), (n), (size)) : calloc((n), (size)))
#define memory_malloc(b, bytes)                                                \
  ((b) ? budget_malloc((b), (bytes)) : malloc((bytes)))
#define memory_strdup(b, s) ((b) ? budget_strdup((b), (s)) : strdup((s)))

typedef struct {
  size_t slot_cap, heap_bytes, map_bytes, sketch_bytes, arena_bytes;
} topk_layout_t;

/* Include worst-case alignment padding for the store, arrays, and optional
 * sketch. */
static inline bool topk_layout_configured(uint32_t k, double half_life_ms,
                                          uint32_t width,
                                          topk_layout_t *layout) {
  size_t slots, bytes;
  if (!k || k > (1U << 28) || width > (1U << 24) || !isfinite(half_life_ms) ||
      half_life_ms <= 0 || !size_mul(k, 2, &slots) ||
      !(layout->slot_cap = next_pow2(slots)) ||
      !size_mul(k, sizeof(bishfpm_heap_entry_t), &layout->heap_bytes) ||
      !size_mul(layout->slot_cap, sizeof(bishfpm_slot_entry_t),
                &layout->map_bytes) ||
      !size_add(sizeof(bishfpm_topk_t), layout->heap_bytes, &bytes) ||
      !size_add(bytes, layout->map_bytes, &bytes) ||
      !size_mul(width, sizeof(bishfpm_sketch_cell_t), &layout->sketch_bytes) ||
      !size_add(bytes, layout->sketch_bytes, &bytes) ||
      !size_add(bytes,
                _Alignof(bishfpm_topk_t) - 1 + _Alignof(bishfpm_heap_entry_t) -
                    1 + _Alignof(bishfpm_slot_entry_t) - 1 +
                    (width ? _Alignof(bishfpm_sketch_cell_t) - 1 : 0),
                &bytes) ||
      bytes > PTRDIFF_MAX)
    return false;
  layout->arena_bytes = bytes;
  return true;
}

static inline bool topk_layout(uint32_t k, double half_life_ms,
                               topk_layout_t *layout) {
  return topk_layout_configured(k, half_life_ms, 0, layout);
}

/* Values, never pointers into a heap that can move during an update. */
typedef struct {
  bool present, was_present, replaced, accepted;
  double upper, lower, error;
  uint64_t time_ms;
  uint64_t hash; /* Cached text-pattern hash for valid string-store inputs. */
} topk_result_t;

/* Internal helpers are not part of the installed/shared-library interface. */
#if defined(__GNUC__) || defined(__clang__)
#define BISHFPM_PRIVATE __attribute__((visibility("hidden")))
#else
#define BISHFPM_PRIVATE
#endif
BISHFPM_PRIVATE bishfpm_arena_t *arena_new_budget(size_t,
                                                  bishfpm_memory_budget_t *);
BISHFPM_PRIVATE bishfpm_rate_table_t *
rate_table_new_budget(size_t, size_t, uint64_t, bishfpm_memory_budget_t *);
BISHFPM_PRIVATE topk_result_t topk_query_result(const bishfpm_topk_t *,
                                                const char *, uint64_t);
BISHFPM_PRIVATE topk_result_t topk_add_result(bishfpm_topk_t *, const char *,
                                              double, uint64_t);
/* Requires a validated nonempty pattern of length < BISHFPM_TOPK_MAX_STR,
 * a valid store, and finite nonnegative weight. */
BISHFPM_PRIVATE topk_result_t topk_add_sized(bishfpm_topk_t *, const char *,
                                             size_t, double, uint64_t);
BISHFPM_PRIVATE bishfpm_rate_entry_t *
rate_table_find(const bishfpm_rate_table_t *, const char *, uint64_t);
/* Distinguish capacity rejection from failed allocation without a second scan.
 */
typedef enum {
  RATE_PREPARED,
  RATE_LIMIT,
  RATE_ALLOCATION,
  RATE_INVALID
} rate_prepare_result_t;
BISHFPM_PRIVATE rate_prepare_result_t
rate_table_prepare_result(bishfpm_rate_table_t *, const char *const *, size_t);
#endif
