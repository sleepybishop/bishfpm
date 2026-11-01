#include "compact.h"
#include <float.h>
#include <stdlib.h>
#include <string.h>

static int slot_map_find(const compact_t *store, uint64_t key, uint64_t hash,
                         uint32_t *out_idx) {
  size_t mask = store->slot_map_cap - 1;
  size_t idx = (size_t)(hash & mask);

  for (size_t i = 0; i < store->slot_map_cap; i++) {
    size_t s = (idx + i) & mask;
    if (!store->slot_map[s]) {
      return 0;
    }
    uint32_t h_idx = store->slot_map[s] - 1;
    if (store->heap[h_idx].hash == hash) {
      if (store->heap[h_idx].key == key) {
        if (out_idx) {
          *out_idx = h_idx;
        }
        return 1;
      }
    }
  }
  return 0;
}

static void slot_map_set(compact_t *store, uint64_t hash, uint32_t heap_idx) {
  size_t mask = store->slot_map_cap - 1;
  size_t idx = (size_t)(hash & mask);

  for (size_t i = 0; i < store->slot_map_cap; i++) {
    size_t s = (idx + i) & mask;
    if (!store->slot_map[s]) {
      store->slot_map[s] = heap_idx + 1;
      return;
    }
  }
}

static void slot_map_remove(compact_t *store, uint64_t hash,
                            uint32_t heap_idx) {
  size_t mask = store->slot_map_cap - 1;
  size_t idx = (size_t)(hash & mask);
  size_t s = idx;

  for (size_t i = 0; i < store->slot_map_cap; i++) {
    s = (idx + i) & mask;
    if (!store->slot_map[s]) {
      return;
    }
    if (store->slot_map[s] == heap_idx + 1) {
      break;
    }
  }

  if (store->slot_map[s] != heap_idx + 1) {
    return;
  }

  /* backward shift deletion */
  size_t j = s;
  for (size_t i = 1; i < store->slot_map_cap; i++) {
    size_t k = (s + i) & mask;
    if (!store->slot_map[k]) {
      break;
    }
    size_t orig = (size_t)(store->heap[store->slot_map[k] - 1].hash & mask);
    bool can_move = false;
    if (j <= k) {
      can_move = (orig <= j || orig > k);
    } else {
      can_move = (orig <= j && orig > k);
    }
    if (can_move) {
      store->slot_map[j] = store->slot_map[k];
      j = k;
    }
  }
  store->slot_map[j] = 0;
}

static size_t slot_for_index(const compact_t *store, uint32_t index) {
  size_t slot = store->heap[index].hash & (store->slot_map_cap - 1);
  while (store->slot_map[slot] != index + 1)
    slot = (slot + 1) & (store->slot_map_cap - 1);
  return slot;
}

static void slot_map_rebuild(compact_t *store) {
  memset(store->slot_map, 0, store->slot_map_cap * sizeof(*store->slot_map));
  for (uint32_t i = 0; i < store->heap_size; i++)
    slot_map_set(store, store->heap[i].hash, i);
}

static inline double item_rate(const compact_entry_t *item, uint64_t now_ms,
                               double half_life_ms) {
  return decay_since(item->rate, item->last_time_ms, now_ms, half_life_ms);
}

static void heapify_up(compact_t *store, uint32_t idx, uint64_t now_ms) {
  compact_entry_t held = store->heap[idx];
  size_t held_slot = slot_for_index(store, idx);
  double rate = item_rate(&held, now_ms, store->half_life_ms);
  uint32_t hole = idx;
  while (hole > 0) {
    uint32_t parent = (hole - 1) / 2;
    if (!(rate < item_rate(&store->heap[parent], now_ms, store->half_life_ms)))
      break;
    size_t slot = slot_for_index(store, parent);
    store->heap[hole] = store->heap[parent];
    store->slot_map[slot] = hole + 1;
    hole = parent;
  }
  store->heap[hole] = held;
  store->slot_map[held_slot] = hole + 1;
}

