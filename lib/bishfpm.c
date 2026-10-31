#include "bishfpm.h"
#include "compact.h"
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_GROUP_CAPACITY 64
#define INITIAL_MARGINAL_CAPACITY 64
#define COOLDOWN_GROWTH_LIMIT 4096

/* Sorted unique metadata survives filtering of the pointer workspace. */
typedef struct {
  const char *key;
  size_t length;
  uint64_t hash;
  double marginal;
} prepared_item_t;

typedef struct {
  prepared_item_t *items;
  const char **keys; /* unique keys during reservation, sorted elevated keys
                        after commit */
  size_t *lengths;
  uint32_t *ids;
  size_t count;
} prepared_event_t;

static size_t workspace_bytes(size_t count) {
  return count * (sizeof(prepared_item_t) + sizeof(const char *) +
                  sizeof(size_t) + sizeof(uint32_t));
}

/* Both cooldown layouts share the metadata used by probing and expiry. */
typedef struct {
  uint64_t hash;
  uint64_t last_trigger_ms;
  uint32_t item_count;
  bool occupied;
} bishfpm_cooldown_entry_t;

typedef struct {
  bishfpm_cooldown_entry_t entry;
  char pattern[BISHFPM_MAX_PATTERN_LEN];
} text_cooldown_t;

typedef struct {
  bishfpm_cooldown_entry_t entry;
  uint64_t key;
} pair_cooldown_t;

static size_t cooldown_entry_size(bool numeric) {
  return numeric ? sizeof(pair_cooldown_t) : sizeof(text_cooldown_t);
}

static bishfpm_cooldown_entry_t *cooldown_entry(void *table, size_t i,
                                                size_t stride) {
  return (bishfpm_cooldown_entry_t *)((char *)table + i * stride);
}

static bool cooldown_matches(bishfpm_cooldown_entry_t *entry,
                             const void *identity, bool numeric) {
  return numeric
             ? ((pair_cooldown_t *)entry)->key == *(const uint64_t *)identity
             : strcmp(((text_cooldown_t *)entry)->pattern, identity) == 0;
}

static void cooldown_set_identity(bishfpm_cooldown_entry_t *entry,
                                  const void *identity, bool numeric) {
  if (numeric)
    ((pair_cooldown_t *)entry)->key = *(const uint64_t *)identity;
  else
    snprintf(((text_cooldown_t *)entry)->pattern, BISHFPM_MAX_PATTERN_LEN, "%s",
             (const char *)identity);
}

/* internal group representation */
typedef struct {
  char *group_key;
  uint64_t hash;
  bishfpm_rate_t group_rate;
  bishfpm_rate_table_t *item_rates;
  bishfpm_topk_t *topk;
  compact_t *pairs;
  bishfpm_arena_t *arena;
  void *cooldowns, *cooldown_spare;
  size_t cooldown_cap, cooldown_count, cooldown_spare_cap;
  /* Lower bound on the oldest trigger; refreshed during maintenance. */
  uint64_t cooldown_oldest_ms;
  uint64_t max_time_ms;
} bishfpm_group_t;

typedef enum {
  REJECT_INVALID,
  REJECT_REENTRANT,
  REJECT_EVENT_ITEMS,
  REJECT_PATTERN_LENGTH,
  REJECT_CANDIDATES,
  REJECT_GROUPS,
  REJECT_MARGINALS,
  REJECT_ALLOCATION,
  REJECT_REASON_COUNT
} rejection_reason_t;

/* engine instance */
struct bishfpm {
  bishfpm_config_t cfg;
  bool numeric_pairs;
  bishfpm_pair_options_t pair_options;
  uint64_t pair_capacity_rejections, pairs_forgotten;
  bishfpm_group_t *groups;
  size_t group_cap;
  size_t group_count;
  prepared_event_t workspace;
  bishfpm_stats_t stats;
  uint64_t rejected[REJECT_REASON_COUNT];
  bool busy;
};

/* default config initialization */
void bishfpm_config_default(bishfpm_config_t *cfg) {
  if (!cfg) {
    return;
  }
  cfg->ewma_half_life_ms = BISHFPM_DEFAULT_HALF_LIFE_MS;
  cfg->item_rate_threshold = BISHFPM_DEFAULT_ITEM_THRESHOLD;
  cfg->item_share_threshold = BISHFPM_DEFAULT_ITEM_SHARE_THRESHOLD;
  cfg->promotion_fraction = BISHFPM_DEFAULT_PROMOTION_FRAC;
  cfg->min_pattern_rate = BISHFPM_DEFAULT_MIN_PATTERN_RATE;
  cfg->min_lift = BISHFPM_DEFAULT_MIN_LIFT;
  cfg->subsumption_ratio = BISHFPM_DEFAULT_SUBSUMPTION_RATIO;
  cfg->min_itemset_order = BISHFPM_DEFAULT_MIN_ORDER;
  cfg->max_itemset_order = BISHFPM_DEFAULT_MAX_ORDER;
  cfg->topk_k = BISHFPM_DEFAULT_TOPK_K;
  cfg->trigger_cooldown_ms = BISHFPM_DEFAULT_COOLDOWN_MS;
  cfg->max_groups = BISHFPM_DEFAULT_MAX_GROUPS;
  cfg->max_items_per_group = BISHFPM_DEFAULT_MAX_ITEMS_PER_GROUP;
  cfg->hash_seed = BISHFPM_DEFAULT_HASH_SEED;
  cfg->max_event_items = 256;
  cfg->max_candidates_per_event = 8192;
  cfg->max_memory_bytes = 256U * 1024U * 1024U;
}

