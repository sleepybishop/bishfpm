#include "store_plugin.h"

static bool text_validate(const store_config_t *c) {
  topk_layout_t layout;
  return topk_layout_configured(c->mining->topk_k, c->mining->ewma_half_life_ms,
                                c->sketch_width, &layout);
}
static size_t text_bytes(const store_config_t *c) {
  topk_layout_t layout;
  size_t bytes;
  return topk_layout_configured(c->mining->topk_k, c->mining->ewma_half_life_ms,
                                c->sketch_width, &layout) &&
                 size_add(sizeof(bishfpm_arena_t), layout.arena_bytes, &bytes)
             ? bytes
             : SIZE_MAX;
}
static bool pair_validate(const store_config_t *c) {
  const bishfpm_config_t *cfg = c->mining;
  const bishfpm_pair_options_t *p = &c->pairs;
  if (cfg->min_itemset_order != 2 || cfg->max_itemset_order != 2 ||
      !isfinite(p->forget_below) || p->forget_below < 0 ||
      !compact_configured_bytes(p->capacity, p->sketch_width, p->store,
                                p->forget_below))
    return false;
  switch (p->store) {
  case BISHFPM_PAIR_SS:
    return p->sketch_width == 0;
  case BISHFPM_PAIR_THRESHOLD:
    return p->sketch_width == 0 && cfg->item_rate_threshold == 0 &&
           cfg->item_share_threshold == 0;
  case BISHFPM_PAIR_FSS:
    return p->sketch_width != 0;
  default:
    return false;
  }
}
static size_t pair_bytes(const store_config_t *c) {
  size_t bytes =
      compact_configured_bytes(c->pairs.capacity, c->pairs.sketch_width,
                               c->pairs.store, c->pairs.forget_below);
  return bytes ? bytes : SIZE_MAX;
}
static bool text_create(store_t *s, bishfpm_memory_budget_t *budget) {
  const store_config_t *c = s->config;
  const bishfpm_config_t *cfg = c->mining;
  topk_layout_t layout;
  if (!topk_layout_configured(cfg->topk_k, cfg->ewma_half_life_ms,
                              c->sketch_width, &layout))
    return false;
  s->arena = arena_new_budget(layout.arena_bytes, budget);
  if (!s->arena)
    return false;
  s->state.text = c->sketch_width
                      ? bishfpm_topk_new_fss_arena(
                            s->arena, cfg->topk_k, cfg->ewma_half_life_ms,
                            c->sketch_width, cfg->hash_seed)
                      : bishfpm_topk_new_arena(s->arena, cfg->topk_k,
                                               cfg->ewma_half_life_ms);
  if (!s->state.text)
    return false;
  bishfpm_topk_set_seed(s->state.text, cfg->hash_seed);
  return true;
}
static bool pair_create(store_t *s, bishfpm_memory_budget_t *budget) {
  const store_config_t *c = s->config;
  s->state.pairs = compact_new_budget(
      c->pairs.capacity, c->mining->ewma_half_life_ms, c->pairs.sketch_width,
      c->pairs.store, c->pairs.forget_below, budget);
  return s->state.pairs != NULL;
}
static void text_destroy(store_t *s) { bishfpm_arena_free(s->arena); }
static void pair_destroy(store_t *s) { compact_free(s->state.pairs); }
static store_result_t text_query(const store_t *s, const char *key,
                                 uint64_t now) {
  topk_result_t r = topk_query_result(s->state.text, key, now);
  return (store_result_t){r, r.present || s->config->sketch_width != 0};
}
static store_result_t pair_query(const store_t *s, uint64_t key, uint64_t hash,
                                 uint64_t now) {
  topk_result_t r = compact_query(s->state.pairs, key, hash, now);
  return (store_result_t){r, r.present ||
                                 s->config->pairs.store != BISHFPM_PAIR_SS};
}
static size_t text_prune(store_t *s, uint64_t now, double cutoff) {
  return bishfpm_topk_prune(s->state.text, now, cutoff);
}
static size_t pair_prune(store_t *s, uint64_t now, double cutoff) {
  return compact_prune(s->state.pairs, now, cutoff);
}
static size_t pair_prune_bounded(store_t *s, uint64_t now, size_t n) {
  return compact_prune_bounded(s->state.pairs, now, n);
}
static store_snapshot_t text_snapshot(const store_t *s) {
  return (store_snapshot_t){bishfpm_topk_count(s->state.text),
                            s->config->mining->topk_k, text_bytes(s->config)};
}
static store_snapshot_t pair_snapshot(const store_t *s) {
  return (store_snapshot_t){s->state.pairs->heap_size,
                            s->config->pairs.capacity,
                            compact_storage_bytes(s->state.pairs)};
}
const store_ops_t store_text_ops = {
    false,      text_validate, text_bytes, text_create, text_destroy,
    text_query, NULL,          text_prune, NULL,        text_snapshot};
const store_ops_t store_pair_ops = {
    true, pair_validate, pair_bytes, pair_create,        pair_destroy,
    NULL, pair_query,    pair_prune, pair_prune_bounded, pair_snapshot};
size_t store_commit(store_t *s, uint64_t now, store_reservation_t *r) {
  if (s->config->extension) {
    store_extension_t *x = s->config->extension;
    x->descriptor.commit_prepared(s->state.extension, r->token);
    return 0;
  }
  return r->prune
             ? compact_prune(s->state.pairs, now, s->config->pairs.forget_below)
             : 0;
}

void store_cancel(store_t *s, store_reservation_t *r) {
  if (s->config->extension) {
    store_extension_t *x = s->config->extension;
    x->descriptor.cancel_event(s->state.extension, r->token);
  }
}
void store_finish(store_t *s, store_reservation_t *r) {
  if (s->config->extension) {
    store_extension_t *x = s->config->extension;
    x->descriptor.finish_event(s->state.extension, r->token);
  }
}
