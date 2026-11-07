#include "bishfpm_wavingsketch.h"
#include "plugin_test.h"

static const bishfpm_store_descriptor_t *d = &bishfpm_wavingsketch_store;
static void primitives(void) {
  bishfpm_config_t c = mining();
  bishfpm_wavingsketch_options_t o = {7, 4};
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
        assert(d->prune(state, 0, 20) == 0);
        assert(d->prune(state, 1000, 20) == 0);
        close_to(d->query_text(state, "ab", 2, 0, 0).upper, 5);
        close_to(d->update_text(state, "ab", 2, 0, 1, 1000).upper, 6);
        assert(d->prune(state, UINT64_MAX, 20) == 1);
        assert(!d->query_text(state, "ab", 2, 0, UINT64_MAX).resident);
        d->destroy(&h, state);
      }
      d->destroy_config(&h, config);
    }
    assert(l.bytes == 0);
  }
  const bishfpm_wavingsketch_options_t invalid[] = {
      {0, 4}, {7, 0}, {7, 65}, {(1U << 24) + 1, 1}, {1000000, 2}};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++)
    assert(!d->plan(&c, &invalid[i], sizeof(o), &plan));
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
  /* a large value can survive even when a standalone decay factor underflows */
  close_to(d->query_text(state, "a", 1, 0, 1100000).upper,
           bishfpm_rate_decay(DBL_MAX, 1100000, 1000));
  assert(d->query_text(state, "a", 1, 0, 1100000).upper > 0);
  d->destroy(&h, state);
  d->destroy_config(&h, config);
  assert(!l.bytes);
}
static int sign_of(const char *key, uint64_t seed) {
  return (bishfpm_hash_str(key, seed ^ UINT64_C(0x13198a2e03707344)) & 1) ? 1
                                                                          : -1;
}
static void cases(void) {
  bishfpm_config_t c = mining();
  bishfpm_wavingsketch_options_t o = {1, 1};
  char positive[32] = "", negative[2][32] = {{0}};
  unsigned negatives = 0;
  for (unsigned i = 0; !*positive || negatives < 2; i++) {
    char key[32];
    snprintf(key, sizeof(key), "sign%u", i);
    if (sign_of(key, c.hash_seed) > 0 && !*positive)
      strcpy(positive, key);
    else if (sign_of(key, c.hash_seed) < 0 && negatives < 2)
      strcpy(negative[negatives++], key);
  }
  ledger_t l = {0, 0, -1, 0};
  bishfpm_store_host_t h = {&l, allocate, zeroed, release};
  void *config, *state;
  assert(d->clone_config(&h, &c, &o, sizeof(o), &config));
  assert(d->create(&h, config, &state));
#define ADD(key, w) d->update_text(state, key, strlen(key), 0, w, 0)
#define GET(key, t) d->query_text(state, key, strlen(key), 0, t)
  /* exact cells don't touch the wave; full-bucket misses do, even if absent */
  assert(ADD(positive, 2).outcome == BISHFPM_UPDATE_ADMITTED);
  assert(GET(negative[0], 0).upper == 0);
  bishfpm_store_count_t r = ADD(negative[0], 2);
  assert(!r.resident && r.outcome == BISHFPM_UPDATE_FILTERED && r.upper == 2);
  assert(GET(positive, 0).upper == 2);
  /* compare before adding the incoming weight; equality permits eviction;
   * transfer the exact victim's signed count: wave=-3+2=-1 */
  r = ADD(negative[0], 1);
  assert(r.resident && r.outcome == BISHFPM_UPDATE_REPLACED && r.upper == 3);
  assert(GET(negative[1], 0).upper == 1);
  assert(GET(positive, 0).upper == 0); /* negative projection is clipped */
  assert(ADD(negative[0], 2).upper ==
         5); /* approximate hit updates wave to -3 */
  assert(ADD(negative[1], 1).outcome == BISHFPM_UPDATE_FILTERED);
  assert(ADD(negative[1], 1).outcome == BISHFPM_UPDATE_FILTERED);
  r = ADD(negative[1], .5);
  assert(r.outcome == BISHFPM_UPDATE_REPLACED && r.upper == 5.5);
  /* the approximate victim is already in the wave; never transfer twice */
  assert(GET(negative[0], 0).upper == 5.5);
  assert(d->prune(state, 0, 100) == 0);
  assert(d->update_text(state, "unused", 6, 0, 0, 99999).outcome ==
         BISHFPM_UPDATE_ZERO);
  close_to(GET(negative[1], 1000).upper, 2.75);
  assert(GET(negative[1], 0).upper == 5.5);
  assert(d->prune(state, UINT64_MAX, 100) == 1);
  assert(GET(negative[1], UINT64_MAX).upper == 0);
  assert(ADD(positive, 1).outcome == BISHFPM_UPDATE_ADMITTED);
  d->destroy(&h, state);
  assert(d->create(&h, config, &state));
  assert(ADD(positive, 1).resident);
  assert(ADD(negative[0], DBL_MAX).outcome == BISHFPM_UPDATE_FILTERED);
  assert(GET(negative[0], 0).upper == DBL_MAX);
  /* the heavy cell underflows first; nonzero negative wave history keeps
   * the bucket intact, even when its standalone decay factor is zero */
  assert(d->prune(state, 1100000, 100) == 0);
  assert(GET(positive, 1100000).resident);
  assert(GET(positive, 1100000).upper == 0);
  assert(GET(negative[0], 1100000).upper > 0);
  close_to(GET(negative[0], 1100000).upper,
           bishfpm_rate_decay(DBL_MAX, 1100000, 1000));
  assert(d->prune(state, UINT64_MAX, 100) == 1);
  d->destroy(&h, state);
  d->destroy_config(&h, config);
  assert(!l.bytes);
#undef ADD
#undef GET
}
/* independent per-identity model, eagerly decaying all buckets on every step;
 * tracks a slot number for deterministic minimum ties, without mirroring
 * the backend's cell arrays or lazy per-bucket timestamps */
