#include "bishfpm.h"
#include "internal.h"
#include "store_plugin.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_GROUP_CAPACITY 64
#define INITIAL_MARGINAL_CAPACITY 64
#define COOLDOWN_GROWTH_LIMIT 4096

/* Sorted unique metadata is compacted with the elevated key workspace. */
typedef struct {
  const char *key;
  size_t length;
  uint64_t hash;
  double marginal;
  bishfpm_rate_entry_t *entry;
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
  bishfpm_memory_budget_t *budget;
  uint64_t hash;
  bishfpm_rate_t group_rate;
  /* upper bound on pattern mass omitted by gates or rejected updates */
  bishfpm_rate_t quality_omitted;
  bishfpm_rate_table_t *item_rates;
  store_t store;
  void *cooldowns;
  size_t cooldown_cap, cooldown_count;
  /* Lower bound on the oldest trigger; refreshed during maintenance. */
  uint64_t cooldown_oldest_ms;
  uint64_t max_time_ms;
  bishfpm_pattern_pressure_t pressure;
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
  bishfpm_options_t options;
  bishfpm_memory_budget_t memory;
  bool numeric_pairs;
  store_config_t store_config;
  uint64_t pair_capacity_rejections, pairs_forgotten;
  bishfpm_group_t *groups;
  size_t group_cap;
  size_t group_count;
  prepared_event_t workspace;
  bishfpm_stats_t stats;
  double min_partition_lift;
  bool conservative_subsumption;
  uint32_t quality_min_order, quality_max_order;
  bishfpm_partition_lift_stats_t partition_lift_stats;
  bishfpm_pattern_pressure_t pressure; /* Counters from removed groups only. */
  uint64_t rejected[REJECT_REASON_COUNT];
  bool busy;
};

static void pressure_add(bishfpm_pattern_pressure_t *to,
                         const bishfpm_pattern_pressure_t *from) {
  to->candidates_evaluated += from->candidates_evaluated;
  to->resident_updates += from->resident_updates;
  to->admissions += from->admissions;
  to->replacements += from->replacements;
  to->filtered_updates += from->filtered_updates;
  to->rejected_updates += from->rejected_updates;
  to->zero_weight_queries += from->zero_weight_queries;
}

static bishfpm_memory_budget_t *engine_budget(bishfpm_t *e) {
  return e->options.memory_mode == BISHFPM_MEMORY_DYNAMIC ? &e->memory : NULL;
}

void bishfpm_options_default(bishfpm_options_t *o) {
  if (o)
    *o = (bishfpm_options_t){BISHFPM_STORE_SS, 0, BISHFPM_MEMORY_CONSERVATIVE};
}

static bool conservative_subsumption_enabled(const bishfpm_t *engine) {
  return engine->conservative_subsumption && !engine->numeric_pairs &&
         engine->cfg.subsumption_ratio > 0 &&
         engine->cfg.trigger_cooldown_ms > 0 &&
         engine->cfg.max_itemset_order > engine->cfg.min_itemset_order;
}

/* tracked omission orders always form one contiguous interval */
static void refresh_quality_orders(bishfpm_t *e) {
  uint32_t lo = UINT32_MAX, hi = 0;
  uint32_t min_k = e->cfg.min_itemset_order, max_k = e->cfg.max_itemset_order;
  if (!e->numeric_pairs && e->min_partition_lift > 0) {
    uint32_t plo = min_k > 3 ? min_k : 3;
    uint32_t phi = max_k < 4 ? max_k : 4;
    if (plo <= phi) {
      lo = plo;
      hi = phi;
    }
  }
  if (conservative_subsumption_enabled(e)) {
    uint32_t slo = min_k + 1;
    if (slo < lo)
      lo = slo;
    if (max_k > hi)
      hi = max_k;
  }
  e->quality_min_order = lo;
  e->quality_max_order = hi;
}

int bishfpm_set_min_partition_lift(bishfpm_t *engine, double threshold) {
  if (!engine || engine->busy || engine->stats.events_accepted ||
      !isfinite(threshold) || threshold < 0.0)
    return 0;
  engine->min_partition_lift = threshold;
  refresh_quality_orders(engine);
  return 1;
}

int bishfpm_set_conservative_subsumption(bishfpm_t *engine, int enabled) {
  if (!engine || engine->busy || engine->stats.events_accepted ||
      (enabled != 0 && enabled != 1))
    return 0;
  engine->conservative_subsumption = enabled != 0;
  refresh_quality_orders(engine);
  return 1;
}

void bishfpm_get_partition_lift_stats(const bishfpm_t *engine,
                                      bishfpm_partition_lift_stats_t *out) {
  if (engine && out)
    *out = engine->partition_lift_stats;
}

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

/* Upper bound on library-requested payload, including concurrent old/new
 * tables during reservation, pruning and cooldown growth; allocator overhead
 * and caller buffers are outside this budget. Limits above bound each factor.
 */
static size_t memory_bound(const bishfpm_config_t *c,
                           const store_config_t *store) {
  size_t store_bytes = store->ops->bytes(store);
  if (store_bytes == SIZE_MAX)
    return SIZE_MAX;
  size_t slot_cap = next_pow2((size_t)c->topk_k * 2);
  size_t rate_cap = next_pow2((size_t)c->max_items_per_group * 2);
  if (rate_cap < INITIAL_MARGINAL_CAPACITY)
    rate_cap = INITIAL_MARGINAL_CAPACITY;
  size_t group_cap = next_pow2((size_t)c->max_groups * 2);
  if (group_cap < INITIAL_GROUP_CAPACITY)
    group_cap = INITIAL_GROUP_CAPACITY;
  size_t staged = c->max_event_items < c->max_items_per_group
                      ? c->max_event_items
                      : c->max_items_per_group;
  size_t cooldown_cap =
      slot_cap > COOLDOWN_GROWTH_LIMIT ? slot_cap : COOLDOWN_GROWTH_LIMIT;
  /* Include simultaneous old/new tables, all final key copies and the
   * bounded staging array. Pruning/growth never exceeds these capacities. */
  uint64_t per_group =
      BISHFPM_MAX_GROUP_KEY_LEN + sizeof(bishfpm_rate_table_t) +
      2 * (uint64_t)rate_cap * sizeof(bishfpm_rate_entry_t) +
      (uint64_t)c->max_items_per_group * BISHFPM_MAX_ITEM_LEN +
      (uint64_t)staged * sizeof(bishfpm_rate_entry_t) +
      2 * (uint64_t)cooldown_cap *
          (cooldown_entry_size(store->ops->pairs) + 2 * sizeof(uint32_t));
  uint64_t total = sizeof(bishfpm_t) + workspace_bytes(c->max_event_items) +
                   2 * (uint64_t)group_cap * sizeof(bishfpm_group_t) +
                   per_group * c->max_groups;
  size_t result, stores;
  if (total > SIZE_MAX || !size_mul(store_bytes, c->max_groups, &stores) ||
      !size_add((size_t)total, stores, &result))
    return SIZE_MAX;
  if (store->extension) {
    store_extension_t *x = store->extension;
    if (!size_add(result, sizeof(*x), &result) ||
        !size_add(result, x->plan.engine_peak_bytes, &result))
      return SIZE_MAX;
  }
  return result;
}

