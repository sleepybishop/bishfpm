#include "bishfpm.h"
#include "bishfpm_lowlevel.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* callback counter */
static int g_cb_count = 0;
static char g_last_pattern[BISHFPM_MAX_PATTERN_LEN];

static void test_cb(const char *grp, const bishfpm_trigger_t *trig,
                    void *udata) {
  (void)grp;
  (void)udata;
  g_cb_count++;
  snprintf(g_last_pattern, sizeof(g_last_pattern), "%s", trig->pattern);
}

typedef struct {
  int calls;
  double lift;
} lift_result_t;

static void lift_cb(const char *group, const bishfpm_trigger_t *trigger,
                    void *udata) {
  (void)group;
  lift_result_t *result = udata;
  result->calls++;
  result->lift = trigger->lift;
}

/* A positive independence expectation can be too small to store as a double,
 * even when all counts and the resulting lift are normal finite doubles. */
static void test_lift_range(void) {
  for (int backend = -1; backend <= BISHFPM_PAIR_FSS; backend++) {
    for (int extreme = 0; extreme < 2; extreme++) {
      for (int reject = 0; reject < 2; reject++) {
        bishfpm_config_t c;
        bishfpm_config_default(&c);
        c.ewma_half_life_ms = 1000;
        c.min_itemset_order = c.max_itemset_order = 2;
        c.item_rate_threshold = c.item_share_threshold = 0;
        c.min_pattern_rate = c.promotion_fraction = 0;
        c.trigger_cooldown_ms = 0;
        c.min_lift = reject ? (extreme ? ldexp(1.0, 668) : nextafter(2, 3)) : 2;
        bishfpm_pair_options_t options = {
            .store = (bishfpm_pair_store_t)backend,
            .capacity = 8,
            .sketch_width = backend == BISHFPM_PAIR_FSS ? 8 : 0,
            .forget_below = 0};
        bishfpm_t *engine = backend < 0 ? bishfpm_create(&c)
                                        : bishfpm_create_pairs(&c, &options);
        assert(engine);
        const char *background[] = {"background"}, *pair[] = {"a", "b"};
        uint64_t now = extreme ? 667000 : 0;
        assert(bishfpm_add_event(engine, "g", now, background, 1, NULL, NULL) ==
               0);
        lift_result_t result = {0};
        assert(bishfpm_add_event(engine, "g", 0, pair, 2, lift_cb, &result) ==
               !reject);
        assert(result.calls == !reject);
        if (!reject) {
          double expected = extreme ? ldexp(1.0, 667) : 2;
          assert(isfinite(result.lift));
          assert(fabs(result.lift / expected - 1) < 1e-12);
        }
        bishfpm_free(engine);
      }
    }
  }

  /* High-order lift can overflow even with ordinary integer event weights.
   * It must still pass a finite lift threshold and report positive infinity. */
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.min_itemset_order = c.max_itemset_order = 32;
  c.item_rate_threshold = c.min_pattern_rate = c.promotion_fraction = 0;
  c.min_lift = 2;
  bishfpm_t *engine = bishfpm_create(&c);
  assert(engine);
  const char *background[] = {"background"};
  for (int i = 0; i < 100; i++)
    assert(bishfpm_add_event_weighted(engine, "g", 0, background, 1, UINT32_MAX,
                                      NULL, NULL) == 0);
  char names[32][8];
  const char *items[32];
  for (int i = 0; i < 32; i++) {
    snprintf(names[i], sizeof(names[i]), "i%02d", i);
    items[i] = names[i];
  }
  lift_result_t result = {0};
  assert(bishfpm_add_event(engine, "g", 0, items, 32, lift_cb, &result) == 1);
  assert(result.calls == 1 && isinf(result.lift) && result.lift > 0);
  bishfpm_free(engine);
}

/* reentrancy callback helper */
static void reentrant_cb(const char *grp, const bishfpm_trigger_t *trig,
                         void *udata) {
  (void)grp;
  (void)trig;
  bishfpm_t *engine = (bishfpm_t *)udata;
  const char *items[] = {"x", "y"};
  int rc = bishfpm_add_event(engine, "other_group", 2000, items, 2, NULL, NULL);
  assert(rc == -1);
}