static void heapify_down(compact_t *store, uint32_t idx, uint64_t now_ms) {
  uint32_t child = 2 * idx + 1;
  if (child >= store->heap_size)
    return;
  double held_rate = item_rate(&store->heap[idx], now_ms, store->half_life_ms);
  double child_rate =
      item_rate(&store->heap[child], now_ms, store->half_life_ms);
  if (child + 1 < store->heap_size) {
    double right_rate =
        item_rate(&store->heap[child + 1], now_ms, store->half_life_ms);
    if (right_rate < child_rate) {
      child++;
      child_rate = right_rate;
    }
  }
  if (!(child_rate < held_rate))
    return;
  /* Move children through a hole, then place the updated entry once.
   * Repair indices as entries move; strict comparisons preserve tie order. */
  compact_entry_t held = store->heap[idx];
  size_t held_slot = slot_for_index(store, idx);
  uint32_t hole = idx;
  for (;;) {
    size_t child_slot = slot_for_index(store, child);
    store->heap[hole] = store->heap[child];
    store->slot_map[child_slot] = hole + 1;
    hole = child;
    child = 2 * hole + 1;
    if (child >= store->heap_size)
      break;
    child_rate = item_rate(&store->heap[child], now_ms, store->half_life_ms);
    if (child + 1 < store->heap_size) {
      double right_rate =
          item_rate(&store->heap[child + 1], now_ms, store->half_life_ms);
      if (right_rate < child_rate) {
        child++;
        child_rate = right_rate;
      }
    }
    if (!(child_rate < held_rate))
      break;
  }
  store->heap[hole] = held;
  store->slot_map[held_slot] = hole + 1;
}

/* Bulk heapification uses the same tie order without consulting the index.
 * Rebuild the index after all entries have reached their final positions. */
static void heapify_down_unindexed(compact_t *store, uint32_t idx,
                                   uint64_t now_ms) {
  compact_entry_t held = store->heap[idx];
  double held_rate = item_rate(&held, now_ms, store->half_life_ms);
  uint32_t hole = idx;
  while (2 * hole + 1 < store->heap_size) {
    uint32_t child = 2 * hole + 1;
    double child_rate =
        item_rate(&store->heap[child], now_ms, store->half_life_ms);
    if (child + 1 < store->heap_size) {
      double right_rate =
          item_rate(&store->heap[child + 1], now_ms, store->half_life_ms);
      if (right_rate < child_rate) {
        child++;
        child_rate = right_rate;
      }
    }
    if (!(child_rate < held_rate))
      break;
    store->heap[hole] = store->heap[child];
    hole = child;
  }
  store->heap[hole] = held;
}

size_t compact_bytes(uint32_t k, uint32_t width, bishfpm_pair_store_t kind) {
  if (!k || k > (1U << 28) || width > (1U << 24))
    return 0;
  if (kind == BISHFPM_PAIR_FSS && !width)
    return 0;
  if ((kind != BISHFPM_PAIR_FSS && width) ||
      (kind != BISHFPM_PAIR_SS && kind != BISHFPM_PAIR_THRESHOLD &&
       kind != BISHFPM_PAIR_FSS))
    return 0;
  size_t slots = next_pow2((size_t)k * 2), bytes, extra;
  if (!size_mul(k, sizeof(compact_entry_t), &extra) ||
      !size_add(sizeof(compact_t), extra, &bytes) ||
      !size_mul(slots, sizeof(uint32_t), &extra) ||
      !size_add(bytes, extra, &bytes) ||
      !size_mul((size_t)width, sizeof(bishfpm_sketch_cell_t), &extra) ||
      !size_add(bytes, extra, &bytes) || bytes > PTRDIFF_MAX)
    return 0;
  return bytes;
}
compact_t *compact_new(uint32_t k, double half, uint32_t width,
                       bishfpm_pair_store_t kind, double floor) {
  return compact_new_budget(k, half, width, kind, floor, NULL);
}
compact_t *compact_new_budget(uint32_t k, double half, uint32_t width,
                              bishfpm_pair_store_t kind, double floor,
                              bishfpm_memory_budget_t *budget) {
  if (!compact_bytes(k, width, kind))
    return NULL;
  compact_t *t = memory_calloc(budget, 1, sizeof(*t));
  if (!t)
    return NULL;
  t->budget = budget;
  t->k = k;
  t->half_life_ms = half;
  t->sketch_width = width;
  t->kind = kind;
  t->forget_below = floor;
  t->slot_map_cap = next_pow2((size_t)k * 2);
  t->heap = memory_calloc(budget, k, sizeof(*t->heap));
  if (!t->heap) {
    compact_free(t);
    return NULL;
  }
  t->slot_map = memory_calloc(budget, t->slot_map_cap, sizeof(*t->slot_map));
  if (width)
    t->sketch = memory_calloc(budget, (size_t)width, sizeof(*t->sketch));
  if (!t->slot_map || (width && !t->sketch)) {
    compact_free(t);
    return NULL;
  }
  return t;
}
void compact_free(compact_t *t) {
  if (t) {
    memory_free(t->budget, t->heap, t->k * sizeof(*t->heap));
    memory_free(t->budget, t->slot_map, t->slot_map_cap * sizeof(*t->slot_map));
    memory_free(t->budget, t->sketch, t->sketch_width * sizeof(*t->sketch));
    memory_free(t->budget, t, sizeof(*t));
  }
}

