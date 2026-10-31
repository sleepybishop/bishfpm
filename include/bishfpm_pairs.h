#ifndef BISHFPM_PAIRS_H
#define BISHFPM_PAIRS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exact static/tumbling-window pair supports over a fixed dense vocabulary.
 * Caller assigns IDs 0..items-1. This is separate from the decayed engine:
 * there is no gating, decay, candidate admission or top-K output limit. */
typedef struct bishfpm_pair_counter bishfpm_pair_counter_t;

/* Allocates items*(items-1)/2 uint64_t counters. Budget includes the wrapper
 * and array payload, excluding allocator overhead. Zero/one item is valid.
 * Returns NULL for overflow, insufficient budget or allocation failure. */
bishfpm_pair_counter_t *bishfpm_pair_counter_new(uint32_t items,
                                                 size_t max_memory_bytes);
void bishfpm_pair_counter_free(bishfpm_pair_counter_t *counter);
void bishfpm_pair_counter_reset(bishfpm_pair_counter_t *counter);
size_t
bishfpm_pair_counter_storage_bytes(const bishfpm_pair_counter_t *counter);

/* Add one transaction, with multiplicity weight. IDs must be strictly
 * increasing (sorted and unique), and below items. Empty transactions advance
 * total weight. Returns 0 on success, -1 on invalid input or total-weight
 * overflow; on failure no counts change. All arguments are validated even when
 * weight is zero. No allocation occurs during updates. Caller serializes
 * concurrent access. */
int bishfpm_pair_counter_add(bishfpm_pair_counter_t *counter,
                             const uint32_t *ids, size_t count,
                             uint64_t weight);

/* Exact support for distinct IDs, in either order. Zero for invalid IDs/NULL.
 * Total is sum of accepted transaction weights, including empty transactions.
 */
uint64_t bishfpm_pair_counter_get(const bishfpm_pair_counter_t *counter,
                                  uint32_t a, uint32_t b);
uint64_t bishfpm_pair_counter_total(const bishfpm_pair_counter_t *counter);

#ifdef __cplusplus
}
#endif
#endif
