#include "bishfpm.h"
#include "lib/internal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void near(double a, double b) {
  assert(fabs(a - b) < 1e-8 * (1 + fabs(b)));
}

static void store_regressions(void) {
  bishfpm_topk_t *s = bishfpm_topk_new(4, 1000);
  assert(s);
  assert(bishfpm_hash_str("candidate-3405", 0) ==
         bishfpm_hash_str("r8(4>w:v", 0));
  bishfpm_topk_add(s, "candidate-3405", 1, 1000);
  bishfpm_topk_add(s, "r8(4>w:v", 2, 1000);
  bishfpm_topk_add(s, "candidate-3405", 3, 1000);
  assert(s->heap_size == 2);
  near(bishfpm_topk_query(s, "candidate-3405", 1000), 4);
  near(bishfpm_topk_query(s, "r8(4>w:v", 1000), 2);
  assert(bishfpm_topk_prune(s, 1000, 3) == 1);
  near(bishfpm_topk_query(s, "candidate-3405", 1000), 4);
  bishfpm_topk_set_seed(s, 123);
  near(bishfpm_topk_query(s, "candidate-3405", 1000), 4);
  bishfpm_topk_free(s);
  s = bishfpm_topk_new(1, 1000);
  bishfpm_topk_add(s, "A", 10, 1000);
  for (int i = 0; i < 100; i++)
    bishfpm_topk_add(s, "B", 1, 1000);
  near(bishfpm_topk_query_lower(s, "B", 1000), 100);
  near(bishfpm_topk_query(s, "B", 1000), 110);
  bishfpm_topk_t *batch = bishfpm_topk_new(1, 1000);
  assert(batch);
  bishfpm_topk_add(batch, "A", 10, 1000);
  bishfpm_topk_add(batch, "B", 100, 1000);
  near(bishfpm_topk_query(batch, "B", 1000), bishfpm_topk_query(s, "B", 1000));
  near(bishfpm_topk_query_lower(batch, "B", 1000),
       bishfpm_topk_query_lower(s, "B", 1000));
  bishfpm_topk_free(batch);
  bishfpm_topk_item_t out;
  assert(bishfpm_topk_get_top(s, 2000, &out, 1) == 1);
  near(out.rate, 55);
  near(out.error, 5);
  assert(out.last_time_ms == 2000);
  bishfpm_topk_free(s);
  s = bishfpm_topk_new(2, 1000);
  bishfpm_topk_add(s, "A", 2, 0);
  bishfpm_topk_add(s, "B", 4, 1000);
  near(bishfpm_topk_add(s, "A", 3, 0), 2.5);
  near(bishfpm_topk_min_rate(s, 1000), 2.5);
  bishfpm_topk_add(s, "C", 3, 1000);
  near(bishfpm_topk_query_lower(s, "C", 1000), 3);
  bishfpm_topk_free(s);
}

static void pruning_readmission(void) {
  bishfpm_topk_t *s = bishfpm_topk_new(2, 1000);
  assert(s);
  bishfpm_topk_add(s, "a", 2, 1000);
  bishfpm_topk_add(s, "b", 4, 1000);
  assert(bishfpm_topk_prune(s, 1000, 3) == 1);
  near(bishfpm_topk_add(s, "c", 1, 1000), 3);
  near(bishfpm_topk_query_lower(s, "c", 1000), 1);
  bishfpm_topk_add(s, "a", 1, 1000);
  double upper = bishfpm_topk_query(s, "a", 1000);
  assert(upper >= 3);
  assert(bishfpm_topk_query_lower(s, "a", 1000) <= 3);
  bishfpm_topk_free(s);
}