static void reference_replay(void) {
  for (unsigned test = 0; test < 6; test++) {
    bishfpm_config_t c = mining();
    c.hash_seed += test;
    bishfpm_wavingsketch_options_t o = {1 + test % 3, 1 + test % 4};
    ledger_t l = {0, 0, -1, 0};
    bishfpm_store_host_t h = {&l, allocate, zeroed, release};
    void *config, *state;
    assert(d->clone_config(&h, &c, &o, sizeof(o), &config));
    assert(d->create(&h, config, &state));
    char keys[31][32];
    double rates[31] = {0}, waves[3] = {0}, evidence[31] = {0};
    int positions[31], signs[31];
    bool exact[31] = {0};
    unsigned bucket[31], counts[3] = {0};
    for (unsigned i = 0; i < 31; i++) {
      snprintf(keys[i], sizeof(keys[i]), "identity%u", i);
      positions[i] = -1;
      signs[i] = sign_of(keys[i], c.hash_seed);
      bucket[i] = bishfpm_hash_str(keys[i],
                                   c.hash_seed ^ UINT64_C(0x243f6a8885a308d3)) %
                  o.buckets;
    }
    unsigned filtered = 0, exact_evictions = 0, approx_evictions = 0;
    uint64_t rng = 123, time = 0;
    for (unsigned step = 0; step < 3000; step++) {
      unsigned key = bishfpm_splitmix64(&rng) % 31, b = bucket[key];
      double weight = .1 + (bishfpm_splitmix64(&rng) % 100003) / 7001.;
      uint64_t dt = bishfpm_splitmix64(&rng) % 17;
      time += dt;
      double factor = exp2(-(double)dt / 1000);
      for (unsigned i = 0; i < 31; i++) {
        rates[i] *= factor;
        evidence[i] *= factor;
      }
      for (unsigned i = 0; i < o.buckets; i++)
        waves[i] *= factor;
      unsigned expected;
      if (positions[key] >= 0) {
        rates[key] += weight;
        evidence[key] += weight;
        if (!exact[key])
          waves[b] += signs[key] * weight;
        expected = BISHFPM_UPDATE_RESIDENT;
      } else if (counts[b] < o.cells_per_bucket) {
        positions[key] = (int)counts[b]++;
        exact[key] = true;
        rates[key] = weight;
        evidence[key] = weight;
        expected = BISHFPM_UPDATE_ADMITTED;
      } else {
        unsigned min = 31;
        for (unsigned i = 0; i < 31; i++)
          if (bucket[i] == b && positions[i] >= 0 &&
              (min == 31 || rates[i] < rates[min] ||
               (rates[i] == rates[min] && positions[i] < positions[min])))
            min = i;
        assert(min < 31);
        double before = waves[b] * signs[key];
        waves[b] += signs[key] * weight;
        if (before >= rates[min]) {
          positions[key] = positions[min];
          positions[min] = -1;
          evidence[min] = 0;
          evidence[key] = weight;
          rates[key] = before + weight;
          exact[key] = false;
          if (exact[min]) {
            waves[b] += rates[min] * signs[min];
            exact_evictions++;
          } else
            approx_evictions++;
          expected = BISHFPM_UPDATE_REPLACED;
        } else {
          expected = BISHFPM_UPDATE_FILTERED;
          filtered++;
        }
      }
      /* all identities deliberately share the caller's hash prefilter */
      assert(d->update_text(state, "zero", 4, 0, 0, time + 99999).outcome ==
             BISHFPM_UPDATE_ZERO);
      bishfpm_store_count_t result =
          d->update_text(state, keys[key], strlen(keys[key]), 0, weight, time);
      assert(result.outcome == expected);
      for (unsigned i = 0; i < 31; i++) {
        double value =
            positions[i] >= 0 ? rates[i] : fmax(0, waves[bucket[i]] * signs[i]);
        result = d->query_text(state, keys[i], strlen(keys[i]), 0, time + 1000);
        close_to(result.upper, value / 2);
        close_to(result.lower, evidence[i] / 2);
        result = d->query_text(state, keys[i], strlen(keys[i]), 0, time);
        close_to(result.upper, value);
        close_to(result.lower, evidence[i]);
        assert(result.resident == (positions[i] >= 0) && !result.known);
      }
      if (!(step % 79))
        assert(d->prune(state, time, 1e6) == 0);
      bishfpm_store_snapshot_t snap;
      d->snapshot(state, &snap);
      assert(snap.resident_patterns == counts[0] + counts[1] + counts[2]);
    }
    assert(filtered && exact_evictions && approx_evictions);
    d->destroy(&h, state);
    d->destroy_config(&h, config);
    assert(!l.bytes);
  }
}
static void admission_evidence(int dynamic) {
  bishfpm_config_t c = mining();
  c.max_itemset_order = 2;
  c.min_pattern_rate = 3;
  char positive[32] = "", negative[32] = "", pattern[64];
  for (unsigned i = 0; !*positive || !*negative; i++) {
    char item[32];
    snprintf(item, sizeof(item), "item%u", i);
    snprintf(pattern, sizeof(pattern), "anchor\037%s", item);
    if (sign_of(pattern, c.hash_seed) > 0 && !*positive)
      strcpy(positive, item);
    if (sign_of(pattern, c.hash_seed) < 0 && !*negative)
      strcpy(negative, item);
  }
  const char *p[] = {"anchor", positive}, *n[] = {"anchor", negative};
  bishfpm_wavingsketch_options_t options = {1, 1};
  bishfpm_t *e = create_with_options(d, &c, &options, sizeof(options), dynamic);
  assert(e);
  unsigned alerts = 0;
  assert(bishfpm_add_event_weighted(e, "g", 0, p, 2, 2, confirmed_alert,
                                    &alerts) == 0);
  assert(bishfpm_add_event_weighted(e, "g", 0, n, 2, 2, confirmed_alert,
                                    &alerts) == 0);
  /* earlier nonresident update is discovery history, not admission evidence */
  assert(bishfpm_add_event(e, "g", 0, n, 2, confirmed_alert, &alerts) == 0);
  snprintf(pattern, sizeof(pattern), "anchor\037%s", negative);
  assert(bishfpm_pattern_rate(e, "g", pattern, 0) == 3);
  assert(bishfpm_add_event_weighted(e, "g", 0, n, 2, 2, confirmed_alert,
                                    &alerts) == 1);
  assert(bishfpm_pattern_rate(e, "g", pattern, 0) == 5);
  /* at t=1000, evidence=1.5+1; the raw estimate is already 3.5 */
  assert(bishfpm_add_event(e, "g", 1000, n, 2, confirmed_alert, &alerts) == 0);
  assert(bishfpm_add_event(e, "g", 0, n, 2, confirmed_alert, &alerts) == 1);
  assert(alerts == 2);
  bishfpm_count_bounds_t bounds;
  assert(bishfpm_pattern_bounds(e, "g", pattern, 1000, &bounds));
  assert(bounds.resident && !bounds.known && bounds.lower == 0 &&
         isinf(bounds.upper));
  bishfpm_free(e);
}
/* unrelated bucket traffic must not repeatedly round another identity's
 * evidence; at t=3 eager decay rounds 5 ulps -> 2 -> 1 -> 0, while the
 * matching-update counter correctly retains round(5/8) == 1 ulp */
