#include "store_twins.h"

typedef struct {
  twin_options_t options;
  bishfpm_config_t mining;
} config_t;
typedef struct {
  const config_t *config;
  bishfpm_store_host_t host;
  bishfpm_arena_t arena;
  bishfpm_topk_t *text;
  compact_t pair;
  bool prune;
  uint64_t time;
} state_t;
static size_t layout(const config_t *c) {
  if (c->options.kind >= 2)
    return compact_bytes(c->options.capacity, c->options.width,
                         c->options.kind == 2   ? BISHFPM_PAIR_SS
                         : c->options.kind == 3 ? BISHFPM_PAIR_THRESHOLD
                                                : BISHFPM_PAIR_FSS);
  topk_layout_t l;
  return topk_layout_configured(c->options.capacity,
                                c->mining.ewma_half_life_ms, c->options.width,
                                &l)
             ? l.arena_bytes
             : 0;
}
static int plan(const bishfpm_config_t *m, const void *raw, size_t n,
                bishfpm_store_plan_t *out) {
  if (!m || !raw || !out || n != sizeof(twin_options_t))
    return 0;
  const twin_options_t *o = raw;
  bool pair = o->kind >= 2;
  if (pair && (m->min_itemset_order != 2 || m->max_itemset_order != 2))
    return 0;
  config_t c = {*o, *m};
  size_t bytes = layout(&c);
  if (o->kind > 4 || !bytes)
    return 0;
  *out = (bishfpm_store_plan_t){sizeof(config_t),
                                sizeof(state_t) + bytes,
                                pair ? 2 : 1,
                                pair ? 2 : BISHFPM_MAX_ITEMSET_ORDER,
                                {0, 0}};
  return 1;
}
static int clone(const bishfpm_store_host_t *h, const bishfpm_config_t *m,
                 const void *raw, size_t n, void **out) {
  (void)n;
  config_t *c = h->allocate(h->context, sizeof(*c));
  if (!c)
    return 0;
  c->options = *(const twin_options_t *)raw;
  c->mining = *m;
  *out = c;
  return 1;
}
static void destroy_config(const bishfpm_store_host_t *h, void *p) {
  h->release(h->context, p, sizeof(config_t));
}
static void destroy(const bishfpm_store_host_t *h, void *raw) {
  state_t *s = raw;
  if (s->config->options.kind < 2) {
    h->release(h->context, s->arena.base, layout(s->config));
  } else {
    compact_t *p = &s->pair;
    h->release(h->context, p->heap, p->k * sizeof(*p->heap));
    h->release(h->context, p->slot_map, p->slot_map_cap * sizeof(*p->slot_map));
    h->release(h->context, p->sketch, p->sketch_width * sizeof(*p->sketch));
  }
  h->release(h->context, s, sizeof(*s));
}
static int create(const bishfpm_store_host_t *h, const void *raw, void **out) {
  const config_t *c = raw;
  state_t *s = h->allocate_zeroed(h->context, 1, sizeof(*s));
  if (!s)
    return 0;
  s->config = c;
  s->host = *h;
  if (c->options.kind < 2) {
    size_t bytes = layout(c);
    s->arena.base = s->arena.beg = h->allocate(h->context, bytes);
    if (s->arena.base) {
      s->arena.end = s->arena.base + bytes;
      s->text = c->options.width
                    ? bishfpm_topk_new_fss_arena(&s->arena, c->options.capacity,
                                                 c->mining.ewma_half_life_ms,
                                                 c->options.width,
                                                 c->mining.hash_seed)
                    : bishfpm_topk_new_arena(&s->arena, c->options.capacity,
                                             c->mining.ewma_half_life_ms);
    }
    if (!s->text) {
      destroy(h, s);
      return 0;
    }
    bishfpm_topk_set_seed(s->text, c->mining.hash_seed);
  } else {
    compact_t *p = &s->pair;
    p->k = c->options.capacity;
    p->sketch_width = c->options.width;
    p->kind = c->options.kind == 2   ? BISHFPM_PAIR_SS
              : c->options.kind == 3 ? BISHFPM_PAIR_THRESHOLD
                                     : BISHFPM_PAIR_FSS;
    p->half_life_ms = c->mining.ewma_half_life_ms;
    p->forget_below = c->options.forget_below;
    p->slot_map_cap = next_pow2((size_t)p->k * 2);
    p->heap = h->allocate_zeroed(h->context, p->k, sizeof(*p->heap));
    p->slot_map =
        h->allocate_zeroed(h->context, p->slot_map_cap, sizeof(*p->slot_map));
    if (p->sketch_width)
      p->sketch =
          h->allocate_zeroed(h->context, p->sketch_width, sizeof(*p->sketch));
    if (!p->heap || !p->slot_map || (p->sketch_width && !p->sketch)) {
      destroy(h, s);
      return 0;
    }
  }
  *out = s;
  return 1;
}
static int prepare(void *raw, const bishfpm_store_event_t *e, void **token) {
  state_t *s = raw;
  *token = s;
  s->time = e->time_ms;
  return s->config->options.kind < 2 ||
         compact_reserve(&s->pair, e->ids, e->count, e->time_ms, &s->prune);
}
static void cancel(void *raw, void *token) {
  (void)raw;
  (void)token;
}
static void commit(void *raw, void *token) {
  (void)token;
  state_t *s = raw;
  if (s->prune)
    compact_prune(&s->pair, s->time, s->pair.forget_below);
}
static void finish(void *raw, void *token) {
  (void)raw;
  (void)token;
}
static bishfpm_store_count_t count(topk_result_t r, bool known, double weight) {
  uint32_t outcome = !weight         ? BISHFPM_UPDATE_ZERO
                     : !r.accepted   ? BISHFPM_UPDATE_REJECTED
                     : r.was_present ? BISHFPM_UPDATE_RESIDENT
                     : r.replaced    ? BISHFPM_UPDATE_REPLACED
                     : r.present     ? BISHFPM_UPDATE_ADMITTED
                                     : BISHFPM_UPDATE_FILTERED;
  return (bishfpm_store_count_t){r.lower, r.upper, r.present,
                                 known,   outcome, 0};
}
static bishfpm_store_count_t update_text(void *raw, const char *key, size_t n,
                                         uint64_t hash, double w,
                                         uint64_t now) {
  (void)hash;
  state_t *s = raw;
  topk_result_t r = topk_add_sized(s->text, key, n, w, now);
  return count(r, r.present || s->config->options.width, w);
}
static bishfpm_store_count_t update_pair(void *raw, uint64_t key, uint64_t hash,
                                         double w, uint64_t now) {
  state_t *s = raw;
  topk_result_t r = compact_add(&s->pair, key, hash, w, now);
  return count(r, r.present || s->pair.kind != BISHFPM_PAIR_SS, w);
}
static bishfpm_store_count_t query_text(const void *raw, const char *key,
                                        size_t n, uint64_t hash, uint64_t now) {
  (void)n;
  (void)hash;
  const state_t *s = raw;
  topk_result_t r = topk_query_result(s->text, key, now);
  return count(r, r.present || s->config->options.width, 0);
}
static bishfpm_store_count_t query_pair(const void *raw, uint64_t key,
                                        uint64_t hash, uint64_t now) {
  const state_t *s = raw;
  topk_result_t r = compact_query(&s->pair, key, hash, now);
  return count(r, r.present || s->pair.kind != BISHFPM_PAIR_SS, 0);
}
static size_t prune(void *raw, uint64_t now, double cutoff) {
  state_t *s = raw;
  return s->text ? bishfpm_topk_prune(s->text, now, cutoff)
                 : compact_prune(&s->pair, now, cutoff);
}
static size_t prune_bounded(void *raw, uint64_t now, size_t n) {
  state_t *s = raw;
  return compact_prune_bounded(&s->pair, now, n);
}
static void snapshot(const void *raw, bishfpm_store_snapshot_t *out) {
  const state_t *s = raw;
  *out = (bishfpm_store_snapshot_t){s->text ? s->text->heap_size
                                            : s->pair.heap_size,
                                    s->config->options.capacity,
                                    {0, 0, 0, 0}};
}
const bishfpm_store_descriptor_t twin_text_store = {
    1,
    sizeof(bishfpm_store_descriptor_t),
    BISHFPM_STORE_TEXT,
    0,
    BISHFPM_STORE_RESIDENT_BOUNDS | BISHFPM_STORE_ABSENT_BOUNDS,
    {0, 0},
    plan,
    clone,
    destroy_config,
    create,
    destroy,
    prepare,
    cancel,
    commit,
    finish,
    update_text,
    NULL,
    query_text,
    NULL,
    prune,
    snapshot,
    NULL};
const bishfpm_store_descriptor_t twin_pair_store = {
    1,
    sizeof(bishfpm_store_descriptor_t),
    BISHFPM_STORE_PAIR64,
    0,
    BISHFPM_STORE_RESIDENT_BOUNDS | BISHFPM_STORE_ABSENT_BOUNDS |
        BISHFPM_STORE_STABLE_IDS,
    {0, 0},
    plan,
    clone,
    destroy_config,
    create,
    destroy,
    prepare,
    cancel,
    commit,
    finish,
    NULL,
    update_pair,
    NULL,
    query_pair,
    prune,
    snapshot,
    prune_bounded};
