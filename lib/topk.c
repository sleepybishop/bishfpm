#include "bishfpm_hash.h"
#include "bishfpm_rate.h"
#include "bishfpm_topk.h"
#include "internal.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* find slot in hash map */
static int slot_map_find(const bishfpm_topk_t *topk, const char *pattern,
                         uint64_t hash, uint32_t *out_idx) {
  size_t mask = topk->slot_map_cap - 1;
  size_t idx = (size_t)(hash & mask);

  for (size_t i = 0; i < topk->slot_map_cap; i++) {
    size_t s = (idx + i) & mask;
    if (!topk->slot_map[s].occupied) {
      return 0;
    }
    if (topk->slot_map[s].hash == hash) {
      uint32_t h_idx = topk->slot_map[s].heap_idx;
      if (strcmp(topk->heap[h_idx].pattern, pattern) == 0) {
        if (out_idx) {
          *out_idx = h_idx;
        }
        return 1;
      }
    }
  }
  return 0;
}

/* set slot in hash map */
static void slot_map_set(bishfpm_topk_t *topk, uint64_t hash,
                         uint32_t heap_idx) {
  size_t mask = topk->slot_map_cap - 1;
  size_t idx = (size_t)(hash & mask);

  for (size_t i = 0; i < topk->slot_map_cap; i++) {
    size_t s = (idx + i) & mask;
    if (!topk->slot_map[s].occupied) {
      topk->slot_map[s].hash = hash;
      topk->slot_map[s].heap_idx = heap_idx;
      topk->slot_map[s].occupied = true;
      return;
    }
  }
}

/* remove slot from hash map using backward shift */
static void slot_map_remove(bishfpm_topk_t *topk, uint64_t hash,
                            uint32_t heap_idx) {
  size_t mask = topk->slot_map_cap - 1;
  size_t idx = (size_t)(hash & mask);
  size_t s = idx;

  for (size_t i = 0; i < topk->slot_map_cap; i++) {
    s = (idx + i) & mask;
    if (!topk->slot_map[s].occupied) {
      return;
    }
    if (topk->slot_map[s].hash == hash &&
        topk->slot_map[s].heap_idx == heap_idx) {
      break;
    }
  }

  if (!topk->slot_map[s].occupied || topk->slot_map[s].hash != hash ||
      topk->slot_map[s].heap_idx != heap_idx) {
    return;
  }

  /* backward shift deletion */
  size_t j = s;
  for (size_t i = 1; i < topk->slot_map_cap; i++) {
    size_t k = (s + i) & mask;
    if (!topk->slot_map[k].occupied) {
      break;
    }
    size_t orig = (size_t)(topk->slot_map[k].hash & mask);
    bool can_move = false;
    if (j <= k) {
      can_move = (orig <= j || orig > k);
    } else {
      can_move = (orig <= j && orig > k);
    }
    if (can_move) {
      topk->slot_map[j] = topk->slot_map[k];
      j = k;
    }
  }
  topk->slot_map[j].occupied = false;
  topk->slot_map[j].hash = 0;
  topk->slot_map[j].heap_idx = 0;
}

/* Locate by old heap position as well as hash, before either item moves. */
static size_t slot_for_index(const bishfpm_topk_t *topk, uint32_t index) {
  size_t slot = topk->heap[index].hash & (topk->slot_map_cap - 1);
  while (!topk->slot_map[slot].occupied ||
         topk->slot_map[slot].heap_idx != index)
    slot = (slot + 1) & (topk->slot_map_cap - 1);
  return slot;
}

static void slot_map_rebuild(bishfpm_topk_t *topk) {
  memset(topk->slot_map, 0, topk->slot_map_cap * sizeof(*topk->slot_map));
  for (uint32_t i = 0; i < topk->heap_size; i++)
    slot_map_set(topk, topk->heap[i].hash, i);
}

/* Compute a counter at the common heap time. */
static inline double item_rate(const bishfpm_heap_entry_t *item,
                               uint64_t now_ms, double half_life_ms) {
  if (now_ms <= item->last_time_ms) {
    return item->rate;
  }
  return bishfpm_rate_decay(item->rate, now_ms - item->last_time_ms,
                            half_life_ms);
}

