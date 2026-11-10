#include "bishfpm.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_ALERTS 512
static int alert_count = 0;
static char alert_patterns[MAX_ALERTS][BISHFPM_MAX_PATTERN_LEN];
static char alert_keys[MAX_ALERTS][BISHFPM_MAX_PATTERN_LEN];
static double alert_lifts[MAX_ALERTS];

static void on_test_trigger(const char *group_key,
                            const bishfpm_trigger_t *trigger, void *udata) {
  (void)group_key;
  (void)udata;
  if (alert_count < MAX_ALERTS) {
    snprintf(alert_patterns[alert_count], sizeof(alert_patterns[alert_count]),
             "%s", trigger->formatted);
    snprintf(alert_keys[alert_count], sizeof(alert_keys[alert_count]),
             "%s", trigger->pattern);
    alert_lifts[alert_count] = trigger->lift;
  }
  alert_count++;
}

static int alerts_for(const char *pattern) {
  int matches = 0;
  assert(alert_count <= MAX_ALERTS);
  for (int i = 0; i < alert_count; i++)
    matches += strcmp(alert_keys[i], pattern) == 0;
  return matches;
}

static bishfpm_config_t partition_config(void) {
  bishfpm_config_t cfg;
  bishfpm_config_default(&cfg);
  cfg.item_rate_threshold = 0.0;
  cfg.item_share_threshold = 0.0;
  cfg.promotion_fraction = 0.20;
  cfg.min_pattern_rate = 288.0;
  cfg.min_lift = 1.2;
  cfg.subsumption_ratio = 0.85;
  cfg.min_itemset_order = 2;
  cfg.max_itemset_order = 3;
  cfg.trigger_cooldown_ms = 10000;
  cfg.topk_k = 32;
  cfg.max_groups = 1;
  cfg.max_items_per_group = 32;
  return cfg;
}

static bishfpm_t *partition_engine(const bishfpm_config_t *cfg,
                                    double threshold, int fss) {
  bishfpm_options_t options;
  bishfpm_options_default(&options);
  if (fss) {
    options.store = BISHFPM_STORE_FSS;
    options.sketch_width = 128;
  }
  bishfpm_t *engine = bishfpm_create_with_options(cfg, &options);
  assert(engine != NULL);
  if (threshold >= 0.0)
    assert(bishfpm_set_min_partition_lift(engine, threshold) == 1);
  return engine;
}

static void feed(bishfpm_t *engine, const char *const *items, size_t n,
                 uint32_t weight) {
  assert(bishfpm_add_event_weighted(engine, "g", 1000, items, n, weight,
                                    NULL, NULL) >= 0);
}

static void finish_fixture(bishfpm_t *engine, const char *const *items,
                            size_t n, bishfpm_alert_quality_stats_t *delta) {
  bishfpm_alert_quality_stats_t before, after;
  bishfpm_get_alert_quality_stats(engine, &before);
  alert_count = 0;
  assert(bishfpm_add_event_weighted(engine, "g", 1000, items, n, 1,
                                    on_test_trigger, NULL) >= 0);
  bishfpm_get_alert_quality_stats(engine, &after);
  if (delta) {
    delta->candidates_checked = after.candidates_checked - before.candidates_checked;
    delta->candidates_suppressed =
        after.candidates_suppressed - before.candidates_suppressed;
    delta->candidates_unknown = after.candidates_unknown - before.candidates_unknown;
  }
}

static bishfpm_t *triple_fixture(double threshold, int productive,
                                 int only_triples, int fss,
                                 bishfpm_alert_quality_stats_t *delta) {
  bishfpm_config_t cfg = partition_config();
  if (only_triples)
    cfg.min_itemset_order = 3;
  bishfpm_t *engine = partition_engine(&cfg, threshold, fss);
  const char *ab[] = {"a", "b"};
  const char *ac[] = {"a", "c"};
  const char *bc[] = {"b", "c"};
  const char *a[] = {"a"};
  const char *b[] = {"b"};
  const char *c[] = {"c"};
  const char *bg[] = {"background"};
  const char *abc[] = {"a", "b", "c"};
  feed(engine, ab, 2, 32);
  feed(engine, ac, 2, productive ? 32 : 72);
  feed(engine, bc, 2, productive ? 32 : 72);
  feed(engine, a, 1, productive ? 48 : 8);
  feed(engine, b, 1, productive ? 48 : 8);
  feed(engine, c, 1, productive ? 48 : 468);
  feed(engine, bg, 1, productive ? 472 : 52);
  feed(engine, abc, 3, 287);
  finish_fixture(engine, abc, 3, delta);
  return engine;
}

