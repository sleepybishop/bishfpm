#include "lib/compact.h"
#include <float.h>
#include <stdio.h>

static void equal(topk_result_t a, topk_result_t b) {
  assert(a.present == b.present && a.was_present == b.was_present);
  assert(a.replaced == b.replaced && a.accepted == b.accepted);
  assert(a.time_ms == b.time_ms);
  assert(a.lower == b.lower && a.upper == b.upper && a.error == b.error);
}
/* allocate a positive-floor heap, then set its floor to 0 as a reference */
static compact_t *reference(uint32_t k) {
  compact_t *t = compact_new(k, 1, 0, BISHFPM_PAIR_THRESHOLD, DBL_MIN);
  assert(t && !t->exact);
  t->forget_below = 0;
  return t;
}
static void differential(void) {
  compact_t *a = compact_new(37, 1, 0, BISHFPM_PAIR_THRESHOLD, 0);
  compact_t *b = reference(37);
  assert(a && a->exact && sizeof(*a->counts) == 24);
  assert(compact_storage_bytes(b) - compact_storage_bytes(a) == 37 * 16);
  uint64_t rng = 42, watermark = 0;
  for (unsigned step = 0; step < 12000; step++) {
    uint64_t bits = bishfpm_splitmix64(&rng);
    uint64_t now = watermark + bits % 5;
    if (step % 211 == 0)
      now += 2200; /* underflow and subsequent late updates */
    if (bits & 4)
      now = now > 17 ? now - 17 : 0;
    uint32_t ids[5];
    size_t n = 2 + bits % 4;
    uint32_t base = (uint32_t)(bits % 12);
    for (size_t i = 0; i < n; i++)
      ids[i] = (base + i) % 12;
    bool pa, pb;
    bool ra = compact_reserve(a, ids, n, now, &pa);
    bool rb = compact_reserve(b, ids, n, now, &pb);
    assert(ra == rb && pa == pb);
    assert(a->heap_size == b->heap_size && a->max_time_ms == b->max_time_ms);
    if (ra) {
      if (pa)
        assert(compact_prune(a, now, 0) == compact_prune(b, now, 0));
      double weight = step % 13 ? (double)(1 + bits % 37) / 8 : 0;
      for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++) {
          uint64_t key = compact_key(ids[i], ids[j]), hash = compact_hash(key);
          equal(compact_add(a, key, hash, weight, now),
                compact_add(b, key, hash, weight, now));
        }
      watermark = effective_time(watermark, now);
    }
    if (step % 97 == 0)
      assert(compact_prune(a, now, 1) == compact_prune(b, now, 1));
    assert(a->forgotten == b->forgotten);
    for (uint32_t i = 0; i < 12; i++)
      for (uint32_t j = i + 1; j < 12; j++) {
        uint64_t key = compact_key(i, j), hash = compact_hash(key);
        equal(compact_query(a, key, hash, now),
              compact_query(b, key, hash, now));
      }
  }
  compact_free(a);
  compact_free(b);
}
static void boundaries(void) {
  compact_t *a = compact_new(4, 1, 0, BISHFPM_PAIR_THRESHOLD, 0);
  compact_t *b = reference(4);
  assert(a);
  uint64_t keys[] = {0, UINT64_MAX, 1, 2, 3};
  double weights[] = {DBL_MAX, DBL_TRUE_MIN, 1, 2};
  for (size_t i = 0; i < 4; i++) {
    uint64_t h = compact_hash(keys[i]);
    equal(compact_add(a, keys[i], h, weights[i], 0),
          compact_add(b, keys[i], h, weights[i], 0));
  }
  equal(compact_add(a, 0, compact_hash(0), DBL_MAX, 0),
        compact_add(b, 0, compact_hash(0), DBL_MAX, 0));
  assert(a->max_time_ms == 0 && b->max_time_ms == 0);
  equal(compact_add(a, 3, compact_hash(3), 1, UINT64_MAX),
        compact_add(b, 3, compact_hash(3), 1, UINT64_MAX));
  assert(a->max_time_ms == 0); /* failed insertion does not move time */
  equal(compact_add(a, 0, compact_hash(0), NAN, 1),
        compact_add(b, 0, compact_hash(0), NAN, 1));
  equal(compact_add(a, 0, compact_hash(0), 0, UINT64_MAX),
        compact_add(b, 0, compact_hash(0), 0, UINT64_MAX));
  /* DBL_TRUE_MIN decays to zero at half a half-life */
  assert(compact_prune_bounded(a, 1, 1) == 1);
  assert(!compact_query(a, UINT64_MAX, compact_hash(UINT64_MAX), 1).present);
  assert(a->heap_size == 3 && a->forgotten == 1);
  assert(compact_prune_bounded(a, 1, 1) == 0);
  assert(compact_add(a, 3, compact_hash(3), 1, 1).present);
  assert(compact_prune_bounded(a, UINT64_MAX, 2) == 2);
  assert(compact_prune(a, UINT64_MAX, 0) == 2);
  assert(!a->heap_size && a->forgotten == 5);
  /* an accepted late contribution can underflow to a resident zero */
  assert(compact_add(a, 0, compact_hash(0), 1, 0).present);
  assert(compact_query(a, 0, compact_hash(0), 0).upper == 0);
  assert(compact_prune(a, 0, 0) == 1);
  compact_free(a);
  compact_free(b);
}
static void collisions(void) {
  compact_t *t = compact_new(2048, 1, 0, BISHFPM_PAIR_THRESHOLD, 0);
  assert(t);
  uint64_t keys[2048], next = 0;
  for (size_t i = 0; i < 2048; i++) {
    /* shared low bits exercise wrapped probe chains and moved dense rows */
    while ((compact_hash(next) & 255) != 255)
      next++;
    keys[i] = next++;
    assert(
        compact_add(t, keys[i], compact_hash(keys[i]), i % 2 ? DBL_MAX : 1, 0)
            .present);
  }
  assert(compact_prune(t, 1100, 0) == 1024);
  for (size_t i = 0; i < 2048; i++) {
    topk_result_t r = compact_query(t, keys[i], compact_hash(keys[i]), 1100);
    assert(r.present == (i % 2 != 0));
  }
  for (size_t i = 0; i < 2048; i += 2)
    assert(compact_add(t, keys[i], compact_hash(keys[i]), i % 4 ? 1 : DBL_MAX,
                       1100)
               .present);
  /* > 1024 removals trigger unindexed compaction and slot_map_rebuild */
  assert(compact_prune(t, 2200, 0) == 1536);
  for (size_t i = 0; i < 2048; i++) {
    topk_result_t r = compact_query(t, keys[i], compact_hash(keys[i]), 2200);
    assert(r.present == (i % 4 == 0));
  }
  assert(compact_prune(t, 3300, 0) == 512);
  compact_free(t);
}
static void budgets(void) {
  compact_t *t = compact_new(97, 100, 0, BISHFPM_PAIR_THRESHOLD, 0);
  assert(t);
  size_t bytes = compact_storage_bytes(t);
  assert(bytes == compact_configured_bytes(97, 0, BISHFPM_PAIR_THRESHOLD, 0));
  assert(bytes + 97 * 16 == compact_bytes(97, 0, BISHFPM_PAIR_THRESHOLD));
  compact_free(t);
  bishfpm_memory_budget_t budget = {.limit = bytes - 1};
  assert(!compact_new_budget(97, 100, 0, BISHFPM_PAIR_THRESHOLD, 0, &budget));
  assert(budget.current == 0 && budget.denials == 1);
  budget = (bishfpm_memory_budget_t){.limit = bytes};
  t = compact_new_budget(97, 100, 0, BISHFPM_PAIR_THRESHOLD, 0, &budget);
  assert(t && budget.current == bytes && budget.peak == bytes);
  compact_free(t);
  assert(budget.current == 0);
}
static void count_alert(const char *group, const bishfpm_trigger_t *trigger,
                        void *context) {
  assert(!strcmp(group, "g") && trigger->pattern_rate > 0);
  (*(unsigned *)context)++;
}
static void engine(void) {
  for (unsigned dynamic = 0; dynamic < 2; dynamic++) {
    bishfpm_config_t c;
    bishfpm_config_default(&c);
    c.min_itemset_order = c.max_itemset_order = 2;
    c.item_rate_threshold = c.item_share_threshold = 0;
    c.min_pattern_rate = 1;
    c.promotion_fraction = c.min_lift = 0;
    c.ewma_half_life_ms = 1;
    c.max_groups = 1;
    c.max_items_per_group = c.max_event_items = 4;
    c.topk_k = 4;
    c.trigger_cooldown_ms = 100;
    bishfpm_pair_options_t pairs = {BISHFPM_PAIR_THRESHOLD, 3, 0, 0};
    bishfpm_options_t options = {BISHFPM_STORE_SS, 0,
                                 dynamic ? BISHFPM_MEMORY_DYNAMIC
                                         : BISHFPM_MEMORY_CONSERVATIVE};
    bishfpm_t *e = bishfpm_create_pairs_with_options(&c, &pairs, &options);
    assert(e);
    /* check exact layout in admission bounds and dynamic allocation ledger */
    bishfpm_stats_t exact_stats, heap_stats;
    bishfpm_get_stats(e, &exact_stats);
    pairs.forget_below = DBL_MIN;
    bishfpm_t *heap_engine =
        bishfpm_create_pairs_with_options(&c, &pairs, &options);
    assert(heap_engine);
    bishfpm_get_stats(heap_engine, &heap_stats);
    if (dynamic)
      assert(heap_stats.memory_bound_bytes == exact_stats.memory_bound_bytes);
    else
      assert(heap_stats.memory_bound_bytes - exact_stats.memory_bound_bytes ==
             3 * 16);
    bishfpm_free(heap_engine);
    const char *abc[] = {"a", "b", "c"}, *abd[] = {"a", "b", "d"};
    unsigned alerts = 0;
    assert(bishfpm_add_event_weighted(e, "g", 0, abc, 3, 2, count_alert,
                                      &alerts) >= 0);
    assert(alerts == 3);
    assert(bishfpm_add_event_weighted(e, "g", 500, abd, 3, 1, count_alert,
                                      &alerts) == -1);
    assert(alerts == 3 && bishfpm_group_rate(e, "g", 0) == 2);
    assert(bishfpm_item_rate(e, "g", "d", 0) == 0);
    bishfpm_pair_stats_t p;
    bishfpm_get_pair_stats(e, &p);
    assert(p.capacity_rejections == 1 && p.resident_pairs == 3);
    assert(p.store_bytes ==
           compact_configured_bytes(3, 0, BISHFPM_PAIR_THRESHOLD, 0));
    assert(bishfpm_prune_pairs(e, "g", 2000, 1) == 1);
    assert(bishfpm_add_event_weighted(e, "g", 2000, abc, 3, 2, count_alert,
                                      &alerts) >= 0);
    assert(alerts == 6);
    /* saturation reclaims all underflowed residents before the new event */
    assert(bishfpm_add_event_weighted(e, "g", 4000, abd, 3, 1, count_alert,
                                      &alerts) >= 0);
    bishfpm_get_pair_stats(e, &p);
    assert(p.patterns_forgotten == 4 && p.resident_pairs == 3);
    char pattern[BISHFPM_MAX_PATTERN_LEN];
    const char *bc[] = {"b", "c"}, *ad[] = {"a", "d"};
    bishfpm_count_bounds_t bounds;
    assert(bishfpm_itemset_serialize(bc, 2, pattern, sizeof(pattern)));
    assert(bishfpm_pattern_bounds(e, "g", pattern, 4000, &bounds));
    assert(bounds.known && !bounds.resident && bounds.lower == 0 &&
           bounds.upper == 0);
    assert(bishfpm_itemset_serialize(ad, 2, pattern, sizeof(pattern)));
    assert(bishfpm_pattern_bounds(e, "g", pattern, 4000, &bounds));
    assert(bounds.known && bounds.resident && bounds.lower == 1 &&
           bounds.upper == 1);
    bishfpm_stats_t stats;
    bishfpm_get_stats(e, &stats);
    assert(stats.memory_current_bytes <= stats.memory_bound_bytes);
    bishfpm_free(e);
  }
}
int main(void) {
  differential();
  boundaries();
  collisions();
  budgets();
  engine();
  puts("test_compact_exact passed.");
}
