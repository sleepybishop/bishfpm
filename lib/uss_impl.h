/* weighted Unbiased Space Saving (Ting, SIGMOD 2018) with pairwise unbiased
 * reduction and lazy exponential time decay using public interfaces */
#include "bishfpm_uss.h"
#include "plugin_common.h"

typedef struct {
#ifdef BISHFPM_USS_PAIR64
  uint64_t key;
#else
  uint64_t hash;
#endif
  double rate, evidence;
  uint64_t last_time_ms;
#ifndef BISHFPM_USS_PAIR64
  uint16_t length;
  char pattern[BISHFPM_MAX_PATTERN_LEN];
#endif
} uss_entry_t;
#ifdef BISHFPM_USS_PAIR64
typedef uint32_t uss_slot_t;
#else
typedef plugin_text_slot_t uss_slot_t;
#endif
typedef struct {
  bishfpm_uss_options_t options;
  double half_life_ms;
  size_t slots, heap_bytes, map_bytes, group_bytes;
} uss_config_t;
typedef struct {
  const uss_config_t *config;
  uint32_t heap_size;
  uss_entry_t *heap;
  uss_slot_t *slot_map;
  uint64_t rng_state, max_time_ms;
  double total;
} uss_store_t;
#define PLUGIN_STORE_T uss_store_t
#define PLUGIN_ENTRY_T uss_entry_t
#include "plugin_common.h"

