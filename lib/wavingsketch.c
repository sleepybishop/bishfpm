/* weighted EWMA adaptation of WavingSketch (Li et al., KDD 2020) using only
 * installed public interfaces */
#include "bishfpm_wavingsketch.h"
#include "plugin_common.h"

typedef struct {
  uint64_t hash;
  uint64_t evidence_time;
  double rate, evidence;
  int sign;
  uint16_t length;
  bool occupied, exact;
  char key[BISHFPM_MAX_PATTERN_LEN];
} wave_cell_t;
typedef struct {
  double wave;
  uint64_t time;
} wave_bucket_t;
typedef struct {
  bishfpm_wavingsketch_options_t options;
  uint64_t seed;
  double half_life;
  size_t capacity, cell_bytes, bucket_bytes, group_bytes;
} wave_config_t;
typedef struct {
  const wave_config_t *config;
  wave_cell_t *cells;
  wave_bucket_t *buckets;
  size_t residents;
  uint64_t max_time;
  double total;
} wave_store_t;
void bishfpm_wavingsketch_options_default(bishfpm_wavingsketch_options_t *o) {
  if (o)
    *o = (bishfpm_wavingsketch_options_t){16, 4};
}
static bool configure(const bishfpm_config_t *m, const void *raw, size_t n,
                      wave_config_t *c) {
  if (!m || !raw || n != sizeof(bishfpm_wavingsketch_options_t))
    return false;
  const bishfpm_wavingsketch_options_t *o = raw;
  if (!o->buckets || o->buckets > (1U << 24) || !o->cells_per_bucket ||
      o->cells_per_bucket > 64 || !isfinite(m->ewma_half_life_ms) ||
      m->ewma_half_life_ms <= 0)
    return false;
  *c = (wave_config_t){0};
  c->options = *o;
  c->seed = m->hash_seed;
  c->half_life = m->ewma_half_life_ms;
  size_t total;
  return checked_mul(o->buckets, o->cells_per_bucket, &c->capacity) &&
         c->capacity <= 1000000 &&
         checked_mul(c->capacity, sizeof(wave_cell_t), &c->cell_bytes) &&
         checked_mul(o->buckets, sizeof(wave_bucket_t), &c->bucket_bytes) &&
         checked_add(sizeof(wave_store_t), c->cell_bytes, &total) &&
         checked_add(total, c->bucket_bytes, &c->group_bytes);
}
BISHFPM_PLUGIN_CONFIG_OPS(wave_config_t, 1, BISHFPM_MAX_ITEMSET_ORDER)
static void destroy(const bishfpm_store_host_t *h, void *raw) {
  wave_store_t *s = raw;
  h->release(h->context, s->cells, s->config->cell_bytes);
  h->release(h->context, s->buckets, s->config->bucket_bytes);
  h->release(h->context, s, sizeof(*s));
}
static int create(const bishfpm_store_host_t *h, const void *raw, void **out) {
  const wave_config_t *c = raw;
  wave_store_t *s = h->allocate_zeroed(h->context, 1, sizeof(*s));
  if (!s)
    return 0;
  s->config = c;
  s->cells = h->allocate_zeroed(h->context, 1, c->cell_bytes);
  s->buckets = h->allocate_zeroed(h->context, 1, c->bucket_bytes);
  if (!s->cells || !s->buckets) {
    destroy(h, s);
    return 0;
  }
  *out = s;
  return 1;
}
/* independent seeded hash domains for bucket placement and wave sign; caller
 * hash is an identity prefilter and full keys resolve aliases */
