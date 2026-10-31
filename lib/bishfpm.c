#include "bishfpm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BISHFPM_SCRATCH_SIZE (64 * 1024)
#define BISHFPM_GROUP_ARENA_SIZE (256 * 1024)

/* cooldown entry */
typedef struct {
  uint64_t hash;
  uint64_t last_trigger_ms;
  bool occupied;
} bishfpm_cooldown_entry_t;

/* internal group representation */
typedef struct {
  char *group_key;
  uint64_t hash;
  bishfpm_rate_t group_rate;
  bishfpm_rate_table_t *item_rates;
  bishfpm_topk_t *topk;
  bishfpm_arena_t *arena;
  bishfpm_cooldown_entry_t *cooldowns;
  size_t cooldown_cap;
} bishfpm_group_t;

/* engine instance */
struct bishfpm {
  bishfpm_config_t cfg;
  bishfpm_group_t *groups;
  size_t group_cap;
  size_t group_count;
  bishfpm_arena_t *scratch_arena;
};

/* default config initialization */
void bishfpm_config_default(bishfpm_config_t *cfg) {
  if (!cfg) {
    return;
  }
  cfg->ewma_half_life_ms = BISHFPM_DEFAULT_HALF_LIFE_MS;
  cfg->item_rate_threshold = BISHFPM_DEFAULT_ITEM_THRESHOLD;
  cfg->promotion_fraction = BISHFPM_DEFAULT_PROMOTION_FRAC;
  cfg->min_pattern_rate = BISHFPM_DEFAULT_MIN_PATTERN_RATE;
  cfg->min_itemset_order = BISHFPM_DEFAULT_MIN_ORDER;
  cfg->max_itemset_order = BISHFPM_DEFAULT_MAX_ORDER;
  cfg->topk_k = BISHFPM_DEFAULT_TOPK_K;
  cfg->topk_width = BISHFPM_DEFAULT_TOPK_WIDTH;
  cfg->topk_depth = BISHFPM_DEFAULT_TOPK_DEPTH;
  cfg->trigger_cooldown_ms = BISHFPM_DEFAULT_COOLDOWN_MS;
}

/* round up to next power of 2 */
static size_t next_pow2(size_t v) {
  size_t p = 16;
  while (p < v) {
    p <<= 1;
  }
  return p;
}

/* create engine instance */
bishfpm_t *bishfpm_create(const bishfpm_config_t *cfg) {
  bishfpm_t *engine = calloc(1, sizeof(bishfpm_t));
  if (!engine) {
    return NULL;
  }

  if (cfg) {
    engine->cfg = *cfg;
  } else {
    bishfpm_config_default(&engine->cfg);
  }

  engine->group_cap = 64;
  engine->groups = calloc(engine->group_cap, sizeof(bishfpm_group_t));
  if (!engine->groups) {
    free(engine);
    return NULL;
  }

  engine->scratch_arena = bishfpm_arena_new(BISHFPM_SCRATCH_SIZE);
  if (!engine->scratch_arena) {
    free(engine->groups);
    free(engine);
    return NULL;
  }

  return engine;
}

/* free single group resources */
static void free_group_resources(bishfpm_group_t *grp) {
  if (!grp || !grp->group_key) {
    return;
  }
  free(grp->group_key);
  if (grp->item_rates) {
    bishfpm_rate_table_free(grp->item_rates);
  }
  if (grp->topk && !grp->topk->arena_backed) {
    bishfpm_topk_free(grp->topk);
  }
  if (grp->arena) {
    bishfpm_arena_free(grp->arena);
  }
  if (grp->cooldowns) {
    free(grp->cooldowns);
  }
  memset(grp, 0, sizeof(bishfpm_group_t));
}

/* free engine instance */
void bishfpm_free(bishfpm_t *engine) {
  if (!engine) {
    return;
  }
  if (engine->groups) {
    for (size_t i = 0; i < engine->group_cap; i++) {
      if (engine->groups[i].group_key) {
        free_group_resources(&engine->groups[i]);
      }
    }
    free(engine->groups);
  }
  if (engine->scratch_arena) {
    bishfpm_arena_free(engine->scratch_arena);
  }
  free(engine);
}