/* create engine instance */
static bishfpm_t *create_internal(const bishfpm_config_t *cfg,
                                  const bishfpm_pair_options_t *pairs,
                                  const bishfpm_options_t *options,
                                  store_extension_t *extension) {
  bishfpm_options_t o;
  bishfpm_options_default(&o);
  if (options)
    o = *options;
  if ((o.store != BISHFPM_STORE_SS && o.store != BISHFPM_STORE_FSS) ||
      (o.memory_mode != BISHFPM_MEMORY_CONSERVATIVE &&
       o.memory_mode != BISHFPM_MEMORY_DYNAMIC) ||
      o.sketch_width > (1U << 24) ||
      (o.store == BISHFPM_STORE_FSS ? !o.sketch_width : o.sketch_width != 0) ||
      (pairs && o.store != BISHFPM_STORE_SS))
    return NULL;
  bishfpm_config_t c;
  if (cfg) {
    c = *cfg;
  } else {
    bishfpm_config_default(&c);
  }

  if (!validate_config(&c)) {
    return NULL;
  }

  store_config_t selected = {0};
  selected.ops = extension ? store_extension_ops(extension)
                 : pairs   ? &store_pair_ops
                           : &store_text_ops;
  selected.extension = extension;
  selected.mining = &c;
  selected.sketch_width = o.sketch_width;
  if (pairs)
    selected.pairs = *pairs;
  if (!selected.ops->validate(&selected))
    return NULL;
  size_t bound = o.memory_mode == BISHFPM_MEMORY_DYNAMIC
                     ? sizeof(bishfpm_t) +
                           INITIAL_GROUP_CAPACITY * sizeof(bishfpm_group_t) +
                           workspace_bytes(c.max_event_items)
                     : memory_bound(&c, &selected);
  if (bound == SIZE_MAX || bound > c.max_memory_bytes)
    return NULL;

  bishfpm_t *engine = calloc(1, sizeof(bishfpm_t));
  if (!engine) {
    return NULL;
  }

  engine->cfg = c;
  engine->options = o;
  engine->quality_min_order = UINT32_MAX;
  engine->memory.limit = c.max_memory_bytes;
  engine->memory.current = engine->memory.peak = sizeof(*engine);
  engine->numeric_pairs = selected.ops->pairs;
  engine->store_config = selected;
  engine->store_config.mining = &engine->cfg;
  bishfpm_memory_budget_t *budget = engine_budget(engine);
  engine->busy = false; /* construction has not published the engine */
  engine->stats.memory_bound_bytes =
      o.memory_mode == BISHFPM_MEMORY_DYNAMIC ? c.max_memory_bytes : bound;
  if (extension) {
    store_extension_t *owned = memory_calloc(budget, 1, sizeof(*owned));
    if (!owned) {
      memory_free(budget, engine, sizeof(*engine));
      return NULL;
    }
    *owned = *extension;
    engine->store_config.extension = owned;
    if (!store_extension_init(owned, budget, &engine->cfg, extension->config,
                              extension->bytes)) {
      /* failed clones clean their own allocations */
      memory_free(budget, owned, sizeof(*owned));
      engine->store_config.extension = NULL;
      memory_free(budget, engine, sizeof(*engine));
      return NULL;
    }
  }
  engine->group_cap = INITIAL_GROUP_CAPACITY;
  engine->groups =
      memory_calloc(budget, engine->group_cap, sizeof(bishfpm_group_t));
  if (!engine->groups) {
    bishfpm_free(engine);
    return NULL;
  }

  engine->workspace.items =
      memory_calloc(budget, c.max_event_items, sizeof(prepared_item_t));
  engine->workspace.keys =
      memory_calloc(budget, c.max_event_items, sizeof(const char *));
  engine->workspace.lengths =
      memory_calloc(budget, c.max_event_items, sizeof(size_t));
  engine->workspace.ids =
      memory_calloc(budget, c.max_event_items, sizeof(uint32_t));
  if (!engine->workspace.items || !engine->workspace.keys ||
      !engine->workspace.lengths || !engine->workspace.ids) {
    bishfpm_free(engine);
    return NULL;
  }

  return engine;
}

static bishfpm_t *create_options(const bishfpm_config_t *c,
                                 const bishfpm_pair_options_t *p,
                                 const bishfpm_options_t *o) {
  return create_internal(c, p, o, NULL);
}

bishfpm_t *
bishfpm_create_with_store(const bishfpm_config_t *cfg,
                          const bishfpm_store_engine_options_t *options,
                          const bishfpm_store_descriptor_t *descriptor,
                          const void *settings, size_t size) {
  bishfpm_config_t c;
  if (cfg)
    c = *cfg;
  else
    bishfpm_config_default(&c);
  bishfpm_store_engine_options_t defaults;
  bishfpm_store_engine_options_default(&defaults);
  const bishfpm_store_engine_options_t *o = options ? options : &defaults;
  if (o->abi_version != BISHFPM_STORE_ABI_V1 || o->struct_size < sizeof(*o))
    return NULL;
  c.topk_k = o->notification_capacity;
  if (!validate_config(&c))
    return NULL;
  store_extension_t extension;
  if (!store_extension_validate(descriptor, o, &c, settings, size, &extension))
    return NULL;
  /* raw settings live only through synchronous construction */
  extension.config = (void *)settings;
  extension.bytes = size;
  bishfpm_options_t engine_options = {BISHFPM_STORE_SS, 0,
                                      (bishfpm_memory_mode_t)o->memory_mode};
  return create_internal(&c, NULL, &engine_options, &extension);
}

bishfpm_t *bishfpm_create_pairs(const bishfpm_config_t *cfg,
                                const bishfpm_pair_options_t *pairs) {
  return pairs ? create_options(cfg, pairs, NULL) : NULL;
}

bishfpm_t *bishfpm_create(const bishfpm_config_t *cfg) {
  return create_options(cfg, NULL, NULL);
}

bishfpm_t *bishfpm_create_with_options(const bishfpm_config_t *c,
                                       const bishfpm_options_t *o) {
  return create_options(c, NULL, o);
}
bishfpm_t *bishfpm_create_pairs_with_options(const bishfpm_config_t *c,
                                             const bishfpm_pair_options_t *p,
                                             const bishfpm_options_t *o) {
  return p ? create_options(c, p, o) : NULL;
}

/* free single group resources */
static void free_group_resources(bishfpm_group_t *grp) {
  if (!grp || !grp->group_key) {
    return;
  }
  bool numeric = store_is_pairs(&grp->store);
  memory_free(grp->budget, grp->group_key, strlen(grp->group_key) + 1);
  if (grp->item_rates) {
    bishfpm_rate_table_free(grp->item_rates);
  }
  if (grp->store.config)
    grp->store.config->ops->destroy(&grp->store);
  if (grp->cooldowns) {
    memory_free(grp->budget, grp->cooldowns,
                grp->cooldown_cap *
                    (cooldown_entry_size(numeric) + 2 * sizeof(uint32_t)));
  }
  memset(grp, 0, sizeof(bishfpm_group_t));
}

/* free engine instance */
void bishfpm_free(bishfpm_t *engine) {
  if (!engine || engine->busy) {
    return;
  }
  bishfpm_memory_budget_t *budget = engine_budget(engine);
  if (engine->groups) {
    for (size_t i = 0; i < engine->group_cap; i++) {
      if (engine->groups[i].group_key) {
        free_group_resources(&engine->groups[i]);
      }
    }
    memory_free(budget, engine->groups,
                engine->group_cap * sizeof(*engine->groups));
  }
  memory_free(budget, engine->workspace.items,
              engine->cfg.max_event_items * sizeof(prepared_item_t));
  memory_free(budget, engine->workspace.keys,
              engine->cfg.max_event_items * sizeof(const char *));
  memory_free(budget, engine->workspace.lengths,
              engine->cfg.max_event_items * sizeof(size_t));
  memory_free(budget, engine->workspace.ids,
              engine->cfg.max_event_items * sizeof(uint32_t));
  if (engine->store_config.extension) {
    store_extension_t *x = engine->store_config.extension;
    x->descriptor.destroy_config(&x->host, x->config);
    assert(x->bytes == 0);
    memory_free(budget, x, sizeof(*x));
  }
  memory_free(budget, engine, sizeof(*engine));
}