static void assert_exact_pattern(bishfpm_t *engine, const char *pattern,
                                 double expected) {
  bishfpm_count_bounds_t bounds;
  assert(bishfpm_pattern_bounds(engine, "g", pattern, 1000, &bounds));
  assert(bounds.known && bounds.resident);
  assert(fabs(bounds.lower - expected) < 1e-10);
  assert(fabs(bounds.upper - expected) < 1e-10);
}

static void test_partition_lift_independent_item(void) {
  /* A=B=400, C=900, AB=320, ABC=288, G=1000. Singleton lift is
   * 2 and ABC/AB is .9, so existing lift/subsumption allow the triple.
   * C is independent of AB: the AB|C partition has lift exactly 1. */
  for (int fss = 0; fss <= 1; fss++) {
    bishfpm_alert_quality_stats_t delta;
    bishfpm_t *baseline = triple_fixture(-1.0, 0, 0, fss, &delta);
    assert(alert_count == 1 && alerts_for("a\037b\037c") == 1);
    assert(fabs(alert_lifts[0] - 2.0) < 1e-10);
    assert(delta.candidates_checked == 0);
    assert(delta.candidates_suppressed == 0 && delta.candidates_unknown == 0);

    bishfpm_t *filtered = triple_fixture(1.1, 0, 0, fss, &delta);
    assert(alert_count == 0);
    assert(delta.candidates_checked == 1 && delta.candidates_suppressed == 1);
    assert(delta.candidates_unknown == 0);

    /* Notification selection must leave every support count unchanged. */
    const char *patterns[] = {"a\037b", "a\037c", "b\037c", "a\037b\037c"};
    const double counts[] = {320, 360, 360, 288};
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
      assert_exact_pattern(baseline, patterns[i], counts[i]);
      assert_exact_pattern(filtered, patterns[i], counts[i]);
    }
    assert(bishfpm_group_rate(baseline, "g", 1000) == 1000);
    assert(bishfpm_group_rate(filtered, "g", 1000) == 1000);
    const char *items[] = {"a", "b", "c"};
    const double marginals[] = {400, 400, 900};
    for (size_t i = 0; i < 3; i++) {
      assert(bishfpm_item_rate(baseline, "g", items[i], 1000) == marginals[i]);
      assert(bishfpm_item_rate(filtered, "g", items[i], 1000) == marginals[i]);
    }
    bishfpm_free(baseline);
    bishfpm_free(filtered);

    bishfpm_t *engine = triple_fixture(0.0, 0, 0, fss, &delta);
    assert(alerts_for("a\037b\037c") == 1 && delta.candidates_checked == 0);
    bishfpm_free(engine);
    engine = triple_fixture(1.0, 0, 0, fss, &delta);
    assert(alerts_for("a\037b\037c") == 1); /* equality is retained */
    assert(delta.candidates_checked == 1 && delta.candidates_suppressed == 0);
    bishfpm_free(engine);
  }
}

static void test_partition_lift_productive_and_unknown(void) {
  for (int fss = 0; fss <= 1; fss++) {
    bishfpm_alert_quality_stats_t delta;
    /* A=B=C=400, each pair=320, ABC=288, G=1000: all three
     * partitions have lift 2.25, including at the threshold boundary. */
    bishfpm_t *engine = triple_fixture(2.25, 1, 0, fss, &delta);
    assert(alert_count == 1 && alerts_for("a\037b\037c") == 1);
    assert(fabs(alert_lifts[0] - 4.5) < 1e-10);
    assert(delta.candidates_checked == 1 && delta.candidates_suppressed == 0);
    assert(delta.candidates_unknown == 0);
    bishfpm_free(engine);

    /* An order-three-only engine has no pair history to certify a split.
     * Even the redundant fixture must pass through with unknown evidence. */
    engine = triple_fixture(1.1, 0, 1, fss, &delta);
    assert(alerts_for("a\037b\037c") == 1);
    assert(delta.candidates_checked == 1 && delta.candidates_suppressed == 0);
    assert(delta.candidates_unknown == 1);
    bishfpm_free(engine);
  }
}