/* f01: group table prune preserves lookup chains */
static void test_f01_prune_lookup_chains(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.item_rate_threshold = 0;
  c.min_pattern_rate = 0;
  c.promotion_fraction = 0;
  c.max_itemset_order = 2;

  const char *a[] = {"a"};
  bishfpm_t *e = bishfpm_create(&c);
  assert(e != NULL);

  bishfpm_add_event(e, "g6", 1000, a, 1, NULL, NULL);
  bishfpm_add_event_weighted(e, "g8", 1000, a, 1, 10, NULL, NULL);

  /* prune g6 (rate 1 < 5); g8 (rate 10 >= 5) must survive */
  size_t pruned = bishfpm_prune(e, 1000, 5.0);
  assert(pruned == 1);

  double r = bishfpm_group_rate(e, "g8", 1000);
  assert(fabs(r - 10.0) < 1e-4);

  bishfpm_free(e);
}

/* f02: duplicate input items deduplication */
static void test_f02_duplicate_items(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.item_rate_threshold = 0;
  c.min_pattern_rate = 0;
  c.promotion_fraction = 0;
  c.min_itemset_order = 2;
  c.max_itemset_order = 2;

  const char *aa[] = {"a", "a"};
  bishfpm_t *e = bishfpm_create(&c);
  assert(e != NULL);

  g_cb_count = 0;
  int alerts = bishfpm_add_event(e, "grp", 1000, aa, 2, test_cb, NULL);
  assert(alerts == 0);
  assert(g_cb_count == 0);

  double ir = bishfpm_item_rate(e, "grp", "a", 1000);
  assert(fabs(ir - 1.0) < 1e-4);

  bishfpm_free(e);
}

/* f03: reject invalid item separator */
static void test_f03_delimiter_rejection(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);

  const char *bad[] = {"a\x1f"
                       "b",
                       "c"};
  bishfpm_t *e = bishfpm_create(&c);
  assert(e != NULL);

  int rc = bishfpm_add_event(e, "grp", 1000, bad, 2, NULL, NULL);
  assert(rc == -1);

  /* verify group was not created */
  double gr = bishfpm_group_rate(e, "grp", 1000);
  assert(gr == 0.0);

  bishfpm_free(e);
}

/* f04: cooldown monotonicity on equal and older timestamps */
static void test_f04_cooldown_monotonicity(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.item_rate_threshold = 0;
  c.min_pattern_rate = 0;
  c.promotion_fraction = 0;
  c.min_itemset_order = 2;
  c.max_itemset_order = 2;
  c.trigger_cooldown_ms = 5000;

  const char *ab[] = {"a", "b"};
  bishfpm_t *e = bishfpm_create(&c);
  assert(e != NULL);

  g_cb_count = 0;
  int rc1 = bishfpm_add_event(e, "grp", 1000, ab, 2, test_cb, NULL);
  assert(rc1 == 1);
  assert(g_cb_count == 1);

  /* equal timestamp: must be suppressed by cooldown */
  int rc2 = bishfpm_add_event(e, "grp", 1000, ab, 2, test_cb, NULL);
  assert(rc2 == 0);
  assert(g_cb_count == 1);

  /* older timestamp: must be suppressed and not rewind clock */
  int rc3 = bishfpm_add_event(e, "grp", 500, ab, 2, test_cb, NULL);
  assert(rc3 == 0);
  assert(g_cb_count == 1);

  /* within cooldown: must be suppressed */
  int rc4 = bishfpm_add_event(e, "grp", 5999, ab, 2, test_cb, NULL);
  assert(rc4 == 0);
  assert(g_cb_count == 1);

  /* after cooldown: triggers */
  int rc5 = bishfpm_add_event(e, "grp", 6001, ab, 2, test_cb, NULL);
  assert(rc5 == 1);
  assert(g_cb_count == 2);

  bishfpm_free(e);
}

/* f05: cooldown table saturation does not allow bypass */
static void test_f05_cooldown_saturation(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.item_rate_threshold = 0;
  c.min_pattern_rate = 0;
  c.promotion_fraction = 0;
  c.min_itemset_order = 1;
  c.max_itemset_order = 1;
  c.topk_k = 2;
  c.trigger_cooldown_ms = 10000;

  bishfpm_t *e = bishfpm_create(&c);
  assert(e != NULL);

  /* generate 20 distinct pattern triggers */
  char buf[32];
  for (int i = 0; i < 20; i++) {
    snprintf(buf, sizeof(buf), "item_%02d", i);
    const char *items[] = {buf};
    int r = bishfpm_add_event(e, "grp", 1000 + i, items, 1, NULL, NULL);
    assert(r == 1);
  }

  /* immediately re-sending the last item must be in cooldown */
  const char *last_items[] = {"item_19"};
  int r_suppressed =
      bishfpm_add_event(e, "grp", 1025, last_items, 1, NULL, NULL);
  assert(r_suppressed == 0);

  bishfpm_free(e);
}

