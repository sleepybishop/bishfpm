/* Exercise cooldown allocation and expiry without depending on candidate churn.
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static size_t rebuild_allocations;
static int fail_rebuild;
static int numeric_mode;
static void *tracked_calloc(size_t n, size_t size, const char *function) {
  if (!strcmp(function, "cooldown_maintain")) {
    rebuild_allocations++;
    if (fail_rebuild)
      return NULL;
  }
  return calloc(n, size);
}
#define calloc(n, size) tracked_calloc((n), (size), __func__)
#include "../../lib/bishfpm.c"
#undef calloc

static bishfpm_group_t group(size_t capacity) {
  bishfpm_group_t g = {0};
  static compact_t numeric_store;
  g.pairs = numeric_mode ? &numeric_store : NULL;
  g.cooldown_cap = capacity;
  g.cooldowns = calloc(capacity, cooldown_entry_size(numeric_mode != 0) +
                                     2 * sizeof(uint32_t));
  assert(g.cooldowns);
  return g;
}
static int update_hashed(bishfpm_group_t *g, unsigned id, uint64_t hash,
                         uint64_t now, bool *saturated) {
  char pattern[32];
  snprintf(pattern, sizeof(pattern), "pattern%u", id);
  *saturated = false;
  uint64_t key = ((uint64_t)id << 32) | (id + 1);
  const void *identity =
      numeric_mode ? (const void *)&key : (const void *)pattern;
  return check_and_update_cooldown(g, hash, identity, numeric_mode ? 2 : 1, now,
                                   100, saturated);
}
static int update(bishfpm_group_t *g, unsigned id, uint64_t now,
                  bool *saturated) {
  return update_hashed(g, id, id, now, saturated);
}
static void release(bishfpm_group_t *g) {
  free(g->cooldowns);
  free(g->cooldown_spare);
}
static void reuse_and_saturation(void) {
  bishfpm_group_t g = group(COOLDOWN_GROWTH_LIMIT);
  bool saturated;
  rebuild_allocations = 0;
  for (unsigned i = 0; i < COOLDOWN_GROWTH_LIMIT; i++)
    assert(!update(&g, i, 1000, &saturated) && !saturated);
  assert(rebuild_allocations == 0);
  for (unsigned i = 0; i < 100; i++)
    assert(update(&g, 5000 + i, 1099, &saturated) && saturated);
  assert(update(&g, 0, 999, &saturated) && !saturated);
  assert(rebuild_allocations == 0);
  assert(!update(&g, 5000, 1100, &saturated) && !saturated);
  assert(g.cooldown_count == 1 && rebuild_allocations == 1);
  for (unsigned i = 0; i < COOLDOWN_GROWTH_LIMIT - 1; i++)
    assert(!update(&g, i, 1100, &saturated) && !saturated);
  assert(!update(&g, 6000, 1200, &saturated) && !saturated);
  assert(g.cooldown_count == 1 && rebuild_allocations == 1);
  assert(update(&g, 6000, 1100, &saturated) && !saturated);
  release(&g);
}
static void stale_expiry_hint(void) {
  bishfpm_group_t g = group(COOLDOWN_GROWTH_LIMIT);
  bool saturated;
  rebuild_allocations = 0;
  for (unsigned i = 0; i < 3000; i++)
    assert(!update(&g, i, 0, &saturated));
  for (unsigned i = 0; i < 3000; i++)
    assert(!update(&g, i, 100, &saturated));
  assert(!update(&g, 4000, 100, &saturated));
  assert(!update(&g, 4001, 100, &saturated));
  assert(g.cooldown_count == 3002 && rebuild_allocations == 0);
  assert(g.cooldown_oldest_ms == 100);
  release(&g);
}
static void allocation_failure(void) {
  bishfpm_group_t g = group(COOLDOWN_GROWTH_LIMIT);
  bool saturated;
  for (unsigned i = 0; i < COOLDOWN_GROWTH_LIMIT; i++)
    assert(!update(&g, i, 1000, &saturated));
  void *active = g.cooldowns;
  fail_rebuild = 1;
  assert(!update(&g, 5000, 1100, &saturated) && !saturated);
  assert(g.cooldowns == active && g.cooldown_count == COOLDOWN_GROWTH_LIMIT);
  assert(update(&g, 5000, 1100, &saturated) && !saturated);
  fail_rebuild = 0;
  assert(!update(&g, 5001, 1100, &saturated) && !saturated);
  assert(g.cooldown_count == 2);
  release(&g);

  g = group(16);
  for (unsigned i = 0; i < 12; i++)
    assert(!update(&g, i, UINT64_MAX - 1, &saturated));
  fail_rebuild = 1;
  assert(!update(&g, 12, UINT64_MAX, &saturated) && !saturated);
  assert(g.cooldown_cap == 16 && g.cooldown_count == 13);
  assert(update(&g, 0, 0, &saturated) && !saturated);
  fail_rebuild = 0;
  assert(!update(&g, 13, UINT64_MAX, &saturated) && !saturated);
  assert(g.cooldown_cap == 32 && g.cooldown_count == 14);
  release(&g);
}
static void fallback_hint_and_probe_chains(void) {
  bishfpm_group_t g = group(COOLDOWN_GROWTH_LIMIT);
  bool saturated;
  uint64_t hash = 2 * g.cooldown_cap - 2;
  for (unsigned i = 0; i < COOLDOWN_GROWTH_LIMIT; i++)
    assert(!update_hashed(&g, i, hash, i ? 1050 : 1000, &saturated));
  fail_rebuild = 1;
  assert(!update_hashed(&g, 5000, hash, 1100, &saturated));
  assert(g.cooldown_oldest_ms == 1050);
  size_t allocations = rebuild_allocations;
  assert(update_hashed(&g, 5001, hash, 1100, &saturated) && saturated);
  assert(rebuild_allocations == allocations);
  for (unsigned i = 1; i < COOLDOWN_GROWTH_LIMIT; i++)
    assert(update_hashed(&g, i, hash, 1100, &saturated) && !saturated);
  assert(update_hashed(&g, 5000, hash, 1100, &saturated) && !saturated);
  fail_rebuild = 0;
  release(&g);
}

static void prune_cooldowns(void) {
  bishfpm_config_t cfg;
  bishfpm_config_default(&cfg);
  cfg.item_rate_threshold = cfg.promotion_fraction = cfg.min_pattern_rate = 0;
  cfg.trigger_cooldown_ms = 100;
  cfg.ewma_half_life_ms = 1000;
  bishfpm_t *e = bishfpm_create(&cfg);
  assert(e);
  const char *ab[] = {"a", "b"};
  assert(bishfpm_add_event(e, "g", 1000, ab, 2, NULL, NULL) == 1);
  bishfpm_group_t *g = find_group(e, "g", NULL);
  assert(g && g->cooldown_count == 1);
  assert(bishfpm_prune(e, 1050, .01) == 0);
  assert(g->cooldown_count == 1);
  assert(bishfpm_add_event(e, "g", 1050, ab, 2, NULL, NULL) == 0);
  assert(bishfpm_prune(e, 1100, .01) == 0);
  assert(g->cooldown_count == 0);
  assert(bishfpm_add_event(e, "g", 1100, ab, 2, NULL, NULL) == 1);
  bishfpm_free(e);
}

static void identity_collisions(void) {
  bishfpm_group_t g = group(16);
  bool saturated;
  assert(!update_hashed(&g, 1, 0, 1000, &saturated) && !saturated);
  assert(!update_hashed(&g, 2, 0, 1000, &saturated) && !saturated);
  assert(update_hashed(&g, 1, 0, 1050, &saturated) && !saturated);
  assert(update_hashed(&g, 2, 0, 1050, &saturated) && !saturated);
  assert(g.cooldown_count == 2);
  release(&g);
}
/* Numeric cooldown reservations fit many small groups, and reused buffers
 * remain included in the public payload accounting. */
