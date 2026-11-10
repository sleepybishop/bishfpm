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

/* Configuration parameters. Floating-point fields must be finite.
 * Count thresholds and min_lift must be nonnegative; shares and
 * subsumption_ratio must be in [0, 1]. When min_pattern_rate is positive,
 * item_rate_threshold must not exceed it. Capacity and work limits must be
 * positive and within the ranges below. In conservative memory mode the
 * configured worst-case payload bound must fit max_memory_bytes. */
typedef struct {
  double ewma_half_life_ms;    /* positive half-life for continuous decay */
  double item_rate_threshold;  /* minimum decayed item count for gating */
  double item_share_threshold; /* min fraction of group count for item gating
                                  (0.0 to disable) */
  double promotion_fraction;   /* minimum pattern lower-count / group count */
  double min_pattern_rate;     /* minimum decayed pattern lower count */
  double min_lift; /* min statistical lift for combination alert (0.0 to
                      disable) */
  double subsumption_ratio;   /* suppress when candidate lower count < ratio *
                                 active subset lower count (0.0 to disable) */
  uint32_t min_itemset_order; /* 1..max_itemset_order */
  uint32_t max_itemset_order; /* at most BISHFPM_MAX_ITEMSET_ORDER */
  uint32_t topk_k; /* 1..1000000; general-store capacity and cooldown sizing */
  uint64_t trigger_cooldown_ms; /* cooldown before re-triggering same pattern */
  uint32_t max_groups;          /* 1..65536 groups */
  uint32_t max_items_per_group; /* 1..1048576 items per group */
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
  size_t
      memory_bound_bytes; /* static bound or dynamic cap, includes transients */
  size_t active_groups;   /* current number of tracked groups */
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
  double pattern_rate;        /* conservative count; heuristic plugins use their
                                 estimate or admission evidence per capability */
  double pattern_upper_bound; /* decayed pattern upper bound */
  double pattern_error;       /* upper minus conservative pattern_rate */
  double group_rate;          /* current decayed group count */
  double rate_fraction;       /* pattern_rate / group_rate */
  double lift;                /* statistical lift of pattern */
  uint32_t item_count;        /* number of items in pattern */
  uint64_t timestamp;         /* effective group time in milliseconds */
} bishfpm_trigger_t;

/* callback for triggered patterns */
typedef void (*bishfpm_trigger_cb)(const char *group_key,
                                   const bishfpm_trigger_t *trigger,
                                   void *udata);

/* opaque engine structure */
typedef struct bishfpm bishfpm_t;

/* initialize default config */
void bishfpm_config_default(bishfpm_config_t *cfg);

typedef enum {
  BISHFPM_STORE_SS = 0,
  BISHFPM_STORE_FSS = 1 /* experimental general-itemset filtered Space-Saving */
} bishfpm_store_t;
typedef enum {
  BISHFPM_MEMORY_CONSERVATIVE = 0,
  BISHFPM_MEMORY_DYNAMIC = 1
} bishfpm_memory_mode_t;
typedef struct {
  bishfpm_store_t store;
  uint32_t sketch_width; /* FSS: 1..2^24; SS: zero. */
  bishfpm_memory_mode_t memory_mode;
} bishfpm_options_t;
/* Defaults select SS and the existing conservative memory mode. */
void bishfpm_options_default(bishfpm_options_t *);
/* Opt-in options; NULL options preserve the existing constructor behavior.
 * Dynamic mode caps each engine-owned requested allocation, including staging
 * and concurrent old/new buffers, rather than requiring the configured maximum
 * group population to fit at construction. Required reservation denial rejects
 * an event before counts change. Optional cooldown allocation denial retains
 * active protection and can suppress alerts after count acceptance. */
bishfpm_t *bishfpm_create_with_options(const bishfpm_config_t *,
                                       const bishfpm_options_t *);

