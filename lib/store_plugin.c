#include "store_plugin.h"

void bishfpm_store_engine_options_default(bishfpm_store_engine_options_t *o) {
  if (o)
    *o = (bishfpm_store_engine_options_t){BISHFPM_STORE_ABI_V1,
                                          sizeof(*o),
                                          BISHFPM_MEMORY_CONSERVATIVE,
                                          BISHFPM_DEFAULT_TOPK_K,
                                          {0, 0}};
}
bool store_extension_validate(const bishfpm_store_descriptor_t *d,
                              const bishfpm_store_engine_options_t *o,
                              const bishfpm_config_t *c, const void *raw,
                              size_t size, store_extension_t *x) {
  if (!d || (!raw && size) || d->abi_version != BISHFPM_STORE_ABI_V1 ||
      d->struct_size < BISHFPM_STORE_DESCRIPTOR_V1_SIZE || d->reserved0 ||
      d->reserved[0] || d->reserved[1] ||
      (d->representation != BISHFPM_STORE_TEXT &&
       d->representation != BISHFPM_STORE_PAIR64) ||
      !(d->capabilities &
        (BISHFPM_STORE_RESIDENT_BOUNDS | BISHFPM_STORE_HEURISTIC_ESTIMATES)) ||
      (d->capabilities &
       ~(BISHFPM_STORE_RESIDENT_BOUNDS | BISHFPM_STORE_ABSENT_BOUNDS |
         BISHFPM_STORE_STABLE_IDS | BISHFPM_STORE_HEURISTIC_ESTIMATES |
         BISHFPM_STORE_ALERT_EVIDENCE)) ||
      !d->plan || !d->clone_config || !d->destroy_config || !d->create ||
      !d->destroy || !d->prepare_event || !d->cancel_event ||
      !d->commit_prepared || !d->finish_event || !d->prune || !d->snapshot)
    return false;
  if ((d->capabilities & BISHFPM_STORE_HEURISTIC_ESTIMATES) &&
      (d->capabilities &
       (BISHFPM_STORE_RESIDENT_BOUNDS | BISHFPM_STORE_ABSENT_BOUNDS)))
    return false;
  if ((d->capabilities & BISHFPM_STORE_ALERT_EVIDENCE) &&
      !(d->capabilities & BISHFPM_STORE_HEURISTIC_ESTIMATES))
    return false;
  bool pair = d->representation == BISHFPM_STORE_PAIR64;
  if (pair ? (!d->update_pair || !d->query_pair || d->update_text ||
              d->query_text || !(d->capabilities & BISHFPM_STORE_STABLE_IDS) ||
              c->min_itemset_order != 2 || c->max_itemset_order != 2)
           : (!d->update_text || !d->query_text || d->update_pair ||
              d->query_pair || (d->capabilities & BISHFPM_STORE_STABLE_IDS)))
    return false;
  if (o->abi_version != BISHFPM_STORE_ABI_V1 || o->struct_size < sizeof(*o) ||
      o->reserved[0] || o->reserved[1] ||
      o->memory_mode > BISHFPM_MEMORY_DYNAMIC || !o->notification_capacity ||
      o->notification_capacity > 1000000)
    return false;
  *x = (store_extension_t){0};
  memcpy(&x->descriptor, d, BISHFPM_STORE_DESCRIPTOR_V1_SIZE);
  if (d->struct_size >= sizeof(*d))
    x->descriptor.prune_bounded = d->prune_bounded;
  if (!d->plan(c, raw, size, &x->plan) || x->plan.reserved[0] ||
      x->plan.reserved[1] || !x->plan.min_order ||
      x->plan.max_order > BISHFPM_MAX_ITEMSET_ORDER ||
      x->plan.min_order > c->min_itemset_order ||
      x->plan.max_order < c->max_itemset_order)
    return false;
  return o->memory_mode == BISHFPM_MEMORY_DYNAMIC ||
         (x->plan.engine_peak_bytes != SIZE_MAX &&
          x->plan.group_peak_bytes != SIZE_MAX);
}
static void *host_allocate(void *ctx, size_t bytes) {
  store_extension_t *x = ctx;
  if (!bytes || bytes > x->limit - x->bytes)
    return NULL;
  void *p = memory_malloc(x->budget, bytes);
  if (p)
    x->bytes += bytes;
  return p;
}
static void *host_zeroed(void *ctx, size_t n, size_t size) {
  store_extension_t *x = ctx;
  size_t bytes;
  if (!n || !size || !size_mul(n, size, &bytes)) {
    if (n && size && x->budget)
      x->budget->denials++;
    return NULL;
  }
  if (bytes > x->limit - x->bytes)
    return NULL;
  void *p = memory_calloc(x->budget, n, size);
  if (p)
    x->bytes += bytes;
  return p;
}
static void host_release(void *ctx, void *p, size_t bytes) {
  store_extension_t *x = ctx;
  if (p) {
    assert(x->bytes >= bytes);
    x->bytes -= bytes;
    memory_free(x->budget, p, bytes);
  }
}
bool store_extension_init(store_extension_t *x, bishfpm_memory_budget_t *budget,
                          const bishfpm_config_t *c, const void *raw,
                          size_t size) {
  x->budget = budget;
  x->bytes = 0;
  x->limit = budget ? SIZE_MAX : x->plan.engine_peak_bytes;
  x->config = NULL;
  x->host = (bishfpm_store_host_t){x, host_allocate, host_zeroed, host_release};
  if (!x->descriptor.clone_config(&x->host, c, raw, size, &x->config))
    return false;
  if (!budget)
    x->limit = x->plan.engine_peak_bytes +
               (size_t)c->max_groups * x->plan.group_peak_bytes;
  return true;
}
static bool extension_validate(const store_config_t *c) {
  return c->extension != NULL;
}
static size_t extension_bytes(const store_config_t *c) {
  const store_extension_t *x = c->extension;
  return x->plan.group_peak_bytes;
}
static bool extension_create(store_t *s, bishfpm_memory_budget_t *budget) {
  (void)budget;
  store_extension_t *x = s->config->extension;
  void *state = NULL;
  if (!x->descriptor.create(&x->host, x->config, &state))
    return false;
  s->state.extension = state;
  return state != NULL;
}
static void extension_destroy(store_t *s) {
  store_extension_t *x = s->config->extension;
  if (s->state.extension)
    x->descriptor.destroy(&x->host, s->state.extension);
}
topk_result_t store_extension_count(bishfpm_store_count_t r, uint64_t hash) {
  /* extensions are trusted, but a malformed/uncertified interval cannot alert
   */
  bool valid = r.known && isfinite(r.lower) && isfinite(r.upper) &&
               r.lower >= 0 && r.upper >= r.lower && !r.reserved &&
               r.resident <= 1 && r.known == 1;
  topk_result_t result = {0};
  result.present = valid && r.resident;
  result.was_present = r.outcome == BISHFPM_UPDATE_RESIDENT;
  result.replaced = r.outcome == BISHFPM_UPDATE_REPLACED;
  result.accepted = r.outcome >= BISHFPM_UPDATE_RESIDENT &&
                    r.outcome <= BISHFPM_UPDATE_FILTERED;
  if (valid) {
    result.lower = r.lower;
    result.upper = r.upper;
    result.error = r.upper - r.lower;
  }
  result.hash = hash;
  return result;
}
static bool heuristic_valid(const store_extension_t *x,
                            bishfpm_store_count_t r) {
  bool evidence = x->descriptor.capabilities & BISHFPM_STORE_ALERT_EVIDENCE;
  return r.known == 0 && isfinite(r.lower) && r.lower >= 0 &&
         (evidence ? (r.resident || r.lower == 0) : r.lower == 0) &&
         isfinite(r.upper) && r.upper >= 0 && r.resident <= 1 && !r.reserved;
}
topk_result_t store_extension_update_count(const store_extension_t *x,
                                           bishfpm_store_count_t r,
                                           uint64_t hash) {
  if (!(x->descriptor.capabilities & BISHFPM_STORE_HEURISTIC_ESTIMATES))
    return store_extension_count(r, hash);
  topk_result_t count = {0};
  if (!heuristic_valid(x, r))
    return count;
  count.present = r.resident;
  count.was_present = r.outcome == BISHFPM_UPDATE_RESIDENT;
  count.replaced = r.outcome == BISHFPM_UPDATE_REPLACED;
  count.accepted = r.outcome >= BISHFPM_UPDATE_RESIDENT &&
                   r.outcome <= BISHFPM_UPDATE_FILTERED;
  /* Explicit alert policy; neither value is exposed as a certified bound. */
  count.lower = x->descriptor.capabilities & BISHFPM_STORE_ALERT_EVIDENCE
                    ? r.lower
                    : r.upper;
  count.upper = count.error = INFINITY;
  count.hash = hash;
  return count;
}
static store_result_t extension_result(const store_extension_t *x,
                                       bishfpm_store_count_t r) {
  if (x->descriptor.capabilities & BISHFPM_STORE_HEURISTIC_ESTIMATES) {
    topk_result_t count = {0};
    if (heuristic_valid(x, r)) {
      count.present = r.resident;
      count.upper = r.upper; /* Raw query estimate, with known=false. */
      if (x->descriptor.capabilities & BISHFPM_STORE_ALERT_EVIDENCE)
        count.lower =
            r.lower; /* Internal subset evidence, not public bounds. */
    }
    count.error = INFINITY;
    return (store_result_t){count, false};
  }
  topk_result_t count = store_extension_count(r, 0);
  bool known = r.known == 1 && isfinite(r.lower) && isfinite(r.upper) &&
               r.lower >= 0 && r.upper >= r.lower && !r.reserved &&
               r.resident <= 1;
  return (store_result_t){count, known};
}
static store_result_t extension_query_text(const store_t *s, const char *key,
                                           uint64_t now) {
  store_extension_t *x = s->config->extension;
  size_t length = strlen(key);
  uint64_t hash = bishfpm_murmur3_64(key, length, s->config->mining->hash_seed);
  return extension_result(
      x, x->descriptor.query_text(s->state.extension, key, length, hash, now));
}
static store_result_t extension_query_pair(const store_t *s, uint64_t key,
                                           uint64_t hash, uint64_t now) {
  store_extension_t *x = s->config->extension;
  return extension_result(
      x, x->descriptor.query_pair(s->state.extension, key, hash, now));
}
static size_t extension_prune(store_t *s, uint64_t now, double cutoff) {
  store_extension_t *x = s->config->extension;
  return x->descriptor.prune(s->state.extension, now, cutoff);
}
static size_t extension_prune_bounded(store_t *s, uint64_t now, size_t n) {
  store_extension_t *x = s->config->extension;
  return x->descriptor.prune_bounded
             ? x->descriptor.prune_bounded(s->state.extension, now, n)
             : 0;
}
static store_snapshot_t extension_snapshot(const store_t *s) {
  store_extension_t *x = s->config->extension;
  bishfpm_store_snapshot_t snap = {0};
  x->descriptor.snapshot(s->state.extension, &snap);
  /* host charges, aggregated once per engine, are authoritative for bytes */
  return (store_snapshot_t){snap.resident_patterns, snap.resident_capacity, 0};
}
const store_ops_t store_extension_text_ops = {false,
                                              extension_validate,
                                              extension_bytes,
                                              extension_create,
                                              extension_destroy,
                                              extension_query_text,
                                              NULL,
                                              extension_prune,
                                              NULL,
                                              extension_snapshot};
const store_ops_t store_extension_pair_ops = {true,
                                              extension_validate,
                                              extension_bytes,
                                              extension_create,
                                              extension_destroy,
                                              NULL,
                                              extension_query_pair,
                                              extension_prune,
                                              extension_prune_bounded,
                                              extension_snapshot};