static void lazy_evidence(void) {
  bishfpm_config_t c = mining();
  c.ewma_half_life_ms = 1;
  bishfpm_wavingsketch_options_t o = {1, 2};
  ledger_t l = {0, 0, -1, 0};
  bishfpm_store_host_t h = {&l, allocate, zeroed, release};
  void *config = NULL, *state = NULL;
  assert(d->clone_config(&h, &c, &o, sizeof(o), &config));
  assert(d->create(&h, config, &state));
  const double tiny = DBL_TRUE_MIN;
  assert(d->update_text(state, "a", 1, 1, 5 * tiny, 0).resident);
  assert(d->update_text(state, "b", 1, 2, 1, 0).resident);
  for (uint64_t now = 1; now <= 3; now++)
    assert(d->update_text(state, "b", 1, 2, 1, now).resident);
  bishfpm_store_count_t r = d->query_text(state, "a", 1, 1, 3);
  assert(r.resident && r.upper == 0 && r.lower == tiny);
  /* future zero updates and queries cannot consume the resident's evidence */
  assert(d->update_text(state, "a", 1, 1, 0, 10000).lower == 0);
  assert(d->query_text(state, "a", 1, 1, 0).lower == tiny);
  r = d->update_text(state, "a", 1, 1, 2 * tiny, 3);
  assert(r.lower == 3 * tiny && r.upper == 2 * tiny);
  assert(d->prune(state, UINT64_MAX, 0) == 2);
  assert(!d->query_text(state, "a", 1, 1, UINT64_MAX).resident);
  d->destroy(&h, state);
  assert(d->create(&h, config, &state));
  assert(d->update_text(state, "a", 1, 1, 9 * tiny, 0).resident);
  assert(d->update_text(state, "b", 1, 2, tiny, 0).resident);
  for (uint64_t now = 1; now <= 3; now++)
    assert(d->update_text(state, "b", 1, 2, tiny, now).resident);
  /* at t=4 both heavy counts and wave are zero, but independently decayed
   * evidence retains one ulp; pruning must preserve that evidence */
  assert(d->prune(state, 4, 0) == 0);
  r = d->query_text(state, "a", 1, 1, 4);
  assert(r.resident && r.upper == 0 && r.lower == tiny);
  assert(d->prune(state, 5, 0) == 2);
  d->destroy(&h, state);
  d->destroy_config(&h, config);
  assert(l.bytes == 0);
}
int main(void) {
  bishfpm_wavingsketch_options_t s = {1, 16};
  check_heuristic_capabilities(
      d, BISHFPM_STORE_HEURISTIC_ESTIMATES | BISHFPM_STORE_ALERT_EVIDENCE, &s,
      sizeof(s));
  primitives();
  reference_replay();
  lazy_evidence();
  cases();
  for (int dynamic = 0; dynamic < 2; dynamic++) {
    check_heuristic_engines(d, &s, sizeof(s), dynamic, 1, 16);
    admission_evidence(dynamic);
    check_heuristic_failures(d, &s, sizeof(s), dynamic, 9);
  }
  puts("test_wavingsketch passed.");
}
