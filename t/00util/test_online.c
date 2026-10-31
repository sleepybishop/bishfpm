#include "bishfpm.h"
#include "bishfpm_hash.h"
#include "bishfpm_itemset.h"
#include "lib/compact.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void near(double x, double y) {
  assert(fabs(x - y) <= 1e-7 * fmax(1, fabs(y)));
}
static void pattern(const char *a, const char *b, char *out) {
  const char *items[] = {a, b};
  bishfpm_itemset_sort(items, 2);
  assert(bishfpm_itemset_serialize(items, 2, out, BISHFPM_MAX_PATTERN_LEN));
}
typedef struct {
  bishfpm_trigger_t t[64];
  size_t n;
} notifications;
static void collect(const char *g, const bishfpm_trigger_t *t, void *p) {
  (void)g;
  notifications *n = p;
  assert(n->n < 64);
  n->t[n->n++] = *t;
}
static void equivalence(int gates) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.min_itemset_order = c.max_itemset_order = 2;
  c.max_groups = 1;
  c.max_items_per_group = 64;
  c.topk_k = 16;
  c.item_rate_threshold = gates ? 2 : 0;
  c.item_share_threshold = gates ? .01 : 0;
  c.min_lift = gates ? .5 : 0;
  c.min_pattern_rate = gates ? 2 : 1;
  c.promotion_fraction = .1;
  c.trigger_cooldown_ms = 0;
  c.hash_seed = 42;
  c.ewma_half_life_ms = 100;
  bishfpm_pair_options_t o = {BISHFPM_PAIR_SS, 16, 0, 0};
  bishfpm_t *a = bishfpm_create(&c), *b = bishfpm_create_pairs(&c, &o);
  assert(a && b);
  char names[12][24];
  for (unsigned i = 0; i < 12; i++)
    snprintf(names[i], 24, "item%u", i);
  uint64_t rng = 42;
  for (unsigned step = 0; step < 1000; step++) {
    const char *items[5];
    size_t n = 0;
    uint64_t bits = bishfpm_splitmix64(&rng);
    for (unsigned i = 0; i < 12 && n < 5; i++)
      if (bits & (1U << i))
        items[n++] = names[i];
    uint64_t now = step > 10 && bits & 1 ? step - 10 : step;
    notifications x = {0}, y = {0};
    int p = bishfpm_add_event_weighted(a, "g", now, items, n, 1 + bits % 3,
                                       collect, &x);
    int q = bishfpm_add_event_weighted(b, "g", now, items, n, 1 + bits % 3,
                                       collect, &y);
    assert(p == q && x.n == y.n);
    for (size_t i = 0; i < x.n; i++) {
      assert(!strcmp(x.t[i].pattern, y.t[i].pattern));
      near(x.t[i].pattern_rate, y.t[i].pattern_rate);
      near(x.t[i].pattern_upper_bound, y.t[i].pattern_upper_bound);
    }
    if (step % 43 == 0) {
      bishfpm_prune(a, step, 2);
      bishfpm_prune(b, step, 2);
    }
    if (step % 37 == 0) {
      for (unsigned i = 0; i < 12; i++)
        for (unsigned j = i + 1; j < 12; j++) {
          char key[BISHFPM_MAX_PATTERN_LEN];
          pattern(names[i], names[j], key);
          near(bishfpm_pattern_rate(a, "g", key, step),
               bishfpm_pattern_rate(b, "g", key, step));
        }
      bishfpm_stats_t sa, sb;
      bishfpm_get_stats(a, &sa);
      bishfpm_get_stats(b, &sb);
      assert(sa.pattern_replacements == sb.pattern_replacements);
      assert(sa.candidates_evaluated == sb.candidates_evaluated);
      assert(sb.memory_current_bytes <= sb.memory_bound_bytes);
    }
  }
  bishfpm_free(a);
  bishfpm_free(b);
}
static void intervals(void) {
  enum { N = 20 };
  char names[N][24];
  for (unsigned i = 0; i < N; i++)
    snprintf(names[i], 24, "v%u", i);
  double truth[N][N] = {{0}};
  uint64_t watermark = 0, rng = 3;
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.min_itemset_order = c.max_itemset_order = 2;
  c.item_rate_threshold = 0;
  c.max_groups = 1;
  c.max_items_per_group = N;
  c.ewma_half_life_ms = 20;
  c.min_pattern_rate = 1e100;
  bishfpm_pair_options_t o = {BISHFPM_PAIR_THRESHOLD, 100, 0, .5};
  bishfpm_t *e = bishfpm_create_pairs(&c, &o);
  assert(e);
  for (unsigned step = 0; step < 1000; step++) {
    uint64_t bits = bishfpm_splitmix64(&rng),
             now = step > 5 && bits & 1 ? step - 5 : step;
    const char *items[5];
    unsigned ids[5];
    size_t n = 0;
    for (unsigned i = 0; i < N && n < 5; i++)
      if (bits & (1U << i)) {
        items[n] = names[i];
        ids[n++] = i;
      }
    int result = bishfpm_add_event_weighted(e, "g", now, items, n, 1 + bits % 3,
                                            NULL, NULL);
    if (result >= 0 && n) {
      uint64_t effective = now > watermark ? now : watermark;
      for (unsigned i = 0; i < N; i++)
        for (unsigned j = i + 1; j < N; j++)
          truth[i][j] *= exp2(-(double)(effective - watermark) / 20);
      double weight = (1 + bits % 3) * exp2(-(double)(effective - now) / 20);
      watermark = effective;
      for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
          truth[ids[i]][ids[j]] += weight;
    }
    if (step % 23 == 0) {
      bishfpm_prune(e, watermark, .5);
      for (unsigned i = 0; i < N; i++)
        for (unsigned j = i + 1; j < N; j++) {
          char key[BISHFPM_MAX_PATTERN_LEN];
          pattern(names[i], names[j], key);
          bishfpm_count_bounds_t b;
          assert(bishfpm_pattern_bounds(e, "g", key, watermark, &b) && b.known);
          assert(b.lower <= truth[i][j] + 1e-7 &&
                 b.upper + 1e-7 >= truth[i][j]);
          assert(b.upper - b.lower <= .5 + 1e-7);
        }
    }
  }
  bishfpm_pair_stats_t stats;
  bishfpm_get_pair_stats(e, &stats);
  assert(stats.patterns_forgotten >
         0); /* exercise actual forgetting/readmission */
  bishfpm_free(e);
}
static void saturation(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.min_itemset_order = c.max_itemset_order = 2;
  c.item_rate_threshold = 0;
  c.max_groups = 1;
  c.max_items_per_group = 8;
  c.ewma_half_life_ms = 1000;
  bishfpm_pair_options_t o = {BISHFPM_PAIR_THRESHOLD, 2, 0, 1};
  bishfpm_t *e = bishfpm_create_pairs(&c, &o);
  assert(e);
  const char *ab[] = {"a", "b"}, *cd[] = {"c", "d"}, *ef[] = {"e", "f"};
  assert(bishfpm_add_event_weighted(e, "g", 0, ab, 2, 10, NULL, NULL) >= 0);
  assert(bishfpm_add_event_weighted(e, "g", 0, cd, 2, 10, NULL, NULL) >= 0);
  assert(bishfpm_add_event(e, "g", 1, ef, 2, NULL, NULL) == -1);
  near(bishfpm_group_rate(e, "g", 0), 20);
  near(bishfpm_item_rate(e, "g", "e", 0), 0);
  bishfpm_pair_stats_t stats;
  bishfpm_get_pair_stats(e, &stats);
  assert(stats.capacity_rejections == 1);
  assert(bishfpm_add_event(e, "g", 10000, ef, 2, NULL, NULL) >= 0);
  char key[BISHFPM_MAX_PATTERN_LEN];
  pattern("a", "b", key);
  bishfpm_count_bounds_t b;
  assert(bishfpm_pattern_bounds(e, "g", key, 10000, &b));
  assert(!b.resident && b.known);
  near(b.upper, 10.0 / 1024);
  /* Dictionary IDs survive marginal pruning and reappearance. */
  bishfpm_prune(e, 10000, .1);
  assert(bishfpm_add_event(e, "g", 10000, ab, 2, NULL, NULL) >= 0);
  assert(bishfpm_pattern_bounds(e, "g", key, 10000, &b));
  assert(b.lower <= 1 + 10.0 / 1024 && b.upper >= 1 + 10.0 / 1024 - 1e-9);
  bishfpm_free(e);
  c.item_rate_threshold = 5;
  assert(!bishfpm_create_pairs(&c, &o));
  c.item_rate_threshold = 0;
  c.max_itemset_order = 3;
  assert(!bishfpm_create_pairs(&c, &o));
}
static void bulk_forgetting(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.min_itemset_order = c.max_itemset_order = 2;
  c.item_rate_threshold = 0;
  c.min_pattern_rate = 1e100;
  c.max_groups = 1;
  c.max_items_per_group = 80;
  c.max_event_items = 80;
  c.max_candidates_per_event = 10000;
  bishfpm_pair_options_t o = {BISHFPM_PAIR_THRESHOLD, 4000, 0, 1};
  bishfpm_t *e = bishfpm_create_pairs(&c, &o);
  assert(e);
  char names[80][16];
  const char *items[80];
  for (unsigned i = 0; i < 80; i++) {
    snprintf(names[i], 16, "bulk%03u", i);
    items[i] = names[i];
  }
  assert(bishfpm_add_event(e, "g", 0, items, 80, NULL, NULL) >= 0);
  bishfpm_prune(e, 0, 1);
  bishfpm_pair_stats_t stats;
  bishfpm_get_pair_stats(e, &stats);
  assert(stats.patterns_forgotten == 3160 && stats.resident_pairs == 0);
  assert(bishfpm_add_event_weighted(e, "g", 0, items, 80, 2, NULL, NULL) >= 0);
  for (unsigned i = 0; i < 80; i++)
    for (unsigned j = i + 1; j < 80; j++) {
      char key[BISHFPM_MAX_PATTERN_LEN];
      pattern(names[i], names[j], key);
      bishfpm_count_bounds_t b;
      assert(bishfpm_pattern_bounds(e, "g", key, 0, &b));
      near(b.upper, 3);
      near(b.lower, 2);
      assert(b.known && b.resident);
    }
  bishfpm_free(e);
}
typedef struct {
  double truth;
  int64_t first;
} delay_state;
static void on_detection(const char *group, const bishfpm_trigger_t *t,
                         void *data) {
  (void)group;
  delay_state *d = data;
  if (!strcmp(t->pattern, "a\x1f"
                          "b")) {
    assert(t->pattern_rate <= d->truth + 1e-7);
    assert(d->truth >= 10 - 1e-7);
    if (d->first < 0)
      d->first = (int64_t)t->timestamp;
  }
}
static void delivered_detection(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.min_itemset_order = c.max_itemset_order = 2;
  c.item_rate_threshold = 0;
  c.ewma_half_life_ms = 100;
  c.min_pattern_rate = 10;
  c.promotion_fraction = 0;
  c.max_groups = 1;
  c.max_items_per_group = 256;
  bishfpm_pair_options_t o = {BISHFPM_PAIR_THRESHOLD, 256, 0, .25};
  bishfpm_t *e = bishfpm_create_pairs(&c, &o);
  assert(e);
  delay_state state = {0, -1};
  int64_t crossing = -1;
  for (unsigned i = 0; i < 1000; i++) {
    state.truth *= exp2(-1.0 / 100);
    int signal = i == 0 || (i >= 500 && i % 5 == 0);
    char a[24], b[24];
    snprintf(a, 24, "noise%02u", i % 64);
    snprintf(b, 24, "other%02u", i % 64);
    const char *items[] = {signal ? "a" : a, signal ? "b" : b};
    if (signal)
      state.truth += 1;
    if (crossing < 0 && state.truth >= 10)
      crossing = i;
    assert(bishfpm_add_event(e, "g", i, items, 2, on_detection, &state) >= 0);
    if (i % 50 == 0)
      bishfpm_prune(e, i, .25);
  }
  assert(crossing >= 500 && state.first >= crossing &&
         state.first - crossing <= 5);
  bishfpm_free(e);
}
static void partial_bulk_forgetting(void) {
  enum { N = 128 };
  char names[N][16];
  const char *items[N];
  for (unsigned i = 0; i < N; i++) {
    snprintf(names[i], sizeof(names[i]), "partial%03u", i);
    items[i] = names[i];
  }
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.min_itemset_order = c.max_itemset_order = 2;
  c.item_rate_threshold = 0;
  c.min_pattern_rate = 1e100;
  c.max_groups = 1;
  c.max_items_per_group = N;
  bishfpm_pair_options_t o = {BISHFPM_PAIR_THRESHOLD, 9000, 0, .25};
  bishfpm_t *e = bishfpm_create_pairs(&c, &o);
  assert(e);
  assert(bishfpm_add_event(e, "g", 0, items, N, NULL, NULL) >= 0);
  size_t strong = 0;
  for (unsigned i = 0; i < N; i++)
    for (unsigned j = i + 1; j < N; j++)
      if ((i + j) % 5 == 0) {
        const char *pair[] = {items[i], items[j]};
        strong++;
        assert(bishfpm_add_event_weighted(e, "g", 0, pair, 2, 16, NULL, NULL) >=
               0);
      }
  bishfpm_prune(e, 15000, .25);
  bishfpm_pair_stats_t ps;
  bishfpm_get_pair_stats(e, &ps);
  assert(ps.resident_pairs == strong && ps.patterns_forgotten > 1024);
  for (unsigned pass = 0; pass < 2; pass++) {
    for (unsigned i = 0; i < N; i++)
      for (unsigned j = i + 1; j < N; j++) {
        char key[BISHFPM_MAX_PATTERN_LEN];
        pattern(items[i], items[j], key);
        bishfpm_count_bounds_t b;
        assert(bishfpm_pattern_bounds(e, "g", key, 15000, &b));
        int retained = (i + j) % 5 == 0;
        assert(b.known && b.resident == (retained || pass));
        near(b.lower, retained ? 17.0 / 8 + 2 * pass : 2 * pass);
        near(b.upper, retained ? 17.0 / 8 + 2 * pass : 1.0 / 8 + 2 * pass);
      }
    if (!pass)
      assert(bishfpm_add_event_weighted(e, "g", 15000, items, N, 2, NULL,
                                        NULL) >= 0);
  }
  bishfpm_free(e);
}
/* Largest observed Kosarak basket: exercise both former hard ceilings,
 * exact pair enumeration, and failure before committing dictionary/counts. */