/* validate configuration parameters */
static bool validate_config(const bishfpm_config_t *c) {
  if (!c) {
    return false;
  }
  if (!isfinite(c->ewma_half_life_ms) || c->ewma_half_life_ms <= 0.0) {
    return false;
  }
  if (!isfinite(c->item_rate_threshold) || c->item_rate_threshold < 0.0) {
    return false;
  }
  if (!isfinite(c->item_share_threshold) || c->item_share_threshold < 0.0 ||
      c->item_share_threshold > 1.0) {
    return false;
  }
  if (!isfinite(c->promotion_fraction) || c->promotion_fraction < 0.0 ||
      c->promotion_fraction > 1.0) {
    return false;
  }
  if (!isfinite(c->min_pattern_rate) || c->min_pattern_rate < 0.0) {
    return false;
  }
  /* admission loss guard: item threshold must not exceed pattern threshold */
  if (c->min_pattern_rate > 0.0 &&
      c->item_rate_threshold > c->min_pattern_rate) {
    return false;
  }
  if (!isfinite(c->min_lift) || c->min_lift < 0.0) {
    return false;
  }
  if (!isfinite(c->subsumption_ratio) || c->subsumption_ratio < 0.0 ||
      c->subsumption_ratio > 1.0) {
    return false;
  }
  if (c->min_itemset_order < 1 ||
      c->min_itemset_order > BISHFPM_MAX_ITEMSET_ORDER) {
    return false;
  }
  if (c->max_itemset_order < c->min_itemset_order ||
      c->max_itemset_order > BISHFPM_MAX_ITEMSET_ORDER) {
    return false;
  }
  if (c->topk_k == 0 || c->topk_k > 1000000) {
    return false;
  }
  if (!c->max_groups || c->max_groups > 65536 || !c->max_items_per_group ||
      c->max_items_per_group > 1048576 || !c->max_event_items ||
      c->max_event_items > BISHFPM_MAX_EVENT_ITEMS ||
      !c->max_candidates_per_event ||
      c->max_candidates_per_event > BISHFPM_MAX_EVENT_CANDIDATES ||
      !c->max_memory_bytes)
    return false;
  return true;
}

static bool validate_pairs(const bishfpm_config_t *cfg,
                           const bishfpm_pair_options_t *pairs) {
  if (cfg->min_itemset_order != 2 || cfg->max_itemset_order != 2)
    return false;
  if (!isfinite(pairs->forget_below) || pairs->forget_below < 0)
    return false;

  switch (pairs->store) {
  case BISHFPM_PAIR_SS:
    return pairs->sketch_width == 0;
  case BISHFPM_PAIR_THRESHOLD:
    return pairs->sketch_width == 0 && cfg->item_rate_threshold == 0 &&
           cfg->item_share_threshold == 0;
  case BISHFPM_PAIR_FSS:
    return pairs->sketch_width != 0;
  default:
    return false;
  }
}

/* Upper bound on library-requested payload, including concurrent old/new
 * tables during reservation, pruning and cooldown growth; allocator overhead
 * and caller buffers are outside this budget. Limits above bound each factor.
 */
static size_t memory_bound(const bishfpm_config_t *c,
                           const bishfpm_pair_options_t *pairs) {
  topk_layout_t layout;
  if (!topk_layout(c->topk_k, c->ewma_half_life_ms, &layout))
    return SIZE_MAX;
  size_t pair_bytes =
      pairs ? compact_bytes(pairs->capacity, pairs->sketch_width, pairs->store)
            : 0;
  if (pairs && !pair_bytes)
    return SIZE_MAX;
  if (pairs)
    layout.arena_bytes = 0;
  size_t rate_cap = next_pow2((size_t)c->max_items_per_group * 2);
  if (rate_cap < INITIAL_MARGINAL_CAPACITY)
    rate_cap = INITIAL_MARGINAL_CAPACITY;
  size_t group_cap = next_pow2((size_t)c->max_groups * 2);
  if (group_cap < INITIAL_GROUP_CAPACITY)
    group_cap = INITIAL_GROUP_CAPACITY;
  size_t staged = c->max_event_items < c->max_items_per_group
                      ? c->max_event_items
                      : c->max_items_per_group;
  size_t cooldown_cap = layout.slot_cap > COOLDOWN_GROWTH_LIMIT
                            ? layout.slot_cap
                            : COOLDOWN_GROWTH_LIMIT;
  /* Include simultaneous old/new tables, all final key copies and the
   * bounded staging array. Pruning/growth never exceeds these capacities. */
  uint64_t per_group =
      sizeof(bishfpm_group_t) + BISHFPM_MAX_GROUP_KEY_LEN +
      sizeof(bishfpm_rate_table_t) + sizeof(bishfpm_arena_t) +
      layout.arena_bytes + pair_bytes +
      2 * (uint64_t)rate_cap * sizeof(bishfpm_rate_entry_t) +
      (uint64_t)c->max_items_per_group * BISHFPM_MAX_ITEM_LEN +
      (uint64_t)staged * sizeof(bishfpm_rate_entry_t) +
      2 * (uint64_t)cooldown_cap * cooldown_entry_size(pairs != NULL);
  uint64_t total = sizeof(bishfpm_t) + workspace_bytes(c->max_event_items) +
                   2 * (uint64_t)group_cap * sizeof(bishfpm_group_t) +
                   per_group * c->max_groups;
  return total > SIZE_MAX ? SIZE_MAX : (size_t)total;
}

/* create engine instance */
static bishfpm_t *create_options(const bishfpm_config_t *cfg,
                                 const bishfpm_pair_options_t *pairs) {
  bishfpm_config_t c;
  if (cfg) {
    c = *cfg;
  } else {
    bishfpm_config_default(&c);
  }

  if (!validate_config(&c)) {
    return NULL;
  }

  if (pairs && !validate_pairs(&c, pairs))
    return NULL;
  size_t bound = memory_bound(&c, pairs);
  if (bound == SIZE_MAX || bound > c.max_memory_bytes)
    return NULL;

  bishfpm_t *engine = calloc(1, sizeof(bishfpm_t));
  if (!engine) {
    return NULL;
  }

  engine->cfg = c;
  engine->numeric_pairs = pairs != NULL;
  if (pairs)
    engine->pair_options = *pairs;
  engine->busy = false; /* construction has not published the engine */
  engine->stats.memory_bound_bytes = bound;
  engine->group_cap = INITIAL_GROUP_CAPACITY;
  engine->groups = calloc(engine->group_cap, sizeof(bishfpm_group_t));
  if (!engine->groups) {
    free(engine);
    return NULL;
  }

  engine->workspace.items = calloc(c.max_event_items, sizeof(prepared_item_t));
  engine->workspace.keys = calloc(c.max_event_items, sizeof(const char *));
  engine->workspace.lengths = calloc(c.max_event_items, sizeof(size_t));
  engine->workspace.ids = calloc(c.max_event_items, sizeof(uint32_t));
  if (!engine->workspace.items || !engine->workspace.keys ||
      !engine->workspace.lengths || !engine->workspace.ids) {
    bishfpm_free(engine);
    return NULL;
  }

  return engine;
}

bishfpm_t *bishfpm_create_pairs(const bishfpm_config_t *cfg,
                                const bishfpm_pair_options_t *pairs) {
  return pairs ? create_options(cfg, pairs) : NULL;
}

