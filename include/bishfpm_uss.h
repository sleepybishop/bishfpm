#ifndef BISHFPM_USS_H
#define BISHFPM_USS_H
#include "bishfpm_store_plugin.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Weighted Unbiased Space Saving with EWMA decay. Selecting this descriptor
 * opts into heuristic alerts; estimates are not certified bounds. Positive
 * sampled counters survive pruning, regardless of the requested cutoff.
 * Alerts use decayed matching updates since admission, excluding inherited
 * mass; query estimates remain heuristic. Whole-group retirement still resets
 * history. */
typedef struct {
  uint32_t
      capacity; /* 1..1000000 resident samples; independent of notifications */
  uint64_t
      random_seed; /* reproducible per-group PRNG stream; all values valid */
} bishfpm_uss_options_t;
void bishfpm_uss_options_default(bishfpm_uss_options_t *);
extern const bishfpm_store_descriptor_t bishfpm_uss_store;
/* Packed group-local pair IDs, orders exactly 2. Same options and USS/evidence
 * semantics as the text descriptor; 32-byte residents and 32-bit index slots.
 * UINT64_MAX is reserved for missing identities and cannot become resident. */
extern const bishfpm_store_descriptor_t bishfpm_uss_pair_store;
#ifdef __cplusplus
}
#endif
#endif
