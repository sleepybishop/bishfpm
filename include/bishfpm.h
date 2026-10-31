#ifndef BISHFPM_H
#define BISHFPM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bishfpm_arena.h"
#include "bishfpm_hash.h"
#include "bishfpm_itemset.h"
#include "bishfpm_rate.h"
#include "bishfpm_topk.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BISHFPM_DEFAULT_HALF_LIFE_MS 5000.0
#define BISHFPM_DEFAULT_ITEM_THRESHOLD 5.0
#define BISHFPM_DEFAULT_PROMOTION_FRAC 0.20
#define BISHFPM_DEFAULT_MIN_PATTERN_RATE 5.0
#define BISHFPM_DEFAULT_MIN_ORDER 2
#define BISHFPM_DEFAULT_MAX_ORDER 3
#define BISHFPM_DEFAULT_TOPK_K 64
#define BISHFPM_DEFAULT_TOPK_WIDTH 256
#define BISHFPM_DEFAULT_TOPK_DEPTH 4
#define BISHFPM_DEFAULT_COOLDOWN_MS 10000

/* configuration parameters */
typedef struct {
  double ewma_half_life_ms;   /* half-life for continuous decay */
  double item_rate_threshold; /* item rate to trigger elevation */
  double promotion_fraction;  /* alpha: fraction of group rate for promotion */
  double min_pattern_rate;    /* absolute minimum rate for promotion */
  uint32_t
      min_itemset_order; /* min size of candidate pattern to trigger (e.g. 2) */
  uint32_t max_itemset_order; /* max size of candidate combinations (e.g. 3) */
  uint32_t topk_k;            /* number of heavy hitters to track per group */
  uint32_t topk_width;        /* HeavyKeeper table width */
  uint32_t topk_depth;        /* HeavyKeeper table depth */
  uint64_t trigger_cooldown_ms; /* cooldown before re-triggering same pattern */
} bishfpm_config_t;

/* triggered pattern result */
typedef struct {
  char pattern[BISHFPM_MAX_PATTERN_LEN]; /* canonical serialized pattern string
                                          */
  char formatted[BISHFPM_MAX_PATTERN_LEN]; /* human readable comma-separated
                                              string */
  double pattern_rate;                     /* current pattern rate */
  double group_rate;                       /* current total group rate */
  double rate_fraction;                    /* pattern_rate / group_rate */
  uint32_t item_count;                     /* number of items in pattern */
  uint64_t timestamp;                      /* timestamp of triggering event */
} bishfpm_trigger_t;

/* callback for triggered patterns */
typedef void (*bishfpm_trigger_cb)(const char *group_key,
                                   const bishfpm_trigger_t *trigger,
                                   void *udata);

/* opaque engine structure */
typedef struct bishfpm bishfpm_t;

/* initialize default config */
void bishfpm_config_default(bishfpm_config_t *cfg);

/* engine lifecycle */
bishfpm_t *bishfpm_create(const bishfpm_config_t *cfg);
void bishfpm_free(bishfpm_t *engine);

/* event ingestion */
int bishfpm_add_event(bishfpm_t *engine, const char *group_key,
                      uint64_t timestamp_ms, const char *const *items,
                      size_t num_items, bishfpm_trigger_cb cb, void *udata);

/* query current group rate */
double bishfpm_group_rate(bishfpm_t *engine, const char *group_key,
                          uint64_t now_ms);

/* query current item rate in group */
double bishfpm_item_rate(bishfpm_t *engine, const char *group_key,
                         const char *item, uint64_t now_ms);

/* query current pattern rate in group */
double bishfpm_pattern_rate(bishfpm_t *engine, const char *group_key,
                            const char *pattern, uint64_t now_ms);

/* prune inactive groups/items */
size_t bishfpm_prune(bishfpm_t *engine, uint64_t now_ms, double min_rate);

#ifdef __cplusplus
}
#endif

#endif /* BISHFPM_H */
