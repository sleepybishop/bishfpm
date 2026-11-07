/* Weighted Unbiased Space Saving, following Ting (SIGMOD 2018), with
 * pairwise unbiased reduction and lazy exponential time decay. Only installed
 * public interfaces are used. */
#include "bishfpm_hash.h"
#include "bishfpm_rate.h"
#include "bishfpm_uss.h"
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#ifdef BISHFPM_USS_PAIR64
typedef uint64_t uss_key_t;
typedef uint32_t uss_slot_t; /* zero empty; otherwise heap index + 1 */
#else
typedef const char *uss_key_t;
#endif
typedef struct {
#ifdef BISHFPM_USS_PAIR64
  uint64_t key;
#else
  char pattern[BISHFPM_MAX_PATTERN_LEN];
  uint64_t hash;
#endif
  double rate, evidence;
  uint64_t last_time_ms;
} uss_entry_t;
#ifndef BISHFPM_USS_PAIR64
typedef struct {
  uint64_t hash;
  uint32_t heap_idx;
  bool occupied;
} uss_slot_t;
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
static uint64_t effective_time(uint64_t a, uint64_t b) { return a > b ? a : b; }
static inline double decay_since(double count, uint64_t then, uint64_t now,
                                 double half) {
  return now <= then ? count : bishfpm_rate_decay(count, now - then, half);
}
static bool checked_add(size_t a, size_t b, size_t *out) {
  if (b > SIZE_MAX - a)
    return false;
  *out = a + b;
  return true;
}
static bool checked_mul(size_t a, size_t b, size_t *out) {
  if (a && b > SIZE_MAX / a)
    return false;
  *out = a * b;
  return true;
}
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
  c->slots = 16;
  while (c->slots < (size_t)o->capacity * 2)
    c->slots *= 2;
  size_t total = sizeof(uss_store_t);
  return checked_mul(o->capacity, sizeof(uss_entry_t), &c->heap_bytes) &&
         checked_mul(c->slots, sizeof(uss_slot_t), &c->map_bytes) &&
         checked_add(total, c->heap_bytes, &total) &&
         checked_add(total, c->map_bytes, &c->group_bytes);
}
static int plan(const bishfpm_config_t *m, const void *raw, size_t n,
                bishfpm_store_plan_t *out) {
  uss_config_t c;
  if (!out || !configure(m, raw, n, &c))
    return 0;
  *out = (bishfpm_store_plan_t){
#ifdef BISHFPM_USS_PAIR64
      sizeof(c), c.group_bytes, 2, 2, {0, 0}};
#else
      sizeof(c), c.group_bytes, 1, BISHFPM_MAX_ITEMSET_ORDER, {0, 0}};