/* resize group hash table */
static bool resize_groups(bishfpm_t *engine, size_t new_cap) {
  if (!new_cap || new_cap > SIZE_MAX / sizeof(bishfpm_group_t))
    return false;
  bishfpm_group_t *new_groups =
      memory_calloc(engine_budget(engine), new_cap, sizeof(bishfpm_group_t));
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

  memory_free(engine_budget(engine), engine->groups,
              engine->group_cap * sizeof(*engine->groups));
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
  grp->budget = engine_budget(engine);
  grp->store.config = &engine->store_config;
  grp->group_key = memory_strdup(grp->budget, group_key);
  if (!grp->group_key) {
    return NULL;
  }
  grp->hash = h;
  bishfpm_rate_init(&grp->group_rate);

  /* allocate item rate table */
  grp->item_rates = rate_table_new_budget(INITIAL_MARGINAL_CAPACITY,
                                          engine->cfg.max_items_per_group,
                                          engine->cfg.hash_seed, grp->budget);
  if (!grp->item_rates) {
    free_group_resources(grp);
    return NULL;
  }

  if (!engine->store_config.ops->create(&grp->store, grp->budget)) {
    free_group_resources(grp);
    return NULL;
  }
  grp->cooldown_cap = next_pow2(engine->cfg.topk_k * 2);
  grp->cooldowns = memory_calloc(
      grp->budget, grp->cooldown_cap,
      cooldown_entry_size(store_is_pairs(&grp->store)) + 2 * sizeof(uint32_t));
  if (!grp->cooldowns) {
    free_group_resources(grp);
    return NULL;
  }

  engine->group_count++;
  return grp;
}

_Static_assert(sizeof(text_cooldown_t) % _Alignof(uint32_t) == 0 &&
                   sizeof(pair_cooldown_t) % _Alignof(uint32_t) == 0,
               "cooldown entry size must be a multiple of uint32_t alignment");

/* Identities are dense; a separate index keeps <=50% load even at saturation.
 * The index shares the allocation, after the aligned entry array. */
static uint32_t *cooldown_index(void *table, size_t capacity, size_t stride) {
  return (uint32_t *)((char *)table + capacity * stride);
}

static void cooldown_index_insert(void *table, size_t capacity, size_t stride,
                                  size_t entry_index) {
  uint32_t *index = cooldown_index(table, capacity, stride);
  size_t mask = 2 * capacity - 1;
  size_t slot = cooldown_entry(table, entry_index, stride)->hash & mask;
  while (index[slot])
    slot = (slot + 1) & mask;
  index[slot] = (uint32_t)entry_index + 1;
}

/* Read-only identity lookup: callers decide whether to refresh the timestamp.
 */
static bishfpm_cooldown_entry_t *
find_cooldown(bishfpm_group_t *grp, uint64_t hash, const void *identity) {
  bool numeric = store_is_pairs(&grp->store);
  size_t stride = cooldown_entry_size(numeric);
  size_t mask = 2 * grp->cooldown_cap - 1;
  uint32_t *index = cooldown_index(grp->cooldowns, grp->cooldown_cap, stride);
  size_t slot = hash & mask;
  while (index[slot]) {
    bishfpm_cooldown_entry_t *entry =
        cooldown_entry(grp->cooldowns, index[slot] - 1, stride);
    if (entry->hash == hash && cooldown_matches(entry, identity, numeric))
      return entry;
    slot = (slot + 1) & mask;
  }
  return NULL;
}

/* expire entries in place without allocating; only allocate on growth */
static size_t cooldown_maintain(bishfpm_group_t *grp, size_t capacity,
                                uint64_t now_ms, uint64_t cooldown_ms,
                                bool expire) {
  size_t stride = cooldown_entry_size(store_is_pairs(&grp->store));
  uint64_t oldest = UINT64_MAX;
  uint64_t ref_time = effective_time(now_ms, grp->max_time_ms);
  size_t count = 0;
  for (size_t i = 0; i < grp->cooldown_count; i++) {
    const bishfpm_cooldown_entry_t *entry =
        cooldown_entry(grp->cooldowns, i, stride);
    if (expire && ref_time >= entry->last_trigger_ms &&
        ref_time - entry->last_trigger_ms >= cooldown_ms)
      continue;
    if (entry->last_trigger_ms < oldest)
      oldest = entry->last_trigger_ms;
    if (count != i)
      memcpy(cooldown_entry(grp->cooldowns, count, stride), entry, stride);
    count++;
  }
  if (capacity == grp->cooldown_cap) {
    if (count != grp->cooldown_count) {
      memset(cooldown_index(grp->cooldowns, capacity, stride), 0,
             2 * capacity * sizeof(uint32_t));
      for (size_t i = 0; i < count; i++)
        cooldown_index_insert(grp->cooldowns, capacity, stride, i);
      grp->cooldown_count = count;
    }
    grp->cooldown_oldest_ms = oldest;
    return count;
  }
  size_t bytes_per_entry = stride + 2 * sizeof(uint32_t);
  void *table = memory_calloc(grp->budget, capacity, bytes_per_entry);
  if (!table) {
    if (count != grp->cooldown_count) {
      memset(cooldown_index(grp->cooldowns, grp->cooldown_cap, stride), 0,
             2 * grp->cooldown_cap * sizeof(uint32_t));
      for (size_t i = 0; i < count; i++)
        cooldown_index_insert(grp->cooldowns, grp->cooldown_cap, stride, i);
      grp->cooldown_count = count;
    }
    grp->cooldown_oldest_ms = oldest;
    return count;
  }
  if (count) {
    memcpy(table, grp->cooldowns, count * stride);
    for (size_t i = 0; i < count; i++)
      cooldown_index_insert(table, capacity, stride, i);
  }
  memory_free(grp->budget, grp->cooldowns, grp->cooldown_cap * bytes_per_entry);
  grp->cooldowns = table;
  grp->cooldown_cap = capacity;
  grp->cooldown_count = count;
  grp->cooldown_oldest_ms = oldest;
  return count;
}

/* Check cooldown and record notification identity independently of residency.
 */