/* sift up in min-heap */
static void heapify_up(bishfpm_topk_t *topk, uint32_t idx, uint64_t now_ms) {
  bishfpm_heap_entry_t held = topk->heap[idx];
  size_t held_slot = slot_for_index(topk, idx);
  double rate = item_rate(&held, now_ms, topk->half_life_ms);
  uint32_t hole = idx;
  while (hole > 0) {
    uint32_t parent = (hole - 1) / 2;
    if (!(rate < item_rate(&topk->heap[parent], now_ms, topk->half_life_ms)))
      break;
    size_t slot = slot_for_index(topk, parent);
    topk->heap[hole] = topk->heap[parent];
    topk->slot_map[slot].heap_idx = hole;
    hole = parent;
  }
  topk->heap[hole] = held;
  topk->slot_map[held_slot].heap_idx = hole;
}

/* sift down in min-heap */
static void heapify_down(bishfpm_topk_t *topk, uint32_t idx, uint64_t now_ms) {
  uint32_t child = 2 * idx + 1;
  if (child >= topk->heap_size)
    return;
  double held_rate = item_rate(&topk->heap[idx], now_ms, topk->half_life_ms);
  double child_rate = item_rate(&topk->heap[child], now_ms, topk->half_life_ms);
  if (child + 1 < topk->heap_size) {
    double right_rate =
        item_rate(&topk->heap[child + 1], now_ms, topk->half_life_ms);
    if (right_rate < child_rate) {
      child++;
      child_rate = right_rate;
    }
  }
  /* Most resident updates do not move. Avoid copying the full pattern buffer
   * or probing its map slot until the first strict comparison requires it. */
  if (!(child_rate < held_rate))
    return;
  /* Carry the updated entry through a hole instead of swapping two complete
   * pattern buffers at every level. Probe chains do not change, only indices.
   * Strict comparisons retain the existing tie and enumeration behavior. */
  bishfpm_heap_entry_t held = topk->heap[idx];
  size_t held_slot = slot_for_index(topk, idx);
  uint32_t hole = idx;
  for (;;) {
    size_t child_slot = slot_for_index(topk, child);
    topk->heap[hole] = topk->heap[child];
    topk->slot_map[child_slot].heap_idx = hole;
    hole = child;
    child = 2 * hole + 1;
    if (child >= topk->heap_size)
      break;
    child_rate = item_rate(&topk->heap[child], now_ms, topk->half_life_ms);
    if (child + 1 < topk->heap_size) {
      double right_rate =
          item_rate(&topk->heap[child + 1], now_ms, topk->half_life_ms);
      if (right_rate < child_rate) {
        child++;
        child_rate = right_rate;
      }
    }
    if (!(child_rate < held_rate))
      break;
  }
  topk->heap[hole] = held;
  topk->slot_map[held_slot].heap_idx = hole;
}

static void topk_init(bishfpm_topk_t *topk, uint32_t k, double half_life_ms,
                      size_t slot_cap, bool arena_backed) {
  topk->k = k;
  topk->half_life_ms = half_life_ms;
  topk->slot_map_cap = slot_cap;
  topk->arena_backed = arena_backed;
}

static bishfpm_topk_t *topk_new(bishfpm_arena_t *a, uint32_t k, double half,
                                uint32_t width, uint64_t seed) {
  topk_layout_t layout;
  if (!topk_layout_configured(k, half, width, &layout))
    return NULL;
  uint8_t *saved = a ? a->beg : NULL;
  bishfpm_topk_t *topk =
      a ? bishfpm_alloc_array(a, bishfpm_topk_t, 1) : calloc(1, sizeof(*topk));
  if (!topk)
    return NULL;
  topk_init(topk, k, half, layout.slot_cap, a != NULL);
  topk->hash_seed = seed;
  topk->sketch_width = width;
  topk->heap = a ? bishfpm_alloc_array(a, bishfpm_heap_entry_t, k)
                 : calloc(1, layout.heap_bytes);
  topk->slot_map =
      a ? bishfpm_alloc_array(a, bishfpm_slot_entry_t, layout.slot_cap)
        : calloc(1, layout.map_bytes);
  if (width)
    topk->sketch = a ? bishfpm_alloc_array(a, bishfpm_sketch_cell_t, width)
                     : calloc(1, layout.sketch_bytes);
  if (!topk->heap || !topk->slot_map || (width && !topk->sketch)) {
    if (a)
      a->beg = saved;
    else
      bishfpm_topk_free(topk);
    return NULL;
  }
  return topk;
}

bishfpm_topk_t *bishfpm_topk_new_arena(bishfpm_arena_t *a, uint32_t k,
                                       double half) {
  return a ? topk_new(a, k, half, 0, 0) : NULL;
}