/* f06 and f07: weighted addition and ubsan safety */
static void test_f06_f07_collision_and_large_weight(void) {
  bishfpm_topk_t *t = bishfpm_topk_new(2, 1000);
  assert(t != NULL);

  bishfpm_topk_add(t, "a", 404, 1);
  double add_ret = bishfpm_topk_add(t, "b", 1000, 1);
  assert(add_ret >= 1000.0);
  assert(bishfpm_topk_query(t, "a", 1) >= 404.0);

  bishfpm_topk_free(t);

  /* test takeover count conservation: weight 3 replaces incumbent 1 */
  bishfpm_topk_t *t2 = bishfpm_topk_new(1, 100000);
  assert(t2 != NULL);
  bishfpm_topk_add(t2, "incumbent", 1, 1);

  double res = bishfpm_topk_add(t2, "pattern_new", 3, 100);
  assert(res >= 3.0);

  bishfpm_topk_free(t2);
}

/* f08: time zero counters and late arrival handling */
static void test_f08_time_zero_and_late_arrival(void) {
  bishfpm_rate_t *r = bishfpm_rate_new();
  assert(r);

  bishfpm_rate_update(r, 0, 1000.0);
  bishfpm_rate_update(r, 0, 1000.0);
  double q0 = bishfpm_rate_query(r, 0, 1000.0);
  assert(fabs(q0 - 2.0) < 1e-4);

  double q1000 = bishfpm_rate_query(r, 1000, 1000.0);
  assert(fabs(q1000 - 1.0) < 1e-4);

  /* late arrival */
  bishfpm_rate_t *r2 = bishfpm_rate_new();
  assert(r2);
  bishfpm_rate_update(r2, 1000, 1000.0);
  bishfpm_rate_update(r2, 0, 1000.0);
  double q_late = bishfpm_rate_query(r2, 1000, 1000.0);
  assert(fabs(q_late - 1.5) < 1e-4);
  bishfpm_rate_free(r);
  bishfpm_rate_free(r2);
}

/* f09: callback reentrancy safety */
static void test_f09_reentrancy_guard(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.item_rate_threshold = 0;
  c.min_pattern_rate = 0;
  c.promotion_fraction = 0;
  c.min_itemset_order = 2;
  c.max_itemset_order = 2;

  bishfpm_t *e = bishfpm_create(&c);
  assert(e != NULL);

  const char *items[] = {"a", "b"};
  int r = bishfpm_add_event(e, "grp", 1000, items, 2, reentrant_cb, e);
  assert(r == 1);

  bishfpm_free(e);
}

/* f10: candidates restricted by min_itemset_order */
static void test_f10_candidate_order_bounds(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.item_rate_threshold = 0;
  c.min_pattern_rate = 0;
  c.promotion_fraction = 0;
  c.min_itemset_order = 2;
  c.max_itemset_order = 3;

  bishfpm_t *e = bishfpm_create(&c);
  assert(e != NULL);

  /* singleton event: should generate zero candidate evaluations */
  const char *single[] = {"a"};
  int r = bishfpm_add_event(e, "grp", 1000, single, 1, NULL, NULL);
  assert(r == 0);

  /* pattern rate of singleton in sketch should remain 0 */
  double pr = bishfpm_pattern_rate(e, "grp", "a", 1000);
  assert(pr == 0.0);

  bishfpm_free(e);
}

/* Events below the minimum order still contribute group and item history. */
static void test_no_candidate_length_limit(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.min_itemset_order = c.max_itemset_order = 4;
  c.item_rate_threshold = c.min_pattern_rate = c.promotion_fraction = 0;
  bishfpm_t *e = bishfpm_create(&c);
  assert(e);

  char names[4][201];
  const char *items[4];
  for (size_t i = 0; i < 4; i++) {
    memset(names[i], 'a' + (int)i, sizeof(names[i]) - 1);
    names[i][sizeof(names[i]) - 1] = '\0';
    items[i] = names[i];
  }
  /* Each item fits, but the untracked three-item encoding would not. */
  assert(bishfpm_add_event(e, "g", 1000, items, 3, NULL, NULL) == 0);
  const char *duplicates[] = {items[2], items[0], items[1], items[1]};
  assert(bishfpm_add_event(e, "g", 1000, duplicates, 4, NULL, NULL) == 0);
  assert(bishfpm_group_rate(e, "g", 0) == 2);
  for (size_t i = 0; i < 3; i++)
    assert(bishfpm_item_rate(e, "g", items[i], 0) == 2);

  /* A configured candidate that exceeds the encoding limit still rejects. */
  assert(bishfpm_add_event(e, "g", 10000, items, 4, NULL, NULL) == -1);
  const char *delimiter[] = {"bad\037item"};
  assert(bishfpm_add_event(e, "g", 10000, delimiter, 1, NULL, NULL) == -1);
  char oversized[BISHFPM_MAX_ITEM_LEN + 1];
  memset(oversized, 'x', sizeof(oversized) - 1);
  oversized[sizeof(oversized) - 1] = '\0';
  const char *invalid[] = {oversized};
  assert(bishfpm_add_event(e, "g", 10000, invalid, 1, NULL, NULL) == -1);
  assert(bishfpm_group_rate(e, "g", 0) == 2);
  for (size_t i = 0; i < 3; i++)
    assert(bishfpm_item_rate(e, "g", items[i], 0) == 2);
  assert(bishfpm_item_rate(e, "g", items[3], 0) == 0);

  bishfpm_stats_t stats;
  bishfpm_rejection_stats_t reasons;
  bishfpm_get_stats(e, &stats);
  bishfpm_get_rejection_stats(e, &reasons);
  assert(stats.events_accepted == 2 && stats.events_rejected == 3);
  assert(stats.events_noop == 0 && stats.candidates_evaluated == 0 &&
         stats.triggers_emitted == 0);
  assert(reasons.pattern_length_limit == 1 && reasons.invalid_input == 2);
  bishfpm_free(e);
}