#ifndef BISHFPM_USS_PAIR64
void bishfpm_uss_options_default(bishfpm_uss_options_t *o) {
  if (o)
    *o = (bishfpm_uss_options_t){64, UINT64_C(123456789)};
}
#endif
static bool configure(const bishfpm_config_t *mining, const void *raw, size_t n,
                      uss_config_t *c) {
  if (!mining || !raw || n != sizeof(bishfpm_uss_options_t))
    return false;
  const bishfpm_uss_options_t *o = raw;
  if (!o->capacity || o->capacity > 1000000 ||
      !isfinite(mining->ewma_half_life_ms) || mining->ewma_half_life_ms <= 0)
    return false;
#ifdef BISHFPM_USS_PAIR64
  if (mining->min_itemset_order != 2 || mining->max_itemset_order != 2)
    return false;
#endif
  *c = (uss_config_t){0};
  c->options = *o;
  c->half_life_ms = mining->ewma_half_life_ms;
  c->slots = plugin_slots(o->capacity);
  size_t total = sizeof(uss_store_t);
  return checked_mul(o->capacity, sizeof(uss_entry_t), &c->heap_bytes) &&
         checked_mul(c->slots, sizeof(uss_slot_t), &c->map_bytes) &&
         checked_add(total, c->heap_bytes, &total) &&
         checked_add(total, c->map_bytes, &c->group_bytes);
}
#ifdef BISHFPM_USS_PAIR64
BISHFPM_PLUGIN_CONFIG_OPS(uss_config_t, 2, 2)
#else
BISHFPM_PLUGIN_CONFIG_OPS(uss_config_t, 1, BISHFPM_MAX_ITEMSET_ORDER)
#endif
static void destroy(const bishfpm_store_host_t *h, void *raw) {
  uss_store_t *s = raw;
  const uss_config_t *c = s->config;
  h->release(h->context, s->heap, c->heap_bytes);
  h->release(h->context, s->slot_map, c->map_bytes);
  h->release(h->context, s, sizeof(*s));
}
static int create(const bishfpm_store_host_t *h, const void *raw, void **out) {
  const uss_config_t *c = raw;
  uss_store_t *s = h->allocate_zeroed(h->context, 1, sizeof(*s));
  if (!s)
    return 0;
  s->config = c;
  s->rng_state = c->options.random_seed;
  s->heap = h->allocate_zeroed(h->context, 1, c->heap_bytes);
  s->slot_map = h->allocate_zeroed(h->context, 1, c->map_bytes);
  if (!s->heap || !s->slot_map) {
    destroy(h, s);
    return 0;
  }
  *out = s;
  return 1;
}
static bishfpm_store_count_t query_key(const void *raw, plugin_key_t key,
                                       size_t length, uint64_t hash,
                                       uint64_t now) {
#ifndef BISHFPM_USS_PAIR64
  if (!valid_text_key(key, length))
    return (bishfpm_store_count_t){0, 0, 0, 0, BISHFPM_UPDATE_ZERO, 0};
#endif
  const uss_store_t *s = raw;
  now = effective_time(now, s->max_time_ms);
  uint32_t idx;
  bool resident = slot_map_find(s, key, length, hash, &idx);
  return (bishfpm_store_count_t){
      resident ? decay_since(s->heap[idx].evidence, s->heap[idx].last_time_ms,
                             now, s->config->half_life_ms)
               : 0,
      resident ? item_rate(&s->heap[idx], now, s->config->half_life_ms) : 0,
      resident,
      0,
      BISHFPM_UPDATE_ZERO,
      0};
}
static bishfpm_store_count_t update_key(void *raw, plugin_key_t pattern,
                                        size_t length, uint64_t hash,
                                        double weight, uint64_t now) {
#ifndef BISHFPM_USS_PAIR64
  if (!valid_text_key(pattern, length))
    return (bishfpm_store_count_t){
        0, 0, 0, 0, weight == 0 ? BISHFPM_UPDATE_ZERO : BISHFPM_UPDATE_REJECTED,
        0};
#endif
  uss_store_t *s = raw;
  now = effective_time(now, s->max_time_ms);
  if (!weight)
    return query_key(s, pattern, length, hash, now);
  bishfpm_store_count_t r = {0};
  double half = s->config->half_life_ms;
  double total = decay_since(s->total, s->max_time_ms, now, half);
  uint32_t idx, k = s->config->options.capacity;
  bool present = slot_map_find(s, pattern, length, hash, &idx);
  bool full = s->heap_size == k;
  if (!present)
    idx = full ? 0 : s->heap_size;
  double old = (present || full) ? item_rate(&s->heap[idx], now, half) : 0;
  /* reject before clocks, counters, identities, or RNG change */
  if (!(weight > 0) || weight > DBL_MAX - total || weight > DBL_MAX - old)
    return r;
  double evidence = (present || full)
                        ? decay_since(s->heap[idx].evidence,
                                      s->heap[idx].last_time_ms, now, half)
                        : 0;
  if (present && weight > DBL_MAX - evidence)
    return r;
  double combined = old + weight;
  s->total = total + weight;
  s->max_time_ms = now;
  bool retain = true;
  if (!present && full && old > 0) {
    double u = (double)(bishfpm_splitmix64(&s->rng_state) >> 11) *
               (1.0 / 9007199254740992.0);
    retain = u < weight / combined;
  }
  /* losing incoming identity still increases old minimum for unbiasedness */
  if (!present && retain) {
    if (full)
      slot_map_remove(s, idx);
    else
      s->heap_size++;
    entry_identity(&s->heap[idx], pattern, length, hash);
    slot_map_set(s, hash, idx);
  }
  /* losing incoming identity adds sampled mass, not resident evidence */
  evidence = present ? evidence + weight : retain ? weight : evidence;
  s->heap[idx].evidence = evidence;
  s->heap[idx].rate = combined;
  s->heap[idx].last_time_ms = now;
  if (!present && !full)
    heapify_up(s, idx, now);
  else
    heapify_down(s, idx, now);
  r.resident = present || retain;
  r.upper = r.resident ? combined : 0;
  r.lower = r.resident ? evidence : 0;
  r.outcome = present   ? BISHFPM_UPDATE_RESIDENT
              : !retain ? BISHFPM_UPDATE_FILTERED
              : full    ? BISHFPM_UPDATE_REPLACED
                        : BISHFPM_UPDATE_ADMITTED;
  return r;
}
static size_t prune(void *raw, uint64_t now, double cutoff) {
  (void)cutoff; /* discarding positive sampled mass would bias estimates */
  return heap_prune(raw, now, 0, true);
}
static void snapshot(const void *raw, bishfpm_store_snapshot_t *out) {
  const uss_store_t *s = raw;
  *out = (bishfpm_store_snapshot_t){
      s->heap_size, s->config->options.capacity, {0, 0, 0, 0}};
}
#ifdef BISHFPM_USS_PAIR64
static bishfpm_store_count_t query_pair(const void *raw, uint64_t key,
                                        uint64_t hash, uint64_t now) {
  (void)hash;
  if (key == UINT64_MAX)
    return (bishfpm_store_count_t){0, 0, 0, 0, BISHFPM_UPDATE_ZERO, 0};
  uint64_t copy = key;
  return query_key(raw, key, 0, bishfpm_splitmix64(&copy), now);
}
static bishfpm_store_count_t update_pair(void *raw, uint64_t key, uint64_t hash,
                                         double weight, uint64_t now) {
  (void)hash;
  if (key == UINT64_MAX)
    return (bishfpm_store_count_t){
        0, 0, 0, 0, weight == 0 ? BISHFPM_UPDATE_ZERO : BISHFPM_UPDATE_REJECTED,
        0};
  uint64_t copy = key;
  return update_key(raw, key, 0, bishfpm_splitmix64(&copy), weight, now);
}
const bishfpm_store_descriptor_t bishfpm_uss_pair_store = {
#else
const bishfpm_store_descriptor_t bishfpm_uss_store = {
#endif
    BISHFPM_STORE_ABI_V1,
    sizeof(bishfpm_store_descriptor_t),
#ifdef BISHFPM_USS_PAIR64
    BISHFPM_STORE_PAIR64,
    0,
    BISHFPM_STORE_HEURISTIC_ESTIMATES | BISHFPM_STORE_ALERT_EVIDENCE |
        BISHFPM_STORE_STABLE_IDS,
#else
    BISHFPM_STORE_TEXT,
    0,
    BISHFPM_STORE_HEURISTIC_ESTIMATES | BISHFPM_STORE_ALERT_EVIDENCE,
#endif
    {0, 0},
    plan,
    clone_config,
    destroy_config,
    create,
    destroy,
    plugin_prepare,
    plugin_complete,
    plugin_complete,
    plugin_complete,
#ifdef BISHFPM_USS_PAIR64
    NULL,
    update_pair,
    NULL,
    query_pair,
#else
    update_key,
    NULL,
    query_key,
    NULL,
#endif
    prune,
    snapshot,
    NULL};
