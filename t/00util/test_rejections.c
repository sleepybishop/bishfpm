#include "bishfpm.h"
#include "bishfpm_rate.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fail actual allocation sites, after engine construction. */
static size_t fail_after = SIZE_MAX;
static size_t allocation_attempts;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
char *__real_strdup(const char *);
static int fail(void) {
  allocation_attempts++;
  if (fail_after == SIZE_MAX)
    return 0;
  if (!fail_after)
    return 1;
  fail_after--;
  return 0;
}
void *__wrap_malloc(size_t size) { return fail() ? NULL : __real_malloc(size); }
void *__wrap_calloc(size_t n, size_t size) {
  return fail() ? NULL : __real_calloc(n, size);
}
char *__wrap_strdup(const char *s) { return fail() ? NULL : __real_strdup(s); }
static bishfpm_rejection_stats_t snapshot(bishfpm_t *e) {
  bishfpm_rejection_stats_t r;
  bishfpm_stats_t s;
  bishfpm_get_rejection_stats(e, &r);
  bishfpm_get_stats(e, &s);
  assert(r.invalid_input + r.reentrant + r.event_items_limit +
             r.pattern_length_limit + r.candidate_limit + r.group_limit +
             r.marginal_limit + r.allocation_failure ==
         s.events_rejected);
  return r;
}
static bishfpm_t *create(bishfpm_config_t *c) {
  bishfpm_t *e = bishfpm_create(c);
  assert(e);
  return e;
}
static void nested(const char *group, const bishfpm_trigger_t *t, void *ud) {
  const char *items[] = {"a", "b"};
  (void)t;
  assert(bishfpm_add_event(ud, group, 1, items, 2, NULL, NULL) == -1);
}
/* Compare every observable count with a control that omitted the failed call.
 * Future-dated failures must not advance decay, insert identities or evict
 * pairs. */