/* f11: partial top-k query accuracy */
static void test_f11_partial_topk(void) {
  bishfpm_topk_t *t = bishfpm_topk_new(3, 1000);
  assert(t != NULL);

  bishfpm_topk_add(t, "a", 1, 1);
  bishfpm_topk_add(t, "b", 5, 1);
  bishfpm_topk_add(t, "c", 10, 1);

  bishfpm_topk_item_t out;
  size_t ret = bishfpm_topk_get_top(t, 1, &out, 1);
  assert(ret == 1);
  assert(strcmp(out.pattern, "c") == 0);
  assert(fabs(out.rate - 10.0) < 1e-4);

  bishfpm_topk_free(t);
}

/* f12: subsumption attribute renaming invariance */
static void test_f12_subsumption_invariance(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.item_rate_threshold = 0;
  c.min_pattern_rate = 0;
  c.promotion_fraction = 0;
  c.min_itemset_order = 2;
  c.max_itemset_order = 3;
  c.subsumption_ratio = 0.85;
  c.trigger_cooldown_ms = 10000;

  /* case 1: BC seeded, then ABC sent */
  bishfpm_t *e1 = bishfpm_create(&c);
  const char *bc[] = {"b", "c"};
  const char *abc[] = {"a", "b", "c"};
  bishfpm_add_event_weighted(e1, "grp", 1000, bc, 2, 100, NULL, NULL);

  g_cb_count = 0;
  g_last_pattern[0] = '\0';
  bishfpm_add_event(e1, "grp", 1000, abc, 3, test_cb, NULL);
  int count_abc = g_cb_count;
  bishfpm_free(e1);

  /* case 2: BC seeded, then BCZ sent (identical except attribute name a -> z)
   */
  bishfpm_t *e2 = bishfpm_create(&c);
  const char *bcz[] = {"b", "c", "z"};
  bishfpm_add_event_weighted(e2, "grp", 1000, bc, 2, 100, NULL, NULL);

  g_cb_count = 0;
  g_last_pattern[0] = '\0';
  bishfpm_add_event(e2, "grp", 1000, bcz, 3, test_cb, NULL);
  int count_bcz = g_cb_count;
  bishfpm_free(e2);

  /* both cases must behave identically under subsumption */
  assert(count_abc == count_bcz);
}

/* f14: configuration parameter validation */
static void test_f14_config_validation(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);

  /* nan half-life */
  c.ewma_half_life_ms = NAN;
  assert(bishfpm_create(&c) == NULL);

  /* zero half-life */
  c.ewma_half_life_ms = 0.0;
  assert(bishfpm_create(&c) == NULL);

  /* negative half-life */
  c.ewma_half_life_ms = -5.0;
  assert(bishfpm_create(&c) == NULL);

  /* restore half-life, test invalid order */
  bishfpm_config_default(&c);
  c.min_itemset_order = 5;
  c.max_itemset_order = 3;
  assert(bishfpm_create(&c) == NULL);

  /* zero topk_k */
  bishfpm_config_default(&c);
  c.topk_k = 0;
  assert(bishfpm_create(&c) == NULL);
}

/* f15: counter precision beyond 2^24 */
static void test_f15_counter_precision(void) {
  bishfpm_topk_t *t = bishfpm_topk_new(2, 1000000);
  assert(t != NULL);

  /* add 16777216, then add 1000 */
  bishfpm_topk_add(t, "p", 16777216, 1000);
  bishfpm_topk_add(t, "p", 1000, 1000);

  double q = bishfpm_topk_query(t, "p", 1000);
  assert(fabs(q - 16778216.0) < 1e-4);

  bishfpm_topk_free(t);
}