/* Each bucket bounds the decayed history of any currently absent key hashing
 * to it. Resident hits update only the heap; rejection adds weight to alpha;
 * eviction/pruning transfers the resident upper bound back with max(). */
static size_t fss_bucket(const compact_t *t, uint64_t hash) {
  return bishfpm_splitmix64(&hash) % t->sketch_width;
}
static double fss_alpha(const compact_t *t, uint64_t hash, uint64_t now) {
  const bishfpm_sketch_cell_t *cell = &t->sketch[fss_bucket(t, hash)];
  return decay_since(cell->count, cell->time_ms, now, t->half_life_ms);
}
static void fss_record(compact_t *t, uint64_t hash, double upper,
                       uint64_t now) {
  bishfpm_sketch_cell_t *cell = &t->sketch[fss_bucket(t, hash)];
  cell->count = fmax(upper, fss_alpha(t, hash, now));
  cell->time_ms = now;
}
static void fss_advance(compact_t *t, uint64_t now, double total) {
  t->omitted_bound =
      decay_since(t->omitted_bound, t->max_time_ms, now, t->half_life_ms);
  t->max_time_ms = now;
  t->sketch_total = total;
}

static topk_result_t fss_add(compact_t *t, uint64_t key, uint64_t hash,
                             double increment, uint64_t timestamp) {
  topk_result_t r = {0};
  uint64_t now = effective_time(timestamp, t->max_time_ms);
  double weight = decay_since(increment, timestamp, now, t->half_life_ms);
  double total =
      decay_since(t->sketch_total, t->max_time_ms, now, t->half_life_ms);
  if (weight > DBL_MAX - total)
    return r;

  uint32_t idx;
  if (slot_map_find(t, key, hash, &idx)) {
    compact_entry_t *entry = &t->heap[idx];
    double rate = item_rate(entry, now, t->half_life_ms);
    if (weight > DBL_MAX - rate)
      return r;

    fss_advance(t, now, total + weight);
    entry->error =
        decay_since(entry->error, entry->last_time_ms, now, t->half_life_ms);
    entry->rate = rate + weight;
    entry->last_time_ms = now;
    r.present = r.was_present = true;
    r.time_ms = now;
    r.upper = entry->rate;
    r.lower = fmax(0, entry->rate - entry->error);
    r.error = r.upper - r.lower;
    heapify_down(t, idx, now);
    return r;
  }

  bool full = t->heap_size == t->k;
  double alpha = fss_alpha(t, hash, now);
  double minimum = full ? item_rate(&t->heap[0], now, t->half_life_ms) : 0;
  double error = alpha;
  /* Eviction can raise the incoming bucket's bound. Check that value before
   * changing the clock, bucket or heap. */
  if (full && alpha + weight >= minimum &&
      fss_bucket(t, hash) == fss_bucket(t, t->heap[0].hash))
    error = fmax(alpha, minimum);
  if (weight > DBL_MAX - error)
    return r;

  fss_advance(t, now, total + weight);
  r.time_ms = now;
  if (full && alpha + weight < minimum) {
    bishfpm_sketch_cell_t *cell = &t->sketch[fss_bucket(t, hash)];
    cell->count = alpha + weight;
    cell->time_ms = now;
    r.upper = r.error = cell->count;
    return r;
  }

  if (full) {
    fss_record(t, t->heap[0].hash, minimum, now);
    slot_map_remove(t, t->heap[0].hash, 0);
    idx = 0;
  } else {
    idx = t->heap_size++;
  }
  /* The staged error already includes bucket history and any eviction. */
  t->heap[idx] = (compact_entry_t){
      .key = key, .rate = error + weight, .error = error, .last_time_ms = now};
  t->heap[idx].hash = hash;
  slot_map_set(t, hash, idx);
  r.present = true;
  r.replaced = full;
  r.upper = t->heap[idx].rate;
  r.lower = fmax(0, r.upper - error);
  r.error = r.upper - r.lower;
  if (full)
    heapify_down(t, idx, now);
  else
    heapify_up(t, idx, now);
  return r;
}

