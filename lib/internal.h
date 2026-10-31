#ifndef BISHFPM_INTERNAL_H
#define BISHFPM_INTERNAL_H

#include "bishfpm_hash.h"
#include "bishfpm_itemset.h"
#include "state.h"
#include <math.h>

static inline uint64_t effective_time(uint64_t requested, uint64_t watermark) {
  return requested > watermark ? requested : watermark;
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

typedef struct {
  size_t slot_cap, heap_bytes, map_bytes, arena_bytes;
} topk_layout_t;

/* Include worst-case alignment padding for all three arena allocations. */
static inline bool topk_layout(uint32_t k, double half_life_ms,
                               topk_layout_t *layout) {
  size_t slots, bytes;
  if (!k || k > (1U << 28) || !isfinite(half_life_ms) || half_life_ms <= 0 ||
      !size_mul(k, 2, &slots) || !(layout->slot_cap = next_pow2(slots)) ||
      !size_mul(k, sizeof(bishfpm_heap_entry_t), &layout->heap_bytes) ||
      !size_mul(layout->slot_cap, sizeof(bishfpm_slot_entry_t),
                &layout->map_bytes) ||
      !size_add(sizeof(bishfpm_topk_t), layout->heap_bytes, &bytes) ||
      !size_add(bytes, layout->map_bytes, &bytes) ||
      !size_add(bytes,
                _Alignof(bishfpm_topk_t) - 1 + _Alignof(bishfpm_heap_entry_t) -
                    1 + _Alignof(bishfpm_slot_entry_t) - 1,
                &bytes) ||
      bytes > PTRDIFF_MAX)
    return false;
  layout->arena_bytes = bytes;
  return true;
}

/* Values, never pointers into a heap that can move during an update. */
typedef struct {
  bool present, was_present, replaced;
  double upper, lower, error;
  uint64_t time_ms;
} topk_result_t;

/* Internal helpers are not part of the installed/shared-library interface. */
#if defined(__GNUC__) || defined(__clang__)
#define BISHFPM_PRIVATE __attribute__((visibility("hidden")))
#else
#define BISHFPM_PRIVATE
#endif
BISHFPM_PRIVATE topk_result_t topk_query_result(const bishfpm_topk_t *,
                                                const char *, uint64_t);
BISHFPM_PRIVATE topk_result_t topk_add_result(bishfpm_topk_t *, const char *,
                                              double, uint64_t);
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