static void wide_basket(void) {
  enum { N = 2497 };
  const uint32_t pairs = N * (N - 1U) / 2;
  char names[N][16];
  const char *items[N];
  for (unsigned i = 0; i < N; i++) {
    snprintf(names[i], sizeof(names[i]), "wide%04u", i);
    items[i] = names[i];
  }
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.min_itemset_order = c.max_itemset_order = 2;
  c.item_rate_threshold = 0;
  c.min_pattern_rate = 1e100;
  c.max_groups = 1;
  c.max_items_per_group = N;
  c.topk_k = 64;
  c.max_event_items = BISHFPM_MAX_EVENT_ITEMS;
  c.max_candidates_per_event = pairs - 1;
  bishfpm_pair_options_t o = {BISHFPM_PAIR_THRESHOLD, pairs, 0, .25};
  bishfpm_t *e = bishfpm_create_pairs(&c, &o);
  assert(e);
  assert(bishfpm_add_event(e, "g", 10000, items, N, NULL, NULL) == -1);
  near(bishfpm_group_rate(e, "g", 0), 0);
  bishfpm_stats_t rejected;
  bishfpm_get_stats(e, &rejected);
  assert(rejected.budget_rejections == 1);
  bishfpm_free(e);
  c.max_candidates_per_event = pairs;
  o.capacity = 1;
  e = bishfpm_create_pairs(&c, &o);
  assert(e);
  assert(bishfpm_add_event(e, "g", 10000, items, N, NULL, NULL) == -1);
  bishfpm_pair_stats_t ps;
  bishfpm_get_pair_stats(e, &ps);
  assert(ps.capacity_rejections == 1 && ps.resident_pairs == 0);
  near(bishfpm_group_rate(e, "g", 0), 0);
  near(bishfpm_item_rate(e, "g", items[N - 1], 0), 0);
  bishfpm_free(e);
  o.capacity = pairs;
  e = bishfpm_create_pairs(&c, &o);
  assert(e);
  assert(bishfpm_add_event(e, "g", 0, items, N, NULL, NULL) >= 0);
  bishfpm_stats_t stats;
  bishfpm_get_stats(e, &stats);
  bishfpm_get_pair_stats(e, &ps);
  assert(stats.candidates_evaluated == pairs && ps.resident_pairs == pairs);
  assert(stats.memory_current_bytes <= stats.memory_bound_bytes &&
         stats.memory_bound_bytes <= c.max_memory_bytes);
  for (unsigned i = 0; i < N; i += 113) {
    char key[BISHFPM_MAX_PATTERN_LEN];
    pattern(items[i], items[N - 1], key);
    bishfpm_count_bounds_t b;
    assert(bishfpm_pattern_bounds(e, "g", key, 0, &b));
    assert(b.known && b.resident);
    near(b.lower, 1);
    near(b.upper, 1);
  }
  bishfpm_free(e);
  c.max_event_items = BISHFPM_MAX_EVENT_ITEMS + 1;
  assert(!bishfpm_create_pairs(&c, &o));
  c.max_event_items = BISHFPM_MAX_EVENT_ITEMS;
  c.max_candidates_per_event = BISHFPM_MAX_EVENT_CANDIDATES + 1;
  assert(!bishfpm_create_pairs(&c, &o));
}