bishfpm_topk_t *bishfpm_topk_new(uint32_t k, double half) {
  return topk_new(NULL, k, half, 0, 0);
}

bishfpm_topk_t *bishfpm_topk_new_fss(uint32_t k, double half, uint32_t width,
                                     uint64_t seed) {
  return width ? topk_new(NULL, k, half, width, seed) : NULL;
}

bishfpm_topk_t *bishfpm_topk_new_fss_arena(bishfpm_arena_t *a, uint32_t k,
                                           double half, uint32_t width,
                                           uint64_t seed) {
  return a && width ? topk_new(a, k, half, width, seed) : NULL;
}

/* free top-k structure */
void bishfpm_topk_free(bishfpm_topk_t *topk) {
  if (!topk) {
    return;
  }
  if (!topk->arena_backed) {
    free(topk->sketch);
    free(topk->heap);
    free(topk->slot_map);
    free(topk);
  }
}

/* reset top-k state without reallocating */
void bishfpm_topk_reset(bishfpm_topk_t *topk) {
  if (!topk) {
    return;
  }
  memset(topk->heap, 0, topk->k * sizeof(bishfpm_heap_entry_t));
  memset(topk->slot_map, 0, topk->slot_map_cap * sizeof(bishfpm_slot_entry_t));
  topk->heap_size = 0;
  topk->max_time_ms = 0;
  topk->omitted_bound = 0;
  topk->sketch_total = 0;
  topk->history_started = false;
  if (topk->sketch)
    memset(topk->sketch, 0, topk->sketch_width * sizeof(*topk->sketch));
}

int bishfpm_topk_try_set_seed(bishfpm_topk_t *topk, uint64_t seed) {
  if (!topk ||
      (topk->sketch_width && topk->history_started && seed != topk->hash_seed))
    return 0;
  topk->hash_seed = seed;
  for (uint32_t i = 0; i < topk->heap_size; i++)
    topk->heap[i].hash = bishfpm_hash_str(topk->heap[i].pattern, seed);
  slot_map_rebuild(topk);
  return 1;
}

void bishfpm_topk_set_seed(bishfpm_topk_t *topk, uint64_t seed) {
  (void)bishfpm_topk_try_set_seed(topk, seed);
}

static size_t sketch_bucket(const bishfpm_topk_t *t, uint64_t hash) {
  return bishfpm_splitmix64(&hash) % t->sketch_width;
}

/* Stored counts are finite and nonnegative. Match rate_decay exactly while
 * avoiding its out-of-line call for the many updates at one event watermark. */
static inline double decay_since(double count, uint64_t then, uint64_t now,
                                 double half) {
  return now == then ? count : bishfpm_rate_decay(count, now - then, half);
}

static double sketch_bound(const bishfpm_topk_t *t, uint64_t hash,
                           uint64_t now) {
  const bishfpm_sketch_cell_t *cell = &t->sketch[sketch_bucket(t, hash)];
  return decay_since(cell->count, cell->time_ms, now, t->half_life_ms);
}

static void sketch_record(bishfpm_topk_t *t, uint64_t hash, double upper,
                          uint64_t now) {
  bishfpm_sketch_cell_t *cell = &t->sketch[sketch_bucket(t, hash)];
  cell->count = fmax(
      upper, decay_since(cell->count, cell->time_ms, now, t->half_life_ms));
  cell->time_ms = now;
}