/* max_groups budget and engine statistics */
static void test_max_groups_and_stats(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.max_groups = 2;
  c.item_rate_threshold = 0.0;
  c.promotion_fraction = 0.0;
  c.min_pattern_rate = 0.0;

  bishfpm_t *e = bishfpm_create(&c);
  assert(e != NULL);

  const char *items[] = {"x", "y"};
  int rc1 = bishfpm_add_event(e, "grp1", 1000, items, 2, NULL, NULL);
  assert(rc1 >= 0);

  int rc2 = bishfpm_add_event(e, "grp2", 1000, items, 2, NULL, NULL);
  assert(rc2 >= 0);

  /* third group should be rejected by max_groups budget */
  int rc3 = bishfpm_add_event(e, "grp3", 1000, items, 2, NULL, NULL);
  assert(rc3 == -1);

  /* existing groups should still be accepted */
  int rc4 = bishfpm_add_event(e, "grp1", 1000, items, 2, NULL, NULL);
  assert(rc4 >= 0);

  bishfpm_stats_t st;
  bishfpm_get_stats(e, &st);
  assert(st.events_total == 4);
  assert(st.events_rejected == 1);
  assert(st.active_groups == 2);
  assert(st.candidates_evaluated >= 2);

  bishfpm_free(e);
}

/* max_items_per_group, loss margin validation, keyed hashing, pruning stats */
static void test_max_items_and_loss_guard_and_seeds(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);

  /* 1. loss margin validation guard */
  c.min_pattern_rate = 5.0;
  c.item_rate_threshold = 10.0;
  assert(bishfpm_create(&c) == NULL);

  /* 2. max_items_per_group budget and keyed hashing */
  bishfpm_config_default(&c);
  c.max_items_per_group = 2;
  c.hash_seed = 0xa5a5a5a512345678ULL;
  c.item_rate_threshold = 0.0;
  c.min_pattern_rate = 0.0;
  c.promotion_fraction = 0.0;

  bishfpm_t *e = bishfpm_create(&c);
  assert(e != NULL);

  const char *items1[] = {"item_a", "item_b"};
  int rc = bishfpm_add_event(e, "grp", 1000, items1, 2, NULL, NULL);
  assert(rc >= 0);

  /* third item should be rejected by max_items_per_group */
  const char *items2[] = {"item_c"};
  rc = bishfpm_add_event(e, "grp", 1000, items2, 1, NULL, NULL);
  assert(rc == -1);
  assert(bishfpm_group_rate(e, "grp", 1000) == 1);

  double ra = bishfpm_item_rate(e, "grp", "item_a", 1000);
  double rb = bishfpm_item_rate(e, "grp", "item_b", 1000);
  double rc_rate = bishfpm_item_rate(e, "grp", "item_c", 1000);
  assert(ra > 0.0);
  assert(rb > 0.0);
  assert(rc_rate == 0.0);

  /* 3. pruning stats */
  size_t pruned = bishfpm_prune(e, 50000, 10.0);
  assert(pruned == 1);

  bishfpm_stats_t st;
  bishfpm_get_stats(e, &st);
  assert(st.groups_pruned == 1);
  assert(st.items_pruned == 2);
  assert(st.active_groups == 0);

  bishfpm_free(e);
}