/* Adversarial widths at bounded capacity: SS/FSS accept and account for every
 * pair, while threshold rejects the entire event when it cannot retain it. */
static void bounded_wide_baskets(void) {
  enum { N = 4097 };
  char names[N][16];
  const char *items[N];
  for (unsigned i = 0; i < N; i++) {
    snprintf(names[i], sizeof(names[i]), "wide%04u", i);
    items[i] = names[i];
  }
  const unsigned widths[] = {32, 256, 1024, 2497, 4096};
  for (int kind = BISHFPM_PAIR_SS; kind <= BISHFPM_PAIR_FSS; kind++) {
    for (size_t w = 0; w < sizeof(widths) / sizeof(*widths); w++) {
      unsigned n = widths[w];
      uint64_t pairs = (uint64_t)n * (n - 1) / 2;
      bishfpm_config_t c;
      bishfpm_config_default(&c);
      c.min_itemset_order = c.max_itemset_order = 2;
      c.item_rate_threshold = c.item_share_threshold = 0;
      c.min_pattern_rate = 1e100;
      c.max_groups = 1;
      c.max_items_per_group = N - 1;
      c.topk_k = 64;
      c.max_event_items = BISHFPM_MAX_EVENT_ITEMS;
      c.max_candidates_per_event = BISHFPM_MAX_EVENT_CANDIDATES;
      c.max_memory_bytes = 16U << 20;
      bishfpm_pair_options_t o = {(bishfpm_pair_store_t)kind, 1024,
                                  kind == BISHFPM_PAIR_FSS ? 17U : 0U, .25};
      bishfpm_t *e = bishfpm_create_pairs(&c, &o);
      assert(e);
      int reject = kind == BISHFPM_PAIR_THRESHOLD && pairs > o.capacity;
      int rc = bishfpm_add_event(e, "g", 1, items, n, NULL, NULL);
      assert(reject ? rc == -1 : rc >= 0);
      near(bishfpm_group_rate(e, "g", 0), reject ? 0 : 1);
      bishfpm_stats_t s;
      bishfpm_get_stats(e, &s);
      assert(s.candidates_evaluated == (reject ? 0 : pairs));
      assert(s.events_accepted == (uint64_t)!reject &&
             s.events_rejected == (uint64_t)reject);
      assert(s.memory_current_bytes <= s.memory_bound_bytes &&
             s.memory_bound_bytes <= c.max_memory_bytes);
      bishfpm_pair_stats_t p;
      bishfpm_get_pair_stats(e, &p);
      assert(p.resident_pairs <= o.capacity &&
             p.capacity_rejections == (uint64_t)reject);
      if (!reject) {
        for (unsigned a = 0; a < n; a += n / 17 + 1) {
          unsigned b = (a + n / 2) % n;
          char key[BISHFPM_MAX_PATTERN_LEN];
          pattern(items[a], items[b], key);
          bishfpm_count_bounds_t bound;
          assert(bishfpm_pattern_bounds(e, "g", key, 1, &bound));
          if (bound.known) {
            assert(isfinite(bound.lower) && isfinite(bound.upper));
            assert(bound.lower <= 1 + 1e-9 && bound.upper + 1e-9 >= 1);
          }
        }
      }
      /* Rejected future input cannot advance decay or partly enumerate pairs.
       */
      assert(bishfpm_add_event(e, "g", 1000000, items, N, NULL, NULL) == -1);
      near(bishfpm_group_rate(e, "g", 0), reject ? 0 : 1);
      bishfpm_get_stats(e, &s);
      assert(s.candidates_evaluated == (reject ? 0 : pairs));
      assert(s.events_rejected == 1 + (uint64_t)reject &&
             s.budget_rejections == 1 + (uint64_t)reject);
      bishfpm_free(e);
    }
  }
}
/* Force numeric slot collisions, with a
 * probe chain wrapping through slot zero. Derived numeric full hashes are
 * bijective. Exercise deletion while entries move through the heap, and
 * reinsertion. */