#endif
  return 1;
}
static int clone_config(const bishfpm_store_host_t *h,
                        const bishfpm_config_t *m, const void *raw, size_t n,
                        void **out) {
  uss_config_t c;
  if (!h || !out || !configure(m, raw, n, &c))
    return 0;
  uss_config_t *owned = h->allocate(h->context, sizeof(*owned));
  if (!owned)
    return 0;
  *owned = c;
  *out = owned;
  return 1;
}
static void destroy_config(const bishfpm_store_host_t *h, void *c) {
  h->release(h->context, c, sizeof(uss_config_t));
}
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
static int prepare(void *state, const bishfpm_store_event_t *event,
                   void **token) {
  (void)state;
  (void)event;
  *token = NULL;
  return 1;
}
static void complete(void *state, void *token) {
  (void)state;
  (void)token;
}
static uint64_t entry_hash(const uss_entry_t *e) {
#ifdef BISHFPM_USS_PAIR64
  uint64_t key = e->key;
  return bishfpm_splitmix64(&key);
#else
  return e->hash;
#endif
}
static bool entry_matches(const uss_entry_t *e, uss_key_t key) {
#ifdef BISHFPM_USS_PAIR64
  return e->key == key;
#else
  return strcmp(e->pattern, key) == 0;
#endif
}
static void entry_identity(uss_entry_t *e, uss_key_t key, size_t length,
                           uint64_t hash) {
#ifdef BISHFPM_USS_PAIR64
  (void)length;
  (void)hash;
  e->key = key;
#else
  memcpy(e->pattern, key, length + 1);
  e->hash = hash;
#endif
}
static bool slot_occupied(const uss_slot_t *s) {
#ifdef BISHFPM_USS_PAIR64
  return *s != 0;
#else
  return s->occupied;
#endif
}
static uint32_t slot_index(const uss_slot_t *s) {
#ifdef BISHFPM_USS_PAIR64
  return *s - 1;
#else
  return s->heap_idx;
#endif
}
static void slot_index_set(uss_slot_t *s, uint32_t index) {
#ifdef BISHFPM_USS_PAIR64
  *s = index + 1;
#else
  s->heap_idx = index;
#endif
}
static uint64_t slot_hash(const uss_store_t *s, size_t slot) {
#ifdef BISHFPM_USS_PAIR64
  return entry_hash(&s->heap[slot_index(&s->slot_map[slot])]);
#else
  return s->slot_map[slot].hash;
#endif
}
static int slot_map_find(const uss_store_t *s, uss_key_t key, uint64_t hash,
                         uint32_t *out_idx) {
  size_t mask = s->config->slots - 1;
  for (size_t i = 0; i < s->config->slots; i++) {
    size_t slot = (hash + i) & mask;
    if (!slot_occupied(&s->slot_map[slot]))
      return 0;
    uint32_t index = slot_index(&s->slot_map[slot]);
#ifndef BISHFPM_USS_PAIR64
    if (s->slot_map[slot].hash != hash)
      continue;
#endif
    if (entry_matches(&s->heap[index], key)) {
      if (out_idx)
        *out_idx = index;
      return 1;
    }
  }
  return 0;
}
static void slot_map_set(uss_store_t *s, uint64_t hash, uint32_t index) {
  size_t mask = s->config->slots - 1;
  for (size_t i = 0; i < s->config->slots; i++) {
    size_t slot = (hash + i) & mask;
    if (!slot_occupied(&s->slot_map[slot])) {
#ifndef BISHFPM_USS_PAIR64
      s->slot_map[slot].hash = hash;
      s->slot_map[slot].occupied = true;
#endif
      slot_index_set(&s->slot_map[slot], index);
      return;
    }
  }
}
/* During hole moves, locate by original index before moving the row. Hash
 * derivation from numeric rows is used only while the index is consistent. */
static size_t slot_for_index(const uss_store_t *s, uint32_t index) {
  size_t mask = s->config->slots - 1;
  size_t slot = entry_hash(&s->heap[index]) & mask;
  while (!slot_occupied(&s->slot_map[slot]) ||
         slot_index(&s->slot_map[slot]) != index)
    slot = (slot + 1) & mask;
  return slot;
}
static void slot_map_remove(uss_store_t *s, uint32_t index) {
  size_t mask = s->config->slots - 1, j = slot_for_index(s, index);
  for (size_t k = (j + 1) & mask; slot_occupied(&s->slot_map[k]);
       k = (k + 1) & mask) {
    size_t orig = slot_hash(s, k) & mask;
    if (((k - orig) & mask) >= ((k - j) & mask)) {
      s->slot_map[j] = s->slot_map[k];
      j = k;
    }
  }
  s->slot_map[j] = (uss_slot_t){0};
}
static void slot_map_rebuild(uss_store_t *s) {
  memset(s->slot_map, 0, s->config->slots * sizeof(*s->slot_map));
  for (uint32_t i = 0; i < s->heap_size; i++)
    slot_map_set(s, entry_hash(&s->heap[i]), i);
}

/* compute decayed item rate */
static inline double item_rate(const uss_entry_t *item, uint64_t now_ms,
                               double half_life_ms) {
  return decay_since(item->rate, item->last_time_ms, now_ms, half_life_ms);
}

/* sift up in min-heap */
static void heapify_up(uss_store_t *topk, uint32_t idx, uint64_t now_ms) {
  if (!idx)
    return;
  double half = topk->config->half_life_ms;
  uint32_t parent = (idx - 1) / 2;
  double rate = item_rate(&topk->heap[idx], now_ms, half);
  if (!(rate < item_rate(&topk->heap[parent], now_ms, half)))
    return;
  uss_entry_t held = topk->heap[idx];
  size_t held_slot = slot_for_index(topk, idx);
  uint32_t hole = idx;
  for (;;) {
    size_t slot = slot_for_index(topk, parent);
    topk->heap[hole] = topk->heap[parent];
    slot_index_set(&topk->slot_map[slot], hole);
    hole = parent;
    if (!hole)
      break;
    parent = (hole - 1) / 2;
    if (!(rate < item_rate(&topk->heap[parent], now_ms, half)))
      break;
  }
  topk->heap[hole] = held;
  slot_index_set(&topk->slot_map[held_slot], hole);
}

