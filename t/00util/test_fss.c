#include "bishfpm.h"
#include "lib/compact.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

static void near(double a, double b) {
  assert(fabs(a - b) <= 1e-8 * fmax(1, fabs(b)));
}
/* All identities, including absent keys, are checked against an independent
 * decayed oracle after weighted/late input, eviction, collisions and pruning.
 */
static void oracle(unsigned width, bool bounded) {
  enum { N = 97 };
  compact_t *t = compact_new(7, 137, width, BISHFPM_PAIR_FSS, bounded ? .8 : 0);
  assert(t);
  double exact[N] = {0};
  uint64_t watermark = 0;
  unsigned rng = 42;
  for (unsigned step = 0; step < 6000; step++) {
    rng = rng * 1664525U + 1013904223U;
    unsigned id = step % 3 ? (rng % N) : (step / 1000) % 6;
    uint64_t time = bounded ? (step / 100) * 2000 + step % 100 : step;
    uint64_t timestamp = time > 20 && step % 11 == 0 ? time - 20 : time;
    uint64_t now = timestamp > watermark ? timestamp : watermark;
    for (unsigned i = 0; i < N; i++)
      exact[i] *= exp2(-(double)(now - watermark) / 137);
    double weight = step % 13 == 0 ? 3.5 : 1;
    exact[id] += weight * exp2(-(double)(now - timestamp) / 137);
    uint64_t hash = compact_hash(id + 1);
    compact_add(t, id + 1, hash, weight, timestamp);
    watermark = now;
    if (step % 79 == 0)
      compact_prune(t, now, .8);
    if (bounded && step % 41 == 0)
      assert(compact_prune_bounded(t, now, 3) <= 3);
    for (unsigned i = 0; i < N; i++) {
      hash = compact_hash(i + 1);
      topk_result_t r = compact_query(t, i + 1, hash, now);
      assert(r.lower <= exact[i] + 1e-7);
      assert(r.upper + 1e-7 >= exact[i]);
      assert(r.lower >= 0 && r.upper >= r.lower);
    }
  }
  compact_free(t);
}
static void transitions(void) {
  compact_t *t = compact_new(1, 1000, 1, BISHFPM_PAIR_FSS, 0);
  assert(t);
  assert(compact_add(t, 1, 1, 10, 0).present);
  topk_result_t r = compact_add(t, 2, 2, 3, 0);
  assert(!r.present);
  near(r.upper, 3);
  r = compact_add(t, 2, 2, 7, 0);
  assert(r.present && r.replaced);
  near(r.lower, 7);
  near(r.upper, 17);
  r = compact_query(t, 1, 1, 0);
  assert(!r.present);
  near(r.upper, 10);
  assert(compact_prune(t, 3000, 3) == 1);
  r = compact_query(t, 2, 2, 3000);
  near(r.upper, 17.0 / 8);
  r = compact_add(t, 2, 2, 2, 3000);
  assert(r.present && !r.replaced);
  near(r.lower, 2);
  near(r.upper, 17.0 / 8 + 2);
  compact_free(t);
  t = compact_new(1, 1000, 1, BISHFPM_PAIR_FSS, 0);
  assert(t && compact_add(t, 1, 1, DBL_MAX, 0).present);
  assert(!compact_add(t, 2, 2, DBL_MAX, 0).present);
  assert(t->heap_size == 1 && t->heap[0].key == 1);
  compact_free(t);
}
static void alert_count(const char *group, const bishfpm_trigger_t *trigger,
                        void *data) {
  (void)group;
  assert(trigger->pattern_rate >= 10);
  (*(unsigned *)data)++;
}
static void public_api(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.min_itemset_order = c.max_itemset_order = 2;
  c.item_rate_threshold = c.item_share_threshold = 0;
  c.min_pattern_rate = 1e100;
  c.max_groups = 1;
  c.max_items_per_group = 16;
  c.topk_k = 16;
  bishfpm_pair_options_t o = {BISHFPM_PAIR_FSS, 1, 3, 0};
  bishfpm_t *e = bishfpm_create_pairs(&c, &o);
  assert(e);
  const char *a[] = {"a", "b"}, *b[] = {"a", "c"};
  assert(bishfpm_add_event_weighted(e, "g", 0, a, 2, 10, NULL, NULL) >= 0);
  assert(bishfpm_add_event(e, "g", 0, b, 2, NULL, NULL) >= 0);
  bishfpm_count_bounds_t bounds;
  assert(bishfpm_pattern_bounds(e, "g",
                                "a\x1f"
                                "c",
                                0, &bounds));
  assert(bounds.known && !bounds.resident && bounds.upper >= 1);
  near(bishfpm_pattern_rate(e, "g",
                            "a\x1f"
                            "c",
                            0),
       bounds.upper);
  assert(bishfpm_pattern_bounds(e, "g",
                                "a\x1f"
                                "b",
                                0, &bounds));
  near(bishfpm_pattern_rate(e, "g",
                            "a\x1f"
                            "b",
                            0),
       bounds.upper);
  bishfpm_stats_t stats;
  bishfpm_get_stats(e, &stats);
  assert(stats.memory_current_bytes <= stats.memory_bound_bytes &&
         stats.memory_bound_bytes <= c.max_memory_bytes);
  assert(bishfpm_prune_pairs(e, "g", 999999, 10) == 0);
  bishfpm_free(e);
  c.min_pattern_rate = 10;
  c.min_lift = c.promotion_fraction = 0;
  o.sketch_width = 1;
  e = bishfpm_create_pairs(&c, &o);
  assert(e);
  unsigned alerts = 0;
  assert(bishfpm_add_event_weighted(e, "g", 0, a, 2, 10, alert_count, &alerts) >
         0);
  assert(bishfpm_add_event_weighted(e, "g", 0, b, 2, 10, alert_count, &alerts) >
         0);
  /* Keep the group alive while pruning its pair. Readmission has upper 21
   * and guaranteed lower 1; only the lower bound may drive notifications. */
  const char *single[] = {"a"}, *other[] = {"b", "c"};
  assert(bishfpm_add_event_weighted(e, "g", 0, single, 1, 20, alert_count,
                                    &alerts) == 0);
  bishfpm_prune(e, 0, 21);
  assert(bishfpm_add_event(e, "g", 0, other, 2, alert_count, &alerts) == 0);
  assert(alerts == 2);
  assert(bishfpm_pattern_bounds(e, "g",
                                "b\x1f"
                                "c",
                                0, &bounds));
  assert(bounds.resident);
  near(bounds.lower, 1);
  near(bounds.upper, 21);
  bishfpm_free(e);
  o.sketch_width = 0;
  assert(!bishfpm_create_pairs(&c, &o));
  o.sketch_width = (1U << 24) + 1;
  assert(!bishfpm_create_pairs(&c, &o));
  o.sketch_width = 3;
  o.store = BISHFPM_PAIR_SS;
  assert(!bishfpm_create_pairs(&c, &o));
}
int main(void) {
  transitions();
  oracle(1, false);
  oracle(17, false);
  oracle(18, false);
  oracle(1, true);
  oracle(17, true);
  oracle(18, true);
  public_api();
  puts("test_fss passed.");
}