static void same_counts(bishfpm_t *a, bishfpm_t *b, const char *const *keys,
                        size_t n) {
  assert(bishfpm_group_rate(a, "g", 0) == bishfpm_group_rate(b, "g", 0));
  for (size_t i = 0; i < n; i++) {
    assert(bishfpm_item_rate(a, "g", keys[i], 0) ==
           bishfpm_item_rate(b, "g", keys[i], 0));
    for (size_t j = i + 1; j < n; j++) {
      char pattern[BISHFPM_MAX_PATTERN_LEN];
      snprintf(pattern, sizeof(pattern), "%s\037%s", keys[i], keys[j]);
      bishfpm_count_bounds_t x, y;
      assert(bishfpm_pattern_bounds(a, "g", pattern, 0, &x));
      assert(bishfpm_pattern_bounds(b, "g", pattern, 0, &y));
      assert(x.lower == y.lower && x.upper == y.upper && x.known == y.known &&
             x.resident == y.resident);
    }
  }
  bishfpm_stats_t x, y;
  bishfpm_get_stats(a, &x);
  bishfpm_get_stats(b, &y);
  assert(x.events_accepted == y.events_accepted &&
         x.candidates_evaluated == y.candidates_evaluated &&
         x.pattern_replacements == y.pattern_replacements &&
         x.active_groups == y.active_groups);
  assert(x.memory_current_bytes <= x.memory_bound_bytes);
  bishfpm_pair_stats_t px, py;
  bishfpm_get_pair_stats(a, &px);
  bishfpm_get_pair_stats(b, &py);
  assert(px.resident_pairs == py.resident_pairs &&
         px.patterns_forgotten == py.patterns_forgotten);
}
static void numeric_failures(void) {
  for (int kind = BISHFPM_PAIR_SS; kind <= BISHFPM_PAIR_FSS; kind++) {
    unsigned failures = 0, constructed = 0;
    for (size_t site = 0; site < 32; site++) {
      bishfpm_config_t c;
      bishfpm_config_default(&c);
      c.min_itemset_order = c.max_itemset_order = 2;
      c.item_rate_threshold = 0;
      bishfpm_pair_options_t o = {(bishfpm_pair_store_t)kind, 16,
                                  kind == BISHFPM_PAIR_FSS ? 17U : 0U, .25};
      fail_after = site;
      bishfpm_t *e = bishfpm_create_pairs(&c, &o);
      fail_after = SIZE_MAX;
      if (e) {
        bishfpm_free(e);
        constructed = 1;
        break;
      }
    }
    assert(constructed);
    for (unsigned initial = 0; initial < 3; initial++) {
      unsigned reached_success = 0;
      for (size_t site = 0; site < 32; site++) {
        bishfpm_config_t c;
        bishfpm_config_default(&c);
        c.min_itemset_order = c.max_itemset_order = 2;
        c.max_groups = 1;
        c.max_items_per_group = 64;
        c.topk_k = 16;
        c.item_rate_threshold = c.item_share_threshold = 0;
        c.min_pattern_rate = 1e100;
        c.ewma_half_life_ms = 100;
        bishfpm_pair_options_t o = {(bishfpm_pair_store_t)kind, 1024,
                                    kind == BISHFPM_PAIR_FSS ? 17U : 0U, .25};
        bishfpm_t *a = bishfpm_create_pairs(&c, &o);
        bishfpm_t *b = bishfpm_create_pairs(&c, &o);
        assert(a && b);
        char names[34][16];
        const char *keys[34];
        for (unsigned i = 0; i < 34; i++) {
          snprintf(names[i], sizeof(names[i]), "key%02u", i);
          keys[i] = names[i];
        }
        size_t seeded = initial == 2 ? 32 : initial;
        if (seeded) {
          assert(bishfpm_add_event(a, "g", 100, keys, seeded, NULL, NULL) >= 0);
          assert(bishfpm_add_event(b, "g", 100, keys, seeded, NULL, NULL) >= 0);
        }
        fail_after = site;
        int rc = bishfpm_add_event(a, "g", 10000, keys + 32, 2, NULL, NULL);
        fail_after = SIZE_MAX;
        if (rc < 0) {
          failures++;
          assert(snapshot(a).allocation_failure == 1);
          bishfpm_stats_t budget;
          bishfpm_get_stats(a, &budget);
          assert(budget.budget_rejections == 0);
          same_counts(a, b, keys, 34);
          /* A late retry makes an incorrectly advanced watermark observable. */
          assert(bishfpm_add_event(a, "g", 101, keys + 32, 2, NULL, NULL) >= 0);
          assert(bishfpm_add_event(b, "g", 101, keys + 32, 2, NULL, NULL) >= 0);
          same_counts(a, b, keys, 34);
        } else {
          reached_success++;
        }
        bishfpm_free(a);
        bishfpm_free(b);
        if (reached_success)
          break; /* Every earlier fallible site was independently injected. */
      }
      assert(reached_success);
    }
    assert(failures >= 8);
  }
}
static void allocation_free_prune(void) {
  bishfpm_rate_table_t *table = bishfpm_rate_table_new(16);
  assert(table);
  assert(bishfpm_rate_table_update(table, "a", 1000, 1000) == 1);
  size_t attempts = allocation_attempts;
  fail_after = 0;
  assert(bishfpm_rate_table_prune(table, 2000, 1000, .1) == 0);
  fail_after = SIZE_MAX;
  assert(allocation_attempts == attempts);
  assert(bishfpm_rate_table_query(table, "a", 2000, 1000) == .5);
  bishfpm_rate_table_free(table);

  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.item_rate_threshold = 0;
  c.min_pattern_rate = c.promotion_fraction = 0;
  c.ewma_half_life_ms = 1000;
  c.trigger_cooldown_ms = 0;
  bishfpm_t *e = create(&c);
  const char *ab[] = {"a", "b"};
  assert(bishfpm_add_event(e, "g", 0, ab, 2, NULL, NULL) >= 0);
  /* Warm up marginal compaction, then populate a stronger group count. */
  assert(bishfpm_prune(e, 0, .1) == 0);
  const char *a[] = {"a"};
  assert(bishfpm_add_event_weighted(e, "g", 1000, a, 1, 10, NULL, NULL) >= 0);
  fail_after = 0;
  assert(bishfpm_prune(e, 1000, 1) == 0);
  fail_after = SIZE_MAX;
  assert(bishfpm_pattern_rate(e, "g", "a\037b", 1000) == 0);
  bishfpm_stats_t stats;
  bishfpm_get_stats(e, &stats);
  assert(stats.items_pruned ==
         0); /* Marginal deletion still needs rehash storage. */
  assert(bishfpm_group_rate(e, "g", 1000) == 10.5);
  size_t before_bytes = stats.memory_current_bytes;
  assert(bishfpm_prune(e, 1000, 1) == 0);
  bishfpm_get_stats(e, &stats);
  assert(stats.items_pruned == 1);
  assert(stats.memory_current_bytes == before_bytes - sizeof("b"));
  bishfpm_free(e);
}