static void oracle(void) {
  char keys[32][24];
  for (int i = 0; i < 32; i++)
    snprintf(keys[i], sizeof(keys[i]), "key-%d", i);
  for (unsigned k = 1; k <= 16; k *= 2) {
    bishfpm_topk_t *s = bishfpm_topk_new(k, 1000);
    bishfpm_rate_t truth[32] = {{0}};
    uint64_t rng = 43, watermark = 0;
    for (unsigned n = 0; n < 10000; n++) {
      uint64_t r = bishfpm_splitmix64(&rng), t = n / 4;
      if (t > watermark)
        watermark = t;
      if (n % 7 == 0 && t > 200)
        t -= 200;
      unsigned key = (r >> 8) % 32, weight = 1 + (r % 97);
      bishfpm_rate_update_weighted(&truth[key], t, 1000, weight);
      bishfpm_topk_add(s, keys[key], weight, t);
      if (n % 211 == 0)
        bishfpm_topk_prune(s, watermark, 0.1);
      bishfpm_topk_item_t out[16];
      size_t count = bishfpm_topk_get_top(s, watermark, out, 16);
      for (size_t i = 0; i < count; i++) {
        int idx = -1;
        for (int j = 0; j < 32; j++)
          if (!strcmp(keys[j], out[i].pattern))
            idx = j;
        assert(idx >= 0);
        double exact = bishfpm_rate_query(&truth[idx], watermark, 1000);
        assert(out[i].rate + 1e-7 >= exact);
        assert(out[i].rate - out[i].error <= exact + 1e-7);
        near(out[i].rate, bishfpm_topk_query(s, out[i].pattern, watermark));
        if (i)
          assert(out[i - 1].rate >= out[i].rate);
        for (size_t j = 0; j < i; j++)
          assert(strcmp(out[j].pattern, out[i].pattern));
      }
      for (uint32_t i = 1; i < s->heap_size; i++) {
        double child = bishfpm_topk_query(s, s->heap[i].pattern, watermark);
        double parent =
            bishfpm_topk_query(s, s->heap[(i - 1) / 2].pattern, watermark);
        assert(parent <= child + 1e-7);
      }
    }
    bishfpm_topk_free(s);
  }
}

static void check_clock(const char *g, const bishfpm_trigger_t *t, void *ud) {
  (void)g;
  (void)ud;
  assert(t->rate_fraction <= 1 + 1e-9);
  assert(t->timestamp == 10001);
  near(t->pattern_rate, 0.0107421875);
}

static bishfpm_config_t open_config(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.item_rate_threshold = c.min_pattern_rate = c.promotion_fraction = 0;
  c.min_itemset_order = c.max_itemset_order = 1;
  c.trigger_cooldown_ms = 0;
  return c;
}

static void engine_regressions(void) {
  bishfpm_config_t c = open_config();
  c.ewma_half_life_ms = 1000;
  bishfpm_t *e = bishfpm_create(&c);
  assert(e);
  const char *a[] = {"A"}, *b[] = {"B"};
  bishfpm_add_event_weighted(e, "g", 1, b, 1, 10, NULL, NULL);
  bishfpm_add_event(e, "g", 10001, a, 1, NULL, NULL);
  bishfpm_add_event(e, "g", 1, b, 1, check_clock, NULL);
  near(bishfpm_item_rate(e, "g", "B", 1), 0.0107421875);
  bishfpm_prune(e, 11001, 0.001);
  bishfpm_add_event(e, "g", 1, b, 1, NULL, NULL);
  assert(bishfpm_pattern_rate(e, "g", "B", 11001) < 0.01);
  bishfpm_free(e);

  c = open_config();
  c.max_items_per_group = 1;
  e = bishfpm_create(&c);
  assert(e);
  const char *ab[] = {"A", "B"};
  assert(bishfpm_add_event(e, "g", 1, ab, 2, NULL, NULL) == -1);
  bishfpm_stats_t st;
  bishfpm_get_stats(e, &st);
  assert(st.active_groups == 0 && st.events_rejected == 1);
  bishfpm_add_event(e, "g", 1, a, 1, NULL, NULL);
  assert(bishfpm_add_event(e, "g", 1, ab, 2, NULL, NULL) == -1);
  near(bishfpm_group_rate(e, "g", 1), 1);
  bishfpm_free(e);

  c = open_config();
  c.min_itemset_order = c.max_itemset_order = 2;
  e = bishfpm_create(&c);
  assert(e);
  char names[33][12];
  const char *items[33];
  for (int i = 0; i < 33; i++) {
    snprintf(names[i], 12, "attr-%d", i);
    items[i] = names[i];
  }
  assert(bishfpm_add_event(e, "g", 1, items, 33, NULL, NULL) >= 0);
  bishfpm_get_stats(e, &st);
  assert(st.candidates_evaluated == 528);
  bishfpm_free(e);
  c.max_candidates_per_event = 527;
  e = bishfpm_create(&c);
  assert(e);
  assert(bishfpm_add_event(e, "g", 1, items, 33, NULL, NULL) == -1);
  bishfpm_get_stats(e, &st);
  assert(st.active_groups == 0 && st.budget_rejections == 1);
  bishfpm_free(e);
  c = open_config();
  c.max_memory_bytes = 1;
  assert(!bishfpm_create(&c));
}