static void memory_accounting(void) {
  bishfpm_config_t cfg;
  bishfpm_config_default(&cfg);
  cfg.min_itemset_order = cfg.max_itemset_order = 2;
  cfg.item_rate_threshold = cfg.item_share_threshold = 0;
  cfg.min_pattern_rate = 1;
  cfg.promotion_fraction = 0;
  cfg.trigger_cooldown_ms = 100;
  cfg.max_groups = 256;
  cfg.max_items_per_group = cfg.max_event_items = 16;
  cfg.topk_k = 4;
  cfg.max_memory_bytes = 128U << 20;
  assert(!bishfpm_create(&cfg));
  const char *items[] = {"a", "b", "c", "d", "e", "f",
                         "g", "h", "i", "j", "k", "l"};
  for (int kind = BISHFPM_PAIR_SS; kind <= BISHFPM_PAIR_FSS; kind++) {
    bishfpm_pair_options_t pairs = {kind, 128,
                                    kind == BISHFPM_PAIR_FSS ? 17 : 0, 0};
    bishfpm_t *e = bishfpm_create_pairs(&cfg, &pairs);
    assert(e);
    assert(bishfpm_add_event(e, "g", 1000, items, 12, NULL, NULL) == 66);
    bishfpm_group_t *g = find_group(e, "g", NULL);
    size_t slots_before = g->cooldown_cap + g->cooldown_spare_cap;
    bishfpm_stats_t before, after;
    bishfpm_get_stats(e, &before);
    assert(bishfpm_add_event(e, "g", 1100, items, 12, NULL, NULL) == 66);
    bishfpm_get_stats(e, &after);
    size_t slots_after = g->cooldown_cap + g->cooldown_spare_cap;
    assert(after.memory_current_bytes - before.memory_current_bytes ==
           (slots_after - slots_before) *
               (sizeof(pair_cooldown_t) + 2 * sizeof(uint32_t)));
    assert(after.memory_current_bytes <= after.memory_bound_bytes &&
           after.memory_bound_bytes <= cfg.max_memory_bytes);
    bishfpm_free(e);
  }
}
static void subsumption_probe_equivalence(void) {
  const char *patterns[] = {"a\037b", "a\037c", "b\037c", "d\037e", "a\037bb"};
  const char *candidates[][3] = {
      {"a", "b", "c"}, {"a", "bb", "c"}, {"a", "d", "e"}, {"x", "y", "z"}};
  for (unsigned seeded = 0; seeded < 2; seeded++) {
    uint64_t seed = seeded ? UINT64_C(0xfedcba9876543210) : 0;
    bishfpm_group_t g = group(16);
    g.topk = bishfpm_topk_new(16, 1000);
    assert(g.topk);
    bishfpm_topk_set_seed(g.topk, seed);
    bool saturated = false;
    for (size_t i = 0; i < sizeof(patterns) / sizeof(*patterns); i++) {
      assert(bishfpm_topk_add(g.topk, patterns[i], 10 + i, 1000) > 0);
      assert(!check_and_update_cooldown(&g, bishfpm_hash_str(patterns[i], seed),
                                        patterns[i], 2, 1000 + i * 10, 100,
                                        &saturated));
    }
    for (unsigned pruned = 0; pruned < 2; pruned++) {
      if (pruned)
        assert(bishfpm_topk_prune(g.topk, 1000, 12) == 2);
      for (uint64_t now = 990; now <= 1140; now += 10) {
        for (size_t candidate = 0; candidate < 4; candidate++) {
          for (unsigned rate = 0; rate < 20; rate++) {
            bool scan = is_candidate_subsumed(&g, candidates[candidate], 3,
                                              rate, now, 100, .85, 1, seed);
            bool probes = is_candidate_subsumed(&g, candidates[candidate], 3,
                                                rate, now, 100, .85, 2, seed);
            assert(scan == probes);
          }
        }
      }
    }
    bishfpm_topk_free(g.topk);
    release(&g);
  }
}