static void test_partition_lift_api(void) {
  bishfpm_config_t cfg = partition_config();
  bishfpm_t *engine = bishfpm_create(&cfg);
  assert(engine);
  assert(!bishfpm_set_min_partition_lift(NULL, 1.0));
  assert(!bishfpm_set_min_partition_lift(engine, -1.0));
  assert(!bishfpm_set_min_partition_lift(engine, NAN));
  assert(!bishfpm_set_min_partition_lift(engine, INFINITY));
  assert(bishfpm_set_min_partition_lift(engine, 1.1));
  assert(bishfpm_set_min_partition_lift(engine, 0.0));

  const char *bg[] = {"background"};
  feed(engine, bg, 1, 0); /* no-op does not freeze the option */
  assert(bishfpm_set_min_partition_lift(engine, 1.1));
  feed(engine, bg, 1, 1);
  assert(!bishfpm_set_min_partition_lift(engine, 0.0));
  assert(!bishfpm_set_min_partition_lift(engine, 2.0));

  bishfpm_alert_quality_stats_t stats = {7, 8, 9};
  bishfpm_get_alert_quality_stats(NULL, &stats);
  assert(stats.candidates_checked == 7 && stats.candidates_suppressed == 8 &&
         stats.candidates_unknown == 9);
  bishfpm_get_alert_quality_stats(engine, NULL);
  bishfpm_get_alert_quality_stats(engine, &stats);
  assert(stats.candidates_checked == 0 && stats.candidates_suppressed == 0 &&
         stats.candidates_unknown == 0);
  bishfpm_free(engine);
}

static void test_partition_lift_quads(void) {
  const char *ab[] = {"a", "b"};
  const char *cd[] = {"c", "d"};
  const char *abcd[] = {"a", "b", "c", "d"};
  const char *bg[] = {"background"};
  for (int fss = 0; fss <= 1; fss++) {
    for (int enabled = 0; enabled <= 1; enabled++) {
      bishfpm_config_t cfg = partition_config();
      cfg.max_itemset_order = 4;
      cfg.min_pattern_rate = 160;
      cfg.promotion_fraction = 0.10;
      cfg.subsumption_ratio = 0.0;
      bishfpm_t *engine = partition_engine(&cfg, enabled ? 1.1 : 0.0, fss);
      /* Two independent correlated blocks: all 1+3 partition lifts are
       * 2.5, but AB|CD has lift 1. Checking only singletons misses this. */
      feed(engine, ab, 2, 240);
      feed(engine, cd, 2, 240);
      feed(engine, bg, 1, 360);
      feed(engine, abcd, 4, 159);
      finish_fixture(engine, abcd, 4, NULL);
      assert(alerts_for("a\037b\037c\037d") == (enabled ? 0 : 1));
      assert_exact_pattern(engine, "a\037b\037c\037d", 160);
      bishfpm_free(engine);
    }

    bishfpm_config_t cfg = partition_config();
    cfg.max_itemset_order = 4;
    bishfpm_t *engine = partition_engine(&cfg, 2.5, fss);
    /* Marginals=400 and all proper multi-item counts=288. Every split
     * has lift at least 2.5, so a productive quad survives. */
    for (size_t i = 0; i < 4; i++)
      feed(engine, &abcd[i], 1, 112);
    feed(engine, bg, 1, 264);
    feed(engine, abcd, 4, 287);
    finish_fixture(engine, abcd, 4, NULL);
    assert(alerts_for("a\037b\037c\037d") == 1);
    bishfpm_free(engine);

    /* Missing pair history must not hide a proven 1+3 split. Here
     * ABC=400, D=900, ABCD=360, G=1000, so ABC|D has lift 1. */
    cfg.min_itemset_order = 3;
    cfg.min_pattern_rate = 360;
    cfg.subsumption_ratio = 0.0;
    engine = partition_engine(&cfg, 1.1, fss);
    feed(engine, abcd, 3, 40);
    feed(engine, &abcd[3], 1, 540);
    feed(engine, bg, 1, 60);
    feed(engine, abcd, 4, 359);
    bishfpm_alert_quality_stats_t delta;
    finish_fixture(engine, abcd, 4, &delta);
    assert(alerts_for("a\037b\037c\037d") == 0);
    assert(delta.candidates_suppressed >= 1);
    assert(delta.candidates_unknown >= 1);
    bishfpm_free(engine);
  }
}