bishfpm_t *bishfpm_create(const bishfpm_config_t *cfg) {
  return create_options(cfg, NULL);
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
  compact_free(grp->pairs);
  if (grp->arena) {
    bishfpm_arena_free(grp->arena);
  }
  if (grp->cooldowns) {
    free(grp->cooldowns);
  }
  free(grp->cooldown_spare);
  memset(grp, 0, sizeof(bishfpm_group_t));
}

/* free engine instance */
void bishfpm_free(bishfpm_t *engine) {
  if (!engine || engine->busy) {
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
  free(engine->workspace.items);
  free(engine->workspace.keys);
  free(engine->workspace.lengths);
  free(engine->workspace.ids);
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

/* Missing lookups stop at the first empty slot; identity includes full key. */
static bishfpm_group_t *find_group(const bishfpm_t *engine, const char *key,
                                   size_t *slot) {
  if (!engine || !key)
    return NULL;
  uint64_t hash = bishfpm_hash_str(key, engine->cfg.hash_seed);
  size_t idx = hash & (engine->group_cap - 1);
  while (engine->groups[idx].group_key) {
    if (engine->groups[idx].hash == hash &&
        strcmp(engine->groups[idx].group_key, key) == 0)
      return &engine->groups[idx];
    idx = (idx + 1) & (engine->group_cap - 1);
  }
  if (slot)
    *slot = idx;
  return NULL;
}

static bishfpm_group_t *get_or_create_group(bishfpm_t *engine,
                                            const char *group_key,
                                            rejection_reason_t *reason) {
  size_t idx;
  bishfpm_group_t *existing = find_group(engine, group_key, &idx);
  if (existing)
    return existing;
  uint64_t h = bishfpm_hash_str(group_key, engine->cfg.hash_seed);
  size_t mask;

  /* check max group budget */
  if (engine->cfg.max_groups > 0 &&
      engine->group_count >= engine->cfg.max_groups) {
    *reason = REJECT_GROUPS;
    return NULL;
  }

  if (engine->group_count * 10 >= engine->group_cap * 7) {
    if (!resize_groups(engine, engine->group_cap * 2))
      return NULL;
    mask = engine->group_cap - 1;
    idx = h & mask;
    while (engine->groups[idx].group_key)
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

  if (!engine->numeric_pairs) {
    topk_layout_t layout;
    if (!topk_layout(engine->cfg.topk_k, engine->cfg.ewma_half_life_ms,
                     &layout)) {
      free_group_resources(grp);
      return NULL;
    }
    grp->arena = bishfpm_arena_new(layout.arena_bytes);
    if (!grp->arena) {
      free(grp->group_key);
      grp->group_key = NULL;
      return NULL;
    }
  }
  /* allocate item rate table */
  grp->item_rates = bishfpm_rate_table_new_configured(
      INITIAL_MARGINAL_CAPACITY, engine->cfg.max_items_per_group,
      engine->cfg.hash_seed);
  if (!grp->item_rates) {
    free_group_resources(grp);
    return NULL;
  }

  if (engine->numeric_pairs) {
    bishfpm_pair_options_t *o = &engine->pair_options;
    grp->pairs = compact_new(o->capacity, engine->cfg.ewma_half_life_ms,
                             o->sketch_width, o->store, o->forget_below);
    if (!grp->pairs) {
      free_group_resources(grp);
      return NULL;
    }
  } else {
    /* allocate top-k using group arena */
    grp->topk = bishfpm_topk_new_arena(grp->arena, engine->cfg.topk_k,
                                       engine->cfg.ewma_half_life_ms);
    if (!grp->topk) {
      free_group_resources(grp);
      return NULL;
    }
    bishfpm_topk_set_seed(grp->topk, engine->cfg.hash_seed);
  }
  grp->cooldown_cap = next_pow2(engine->cfg.topk_k * 2);
  grp->cooldowns =
      calloc(grp->cooldown_cap, cooldown_entry_size(grp->pairs != NULL));
  if (!grp->cooldowns) {
    free_group_resources(grp);
    return NULL;
  }

  engine->group_count++;
  return grp;
}

/* Rebuild only to reclaim expired entries or grow. Keep the previous table as
 * reusable storage; allocation failure leaves the active table unchanged. */
static size_t cooldown_maintain(bishfpm_group_t *grp, size_t capacity,
                                uint64_t now_ms, uint64_t cooldown_ms,
                                bool expire) {
  size_t stride = cooldown_entry_size(grp->pairs != NULL);
  size_t occupied = 0;
  uint64_t oldest = UINT64_MAX;
  uint64_t ref_time = effective_time(now_ms, grp->max_time_ms);
  for (size_t i = 0; i < grp->cooldown_cap; i++) {
    const bishfpm_cooldown_entry_t *entry =
        cooldown_entry(grp->cooldowns, i, stride);
    if (!entry->occupied || (expire && ref_time >= entry->last_trigger_ms &&
                             ref_time - entry->last_trigger_ms >= cooldown_ms))
      continue;
    occupied++;
    if (entry->last_trigger_ms < oldest)
      oldest = entry->last_trigger_ms;
  }
  if (occupied == grp->cooldown_count && capacity == grp->cooldown_cap) {
    grp->cooldown_oldest_ms = oldest;
    return occupied;
  }
  if (grp->cooldown_spare_cap != capacity) {
    /* Release a differently sized spare before allocating, so two maximum
     * tables remain the worst-case reservation, including during growth. */
    free(grp->cooldown_spare);
    grp->cooldown_spare = NULL;
    grp->cooldown_spare_cap = 0;
    grp->cooldown_spare = calloc(capacity, stride);
    if (!grp->cooldown_spare)
      return grp->cooldown_count;
    grp->cooldown_spare_cap = capacity;
  } else {
    memset(grp->cooldown_spare, 0, capacity * stride);
  }
  void *table = grp->cooldown_spare;
  for (size_t i = 0; i < grp->cooldown_cap; i++) {
    const bishfpm_cooldown_entry_t *entry =
        cooldown_entry(grp->cooldowns, i, stride);
    if (!entry->occupied || (expire && ref_time >= entry->last_trigger_ms &&
                             ref_time - entry->last_trigger_ms >= cooldown_ms))
      continue;
    size_t j = entry->hash & (capacity - 1);
    while (cooldown_entry(table, j, stride)->occupied)
      j = (j + 1) & (capacity - 1);
    memcpy(cooldown_entry(table, j, stride), entry, stride);
  }
  grp->cooldown_spare = grp->cooldowns;
  grp->cooldown_spare_cap = grp->cooldown_cap;
  grp->cooldowns = table;
  grp->cooldown_cap = capacity;
  grp->cooldown_count = occupied;
  grp->cooldown_oldest_ms = oldest;
  return occupied;
}

/* Check cooldown and record notification identity independently of residency.
 */
static bool check_and_update_cooldown(bishfpm_group_t *grp, uint64_t hash,
                                      const void *identity, uint32_t item_count,
                                      uint64_t now_ms, uint64_t cooldown_ms,
                                      bool *saturated) {
  if (!grp->cooldowns || grp->cooldown_cap == 0 || cooldown_ms == 0) {
    return false;
  }

  if (now_ms > grp->max_time_ms) {
    grp->max_time_ms = now_ms;
  }
  uint64_t ref_time = grp->max_time_ms;
  bool numeric = grp->pairs != NULL;
  size_t stride = cooldown_entry_size(numeric);

  size_t mask = grp->cooldown_cap - 1;
  size_t idx = (size_t)(hash & mask);

  /* search existing entry */
  for (size_t i = 0; i < grp->cooldown_cap; i++) {
    size_t s = (idx + i) & mask;
    bishfpm_cooldown_entry_t *entry = cooldown_entry(grp->cooldowns, s, stride);
    if (!entry->occupied) {
      break;
    }
    if (entry->hash == hash && cooldown_matches(entry, identity, numeric)) {
      if (now_ms < entry->last_trigger_ms) {
        return true; /* earlier timestamp: in cooldown */
      }
      if (now_ms - entry->last_trigger_ms < cooldown_ms) {
        return true; /* active cooldown */
      }
      entry->last_trigger_ms = now_ms;
      entry->item_count = item_count;
      return false; /* cooldown expired */
    }
  }

  bool can_expire = grp->cooldown_count &&
                    ref_time >= grp->cooldown_oldest_ms &&
                    ref_time - grp->cooldown_oldest_ms >= cooldown_ms;
  if (grp->cooldown_count * 10 >= grp->cooldown_cap * 7) {
    if (can_expire)
      cooldown_maintain(grp, grp->cooldown_cap, now_ms, cooldown_ms, true);
    if (grp->cooldown_count * 10 >= grp->cooldown_cap * 7 &&
        grp->cooldown_cap < COOLDOWN_GROWTH_LIMIT)
      cooldown_maintain(grp, grp->cooldown_cap * 2, now_ms, cooldown_ms, false);
    mask = grp->cooldown_cap - 1;
    idx = (size_t)(hash & mask);
  }
  if (grp->cooldown_count == grp->cooldown_cap && !can_expire) {
    *saturated = true;
    return true;
  }

  /* insert new entry */
  for (size_t i = 0; i < grp->cooldown_cap; i++) {
    size_t s = (idx + i) & mask;
    bishfpm_cooldown_entry_t *entry = cooldown_entry(grp->cooldowns, s, stride);
    if (!entry->occupied) {
      entry->hash = hash;
      cooldown_set_identity(entry, identity, numeric);
      entry->last_trigger_ms = now_ms;
      entry->item_count = item_count;
      entry->occupied = true;
      if (!grp->cooldown_count || now_ms < grp->cooldown_oldest_ms)
        grp->cooldown_oldest_ms = now_ms;
      grp->cooldown_count++;
      return false;
    }
  }

  /* table saturated: evict oldest entry only if it has expired */
  size_t oldest = 0;
  uint64_t oldest_time = UINT64_MAX;
  for (size_t i = 0; i < grp->cooldown_cap; i++) {
    const bishfpm_cooldown_entry_t *entry =
        cooldown_entry(grp->cooldowns, i, stride);
    if (entry->last_trigger_ms < oldest_time) {
      oldest_time = entry->last_trigger_ms;
      oldest = i;
    }
  }
  if (ref_time >= oldest_time && ref_time - oldest_time >= cooldown_ms) {
    bishfpm_cooldown_entry_t *entry =
        cooldown_entry(grp->cooldowns, oldest, stride);
    entry->hash = hash;
    cooldown_set_identity(entry, identity, numeric);
    entry->last_trigger_ms = now_ms;
    entry->item_count = item_count;
    entry->occupied = true;
    if (now_ms < grp->cooldown_oldest_ms)
      grp->cooldown_oldest_ms = now_ms;
    return false;
  }

  *saturated = true;
  /* all entries actively in cooldown: preserve cooldown guarantees */
  return true;
}

/* check if all items in sub_pattern exist in sup_items */
static bool is_pattern_subset(const char *sub_pattern,
                              const char *const *sup_items, size_t sup_len) {
  if (!sub_pattern || !*sub_pattern)
    return false;
  size_t j = 0;
  const char *token = sub_pattern;
  while (*token) {
    const char *end = strchr(token, BISHFPM_ITEM_DELIM);
    size_t length = end ? (size_t)(end - token) : strlen(token);
    bool matched = false;
    while (j < sup_len) {
      int cmp = strncmp(sup_items[j], token, length);
      if (!cmp && sup_items[j][length])
        cmp = 1; /* token is a strict prefix of this item */
      j++;
      if (cmp > 0)
        return false;
      if (!cmp) {
        matched = true;
        break;
      }
    }
    if (!matched)
      return false;
    if (!end)
      return true;
    token = end + 1;
  }
  return false;
}

/* check if candidate is subsumed by an active subset trigger */
static bool is_candidate_subsumed(bishfpm_group_t *grp,
                                  const char *const *subset_items,
                                  size_t subset_len, double candidate_rate,
                                  uint64_t now_ms, uint64_t cooldown_ms,
                                  double subsumption_ratio) {
  if (grp->pairs || subsumption_ratio <= 0.0 || !grp->cooldowns) {
    return false;
  }
  for (size_t i = 0; i < grp->cooldown_cap; i++) {
    const text_cooldown_t *slot = (const text_cooldown_t *)grp->cooldowns + i;
    if (!slot->entry.occupied) {
      continue;
    }
    if (now_ms > slot->entry.last_trigger_ms &&
        now_ms - slot->entry.last_trigger_ms >= cooldown_ms) {
      continue; /* expired */
    }
    if (slot->entry.item_count < subset_len) {
      if (is_pattern_subset(slot->pattern, subset_items, subset_len)) {
        double parent_rate =
            bishfpm_topk_query_lower(grp->topk, slot->pattern, now_ms);
        if (parent_rate > 0.0 &&
            candidate_rate < subsumption_ratio * parent_rate) {
          return true; /* subsumed by active parent pattern */
        }
      }
    }
  }
  return false;
}

/* compute statistical lift of candidate subset */
static double compute_subset_lift(const prepared_event_t *event,
                                  const char *const *subset_items,
                                  size_t subset_len, double p_rate,
                                  double group_rate) {
  if (subset_len < 2 || group_rate <= 0.0 || p_rate <= 0.0) {
    return 1.0;
  }
  double r_indep = group_rate;
  for (size_t i = 0; i < subset_len; i++) {
    size_t lo = 0, hi = event->count;
    while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2;
      if (strcmp(event->items[mid].key, subset_items[i]) < 0)
        lo = mid + 1;
      else
        hi = mid;
    }
    double ir = event->items[lo].marginal;
    if (ir <= 0.0) {
      return 1.0;
    }
    r_indep *= (ir / group_rate);
  }
  if (r_indep <= 0.0) {
    return 1.0;
  }
  return p_rate / r_indep;
}

/* context for combination evaluation */
typedef struct {
  bishfpm_t *engine;
  bishfpm_group_t *grp;
  const prepared_event_t *event;
  uint64_t timestamp_ms;
  double group_rate;
  double weight;
  bishfpm_trigger_cb cb;
  void *udata;
  int triggered_count;
} bishfpm_eval_ctx_t;

static bool candidate_promoted(bishfpm_eval_ctx_t *ctx, size_t order,
                               topk_result_t update) {
  if (update.replaced)
    ctx->engine->stats.pattern_replacements++;
  ctx->engine->stats.candidates_evaluated++;
  if ((ctx->grp->pairs && !update.present) ||
      order < ctx->engine->cfg.min_itemset_order ||
      !(update.lower >= ctx->engine->cfg.min_pattern_rate))
    return false;
  double fraction = ctx->group_rate > 0 ? update.lower / ctx->group_rate : 0;
  return fraction >= ctx->engine->cfg.promotion_fraction;
}

static void notify_candidate(bishfpm_eval_ctx_t *ctx,
                             const char *const *subset_items, size_t subset_len,
                             topk_result_t update, const char *pattern,
                             uint64_t key) {
  char numeric_pattern[BISHFPM_MAX_PATTERN_LEN];
  double p_rate = update.lower;
  double frac = ctx->group_rate > 0.0 ? (p_rate / ctx->group_rate) : 0.0;

  /* check statistical lift threshold if configured */
  double lift = compute_subset_lift(ctx->event, subset_items, subset_len,
                                    p_rate, ctx->group_rate);
  if (ctx->engine->cfg.min_lift > 0.0 && lift < ctx->engine->cfg.min_lift) {
    ctx->engine->stats.triggers_suppressed++;
    return; /* lack of positive statistical correlation */
  }

  /* check subsumption by active subset alerts */
  if (is_candidate_subsumed(ctx->grp, subset_items, subset_len, p_rate,
                            ctx->timestamp_ms,
                            ctx->engine->cfg.trigger_cooldown_ms,
                            ctx->engine->cfg.subsumption_ratio)) {
    ctx->engine->stats.triggers_suppressed++;
    return; /* subsumed by active parent pattern */
  }

  /* Numeric callers pass the packed key and defer string serialization. */
  bool numeric = pattern == NULL;
  bool saturated = false;
  const void *identity = numeric ? (const void *)&key : (const void *)pattern;
  uint64_t phash = numeric ? compact_hash(key) : bishfpm_hash_str(pattern, 0);
  if (check_and_update_cooldown(
          ctx->grp, phash, identity, (uint32_t)subset_len, ctx->timestamp_ms,
          ctx->engine->cfg.trigger_cooldown_ms, &saturated)) {
    ctx->engine->stats.triggers_suppressed++;
    if (saturated)
      ctx->engine->stats.cooldown_saturation++;
    return; /* cooldown active */
  }

  if (numeric) {
    if (!bishfpm_itemset_serialize(subset_items, subset_len, numeric_pattern,
                                   sizeof(numeric_pattern)))
      return;
    pattern = numeric_pattern;
  }
  ctx->triggered_count++;
  ctx->engine->stats.triggers_emitted++;

  if (ctx->cb) {
    bishfpm_trigger_t trig;
    memset(&trig, 0, sizeof(trig));
    snprintf(trig.pattern, sizeof(trig.pattern), "%s", pattern);
    bishfpm_itemset_format(pattern, trig.formatted, sizeof(trig.formatted));
    trig.pattern_rate = p_rate;
    trig.pattern_upper_bound = update.upper;
    trig.pattern_error = update.error;
    trig.group_rate = ctx->group_rate;
    trig.rate_fraction = frac;
    trig.lift = lift;
    trig.item_count = (uint32_t)subset_len;
    trig.timestamp = ctx->timestamp_ms;

    ctx->engine->stats.callbacks_delivered++;
    ctx->cb(ctx->grp->group_key, &trig, ctx->udata);
  }
}

static void on_candidate_subset(const char *const *items, size_t n,
                                void *udata) {
  bishfpm_eval_ctx_t *ctx = udata;
  char pattern[BISHFPM_MAX_PATTERN_LEN];
  if (!bishfpm_itemset_serialize(items, n, pattern, sizeof(pattern)))
    return;
  topk_result_t result =
      topk_add_result(ctx->grp->topk, pattern, ctx->weight, ctx->timestamp_ms);
  if (candidate_promoted(ctx, n, result))
    notify_candidate(ctx, items, n, result, pattern, 0);
}
static int compare_lengths(const void *a, const void *b) {
  size_t x = *(const size_t *)a, y = *(const size_t *)b;
  return (x < y) - (x > y);
}

static int compare_prepared_items(const void *a, const void *b) {
  const prepared_item_t *x = a, *y = b;
  return strcmp(x->key, y->key);
}

/* Pure preparation: validates, canonicalizes and admits work before group
 * state. */
static int prepare_event(bishfpm_t *engine, const char *const *items,
                         size_t num_items, prepared_event_t *event,
                         rejection_reason_t *reason) {
  size_t valid = 0;
  for (size_t i = 0; i < num_items; i++) {
    if (!items[i] || !items[i][0])
      continue;
    size_t length = strnlen(items[i], BISHFPM_MAX_ITEM_LEN);
    if (length >= BISHFPM_MAX_ITEM_LEN || strchr(items[i], BISHFPM_ITEM_DELIM))
      return -1;
    event->items[valid++] = (prepared_item_t){items[i], length, 0, 0};
  }
  qsort(event->items, valid, sizeof(*event->items), compare_prepared_items);
  event->count = 0;
  for (size_t i = 0; i < valid; i++) {
    if (i && strcmp(event->items[i].key, event->items[i - 1].key) == 0)
      continue;
    size_t n = event->count++;
    event->items[n] = event->items[i];
    event->items[n].hash =
        bishfpm_hash_str(event->items[n].key, engine->cfg.hash_seed);
    event->keys[n] = event->items[n].key;
    event->lengths[n] = event->items[n].length;
  }
  if (!event->count)
    return 0;
  size_t max_k = event->count < engine->cfg.max_itemset_order
                     ? event->count
                     : engine->cfg.max_itemset_order;
  qsort(event->lengths, event->count, sizeof(*event->lengths), compare_lengths);
  size_t length = max_k - 1;
  for (size_t i = 0; i < max_k; i++)
    length += event->lengths[i];
  if (length >= BISHFPM_MAX_PATTERN_LEN) {
    *reason = REJECT_PATTERN_LENGTH;
    return -1;
  }
  uint64_t combinations = 1, work = 0;
  for (size_t k = 1; k <= max_k; k++) {
    combinations = combinations * (event->count - k + 1) / k;
    if (k >= engine->cfg.min_itemset_order)
      work += combinations;
    if (work > engine->cfg.max_candidates_per_event ||
        combinations > UINT64_MAX / (event->count + 1)) {
      *reason = REJECT_CANDIDATES;
      return -1;
    }
  }
  return 1;
}

int bishfpm_add_event_weighted(bishfpm_t *engine, const char *group_key,
                               uint64_t timestamp_ms, const char *const *items,
                               size_t num_items, uint32_t weight,
                               bishfpm_trigger_cb cb, void *udata) {
  if (!engine)
    return -1;
  engine->stats.events_total++;
  /* These exits never acquire or release the outer call's busy guard. */
  if (!group_key || (!items && num_items) || engine->busy) {
    engine->rejected[engine->busy ? REJECT_REENTRANT : REJECT_INVALID]++;
    engine->stats.events_rejected++;
    return -1;
  }
  if (num_items > engine->cfg.max_event_items) {
    engine->rejected[REJECT_EVENT_ITEMS]++;
    engine->stats.events_rejected++;
    engine->stats.budget_rejections++;
    return -1;
  }
  if (!group_key[0] || strnlen(group_key, BISHFPM_MAX_GROUP_KEY_LEN) >=
                           BISHFPM_MAX_GROUP_KEY_LEN) {
    engine->rejected[REJECT_INVALID]++;
    engine->stats.events_rejected++;
    return -1;
  }
  if (!num_items || !weight) {
    engine->stats.events_noop++;
    return 0;
  }
  engine->busy = true;
  bool budget_rejection = false;
  rejection_reason_t reason = REJECT_INVALID;
  int result = -1;
  prepared_event_t *event = &engine->workspace;
  int prepared = prepare_event(engine, items, num_items, event, &reason);
  budget_rejection = reason == REJECT_CANDIDATES;
  if (prepared <= 0) {
    result = prepared;
    if (!prepared)
      engine->stats.events_noop++;
    goto finalize;
  }

  /* Reserve all fallible group/marginal storage before committing counters. */
  size_t old_group_count = engine->group_count;
  reason = REJECT_ALLOCATION;
  bishfpm_group_t *grp = get_or_create_group(engine, group_key, &reason);
  if (!grp)
    goto finalize;
  bool forget_before_commit = false;
  if (engine->numeric_pairs) {
    uint32_t next = (uint32_t)grp->item_rates->count;
    for (size_t i = 0; i < event->count; i++) {
      bishfpm_rate_entry_t *entry = rate_table_find(
          grp->item_rates, event->keys[i], event->items[i].hash);
      event->ids[i] = entry ? entry->id : next++;
    }
    if (!compact_reserve(grp->pairs, event->ids, event->count,
                         effective_time(timestamp_ms, grp->max_time_ms),
                         &forget_before_commit)) {
      engine->pair_capacity_rejections++;
      reason = REJECT_CANDIDATES;
      budget_rejection = true;
      if (engine->group_count != old_group_count) {
        free_group_resources(grp);
        engine->group_count--;
      }
      goto finalize;
    }
  }
  rate_prepare_result_t reservation =
      rate_table_prepare_result(grp->item_rates, event->keys, event->count);
  if (reservation != RATE_PREPARED) {
    reason = reservation == RATE_LIMIT ? REJECT_MARGINALS : REJECT_ALLOCATION;
    if (engine->group_count != old_group_count) {
      free_group_resources(grp);
      engine->group_count--;
    }
    budget_rejection = true;
    goto finalize;
  }

  uint64_t now = effective_time(timestamp_ms, grp->max_time_ms);
  double effective_weight = bishfpm_rate_decay(
      (double)weight, now - timestamp_ms, engine->cfg.ewma_half_life_ms);
  if (forget_before_commit) {
    size_t forgotten =
        compact_prune(grp->pairs, now, engine->pair_options.forget_below);
    engine->pairs_forgotten += forgotten;
  }
  grp->max_time_ms = now;
  engine->stats.events_accepted++;
  double group_rate = bishfpm_rate_update_weighted(
      &grp->group_rate, now, engine->cfg.ewma_half_life_ms, effective_weight);
  size_t elevated = 0;
  for (size_t i = 0; i < event->count; i++) {
    prepared_item_t *item = &event->items[i];
    /* Reservation has finished rehashing; these lookups/updates cannot
     * allocate. */
    bishfpm_rate_entry_t *entry =
        rate_table_find(grp->item_rates, item->key, item->hash);
    double ir = bishfpm_rate_update_weighted(
        &entry->rate, now, engine->cfg.ewma_half_life_ms, effective_weight);
    item->marginal = ir;
    double share = group_rate > 0 ? ir / group_rate : 0;
    if (ir >= engine->cfg.item_rate_threshold &&
        (engine->cfg.item_share_threshold <= 0 ||
         share >= engine->cfg.item_share_threshold)) {
      event->ids[elevated] = event->ids[i];
      event->keys[elevated++] = item->key;
    }
  }

  result = 0;
  if (elevated >= engine->cfg.min_itemset_order) {
    size_t max_k = elevated < engine->cfg.max_itemset_order
                       ? elevated
                       : engine->cfg.max_itemset_order;
    bishfpm_eval_ctx_t ctx = {engine,           grp, event, now, group_rate,
                              effective_weight, cb,  udata, 0};
    /* Filtering preserves canonical order. Each update and its notification
     * finish before the next candidate; callbacks can query this exact state.
     */
    if (engine->numeric_pairs) {
      for (size_t i = 0; i < elevated; i++)
        for (size_t j = i + 1; j < elevated; j++) {
          uint64_t key = compact_key(event->ids[i], event->ids[j]);
          uint64_t hash = compact_hash(key);
          topk_result_t update =
              compact_add(grp->pairs, key, hash, effective_weight, now);
          if (candidate_promoted(&ctx, 2, update)) {
            const char *items[] = {event->keys[i], event->keys[j]};
            notify_candidate(&ctx, items, 2, update, NULL, key);
          }
        }
    } else {
      bishfpm_generate_subsets_order(event->keys, elevated,
                                     engine->cfg.min_itemset_order, max_k,
                                     on_candidate_subset, &ctx);
    }
    result = ctx.triggered_count;
  }
finalize:
  if (result < 0) {
    engine->rejected[reason]++;
    engine->stats.events_rejected++;
    if (budget_rejection)
      engine->stats.budget_rejections++;
  }
  engine->busy = false;
  return result;
}

/* ingest event with default weight 1 */
int bishfpm_add_event(bishfpm_t *engine, const char *group_key,
                      uint64_t timestamp_ms, const char *const *items,
                      size_t num_items, bishfpm_trigger_cb cb, void *udata) {
  return bishfpm_add_event_weighted(engine, group_key, timestamp_ms, items,
                                    num_items, 1, cb, udata);
}

double bishfpm_group_rate(bishfpm_t *engine, const char *key, uint64_t now_ms) {
  bishfpm_group_t *grp = find_group(engine, key, NULL);
  return grp ? bishfpm_rate_query(&grp->group_rate,
                                  effective_time(now_ms, grp->max_time_ms),
                                  engine->cfg.ewma_half_life_ms)
             : 0;
}

double bishfpm_item_rate(bishfpm_t *engine, const char *key, const char *item,
                         uint64_t now_ms) {
  bishfpm_group_t *grp = find_group(engine, key, NULL);
  return grp && item ? bishfpm_rate_table_query(
                           grp->item_rates, item,
                           effective_time(now_ms, grp->max_time_ms),
                           engine->cfg.ewma_half_life_ms)
                     : 0;
}

static bool lookup_pair(const bishfpm_t *engine, const bishfpm_group_t *group,
                        const char *pattern, uint64_t *key, uint64_t *hash) {
  size_t len = strnlen(pattern, BISHFPM_MAX_PATTERN_LEN);
  if (!len || len >= BISHFPM_MAX_PATTERN_LEN)
    return false;
  const char *split = strchr(pattern, BISHFPM_ITEM_DELIM);
  if (!split || split == pattern || !split[1] ||
      strchr(split + 1, BISHFPM_ITEM_DELIM) ||
      (size_t)(split - pattern) >= BISHFPM_MAX_ITEM_LEN ||
      strlen(split + 1) >= BISHFPM_MAX_ITEM_LEN)
    return false;
  char a[BISHFPM_MAX_ITEM_LEN];
  memcpy(a, pattern, (size_t)(split - pattern));
  a[split - pattern] = 0;
  if (strcmp(a, split + 1) >= 0)
    return false;
  bishfpm_rate_entry_t *x = rate_table_find(
      group->item_rates, a, bishfpm_hash_str(a, engine->cfg.hash_seed));
  bishfpm_rate_entry_t *y =
      rate_table_find(group->item_rates, split + 1,
                      bishfpm_hash_str(split + 1, engine->cfg.hash_seed));
  if (!x || !y) {
    *key = UINT64_MAX;
    *hash = 0;
    return true;
  }
  *key = compact_key(x->id, y->id);
  *hash = compact_hash(*key);
  return true;
}
int bishfpm_pattern_bounds(bishfpm_t *engine, const char *name,
                           const char *pattern, uint64_t now,
                           bishfpm_count_bounds_t *out) {
  if (!engine || !name || !pattern || !out || !pattern[0] ||
      strnlen(pattern, BISHFPM_MAX_PATTERN_LEN) >= BISHFPM_MAX_PATTERN_LEN)
    return 0;
  bishfpm_group_t *g = find_group(engine, name, NULL);
  *out = (bishfpm_count_bounds_t){0, 0, 0, 1};
  if (!g)
    return 1;
  now = effective_time(now, g->max_time_ms);
  topk_result_t r;
  if (g->pairs) {
    uint64_t key, hash;
    if (!lookup_pair(engine, g, pattern, &key, &hash))
      return 0;
    r = compact_query(g->pairs, key, hash, now);
  } else
    r = topk_query_result(g->topk, pattern, now);
  out->lower = r.lower;
  out->upper = r.upper;
  out->resident = r.present;
  out->known =
      r.present || (g->pairs && (g->pairs->kind == BISHFPM_PAIR_THRESHOLD ||
                                 g->pairs->kind == BISHFPM_PAIR_FSS));
  return 1;
}
void bishfpm_get_pair_stats(const bishfpm_t *engine,
                            bishfpm_pair_stats_t *out) {
  if (!engine || !out)
    return;
  *out = (bishfpm_pair_stats_t){engine->pair_capacity_rejections,
                                engine->pairs_forgotten, 0, 0};
  for (size_t i = 0; i < engine->group_cap; i++)
    if (engine->groups[i].group_key && engine->groups[i].pairs) {
      compact_t *p = engine->groups[i].pairs;
      out->resident_pairs += p->heap_size;
      out->store_bytes += compact_bytes(p->k, p->sketch_width, p->kind);
    }
}

double bishfpm_pattern_rate(bishfpm_t *engine, const char *key,
                            const char *pattern, uint64_t now_ms) {
  bishfpm_group_t *grp = find_group(engine, key, NULL);
  if (grp && grp->pairs) {
    bishfpm_count_bounds_t bounds;
    return bishfpm_pattern_bounds(engine, key, pattern, now_ms, &bounds)
               ? bounds.upper
               : 0;
  }
  return grp && pattern
             ? bishfpm_topk_query(grp->topk, pattern,
                                  effective_time(now_ms, grp->max_time_ms))
             : 0;
}

/* Retain numeric identities, but preserve legacy marginal forgetting/gating.
 * A dormant dictionary key has a reset rate and is not counted twice as pruned.
 */
static size_t prune_numeric_marginals(bishfpm_rate_table_t *table, uint64_t now,
                                      double half, double cutoff) {
  size_t removed = 0;
  for (size_t i = 0; i < table->capacity; i++)
    if (table->entries[i].key) {
      bishfpm_rate_t *rate = &table->entries[i].rate;
      double value = bishfpm_rate_query(rate, now, half);
      if (value < cutoff) {
        removed += rate->initialized;
        bishfpm_rate_init(rate);
      } else {
        rate->rate = value;
        rate->last_time_ms = now;
        rate->initialized = true;
      }
    }
  return removed;
}

size_t bishfpm_prune_pairs(bishfpm_t *engine, const char *group_key,
                           uint64_t now_ms, size_t max_pairs) {
  if (!engine || !group_key || !max_pairs || engine->busy ||
      !engine->numeric_pairs)
    return 0;
  bishfpm_group_t *group = find_group(engine, group_key, NULL);
  if (!group)
    return 0;
  engine->busy = true;
  uint64_t now = effective_time(now_ms, group->max_time_ms);
  group->group_rate.rate = bishfpm_rate_query(&group->group_rate, now,
                                              engine->cfg.ewma_half_life_ms);
  group->group_rate.last_time_ms = now;
  group->max_time_ms = now;
  size_t forgotten = compact_prune_bounded(group->pairs, now, max_pairs);
  engine->pairs_forgotten += forgotten;
  engine->busy = false;
  return forgotten;
}

/* prune inactive groups and items */
size_t bishfpm_prune(bishfpm_t *engine, uint64_t now_ms, double min_rate) {
  if (!engine || engine->group_count == 0 || !isfinite(min_rate) ||
      min_rate <= 0) {
    return 0;
  }
  if (engine->busy) {
    return 0;
  }
  engine->busy = true;

  size_t pruned = 0;
  size_t survivors = 0;

  for (size_t i = 0; i < engine->group_cap; i++) {
    if (!engine->groups[i].group_key) {
      continue;
    }
    double gr = bishfpm_rate_query(
        &engine->groups[i].group_rate,
        effective_time(now_ms, engine->groups[i].max_time_ms),
        engine->cfg.ewma_half_life_ms);
    if (gr < min_rate) {
      pruned++;
    } else {
      survivors++;
    }
  }

  size_t new_cap =
      next_pow2(survivors * 2 < INITIAL_GROUP_CAPACITY ? INITIAL_GROUP_CAPACITY
                                                       : survivors * 2);
  bishfpm_group_t *new_groups = calloc(new_cap, sizeof(bishfpm_group_t));
  if (!new_groups) {
    engine->busy = false;
    return 0;
  }

  size_t mask = new_cap - 1;
  for (size_t i = 0; i < engine->group_cap; i++) {
    if (!engine->groups[i].group_key) {
      continue;
    }
    double gr = bishfpm_rate_query(
        &engine->groups[i].group_rate,
        effective_time(now_ms, engine->groups[i].max_time_ms),
        engine->cfg.ewma_half_life_ms);
    if (gr < min_rate) {
      if (engine->groups[i].item_rates) {
        engine->stats.items_pruned += engine->groups[i].item_rates->count;
      }
      free_group_resources(&engine->groups[i]);
      engine->stats.groups_pruned++;
    } else {
      uint64_t effective =
          effective_time(now_ms, engine->groups[i].max_time_ms);
      engine->groups[i].max_time_ms = effective;
      engine->groups[i].group_rate.rate = gr;
      engine->groups[i].group_rate.last_time_ms = effective;
      size_t ip =
          engine->numeric_pairs
              ? prune_numeric_marginals(engine->groups[i].item_rates, effective,
                                        engine->cfg.ewma_half_life_ms, min_rate)
              : bishfpm_rate_table_prune(
                    engine->groups[i].item_rates, effective,
                    engine->cfg.ewma_half_life_ms, min_rate);
      engine->stats.items_pruned += ip;
      if (engine->groups[i].pairs) {
        engine->pairs_forgotten +=
            compact_prune(engine->groups[i].pairs, effective, min_rate);
      }
      if (engine->groups[i].topk) {
        bishfpm_topk_prune(engine->groups[i].topk, effective, min_rate);
      }
      size_t idx = (size_t)(engine->groups[i].hash & mask);
      while (new_groups[idx].group_key) {
        idx = (idx + 1) & mask;
      }
      new_groups[idx] = engine->groups[i];
    }
  }

  free(engine->groups);
  engine->groups = new_groups;
  engine->group_cap = new_cap;
  engine->group_count = survivors;
  engine->busy = false;

  return pruned;
}

/* query engine statistics */
void bishfpm_get_stats(const bishfpm_t *engine, bishfpm_stats_t *out) {
  if (!engine || !out) {
    return;
  }
  *out = engine->stats;
  out->active_groups = engine->group_count;
  out->memory_current_bytes = sizeof(*engine) +
                              engine->group_cap * sizeof(*engine->groups) +
                              workspace_bytes(engine->cfg.max_event_items);
  for (size_t i = 0; i < engine->group_cap; i++) {
    const bishfpm_group_t *g = &engine->groups[i];
    if (!g->group_key)
      continue;
    out->memory_current_bytes +=
        strlen(g->group_key) + 1 + sizeof(*g->item_rates) +
        g->item_rates->capacity * sizeof(*g->item_rates->entries) +
        (g->arena ? sizeof(*g->arena) + (size_t)(g->arena->end - g->arena->base)
                  : 0) +
        (g->cooldown_cap + g->cooldown_spare_cap) *
            cooldown_entry_size(g->pairs != NULL);
    if (g->pairs)
      out->memory_current_bytes +=
          compact_bytes(g->pairs->k, g->pairs->sketch_width, g->pairs->kind);
    if (g->topk && !g->topk->arena_backed)
      out->memory_current_bytes += bishfpm_topk_storage_bytes(g->topk);
    for (size_t j = 0; j < g->item_rates->capacity; j++)
      if (g->item_rates->entries[j].key)
        out->memory_current_bytes += strlen(g->item_rates->entries[j].key) + 1;
  }
}

void bishfpm_get_rejection_stats(const bishfpm_t *engine,
                                 bishfpm_rejection_stats_t *out) {
  if (!engine || !out)
    return;
  *out = (bishfpm_rejection_stats_t){engine->rejected[REJECT_INVALID],
                                     engine->rejected[REJECT_REENTRANT],
                                     engine->rejected[REJECT_EVENT_ITEMS],
                                     engine->rejected[REJECT_PATTERN_LENGTH],
                                     engine->rejected[REJECT_CANDIDATES],
                                     engine->rejected[REJECT_GROUPS],
                                     engine->rejected[REJECT_MARGINALS],
                                     engine->rejected[REJECT_ALLOCATION]};
}

/* query library version */
const char *bishfpm_version(void) { return BISHFPM_VERSION_STRING; }