/* Same bucket-history invariants and staged guards as numeric FSS. */
static topk_result_t fss_add(bishfpm_topk_t *t, const char *pattern,
                             size_t length, double increment,
                             uint64_t timestamp) {
  topk_result_t r = {0};
  uint64_t now = effective_time(timestamp, t->max_time_ms);
  uint64_t hash = bishfpm_murmur3_64(pattern, length, t->hash_seed);
  double weight = decay_since(increment, timestamp, now, t->half_life_ms);
  double total =
      decay_since(t->sketch_total, t->max_time_ms, now, t->half_life_ms);
  if (weight > DBL_MAX - total)
    return r;
  uint32_t idx;
  bool present = slot_map_find(t, pattern, hash, &idx);
  bool full = t->heap_size == t->k;
  double alpha = present ? 0 : sketch_bound(t, hash, now);
  double minimum =
      full && !present ? item_rate(&t->heap[0], now, t->half_life_ms) : 0;
  double error = alpha;
  double rate = present ? item_rate(&t->heap[idx], now, t->half_life_ms) : 0;
  if (!present && full && alpha + weight >= minimum &&
      sketch_bucket(t, hash) == sketch_bucket(t, t->heap[0].hash))
    error = fmax(alpha, minimum);
  if (weight > DBL_MAX - (present ? rate : error))
    return r;
  t->omitted_bound =
      decay_since(t->omitted_bound, t->max_time_ms, now, t->half_life_ms);
  t->max_time_ms = now;
  t->sketch_total = total + weight;
  t->history_started = true;
  r.accepted = true;
  r.hash = hash;
  r.time_ms = now;
  if (present) {
    bishfpm_heap_entry_t *entry = &t->heap[idx];
    entry->error =
        decay_since(entry->error, entry->last_time_ms, now, t->half_life_ms);
    entry->rate = rate + weight;
    entry->last_time_ms = now;
    r.present = r.was_present = true;
    r.upper = entry->rate;
    r.lower = fmax(0, entry->rate - entry->error);
    r.error = r.upper - r.lower;
    heapify_down(t, idx, now);
    return r;
  }
  if (full && alpha + weight < minimum) {
    bishfpm_sketch_cell_t *cell = &t->sketch[sketch_bucket(t, hash)];
    cell->count = alpha + weight;
    cell->time_ms = now;
    r.upper = r.error = cell->count;
    return r;
  }
  if (full) {
    sketch_record(t, t->heap[0].hash, minimum, now);
    slot_map_remove(t, t->heap[0].hash, 0);
    idx = 0;
  } else {
    idx = t->heap_size++;
  }
  bishfpm_heap_entry_t *entry = &t->heap[idx];
  memcpy(entry->pattern, pattern, length + 1);
  entry->hash = hash;
  entry->rate = error + weight;
  entry->error = error;
  entry->last_time_ms = now;
  slot_map_set(t, hash, idx);
  r.present = true;
  r.replaced = full;
  r.upper = entry->rate;
  r.lower = fmax(0, r.upper - error);
  r.error = r.upper - r.lower;
  if (full)
    heapify_down(t, idx, now);
  else
    heapify_up(t, idx, now);
  return r;
}

/* add item with increment/weight */
topk_result_t topk_add_result(bishfpm_topk_t *topk, const char *pattern,
                              double increment, uint64_t now_ms) {
  topk_result_t result = {0};
  if (!topk || !pattern || !pattern[0] || !isfinite(increment) ||
      increment < 0.0)
    return result;
  size_t length = strnlen(pattern, BISHFPM_TOPK_MAX_STR);
  if (length >= BISHFPM_TOPK_MAX_STR)
    return result;
  return topk_add_sized(topk, pattern, length, increment, now_ms);
}

/* The engine supplies a validated canonical pattern and its known length. */
topk_result_t topk_add_sized(bishfpm_topk_t *topk, const char *pattern,
                             size_t length, double increment, uint64_t now_ms) {
  topk_result_t result = {0};
  if (increment == 0.0)
    return topk_query_result(topk, pattern, now_ms);
  if (topk->sketch_width)
    return fss_add(topk, pattern, length, increment, now_ms);
  uint64_t event_time = now_ms;
  now_ms = effective_time(now_ms, topk->max_time_ms);
  /* Stage clock and history changes until the count can be represented. */
  double omitted_bound = bishfpm_rate_decay(
      topk->omitted_bound, now_ms - topk->max_time_ms, topk->half_life_ms);
  double weight =
      bishfpm_rate_decay(increment, now_ms - event_time, topk->half_life_ms);
  uint64_t hash = bishfpm_murmur3_64(pattern, length, topk->hash_seed);
  result.time_ms = now_ms;
  result.hash = hash;
  uint32_t idx;
  if (slot_map_find(topk, pattern, hash, &idx)) {
    result.was_present = true;
    bishfpm_heap_entry_t *item = &topk->heap[idx];
    double decayed = item_rate(item, now_ms, topk->half_life_ms);
    if (weight > DBL_MAX - decayed)
      return result;
    topk->omitted_bound = omitted_bound;
    topk->max_time_ms = now_ms;
    result.accepted = true;
    item->error = bishfpm_rate_decay(item->error, now_ms - item->last_time_ms,
                                     topk->half_life_ms);
    item->rate = decayed + weight;
    item->last_time_ms = now_ms;
    result.present = true;
    result.upper = item->rate;
    result.lower = fmax(0, item->rate - item->error);
    result.error = result.upper - result.lower;
    heapify_down(topk, idx, now_ms);
    return result;
  }
  bool replacing = topk->heap_size == topk->k;
  double admission = replacing
                         ? item_rate(&topk->heap[0], now_ms, topk->half_life_ms)
                         : omitted_bound;
  if (weight > DBL_MAX - admission)
    return result;
  topk->omitted_bound = omitted_bound;
  topk->max_time_ms = now_ms;
  result.accepted = true;
  double error = admission;
  if (topk->heap_size < topk->k) {
    idx = topk->heap_size++;
  } else {
    idx = 0;
    slot_map_remove(topk, topk->heap[0].hash, 0);
  }
  bishfpm_heap_entry_t *item = &topk->heap[idx];
  memcpy(item->pattern, pattern, length + 1);
  item->hash = hash;
  item->rate = error + weight;
  item->error = error;
  item->last_time_ms = now_ms;
  result.present = true;
  result.replaced = replacing;
  result.upper = item->rate;
  result.lower = fmax(0, item->rate - item->error);
  result.error = result.upper - result.lower;
  slot_map_set(topk, hash, idx);
  if (replacing)
    heapify_down(topk, idx, now_ms);
  else
    heapify_up(topk, idx, now_ms);
  return result;
}