/* Exercise the explicit ceilings and reject over-budget work before mutation.
 */
static void wide_basket(void) {
  enum { N = BISHFPM_MAX_EVENT_ITEMS };
  const uint32_t pairs = N * (N - 1U) / 2;
  char names[N][16];
  const char *items[N];
  for (unsigned i = 0; i < N; i++) {
    snprintf(names[i], sizeof(names[i]), "wide%04u", i);
    items[i] = names[i];
  }
  bishfpm_config_t c = open_config();
  c.min_itemset_order = c.max_itemset_order = 2;
  c.min_pattern_rate = 1e100;
  c.max_groups = 1;
  c.max_items_per_group = N;
  c.max_event_items = N;
  c.max_candidates_per_event = pairs - 1;
  bishfpm_t *e = bishfpm_create(&c);
  assert(e);
  assert(bishfpm_add_event(e, "g", 10000, items, N, NULL, NULL) == -1);
  bishfpm_stats_t stats;
  bishfpm_get_stats(e, &stats);
  assert(stats.active_groups == 0 && stats.candidates_evaluated == 0 &&
         stats.budget_rejections == 1);
  near(bishfpm_group_rate(e, "g", 0), 0);
  near(bishfpm_item_rate(e, "g", items[N - 1], 0), 0);
  bishfpm_free(e);

  c.max_candidates_per_event = pairs;
  e = bishfpm_create(&c);
  assert(e);
  assert(bishfpm_add_event(e, "g", 0, items, N, NULL, NULL) == 0);
  bishfpm_get_stats(e, &stats);
  assert(stats.events_accepted == 1 && stats.candidates_evaluated == pairs);
  assert(stats.memory_current_bytes <= stats.memory_bound_bytes &&
         stats.memory_bound_bytes <= c.max_memory_bytes);
  near(bishfpm_group_rate(e, "g", 0), 1);
  near(bishfpm_item_rate(e, "g", items[N - 1], 0), 1);
  /* Input count is checked before reading the extra pointer. */
  assert(bishfpm_add_event(e, "g", 1, items, N + 1, NULL, NULL) == -1);
  near(bishfpm_group_rate(e, "g", 0), 1);
  bishfpm_free(e);

  c.max_candidates_per_event = BISHFPM_MAX_EVENT_CANDIDATES;
  e = bishfpm_create(&c);
  assert(e);
  bishfpm_free(e);
  c.max_event_items++;
  assert(!bishfpm_create(&c));
  c.max_event_items = N;
  c.max_candidates_per_event++;
  assert(!bishfpm_create(&c));
}

static void arena_invariants(void) {
  unsigned char buf[128];
  bishfpm_arena_t *arena = bishfpm_arena_new_buffer(buf, sizeof(buf));
  assert(arena);
  uint8_t *saved = arena->beg;
  assert(!bishfpm_topk_new_arena(arena, 64, 1000));
  assert(arena->beg == saved && bishfpm_arena_used(arena) == 0);
  assert(!bishfpm_alloc_uninit(arena, 1, 3, 1));
  bishfpm_arena_free(arena); /* wrapper freed; caller buffer remains valid */
  buf[0] = 42;
  assert(buf[0] == 42);
}

static void count_triples(const char *g, const bishfpm_trigger_t *t, void *ud) {
  (void)g;
  if (t->item_count == 3)
    (*(int *)ud)++;
}

static void suppression(void) {
  for (int enabled = 0; enabled < 2; enabled++) {
    bishfpm_config_t c = open_config();
    c.min_itemset_order = 2;
    c.max_itemset_order = 3;
    c.trigger_cooldown_ms = 10000;
    c.subsumption_ratio = enabled ? 0.85 : 0;
    bishfpm_t *e = bishfpm_create(&c);
    assert(e);
    const char *ab[] = {"a", "b"}, *abc[] = {"a", "b", "c"};
    bishfpm_add_event_weighted(e, "g", 1000, ab, 2, 100, NULL, NULL);
    int triples = 0;
    bishfpm_add_event(e, "g", 1000, abc, 3, count_triples, &triples);
    assert(triples == (enabled ? 0 : 1));
    bishfpm_free(e);
  }
}

