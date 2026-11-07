/* shared test harness for pattern store plugins */
#ifndef BISHFPM_PLUGIN_TEST_H
#define BISHFPM_PLUGIN_TEST_H
#include "bishfpm_hash.h"
#include "bishfpm_rate.h"
#include "bishfpm_store_plugin.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NO_MALLOC_WRAP
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
char *__real_strdup(const char *);
static long fail_at = -1, calls;
static int deny(void) { return fail_at >= 0 && calls++ == fail_at; }
void *__wrap_malloc(size_t n) { return deny() ? NULL : __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t s) {
  return deny() ? NULL : __real_calloc(n, s);
}
char *__wrap_strdup(const char *s) { return deny() ? NULL : __real_strdup(s); }
#endif

typedef struct {
  size_t bytes, peak;
  long fail, allocs;
} ledger_t;
static inline void *allocate(void *ctx, size_t n) {
  ledger_t *l = ctx;
  if (l->fail >= 0 && l->allocs++ == l->fail)
    return NULL;
  void *p = malloc(n);
  if (p) {
    l->bytes += n;
    if (l->bytes > l->peak)
      l->peak = l->bytes;
  }
  return p;
}
static inline void *zeroed(void *ctx, size_t n, size_t size) {
  assert(!n || size <= SIZE_MAX / n);
  void *p = allocate(ctx, n * size);
  if (p)
    memset(p, 0, n * size);
  return p;
}
static inline void release(void *ctx, void *p, size_t n) {
  ledger_t *l = ctx;
  if (p) {
    assert(l->bytes >= n);
    l->bytes -= n;
    free(p);
  }
}
static inline bishfpm_config_t mining(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.min_itemset_order = 2;
  c.max_itemset_order = 3;
  c.item_rate_threshold = c.item_share_threshold = c.promotion_fraction = 0;
  c.min_pattern_rate = 1;
  c.trigger_cooldown_ms = 0;
  c.ewma_half_life_ms = 1000;
  c.max_groups = 2;
  c.max_items_per_group = 8;
  c.max_event_items = 8;
  c.hash_seed = 42;
  return c;
}
static inline void close_to(double x, long double y) {
  assert(fabsl(x - y) <= 1e-10L * fmaxl(1, fabsl(y)));
}
static inline void heuristic_alert(const char *group,
                                   const bishfpm_trigger_t *t, void *ctx) {
  bishfpm_t *e = ctx;
  assert(!strcmp(group, "g"));
  assert(t->pattern_rate >= 1);
  assert(isinf(t->pattern_upper_bound) && isinf(t->pattern_error));
  bishfpm_count_bounds_t b;
  assert(bishfpm_pattern_bounds(e, group, t->pattern, t->timestamp, &b));
  assert(b.resident && !b.known && b.lower == 0 && isinf(b.upper));
  close_to(bishfpm_pattern_rate(e, group, t->pattern, t->timestamp),
           t->pattern_rate);
}
static inline void confirmed_alert(const char *group,
                                   const bishfpm_trigger_t *t, void *ctx) {
  (void)group;
  (*(unsigned *)ctx)++;
  close_to(t->pattern_rate, 3);
  assert(isinf(t->pattern_upper_bound) && isinf(t->pattern_error));
}
static inline bishfpm_t *
create_with_options(const bishfpm_store_descriptor_t *d,
                    const bishfpm_config_t *c, const void *s, size_t n,
                    int dynamic) {
  bishfpm_store_engine_options_t o;
  bishfpm_store_engine_options_default(&o);
  o.memory_mode =
      dynamic ? BISHFPM_MEMORY_DYNAMIC : BISHFPM_MEMORY_CONSERVATIVE;
  return bishfpm_create_with_store(c, &o, d, s, n);
}
#ifndef NO_MALLOC_WRAP
static const char *const items[] = {"a", "b", "c"};
static inline void check_heuristic_engines(const bishfpm_store_descriptor_t *d,
                                           void *s, size_t n, int dynamic,
                                           uint64_t diag0, uint64_t diag1) {
  bishfpm_config_t c = mining();
  uint32_t saved = *(uint32_t *)s;
  bishfpm_t *e = create_with_options(d, &c, s, n, dynamic);
  assert(e);
  *(uint32_t *)s = 0; /* config is owned after construction */
  assert(bishfpm_add_event_weighted(e, "g", 0, items, 3, 3, heuristic_alert,
                                    e) == 4);
  close_to(bishfpm_pattern_rate(e, "g", "a\037b", 1000), 1.5);
  close_to(bishfpm_pattern_rate(e, "g", "a\037b", 0), 3);
  assert(bishfpm_add_event_weighted(e, "g", 1000, items, 3, 2, heuristic_alert,
                                    e) == 4);
  assert(bishfpm_add_event_weighted(e, "g", 0, items, 3, 2, NULL, NULL) >= 0);
  close_to(bishfpm_pattern_rate(e, "g", "a\037b", 1000), 4.5);
  bishfpm_stats_t st;
  bishfpm_get_stats(e, &st);
  bishfpm_memory_stats_t m;
  bishfpm_get_memory_stats(e, &m);
  assert(st.memory_current_bytes == m.current_bytes &&
         m.current_bytes <= st.memory_bound_bytes);
  if (dynamic)
    assert(m.current_bytes <= m.peak_bytes && m.peak_bytes <= m.limit_bytes);
  bishfpm_store_snapshot_t snap;
  assert(bishfpm_get_store_snapshot(e, "g", sizeof(snap), &snap));
  assert(snap.resident_patterns == 4 && snap.resident_capacity == 16 &&
         snap.diagnostic[0] == diag0 && snap.diagnostic[1] == diag1);
  assert(bishfpm_prune(e, 1000000, .1) == 1);
  assert(bishfpm_add_event(e, "g", 0, items, 3, heuristic_alert, e) == 4);
  *(uint32_t *)s = saved;
  if (!dynamic) {
    c.max_memory_bytes = st.memory_bound_bytes - 1;
    assert(!create_with_options(d, &c, s, n, dynamic));
  }
  bishfpm_free(e);
  c = mining();
  c.max_items_per_group = 2;
  e = create_with_options(d, &c, s, n, dynamic);
  assert(e);
  assert(bishfpm_add_event(e, "g", 1000, items, 3, NULL, NULL) == -1);
  assert(bishfpm_group_rate(e, "g", 1000) == 0);
  assert(bishfpm_add_event(e, "g", 0, items, 2, NULL, NULL) == 1);
  assert(bishfpm_add_event(e, "g", 1000, items, 3, NULL, NULL) == -1);
  close_to(bishfpm_pattern_rate(e, "g", "a\037b", 0), 1);
  assert(bishfpm_add_event(e, "g", 0, items, 2, NULL, NULL) == 1);
  close_to(bishfpm_pattern_rate(e, "g", "a\037b", 0), 2);
  bishfpm_free(e);
}
static inline void check_heuristic_failures(const bishfpm_store_descriptor_t *d,
                                            const void *s, size_t n,
                                            int dynamic,
                                            unsigned min_rejected) {
  unsigned constructor_failures = 0, rejected = 0;
  for (long fault = 0; fault < 24; fault++) {
    bishfpm_config_t c = mining();
    calls = 0;
    fail_at = fault;
    bishfpm_t *e = create_with_options(d, &c, s, n, dynamic);
    fail_at = -1;
    if (e)
      bishfpm_free(e);
    else
      constructor_failures++;
  }
  for (long fault = 0; fault < 24; fault++) {
    bishfpm_config_t c = mining();
    bishfpm_t *e = create_with_options(d, &c, s, n, dynamic);
    assert(e);
    calls = 0;
    fail_at = fault;
    int result = bishfpm_add_event(e, "g", 1000, items, 3, NULL, NULL);
    fail_at = -1;
    if (result < 0) {
      rejected++;
      assert(bishfpm_group_rate(e, "g", 0) == 0);
      assert(bishfpm_add_event(e, "g", 0, items, 3, NULL, NULL) == 4);
      close_to(bishfpm_pattern_rate(e, "g", "a\037b", 0), 1);
    } else
      close_to(bishfpm_pattern_rate(e, "g", "a\037b", 1000), 1);
    bishfpm_free(e);
  }
  assert(constructor_failures >= 8 && rejected >= min_rejected);
}
static inline void
check_heuristic_capabilities(const bishfpm_store_descriptor_t *d,
                             uint32_t expected, const void *s, size_t n) {
  assert(d->capabilities == expected);
  bishfpm_config_t c = mining();
  bishfpm_store_descriptor_t invalid = *d;
  invalid.capabilities |= BISHFPM_STORE_RESIDENT_BOUNDS;
  assert(!bishfpm_create_with_store(&c, NULL, &invalid, s, n));
  invalid = *d;
  invalid.capabilities |= BISHFPM_STORE_ABSENT_BOUNDS;
  assert(!bishfpm_create_with_store(&c, NULL, &invalid, s, n));
  if (d->representation == BISHFPM_STORE_TEXT) {
    ledger_t l = {0, 0, -1, 0};
    bishfpm_store_host_t h = {&l, allocate, zeroed, release};
    void *config = NULL, *state = NULL;
    assert(d->clone_config(&h, &c, s, n, &config));
    assert(d->create(&h, config, &state));
    char *slice = malloc(3);
    assert(slice);
    memcpy(slice, "abc", 3);
    uint64_t hash = bishfpm_murmur3_64(slice, 3, c.hash_seed);
    assert(d->update_text(state, slice, 3, hash, 2, 0).upper == 2);
    assert(d->query_text(state, slice, 3, hash, 0).upper == 2);
    free(slice);
    assert(d->update_text(state, NULL, 1, 0, 1, 0).outcome ==
           BISHFPM_UPDATE_REJECTED);
    assert(d->update_text(state, "a", 0, 0, 1, 0).outcome ==
           BISHFPM_UPDATE_REJECTED);
    assert(
        d->update_text(state, "a", BISHFPM_MAX_PATTERN_LEN, 0, 1, 0).outcome ==
        BISHFPM_UPDATE_REJECTED);
    assert(d->query_text(state, NULL, 1, 0, 0).outcome == BISHFPM_UPDATE_ZERO);
    assert(d->query_text(state, "a", 0, 0, 0).outcome == BISHFPM_UPDATE_ZERO);
    d->destroy(&h, state);
    d->destroy_config(&h, config);
    assert(!l.bytes);
  }
}
#endif
#endif
