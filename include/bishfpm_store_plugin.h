#ifndef BISHFPM_STORE_PLUGIN_H
#define BISHFPM_STORE_PLUGIN_H
#include "bishfpm.h"
#ifdef __cplusplus
extern "C" {
#endif

/* trusted synchronous in-process stores; all structs have an exact v1 prefix,
 * larger structs are accepted, and unknown trailing bytes are ignored; ABI
 * major and reserved fields in the known prefix must match; inputs are borrowed
 * only during a call while the engine copies the descriptor and clones config;
 * failed clone/create operations unwind all their own partial allocations;
 * successful create returns a non-NULL state; function code must remain loaded
 * until bishfpm_free; see docs/STORES.md */
#define BISHFPM_STORE_ABI_V1 1U
#define BISHFPM_STORE_TEXT 1U
#define BISHFPM_STORE_PAIR64 2U
#define BISHFPM_STORE_RESIDENT_BOUNDS UINT64_C(1)
#define BISHFPM_STORE_ABSENT_BOUNDS UINT64_C(2)
#define BISHFPM_STORE_STABLE_IDS UINT64_C(4)
/* Mutually exclusive with RESIDENT_BOUNDS/ABSENT_BOUNDS. Passing an estimate
 * descriptor to create_with_store explicitly opts into heuristic alerts.
 * known must be zero, lower zero (unless ALERT_EVIDENCE is set), and upper
 * a finite nonnegative estimate.
 * The host exposes bounds as unknown [0,+infinity] and alert error as infinity.
 * Existing certified descriptors retain their v1 semantics. */
#define BISHFPM_STORE_HEURISTIC_ESTIMATES UINT64_C(8)
/* Requires HEURISTIC_ESTIMATES. Results keep known=0 and upper=raw estimate,
 * but lower carries finite nonnegative matching-update evidence since the
 * resident identity's latest admission (including its admitting update).
 * Nonresident evidence must be zero. Collision/inherited mass never counts.
 * Promotion, lift, callback pattern_rate and subset suppression use evidence;
 * public bounds remain unknown. Without this bit, lower must remain zero and
 * heuristic alerts retain their estimate-based policy. No v1 layout changes. */
#define BISHFPM_STORE_ALERT_EVIDENCE UINT64_C(16)

typedef struct {
  void *context;
  void *(*allocate)(void *, size_t);
  void *(*allocate_zeroed)(void *, size_t, size_t);
  void (*release)(void *, void *, size_t);
  /* allocation alignment is that of malloc (at least max_align_t); no realloc;
   * sized release must use the exact original requested payload */
} bishfpm_store_host_t;
typedef struct {
  size_t engine_peak_bytes; /* config clone, including simultaneous buffers */
  size_t group_peak_bytes;  /* state plus all reservations/transients */
  uint32_t min_order, max_order;
  uint32_t reserved[2];
} bishfpm_store_plan_t;
/* dynamic mode permits SIZE_MAX peaks; conservative mode requires finite,
 * checked peaks; plans exclude host metadata, which the host adds itself */
typedef struct {
  uint32_t abi_version, struct_size;
  uint32_t memory_mode;
  uint32_t
      notification_capacity; /* 1..1000000, mapped to legacy topk_k sizing:
                              initial next_pow2(2*n), growth at least 4096 */
  uint64_t reserved[2];
} bishfpm_store_engine_options_t;
void bishfpm_store_engine_options_default(bishfpm_store_engine_options_t *);

typedef enum {
  BISHFPM_UPDATE_REJECTED =
      0, /* guarded numeric overflow; event stays accepted */
  BISHFPM_UPDATE_RESIDENT = 1,
  BISHFPM_UPDATE_ADMITTED = 2,
  BISHFPM_UPDATE_REPLACED = 3,
  BISHFPM_UPDATE_FILTERED =
      4,                  /* accepted contribution to nonresident history */
  BISHFPM_UPDATE_ZERO = 5 /* effective weight zero: query without mutation */
} bishfpm_store_update_t;
typedef struct {
  double lower,
      upper; /* ALERT_EVIDENCE: evidence, estimate; otherwise bounds */
  uint32_t resident, known;
  uint32_t outcome, reserved;
} bishfpm_store_count_t;
/* known intervals must be finite, 0 <= lower <= upper, and certified for the
 * evaluated-candidate stream; updates receive an already late-discounted weight
 * and common group time; never discount it again; zero weight cannot mutate;
 * queries are read-only and future queries cannot advance a watermark */
typedef struct {
  const char *const *items; /* canonical sorted unique items, before gating */
  const size_t *lengths;
  const uint32_t *ids; /* provisional group-local IDs, PAIR64 only */
  size_t count;
  uint64_t time_ms;
  double weight;
  uint32_t min_order, max_order;
} bishfpm_store_event_t;
typedef struct {
  size_t resident_patterns, resident_capacity;
  uint64_t diagnostic[4]; /* backend-defined; meanings belong to its guide */
} bishfpm_store_snapshot_t;

/* prepare: 1 ready, 0 capacity denial, -1 allocation failure; failed prepare
 * cleans its own scratch; successful prepare gets one cancel, or one commit
 * then one finish, even when gates produce no candidates; preparation changes
 * no visible counts, identity, history, residency, or clocks; a token must own
 * budgeted copies of anything needed after preparation; all required capacity
 * and allocation checks precede commit; commit/update/finish cannot fail with a
 * whole-event error; optional notification allocations are host-owned */
typedef struct {
  uint32_t abi_version, struct_size;
  uint32_t representation, reserved0;
  uint64_t capabilities;
  uint64_t reserved[2];
  int (*plan)(const bishfpm_config_t *, const void *, size_t,
              bishfpm_store_plan_t *);
  int (*clone_config)(const bishfpm_store_host_t *, const bishfpm_config_t *,
                      const void *, size_t, void **);
  void (*destroy_config)(const bishfpm_store_host_t *, void *);
  int (*create)(const bishfpm_store_host_t *, const void *, void **);
  void (*destroy)(const bishfpm_store_host_t *, void *);
  int (*prepare_event)(void *, const bishfpm_store_event_t *, void **);
  void (*cancel_event)(void *, void *);
  void (*commit_prepared)(void *, void *);
  void (*finish_event)(void *, void *);
  bishfpm_store_count_t (*update_text)(void *, const char *, size_t, uint64_t,
                                       double, uint64_t);
  bishfpm_store_count_t (*update_pair)(void *, uint64_t, uint64_t, double,
                                       uint64_t);
  bishfpm_store_count_t (*query_text)(const void *, const char *, size_t,
                                      uint64_t, uint64_t);
  /* missing dictionary identity: key UINT64_MAX, hash zero; this reserved key
   * cannot be resident; return the backend's certified/unknown absent interval
   */
  bishfpm_store_count_t (*query_pair)(const void *, uint64_t, uint64_t,
                                      uint64_t);
  size_t (*prune)(void *, uint64_t, double);
  void (*snapshot)(const void *, bishfpm_store_snapshot_t *);
  /* optional tail; short v1 descriptors end immediately before this field */
  size_t (*prune_bounded)(void *, uint64_t, size_t);
} bishfpm_store_descriptor_t;
#define BISHFPM_STORE_DESCRIPTOR_V1_SIZE                                       \
  offsetof(bishfpm_store_descriptor_t, prune_bounded)

bishfpm_t *bishfpm_create_with_store(const bishfpm_config_t *,
                                     const bishfpm_store_engine_options_t *,
                                     const bishfpm_store_descriptor_t *,
                                     const void *backend_config,
                                     size_t backend_config_size);
/* read-only copied descriptor metadata and backend diagnostics; a named group
 * is required for a snapshot; missing/unsupported calls return 0 and leave out
 * unchanged; struct_size must be at least sizeof the v1 output and larger
 * output tails are untouched; host pressure/pair metrics retain old domains */
typedef struct {
  uint32_t abi_version, struct_size;
  uint32_t representation, reserved;
  uint64_t capabilities;
  bishfpm_store_plan_t plan;
} bishfpm_store_description_t;
int bishfpm_get_store_description(const bishfpm_t *,
                                  bishfpm_store_description_t *);
int bishfpm_get_store_snapshot(const bishfpm_t *, const char *,
                               uint32_t struct_size,
                               bishfpm_store_snapshot_t *);
#ifdef __cplusplus
}
#endif
#endif