static void full_stale_expiry_hint(void) {
  bishfpm_group_t g = group(COOLDOWN_GROWTH_LIMIT);
  bool saturated;
  for (unsigned i = 0; i < COOLDOWN_GROWTH_LIMIT; i++)
    assert(!update(&g, i, 1000, &saturated));
  for (unsigned i = 0; i < COOLDOWN_GROWTH_LIMIT; i++)
    assert(!update(&g, i, 1100, &saturated));
  assert(g.cooldown_oldest_ms == 1000);
  size_t allocations = rebuild_allocations;
  assert(update(&g, 5000, 1100, &saturated) && saturated);
  assert(g.cooldown_oldest_ms == 1100);
  assert(g.cooldown_count == COOLDOWN_GROWTH_LIMIT);
  assert(rebuild_allocations == allocations);
  assert(update(&g, 0, 1100, &saturated) && !saturated);
  release(&g);
}

int main(void) {
  subsumption_probe_equivalence();
  memory_accounting();
  prune_cooldowns();
  for (numeric_mode = 0; numeric_mode <= 1; numeric_mode++) {
    reuse_and_saturation();
    stale_expiry_hint();
    full_stale_expiry_hint();
    allocation_failure();
    identity_collisions();
    fallback_hint_and_probe_chains();
  }
  puts("test_cooldown passed.");
}
