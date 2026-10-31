#ifndef BISHFPM_H
#define BISHFPM_H

/*
 * bishfpm - Boundary-Independent Streaming Heuristic Frequent Pattern Mining
 */

#include "bishfpm_limits.h"
#include "bishfpm_version.h"
#include <stddef.h>
#include <stdint.h>

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
#define BISHFPM_DEFAULT_COOLDOWN_MS 10000
#define BISHFPM_DEFAULT_ITEM_SHARE_THRESHOLD 0.0
#define BISHFPM_DEFAULT_MIN_LIFT 0.0
#define BISHFPM_DEFAULT_SUBSUMPTION_RATIO 0.0
#define BISHFPM_DEFAULT_MAX_GROUPS 32
#define BISHFPM_DEFAULT_MAX_ITEMS_PER_GROUP 1024
#define BISHFPM_DEFAULT_HASH_SEED 0

/* configuration parameters */
typedef struct {
  double ewma_half_life_ms;    /* half-life for continuous decay */
  double item_rate_threshold;  /* item rate to trigger elevation */
  double item_share_threshold; /* min fraction of group rate for item gating
                                  (0.0 to disable) */
  double promotion_fraction;   /* alpha: fraction of group rate for promotion */
  double min_pattern_rate;     /* absolute minimum rate for promotion */
  double min_lift; /* min statistical lift for combination alert (0.0 to
                      disable) */
  double subsumption_ratio; /* max rate drop for superset alerts vs active
                               subset (0.0 to disable) */
  uint32_t
      min_itemset_order; /* min size of candidate pattern to trigger (e.g. 2) */
  uint32_t max_itemset_order; /* max size of candidate combinations (e.g. 3) */
  uint32_t topk_k;            /* number of heavy hitters to track per group */
  uint64_t trigger_cooldown_ms; /* cooldown before re-triggering same pattern */
  uint32_t max_groups;          /* positive maximum groups to track */
  uint32_t max_items_per_group; /* positive maximum items per group */
  uint64_t hash_seed;           /* hash seed for partitioning (0 = default) */
  uint32_t max_event_items; /* input pointer count, default 256, at most 4096 */
  uint32_t max_candidates_per_event; /* worst-case subset budget, default 8192;
                                       ceiling BISHFPM_MAX_EVENT_CANDIDATES */
  size_t max_memory_bytes; /* conservative requested-payload budget, default 256
                              MiB */
} bishfpm_config_t;

/* operational statistics */
typedef struct {
  uint64_t events_total;         /* attempted ingestion calls on this engine */
  uint64_t events_rejected;      /* events rejected */
  uint64_t candidates_evaluated; /* candidate combinations evaluated */
  uint64_t triggers_emitted;     /* eligible notification decisions */
  uint64_t triggers_suppressed;  /* triggers suppressed */
  uint64_t groups_pruned;        /* total groups pruned */
  uint64_t items_pruned;         /* total items pruned */
  uint64_t events_accepted;
  uint64_t events_noop;
  uint64_t budget_rejections;
  uint64_t cooldown_saturation;
  uint64_t callbacks_delivered;
  uint64_t pattern_replacements;
  size_t memory_current_bytes; /* current requested payload, excludes allocator
                                  overhead */
  size_t memory_bound_bytes;   /* includes worst-case transient reservations */
  size_t active_groups;        /* current number of tracked groups */
} bishfpm_stats_t;

/* One first-detected reason per rejected call on a non-NULL engine.
 * Separate snapshot keeps the existing bishfpm_stats_t layout unchanged. */
typedef struct {
  uint64_t invalid_input;
  uint64_t reentrant;
  uint64_t event_items_limit;
  uint64_t pattern_length_limit;
  uint64_t candidate_limit;
  uint64_t group_limit;
  uint64_t marginal_limit;
  uint64_t allocation_failure;
} bishfpm_rejection_stats_t;