static size_t bucket_for(const wave_config_t *c, const char *key,
                         size_t length) {
  return bishfpm_murmur3_64(key, length,
                            c->seed ^ UINT64_C(0x243f6a8885a308d3)) %
         c->options.buckets;
}
static int sign_for(const wave_config_t *c, const char *key, size_t length) {
  return (bishfpm_murmur3_64(key, length,
                             c->seed ^ UINT64_C(0x13198a2e03707344)) &
          1)
             ? 1
             : -1;
}
static double decayed(const wave_store_t *s, double value, uint64_t dt) {
  /* preserve sign and avoid premature multiplier underflow for large counts */
  return dt ? copysign(
                  bishfpm_rate_decay(fabs(value), dt, s->config->half_life),
                  value)
            : value;
}
static double evidence_at(const wave_store_t *s, const wave_cell_t *cell,
                          uint64_t now) {
  return decay_since(cell->evidence, cell->evidence_time, now,
                     s->config->half_life);
}
static bishfpm_store_count_t query_text(const void *raw, const char *key,
                                        size_t length, uint64_t hash,
                                        uint64_t now) {
  if (!valid_text_key(key, length))
    return (bishfpm_store_count_t){0, 0, 0, 0, BISHFPM_UPDATE_ZERO, 0};
  const wave_store_t *s = raw;
  now = effective_time(now, s->max_time);
  size_t bucket = bucket_for(s->config, key, length);
  const wave_bucket_t *b = &s->buckets[bucket];
  const wave_cell_t *cells =
      &s->cells[bucket * s->config->options.cells_per_bucket];
  for (uint32_t i = 0; i < s->config->options.cells_per_bucket; i++)
    if (cells[i].occupied && cells[i].hash == hash &&
        cells[i].length == length && !memcmp(cells[i].key, key, length))
      return (bishfpm_store_count_t){evidence_at(s, &cells[i], now),
                                     decayed(s, cells[i].rate, now - b->time),
                                     1,
                                     0,
                                     BISHFPM_UPDATE_ZERO,
                                     0};
  return (bishfpm_store_count_t){0,
                                 fmax(0, decayed(s, b->wave, now - b->time) *
                                             sign_for(s->config, key, length)),
                                 0,
                                 0,
                                 BISHFPM_UPDATE_ZERO,
                                 0};
}
static bishfpm_store_count_t update_text(void *raw, const char *key,
                                         size_t length, uint64_t hash,
                                         double weight, uint64_t now) {
  if (!valid_text_key(key, length))
    return (bishfpm_store_count_t){
        0, 0, 0, 0, weight == 0 ? BISHFPM_UPDATE_ZERO : BISHFPM_UPDATE_REJECTED,
        0};
  wave_store_t *s = raw;
  now = effective_time(now, s->max_time);
  if (!weight)
    return query_text(s, key, length, hash, now);
  bishfpm_store_count_t r = {0};
  if (!(weight > 0))
    return r;
  size_t bucket = bucket_for(s->config, key, length);
  wave_bucket_t *b = &s->buckets[bucket];
  uint32_t n = s->config->options.cells_per_bucket;
  wave_cell_t *cells = &s->cells[bucket * n];
  uint32_t found = n, empty = n, minimum = n;
  double minrate = DBL_MAX;
  double rates[64]; /* config bounds bucket width; no update allocation */
  uint64_t dt = now - b->time;
  for (uint32_t i = 0; i < n; i++) {
    if (!cells[i].occupied) {
      rates[i] = 0;
      if (empty == n)
        empty = i;
      continue;
    }
    if (cells[i].hash == hash && cells[i].length == length &&
        !memcmp(cells[i].key, key, length))
      found = i;
    double rate = rates[i] = decayed(s, cells[i].rate, dt);
    if (minimum == n || rate < minrate) {
      minimum = i;
      minrate = rate;
    }
  }
  int sign = found < n ? cells[found].sign : sign_for(s->config, key, length);
  double wave = decayed(s, b->wave, dt), rate = weight;
  uint32_t target = found;
  bool replace = false, exact = true;
  if (found < n) {
    rate = rates[found] + weight;
    exact = cells[found].exact;
    if (!exact)
      wave += sign * weight;
  } else if (empty < n)
    target = empty;
  else {
    /* compare pre-update projection with minimum */
    double projection = wave * sign;
    wave += sign * weight;
    if (projection >= minrate) {
      target = minimum;
      replace = true;
      exact = false;
      rate = projection + weight;
      if (cells[minimum].exact)
        wave += cells[minimum].sign * minrate;
    }
  }
  double evidence =
      found < n ? evidence_at(s, &cells[found], now) + weight : weight;
  double total = decayed(s, s->total, now - s->max_time);
  /* preflight potentially overflowing results before mutating state */
  if (weight > DBL_MAX - total || !isfinite(total + weight) ||
      !isfinite(rate) || !isfinite(wave) || !isfinite(evidence))
    return r;
  if (dt)
    for (uint32_t i = 0; i < n; i++)
      if (cells[i].occupied)
        cells[i].rate = rates[i];
  b->wave = wave;
  b->time = now;
  s->max_time = now;
  s->total = total + weight;
  if (target < n) {
    wave_cell_t *cell = &cells[target];
    if (found == n) {
      if (!cell->occupied)
        s->residents++;
      memcpy(cell->key, key, length);
      cell->key[length] = '\0';
      cell->length = (uint16_t)length;
      cell->hash = hash;
      cell->sign = sign;
      cell->occupied = true;
      cell->exact = exact;
    }
    cell->rate = rate;
    cell->evidence = evidence;
    cell->evidence_time = now;
    r.resident = 1;
    r.upper = rate;
    r.lower = evidence;
    r.outcome = found < n ? BISHFPM_UPDATE_RESIDENT
                : replace ? BISHFPM_UPDATE_REPLACED
                          : BISHFPM_UPDATE_ADMITTED;
  } else {
    r.upper = fmax(0, wave * sign);
    r.outcome = BISHFPM_UPDATE_FILTERED;
  }
  return r;
}
static size_t prune(void *raw, uint64_t now, double cutoff) {
  wave_store_t *s = raw;
  (void)cutoff; /* keep positive heavy counts and signed history together */
  now = effective_time(now, s->max_time);
  size_t removed = 0;
  uint32_t n = s->config->options.cells_per_bucket;
  for (uint32_t j = 0; j < s->config->options.buckets; j++) {
    wave_bucket_t *b = &s->buckets[j];
    wave_cell_t *cells = &s->cells[(size_t)j * n];
    uint64_t dt = now - b->time;
    bool zero = decayed(s, b->wave, dt) == 0;
    for (uint32_t i = 0; i < n && zero; i++)
      if (cells[i].occupied && (decayed(s, cells[i].rate, dt) != 0 ||
                                evidence_at(s, &cells[i], now) != 0))
        zero = false;
    if (zero) {
      for (uint32_t i = 0; i < n; i++) {
        removed += cells[i].occupied;
        cells[i].occupied = false;
      }
      b->wave = 0;
      b->time = now;
    }
  }
  s->residents -= removed;
  s->total = decayed(s, s->total, now - s->max_time);
  s->max_time = now;
  return removed;
}
static void snapshot(const void *raw, bishfpm_store_snapshot_t *out) {
  const wave_store_t *s = raw;
  *out = (bishfpm_store_snapshot_t){
      s->residents,
      s->config->capacity,
      {s->config->options.buckets, s->config->options.cells_per_bucket, 0, 0}};
}
const bishfpm_store_descriptor_t bishfpm_wavingsketch_store = {
    BISHFPM_STORE_ABI_V1,
    sizeof(bishfpm_store_descriptor_t),
    BISHFPM_STORE_TEXT,
    0,
    BISHFPM_STORE_HEURISTIC_ESTIMATES | BISHFPM_STORE_ALERT_EVIDENCE,
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