double bishfpm_topk_add_weight(bishfpm_topk_t *topk, const char *pattern,
                               double increment, uint64_t now_ms) {
  return topk_add_result(topk, pattern, increment, now_ms).upper;
}

double bishfpm_topk_add(bishfpm_topk_t *topk, const char *pattern,
                        uint32_t increment, uint64_t now_ms) {
  return bishfpm_topk_add_weight(topk, pattern, increment, now_ms);
}

/* update item in top-k heap (increment 1) */
double bishfpm_topk_update(bishfpm_topk_t *topk, const char *pattern,
                           uint64_t now_ms) {
  return bishfpm_topk_add(topk, pattern, 1, now_ms);
}

topk_result_t topk_query_result(const bishfpm_topk_t *topk, const char *pattern,
                                uint64_t now_ms) {
  topk_result_t result = {0};
  if (!topk || !pattern || !pattern[0])
    return result;
  size_t length = strnlen(pattern, BISHFPM_TOPK_MAX_STR);
  if (length >= BISHFPM_TOPK_MAX_STR)
    return result;
  result.time_ms = effective_time(now_ms, topk->max_time_ms);
  result.hash = bishfpm_murmur3_64(pattern, length, topk->hash_seed);
  uint32_t idx;
  if (!slot_map_find(topk, pattern, result.hash, &idx)) {
    if (topk->sketch_width)
      result.upper = result.error =
          sketch_bound(topk, result.hash, result.time_ms);
    return result;
  }
  const bishfpm_heap_entry_t *item = &topk->heap[idx];
  result.present = result.was_present = true;
  result.upper = item_rate(item, result.time_ms, topk->half_life_ms);
  result.lower = bishfpm_rate_decay(fmax(0, item->rate - item->error),
                                    result.time_ms - item->last_time_ms,
                                    topk->half_life_ms);
  result.error = result.upper - result.lower;
  return result;
}

int bishfpm_topk_bounds(const bishfpm_topk_t *topk, const char *pattern,
                        uint64_t now, bishfpm_topk_bounds_t *out) {
  if (!topk || !pattern || !out || !pattern[0] ||
      strnlen(pattern, BISHFPM_TOPK_MAX_STR) >= BISHFPM_TOPK_MAX_STR)
    return 0;
  topk_result_t r = topk_query_result(topk, pattern, now);
  *out = (bishfpm_topk_bounds_t){r.lower, r.upper, r.error, r.present,
                                 r.present || topk->sketch_width != 0};
  return 1;
}

double bishfpm_topk_query_lower(bishfpm_topk_t *topk, const char *pattern,
                                uint64_t now_ms) {
  return topk_query_result(topk, pattern, now_ms).lower;
}

double bishfpm_topk_query(bishfpm_topk_t *topk, const char *pattern,
                          uint64_t now_ms) {
  return topk_query_result(topk, pattern, now_ms).upper;
}