static void test_partition_lift_gated_history(void) {
  const char *ab[] = {"a", "b"};
  const char *abc[] = {"a", "b", "c"};
  const char *bg[] = {"background"};
  for (int fss = 0; fss <= 1; fss++) {
    bishfpm_config_t cfg = partition_config();
    cfg.item_rate_threshold = cfg.min_pattern_rate = 100;
    cfg.promotion_fraction = cfg.min_lift = cfg.subsumption_ratio = 0.0;
    cfg.trigger_cooldown_ms = 0;
    bishfpm_t *engine = partition_engine(&cfg, 1.8, fss);
    for (int i = 0; i < 100; i++)
      feed(engine, ab, 2, 1);
    for (int i = 0; i < 199; i++)
      feed(engine, abc, 3, 1);
    feed(engine, bg, 1, 300);
    finish_fixture(engine, abc, 3, NULL);
    /* Full support is ABC=200, AB=300, C=200, G=600: minimum lift 2.
     * Gating leaves evaluated ABC=101 and AB=201; mixing those counts
     * with full marginals gives 1.507 and would incorrectly suppress. */
    assert(alerts_for("a\037b\037c") == 1);
    assert(bishfpm_group_rate(engine, "g", 1000) == 600);
    assert(bishfpm_item_rate(engine, "g", "c", 1000) == 200);
    assert_exact_pattern(engine, "a\037b", 201);
    assert_exact_pattern(engine, "a\037b\037c", 101);
    bishfpm_free(engine);
  }
}

static void test_partition_lift_unchanged_orders(void) {
  const char *items[] = {"a", "b", "c", "d", "e"};
  const unsigned orders[] = {2, 5};
  for (size_t i = 0; i < 2; i++) {
    bishfpm_config_t cfg = partition_config();
    cfg.min_itemset_order = cfg.max_itemset_order = orders[i];
    cfg.min_pattern_rate = 1;
    cfg.promotion_fraction = cfg.min_lift = cfg.subsumption_ratio = 0.0;
    bishfpm_t *engine = partition_engine(&cfg, 100.0, 0);
    bishfpm_alert_quality_stats_t delta;
    finish_fixture(engine, items, orders[i], &delta);
    assert(alert_count == 1);
    assert(delta.candidates_checked == 0 && delta.candidates_suppressed == 0 &&
           delta.candidates_unknown == 0);
    bishfpm_free(engine);
  }
}

