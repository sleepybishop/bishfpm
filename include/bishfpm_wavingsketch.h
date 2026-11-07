#ifndef BISHFPM_WAVINGSKETCH_H
#define BISHFPM_WAVINGSKETCH_H
#include "bishfpm_store_plugin.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Experimental weighted EWMA adaptation of WavingSketch. Selecting this
 * descriptor opts into heuristic alerts and unknown bounds. Resident queries
 * return heavy-cell counts; absent queries return max(0, signed wave estimate).
 * Alerts use matching updates since admission, excluding collision history.
 * Evidence has its own timestamp and decays independently of bucket traffic.
 * No unbiasedness guarantee is claimed for this public estimator. Prune clears
 * only entirely zero buckets. */
typedef struct {
  uint32_t buckets;          /* 1..2^24; need not be a power of two */
  uint32_t cells_per_bucket; /* 1..64; product with buckets <=1000000 */
} bishfpm_wavingsketch_options_t;
void bishfpm_wavingsketch_options_default(bishfpm_wavingsketch_options_t *);
extern const bishfpm_store_descriptor_t bishfpm_wavingsketch_store;
#ifdef __cplusplus
}
#endif
#endif
