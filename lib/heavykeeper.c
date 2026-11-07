/* Extracted from perf (028ae33):lib/topk.c. Keep this backend independent
 * of private engine/store layouts; it uses only installed public interfaces. */
#include "bishfpm_hash.h"
#include "bishfpm_heavykeeper.h"
#include "bishfpm_rate.h"
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
  char pattern[BISHFPM_MAX_PATTERN_LEN];
  uint64_t hash;
  double rate;
  uint64_t last_time_ms;
} hk_entry_t;
typedef struct {
  uint64_t hash;
  uint32_t heap_idx;
  bool occupied;
} hk_slot_t;
typedef struct {
  double rate;
  uint64_t time_ms;
  uint32_t fingerprint;
} hk_bucket_t;
typedef struct {
  bishfpm_heavykeeper_options_t options;
  double half_life_ms;
  uint64_t seed;
  size_t slots, heap_bytes, map_bytes, bucket_bytes, group_bytes;
} hk_config_t;
typedef struct {
  const hk_config_t *config;
  uint32_t heap_size;
  hk_entry_t *heap;
  hk_slot_t *slot_map;
  hk_bucket_t *hk;
  uint64_t rng_state, max_time_ms;
  double hk_total;
} hk_store_t;
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
void bishfpm_heavykeeper_options_default(bishfpm_heavykeeper_options_t *o) {
  if (o)
    *o = (bishfpm_heavykeeper_options_t){64, 4, 256, .95};
}
static bool configure(const bishfpm_config_t *mining, const void *raw, size_t n,
                      hk_config_t *c) {
  if (!mining || !raw || n != sizeof(bishfpm_heavykeeper_options_t))
    return false;
  const bishfpm_heavykeeper_options_t *o = raw;
  if (!o->capacity || o->capacity > 1000000 || !o->depth || o->depth > 32 ||
      !o->width || o->width > (1U << 24) || !isfinite(o->decay_base) ||
      o->decay_base <= 0 || o->decay_base == 1 ||
      !isfinite(mining->ewma_half_life_ms) || mining->ewma_half_life_ms <= 0)
    return false;
  *c = (hk_config_t){0};
  c->options = *o;
  if (c->options.decay_base > 1)
    c->options.decay_base = 1 / c->options.decay_base;
  c->half_life_ms = mining->ewma_half_life_ms;
  c->seed = mining->hash_seed;
  c->slots = 16;
  while (c->slots < (size_t)o->capacity * 2)
    c->slots *= 2;
  size_t cells, total = sizeof(hk_store_t);
  return checked_mul(o->capacity, sizeof(hk_entry_t), &c->heap_bytes) &&
         checked_mul(c->slots, sizeof(hk_slot_t), &c->map_bytes) &&
         checked_mul(o->depth, o->width, &cells) &&
         checked_mul(cells, sizeof(hk_bucket_t), &c->bucket_bytes) &&
         checked_add(total, c->heap_bytes, &total) &&
         checked_add(total, c->map_bytes, &total) &&
         checked_add(total, c->bucket_bytes, &c->group_bytes);
}
static int plan(const bishfpm_config_t *m, const void *raw, size_t n,
                bishfpm_store_plan_t *out) {
  hk_config_t c;
  if (!out || !configure(m, raw, n, &c))
    return 0;
  *out = (bishfpm_store_plan_t){
      sizeof(c), c.group_bytes, 1, BISHFPM_MAX_ITEMSET_ORDER, {0, 0}};
  return 1;
}
static int clone_config(const bishfpm_store_host_t *h,
                        const bishfpm_config_t *m, const void *raw, size_t n,
                        void **out) {
  hk_config_t c;
  if (!h || !out || !configure(m, raw, n, &c))
    return 0;
  hk_config_t *owned = h->allocate(h->context, sizeof(*owned));
  if (!owned)
    return 0;
  *owned = c;
  *out = owned;
  return 1;
}
static void destroy_config(const bishfpm_store_host_t *h, void *c) {
  h->release(h->context, c, sizeof(hk_config_t));
}
static void destroy(const bishfpm_store_host_t *h, void *raw) {
  hk_store_t *s = raw;
  const hk_config_t *c = s->config;
  h->release(h->context, s->heap, c->heap_bytes);
  h->release(h->context, s->slot_map, c->map_bytes);
  h->release(h->context, s->hk, c->bucket_bytes);
  h->release(h->context, s, sizeof(*s));
}
static int create(const bishfpm_store_host_t *h, const void *raw, void **out) {
  const hk_config_t *c = raw;
  hk_store_t *s = h->allocate_zeroed(h->context, 1, sizeof(*s));
  if (!s)
    return 0;
  s->config = c;
  s->rng_state = 123456789ULL;
  s->heap = h->allocate_zeroed(h->context, 1, c->heap_bytes);
  s->slot_map = h->allocate_zeroed(h->context, 1, c->map_bytes);
  s->hk = h->allocate_zeroed(h->context, 1, c->bucket_bytes);
  if (!s->heap || !s->slot_map || !s->hk) {
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
/* find slot in hash map */
static int slot_map_find(const hk_store_t *topk, const char *pattern,
                         uint64_t hash, uint32_t *out_idx) {
  size_t cap = topk->config->slots, mask = cap - 1;
  size_t idx = (size_t)(hash & mask);

  for (size_t i = 0; i < cap; i++) {
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
static void slot_map_set(hk_store_t *topk, uint64_t hash, uint32_t heap_idx) {
  size_t cap = topk->config->slots, mask = cap - 1;
  size_t idx = (size_t)(hash & mask);

  for (size_t i = 0; i < cap; i++) {
    size_t s = (idx + i) & mask;
    if (!topk->slot_map[s].occupied) {
      topk->slot_map[s].hash = hash;
      topk->slot_map[s].heap_idx = heap_idx;
      topk->slot_map[s].occupied = true;
      return;
    }
  }
}

/* Locate by old heap position as well as hash, before either item moves. */
static size_t slot_for_index(const hk_store_t *topk, uint32_t index) {
  size_t mask = topk->config->slots - 1;
  size_t slot = topk->heap[index].hash & mask;
  while (!topk->slot_map[slot].occupied ||
         topk->slot_map[slot].heap_idx != index)
    slot = (slot + 1) & mask;
  return slot;
}

/* remove slot from hash map using backward shift */
static void slot_map_remove(hk_store_t *topk, uint32_t heap_idx) {
  size_t mask = topk->config->slots - 1;
  size_t j = slot_for_index(topk, heap_idx);
  for (size_t k = (j + 1) & mask; topk->slot_map[k].occupied;
       k = (k + 1) & mask) {
    size_t orig = (size_t)(topk->slot_map[k].hash & mask);
    if (((k - orig) & mask) >= ((k - j) & mask)) {
      topk->slot_map[j] = topk->slot_map[k];
      j = k;
    }
  }
  topk->slot_map[j] = (hk_slot_t){0};
}

static void slot_map_rebuild(hk_store_t *topk) {
  memset(topk->slot_map, 0, topk->config->slots * sizeof(*topk->slot_map));
  for (uint32_t i = 0; i < topk->heap_size; i++)
    slot_map_set(topk, topk->heap[i].hash, i);
}

/* compute decayed item rate */
static inline double item_rate(const hk_entry_t *item, uint64_t now_ms,
                               double half_life_ms) {
  return decay_since(item->rate, item->last_time_ms, now_ms, half_life_ms);
}

/* sift up in min-heap */
static void heapify_up(hk_store_t *topk, uint32_t idx, uint64_t now_ms) {
  if (!idx)
    return;
  double half = topk->config->half_life_ms;
  uint32_t parent = (idx - 1) / 2;
  double rate = item_rate(&topk->heap[idx], now_ms, half);
  if (!(rate < item_rate(&topk->heap[parent], now_ms, half)))
    return;
  hk_entry_t held = topk->heap[idx];
  size_t held_slot = slot_for_index(topk, idx);
  uint32_t hole = idx;
  for (;;) {
    size_t slot = slot_for_index(topk, parent);
    topk->heap[hole] = topk->heap[parent];
    topk->slot_map[slot].heap_idx = hole;
    hole = parent;
    if (!hole)
      break;
    parent = (hole - 1) / 2;
    if (!(rate < item_rate(&topk->heap[parent], now_ms, half)))
      break;
  }
  topk->heap[hole] = held;
  topk->slot_map[held_slot].heap_idx = hole;
}

/* sift down in min-heap */
static void heapify_down(hk_store_t *topk, uint32_t idx, uint64_t now_ms) {
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
  hk_entry_t held = topk->heap[idx];
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
  topk->slot_map[held_slot].heap_idx = hole;
}

/* HeavyKeeper collision decay adapted from oldhk:lib/topk.c. Integer weights
 * use geometric skips of Bernoulli trials; a discounted fractional remainder
 * gets one proportional decrement trial. Unlike oldhk, no artificial floor of
 * one is applied to decayed/late updates. Fingerprints remain approximate;
 * resident identity checks use the full string in the current index. */
static double hk_uniform(hk_store_t *t) {
  return ((double)(bishfpm_splitmix64(&t->rng_state) >> 11) + 1) /
         9007199254740994.0;
}
static hk_bucket_t *hk_bucket(const hk_store_t *t, const char *pattern,
                              size_t length, uint32_t row, uint32_t *fp) {
  const hk_config_t *c = t->config;
  uint64_t seed = c->seed ^ (((uint64_t)row + 1) * 0x9e3779b97f4a7c15ULL);
  uint64_t h = bishfpm_murmur3_64(pattern, length, seed);
  *fp = (uint32_t)(h >> 32);
  if (!*fp)
    *fp = (uint32_t)h | 1;
  return &t->hk[(size_t)row * c->options.width +
                (uint32_t)h % c->options.width];
}
static double hk_estimate(const hk_store_t *t, const char *pattern,
                          size_t length, uint64_t now) {
  double estimate = 0, half = t->config->half_life_ms;
  for (uint32_t row = 0; row < t->config->options.depth; row++) {
    uint32_t fp;
    const hk_bucket_t *b = hk_bucket(t, pattern, length, row, &fp);
    if (b->fingerprint == fp)
      estimate = fmax(estimate, decay_since(b->rate, b->time_ms, now, half));
  }
  return estimate;
}
static bool hk_update(hk_store_t *t, const char *pattern, size_t length,
                      double weight, uint64_t now, double *out) {
  const hk_config_t *c = t->config;
  uint32_t depth = c->options.depth;
  hk_bucket_t *buckets[32];
  uint32_t fps[32];
  double rates[32];
  for (uint32_t row = 0; row < depth; row++) {
    buckets[row] = hk_bucket(t, pattern, length, row, &fps[row]);
    rates[row] = decay_since(buckets[row]->rate, buckets[row]->time_ms, now,
                             c->half_life_ms);
    if (rates[row] >= .01 && buckets[row]->fingerprint == fps[row] &&
        weight > DBL_MAX - rates[row])
      return false;
  }
  double estimate = 0, base = c->options.decay_base;
  for (uint32_t row = 0; row < depth; row++) {
    uint32_t fp = fps[row];
    hk_bucket_t *b = buckets[row];
    b->rate = rates[row];
    b->time_ms = now;
    if (b->rate < .01) {
      b->fingerprint = fp;
      b->rate = weight;
    } else if (b->fingerprint == fp) {
      b->rate += weight;
    } else {
      double whole = floor(weight), fraction = weight - whole;
      while (whole >= 1 && b->rate > 0 && b->rate - 1 < b->rate) {
        double probability = pow(base, b->rate);
        if (probability <= 0)
          break;
        double skip = probability >= 1
                          ? 0
                          : floor(log(hk_uniform(t)) / log1p(-probability));
        if (!isfinite(skip) || skip >= whole)
          break;
        whole -= skip + 1;
        b->rate -= 1;
        if (b->rate <= 0) {
          b->fingerprint = fp;
          b->rate = 1 + whole + fraction;
          fraction = 0;
          break;
        }
      }
      if (b->fingerprint != fp && fraction > 0 &&
          hk_uniform(t) < pow(base, b->rate)) {
        b->rate -= fraction;
        if (b->rate <= 0) {
          b->fingerprint = fp;
          b->rate = fraction;
        }
      }
    }
    if (b->fingerprint == fp)
      estimate = fmax(estimate, b->rate);
  }
  *out = estimate;
  return true;
}
static bishfpm_store_count_t query_text(const void *raw, const char *key,
                                        size_t length, uint64_t hash,
                                        uint64_t now) {
  const hk_store_t *s = raw;
  now = effective_time(now, s->max_time_ms);
  uint32_t idx;
  bool resident = slot_map_find(s, key, hash, &idx);
  return (bishfpm_store_count_t){
      0, hk_estimate(s, key, length, now), resident, 0, BISHFPM_UPDATE_ZERO, 0};
}
static bishfpm_store_count_t update_text(void *raw, const char *pattern,
                                         size_t length, uint64_t hash,
                                         double weight, uint64_t now) {
  hk_store_t *t = raw;
  /* Host already clamps time and discounts late weights. */
  now = effective_time(now, t->max_time_ms);
  if (!weight)
    return query_text(t, pattern, length, hash, now);
  bishfpm_store_count_t r = {0};
  double half = t->config->half_life_ms;
  double total = decay_since(t->hk_total, t->max_time_ms, now, half);
  double estimate;
  if (!(weight > 0) || weight > DBL_MAX - total ||
      !hk_update(t, pattern, length, weight, now, &estimate))
    return r;
  t->hk_total = total + weight;
  t->max_time_ms = now;
  uint32_t idx, k = t->config->options.capacity;
  bool present = slot_map_find(t, pattern, hash, &idx);
  bool full = t->heap_size == k;
  r.upper = estimate;
  r.outcome = BISHFPM_UPDATE_FILTERED;
  if (!present &&
      (!estimate || (full && estimate <= item_rate(&t->heap[0], now, half))))
    return r;
  if (!present) {
    idx = full ? 0 : t->heap_size++;
    if (full)
      slot_map_remove(t, 0);
    memcpy(t->heap[idx].pattern, pattern, length + 1);
    t->heap[idx].hash = hash;
  }
  t->heap[idx].rate = estimate;
  t->heap[idx].last_time_ms = now;
  if (!present)
    slot_map_set(t, hash, idx);
  r.resident = 1;
  r.outcome = present ? BISHFPM_UPDATE_RESIDENT
              : full  ? BISHFPM_UPDATE_REPLACED
                      : BISHFPM_UPDATE_ADMITTED;
  if (idx && estimate < item_rate(&t->heap[(idx - 1) / 2], now, half))
    heapify_up(t, idx, now);
  else
    heapify_down(t, idx, now);
  return r;
}
static size_t prune(void *raw, uint64_t now, double cutoff) {
  hk_store_t *s = raw;
  double half = s->config->half_life_ms;
  now = effective_time(now, s->max_time_ms);
  s->hk_total = decay_since(s->hk_total, s->max_time_ms, now, half);
  s->max_time_ms = now;
  uint32_t i = 0;
  size_t removed = 0;
  while (i < s->heap_size) {
    if (item_rate(&s->heap[i], now, half) < cutoff) {
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
  const hk_store_t *s = raw;
  const hk_config_t *c = s->config;
  *out = (bishfpm_store_snapshot_t){s->heap_size,
                                    c->options.capacity,
                                    {c->options.depth, c->options.width, 0, 0}};
}
const bishfpm_store_descriptor_t bishfpm_heavykeeper_store = {
    BISHFPM_STORE_ABI_V1,
    sizeof(bishfpm_store_descriptor_t),
    BISHFPM_STORE_TEXT,
    0,
    BISHFPM_STORE_HEURISTIC_ESTIMATES,
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
    update_text,
    NULL,
    query_text,
    NULL,
    prune,
    snapshot,
    NULL};