static void colliding_indices(void) {
  for (int mode = BISHFPM_PAIR_SS; mode <= BISHFPM_PAIR_THRESHOLD; mode++) {
    compact_t *t = compact_new(16, 1000, 0, mode, .25);
    assert(t && sizeof(*t->slot_map) == 4 && sizeof(*t->heap) == 40);
    uint64_t hash = t->slot_map_cap - 1;
    uint64_t keys[100] = {0}, next = 1;
    for (unsigned i = 1; i < 100; i++) {
      while ((compact_hash(next) & (t->slot_map_cap - 1)) != hash)
        next++;
      keys[i] = next++;
    }
    for (uint64_t i = 1; i <= 16; i++)
      assert(compact_add(t, keys[i], hash, i, 0).present);
    for (uint64_t i = 1; i <= 16; i++) {
      topk_result_t r = compact_query(t, keys[i], hash, 0);
      assert(r.present);
      near(r.lower, i);
      assert(compact_add(t, keys[i], hash, 16 - i, 0).present);
    }
    assert(!compact_query(t, keys[99], hash, 0).present);
    if (mode != BISHFPM_PAIR_THRESHOLD) {
      assert(compact_add(t, keys[99], hash, 100, 0).present);
      assert(compact_query(t, keys[99], hash, 0).present);
    }
    assert(compact_prune(t, 20000, .25) == 16);
    for (uint64_t i = 1; i <= 16; i++) {
      assert(!compact_query(t, keys[i], hash, 20000).present);
      assert(compact_add(t, keys[i], hash, i, 20000).present);
    }
    for (uint64_t i = 1; i <= 16; i++) {
      topk_result_t r = compact_query(t, keys[i], hash, 20000);
      assert(r.present);
      near(r.lower, i);
    }
    compact_free(t);
  }
}
typedef struct {
  bishfpm_t *engine;
  unsigned calls;
} bounded_callback_state;
static void bounded_callback(const char *group,
                             const bishfpm_trigger_t *trigger, void *data) {
  (void)trigger;
  bounded_callback_state *state = data;
  state->calls++;
  assert(bishfpm_prune_pairs(state->engine, group, UINT64_MAX, 1) == 0);
}
static void bounded_forgetting(void) {
  compact_t *t = compact_new(32, 1000, 0, BISHFPM_PAIR_THRESHOLD, .25);
  assert(t);
  uint64_t hash = t->slot_map_cap - 1;
  for (uint64_t i = 1; i <= 32; i++)
    assert(compact_add(t, i, hash, i % 4 == 0 ? 16 : 1, 0).present);
  assert(compact_prune_bounded(t, 3000, 0) == 0);
  assert(t->max_time_ms == 0);
  size_t total = 0;
  for (unsigned call = 0; call < 8; call++) {
    assert(compact_prune_bounded(t, 3000, 3) == 3);
    total += 3;
    assert(t->heap_size == 32 - total && t->forgotten == total);
    for (uint64_t i = 1; i <= 32; i++) {
      topk_result_t r = compact_query(t, i, hash, 3000);
      double truth = i % 4 == 0 ? 2 : .125;
      assert(r.lower <= truth + 1e-12 && r.upper + 1e-12 >= truth);
      if (i % 4 == 0) {
        assert(r.present);
        near(r.lower, 2);
      }
    }
  }
  assert(compact_prune_bounded(t, 2000, 99) == 0);
  assert(t->max_time_ms == 3000 && t->heap_size == 8);
  for (uint64_t i = 1; i <= 32; i++) {
    topk_result_t r = compact_add(t, i, hash, 1, 3000);
    assert(r.present);
    near(r.lower, i % 4 == 0 ? 3 : 1);
    near(r.upper, i % 4 == 0 ? 3 : 1.125);
  }
  compact_free(t);

  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.min_itemset_order = c.max_itemset_order = 2;
  c.item_rate_threshold = c.item_share_threshold = 0;
  c.ewma_half_life_ms = 1000;
  c.min_pattern_rate = 1e100;
  c.max_groups = 2;
  c.max_items_per_group = 16;
  c.topk_k = 16;
  bishfpm_pair_options_t o = {BISHFPM_PAIR_THRESHOLD, 16, 0, .25};
  bishfpm_t *e = bishfpm_create_pairs(&c, &o);
  assert(e);
  const char *items[] = {"a=1", "b=1", "c=1", "d=1"};
  assert(bishfpm_add_event(e, "a", 0, items, 4, NULL, NULL) >= 0);
  assert(bishfpm_add_event(e, "b", 0, items, 4, NULL, NULL) >= 0);
  assert(bishfpm_prune_pairs(e, "a", 3000, 0) == 0);
  near(bishfpm_group_rate(e, "a", 0), 1);
  assert(bishfpm_prune_pairs(e, "missing", 3000, 2) == 0);
  assert(bishfpm_prune_pairs(NULL, "a", 3000, 2) == 0);
  assert(bishfpm_prune_pairs(e, NULL, 3000, 2) == 0);
  assert(bishfpm_prune_pairs(e, "a", 3000, 2) == 2);
  near(bishfpm_group_rate(e, "a", 0), .125);
  near(bishfpm_group_rate(e, "b", 0), 1);
  assert(bishfpm_prune_pairs(e, "a", 2000, 100) == 4);
  assert(bishfpm_add_event(e, "a", 2000, items, 4, NULL, NULL) >= 0);
  near(bishfpm_group_rate(e, "a", 0), .625);
  char p[BISHFPM_MAX_PATTERN_LEN];
  pattern(items[0], items[1], p);
  bishfpm_count_bounds_t bounds;
  assert(bishfpm_pattern_bounds(e, "a", p, 3000, &bounds));
  near(bounds.lower, .5);
  near(bounds.upper, .625);
  bishfpm_pair_stats_t ps;
  bishfpm_get_pair_stats(e, &ps);
  assert(ps.patterns_forgotten == 6 && ps.resident_pairs == 12);
  bishfpm_free(e);
  c.min_pattern_rate = .1;
  c.min_lift = c.promotion_fraction = 0;
  e = bishfpm_create_pairs(&c, &o);
  assert(e);
  bounded_callback_state state = {e, 0};
  assert(bishfpm_add_event(e, "a", 0, items, 2, bounded_callback, &state) > 0);
  assert(state.calls == 1);
  near(bishfpm_group_rate(e, "a", 0), 1);
  assert(bishfpm_pattern_bounds(e, "a", p, 0, &bounds));
  near(bounds.lower, 1);
  bishfpm_free(e);
  const bishfpm_pair_store_t modes[] = {BISHFPM_PAIR_SS, BISHFPM_PAIR_FSS};
  for (unsigned i = 0; i < sizeof(modes) / sizeof(*modes); i++) {
    o.store = modes[i];
    o.sketch_width = o.store == BISHFPM_PAIR_FSS ? 16 : 0;
    e = bishfpm_create_pairs(&c, &o);
    assert(e);
    assert(bishfpm_add_event(e, "a", 0, items, 4, NULL, NULL) >= 0);
    assert(bishfpm_prune_pairs(e, "a", 2000, 100) == 0); /* strict cutoff */
    assert(bishfpm_prune_pairs(e, "a", 3000, 2) == 2);
    assert(bishfpm_prune_pairs(e, "a", 3000, 100) == 4);
    near(bishfpm_group_rate(e, "a", 0), .125);
    assert(bishfpm_pattern_bounds(e, "a", p, 3000, &bounds));
    assert(!bounds.resident);
    near(bounds.upper, o.store == BISHFPM_PAIR_FSS ? .125 : 0);
    assert(bishfpm_add_event(e, "a", 3000, items, 4, NULL, NULL) >= 0);
    assert(bishfpm_pattern_bounds(e, "a", p, 3000, &bounds));
    near(bounds.lower, 1);
    near(bounds.upper, 1.125);
    bishfpm_free(e);
  }
}
/* A valid interval can straddle a support threshold after weak history was
 * forgotten. Keeping that history must recover the same pair conservatively. */