/* resize group hash table */
static bool resize_groups(bishfpm_t *engine, size_t new_cap) {
  bishfpm_group_t *new_groups = calloc(new_cap, sizeof(bishfpm_group_t));
  if (!new_groups) {
    return false;
  }

  size_t mask = new_cap - 1;
  for (size_t i = 0; i < engine->group_cap; i++) {
    if (!engine->groups[i].group_key) {
      continue;
    }
    size_t idx = (size_t)(engine->groups[i].hash & mask);
    while (new_groups[idx].group_key) {
      idx = (idx + 1) & mask;
    }
    new_groups[idx] = engine->groups[i];
  }

  free(engine->groups);
  engine->groups = new_groups;
  engine->group_cap = new_cap;
  return true;
}

/* find or create group context */
static bishfpm_group_t *get_or_create_group(bishfpm_t *engine,
                                            const char *group_key) {
  if (engine->group_count * 10 >= engine->group_cap * 7) {
    if (!resize_groups(engine, engine->group_cap * 2)) {
      return NULL;
    }
  }

  uint64_t h = bishfpm_hash_str(group_key, 0);
  size_t mask = engine->group_cap - 1;
  size_t idx = (size_t)(h & mask);

  while (engine->groups[idx].group_key) {
    if (engine->groups[idx].hash == h &&
        strcmp(engine->groups[idx].group_key, group_key) == 0) {
      return &engine->groups[idx];
    }
    idx = (idx + 1) & mask;
  }

  /* initialize new group */
  bishfpm_group_t *grp = &engine->groups[idx];
  grp->group_key = strdup(group_key);
  if (!grp->group_key) {
    return NULL;
  }
  grp->hash = h;
  bishfpm_rate_init(&grp->group_rate);

  /* allocate group arena */
  grp->arena = bishfpm_arena_new(BISHFPM_GROUP_ARENA_SIZE);
  if (!grp->arena) {
    free(grp->group_key);
    grp->group_key = NULL;
    return NULL;
  }

  /* allocate item rate table */
  grp->item_rates = bishfpm_rate_table_new(64);
  if (!grp->item_rates) {
    free_group_resources(grp);
    return NULL;
  }

  /* allocate top-k using group arena */
  grp->topk = bishfpm_topk_new_arena(
      grp->arena, engine->cfg.topk_k, engine->cfg.topk_depth,
      engine->cfg.topk_width, engine->cfg.ewma_half_life_ms, 0.95);
  if (!grp->topk) {
    /* fallback to standard allocation if arena size exceeded */
    grp->topk = bishfpm_topk_new(engine->cfg.topk_k, engine->cfg.topk_depth,
                                 engine->cfg.topk_width,
                                 engine->cfg.ewma_half_life_ms, 0.95);
    if (!grp->topk) {
      free_group_resources(grp);
      return NULL;
    }
  }

  grp->cooldown_cap = next_pow2(engine->cfg.topk_k * 2);
  grp->cooldowns = calloc(grp->cooldown_cap, sizeof(bishfpm_cooldown_entry_t));
  if (!grp->cooldowns) {
    free_group_resources(grp);
    return NULL;
  }

  engine->group_count++;
  return grp;
}

/* check cooldown and update timestamp */
static bool check_and_update_cooldown(bishfpm_group_t *grp, uint64_t hash,
                                      uint64_t now_ms, uint64_t cooldown_ms) {
  if (!grp->cooldowns || grp->cooldown_cap == 0) {
    return false;
  }

  size_t mask = grp->cooldown_cap - 1;
  size_t idx = (size_t)(hash & mask);

  for (size_t i = 0; i < grp->cooldown_cap; i++) {
    size_t s = (idx + i) & mask;
    if (!grp->cooldowns[s].occupied) {
      grp->cooldowns[s].hash = hash;
      grp->cooldowns[s].last_trigger_ms = now_ms;
      grp->cooldowns[s].occupied = true;
      return false; /* not in cooldown */
    }
    if (grp->cooldowns[s].hash == hash) {
      if (now_ms > grp->cooldowns[s].last_trigger_ms &&
          now_ms - grp->cooldowns[s].last_trigger_ms < cooldown_ms) {
        return true; /* active cooldown */
      }
      grp->cooldowns[s].last_trigger_ms = now_ms;
      return false; /* cooldown expired */
    }
  }

  return false;
}