int main(void) {
  test_partition_lift_independent_item();
  test_partition_lift_productive_and_unknown();
  test_partition_lift_api();
  test_partition_lift_quads();
  test_partition_lift_gated_history();
  test_partition_lift_unchanged_orders();
  printf("PASS: partition lift alert quality tests passed\n");
  /* test 1: verify lift calculation and filtering */
  {
    bishfpm_config_t cfg;
    bishfpm_config_default(&cfg);
    cfg.ewma_half_life_ms = 2000.0;
    cfg.item_rate_threshold = 4.0;
    cfg.promotion_fraction = 0.15;
    cfg.min_pattern_rate = 4.0;
    cfg.min_lift = 1.20; /* require at least 1.2x lift */
    cfg.min_itemset_order = 2;
    cfg.trigger_cooldown_ms = 3000;

    bishfpm_t *engine = bishfpm_create(&cfg);
    assert(engine != NULL);

    alert_count = 0;
    uint64_t t = 1000;

    /* send stream: 75% background "other=item", 25% coordinated burst {attr=A,
     * attr=B} */
    /* P(A) = 0.25, P(B) = 0.25, P(A,B) = 0.25 -> Lift = 0.25 / (0.25 * 0.25)
     * = 4.0 */
    for (int i = 0; i < 150; i++) {
      if (i % 4 == 0) {
        const char *items[] = {"attr=A", "attr=B"};
        bishfpm_add_event(engine, "test_grp", t, items, 2, on_test_trigger,
                          NULL);
      } else {
        const char *items[] = {"other=normal"};
        bishfpm_add_event(engine, "test_grp", t, items, 1, on_test_trigger,
                          NULL);
      }
      t += 20;
    }

    /* correlated burst must trigger alert with lift >= 1.20 */
    assert(alert_count > 0);
    assert(alert_lifts[alert_count - 1] >= 1.20);
    assert(strstr(alert_patterns[alert_count - 1], "attr=A") != NULL);
    assert(strstr(alert_patterns[alert_count - 1], "attr=B") != NULL);

    bishfpm_free(engine);
  }

  /* test 2: verify subsumption pruning of noisy supersets */
  {
    bishfpm_config_t cfg;
    bishfpm_config_default(&cfg);
    cfg.ewma_half_life_ms = 2000.0;
    cfg.item_rate_threshold = 4.0;
    cfg.promotion_fraction = 0.15;
    cfg.min_pattern_rate = 4.0;
    cfg.min_itemset_order = 2;
    cfg.max_itemset_order = 3;
    cfg.subsumption_ratio = 0.85; /* require 85% retention */
    cfg.trigger_cooldown_ms = 3000;

    bishfpm_t *engine = bishfpm_create(&cfg);
    assert(engine != NULL);

    alert_count = 0;
    uint64_t t = 10000;

    /* send 60% background, 40% attack {A, B}, with noisy passenger {C} on only
     * 10% */
    for (int i = 0; i < 200; i++) {
      if (i % 5 < 2) {
        /* attack event */
        const char *items[3];
        items[0] = "attr=A";
        items[1] = "attr=B";
        size_t n = 2;
        if (i % 10 == 0) {
          items[n++] = "attr=C_noise";
        }
        bishfpm_add_event(engine, "subsume_grp", t, items, n, on_test_trigger,
                          NULL);
      } else {
        const char *bg[] = {"other=item"};
        bishfpm_add_event(engine, "subsume_grp", t, bg, 1, on_test_trigger,
                          NULL);
      }
      t += 20;
    }

    /* root pattern {A, B} must alert, while {A, B, C_noise} must never alert */
    assert(alert_count > 0);
    int c_noise_count = 0;
    for (int k = 0; k < alert_count && k < MAX_ALERTS; k++) {
      if (strstr(alert_patterns[k], "attr=C_noise") != NULL) {
        c_noise_count++;
      }
    }
    assert(c_noise_count == 0);

    bishfpm_free(engine);
  }

  /* test 3: verify item_share_threshold */
  {
    bishfpm_config_t cfg;
    bishfpm_config_default(&cfg);
    cfg.ewma_half_life_ms = 1000.0;
    cfg.item_rate_threshold = 5.0;
    cfg.item_share_threshold = 0.50; /* item must have >= 50% of traffic */
    cfg.promotion_fraction = 0.20;
    cfg.min_pattern_rate = 5.0;
    cfg.min_itemset_order = 2;

    bishfpm_t *engine = bishfpm_create(&cfg);
    assert(engine != NULL);

    alert_count = 0;
    uint64_t t = 20000;

    /* send 100 events: all have X, 30% have Y (rate = 15/s, but share = 30% <
     * 50%) */
    for (int i = 0; i < 100; i++) {
      const char *items[2];
      items[0] = "attr=X";
      size_t n = 1;
      if (i % 3 == 0) {
        items[n++] = "attr=Y_low_share";
      }
      bishfpm_add_event(engine, "share_grp", t, items, n, on_test_trigger,
                        NULL);
      t += 20;
    }

    /* Y_low_share should not be gated as elevated despite rate > 5.0 */
    assert(alert_count == 0);

    bishfpm_free(engine);
  }

  printf("PASS: all suppression, lift, and subsumption tests passed\n");
  return 0;
}
