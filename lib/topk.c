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
  /* Carry the updated entry through a hole instead of swapping two complete
   * pattern buffers at every level. Probe chains do not change, only indices.
   * Strict comparisons retain the existing tie and enumeration behavior. */
  bishfpm_heap_entry_t held = topk->heap[idx];
  size_t held_slot = slot_for_index(topk, idx);
  double held_rate = item_rate(&held, now_ms, topk->half_life_ms);
  uint32_t hole = idx;
  while (2 * hole + 1 < topk->heap_size) {
    uint32_t child = 2 * hole + 1;
    double child_rate =
        item_rate(&topk->heap[child], now_ms, topk->half_life_ms);
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
    size_t child_slot = slot_for_index(topk, child);
    topk->heap[hole] = topk->heap[child];
    topk->slot_map[child_slot].heap_idx = hole;
    hole = child;
  }
  if (hole != idx) {
    topk->heap[hole] = held;
    topk->slot_map[held_slot].heap_idx = hole;
  }
}

static void topk_init(bishfpm_topk_t *topk, uint32_t k, double half_life_ms,
                      size_t slot_cap, bool arena_backed) {
  topk->k = k;
  topk->half_life_ms = half_life_ms;
  topk->slot_map_cap = slot_cap;
  topk->arena_backed = arena_backed;
}

/* Owned and borrowed allocation paths share layout and initialization. */
bishfpm_topk_t *bishfpm_topk_new_arena(bishfpm_arena_t *a, uint32_t k,
                                       double half_life_ms) {
  topk_layout_t layout;
  if (!a || !topk_layout(k, half_life_ms, &layout))
    return NULL;
  uint8_t *saved = a->beg;
  bishfpm_topk_t *topk = bishfpm_alloc_array(a, bishfpm_topk_t, 1);
  if (topk) {
    topk_init(topk, k, half_life_ms, layout.slot_cap, true);
    topk->heap = bishfpm_alloc_array(a, bishfpm_heap_entry_t, k);
    topk->slot_map =
        bishfpm_alloc_array(a, bishfpm_slot_entry_t, layout.slot_cap);
    if (topk->heap && topk->slot_map)
      return topk;
  }
  a->beg = saved;
  return NULL;
}

bishfpm_topk_t *bishfpm_topk_new(uint32_t k, double half_life_ms) {
  topk_layout_t layout;
  if (!topk_layout(k, half_life_ms, &layout))
    return NULL;
  bishfpm_topk_t *topk = calloc(1, sizeof(*topk));
  if (!topk)
    return NULL;
  topk_init(topk, k, half_life_ms, layout.slot_cap, false);
  topk->heap = calloc(1, layout.heap_bytes);
  topk->slot_map = calloc(1, layout.map_bytes);
  if (!topk->heap || !topk->slot_map) {
    bishfpm_topk_free(topk);
    return NULL;
  }
  return topk;
}

/* free top-k structure */
void bishfpm_topk_free(bishfpm_topk_t *topk) {
  if (!topk) {
    return;
  }
  if (!topk->arena_backed) {
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
}

/* set hash seed */
void bishfpm_topk_set_seed(bishfpm_topk_t *topk, uint64_t seed) {
  if (topk) {
    topk->hash_seed = seed;
    for (uint32_t i = 0; i < topk->heap_size; i++)
      topk->heap[i].hash = bishfpm_hash_str(topk->heap[i].pattern, seed);
    slot_map_rebuild(topk);
  }
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
  if (!slot_map_find(topk, pattern, result.hash, &idx))
    return result;
  const bishfpm_heap_entry_t *item = &topk->heap[idx];
  result.present = result.was_present = true;
  result.upper = item_rate(item, result.time_ms, topk->half_life_ms);
  result.lower = bishfpm_rate_decay(fmax(0, item->rate - item->error),
                                    result.time_ms - item->last_time_ms,
                                    topk->half_life_ms);
  result.error = result.upper - result.lower;
  return result;
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
                    topk->slot_map_cap * sizeof(*topk->slot_map)
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
  topk->max_time_ms = now_ms;
  size_t pruned = 0;
  uint32_t i = 0;
  while (i < topk->heap_size) {
    double r = item_rate(&topk->heap[i], now_ms, topk->half_life_ms);
    if (r < min_rate) {
      if (r > topk->omitted_bound)
        topk->omitted_bound = r;
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
