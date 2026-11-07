#ifndef BISHFPM_COMPACT_H
#define BISHFPM_COMPACT_H
#include "bishfpm.h"
#include "internal.h"

/* Resident interval at last_time_ms: [max(0, rate - error), rate].
 * Both rate and error decay with the same half-life. Heap order compares rate
 * at a common time; advancing time alone does not require reheapification.
 * New-resident error: SS inherits the victim upper count or omitted_bound in a
 * free slot; threshold inherits omitted_bound; FSS inherits its bucket after
 * eviction. All add weight to inherited error to obtain the upper count. */
typedef struct {
  uint64_t key;
  uint64_t hash;
  double rate, error;
  uint64_t last_time_ms;
} compact_entry_t;

/* zero-floor threshold counts need neither uncertainty nor heap ordering */
typedef struct {
  uint64_t key;
  double rate;
  uint64_t last_time_ms;
} compact_exact_entry_t;

typedef struct {
  uint32_t k, heap_size, sketch_width;
  bool exact;
  bishfpm_memory_budget_t *budget;
  bishfpm_pair_store_t kind;
  double half_life_ms;
  /* Maximum upper count removed by pruning, decayed to max_time_ms. SS and
   * threshold reuse it as the initial error when filling a free slot.
   * It does not include SS evictions. */
  union {
    double omitted_bound;
    /* exact mode: earliest timestamp of the conservative minimum below */
    uint64_t exact_min_time_ms;
  };
  /* Active only with a sketch: total input weight at max_time_ms, including
   * filtered misses. Cells have their own timestamps and decay lazily. */
  /* exact mode reuses this as a lower bound on raw resident counts; with
   * exact_min_time_ms it proves that no resident has decayed to zero */
  double sketch_total;
  /* Threshold mode forgets counts <= this floor and never evicts a stronger
   * resident. Other modes prune strictly below the requested cutoff. */
  double forget_below;
  /* Monotone watermark. Late input is discounted to it; queries never move it.
   * forgotten counts removals by pruning, excluding replacement evictions. */
  uint64_t max_time_ms, forgotten;
  size_t slot_map_cap;
  union {
    compact_entry_t *heap;
    compact_exact_entry_t *counts;
  };
  /* Zero is empty; otherwise the slot contains heap index + 1.
   * k <= 2^28. */
  uint32_t *slot_map;
  /* FSS: one bucket bound per absent-key history; resident hits skip buckets.
   * A new resident inherits sketch uncertainty, even after pruning. */
  bishfpm_sketch_cell_t *sketch;
} compact_t;

BISHFPM_PRIVATE size_t compact_bytes(uint32_t, uint32_t, bishfpm_pair_store_t);
BISHFPM_PRIVATE size_t compact_configured_bytes(uint32_t, uint32_t,
                                                bishfpm_pair_store_t, double);
BISHFPM_PRIVATE size_t compact_storage_bytes(const compact_t *);
BISHFPM_PRIVATE compact_t *compact_new(uint32_t, double, uint32_t,
                                       bishfpm_pair_store_t, double);
BISHFPM_PRIVATE compact_t *compact_new_budget(uint32_t, double, uint32_t,
                                              bishfpm_pair_store_t, double,
                                              bishfpm_memory_budget_t *);
BISHFPM_PRIVATE void compact_free(compact_t *);
/* heap entries cache the supplied hash; exact entries require compact_hash(key)
 */
BISHFPM_PRIVATE topk_result_t compact_add(compact_t *, uint64_t, uint64_t,
                                          double, uint64_t);
/* Absent-key bounds are returned only for threshold (omitted_bound) and FSS
 * (bucket history). A zero result in SS is not an absence guarantee.
 * Query decays the stored lower bound; add subtracts decayed error from upper.
 * Keep these evaluation orders: they can differ by floating-point rounding. */
BISHFPM_PRIVATE topk_result_t compact_query(const compact_t *, uint64_t,
                                            uint64_t, uint64_t);
BISHFPM_PRIVATE size_t compact_prune(compact_t *, uint64_t, double);
BISHFPM_PRIVATE size_t compact_prune_bounded(compact_t *, uint64_t, size_t);
/* Preflight complete pair transaction without mutation. Returns whether every
 * pair can be retained after safe forgetting; tells caller if pruning is
 * needed. IDs are distinct. Threshold indexing uses compact_hash(key). */
BISHFPM_PRIVATE bool compact_reserve(const compact_t *, const uint32_t *,
                                     size_t, uint64_t, bool *);
static inline uint64_t compact_key(uint32_t a, uint32_t b) {
  return a < b ? ((uint64_t)a << 32) | b : ((uint64_t)b << 32) | a;
}
static inline uint64_t compact_hash(uint64_t key) {
  return bishfpm_splitmix64(&key);
}
#endif