int main(void) {
  allocation_free_prune();
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.max_groups = 1;
  c.max_items_per_group = 2;
  c.max_event_items = 3;
  c.max_candidates_per_event = 1;
  c.min_itemset_order = c.max_itemset_order = 2;
  bishfpm_t *e = create(&c);
  const char *ab[] = {"a", "b"}, *abc[] = {"a", "b", "c"};
  const char *wide[] = {"a", "b", "c", "d"};
  const char *bad[] = {"a\x1f"
                       "b"};
  assert(bishfpm_add_event(e, NULL, 1, ab, 2, NULL, NULL) == -1);
  assert(bishfpm_add_event(e, "", 1, ab, 2, NULL, NULL) == -1);
  assert(bishfpm_add_event(e, "g", 1, bad, 1, NULL, NULL) == -1);
  assert(bishfpm_add_event(e, "g", 1, wide, 4, NULL, NULL) == -1);
  assert(bishfpm_add_event(e, "g", 1, abc, 3, NULL, NULL) == -1);
  assert(bishfpm_add_event(e, "g", 1, ab, 2, NULL, NULL) >= 0);
  const char *ac[] = {"a", "c"};
  assert(bishfpm_add_event(e, "g", 1, ac, 2, NULL, NULL) == -1);
  assert(bishfpm_add_event(e, "h", 1, ab, 2, NULL, NULL) == -1);
  assert(bishfpm_group_rate(e, "g", 1) == 1);
  bishfpm_rejection_stats_t r = snapshot(e);
  assert(r.invalid_input == 3 && r.event_items_limit == 1 &&
         r.candidate_limit == 1 && r.marginal_limit == 1 && r.group_limit == 1);
  bishfpm_stats_t budget;
  bishfpm_get_stats(e, &budget);
  assert(budget.budget_rejections == 4);
  bishfpm_rejection_stats_t unchanged = r;
  bishfpm_get_rejection_stats(NULL, &unchanged);
  assert(!memcmp(&r, &unchanged, sizeof(r)));
  bishfpm_get_rejection_stats(e, NULL);
  bishfpm_free(e);

  bishfpm_config_default(&c);
  e = create(&c);
  char long_items[3][201];
  const char *long_keys[3];
  for (int i = 0; i < 3; i++) {
    memset(long_items[i], 'a' + i, 200);
    long_items[i][200] = 0;
    long_keys[i] = long_items[i];
  }
  assert(bishfpm_add_event(e, "g", 1, long_keys, 3, NULL, NULL) == -1);
  assert(snapshot(e).pattern_length_limit == 1);
  bishfpm_free(e);

  c.item_rate_threshold = c.min_pattern_rate = c.promotion_fraction = 0;
  e = create(&c);
  assert(bishfpm_add_event(e, "g", 1, ab, 2, nested, e) >= 0);
  assert(snapshot(e).reentrant == 1);
  bishfpm_free(e);

  unsigned failed = 0;
  for (int existing = 0; existing < 3; existing++) {
    for (size_t site = 0; site < 16; site++) {
      bishfpm_config_default(&c);
      e = create(&c);
      const char *x[] = {"x"};
      if (existing == 1)
        assert(bishfpm_add_event(e, "g", 1, x, 1, NULL, NULL) >= 0);
      if (existing == 2) {
        char names[32][16];
        const char *seed[32];
        for (int i = 0; i < 32; i++) {
          snprintf(names[i], sizeof(names[i]), "seed-%d", i);
          seed[i] = names[i];
        }
        assert(bishfpm_add_event(e, "g", 1, seed, 32, NULL, NULL) >= 0);
      }
      fail_after = site;
      int rc = bishfpm_add_event(e, "g", 1, ab, 2, NULL, NULL);
      fail_after = SIZE_MAX;
      r = snapshot(e);
      if (rc < 0) {
        failed++;
        assert(r.allocation_failure == 1 && r.marginal_limit == 0);
        assert(bishfpm_group_rate(e, "g", 1) == (existing > 0));
        bishfpm_stats_t s;
        bishfpm_get_stats(e, &s);
        assert(s.active_groups == (size_t)(existing > 0));
        assert(s.budget_rejections == 0);
        assert(bishfpm_item_rate(e, "g", "a", 1) == 0);
        assert(bishfpm_add_event(e, "g", 1, ab, 2, NULL, NULL) >= 0);
        assert(snapshot(e).allocation_failure == 1);
      }
      bishfpm_free(e);
    }
  }
  assert(failed >= 8);
  bishfpm_config_default(&c);
  c.min_itemset_order = c.max_itemset_order = 2;
  c.item_rate_threshold = c.item_share_threshold = 0;
  bishfpm_pair_options_t pairs = {BISHFPM_PAIR_THRESHOLD, 1, 0, 0};
  e = bishfpm_create_pairs(&c, &pairs);
  assert(e);
  assert(bishfpm_add_event(e, "g", 1, ab, 2, NULL, NULL) >= 0);
  assert(bishfpm_add_event(e, "g", 10000, ac, 2, NULL, NULL) == -1);
  assert(snapshot(e).candidate_limit == 1);
  bishfpm_pair_stats_t ps;
  bishfpm_get_pair_stats(e, &ps);
  assert(ps.capacity_rejections == 1);
  assert(bishfpm_group_rate(e, "g", 0) == 1);
  bishfpm_free(e);
  numeric_failures();
  puts("test_rejections passed");
  return 0;
}