topk_result_t compact_add(compact_t *store, uint64_t key, uint64_t hash,
                          double increment, uint64_t now_ms) {
  topk_result_t result = {0};
  if (!store || !isfinite(increment) || increment < 0)
    return result;
  if (increment == 0.0)
    return compact_query(store, key, hash, now_ms);
  if (store->kind == BISHFPM_PAIR_FSS)
    return fss_add(store, key, hash, increment, now_ms);
  uint64_t timestamp = now_ms;
  now_ms = effective_time(timestamp, store->max_time_ms);
  double weight =
      decay_since(increment, timestamp, now_ms, store->half_life_ms);
  double omitted_bound = decay_since(store->omitted_bound, store->max_time_ms,
                                     now_ms, store->half_life_ms);
  result.time_ms = now_ms;
  uint32_t idx;
  if (slot_map_find(store, key, hash, &idx)) {
    result.was_present = true;
    compact_entry_t *item = &store->heap[idx];
    double decayed = item_rate(item, now_ms, store->half_life_ms);
    if (weight > DBL_MAX - decayed)
      return result;
    store->omitted_bound = omitted_bound;
    store->max_time_ms = now_ms;
    item->error = decay_since(item->error, item->last_time_ms, now_ms,
                              store->half_life_ms);
    item->rate = decayed + weight;
    item->last_time_ms = now_ms;
    result.present = true;
    result.upper = item->rate;
    result.lower = fmax(0, item->rate - item->error);
    result.error = result.upper - result.lower;
    heapify_down(store, idx, now_ms);
    return result;
  }

  bool replacing = store->heap_size == store->k;
  if (store->kind == BISHFPM_PAIR_THRESHOLD && replacing)
    return result;
  double error = replacing
                     ? item_rate(&store->heap[0], now_ms, store->half_life_ms)
                     : omitted_bound;
  if (weight > DBL_MAX - error)
    return result;
  store->omitted_bound = omitted_bound;
  store->max_time_ms = now_ms;

  if (replacing) {
    idx = 0;
    slot_map_remove(store, store->heap[idx].hash, idx);
  } else {
    idx = store->heap_size++;
  }
  compact_entry_t *item = &store->heap[idx];
  item->key = key;
  store->heap[idx].hash = hash;
  item->rate = error + weight;
  item->error = error;
  item->last_time_ms = now_ms;
  result.present = true;
  result.replaced = replacing;
  result.upper = item->rate;
  result.lower = fmax(0, item->rate - item->error);
  result.error = result.upper - result.lower;
  slot_map_set(store, hash, idx);
  if (replacing)
    heapify_down(store, idx, now_ms);
  else
    heapify_up(store, idx, now_ms);
  return result;
}

topk_result_t compact_query(const compact_t *store, uint64_t key, uint64_t hash,
                            uint64_t now_ms) {
  topk_result_t result = {0};
  if (!store)
    return result;
  result.time_ms = effective_time(now_ms, store->max_time_ms);
  uint32_t idx;
  if (!slot_map_find(store, key, hash, &idx)) {
    if (store->kind == BISHFPM_PAIR_FSS)
      result.upper = result.error = fss_alpha(store, hash, result.time_ms);
    if (store->kind == BISHFPM_PAIR_THRESHOLD)
      result.upper = result.error =
          decay_since(store->omitted_bound, store->max_time_ms, result.time_ms,
                      store->half_life_ms);
    return result;
  }
  const compact_entry_t *item = &store->heap[idx];
  result.present = result.was_present = true;
  result.upper = item_rate(item, result.time_ms, store->half_life_ms);
  result.lower =
      decay_since(fmax(0, item->rate - item->error), item->last_time_ms,
                  result.time_ms, store->half_life_ms);
  result.error = result.upper - result.lower;
  return result;
}

/* Remove weak roots without a whole-store scan/rebuild. The heap order makes
 * stopping at the first strong root safe. Hash chains can still affect cost. */
static size_t prune_roots(compact_t *store, uint64_t now_ms, double min_rate,
                          size_t limit) {
  size_t pruned = 0;
  while (store->heap_size && pruned < limit) {
    double rate = item_rate(&store->heap[0], now_ms, store->half_life_ms);
    if (rate > min_rate ||
        (store->kind != BISHFPM_PAIR_THRESHOLD && rate == min_rate))
      return pruned;
    store->omitted_bound = fmax(store->omitted_bound, rate);
    if (store->kind == BISHFPM_PAIR_FSS)
      fss_record(store, store->heap[0].hash, rate, now_ms);
    slot_map_remove(store, store->heap[0].hash, 0);
    uint32_t last = store->heap_size - 1;
    if (last) {
      size_t slot = slot_for_index(store, last);
      store->heap[0] = store->heap[last];
      store->slot_map[slot] = 1;
    }
    store->heap_size--;
    pruned++;
    if (store->heap_size)
      heapify_down(store, 0, now_ms);
  }
  return pruned;
}