static bool check_and_update_cooldown(bishfpm_group_t *grp, uint64_t hash,
                                      const void *identity, uint32_t item_count,
                                      uint64_t now_ms, uint64_t cooldown_ms,
                                      bool *saturated) {
  if (!grp->cooldowns || !grp->cooldown_cap || !cooldown_ms)
    return false;
  if (now_ms > grp->max_time_ms)
    grp->max_time_ms = now_ms;
  uint64_t ref_time = grp->max_time_ms;
  bool numeric = store_is_pairs(&grp->store);
  size_t stride = cooldown_entry_size(numeric);
  bishfpm_cooldown_entry_t *existing = find_cooldown(grp, hash, identity);
  if (existing) {
    if (now_ms < existing->last_trigger_ms ||
        now_ms - existing->last_trigger_ms < cooldown_ms)
      return true;
    existing->last_trigger_ms = ref_time;
    existing->item_count = item_count;
    return false;
  }

  bool can_expire = grp->cooldown_count &&
                    ref_time >= grp->cooldown_oldest_ms &&
                    ref_time - grp->cooldown_oldest_ms >= cooldown_ms;
  if (grp->cooldown_count * 10 >= grp->cooldown_cap * 7) {
    if (can_expire)
      cooldown_maintain(grp, grp->cooldown_cap, ref_time, cooldown_ms, true);
    if (grp->cooldown_count * 10 >= grp->cooldown_cap * 7 &&
        grp->cooldown_cap < COOLDOWN_GROWTH_LIMIT)
      cooldown_maintain(grp, grp->cooldown_cap * 2, ref_time, cooldown_ms,
                        false);
  }

  if (grp->cooldown_count == grp->cooldown_cap) {
    *saturated = true;
    return true;
  }
  size_t entry_index = grp->cooldown_count++;
  bishfpm_cooldown_entry_t *entry =
      cooldown_entry(grp->cooldowns, entry_index, stride);
  entry->hash = hash;
  cooldown_set_identity(entry, identity, numeric);
  entry->last_trigger_ms = ref_time;
  entry->item_count = item_count;
  if (grp->cooldown_count == 1 || ref_time < grp->cooldown_oldest_ms)
    grp->cooldown_oldest_ms = ref_time;
  cooldown_index_insert(grp->cooldowns, grp->cooldown_cap, stride, entry_index);
  return false;
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

/* keep equality and near-boundary floating-point results eligible */
static bool conservatively_below(double upper, double threshold) {
  upper = nextafter(upper, INFINITY);
  return upper < threshold && threshold - upper > 64 * DBL_EPSILON * threshold;
}

static bool subset_suppresses(const bishfpm_group_t *grp, const char *pattern,
                              uint64_t now, double candidate_rate, double ratio,
                              bool conservative) {
  store_result_t parent =
      grp->store.config->ops->query_text(&grp->store, pattern, now);
  double lower = parent.count.lower;
  if (!conservative)
    return lower > 0 && candidate_rate < ratio * lower;
  return parent.known && isfinite(lower) && lower > 0 &&
         conservatively_below(candidate_rate, ratio * lower);
}

static size_t proper_subset_probes(size_t n, size_t min_order) {
  if (n > 8)
    return SIZE_MAX;
  size_t binom = 1, excluded = 1;
  for (size_t k = 1; k < min_order; k++) {
    binom = binom * (n - k + 1) / k;
    excluded += binom;
  }
  return ((size_t)1 << n) - 1 - excluded;
}

/* conservative mode passes a certified full-history upper bound; legacy mode
 * compares lower counts */
static bool is_candidate_subsumed(bishfpm_group_t *grp,
                                  const char *const *subset_items,
                                  const size_t *item_lengths, size_t subset_len,
                                  double candidate_rate, uint64_t now_ms,
                                  uint64_t cooldown_ms,
                                  double subsumption_ratio, size_t min_order,
                                  uint64_t hash_seed, bool conservative) {
  if (store_is_pairs(&grp->store) || subsumption_ratio <= 0.0 || !cooldown_ms ||
      !grp->cooldowns || !grp->cooldown_count || subset_len <= min_order) {
    return false;
  }
  size_t k_min = min_order ? min_order : 1;
  if (grp->cooldown_count > proper_subset_probes(subset_len, k_min)) {
    size_t local_lengths[8];
    if (!item_lengths) {
      for (size_t i = 0; i < subset_len; i++)
        local_lengths[i] = strlen(subset_items[i]);
      item_lengths = local_lengths;
    }
    char sub[BISHFPM_MAX_PATTERN_LEN];
    unsigned limit = (1u << subset_len) - 1;
    for (unsigned mask = (1u << k_min) - 1; mask < limit; mask++) {
      size_t bits = 0;
      for (unsigned m = mask; m; m &= m - 1)
        bits++;
      if (bits < k_min)
        continue;
      size_t length = 0;
      for (size_t i = 0; i < subset_len; i++) {
        if (!(mask & (1u << i)))
          continue;
        if (length)
          sub[length++] = BISHFPM_ITEM_DELIM;
        memcpy(sub + length, subset_items[i], item_lengths[i]);
        length += item_lengths[i];
      }
      sub[length] = '\0';
      uint64_t hash = bishfpm_murmur3_64(sub, length, hash_seed);
      const bishfpm_cooldown_entry_t *entry = find_cooldown(grp, hash, sub);
      if (!entry || (now_ms >= entry->last_trigger_ms &&
                     now_ms - entry->last_trigger_ms >= cooldown_ms))
        continue;
      if (subset_suppresses(grp, sub, now_ms, candidate_rate, subsumption_ratio,
                            conservative))
        return true;
    }
    return false;
  }
  for (size_t i = 0; i < grp->cooldown_count; i++) {
    const text_cooldown_t *slot = (const text_cooldown_t *)grp->cooldowns + i;
    if (now_ms >= slot->entry.last_trigger_ms &&
        now_ms - slot->entry.last_trigger_ms >= cooldown_ms) {
      continue; /* expired */
    }
    if (slot->entry.item_count < subset_len) {
      if (is_pattern_subset(slot->pattern, subset_items, subset_len)) {
        if (subset_suppresses(grp, slot->pattern, now_ms, candidate_rate,
                              subsumption_ratio, conservative)) {
          return true; /* subsumed by active parent pattern */
        }
      }
    }
  }
  return false;
}

/* compute statistical lift of candidate subset */
static double compute_subset_lift(size_t subset_len, double p_rate,
                                  double group_rate, const double *marginals) {
  if (subset_len < 2 || group_rate <= 0.0 || p_rate <= 0.0) {
    return 1.0;
  }
  double r_indep = group_rate;
  bool needs_scaling = false;
  for (size_t i = 0; i < subset_len; i++) {
    double ir = marginals[i];
    if (ir <= 0.0) {
      return 1.0;
    }
    double share = ir / group_rate;
    r_indep *= share;
    if (!isnormal(share) || !isnormal(r_indep))
      needs_scaling = true;
  }
  if (!needs_scaling)
    return p_rate / r_indep;

  /* Preserve ordinary rounding above. For extreme counts, keep the binary
   * exponent separate so an underflowed expectation cannot turn lift into 1.
   * lift = (L / G) * product(G / I_i); all mantissa ratios stay bounded. */
  int exponent, group_exponent;
  double mantissa = frexp(p_rate, &exponent);
  double group_mantissa = frexp(group_rate, &group_exponent);
  mantissa /= group_mantissa;
  exponent -= group_exponent;
  for (size_t i = 0; i < subset_len; i++) {
    int item_exponent, adjustment;
    double item_mantissa = frexp(marginals[i], &item_exponent);
    mantissa *= group_mantissa / item_mantissa;
    exponent += group_exponent - item_exponent;
    mantissa = frexp(mantissa, &adjustment);
    exponent += adjustment;
  }
  /* At most 32 items keeps exponent arithmetic safely within int. Only this
   * final scaling may underflow or overflow the actual lift value. */
  return scalbn(mantissa, exponent);
}

/* context for combination evaluation */
typedef struct {
  bishfpm_t *engine;
  bishfpm_group_t *grp;
  const prepared_event_t *event;
  uint64_t timestamp_ms;
  double group_rate;
  double weight;
  double omitted_mass;
  bishfpm_trigger_cb cb;
  void *udata;
  int triggered_count;
  bool require_resident;
  bool quality_omitted_event;
  bishfpm_store_count_t (*update_text)(void *, const char *, size_t, uint64_t,
                                       double, uint64_t);
} bishfpm_eval_ctx_t;

/* canonical patterns occur at most once per event; infinity disables evidence
 * for this group if the bound itself overflows */
static void record_quality_omission(bishfpm_group_t *grp, uint64_t now,
                                    double half_life, double weight) {
  if (weight > 0 && isfinite(grp->quality_omitted.rate) &&
      !(bishfpm_rate_update_weighted(&grp->quality_omitted, now, half_life,
                                     weight) > 0))
    grp->quality_omitted.rate = INFINITY;
}

static bool full_pattern_upper(bishfpm_eval_ctx_t *ctx, topk_result_t update,
                               const char *pattern, double *out) {
  bishfpm_t *engine = ctx->engine;
  bishfpm_group_t *grp = ctx->grp;
  double upper;
  if (!engine->store_config.extension && update.present) {
    upper = update.upper;
  } else {
    store_result_t candidate = grp->store.config->ops->query_text(
        &grp->store, pattern, ctx->timestamp_ms);
    if (!candidate.known)
      return false;
    upper = candidate.count.upper;
  }
  double group = ctx->group_rate;
  if (!isfinite(upper) || upper < 0 || !isfinite(group) || group <= 0 ||
      !isfinite(grp->quality_omitted.rate))
    return false;
  double omitted = ctx->omitted_mass;
  if (omitted < 0) {
    if (!grp->quality_omitted.initialized || grp->quality_omitted.rate <= 0) {
      omitted = 0;
    } else {
      omitted = bishfpm_rate_query(&grp->quality_omitted, ctx->timestamp_ms,
                                   engine->cfg.ewma_half_life_ms);
      if (!isfinite(omitted) || omitted < 0)
        return false;
      if (omitted == 0)
        grp->quality_omitted.rate = 0;
    }
    ctx->omitted_mass = omitted;
  }
  /* certified store bounds cover evaluated history; add omitted-event bound */
  *out = upper >= group || omitted >= group - upper ? group : upper + omitted;
  return true;
}

static double partition_part_lower(const bishfpm_eval_ctx_t *ctx,
                                   const char *const *items,
                                   const size_t *lengths,
                                   const double *marginals, size_t order,
                                   unsigned mask) {
  if (!(mask & (mask - 1))) {
    for (size_t i = 0; i < order; i++)
      if (mask & (1U << i))
        return marginals[i];
    return 0;
  }
  size_t bits = 0;
  for (unsigned m = mask; m; m &= m - 1)
    bits++;
  if (bits < ctx->engine->cfg.min_itemset_order ||
      bits > ctx->engine->cfg.max_itemset_order)
    return 0;
  size_t length = 0;
  char subset[BISHFPM_MAX_PATTERN_LEN];
  for (size_t i = 0; i < order; i++) {
    if (!(mask & (1U << i)))
      continue;
    if (length)
      subset[length++] = BISHFPM_ITEM_DELIM;
    memcpy(subset + length, items[i], lengths[i]);
    length += lengths[i];
  }
  subset[length] = '\0';
  store_result_t r = ctx->grp->store.config->ops->query_text(
      &ctx->grp->store, subset, ctx->timestamp_ms);
  return r.known ? r.count.lower : 0;
}

static bool partition_lift_suppressed(bishfpm_eval_ctx_t *ctx,
                                      const char *const *items,
                                      const size_t *lengths, size_t order,
                                      topk_result_t update, const char *pattern,
                                      const double *marginals, int *upper_state,
                                      double *cached_upper) {
  bishfpm_t *engine = ctx->engine;
  bishfpm_partition_lift_stats_t *stats = &engine->partition_lift_stats;
  stats->candidates_checked++;
  double upper;
  if (!full_pattern_upper(ctx, update, pattern, &upper)) {
    *upper_state = -1;
    stats->candidates_unknown++;
    return false;
  }
  *upper_state = 1;
  *cached_upper = upper;
  double threshold = engine->min_partition_lift;
  double group = ctx->group_rate;
  unsigned full = (1U << order) - 1;
  bool unknown = false;
  /* fix item 0 on the left to visit each unordered two-part split once */
  for (unsigned mask = 1; mask < full; mask += 2) {
    double p0 =
        partition_part_lower(ctx, items, lengths, marginals, order, mask);
    if (!isfinite(p0) || p0 <= 0) {
      unknown = true;
      continue;
    }
    double p1 = partition_part_lower(ctx, items, lengths, marginals, order,
                                     full ^ mask);
    if (!isfinite(p1) || p1 <= 0) {
      unknown = true;
      continue;
    }
    double parts[] = {p0, p1};
    double score = upper > 0 ? compute_subset_lift(2, upper, group, parts) : 0;
    if (conservatively_below(score, threshold)) {
      stats->candidates_suppressed++;
      return true;
    }
  }
  if (unknown)
    stats->candidates_unknown++;
  return false;
}

static bool candidate_promoted(bishfpm_eval_ctx_t *ctx, size_t order,
                               topk_result_t update) {
  if (update.replaced)
    ctx->engine->stats.pattern_replacements++;
  ctx->engine->stats.candidates_evaluated++;
  if ((ctx->require_resident && !update.present) ||
      order < ctx->engine->cfg.min_itemset_order ||
      !(update.lower >= ctx->engine->cfg.min_pattern_rate))
    return false;
  double fraction = ctx->group_rate > 0 ? update.lower / ctx->group_rate : 0;
  return fraction >= ctx->engine->cfg.promotion_fraction;
}

static void notify_candidate(bishfpm_eval_ctx_t *ctx,
                             const char *const *subset_items,
                             const size_t *item_lengths, size_t subset_len,
                             topk_result_t update, const char *pattern,
                             size_t pattern_len, uint64_t key, uint64_t hash,
                             const double *marginals) {
  double p_rate = update.lower;
  double frac = ctx->group_rate > 0.0 ? (p_rate / ctx->group_rate) : 0.0;

  /* check statistical lift threshold if configured */
  double lift = 0.0;
  if (ctx->engine->cfg.min_lift > 0.0) {
    lift = compute_subset_lift(subset_len, p_rate, ctx->group_rate, marginals);
    if (lift < ctx->engine->cfg.min_lift) {
      ctx->engine->stats.triggers_suppressed++;
      return; /* lack of positive statistical correlation */
    }
  }

  int upper_state = 0;
  double cached_upper = 0;
  if (ctx->engine->min_partition_lift > 0 && subset_len >= 3 &&
      subset_len <= 4 &&
      partition_lift_suppressed(ctx, subset_items, item_lengths, subset_len,
                                update, pattern, marginals, &upper_state,
                                &cached_upper)) {
    ctx->engine->stats.triggers_suppressed++;
    return;
  }

  /* check subsumption by active subset alerts */
  bool conservative = conservative_subsumption_enabled(ctx->engine);
  double subsumption_rate = p_rate;
  bool evidence_known = true;
  if (conservative && subset_len > ctx->engine->cfg.min_itemset_order &&
      ctx->grp->cooldown_count) {
    if (upper_state == 0)
      evidence_known =
          full_pattern_upper(ctx, update, pattern, &subsumption_rate);
    else {
      evidence_known = upper_state > 0;
      subsumption_rate = cached_upper;
    }
  }
  if (evidence_known &&
      is_candidate_subsumed(ctx->grp, subset_items, item_lengths, subset_len,
                            subsumption_rate, ctx->timestamp_ms,
                            ctx->engine->cfg.trigger_cooldown_ms,
                            ctx->engine->cfg.subsumption_ratio,
                            ctx->engine->cfg.min_itemset_order,
                            ctx->engine->cfg.hash_seed, conservative)) {
    ctx->engine->stats.triggers_suppressed++;
    return; /* subsumed by active parent pattern */
  }

  /* numeric callers pass the packed key and defer string serialization */
  bool numeric = pattern == NULL;
  bool saturated = false;
  const void *identity = numeric ? (const void *)&key : (const void *)pattern;
  if (check_and_update_cooldown(
          ctx->grp, hash, identity, (uint32_t)subset_len, ctx->timestamp_ms,
          ctx->engine->cfg.trigger_cooldown_ms, &saturated)) {
    ctx->engine->stats.triggers_suppressed++;
    if (saturated)
      ctx->engine->stats.cooldown_saturation++;
    return; /* cooldown active */
  }

  ctx->triggered_count++;
  ctx->engine->stats.triggers_emitted++;

  if (ctx->cb) {
    if (ctx->engine->cfg.min_lift <= 0.0)
      lift =
          compute_subset_lift(subset_len, p_rate, ctx->group_rate, marginals);
    bishfpm_trigger_t trig;
    if (numeric) {
      size_t n0 = item_lengths[0], n1 = item_lengths[1];
      memcpy(trig.pattern, subset_items[0], n0);
      trig.pattern[n0] = BISHFPM_ITEM_DELIM;
      memcpy(trig.pattern + n0 + 1, subset_items[1], n1 + 1);
      if (n0 + 2 + n1 < sizeof(trig.formatted)) {
        memcpy(trig.formatted, subset_items[0], n0);
        trig.formatted[n0] = ',';
        trig.formatted[n0 + 1] = ' ';
        memcpy(trig.formatted + n0 + 2, subset_items[1], n1 + 1);
      } else {
        bishfpm_itemset_format(trig.pattern, trig.formatted,
                               sizeof(trig.formatted));
      }
    } else {
      memcpy(trig.pattern, pattern, pattern_len + 1);
      bishfpm_itemset_format(pattern, trig.formatted, sizeof(trig.formatted));
    }
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

/* inputs and all possible subset lengths were validated by prepare_event;
 * append each prefix once per order, preserving lexicographic enumeration */
static void evaluate_combinations(bishfpm_eval_ctx_t *ctx, size_t count,
                                  size_t target, size_t start, size_t depth,
                                  const char **items, size_t *lengths,
                                  double *marginals, char *pattern,
                                  size_t length) {
  if (depth == target) {
    pattern[length] = 0;
    topk_result_t update;
    if (ctx->update_text) {
      uint64_t hash =
          bishfpm_murmur3_64(pattern, length, ctx->engine->cfg.hash_seed);
      update = store_extension_update_count(
          ctx->engine->store_config.extension,
          ctx->update_text(ctx->grp->store.state.extension, pattern, length,
                           hash, ctx->weight, ctx->timestamp_ms),
          hash);
    } else {
      update = topk_add_sized(ctx->grp->store.state.text, pattern, length,
                              ctx->weight, ctx->timestamp_ms);
      bishfpm_pattern_pressure_t *p = &ctx->grp->pressure;
      p->candidates_evaluated++;
      if (ctx->weight == 0)
        p->zero_weight_queries++;
      else if (!update.accepted)
        p->rejected_updates++;
      else if (update.was_present)
        p->resident_updates++;
      else if (update.present)
        p->admissions++;
      else
        p->filtered_updates++;
      p->replacements += update.replaced;
    }
    if (!update.accepted && ctx->weight > 0 && !ctx->quality_omitted_event &&
        depth >= ctx->engine->quality_min_order &&
        depth <= ctx->engine->quality_max_order) {
      record_quality_omission(ctx->grp, ctx->timestamp_ms,
                              ctx->engine->cfg.ewma_half_life_ms, ctx->weight);
      ctx->quality_omitted_event = true;
      ctx->omitted_mass = -1;
    }
    if (candidate_promoted(ctx, depth, update))
      notify_candidate(ctx, items, lengths, depth, update, pattern, length, 0,
                       update.hash, marginals);
    return;
  }
  size_t needed = target - depth;
  for (size_t i = start; i + needed <= count; i++) {
    size_t pos = length;
    if (depth)
      pattern[pos++] = BISHFPM_ITEM_DELIM;
    const prepared_item_t *item = &ctx->event->items[i];
    size_t item_length = item->length;
    items[depth] = item->key;
    lengths[depth] = item_length;
    marginals[depth] = item->marginal;
    memcpy(pattern + pos, item->key, item_length);
    evaluate_combinations(ctx, count, target, i + 1, depth + 1, items, lengths,
                          marginals, pattern, pos + item_length);
  }
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
    if (length >= BISHFPM_MAX_ITEM_LEN ||
        memchr(items[i], BISHFPM_ITEM_DELIM, length))
      return -1;
    event->items[valid++] = (prepared_item_t){items[i], length, 0, 0, NULL};
  }
  qsort(event->items, valid, sizeof(*event->items), compare_prepared_items);
  event->count = 0;
  size_t total_length = 0;
  for (size_t i = 0; i < valid; i++) {
    if (i && strcmp(event->items[i].key, event->items[i - 1].key) == 0)
      continue;
    size_t n = event->count++;
    event->items[n] = event->items[i];
    event->items[n].hash = bishfpm_murmur3_64(
        event->items[n].key, event->items[n].length, engine->cfg.hash_seed);
    total_length += event->items[n].length;
    event->keys[n] = event->items[n].key;
    event->lengths[n] = event->items[n].length;
  }
  if (!event->count)
    return 0;
  /* Valid items still update marginals when no configured order can fit. */
  if (event->count < engine->cfg.min_itemset_order)
    return 1;
  size_t max_k = event->count < engine->cfg.max_itemset_order
                     ? event->count
                     : engine->cfg.max_itemset_order;
  size_t length = total_length + max_k - 1;
  if (length >= BISHFPM_MAX_PATTERN_LEN && max_k < event->count) {
    qsort(event->lengths, event->count, sizeof(*event->lengths),
          compare_lengths);
    length = max_k - 1;
    for (size_t i = 0; i < max_k; i++)
      length += event->lengths[i];
    for (size_t i = 0; i < event->count; i++)
      event->lengths[i] = event->items[i].length;
  }
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
  uint64_t memory_denials = engine->memory.denials;
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
  if (!grp) {
    budget_rejection = reason == REJECT_GROUPS;
    goto finalize;
  }
  store_reservation_t store_reservation = {0};
  size_t missing_marginals = 0;
  uint32_t next_id = (uint32_t)grp->item_rates->count;
  for (size_t i = 0; i < event->count; i++) {
    bishfpm_rate_entry_t *entry = rate_table_find(
        grp->item_rates, event->items[i].key, event->items[i].hash);
    event->items[i].entry = entry;
    if (!entry)
      missing_marginals++;
    if (engine->numeric_pairs)
      event->ids[i] = entry ? entry->id : next_id++;
  }
  uint64_t now = effective_time(timestamp_ms, grp->max_time_ms);
  double effective_weight = 0;
  int store_ready;
  if (engine->store_config.extension) {
    effective_weight = decay_since((double)weight, timestamp_ms, now,
                                   engine->cfg.ewma_half_life_ms);
    store_extension_t *x = engine->store_config.extension;
    bishfpm_store_event_t view = {event->keys,
                                  event->lengths,
                                  engine->numeric_pairs ? event->ids : NULL,
                                  event->count,
                                  now,
                                  effective_weight,
                                  engine->cfg.min_itemset_order,
                                  engine->cfg.max_itemset_order};
    store_ready = x->descriptor.prepare_event(grp->store.state.extension, &view,
                                              &store_reservation.token);
  } else
    store_ready = !engine->numeric_pairs ||
                  store_prepare_pair(&grp->store, event->ids, event->count, now,
                                     &store_reservation);
  if (store_ready != 1) {
    if (engine->numeric_pairs && !engine->store_config.extension)
      engine->pair_capacity_rejections++;
    reason = store_ready == 0 ? REJECT_CANDIDATES : REJECT_ALLOCATION;
    budget_rejection = store_ready == 0;
    if (engine->group_count != old_group_count) {
      free_group_resources(grp);
      engine->group_count--;
    }
    goto finalize;
  }
  if (missing_marginals) {
    size_t old_cap = grp->item_rates->capacity;
    rate_prepare_result_t reservation =
        rate_table_prepare_result(grp->item_rates, event->keys, event->count);
    if (reservation != RATE_PREPARED) {
      if (engine->store_config.extension)
        store_cancel(&grp->store, &store_reservation);
      reason = reservation == RATE_LIMIT ? REJECT_MARGINALS : REJECT_ALLOCATION;
      if (engine->group_count != old_group_count) {
        free_group_resources(grp);
        engine->group_count--;
      }
      budget_rejection = reservation == RATE_LIMIT;
      goto finalize;
    }
    bool rehashed = grp->item_rates->capacity != old_cap;
    for (size_t i = 0; i < event->count; i++) {
      if (rehashed || !event->items[i].entry)
        event->items[i].entry = rate_table_find(
            grp->item_rates, event->items[i].key, event->items[i].hash);
    }
  }

  /* built-ins do not consume weight during pure capacity preflight; preserve
   * the legacy rejection path's avoidance of unnecessary decay arithmetic */
  if (!engine->store_config.extension)
    effective_weight = decay_since((double)weight, timestamp_ms, now,
                                   engine->cfg.ewma_half_life_ms);
  if (store_reservation.prune || engine->store_config.extension)
    engine->pairs_forgotten +=
        store_commit(&grp->store, now, &store_reservation);
  grp->max_time_ms = now;
  engine->stats.events_accepted++;
  double group_rate = bishfpm_rate_update_weighted(
      &grp->group_rate, now, engine->cfg.ewma_half_life_ms, effective_weight);
  if (engine->quality_max_order != 0 && effective_weight > 0 &&
      !(group_rate > 0))
    grp->quality_omitted.rate = INFINITY;
  size_t elevated = 0;
  for (size_t i = 0; i < event->count; i++) {
    prepared_item_t *item = &event->items[i];
    double ir = bishfpm_rate_update_weighted(&item->entry->rate, now,
                                             engine->cfg.ewma_half_life_ms,
                                             effective_weight);
    item->marginal = ir;
    double share = group_rate > 0 ? ir / group_rate : 0;
    if (ir >= engine->cfg.item_rate_threshold &&
        (engine->cfg.item_share_threshold <= 0 ||
         share >= engine->cfg.item_share_threshold)) {
      if (engine->numeric_pairs)
        event->ids[elevated] = event->ids[i];
      event->items[elevated++] = *item;
    }
  }

  bool quality_omitted_event = effective_weight > 0 &&
                               elevated < event->count &&
                               event->count >= engine->quality_min_order;
  if (quality_omitted_event)
    record_quality_omission(grp, now, engine->cfg.ewma_half_life_ms,
                            effective_weight);
  event->count = elevated;
  result = 0;
  if (elevated >= engine->cfg.min_itemset_order) {
    size_t max_k = elevated < engine->cfg.max_itemset_order
                       ? elevated
                       : engine->cfg.max_itemset_order;
    bishfpm_eval_ctx_t ctx = {
        engine,
        grp,
        event,
        now,
        group_rate,
        effective_weight,
        -1,
        cb,
        udata,
        0,
        engine->numeric_pairs || engine->store_config.sketch_width ||
            engine->store_config.extension,
        quality_omitted_event,
        engine->store_config.extension
            ? ((store_extension_t *)engine->store_config.extension)
                  ->descriptor.update_text
            : NULL};
    /* Filtering preserves canonical order. Each update and its notification
     * finish before the next candidate; callbacks can query this exact state.
     */
    if (engine->numeric_pairs) {
      store_extension_t *x = engine->store_config.extension;
      for (size_t i = 0; i < elevated; i++)
        for (size_t j = i + 1; j < elevated; j++) {
          uint64_t key = compact_key(event->ids[i], event->ids[j]);
          uint64_t hash = compact_hash(key);
          topk_result_t update =
              x ? store_extension_update_count(
                      x,
                      x->descriptor.update_pair(grp->store.state.extension, key,
                                                hash, effective_weight, now),
                      hash)
                : compact_add(grp->store.state.pairs, key, hash,
                              effective_weight, now);
          if (candidate_promoted(&ctx, 2, update)) {
            const char *pair_items[] = {event->items[i].key,
                                        event->items[j].key};
            size_t lengths[] = {event->items[i].length, event->items[j].length};
            double marginals[] = {event->items[i].marginal,
                                  event->items[j].marginal};
            notify_candidate(&ctx, pair_items, lengths, 2, update, NULL, 0, key,
                             hash, marginals);
          }
        }
    } else {
      const char *sub_items[BISHFPM_MAX_ITEMSET_ORDER];
      size_t item_lengths[BISHFPM_MAX_ITEMSET_ORDER];
      double marginals[BISHFPM_MAX_ITEMSET_ORDER];
      char pattern[BISHFPM_MAX_PATTERN_LEN];
      for (size_t k = engine->cfg.min_itemset_order; k <= max_k; k++)
        evaluate_combinations(&ctx, elevated, k, 0, 0, sub_items, item_lengths,
                              marginals, pattern, 0);
    }
    result = ctx.triggered_count;
  }
  if (engine->store_config.extension)
    store_finish(&grp->store, &store_reservation);
finalize:
  if (result < 0) {
    if (engine_budget(engine) && engine->memory.denials != memory_denials) {
      budget_rejection = true;
      engine->memory.event_rejections++;
    }
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
  if (!group)
    return true;
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
static bool query_pattern(const bishfpm_t *engine, const char *name,
                          const char *pattern, uint64_t now,
                          store_result_t *out) {
  if (!engine || !name || !pattern || !pattern[0] ||
      strnlen(pattern, BISHFPM_MAX_PATTERN_LEN) >= BISHFPM_MAX_PATTERN_LEN)
    return false;
  const bishfpm_group_t *g = find_group(engine, name, NULL);
  if (!g) {
    uint64_t key, hash;
    *out = (store_result_t){{0}, true};
    return !engine->numeric_pairs ||
           lookup_pair(engine, NULL, pattern, &key, &hash);
  }
  now = effective_time(now, g->max_time_ms);
  if (engine->numeric_pairs) {
    uint64_t key, hash;
    if (!lookup_pair(engine, g, pattern, &key, &hash))
      return false;
    *out = g->store.config->ops->query_pair(&g->store, key, hash, now);
  } else
    *out = g->store.config->ops->query_text(&g->store, pattern, now);
  return true;
}

int bishfpm_pattern_bounds(bishfpm_t *engine, const char *name,
                           const char *pattern, uint64_t now,
                           bishfpm_count_bounds_t *out) {
  store_result_t r;
  if (!out || !query_pattern(engine, name, pattern, now, &r))
    return 0;
  bool heuristic = isinf(r.count.error);
  *out = (bishfpm_count_bounds_t){heuristic ? 0 : r.count.lower,
                                  heuristic ? INFINITY : r.count.upper,
                                  r.count.present, r.known};
  return 1;
}

void bishfpm_get_pair_stats(const bishfpm_t *engine,
                            bishfpm_pair_stats_t *out) {
  if (!engine || !out)
    return;
  if (!engine->numeric_pairs || engine->store_config.extension) {
    *out = (bishfpm_pair_stats_t){0};
    return;
  }
  *out = (bishfpm_pair_stats_t){engine->pair_capacity_rejections,
                                engine->pairs_forgotten, 0, 0};
  for (size_t i = 0; i < engine->group_cap; i++)
    if (engine->groups[i].group_key) {
      store_t const *s = &engine->groups[i].store;
      store_snapshot_t snap = s->config->ops->snapshot(s);
      out->resident_pairs += snap.residents;
      out->store_bytes += snap.bytes;
    }
}

double bishfpm_pattern_rate(bishfpm_t *engine, const char *key,
                            const char *pattern, uint64_t now_ms) {
  store_result_t r;
  return query_pattern(engine, key, pattern, now_ms, &r) ? r.count.upper : 0;
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
      !engine->numeric_pairs || !engine->store_config.ops->prune_bounded)
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
  size_t forgotten =
      group->store.config->ops->prune_bounded(&group->store, now, max_pairs);
  engine->pairs_forgotten += forgotten;
  engine->busy = false;
  return forgotten;
}

/* Keep all surviving-group maintenance identical in both prune paths. */
static void prune_group(bishfpm_t *engine, bishfpm_group_t *group,
                        uint64_t now_ms, double min_rate, double gr) {
  uint64_t effective = effective_time(now_ms, group->max_time_ms);
  group->max_time_ms = effective;
  group->group_rate.rate = gr;
  group->group_rate.last_time_ms = effective;
  if (group->quality_omitted.initialized &&
      isfinite(group->quality_omitted.rate)) {
    group->quality_omitted.rate = bishfpm_rate_query(
        &group->quality_omitted, effective, engine->cfg.ewma_half_life_ms);
    group->quality_omitted.last_time_ms = effective;
  }
  size_t ip =
      engine->numeric_pairs
          ? prune_numeric_marginals(group->item_rates, effective,
                                    engine->cfg.ewma_half_life_ms, min_rate)
          : bishfpm_rate_table_prune(group->item_rates, effective,
                                     engine->cfg.ewma_half_life_ms, min_rate);
  engine->stats.items_pruned += ip;
  size_t forgotten =
      group->store.config->ops->prune(&group->store, effective, min_rate);
  if (engine->numeric_pairs)
    engine->pairs_forgotten += forgotten;
  if (group->cooldown_count && effective >= group->cooldown_oldest_ms &&
      effective - group->cooldown_oldest_ms >= engine->cfg.trigger_cooldown_ms)
    cooldown_maintain(group, group->cooldown_cap, effective,
                      engine->cfg.trigger_cooldown_ms, true);
}

/* Budget exhaustion must not prevent group retirement. A denied compaction
 * can keep the old capacity while releasing weak groups and repairing probes.
 */
static size_t prune_groups_in_place(bishfpm_t *engine, uint64_t now,
                                    double cutoff) {
  size_t removed = 0, mask = engine->group_cap - 1;
  for (size_t i = 0; i < engine->group_cap;) {
    bishfpm_group_t *group = &engine->groups[i];
    if (!group->group_key) {
      i++;
      continue;
    }
    double rate = bishfpm_rate_query(&group->group_rate,
                                     effective_time(now, group->max_time_ms),
                                     engine->cfg.ewma_half_life_ms);
    if (rate >= cutoff) {
      prune_group(engine, group, now, cutoff, rate);
      i++;
      continue;
    }
    engine->stats.items_pruned += group->item_rates->count;
    engine->stats.groups_pruned++;
    pressure_add(&engine->pressure, &group->pressure);
    free_group_resources(group);
    engine->group_count--;
    removed++;
    size_t hole = i;
    for (size_t probe = (i + 1) & mask; engine->groups[probe].group_key;
         probe = (probe + 1) & mask) {
      size_t home = engine->groups[probe].hash & mask;
      if (((probe - home) & mask) >= ((probe - hole) & mask)) {
        engine->groups[hole] = engine->groups[probe];
        hole = probe;
      }
    }
    memset(&engine->groups[hole], 0, sizeof(engine->groups[hole]));
  }
  return removed;
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
  if (new_cap == engine->group_cap) {
    if (!pruned) {
      for (size_t i = 0; i < engine->group_cap; i++) {
        bishfpm_group_t *group = &engine->groups[i];
        if (!group->group_key)
          continue;
        double gr = bishfpm_rate_query(
            &group->group_rate, effective_time(now_ms, group->max_time_ms),
            engine->cfg.ewma_half_life_ms);
        prune_group(engine, group, now_ms, min_rate, gr);
      }
      engine->busy = false;
      return 0;
    }
    size_t removed = prune_groups_in_place(engine, now_ms, min_rate);
    engine->busy = false;
    return removed;
  }
  bishfpm_group_t *new_groups =
      new_cap && new_cap <= SIZE_MAX / sizeof(bishfpm_group_t)
          ? memory_calloc(engine_budget(engine), new_cap,
                          sizeof(bishfpm_group_t))
          : NULL;
  if (!new_groups) {
    size_t removed = prune_groups_in_place(engine, now_ms, min_rate);
    engine->busy = false;
    return removed;
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
      pressure_add(&engine->pressure, &engine->groups[i].pressure);
      free_group_resources(&engine->groups[i]);
      engine->stats.groups_pruned++;
    } else {
      prune_group(engine, &engine->groups[i], now_ms, min_rate, gr);
      size_t idx = (size_t)(engine->groups[i].hash & mask);
      while (new_groups[idx].group_key) {
        idx = (idx + 1) & mask;
      }
      new_groups[idx] = engine->groups[i];
    }
  }

  memory_free(engine_budget(engine), engine->groups,
              engine->group_cap * sizeof(*engine->groups));
  engine->groups = new_groups;
  engine->group_cap = new_cap;
  engine->group_count = survivors;
  engine->busy = false;

  return pruned;
}

int bishfpm_get_pattern_pressure(const bishfpm_t *engine, const char *name,
                                 bishfpm_pattern_pressure_t *out) {
  if (!engine || !out || engine->numeric_pairs ||
      engine->store_config.extension)
    return 0;
  bishfpm_pattern_pressure_t result = engine->pressure;
  result.resident_patterns = result.resident_capacity = 0;
  if (name) {
    const bishfpm_group_t *group = find_group(engine, name, NULL);
    if (!group)
      return 0;
    result = group->pressure;
    result.resident_patterns =
        group->store.config->ops->snapshot(&group->store).residents;
    result.resident_capacity = engine->cfg.topk_k;
  } else {
    for (size_t i = 0; i < engine->group_cap; i++) {
      if (!engine->groups[i].group_key)
        continue;
      pressure_add(&result, &engine->groups[i].pressure);
      result.resident_patterns +=
          engine->store_config.ops->snapshot(&engine->groups[i].store)
              .residents;
      result.resident_capacity += engine->cfg.topk_k;
    }
  }
  *out = result;
  return 1;
}

int bishfpm_get_vocabulary_stats(const bishfpm_t *engine, const char *name,
                                 bishfpm_vocabulary_stats_t *out) {
  if (!engine || !name || !out)
    return 0;
  const bishfpm_group_t *g = find_group(engine, name, NULL);
  if (!g)
    return 0;
  *out = (bishfpm_vocabulary_stats_t){
      g->item_rates->count, engine->cfg.max_items_per_group,
      engine->cfg.max_items_per_group - g->item_rates->count,
      engine->numeric_pairs};
  return 1;
}

void bishfpm_get_memory_stats(const bishfpm_t *engine,
                              bishfpm_memory_stats_t *out) {
  if (!engine || !out)
    return;
  *out = (bishfpm_memory_stats_t){0};
  out->mode = engine->options.memory_mode;
  out->limit_bytes = engine->cfg.max_memory_bytes;
  if (out->mode == BISHFPM_MEMORY_DYNAMIC) {
    out->current_bytes = engine->memory.current;
    out->peak_bytes = engine->memory.peak;
    out->allocation_denials = engine->memory.denials;
    out->event_rejections = engine->memory.event_rejections;
  } else {
    bishfpm_stats_t stats;
    bishfpm_get_stats(engine, &stats);
    out->current_bytes = stats.memory_current_bytes;
  }
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
  if (engine->store_config.extension) {
    const store_extension_t *x = engine->store_config.extension;
    out->memory_current_bytes += sizeof(*x) + x->bytes;
  }
  for (size_t i = 0; i < engine->group_cap; i++) {
    const bishfpm_group_t *g = &engine->groups[i];
    if (!g->group_key)
      continue;
    out->memory_current_bytes +=
        strlen(g->group_key) + 1 + sizeof(*g->item_rates) +
        g->item_rates->capacity * sizeof(*g->item_rates->entries) +
        g->cooldown_cap *
            (cooldown_entry_size(engine->numeric_pairs) + 2 * sizeof(uint32_t));
    out->memory_current_bytes +=
        g->store.config->ops->snapshot(&g->store).bytes;
    out->memory_current_bytes += g->item_rates->key_bytes;
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

int bishfpm_get_store_description(const bishfpm_t *e,
                                  bishfpm_store_description_t *out) {
  if (!e || !out || out->abi_version != BISHFPM_STORE_ABI_V1 ||
      out->struct_size < sizeof(*out) || !e->store_config.extension)
    return 0;
  const store_extension_t *x = e->store_config.extension;
  uint32_t size = out->struct_size;
  *out = (bishfpm_store_description_t){BISHFPM_STORE_ABI_V1,         size,
                                       x->descriptor.representation, 0,
                                       x->descriptor.capabilities,   x->plan};
  return 1;
}
int bishfpm_get_store_snapshot(const bishfpm_t *e, const char *name,
                               uint32_t size, bishfpm_store_snapshot_t *out) {
  if (!e || !name || !out || size < sizeof(*out) || !e->store_config.extension)
    return 0;
  const bishfpm_group_t *g = find_group(e, name, NULL);
  if (!g)
    return 0;
  const store_extension_t *x = e->store_config.extension;
  bishfpm_store_snapshot_t result = {0};
  x->descriptor.snapshot(g->store.state.extension, &result);
  *out = result;
  return 1;
}