/* triggered pattern result */
typedef struct {
  char pattern[BISHFPM_MAX_PATTERN_LEN]; /* canonical serialized pattern string
                                          */
  char formatted[BISHFPM_MAX_PATTERN_LEN]; /* human readable comma-separated
                                              string */
  double pattern_rate;        /* conservative decayed pattern count */
  double pattern_upper_bound; /* resident SpaceSaving upper estimate */
  double pattern_error;       /* upper minus conservative pattern_rate */
  double group_rate;          /* current total group rate */
  double rate_fraction;       /* pattern_rate / group_rate */
  double lift;                /* statistical lift of pattern */
  uint32_t item_count;        /* number of items in pattern */
  uint64_t timestamp;         /* timestamp of triggering event */
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
/* Opt-in numeric pair backends; cfg must have min/max itemset order 2.
 * IDs persist for a group's lifetime: marginal keys are retained across prune.
 * capacity bounds pair identities independently of topk_k (notification state).
 * THRESHOLD requires disabled marginal gates. It only forgets patterns whose
 * upper bound <= forget_below; saturation rejects the whole event before count
 * mutation. No offline prepass or future vocabulary is used. */
typedef enum {
  BISHFPM_PAIR_SS = 0,
  BISHFPM_PAIR_THRESHOLD = 1,
  BISHFPM_PAIR_FSS =
      2 /* experimental filtered SpaceSaving with decayed buckets */
} bishfpm_pair_store_t;
typedef struct {
  bishfpm_pair_store_t store;
  uint32_t capacity;
  uint32_t sketch_width; /* FSS bucket count, 1..2^24; zero for SS/threshold */
  double forget_below;   /* bounded-prune cutoff; threshold also uses it on
                            ingestion */
} bishfpm_pair_options_t;
bishfpm_t *bishfpm_create_pairs(const bishfpm_config_t *,
                                const bishfpm_pair_options_t *);

typedef struct {
  double lower, upper;
  int resident;
  int known; /* 0: no certified interval */
} bishfpm_count_bounds_t;
/* Returns 1 for a valid query, including absent keys. Threshold/FSS absence has
 * lower=0, upper=the decayed absent-history bound. Invalid queries return 0.
 * Bounds cover evaluated candidates; disable both marginal gates for full
 * transaction support. */
int bishfpm_pattern_bounds(bishfpm_t *, const char *group, const char *pattern,
                           uint64_t now_ms, bishfpm_count_bounds_t *);
typedef struct {
  uint64_t capacity_rejections;
  uint64_t patterns_forgotten;
  size_t resident_pairs;
  size_t store_bytes;
} bishfpm_pair_stats_t;
void bishfpm_get_pair_stats(const bishfpm_t *, bishfpm_pair_stats_t *);

void bishfpm_free(bishfpm_t *engine);

/* Event ingestion: -1 failure (no event mutation), otherwise notification
 * decisions. Zero-weight/empty-item calls are no-ops. See docs/DESIGN.md for
 * timestamp, retry, callback lifetime/reentrancy and synchronization rules. */
int bishfpm_add_event(bishfpm_t *engine, const char *group_key,
                      uint64_t timestamp_ms, const char *const *items,
                      size_t num_items, bishfpm_trigger_cb cb, void *udata);

/* event ingestion with count/weight */
int bishfpm_add_event_weighted(bishfpm_t *engine, const char *group_key,
                               uint64_t timestamp_ms, const char *const *items,
                               size_t num_items, uint32_t weight,
                               bishfpm_trigger_cb cb, void *udata);

/* query current group rate */
double bishfpm_group_rate(bishfpm_t *engine, const char *group_key,
                          uint64_t now_ms);

/* query current item rate in group */
double bishfpm_item_rate(bishfpm_t *engine, const char *group_key,
                         const char *item, uint64_t now_ms);

/* query current pattern rate in group */
double bishfpm_pattern_rate(bishfpm_t *engine, const char *group_key,
                            const char *pattern, uint64_t now_ms);

/* Numeric pair engines: forget at most max_pairs weak candidates in the named
 * group, using options.forget_below. SS/FSS remove upper counts strictly below
 * the cutoff; threshold also removes equal counts. Leaves groups and marginal
 * identities/counts intact; returns the number forgotten. Missing groups,
 * general-itemset stores, zero budget and reentrant calls are no-ops.
 * Timestamps advance the group clock; late input is discounted. Bounds
 * removals, not wall-clock time: hash probes/heap movement vary in cost.
 * Threshold ingestion may still bulk-prune to reserve capacity for a complete
 * event. */
size_t bishfpm_prune_pairs(bishfpm_t *, const char *group_key, uint64_t now_ms,
                           size_t max_pairs);

/* prune inactive groups/items */
size_t bishfpm_prune(bishfpm_t *engine, uint64_t now_ms, double min_rate);

/* query engine statistics */
void bishfpm_get_stats(const bishfpm_t *engine, bishfpm_stats_t *out);

/* Snapshot cumulative rejection reasons; NULL arguments leave output unchanged.
 * The sum equals events_rejected. Reading does not reset counters. */
void bishfpm_get_rejection_stats(const bishfpm_t *engine,
                                 bishfpm_rejection_stats_t *out);

/* query library version */
const char *bishfpm_version(void);

#ifdef __cplusplus
}
#endif

#endif /* BISHFPM_H */