static void near_threshold_history(void) {
  const bishfpm_pair_store_t modes[] = {BISHFPM_PAIR_SS, BISHFPM_PAIR_THRESHOLD,
                                        BISHFPM_PAIR_FSS};
  const char *items[] = {"a", "b"};
  char key[BISHFPM_MAX_PATTERN_LEN];
  pattern(items[0], items[1], key);
  for (unsigned mode = 0; mode < sizeof(modes) / sizeof(*modes); mode++)
    for (unsigned forget = 0; forget < 2; forget++) {
      bishfpm_config_t c;
      bishfpm_config_default(&c);
      c.min_itemset_order = c.max_itemset_order = 2;
      c.max_groups = 1;
      c.max_items_per_group = 8;
      c.item_rate_threshold = c.item_share_threshold = 0;
      c.min_pattern_rate = 1e100;
      c.ewma_half_life_ms = 1000000;
      bishfpm_pair_options_t o = {modes[mode], 16,
                                  modes[mode] == BISHFPM_PAIR_FSS ? 64 : 0,
                                  forget ? .01 : 0};
      bishfpm_t *e = bishfpm_create_pairs(&c, &o);
      assert(e);
      assert(bishfpm_add_event_weighted(e, "g", 0, items, 2, 1, NULL, NULL) >=
             0);
      assert(bishfpm_prune_pairs(e, "g", 8000000, 16) == forget);
      assert(bishfpm_add_event_weighted(e, "g", 7999900, items, 2, 25, NULL,
                                        NULL) >= 0);
      double recent = 25 * exp2(-.0001), truth = exp2(-8) + recent;
      bishfpm_count_bounds_t bounds;
      assert(bishfpm_pattern_bounds(e, "g", key, 8000000, &bounds));
      assert(bounds.known && bounds.resident);
      assert(bounds.lower <= truth + 1e-10 && bounds.upper >= truth - 1e-10);
      if (forget) {
        assert(bounds.lower < 25 && bounds.upper > 25 && truth > 25);
        near(bounds.lower, recent);
      } else {
        assert(bounds.lower > 25);
        near(bounds.lower, truth);
        near(bounds.upper, truth);
      }
      bishfpm_free(e);
    }
}

int main(void) {
  near_threshold_history();
  colliding_indices();
  bounded_forgetting();
  equivalence(0);
  equivalence(1);
  intervals();
  saturation();
  bulk_forgetting();
  delivered_detection();
  wide_basket();
  bounded_wide_baskets();
  partial_bulk_forgetting();
  puts("test_online passed.");
}