/* f03 and f13: atomicity and key length boundaries */
static void test_f03_f13_atomicity_and_limits(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.item_rate_threshold = 0;
  c.min_pattern_rate = 0;
  c.promotion_fraction = 0;
  c.min_itemset_order = 1;
  c.max_itemset_order = 3;

  bishfpm_t *e = bishfpm_create(&c);
  assert(e != NULL);

  /* oversized key at low-level topk api (> 512 bytes) */
  bishfpm_topk_t *topk = bishfpm_topk_new(10, 1000.0);
  assert(topk != NULL);
  char huge_key[700];
  memset(huge_key, 'x', sizeof(huge_key) - 1);
  huge_key[sizeof(huge_key) - 1] = '\0';
  assert(bishfpm_topk_add(topk, huge_key, 10, 1000) == 0.0);
  assert(bishfpm_topk_query(topk, huge_key, 1000) == 0.0);
  bishfpm_topk_free(topk);

  /* oversized item (> 256 bytes) */
  char long_item[300];
  memset(long_item, 'a', sizeof(long_item) - 1);
  long_item[sizeof(long_item) - 1] = '\0';
  const char *items_long[] = {long_item};
  int rc_long =
      bishfpm_add_event(e, "grp_long", 1000, items_long, 1, NULL, NULL);
  assert(rc_long == -1);
  assert(bishfpm_group_rate(e, "grp_long", 1000) == 0.0);

  /* oversized combination: three 200-byte items sum to > 512 bytes */
  char item1[201];
  char item2[201];
  char item3[201];
  memset(item1, 'y', sizeof(item1) - 1);
  item1[sizeof(item1) - 1] = '\0';
  memset(item2, 'z', sizeof(item2) - 1);
  item2[sizeof(item2) - 1] = '\0';
  memset(item3, 'w', sizeof(item3) - 1);
  item3[sizeof(item3) - 1] = '\0';
  const char *triple[] = {item1, item2, item3};

  int rc_triple = bishfpm_add_event(e, "grp_comb", 1000, triple, 3, NULL, NULL);
  assert(rc_triple == -1);
  /* verify no mutation occurred */
  assert(bishfpm_group_rate(e, "grp_comb", 1000) == 0.0);
  assert(bishfpm_item_rate(e, "grp_comb", item1, 1000) == 0.0);

  /* oversized event exceeding scratch arena (4097 items) */
  const char **many = malloc(4097 * sizeof(const char *));
  assert(many != NULL);
  for (int i = 0; i < 4097; i++) {
    many[i] = "item";
  }
  int rc_many = bishfpm_add_event(e, "grp_huge", 1000, many, 4097, NULL, NULL);
  assert(rc_many == -1);
  assert(bishfpm_group_rate(e, "grp_huge", 1000) == 0.0);
  free(many);

  bishfpm_stats_t st;
  bishfpm_get_stats(e, &st);
  assert(st.events_rejected >= 3);

  bishfpm_free(e);
}

/* f08: counter consistency on late arrivals */
static void test_f08_late_arrival_consistency(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.ewma_half_life_ms = 1000.0;
  c.item_rate_threshold = 0.0;
  c.promotion_fraction = 0.0;
  c.min_pattern_rate = 0.0;
  c.min_itemset_order = 1;
  c.max_itemset_order = 1;

  bishfpm_t *e = bishfpm_create(&c);
  assert(e != NULL);

  const char *items[] = {"alpha"};
  /* add weight 10 at time 1001 */
  bishfpm_add_event_weighted(e, "grp_late", 1001, items, 1, 10, NULL, NULL);

  /* add weight 1 at time 1 (late arrival) */
  bishfpm_add_event_weighted(e, "grp_late", 1, items, 1, 1, NULL, NULL);

  double gr = bishfpm_group_rate(e, "grp_late", 1001);
  double ir = bishfpm_item_rate(e, "grp_late", "alpha", 1001);
  double pr = bishfpm_pattern_rate(e, "grp_late", "alpha", 1001);

  /* all counters must consistently evaluate to 10.5 */
  assert(fabs(gr - 10.5) < 1e-4);
  assert(fabs(ir - 10.5) < 1e-4);
  assert(fabs(pr - 10.5) < 1e-4);

  bishfpm_free(e);
}

/* f04: late arrival does not defeat active cooldowns */
static void test_f04_late_arrival_cooldown_retention(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.item_rate_threshold = 0;
  c.min_pattern_rate = 0;
  c.promotion_fraction = 0;
  c.min_itemset_order = 1;
  c.max_itemset_order = 1;
  c.topk_k = 1;
  c.trigger_cooldown_ms = 10000;

  bishfpm_t *e = bishfpm_create(&c);
  assert(e != NULL);

  /* create 12 triggers at time 1000 */
  char buf[32];
  for (int i = 0; i < 12; i++) {
    snprintf(buf, sizeof(buf), "pattern_%02d", i);
    const char *it[] = {buf};
    int rc = bishfpm_add_event(e, "grp_cd", 1000, it, 1, NULL, NULL);
    assert(rc == 1);
  }

  /* thirteenth pattern arrives late at time 500 */
  const char *late_it[] = {"pattern_late"};
  int rc_late = bishfpm_add_event(e, "grp_cd", 500, late_it, 1, NULL, NULL);
  (void)rc_late;

  /* repeat all 12 patterns at time 1001: must still be in cooldown */
  int re_alerts = 0;
  for (int i = 0; i < 12; i++) {
    snprintf(buf, sizeof(buf), "pattern_%02d", i);
    const char *it[] = {buf};
    re_alerts += bishfpm_add_event(e, "grp_cd", 1001, it, 1, NULL, NULL);
  }
  assert(re_alerts == 0);

  bishfpm_free(e);
}