/* sift down in min-heap */
static void heapify_down(uss_store_t *topk, uint32_t idx, uint64_t now_ms) {
  uint32_t child = 2 * idx + 1;
  if (child >= topk->heap_size)
    return;
  double half = topk->config->half_life_ms;
  double held_rate = item_rate(&topk->heap[idx], now_ms, half);
  double child_rate = item_rate(&topk->heap[child], now_ms, half);
  if (child + 1 < topk->heap_size) {
    double right_rate = item_rate(&topk->heap[child + 1], now_ms, half);
    if (right_rate < child_rate) {
      child++;
      child_rate = right_rate;
    }
  }
  if (!(child_rate < held_rate))
    return;
  /* Carry the updated entry through a hole instead of swapping two complete
   * pattern buffers at every level. Probe chains do not change, only indices.
   * Strict comparisons retain the existing tie and enumeration behavior. */
  uss_entry_t held = topk->heap[idx];
  size_t held_slot = slot_for_index(topk, idx);
  uint32_t hole = idx;
  for (;;) {
    size_t child_slot = slot_for_index(topk, child);
    topk->heap[hole] = topk->heap[child];
    slot_index_set(&topk->slot_map[child_slot], hole);
    hole = child;
    child = 2 * hole + 1;
    if (child >= topk->heap_size)
      break;
    child_rate = item_rate(&topk->heap[child], now_ms, half);
    if (child + 1 < topk->heap_size) {
      double right_rate = item_rate(&topk->heap[child + 1], now_ms, half);
      if (right_rate < child_rate) {
        child++;
        child_rate = right_rate;
      }
    }
    if (!(child_rate < held_rate))
      break;
  }
  topk->heap[hole] = held;
  slot_index_set(&topk->slot_map[held_slot], hole);
}

static bishfpm_store_count_t query_key(const void *raw, uss_key_t key,
                                       size_t length, uint64_t hash,
                                       uint64_t now) {
  (void)length;
  const uss_store_t *s = raw;
  now = effective_time(now, s->max_time_ms);
  uint32_t idx;
  bool resident = slot_map_find(s, key, hash, &idx);
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
static bishfpm_store_count_t update_key(void *raw, uss_key_t pattern,
                                        size_t length, uint64_t hash,
                                        double weight, uint64_t now) {
  uss_store_t *s = raw;
  now = effective_time(now, s->max_time_ms);
  if (!weight)
    return query_key(s, pattern, length, hash, now);
  bishfpm_store_count_t r = {0};
  double half = s->config->half_life_ms;
  double total = decay_since(s->total, s->max_time_ms, now, half);
  uint32_t idx, k = s->config->options.capacity;
  bool present = slot_map_find(s, pattern, hash, &idx);
  bool full = s->heap_size == k;
  if (!present)
    idx = full ? 0 : s->heap_size;
  double old = (present || full) ? item_rate(&s->heap[idx], now, half) : 0;
  /* Reject before clocks, counters, identities, or RNG change. The local
   * check also protects against rounding differences in lazy total decay. */
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
  /* Even when the incoming identity loses, its weight increases the old
   * minimum. E[new estimate]=w; E[old estimate]=old in exact arithmetic. */
  if (!present && retain) {
    if (full)
      slot_map_remove(s, idx);
    else
      s->heap_size++;
    entry_identity(&s->heap[idx], pattern, length, hash);
    slot_map_set(s, hash, idx);
  }
  /* A losing incoming identity contributes sampled mass, not evidence for
   * the retained identity. Admission starts fresh, never with victim mass. */
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
  uss_store_t *s = raw;
  double half = s->config->half_life_ms;
  (void)cutoff; /* Discarding positive sampled mass would bias estimates. */
  now = effective_time(now, s->max_time_ms);
  s->total = decay_since(s->total, s->max_time_ms, now, half);
  s->max_time_ms = now;
  uint32_t i = 0;
  size_t removed = 0;
  while (i < s->heap_size) {
    if (item_rate(&s->heap[i], now, half) == 0) {
      s->heap[i] = s->heap[--s->heap_size];
      removed++;
    } else
      i++;
  }
  if (removed) {
    slot_map_rebuild(s);
    for (uint32_t j = s->heap_size / 2; j > 0; j--)
      heapify_down(s, j - 1, now);
  }
  return removed;
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
    prepare,
    complete,
    complete,
    complete,
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
