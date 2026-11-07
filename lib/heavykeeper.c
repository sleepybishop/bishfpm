/* EWMA-decayed HeavyKeeper store plugin using only installed public interfaces
 */
#include "bishfpm_heavykeeper.h"
#include "plugin_common.h"

typedef struct {
  uint64_t hash;
  double rate;
  uint64_t last_time_ms;
  uint16_t length;
  char pattern[BISHFPM_MAX_PATTERN_LEN];
} hk_entry_t;
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
  plugin_text_slot_t *slot_map;
  hk_bucket_t *hk;
  uint64_t rng_state, max_time_ms;
  double total;
} hk_store_t;
#define PLUGIN_STORE_T hk_store_t
#define PLUGIN_ENTRY_T hk_entry_t
#include "plugin_common.h"

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
      o->decay_base <= 0 || !isfinite(mining->ewma_half_life_ms) ||
      mining->ewma_half_life_ms <= 0)
    return false;
  double base = o->decay_base > 1 ? 1 / o->decay_base : o->decay_base;
  /* bound weighted collision work: at .99 the decrement probability
   * underflows before a double counter reaches 75000 */
  if (base > .99)
    return false;
  *c = (hk_config_t){0};
  c->options = *o;
  c->options.decay_base = base;
  c->half_life_ms = mining->ewma_half_life_ms;
  c->seed = mining->hash_seed;
  c->slots = plugin_slots(o->capacity);
  size_t cells, total = sizeof(hk_store_t);
  return checked_mul(o->capacity, sizeof(hk_entry_t), &c->heap_bytes) &&
         checked_mul(c->slots, sizeof(plugin_text_slot_t), &c->map_bytes) &&
         checked_mul(o->depth, o->width, &cells) &&
         checked_mul(cells, sizeof(hk_bucket_t), &c->bucket_bytes) &&
         checked_add(total, c->heap_bytes, &total) &&
         checked_add(total, c->map_bytes, &total) &&
         checked_add(total, c->bucket_bytes, &c->group_bytes);
}
BISHFPM_PLUGIN_CONFIG_OPS(hk_config_t, 1, BISHFPM_MAX_ITEMSET_ORDER)
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
/* integer weights use geometric skips of Bernoulli trials; fractional
 * remainders use one proportional decrement trial */
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
    if (buckets[row]->fingerprint == fps[row] && weight > DBL_MAX - rates[row])
      return false;
  }
  double estimate = 0, base = c->options.decay_base;
  for (uint32_t row = 0; row < depth; row++) {
    uint32_t fp = fps[row];
    hk_bucket_t *b = buckets[row];
    b->rate = rates[row];
    b->time_ms = now;
    if (b->fingerprint == fp) {
      b->rate += weight;
    } else if (b->rate < .01) {
      b->fingerprint = fp;
      b->rate = weight;
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
  if (!valid_text_key(key, length))
    return (bishfpm_store_count_t){0, 0, 0, 0, BISHFPM_UPDATE_ZERO, 0};
  const hk_store_t *s = raw;
  now = effective_time(now, s->max_time_ms);
  uint32_t idx;
  bool resident = slot_map_find(s, key, length, hash, &idx);
  return (bishfpm_store_count_t){
      0, hk_estimate(s, key, length, now), resident, 0, BISHFPM_UPDATE_ZERO, 0};
}
static bishfpm_store_count_t update_text(void *raw, const char *pattern,
                                         size_t length, uint64_t hash,
                                         double weight, uint64_t now) {
  if (!valid_text_key(pattern, length))
    return (bishfpm_store_count_t){
        0, 0, 0, 0, weight == 0 ? BISHFPM_UPDATE_ZERO : BISHFPM_UPDATE_REJECTED,
        0};
  hk_store_t *t = raw;
  /* host already clamps time and discounts late weights */
  now = effective_time(now, t->max_time_ms);
  if (!weight)
    return query_text(t, pattern, length, hash, now);
  bishfpm_store_count_t r = {0};
  double half = t->config->half_life_ms;
  double total = decay_since(t->total, t->max_time_ms, now, half);
  double estimate;
  if (!(weight > 0) || weight > DBL_MAX - total ||
      !hk_update(t, pattern, length, weight, now, &estimate))
    return r;
  t->total = total + weight;
  t->max_time_ms = now;
  uint32_t idx, k = t->config->options.capacity;
  bool present = slot_map_find(t, pattern, length, hash, &idx);
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
    entry_identity(&t->heap[idx], pattern, length, hash);
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
  return heap_prune(raw, now, cutoff, false);
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
    plugin_prepare,
    plugin_complete,
    plugin_complete,
    plugin_complete,
    update_text,
    NULL,
    query_text,
    NULL,
    prune,
    snapshot,
    NULL};
