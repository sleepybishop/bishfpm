#include "bishfpm_hash.h"
#include "bishfpm_rate.h"
#include "bishfpm_uss.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

typedef struct {
  size_t bytes, peak;
  long fail, allocs;
} ledger_t;
static void *allocate(void *ctx, size_t n) {
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
static void *zeroed(void *ctx, size_t n, size_t size) {
  assert(!n || size <= SIZE_MAX / n);
  void *p = allocate(ctx, n * size);
  if (p)
    memset(p, 0, n * size);
  return p;
}
static void release(void *ctx, void *p, size_t n) {
  ledger_t *l = ctx;
  if (p) {
    assert(l->bytes >= n);
    l->bytes -= n;
    free(p);
  }
}
static bishfpm_config_t mining(void) {
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
static const bishfpm_store_descriptor_t *d = &bishfpm_uss_store;
static const char *items[] = {"a", "b", "c"};
static void close_to(double x, double y) {
  assert(fabs(x - y) <= 1e-10 * fmax(1, fabs(y)));
}

static void primitives(void) {
  bishfpm_config_t c = mining();
  bishfpm_uss_options_t o = {7, 42};
  bishfpm_store_plan_t plan;
  assert(d->plan(&c, &o, sizeof(o), &plan));
  for (long fault = 0; fault < 6; fault++) {
    ledger_t l = {0, 0, fault, 0};
    bishfpm_store_host_t h = {&l, allocate, zeroed, release};
    void *config = NULL, *state = NULL;
    if (d->clone_config(&h, &c, &o, sizeof(o), &config)) {
      if (d->create(&h, config, &state)) {
        assert(l.peak == plan.engine_peak_bytes + plan.group_peak_bytes);
        bishfpm_store_count_t r = d->update_text(state, "ab", 2, 0, 10, 0);
        assert(r.resident && !r.known && r.lower == 10);
        close_to(r.upper, 10);
        close_to(d->query_text(state, "ab", 2, 0, 1000).upper, 5);
        close_to(d->query_text(state, "ab", 2, 0, 0).upper, 10);
        r = d->update_text(state, "ab", 2, 0, 0, 100000);
        assert(r.outcome == BISHFPM_UPDATE_ZERO);
        close_to(d->query_text(state, "ab", 2, 0, 0).upper, 10);
        assert(d->prune(state, 0, 20) == 0); /* Keep positive mass. */
        close_to(d->query_text(state, "ab", 2, 0, 0).upper, 10);
        assert(d->prune(state, 1000, 20) == 0);
        close_to(d->query_text(state, "ab", 2, 0, 0).upper, 5);
        r = d->update_text(state, "ab", 2, 0, 1, 1000);
        close_to(r.upper, 6);
        assert(d->prune(state, UINT64_MAX, 20) == 1);
        assert(!d->query_text(state, "ab", 2, 0, UINT64_MAX).resident);
        d->destroy(&h, state);
      }
      d->destroy_config(&h, config);
    }
    assert(l.bytes == 0);
  }
  o.capacity = 0;
  assert(!d->plan(&c, &o, sizeof(o), &plan));
  o.capacity = 1000001;
  assert(!d->plan(&c, &o, sizeof(o), &plan));
  o.capacity = 1;
  assert(!d->plan(NULL, &o, sizeof(o), &plan));
  assert(!d->plan(&c, &o, sizeof(o), NULL));
  assert(!d->plan(&c, NULL, sizeof(o), &plan));
  assert(!d->plan(&c, &o, sizeof(o) - 1, &plan));
  c.ewma_half_life_ms = NAN;
  assert(!d->plan(&c, &o, sizeof(o), &plan));
  c = mining();
  ledger_t l = {0, 0, -1, 0};
  bishfpm_store_host_t h = {&l, allocate, zeroed, release};
  void *config, *state;
  assert(d->clone_config(&h, &c, &o, sizeof(o), &config));
  assert(d->create(&h, config, &state));
  assert(d->update_text(state, "a", 1, 0, -1, 0).outcome ==
         BISHFPM_UPDATE_REJECTED);
  assert(d->update_text(state, "a", 1, 0, NAN, 0).outcome ==
         BISHFPM_UPDATE_REJECTED);
  assert(d->update_text(state, "a", 1, 0, DBL_MAX, 0).upper == DBL_MAX);
  assert(d->update_text(state, "b", 1, 0, DBL_MAX, 0).outcome ==
         BISHFPM_UPDATE_REJECTED);
  assert(d->query_text(state, "a", 1, 0, 0).upper == DBL_MAX);
  assert(!d->query_text(state, "b", 1, 0, 0).resident);
  assert(d->update_text(state, "a", 1, 0, DBL_MAX, 1000).outcome ==
         BISHFPM_UPDATE_REJECTED);
  assert(d->query_text(state, "a", 1, 0, 0).upper == DBL_MAX);
  d->destroy(&h, state);
  d->destroy_config(&h, config);
  assert(!l.bytes);
}
/* Independent linear-array oracle: eagerly decays every counter and scans
 * for the minimum. Force every identity to have the SAME index hash, so heap
 * moves, eviction and backward-shift deletion also exercise full-key checks. */
static uint64_t random64(uint64_t *state) {
  uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
  z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
  return z ^ (z >> 31);
}
static void reference_replay(void) {
  for (unsigned capacity = 1; capacity <= 17; capacity += 4) {
    bishfpm_config_t c = mining();
    bishfpm_uss_options_t o = {capacity, 987654321};
    ledger_t l = {0, 0, -1, 0};
    bishfpm_store_host_t h = {&l, allocate, zeroed, release};
    void *config, *state;
    assert(d->clone_config(&h, &c, &o, sizeof(o), &config));
    assert(d->create(&h, config, &state));
    double ref[31] = {0}, evidence[31] = {0};
    unsigned count = 0, filtered = 0, replaced = 0;
    uint64_t rng = o.random_seed, input_rng = 73, time = 0;
    double total = 0;
    for (unsigned step = 0; step < 3000; step++) {
      unsigned key = random64(&input_rng) % 31;
      double weight = .1 + (random64(&input_rng) % 100003) / 7001.;
      uint64_t dt = random64(&input_rng) % 17;
      time += dt;
      double decay = exp2(-(double)dt / c.ewma_half_life_ms);
      for (unsigned i = 0; i < 31; i++) {
        ref[i] *= decay;
        evidence[i] *= decay;
      }
      total = total * decay + weight;
      bool resident = ref[key] > 0;
      if (resident || count < capacity) {
        if (!resident)
          count++;
        ref[key] += weight;
        evidence[key] += weight;
      } else {
        unsigned min = 31;
        for (unsigned i = 0; i < 31; i++)
          if (ref[i] && (min == 31 || ref[i] < ref[min]))
            min = i;
        assert(min < 31);
        double combined = ref[min] + weight;
        double u = (double)(random64(&rng) >> 11) / 9007199254740992.;
        if (u < weight / combined) {
          ref[min] = 0;
          evidence[min] = 0;
          ref[key] = combined;
          evidence[key] = weight;
          replaced++;
        } else {
          ref[min] = combined;
          filtered++;
        }
      }
      char name[32];
      snprintf(name, sizeof(name), "key%u", key);
      /* A future zero update must leave both clock and RNG unchanged. */
      assert(d->update_text(state, "unused", 6, 0, 0, time + 99999).outcome ==
             BISHFPM_UPDATE_ZERO);
      bishfpm_store_count_t r =
          d->update_text(state, name, strlen(name), 0, weight, time);
      close_to(r.upper, ref[key]);
      close_to(r.lower, evidence[key]);
      assert(r.resident == (ref[key] > 0));
      double sum = 0;
      for (unsigned i = 0; i < 31; i++) {
        snprintf(name, sizeof(name), "key%u", i);
        r = d->query_text(state, name, strlen(name), 0, time + 1000);
        close_to(r.upper, ref[i] / 2);
        close_to(r.lower, evidence[i] / 2);
        r = d->query_text(state, name, strlen(name), 0, time);
        close_to(r.upper, ref[i]);
        close_to(r.lower, evidence[i]);
        assert(r.resident == (ref[i] > 0) && !r.known);
        sum += r.upper;
      }
      close_to(sum, total); /* All mass survives every stochastic reduction. */
      if (!(step % 79))
        assert(d->prune(state, time, 1e6) == 0);
      bishfpm_store_snapshot_t snap;
      d->snapshot(state, &snap);
      assert(snap.resident_patterns == count && count <= capacity);
    }
    assert(filtered && replaced);
    d->destroy(&h, state);
    d->destroy_config(&h, config);
    assert(!l.bytes);
  }
}
/* Distinguishes USS from unconditional Space Saving replacement and checks
 * fractional weights and decay over many independent deterministic seeds. */
static void unbiased_trials(void) {
  double sums[3] = {0};
  unsigned retained = 0;
  for (unsigned seed = 0; seed < 20000; seed++) {
    bishfpm_config_t c = mining();
    bishfpm_uss_options_t o = {1, seed};
    ledger_t l = {0, 0, -1, 0};
    bishfpm_store_host_t h = {&l, allocate, zeroed, release};
    void *config, *state;
    assert(d->clone_config(&h, &c, &o, sizeof(o), &config));
    assert(d->create(&h, config, &state));
    d->update_text(state, "a", 1, 0, 10, 0);
    bishfpm_store_count_t b = d->update_text(state, "b", 1, 0, .5, 1000);
    retained += b.resident;
    if (!b.resident) {
      assert(b.outcome == BISHFPM_UPDATE_FILTERED && b.upper == 0);
      close_to(d->query_text(state, "a", 1, 0, 1000).upper, 5.5);
    } else
      close_to(b.upper, 5.5);
    /* Fresh state for the multiple-reduction expectation check. */
    d->destroy(&h, state);
    assert(d->create(&h, config, &state));
    d->update_text(state, "a", 1, 0, 10, 0);
    d->update_text(state, "b", 1, 0, .5, 1000);
    d->update_text(state, "c", 1, 0, 3.25, 1000);
    for (unsigned i = 0; i < 3; i++) {
      char key[2] = {(char)('a' + i), 0};
      sums[i] += d->query_text(state, key, 1, 0, 1000).upper;
    }
    d->destroy(&h, state);
    d->destroy_config(&h, config);
    assert(!l.bytes);
  }
  assert(retained > 1600 && retained < 2050); /* p=.5/5.5 */
  assert(fabs(sums[0] / 20000 - 5) < .12);
  assert(fabs(sums[1] / 20000 - .5) < .06);
  assert(fabs(sums[2] / 20000 - 3.25) < .12);
}
static void alert(const char *group, const bishfpm_trigger_t *t, void *ctx) {
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
static bishfpm_t *engine(bishfpm_config_t *c, bishfpm_uss_options_t *s,
                         int dynamic) {
  bishfpm_store_engine_options_t o;
  bishfpm_store_engine_options_default(&o);
  o.memory_mode =
      dynamic ? BISHFPM_MEMORY_DYNAMIC : BISHFPM_MEMORY_CONSERVATIVE;
  return bishfpm_create_with_store(c, &o, d, s, sizeof(*s));
}
static void engines(int dynamic) {
  bishfpm_config_t c = mining();
  bishfpm_uss_options_t s = {16, 42};
  bishfpm_t *e = engine(&c, &s, dynamic);
  assert(e);
  s.capacity = 0; /* Config is owned after construction. */
  assert(bishfpm_add_event_weighted(e, "g", 0, items, 3, 3, alert, e) == 4);
  close_to(bishfpm_pattern_rate(e, "g", "a\037b", 1000), 1.5);
  close_to(bishfpm_pattern_rate(e, "g", "a\037b", 0), 3);
  assert(bishfpm_add_event_weighted(e, "g", 1000, items, 3, 2, alert, e) == 4);
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
         snap.diagnostic[0] == 0);
  assert(bishfpm_prune(e, 1000000, .1) == 1);
  assert(bishfpm_add_event(e, "g", 0, items, 3, alert, e) == 4);
  if (!dynamic) {
    c.max_memory_bytes = st.memory_bound_bytes - 1;
    s.capacity = 16;
    assert(!engine(&c, &s, dynamic));
  }
  bishfpm_free(e);
  c = mining();
  c.max_items_per_group = 2;
  s.capacity = 16;
  e = engine(&c, &s, dynamic);
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
static void confirmed_alert(const char *group, const bishfpm_trigger_t *t,
                            void *ctx) {
  (void)group;
  (*(unsigned *)ctx)++;
  close_to(t->pattern_rate, 3);
  assert(isinf(t->pattern_upper_bound) && isinf(t->pattern_error));
}
static void admission_evidence(int dynamic) {
  uint64_t seed = 0;
  for (;; seed++) {
    uint64_t rng = seed;
    double first = (double)(random64(&rng) >> 11) / 9007199254740992.;
    double second = (double)(random64(&rng) >> 11) / 9007199254740992.;
    if (first < 1. / 101 && second >= 1. / 102)
      break;
  }
  bishfpm_config_t c = mining();
  c.max_itemset_order = 2;
  c.min_pattern_rate = 3;
  bishfpm_uss_options_t options = {1, seed};
  bishfpm_t *e = engine(&c, &options, dynamic);
  assert(e);
  const char *ab[] = {"a", "b"}, *ac[] = {"a", "c"}, *ad[] = {"a", "d"};
  unsigned alerts = 0;
  assert(bishfpm_add_event_weighted(e, "g", 0, ab, 2, 100, NULL, NULL) == 1);
  /* A rare admission inherits 100 estimate units, but only one evidence unit.
   */
  assert(bishfpm_add_event(e, "g", 0, ac, 2, confirmed_alert, &alerts) == 0);
  assert(bishfpm_pattern_rate(e, "g", "a\037c", 0) == 101);
  /* A rejected admission increases the resident estimate, never its evidence.
   */
  assert(bishfpm_add_event(e, "g", 0, ad, 2, confirmed_alert, &alerts) == 0);
  assert(bishfpm_pattern_rate(e, "g", "a\037c", 0) == 102);
  assert(bishfpm_add_event(e, "g", 0, ac, 2, confirmed_alert, &alerts) == 0);
  assert(bishfpm_add_event(e, "g", 0, ac, 2, confirmed_alert, &alerts) == 1);
  assert(alerts == 1);
  bishfpm_count_bounds_t bounds;
  assert(bishfpm_pattern_bounds(e, "g", "a\037c", 0, &bounds));
  assert(bounds.resident && !bounds.known && bounds.lower == 0 &&
         isinf(bounds.upper));
  bishfpm_free(e);
}
static void failures(int dynamic) {
  unsigned constructor_failures = 0, rejected = 0;
  for (long fault = 0; fault < 24; fault++) {
    bishfpm_config_t c = mining();
    bishfpm_uss_options_t s = {16, 42};
    calls = 0;
    fail_at = fault;
    bishfpm_t *e = engine(&c, &s, dynamic);
    fail_at = -1;
    if (e)
      bishfpm_free(e);
    else
      constructor_failures++;
  }
  for (long fault = 0; fault < 24; fault++) {
    bishfpm_config_t c = mining();
    bishfpm_uss_options_t s = {16, 42};
    bishfpm_t *e = engine(&c, &s, dynamic);
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
  assert(constructor_failures >= 8 && rejected >= 9);
}
int main(void) {
  assert(d->capabilities ==
         (BISHFPM_STORE_HEURISTIC_ESTIMATES | BISHFPM_STORE_ALERT_EVIDENCE));
  primitives();
  reference_replay();
  unbiased_trials();
  for (int dynamic = 0; dynamic < 2; dynamic++) {
    engines(dynamic);
    admission_evidence(dynamic);
    failures(dynamic);
  }
  bishfpm_config_t c = mining();
  bishfpm_uss_options_t s = {16, 42};
  bishfpm_store_descriptor_t invalid = *d;
  invalid.capabilities |= BISHFPM_STORE_RESIDENT_BOUNDS;
  assert(!bishfpm_create_with_store(&c, NULL, &invalid, &s, sizeof(s)));
  invalid = *d;
  invalid.capabilities |= BISHFPM_STORE_ABSENT_BOUNDS;
  assert(!bishfpm_create_with_store(&c, NULL, &invalid, &s, sizeof(s)));
  puts("test_uss passed.");
}