size_t compact_prune_bounded(compact_t *store, uint64_t now_ms, size_t limit) {
  if (!store || !limit || !store->heap_size)
    return 0;
  now_ms = effective_time(now_ms, store->max_time_ms);
  store->omitted_bound = bishfpm_rate_decay(
      store->omitted_bound, now_ms - store->max_time_ms, store->half_life_ms);
  store->sketch_total = bishfpm_rate_decay(
      store->sketch_total, now_ms - store->max_time_ms, store->half_life_ms);
  store->max_time_ms = now_ms;
  size_t pruned = prune_roots(store, now_ms, store->forget_below, limit);
  store->forgotten += pruned;
  return pruned;
}

size_t compact_prune(compact_t *store, uint64_t now_ms, double min_rate) {
  if (!store || store->heap_size == 0) {
    return 0;
  }

  if (!isfinite(min_rate) || min_rate < 0)
    return 0;
  bool threshold = store->kind == BISHFPM_PAIR_THRESHOLD;
  if (threshold)
    min_rate = fmin(min_rate, store->forget_below);
  now_ms = effective_time(now_ms, store->max_time_ms);
  store->omitted_bound = bishfpm_rate_decay(
      store->omitted_bound, now_ms - store->max_time_ms, store->half_life_ms);
  store->sketch_total = bishfpm_rate_decay(
      store->sketch_total, now_ms - store->max_time_ms, store->half_life_ms);
  store->max_time_ms = now_ms;
  size_t pruned = 0;
  /* Small expirations avoid a full scan. Larger sweeps use bulk compaction. */
  if (threshold) {
    pruned = prune_roots(store, now_ms, min_rate, 1024);
    /* Below the limit, pruning stopped at a strong root or an empty heap. */
    if (pruned < 1024 || !store->heap_size) {
      store->forgotten += pruned;
      return pruned;
    }
  }
  uint32_t i = 0;
  while (i < store->heap_size) {
    double r = item_rate(&store->heap[i], now_ms, store->half_life_ms);
    if (r < min_rate || (threshold && r <= min_rate)) {
      if (r > store->omitted_bound)
        store->omitted_bound = r;
      if (store->kind == BISHFPM_PAIR_FSS)
        fss_record(store, store->heap[i].hash, r, now_ms);
      /* move last element into this position */
      uint32_t last = store->heap_size - 1;
      if (i != last) {
        store->heap[i] = store->heap[last];
      }
      store->heap_size--;
      pruned++;
    } else {
      i++;
    }
  }

  if (!pruned)
    return 0;
  store->forgotten += pruned;
  if (threshold) {
    for (uint32_t j = store->heap_size / 2; j > 0; j--)
      heapify_down_unindexed(store, j - 1, now_ms);
    slot_map_rebuild(store);
  } else {
    /* Index first to preserve the probe-chain order of SS/FSS. */
    slot_map_rebuild(store);
    for (uint32_t j = store->heap_size / 2; j > 0; j--)
      heapify_down(store, j - 1, now_ms);
  }
  return pruned;
}

/* A strong heap node has only strong descendants. Count just enough weak
 * nodes to prove capacity; do not scan the entire table on every rejection. */
static size_t weak_count(const compact_t *t, size_t index, uint64_t now,
                         size_t limit) {
  if (!limit || index >= t->heap_size ||
      item_rate(&t->heap[index], now, t->half_life_ms) > t->forget_below)
    return 0;
  size_t count = 1;
  count += weak_count(t, 2 * index + 1, now, limit - count);
  count += weak_count(t, 2 * index + 2, now, limit - count);
  return count;
}
bool compact_reserve(const compact_t *t, const uint32_t *ids, size_t n,
                     uint64_t now, bool *prune) {
  *prune = false;
  if (t->kind != BISHFPM_PAIR_THRESHOLD || n < 2)
    return true;
  size_t pairs = n * (n - 1) / 2;
  if (pairs > t->k)
    return false;
  now = effective_time(now, t->max_time_ms);
  size_t missing = 0, event_strong = 0;
  for (size_t i = 0; i < n; i++)
    for (size_t j = i + 1; j < n; j++) {
      uint64_t key = compact_key(ids[i], ids[j]);
      uint32_t idx;
      if (!slot_map_find(t, key, compact_hash(key), &idx))
        missing++;
      else if (item_rate(&t->heap[idx], now, t->half_life_ms) > t->forget_below)
        event_strong++;
    }
  if (missing <= t->k - t->heap_size)
    return true;
  size_t needed = pairs - event_strong - (t->k - t->heap_size);
  if (weak_count(t, 0, now, needed) < needed)
    return false;
  *prune = true;
  return true;
}