static void saturation(void) {
  bishfpm_config_t c = open_config();
  c.max_groups = 1;
  c.max_items_per_group = 8192;
  c.trigger_cooldown_ms = 10000;
  c.topk_k = 1;
  bishfpm_t *e = bishfpm_create(&c);
  assert(e);
  char key[24];
  const char *items[] = {key};
  for (int i = 0; i < 4097; i++) {
    snprintf(key, sizeof(key), "key-%d", i);
    int rc = bishfpm_add_event(e, "g", 1000, items, 1, NULL, NULL);
    assert(rc == (i < 4096 ? 1 : 0));
  }
  snprintf(key, sizeof(key), "key-0");
  assert(bishfpm_add_event(e, "g", 1000, items, 1, NULL, NULL) == 0);
  bishfpm_stats_t st;
  bishfpm_get_stats(e, &st);
  assert(st.cooldown_saturation == 1 && st.callbacks_delivered == 0);
  assert(st.events_accepted == 4098);
  assert(st.memory_current_bytes <= st.memory_bound_bytes);
  bishfpm_free(e);
}

/* Presence survives numerical underflow; zero is not an absence sentinel. */
static void zero_resident(void) {
  bishfpm_topk_t *s = bishfpm_topk_new(1, 1);
  assert(s);
  topk_result_t r = topk_add_result(s, "a", 1, 0);
  assert(r.present && !r.was_present && !r.replaced);
  r = topk_query_result(s, "a", UINT64_MAX);
  assert(r.present && r.upper == 0 && r.lower == 0);
  assert(!topk_query_result(s, "missing", UINT64_MAX).present);
  r = topk_add_result(s, "a", 1, UINT64_MAX);
  assert(r.present && r.was_present && !r.replaced && r.lower == 1);
  r = topk_add_result(s, "b", 1, UINT64_MAX);
  assert(r.present && !r.was_present && r.replaced && r.upper == 2 &&
         r.lower == 1);
  bishfpm_topk_free(s);

  bishfpm_config_t c = open_config();
  c.topk_k = 1;
  c.ewma_half_life_ms = 1;
  bishfpm_t *e = bishfpm_create(&c);
  const char *items[] = {"a"};
  assert(bishfpm_add_event(e, "g", 0, items, 1, NULL, NULL) == 1);
  assert(bishfpm_add_event(e, "g", UINT64_MAX, items, 1, NULL, NULL) == 1);
  bishfpm_stats_t stats;
  bishfpm_get_stats(e, &stats);
  assert(stats.pattern_replacements == 0);
  bishfpm_free(e);
}

/* The arena layout works for every initial alignment and rolls back on failure.
 */
static void layout_alignment(void) {
  for (uint32_t k = 1; k <= 65; k += 16) {
    topk_layout_t layout;
    assert(topk_layout(k, 1000, &layout));
    unsigned char *buffer = malloc(layout.arena_bytes + 16);
    assert(buffer);
    for (size_t offset = 0; offset < 16; offset++) {
      bishfpm_arena_t *a =
          bishfpm_arena_new_buffer(buffer + offset, layout.arena_bytes);
      assert(a);
      bishfpm_topk_t *s = bishfpm_topk_new_arena(a, k, 1000);
      assert(s && a->beg <= a->end);
      assert(s->slot_map_cap >= 2 * k);
      near(bishfpm_topk_add(s, "pattern", 7, 0), 7);
      bishfpm_arena_free(a);
    }
    free(buffer);
  }
}

/* Callback queries observe only candidates evaluated so far, and nested
 * rejection must leave the outer busy guard intact for the next callback. */