/* context for combination evaluation */
typedef struct {
  bishfpm_t *engine;
  bishfpm_group_t *grp;
  uint64_t timestamp_ms;
  double group_rate;
  bishfpm_trigger_cb cb;
  void *udata;
  int triggered_count;
} bishfpm_eval_ctx_t;

/* callback for each candidate subset */
static void on_candidate_subset(const char *const *subset_items,
                                size_t subset_len, void *udata) {
  bishfpm_eval_ctx_t *ctx = (bishfpm_eval_ctx_t *)udata;
  char pattern[BISHFPM_MAX_PATTERN_LEN];

  size_t plen = bishfpm_itemset_serialize(subset_items, subset_len, pattern,
                                          sizeof(pattern));
  if (plen == 0) {
    return;
  }

  double p_rate =
      bishfpm_topk_update(ctx->grp->topk, pattern, ctx->timestamp_ms);
  double frac = ctx->group_rate > 0.0 ? (p_rate / ctx->group_rate) : 0.0;

  /* verify promotion threshold */
  if (subset_len >= ctx->engine->cfg.min_itemset_order &&
      p_rate >= ctx->engine->cfg.min_pattern_rate &&
      frac >= ctx->engine->cfg.promotion_fraction) {
    uint64_t phash = bishfpm_hash_str(pattern, 0);
    if (check_and_update_cooldown(ctx->grp, phash, ctx->timestamp_ms,
                                  ctx->engine->cfg.trigger_cooldown_ms)) {
      return; /* cooldown active */
    }

    ctx->triggered_count++;

    if (ctx->cb) {
      bishfpm_trigger_t trig;
      memset(&trig, 0, sizeof(trig));
      snprintf(trig.pattern, sizeof(trig.pattern), "%s", pattern);
      bishfpm_itemset_format(pattern, trig.formatted, sizeof(trig.formatted));
      trig.pattern_rate = p_rate;
      trig.group_rate = ctx->group_rate;
      trig.rate_fraction = frac;
      trig.item_count = (uint32_t)subset_len;
      trig.timestamp = ctx->timestamp_ms;

      ctx->cb(ctx->grp->group_key, &trig, ctx->udata);
    }
  }
}

/* ingest event */
int bishfpm_add_event(bishfpm_t *engine, const char *group_key,
                      uint64_t timestamp_ms, const char *const *items,
                      size_t num_items, bishfpm_trigger_cb cb, void *udata) {
  if (!engine || !group_key || !items || num_items == 0) {
    return 0;
  }

  /* reset scratch arena for this event */
  bishfpm_arena_reset(engine->scratch_arena);

  bishfpm_group_t *grp = get_or_create_group(engine, group_key);
  if (!grp) {
    return -1;
  }

  /* 1. update group baseline rate */
  double g_rate = bishfpm_rate_update(&grp->group_rate, timestamp_ms,
                                      engine->cfg.ewma_half_life_ms);

  /* 2. update item rates and collect elevated items */
  const char **elevated =
      bishfpm_alloc_array(engine->scratch_arena, const char *, num_items);
  if (!elevated) {
    return -1;
  }
  size_t elevated_count = 0;

  for (size_t i = 0; i < num_items; i++) {
    if (!items[i] || items[i][0] == '\0') {
      continue;
    }
    double ir = bishfpm_rate_table_update(
        grp->item_rates, items[i], timestamp_ms, engine->cfg.ewma_half_life_ms);
    if (ir >= engine->cfg.item_rate_threshold) {
      elevated[elevated_count++] = items[i];
    }
  }

  /* fast path: no elevated items */
  if (elevated_count == 0) {
    return 0;
  }

  /* sort elevated items lexicographically */
  bishfpm_itemset_sort(elevated, elevated_count);

  /* 3. generate candidate subsets and evaluate Top-K */
  bishfpm_eval_ctx_t ctx;
  ctx.engine = engine;
  ctx.grp = grp;
  ctx.timestamp_ms = timestamp_ms;
  ctx.group_rate = g_rate;
  ctx.cb = cb;
  ctx.udata = udata;
  ctx.triggered_count = 0;

  size_t max_k = engine->cfg.max_itemset_order;
  if (max_k > elevated_count) {
    max_k = elevated_count;
  }

  bishfpm_generate_subsets(elevated, elevated_count, max_k, on_candidate_subset,
                           &ctx);

  return ctx.triggered_count;
}

