#include "bishfpm_hash.h"
#include "bishfpm_itemset.h"
#include "bishfpm_uss.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
  size_t bytes, peak;
  long fail, calls;
} ledger_t;
static void *allocate(void *raw, size_t n) {
  ledger_t *l = raw;
  if (l->fail >= 0 && l->calls++ == l->fail)
    return NULL;
  void *p = malloc(n);
  if (p) {
    l->bytes += n;
    if (l->bytes > l->peak)
      l->peak = l->bytes;
  }
  return p;
}
static void *zeroed(void *raw, size_t n, size_t size) {
  if (size && n > SIZE_MAX / size)
    return NULL;
  void *p = allocate(raw, n * size);
  if (p)
    memset(p, 0, n * size);
  return p;
}
static void release(void *raw, void *p, size_t n) {
  ledger_t *l = raw;
  if (p) {
    assert(l->bytes >= n);
    l->bytes -= n;
    free(p);
  }
}
static const bishfpm_store_descriptor_t *pair = &bishfpm_uss_pair_store,
                                        *text = &bishfpm_uss_store;
static void same(bishfpm_store_count_t a, bishfpm_store_count_t b) {
  assert(a.lower == b.lower && a.upper == b.upper && a.resident == b.resident &&
         a.known == b.known && a.outcome == b.outcome && !a.reserved &&
         !b.reserved);
}
static void differential(void) {
  const unsigned capacities[] = {1, 2, 7, 37};
  for (unsigned test = 0; test < 12; test++) {
    bishfpm_config_t c;
    bishfpm_config_default(&c);
    c.min_itemset_order = c.max_itemset_order = 2;
    c.ewma_half_life_ms = 1000;
    bishfpm_uss_options_t o = {capacities[test % 4], 7 + test / 4};
    ledger_t l = {0, 0, -1, 0};
    bishfpm_store_host_t h = {&l, allocate, zeroed, release};
    void *pc, *ps, *tc, *ts;
    bishfpm_store_plan_t pp, tp;
    assert(pair->plan(&c, &o, sizeof(o), &pp));
    assert(text->plan(&c, &o, sizeof(o), &tp));
    assert(pp.group_peak_bytes < tp.group_peak_bytes && pp.min_order == 2 &&
           pp.max_order == 2);
    assert(pair->clone_config(&h, &c, &o, sizeof(o), &pc) &&
           pair->create(&h, pc, &ps));
    assert(text->clone_config(&h, &c, &o, sizeof(o), &tc) &&
           text->create(&h, tc, &ts));
    uint64_t rng = 73, time = 0;
    for (unsigned step = 0; step < 9000; step++) {
      uint64_t bits = bishfpm_splitmix64(&rng);
      unsigned id = bits % 31;
      uint64_t key = id ? ((uint64_t)id << 32) | (id + 1) : 0;
      char name[32];
      snprintf(name, sizeof(name), "key%u", id);
      time += bits % 17;
      if (step % 211 == 0) {
        time += 2200000;
        assert(pair->prune(ps, time, 1e9) == text->prune(ts, time, 1e9));
      }
      double weight = (bits & 7) ? (double)(1 + bits % 1001) / 71 : 0;
      same(pair->update_pair(ps, key, 0, weight, time),
           text->update_text(ts, name, strlen(name), 0, weight, time));
      for (unsigned i = 0; i < 31; i++) {
        uint64_t k = i ? ((uint64_t)i << 32) | (i + 1) : 0;
        snprintf(name, sizeof(name), "key%u", i);
        same(pair->query_pair(ps, k, UINT64_MAX, time + 1000),
             text->query_text(ts, name, strlen(name), 0, time + 1000));
        same(pair->query_pair(ps, k, UINT64_MAX, time),
             text->query_text(ts, name, strlen(name), 0, time));
      }
      if (step % 97 == 0)
        assert(pair->prune(ps, time, 1e9) == text->prune(ts, time, 1e9));
    }
    pair->destroy(&h, ps);
    text->destroy(&h, ts);
    pair->destroy_config(&h, pc);
    text->destroy_config(&h, tc);
    assert(!l.bytes);
  }
}
static void boundaries(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.min_itemset_order = c.max_itemset_order = 2;
  bishfpm_uss_options_t o = {97, 42};
  bishfpm_store_plan_t p;
  assert(pair->plan(&c, &o, sizeof(o), &p));
  /* 32-byte rows plus 256 32-bit slots and the fixed store header. */
  bishfpm_uss_options_t o2 = {98, 42};
  bishfpm_store_plan_t p2;
  assert(pair->plan(&c, &o2, sizeof(o2), &p2));
  assert(p2.group_peak_bytes - p.group_peak_bytes == 32);
  for (long fault = 0; fault < 5; fault++) {
    ledger_t l = {0, 0, fault, 0};
    bishfpm_store_host_t h = {&l, allocate, zeroed, release};
    void *cfg = NULL, *s = NULL;
    if (pair->clone_config(&h, &c, &o, sizeof(o), &cfg)) {
      if (pair->create(&h, cfg, &s)) {
        assert(l.peak == p.engine_peak_bytes + p.group_peak_bytes);
        assert(pair->update_pair(s, UINT64_MAX, 0, 1, 0).outcome ==
               BISHFPM_UPDATE_REJECTED);
        assert(pair->update_pair(s, 0, 123, DBL_MAX, 0).resident);
        assert(pair->update_pair(s, 0, 123, DBL_MAX, 1).outcome ==
               BISHFPM_UPDATE_REJECTED);
        assert(pair->update_pair(s, 0, 123, NAN, 0).outcome ==
               BISHFPM_UPDATE_REJECTED);
        assert(pair->query_pair(s, 0, 0, 0).lower == DBL_MAX);
        assert(!pair->query_pair(s, UINT64_MAX, 0, 0).resident);
        assert(pair->update_pair(s, 0, 0, 0, UINT64_MAX).outcome ==
               BISHFPM_UPDATE_ZERO);
        assert(pair->query_pair(s, 0, 0, 0).lower == DBL_MAX);
        pair->destroy(&h, s);
      }
      pair->destroy_config(&h, cfg);
    }
    assert(!l.bytes);
  }
  c.max_itemset_order = 3;
  assert(!pair->plan(&c, &o, sizeof(o), &p));
}
static void collisions(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.min_itemset_order = c.max_itemset_order = 2;
  c.ewma_half_life_ms = 1000;
  bishfpm_uss_options_t o = {512, 42};
  ledger_t l = {0, 0, -1, 0};
  bishfpm_store_host_t h = {&l, allocate, zeroed, release};
  void *cfg, *s;
  assert(pair->clone_config(&h, &c, &o, sizeof(o), &cfg) &&
         pair->create(&h, cfg, &s));
  uint64_t keys[512], next = 0;
  for (unsigned i = 0; i < 512; i++) {
    for (;; next++) {
      uint64_t k = next;
      if ((bishfpm_splitmix64(&k) & 1023) == 1023)
        break;
    }
    keys[i] = next++;
    assert(pair->update_pair(s, keys[i], 0, i % 2 ? 0x1p1000 : 1, 0).resident);
  }
  assert(pair->prune(s, 1100000, 1e9) == 256);
  for (unsigned i = 0; i < 512; i++)
    assert(pair->query_pair(s, keys[i], 0, 1100000).resident == (i % 2));
  for (unsigned i = 0; i < 512; i += 2)
    assert(pair->update_pair(s, keys[i], 0, 1, 1100000).resident);
  assert(pair->prune(s, 2200000, 1e9) == 512);
  pair->destroy(&h, s);
  pair->destroy_config(&h, cfg);
  assert(!l.bytes);
}
static void alert(const char *g, const bishfpm_trigger_t *t, void *raw) {
  unsigned *n = raw;
  (*n)++;
  assert(!strcmp(g, "g"));
  assert(t->pattern_rate >= 1 && isinf(t->pattern_error));
  char expected[BISHFPM_MAX_PATTERN_LEN];
  assert(bishfpm_itemset_format(t->pattern, expected, sizeof(expected)));
  assert(!strcmp(expected, t->formatted));
}
static void engines(void) {
  for (unsigned dynamic = 0; dynamic < 2; dynamic++) {
    bishfpm_config_t c;
    bishfpm_config_default(&c);
    c.min_itemset_order = c.max_itemset_order = 2;
    c.item_rate_threshold = c.item_share_threshold = c.min_lift =
        c.promotion_fraction = 0;
    c.min_pattern_rate = 1;
    c.trigger_cooldown_ms = 0;
    c.max_groups = 1;
    c.max_items_per_group = 64;
    c.max_event_items = 8;
    bishfpm_uss_options_t cfg = {64, 42};
    bishfpm_store_engine_options_t o;
    bishfpm_store_engine_options_default(&o);
    o.memory_mode =
        dynamic ? BISHFPM_MEMORY_DYNAMIC : BISHFPM_MEMORY_CONSERVATIVE;
    bishfpm_t *e = bishfpm_create_with_store(&c, &o, pair, &cfg, sizeof(cfg));
    assert(e);
    const char *items[] = {"c", "a", "b"};
    unsigned calls = 0;
    assert(bishfpm_add_event(e, "g", 0, items, 3, alert, &calls) == 3 &&
           calls == 3);
    bishfpm_count_bounds_t b;
    assert(bishfpm_pattern_bounds(e, "g", "a\037b", 0, &b));
    assert(!b.known && b.resident && b.lower == 0 && isinf(b.upper));
    assert(bishfpm_pattern_estimate(e, "g", "a\037b", 0) == 1);
    assert(bishfpm_pattern_estimate(e, "g", "missing\037x", 0) == 0);
    bishfpm_store_snapshot_t stats;
    assert(bishfpm_get_store_snapshot(e, "g", sizeof(stats), &stats));
    assert(stats.resident_patterns == 3);
    bishfpm_memory_stats_t m;
    bishfpm_get_memory_stats(e, &m);
    assert(m.current_bytes <= m.limit_bytes);
    assert(bishfpm_prune(e, UINT64_MAX, 1) == 1);
    assert(bishfpm_add_event(e, "g", 0, items, 3, alert, &calls) == 3);
    bishfpm_free(e);
  }
}
int main(void) {
  differential();
  boundaries();
  collisions();
  engines();
  puts("test_uss_pair passed.");
}