/* Engine lifecycle. Constructors return NULL for invalid configuration,
 * an excessive payload requirement for the selected memory mode, or allocation
 * failure. Existing constructors use conservative memory mode.
 * bishfpm_create(NULL) uses defaults. */
bishfpm_t *bishfpm_create(const bishfpm_config_t *cfg);
/* Optional notification filter for triples and quadruples. Zero (the default)
 * disables it. A positive finite threshold suppresses only when certified
 * count bounds prove that some two-part partition has lift below it.
 * Uncertain evidence passes through; pairs and orders above four are unchanged.
 * Call before the first accepted event. Returns 1 on success, or 0 for NULL,
 * negative/nonfinite thresholds, reentrancy, or an engine that has accepted data.
 * Counting, candidate admission, existing config/trigger layouts and lift are
 * unchanged. This descriptive filter is not a statistical significance test. */
int bishfpm_set_min_partition_lift(bishfpm_t *, double threshold);

typedef struct {
  uint64_t candidates_checked;
  uint64_t candidates_suppressed;
  uint64_t candidates_unknown;
} bishfpm_alert_quality_stats_t;
/* Cumulative filter decisions after count/share and existing lift eligibility.
 * Unknown counts pass-through decisions with unavailable partition evidence.
 * A checked candidate is not necessarily proven productive. NULL arguments
 * leave output unchanged; snapshots do not reset counters. */
void bishfpm_get_alert_quality_stats(const bishfpm_t *,
                                     bishfpm_alert_quality_stats_t *);

/* Opt-in numeric pair backends; cfg must have min/max itemset order 2.
 * IDs persist for a group's lifetime: marginal keys are retained across prune.
 * capacity bounds pair identities independently of topk_k (notification state).
 * THRESHOLD requires disabled marginal gates. In surviving groups it only
 * forgets patterns whose upper bound <= forget_below; saturation rejects the
 * whole event before count mutation. No offline prepass or future vocabulary
 * is used. A zero forgetting floor uses compact exact counts and reclaims
 * only counts that decay to zero. */
typedef enum {
  BISHFPM_PAIR_SS = 0,
  BISHFPM_PAIR_THRESHOLD = 1,
  BISHFPM_PAIR_FSS =
      2 /* experimental filtered SpaceSaving with decayed buckets */
} bishfpm_pair_store_t;
typedef struct {
  bishfpm_pair_store_t store;
  uint32_t capacity;     /* 1..2^28 pairs, subject to the memory budget */
  uint32_t sketch_width; /* FSS bucket count, 1..2^24; zero for SS/threshold */
  double forget_below;   /* finite, nonnegative bounded-prune cutoff;
                            threshold also uses it on ingestion */
} bishfpm_pair_options_t;
bishfpm_t *bishfpm_create_pairs(const bishfpm_config_t *,
                                const bishfpm_pair_options_t *);

/* Pair selection stays in pair_options; engine options must select SS with
 * sketch_width zero here, and may select either memory mode. */
bishfpm_t *bishfpm_create_pairs_with_options(const bishfpm_config_t *,
                                             const bishfpm_pair_options_t *,
                                             const bishfpm_options_t *);

typedef struct {
  double lower, upper;
  int resident;
  int known; /* 0: no certified interval */
} bishfpm_count_bounds_t;
/* Returns 1 for a valid query, including absent keys. Threshold/FSS absence has
 * lower=0, upper=the decayed absent-history bound (including general FSS).
 * Invalid queries return 0.
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

/* Query decayed group count at now_ms, clamped to the group watermark. */
double bishfpm_group_rate(bishfpm_t *engine, const char *group_key,
                          uint64_t now_ms);

/* Query decayed item count at now_ms, clamped to the group watermark. */
double bishfpm_item_rate(bishfpm_t *engine, const char *group_key,
                         const char *item, uint64_t now_ms);

/* Query the pattern upper estimate at now_ms, clamped to the group watermark.
 * Explicit heuristic plugins return an uncertified estimate instead.
 * Use bishfpm_pattern_bounds to distinguish unknown absence from zero support.
 */
