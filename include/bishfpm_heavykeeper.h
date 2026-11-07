#ifndef BISHFPM_HEAVYKEEPER_H
#define BISHFPM_HEAVYKEEPER_H
#include "bishfpm_store_plugin.h"
#ifdef __cplusplus
extern "C" {
#endif
/* experimental EWMA-decayed HeavyKeeper store plugin; selecting this
 * descriptor explicitly opts into estimate-based alerts without certified
 * lower or upper bounds */
typedef struct {
  uint32_t capacity; /* 1..1000000 resident candidates; independent of
                        notifications */
  uint32_t depth;    /* 1..32 rows */
  uint32_t width;    /* 1..2^24 cells per row, not necessarily a power of two */
  double decay_base; /* finite >0, !=1; values >1 are reciprocated */
} bishfpm_heavykeeper_options_t;
void bishfpm_heavykeeper_options_default(bishfpm_heavykeeper_options_t *);
extern const bishfpm_store_descriptor_t bishfpm_heavykeeper_store;
#ifdef __cplusplus
}
#endif
#endif
