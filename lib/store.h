#ifndef BISHFPM_STORE_H
#define BISHFPM_STORE_H

#include "compact.h"

/* cold store operations; candidate loops bind the typed state directly */
typedef struct store_config store_config_t;
typedef struct store store_t;
typedef struct {
  topk_result_t count;
  bool known;
} store_result_t;
typedef struct {
  size_t residents, capacity, bytes;
} store_snapshot_t;
typedef struct {
  bool prune;
  void *token;
} store_reservation_t;
typedef struct {
  bool pairs;
  bool (*validate)(const store_config_t *);
  size_t (*bytes)(const store_config_t *);
  bool (*create)(store_t *, bishfpm_memory_budget_t *);
  void (*destroy)(store_t *);
  store_result_t (*query_text)(const store_t *, const char *, uint64_t);
  store_result_t (*query_pair)(const store_t *, uint64_t, uint64_t, uint64_t);
  size_t (*prune)(store_t *, uint64_t, double);
  size_t (*prune_bounded)(store_t *, uint64_t, size_t);
  store_snapshot_t (*snapshot)(const store_t *);
} store_ops_t;
struct store_config {
  const store_ops_t *ops;
  const bishfpm_config_t *mining;
  uint32_t sketch_width;
  bishfpm_pair_options_t pairs;
  void *extension;
};
struct store {
  const store_config_t *config;
  union {
    bishfpm_topk_t *text;
    compact_t *pairs;
    void *extension;
  } state;
  bishfpm_arena_t *arena;
};

BISHFPM_PRIVATE extern const store_ops_t store_text_ops, store_pair_ops;
BISHFPM_PRIVATE size_t store_commit(store_t *, uint64_t, store_reservation_t *);
BISHFPM_PRIVATE void store_cancel(store_t *, store_reservation_t *);
BISHFPM_PRIVATE void store_finish(store_t *, store_reservation_t *);
static inline bool store_is_pairs(const store_t *s) {
  return s->config && s->config->ops->pairs;
}
static inline bool store_prepare_pair(store_t *s, const uint32_t *ids, size_t n,
                                      uint64_t now, store_reservation_t *r) {
  r->prune = false;
  return compact_reserve(s->state.pairs, ids, n, now, &r->prune);
}
#endif