/* query current minimum rate in top-k heap */
double bishfpm_topk_min_rate(const bishfpm_topk_t *topk, uint64_t now_ms) {
  if (!topk || topk->heap_size == 0) {
    return 0.0;
  }
  if (now_ms < topk->max_time_ms)
    now_ms = topk->max_time_ms;
  return item_rate(&topk->heap[0], now_ms, topk->half_life_ms);
}

/* get number of active items in heap */
uint32_t bishfpm_topk_count(const bishfpm_topk_t *topk) {
  return topk ? topk->heap_size : 0;
}

size_t bishfpm_topk_storage_bytes(const bishfpm_topk_t *topk) {
  return topk ? sizeof(*topk) + topk->k * sizeof(*topk->heap) +
                    topk->slot_map_cap * sizeof(*topk->slot_map) +
                    topk->sketch_width * sizeof(*topk->sketch)
              : 0;
}

/* prune stale entries whose rate is below min_rate */
size_t bishfpm_topk_prune(bishfpm_topk_t *topk, uint64_t now_ms,
                          double min_rate) {
  if (!topk || topk->heap_size == 0) {
    return 0;
  }

  if (!isfinite(min_rate) || min_rate <= 0)
    return 0;
  if (now_ms < topk->max_time_ms)
    now_ms = topk->max_time_ms;
  topk->omitted_bound = bishfpm_rate_decay(
      topk->omitted_bound, now_ms - topk->max_time_ms, topk->half_life_ms);
  topk->sketch_total = bishfpm_rate_decay(
      topk->sketch_total, now_ms - topk->max_time_ms, topk->half_life_ms);
  topk->max_time_ms = now_ms;
  size_t pruned = 0;
  uint32_t i = 0;
  while (i < topk->heap_size) {
    double r = item_rate(&topk->heap[i], now_ms, topk->half_life_ms);
    if (r < min_rate) {
      if (r > topk->omitted_bound)
        topk->omitted_bound = r;
      if (topk->sketch_width)
        sketch_record(topk, topk->heap[i].hash, r, now_ms);
      /* move last element into this position */
      uint32_t last = topk->heap_size - 1;
      if (i != last) {
        topk->heap[i] = topk->heap[last];
      }
      topk->heap_size--;
      pruned++;
    } else {
      i++;
    }
  }

  if (!pruned)
    return 0;
  slot_map_rebuild(topk);
  for (uint32_t j = topk->heap_size / 2; j > 0; j--)
    heapify_down(topk, j - 1, now_ms);
  return pruned;
}

/* comparator for sorting heap items descending by decayed rate */
static int cmp_heap_items_desc(const void *a, const void *b) {
  const bishfpm_topk_item_t *ia = (const bishfpm_topk_item_t *)a;
  const bishfpm_topk_item_t *ib = (const bishfpm_topk_item_t *)b;
  if (ia->rate > ib->rate) {
    return -1;
  }
  if (ia->rate < ib->rate) {
    return 1;
  }
  return strcmp(ia->pattern, ib->pattern);
}

/* get current top-k items sorted by rate descending */
size_t bishfpm_topk_get_top(bishfpm_topk_t *topk, uint64_t now_ms,
                            bishfpm_topk_item_t *out_items, size_t max_items) {
  if (!topk || !out_items || max_items == 0 || topk->heap_size == 0) {
    return 0;
  }

  if (now_ms < topk->max_time_ms)
    now_ms = topk->max_time_ms;
  bishfpm_topk_item_t *all =
      malloc(topk->heap_size * sizeof(bishfpm_topk_item_t));
  if (!all) {
    return 0;
  }

  for (size_t i = 0; i < topk->heap_size; i++) {
    memset(&all[i], 0, sizeof(all[i]));
    memcpy(all[i].pattern, topk->heap[i].pattern,
           strlen(topk->heap[i].pattern) + 1);
    all[i].error = topk->heap[i].error;
    all[i].last_time_ms = topk->heap[i].last_time_ms;
    all[i].rate = item_rate(&topk->heap[i], now_ms, topk->half_life_ms);
    all[i].error = bishfpm_rate_decay(
        all[i].error, now_ms - all[i].last_time_ms, topk->half_life_ms);
    all[i].last_time_ms = now_ms;
  }

  qsort(all, topk->heap_size, sizeof(bishfpm_topk_item_t), cmp_heap_items_desc);

  size_t count = topk->heap_size < max_items ? topk->heap_size : max_items;
  memcpy(out_items, all, count * sizeof(bishfpm_topk_item_t));
  free(all);

  return count;
}