typedef struct {
  bishfpm_t *engine;
  unsigned step;
} visibility_t;
static void visible_callback(const char *group, const bishfpm_trigger_t *t,
                             void *ud) {
  visibility_t *v = ud;
  const char *patterns[] = {"a\x1f"
                            "b",
                            "a\x1f"
                            "c",
                            "b\x1f"
                            "c",
                            "a\x1f"
                            "b\x1f"
                            "c"};
  assert(v->step < 4 && !strcmp(t->pattern, patterns[v->step]));
  for (unsigned i = 0; i < 4; i++)
    near(bishfpm_pattern_rate(v->engine, group, patterns[i], 0),
         i <= v->step ? 1 : 0);
  const char *items[] = {"nested"};
  assert(bishfpm_add_event(v->engine, "nested", 0, items, 1, NULL, NULL) == -1);
  assert(bishfpm_prune(v->engine, 0, 2) == 0);
  bishfpm_free(v->engine);
  near(t->lift, 1);
  v->step++;
}
static void callback_visibility(void) {
  bishfpm_config_t c = open_config();
  c.min_itemset_order = 2;
  c.max_itemset_order = 3;
  visibility_t v = {bishfpm_create(&c), 0};
  assert(v.engine);
  const char *items[] = {"c", "b", "a", "a"};
  assert(bishfpm_add_event(v.engine, "g", 0, items, 4, visible_callback, &v) ==
         4);
  assert(v.step == 4);
  bishfpm_stats_t s;
  bishfpm_get_stats(v.engine, &s);
  assert(s.events_total == 5 && s.events_accepted == 1 &&
         s.events_rejected == 4);
  assert(s.callbacks_delivered == 4 && s.pattern_replacements == 0);
  bishfpm_free(v.engine);
}

static void representation_boundaries(void) {
  char first[256], second[257], pattern[BISHFPM_MAX_PATTERN_LEN];
  memset(first, 'a', 255);
  first[255] = 0;
  memset(second, 'b', 255);
  second[255] = 0;
  const char *items[] = {first, second};
  assert(bishfpm_itemset_serialize(items, 2, pattern, sizeof(pattern)) == 511);
  bishfpm_topk_t *store = bishfpm_topk_new(1, 1000);
  assert(store && bishfpm_topk_add(store, pattern, 7, 0) == 7);
  bishfpm_topk_item_t top;
  assert(bishfpm_topk_get_top(store, 0, &top, 1) == 1);
  assert(!strcmp(top.pattern, pattern));
  bishfpm_topk_free(store);
  second[255] = 'b';
  second[256] = 0;
  assert(bishfpm_itemset_serialize(items, 2, pattern, sizeof(pattern)) == 0);

  bishfpm_config_t c = open_config();
  c.min_itemset_order = c.max_itemset_order = BISHFPM_MAX_ITEMSET_ORDER;
  char names[BISHFPM_MAX_ITEMSET_ORDER][2];
  const char *all[BISHFPM_MAX_ITEMSET_ORDER];
  for (size_t i = 0; i < BISHFPM_MAX_ITEMSET_ORDER; i++) {
    names[i][0] = '!' + (char)i;
    names[i][1] = 0;
    all[i] = names[i];
  }
  bishfpm_t *e = bishfpm_create(&c);
  assert(e);
  assert(bishfpm_add_event(e, "g", 0, all, BISHFPM_MAX_ITEMSET_ORDER, NULL,
                           NULL) == 1);
  bishfpm_stats_t stats;
  bishfpm_get_stats(e, &stats);
  assert(stats.candidates_evaluated == 1);
  bishfpm_free(e);
  c.max_itemset_order++;
  assert(!bishfpm_create(&c));
}

static void subset_prefixes(void) {
  const char *parents[][2] = {{"a", "b"}, {"aa", "b"}};
  const char *candidates[][3] = {{"a", "b", "c"}, {"aa", "b", "c"}};
  for (unsigned p = 0; p < 2; p++) {
    for (unsigned q = 0; q < 2; q++) {
      bishfpm_config_t c = open_config();
      c.min_itemset_order = 2;
      c.max_itemset_order = 3;
      c.subsumption_ratio = 0.85;
      c.trigger_cooldown_ms = 10000;
      bishfpm_t *e = bishfpm_create(&c);
      assert(e);
      assert(bishfpm_add_event_weighted(e, "g", 0, parents[p], 2, 100, NULL,
                                        NULL) == 1);
      int triples = 0;
      bishfpm_add_event(e, "g", 0, candidates[q], 3, count_triples, &triples);
      assert(triples == (p == q ? 0 : 1));
      bishfpm_free(e);
    }
  }
}

int main(void) {
  subset_prefixes();
  representation_boundaries();
  zero_resident();
  layout_alignment();
  callback_visibility();
  store_regressions();
  pruning_readmission();
  oracle();
  engine_regressions();
  wide_basket();
  arena_invariants();
  suppression();
  saturation();
  puts("test_invariants passed.");
  return 0;
}