/* query group rate */
double bishfpm_group_rate(bishfpm_t *engine, const char *group_key,
                          uint64_t now_ms) {
  if (!engine || !group_key) {
    return 0.0;
  }
  uint64_t h = bishfpm_hash_str(group_key, 0);
  size_t mask = engine->group_cap - 1;
  size_t idx = (size_t)(h & mask);

  for (size_t i = 0; i < engine->group_cap; i++) {
    size_t s = (idx + i) & mask;
    if (!engine->groups[s].group_key) {
      break;
    }
    if (engine->groups[s].hash == h &&
        strcmp(engine->groups[s].group_key, group_key) == 0) {
      return bishfpm_rate_query(&engine->groups[s].group_rate, now_ms,
                                engine->cfg.ewma_half_life_ms);
    }
  }
  return 0.0;
}

/* query item rate in group */
double bishfpm_item_rate(bishfpm_t *engine, const char *group_key,
                         const char *item, uint64_t now_ms) {
  if (!engine || !group_key || !item) {
    return 0.0;
  }
  uint64_t h = bishfpm_hash_str(group_key, 0);
  size_t mask = engine->group_cap - 1;
  size_t idx = (size_t)(h & mask);

  for (size_t i = 0; i < engine->group_cap; i++) {
    size_t s = (idx + i) & mask;
    if (!engine->groups[s].group_key) {
      break;
    }
    if (engine->groups[s].hash == h &&
        strcmp(engine->groups[s].group_key, group_key) == 0) {
      return bishfpm_rate_table_query(engine->groups[s].item_rates, item,
                                      now_ms, engine->cfg.ewma_half_life_ms);
    }
  }
  return 0.0;
}

/* query pattern rate in group */
double bishfpm_pattern_rate(bishfpm_t *engine, const char *group_key,
                            const char *pattern, uint64_t now_ms) {
  if (!engine || !group_key || !pattern) {
    return 0.0;
  }
  uint64_t h = bishfpm_hash_str(group_key, 0);
  size_t mask = engine->group_cap - 1;
  size_t idx = (size_t)(h & mask);

  for (size_t i = 0; i < engine->group_cap; i++) {
    size_t s = (idx + i) & mask;
    if (!engine->groups[s].group_key) {
      break;
    }
    if (engine->groups[s].hash == h &&
        strcmp(engine->groups[s].group_key, group_key) == 0) {
      return bishfpm_topk_query(engine->groups[s].topk, pattern, now_ms);
    }
  }
  return 0.0;
}

/* prune inactive groups and items */
size_t bishfpm_prune(bishfpm_t *engine, uint64_t now_ms, double min_rate) {
  if (!engine || engine->group_count == 0) {
    return 0;
  }

  size_t pruned = 0;
  for (size_t i = 0; i < engine->group_cap; i++) {
    if (!engine->groups[i].group_key) {
      continue;
    }
    double gr = bishfpm_rate_query(&engine->groups[i].group_rate, now_ms,
                                   engine->cfg.ewma_half_life_ms);
    if (gr < min_rate) {
      free_group_resources(&engine->groups[i]);
      engine->group_count--;
      pruned++;
    } else {
      bishfpm_rate_table_prune(engine->groups[i].item_rates, now_ms,
                               engine->cfg.ewma_half_life_ms, min_rate);
    }
  }

  return pruned;
}