/* f14: constructor boundary safety */
static void test_f14_constructor_safety(void) {
  /* rate table constructor with SIZE_MAX must not loop indefinitely */
  bishfpm_rate_table_t *tbl = bishfpm_rate_table_new(SIZE_MAX);
  assert(tbl == NULL);

  /* topk constructor with huge k must not wrap or crash */
  bishfpm_topk_t *topk = bishfpm_topk_new(1U << 31, 1000.0);
  assert(topk == NULL);
}

static void test_numeric_query_validation(void) {
  const char *malformed[] = {"a", "b\037a", "a\037b\037c", "\037b", "a\037"};
  const char *ab[] = {"a", "b"};
  for (int kind = BISHFPM_PAIR_SS; kind <= BISHFPM_PAIR_FSS; kind++) {
    bishfpm_config_t c;
    bishfpm_config_default(&c);
    c.min_itemset_order = c.max_itemset_order = 2;
    c.item_rate_threshold = c.item_share_threshold = 0;
    bishfpm_pair_options_t options = {kind, 4,
                                      kind == BISHFPM_PAIR_FSS ? 1U : 0U, 0};
    bishfpm_t *e = bishfpm_create_pairs(&c, &options);
    assert(e);
    for (unsigned phase = 0; phase < 3; phase++) {
      bishfpm_count_bounds_t bounds;
      for (size_t i = 0; i < sizeof(malformed) / sizeof(*malformed); i++)
        assert(!bishfpm_pattern_bounds(e, "g", malformed[i], 0, &bounds));
      assert(bishfpm_pattern_bounds(e, "g", "a\037b", 0, &bounds));
      if (phase == 0)
        assert(bishfpm_add_event(e, "g", 0, ab, 2, NULL, NULL) >= 0);
      else if (phase == 1)
        assert(bishfpm_prune(e, 0, 2) == 1);
    }
    bishfpm_free(e);
  }
}

typedef struct {
  bishfpm_t *engine;
  unsigned calls;
} lift_oracle_t;

static void check_callback_lift(const char *group, const bishfpm_trigger_t *t,
                                void *udata) {
  lift_oracle_t *oracle = udata;
  oracle->calls++;
  double expected = bishfpm_group_rate(oracle->engine, group, t->timestamp);
  assert(expected == t->group_rate);
  char pattern[BISHFPM_MAX_PATTERN_LEN];
  strcpy(pattern, t->pattern);
  char *item = pattern;
  for (;;) {
    char *split = strchr(item, BISHFPM_ITEM_DELIM);
    if (split)
      *split = 0;
    double marginal =
        bishfpm_item_rate(oracle->engine, group, item, t->timestamp);
    assert(marginal > 0);
    expected *= marginal / t->group_rate;
    if (!split)
      break;
    item = split + 1;
  }
  expected = t->pattern_rate / expected;
  assert(fabs(t->lift - expected) < 1e-12 * fmax(1, expected));
}

