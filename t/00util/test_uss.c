#include "bishfpm_uss.h"
#include "plugin_test.h"

static const bishfpm_store_descriptor_t *d = &bishfpm_uss_store;
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
        assert(d->prune(state, 0, 20) == 0); /* keep positive mass */
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
/* independent linear-array oracle: eagerly decays every counter and scans
 * for the minimum; force every identity to have the same index hash, so heap
 * moves, eviction and backward-shift deletion also exercise full-key checks */
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
      unsigned key = bishfpm_splitmix64(&input_rng) % 31;
      double weight = .1 + (bishfpm_splitmix64(&input_rng) % 100003) / 7001.;
      uint64_t dt = bishfpm_splitmix64(&input_rng) % 17;
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
        double u = (double)(bishfpm_splitmix64(&rng) >> 11) / 9007199254740992.;
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
      /* a future zero update must leave both clock and RNG unchanged */
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
      close_to(sum, total); /* all mass survives every stochastic reduction */
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
/* distinguishes USS from unconditional Space Saving replacement and checks
 * fractional weights and decay over many independent deterministic seeds */
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
    /* fresh state for the multiple-reduction expectation check */
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
static void admission_evidence(int dynamic) {
  uint64_t seed = 0;
  for (;; seed++) {
    uint64_t rng = seed;
    double first = (double)(bishfpm_splitmix64(&rng) >> 11) / 9007199254740992.;
    double second =
        (double)(bishfpm_splitmix64(&rng) >> 11) / 9007199254740992.;
    if (first < 1. / 101 && second >= 1. / 102)
      break;
  }
  bishfpm_config_t c = mining();
  c.max_itemset_order = 2;
  c.min_pattern_rate = 3;
  bishfpm_uss_options_t options = {1, seed};
  bishfpm_t *e = create_with_options(d, &c, &options, sizeof(options), dynamic);
  assert(e);
  const char *ab[] = {"a", "b"}, *ac[] = {"a", "c"}, *ad[] = {"a", "d"};
  unsigned alerts = 0;
  assert(bishfpm_add_event_weighted(e, "g", 0, ab, 2, 100, NULL, NULL) == 1);
  /* a rare admission inherits 100 estimate units, but only one evidence unit */
  assert(bishfpm_add_event(e, "g", 0, ac, 2, confirmed_alert, &alerts) == 0);
  assert(bishfpm_pattern_rate(e, "g", "a\037c", 0) == 101);
  /* a rejected admission increases the resident estimate, never its evidence */
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
int main(void) {
  bishfpm_uss_options_t s = {16, 42};
  check_heuristic_capabilities(
      d, BISHFPM_STORE_HEURISTIC_ESTIMATES | BISHFPM_STORE_ALERT_EVIDENCE, &s,
      sizeof(s));
  primitives();
  reference_replay();
  unbiased_trials();
  for (int dynamic = 0; dynamic < 2; dynamic++) {
    check_heuristic_engines(d, &s, sizeof(s), dynamic, 0, 0);
    admission_evidence(dynamic);
    check_heuristic_failures(d, &s, sizeof(s), dynamic, 9);
  }
  puts("test_uss passed.");
}
