#ifndef BISHFPM_TOPK_H
#define BISHFPM_TOPK_H

#include "bishfpm_arena.h"
#include "bishfpm_limits.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returned value snapshot; editing it does not mutate store state. */
typedef struct {
  char pattern[BISHFPM_TOPK_MAX_STR];
  double rate;
  double error;
  uint64_t last_time_ms;
} bishfpm_topk_item_t;

typedef struct bishfpm_topk bishfpm_topk_t;

/* lifecycle */
bishfpm_topk_t *bishfpm_topk_new(uint32_t k, double half_life_ms);
bishfpm_topk_t *bishfpm_topk_new_arena(bishfpm_arena_t *a, uint32_t k,
                                       double half_life_ms);
/* Experimental FSS, with 1..2^24 sketch buckets. SS constructors above are
 * unchanged. Seed is fixed once positive input has been accepted, including
 * after pruning all residents. reset starts a new epoch and permits reseeding.
 */
bishfpm_topk_t *bishfpm_topk_new_fss(uint32_t k, double half_life_ms,
                                     uint32_t sketch_width, uint64_t seed);
bishfpm_topk_t *bishfpm_topk_new_fss_arena(bishfpm_arena_t *, uint32_t k,
                                           double half_life_ms,
                                           uint32_t sketch_width,
                                           uint64_t seed);
/* No-op for arena-backed stores; reset/free of their arena ends their lifetime.
 */
void bishfpm_topk_free(bishfpm_topk_t *topk);
void bishfpm_topk_reset(bishfpm_topk_t *topk);
/* Legacy setter leaves a nonempty FSS history unchanged on refusal. */
void bishfpm_topk_set_seed(bishfpm_topk_t *topk, uint64_t seed);
/* Returns 0 for NULL or an attempted FSS reseed after history has started.
 * Reapplying the current seed succeeds. SS can always rehash its residents. */
int bishfpm_topk_try_set_seed(bishfpm_topk_t *, uint64_t seed);

typedef struct {
  double lower, upper, error;
  int resident, known;
} bishfpm_topk_bounds_t;
/* Valid nonempty string queries return 1, including absent identities. FSS
 * absence has a certified bucket upper bound and lower zero; SS absence is
 * unknown. Invalid arguments return 0 without changing output. Queries do not
 * advance the watermark. Bounds cover the stream supplied to this store. */
int bishfpm_topk_bounds(const bishfpm_topk_t *, const char *, uint64_t now_ms,
                        bishfpm_topk_bounds_t *);

/* update item in top-k heap, returns estimated rate */
double bishfpm_topk_update(bishfpm_topk_t *topk, const char *pattern,
                           uint64_t now_ms);

/* Add a finite nonnegative weight, discounted to the store watermark.
 * Returns the upper estimate. Invalid input or count overflow returns zero
 * without changing the store. Zero weight queries without advancing time. */
double bishfpm_topk_add_weight(bishfpm_topk_t *, const char *, double,
                               uint64_t);

/* add item with increment/weight */
double bishfpm_topk_add(bishfpm_topk_t *topk, const char *pattern,
                        uint32_t increment, uint64_t now_ms);

/* Conservative resident count: estimate minus admission error. */
double bishfpm_topk_query_lower(bishfpm_topk_t *, const char *, uint64_t);

/* query estimated rate of item */
double bishfpm_topk_query(bishfpm_topk_t *topk, const char *pattern,
                          uint64_t now_ms);

/* query current minimum rate in top-k heap */
double bishfpm_topk_min_rate(const bishfpm_topk_t *topk, uint64_t now_ms);

/* get number of active items in heap */
uint32_t bishfpm_topk_count(const bishfpm_topk_t *topk);

/* Requested payload for the store and its arrays, excluding any enclosing
 * arena, alignment padding and allocator overhead. Zero for a NULL store. */
size_t bishfpm_topk_storage_bytes(const bishfpm_topk_t *topk);

/* prune stale entries whose rate is below min_rate */
size_t bishfpm_topk_prune(bishfpm_topk_t *topk, uint64_t now_ms,
                          double min_rate);

/* get current top-k items sorted by rate descending */
size_t bishfpm_topk_get_top(bishfpm_topk_t *topk, uint64_t now_ms,
                            bishfpm_topk_item_t *out_items, size_t max_items);

#ifdef __cplusplus
}
#endif

#endif /* BISHFPM_TOPK_H */