double bishfpm_pattern_rate(bishfpm_t *engine, const char *group_key,
                            const char *pattern, uint64_t now_ms);

/* Numeric pair engines: forget at most max_pairs weak candidates in the named
 * group, using options.forget_below. SS/FSS remove upper counts strictly below
 * the cutoff; threshold also removes equal counts. Leaves groups and marginal
 * identities/counts intact; returns the number forgotten. Missing groups,
 * general-itemset stores, zero budget and reentrant calls are no-ops.
 * Timestamps advance the group clock; late input is discounted. Bounds
 * removals, not wall-clock time: hash probes, scans and heap movement vary in
 * cost. Threshold ingestion may still bulk-prune to reserve capacity for a
 * complete event. */
size_t bishfpm_prune_pairs(bishfpm_t *, const char *group_key, uint64_t now_ms,
                           size_t max_pairs);

/* Full maintenance: remove groups and marginal counts strictly below min_rate.
 * Numeric marginal identities remain in surviving groups. SS/FSS patterns are
 * removed when their upper counts are < min_rate; threshold pairs use
 * upper <= min(min_rate, options.forget_below). Removing a group discards all
 * its state regardless of pattern cutoffs. Successful maintenance advances
 * surviving group clocks, clamping late timestamps to their watermarks.
 * Returns groups removed, not items or patterns; zero can still mean those
 * were pruned. Nonfinite/nonpositive min_rate and reentrant calls are no-ops.
 */
size_t bishfpm_prune(bishfpm_t *engine, uint64_t now_ms, double min_rate);

typedef struct {
  size_t retained_items, item_limit, remaining_items;
  int lifetime_retention; /* numeric dictionaries keep IDs until group removal
                           */
} bishfpm_vocabulary_stats_t;
/* Read-only; returns 1 for an existing group. Missing/invalid input returns 0
 * without changing output. Retained includes dormant numeric identities. */
int bishfpm_get_vocabulary_stats(const bishfpm_t *, const char *group,
                                 bishfpm_vocabulary_stats_t *);

typedef struct {
  uint64_t candidates_evaluated, resident_updates, admissions;
  uint64_t replacements, filtered_updates, rejected_updates,
      zero_weight_queries;
  size_t resident_patterns, resident_capacity;
} bishfpm_pattern_pressure_t;
/* General SS/FSS only. NULL group selects engine totals, including counters
 * from removed groups; a named group selects its current lifetime. Gauges
 * describe currently retained groups. Admissions include replacements.
 * evaluated = resident_updates + admissions + filtered_updates +
 * rejected_updates
 *             + zero_weight_queries.
 * Filtered updates leave a nonresident candidate in the FSS sketch; rejected
 * updates overflow a store count. Zero effective weights count separately as
 * queries without changing store state. No time is advanced by this snapshot.
 * Missing groups, numeric engines, or invalid arguments return 0 and leave out
 * unchanged. Read-only snapshots do not reset counters. Differences between
 * snapshots give interval replacement/filter rates, not measured recall. */
int bishfpm_get_pattern_pressure(const bishfpm_t *, const char *group,
                                 bishfpm_pattern_pressure_t *out);

typedef struct {
  bishfpm_memory_mode_t mode;
  size_t current_bytes, peak_bytes, limit_bytes;
  uint64_t allocation_denials, event_rejections;
} bishfpm_memory_stats_t;
/* Dynamic mode reports exact current and peak requested payload. Denials
 * include optional maintenance; event_rejections counts required reservations
 * denied by the memory cap. Such event failures also count in the existing
 * allocation_failure reason and budget_rejections statistic. Conservative mode
 * reports current payload and configured limit; peak/denial fields are zero
 * because it uses a static bound rather than an allocation ledger. */
void bishfpm_get_memory_stats(const bishfpm_t *, bishfpm_memory_stats_t *);

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