static void test_filtered_lift_and_callback_stats(void) {
  unsigned delivered = 0, suppressed = 0;
  for (int backend = -1; backend <= BISHFPM_PAIR_FSS; backend++) {
    for (unsigned filter = 0; filter < 3; filter++) {
      for (unsigned seeded = 0; seeded < 2; seeded++) {
        bishfpm_config_t c;
        bishfpm_config_default(&c);
        c.min_itemset_order = 2;
        c.max_itemset_order = backend < 0 ? 3 : 2;
        c.item_rate_threshold = backend == BISHFPM_PAIR_THRESHOLD ? 0 : 3;
        c.item_share_threshold = 0;
        c.min_pattern_rate = 3;
        c.promotion_fraction = 0;
        c.ewma_half_life_ms = 1000000;
        c.trigger_cooldown_ms = 100;
        c.subsumption_ratio = .85;
        c.topk_k = 64;
        c.min_lift = filter == 0 ? 0 : filter == 1 ? 1.1 : 1e100;
        c.hash_seed = seeded ? UINT64_C(0xfedcba9876543210) : 0;
        bishfpm_pair_options_t options = {
            (bishfpm_pair_store_t)backend, 64,
            backend == BISHFPM_PAIR_FSS ? 17U : 0U, 0};
        bishfpm_t *with_cb = backend < 0 ? bishfpm_create(&c)
                                         : bishfpm_create_pairs(&c, &options);
        bishfpm_t *without_cb = backend < 0
                                    ? bishfpm_create(&c)
                                    : bishfpm_create_pairs(&c, &options);
        assert(with_cb && without_cb);
        lift_oracle_t oracle = {with_cb, 0};
        const char *selected[] = {"a", "b", "c"};
        const char *background[] = {"background"};
        const char *a[] = {"a"}, *b[] = {"b"};
        for (unsigned control = 0; control < 2; control++) {
          bishfpm_t *e = control ? without_cb : with_cb;
          assert(bishfpm_add_event_weighted(e, "g", 0, selected, 3, 10, NULL,
                                            NULL) >= 0);
          assert(bishfpm_add_event_weighted(e, "g", 0, background, 1, 100, NULL,
                                            NULL) == 0);
          assert(bishfpm_add_event_weighted(e, "g", 0, a, 1, 20, NULL, NULL) ==
                 0);
          assert(bishfpm_add_event_weighted(e, "g", 0, b, 1, 30, NULL, NULL) ==
                 0);
        }
        /* Cold items precede, separate, and follow the elevated items. */
        const char *all[] = {"0", "a", "aa", "b", "bb", "c", "cc"};
        const uint64_t times[] = {1000, 1000, 900, 1100, 1200};
        for (size_t i = 0; i < sizeof(times) / sizeof(*times); i++) {
          int notified = bishfpm_add_event(with_cb, "g", times[i], all, 7,
                                           check_callback_lift, &oracle);
          int silent =
              bishfpm_add_event(without_cb, "g", times[i], all, 7, NULL, NULL);
          assert(notified >= 0 && notified == silent);
          bishfpm_stats_t x, y;
          bishfpm_get_stats(with_cb, &x);
          bishfpm_get_stats(without_cb, &y);
          assert(x.triggers_emitted == y.triggers_emitted);
          assert(x.triggers_suppressed == y.triggers_suppressed);
          assert(x.cooldown_saturation == y.cooldown_saturation);
          assert(x.candidates_evaluated == y.candidates_evaluated);
          assert(x.callbacks_delivered == oracle.calls &&
                 y.callbacks_delivered == 0);
          delivered += notified;
          suppressed += x.triggers_suppressed > 0;
        }
        if (filter == 2)
          assert(oracle.calls == 0);
        else
          assert(oracle.calls > 0);
        bishfpm_free(with_cb);
        bishfpm_free(without_cb);
      }
    }
  }
  assert(delivered && suppressed);
}

static void test_preparation_length_paths(void) {
  const size_t lengths[][4] = {{10, 11, 12, 13}, {80, 81, 200, 201}};
  for (size_t row = 0; row < 2; row++) {
    char names[4][BISHFPM_MAX_ITEM_LEN];
    const char *items[4];
    for (size_t i = 0; i < 4; i++) {
      memset(names[i], 'a' + (int)i, lengths[row][i]);
      names[i][lengths[row][i]] = 0;
      items[i] = names[i];
    }
    for (unsigned order = 2; order <= 4; order++) {
      bishfpm_config_t c;
      bishfpm_config_default(&c);
      c.item_rate_threshold = c.min_pattern_rate = c.promotion_fraction = 0;
      c.min_itemset_order = 2;
      c.max_itemset_order = order;
      c.trigger_cooldown_ms = 0;
      bishfpm_t *e = bishfpm_create(&c);
      assert(e);
      int expected = order == 2 ? 6 : order == 3 ? 10 : row ? -1 : 11;
      assert(bishfpm_add_event(e, "g", 1000, items, 4, NULL, NULL) == expected);
      assert(bishfpm_group_rate(e, "g", 1000) == (expected >= 0));
      if (expected >= 0) {
        for (size_t i = 0; i < 4; i++)
          assert(bishfpm_item_rate(e, "g", items[i], 1000) == 1);
      }
      bishfpm_free(e);
    }
  }
}

int main(void) {
  test_preparation_length_paths();
  test_filtered_lift_and_callback_stats();
  test_numeric_query_validation();
  test_lift_range();
  test_f01_prune_lookup_chains();
  test_f02_duplicate_items();
  test_f03_delimiter_rejection();
  test_f04_cooldown_monotonicity();
  test_f05_cooldown_saturation();
  test_f06_f07_collision_and_large_weight();
  test_f08_time_zero_and_late_arrival();
  test_f09_reentrancy_guard();
  test_f10_candidate_order_bounds();
  test_no_candidate_length_limit();
  test_f11_partial_topk();
  test_f12_subsumption_invariance();
  test_f14_config_validation();
  test_f15_counter_precision();
  test_max_groups_and_stats();
  test_max_items_and_loss_guard_and_seeds();
  test_f03_f13_atomicity_and_limits();
  test_f08_late_arrival_consistency();
  test_f04_late_arrival_cooldown_retention();
  test_f14_constructor_safety();

  /* verify library version query */
  assert(strcmp(bishfpm_version(), BISHFPM_VERSION_STRING) == 0);

  printf("test_regressions passed.\n");
  return 0;
}
