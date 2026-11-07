#include "bishfpm_heavykeeper.h"
#include "plugin_test.h"

static const bishfpm_store_descriptor_t *d = &bishfpm_heavykeeper_store;
static void primitives(void) {
  bishfpm_config_t c = mining();
  bishfpm_heavykeeper_options_t o = {7, 4, 17, .95};
  bishfpm_store_plan_t plan;
  assert(d->plan(&c, &o, sizeof(o), &plan));
  for (long fault = 0; fault < 7; fault++) {
    ledger_t l = {0, 0, fault, 0};
    bishfpm_store_host_t h = {&l, allocate, zeroed, release};
    void *config = NULL, *state = NULL;
    if (d->clone_config(&h, &c, &o, sizeof(o), &config)) {
      if (d->create(&h, config, &state)) {
        assert(l.peak == plan.engine_peak_bytes + plan.group_peak_bytes);
        uint64_t hash = bishfpm_hash_str("a\037b", 42);
        bishfpm_store_count_t r =
            d->update_text(state, "a\037b", 3, hash, 10, 0);
        assert(r.resident && !r.known && r.lower == 0);
        close_to(r.upper, 10);
        r = d->query_text(state, "a\037b", 3, hash, 1000);
        close_to(r.upper, 5);
        r = d->query_text(state, "a\037b", 3, hash, 0);
        close_to(r.upper, 10);
        r = d->update_text(state, "a\037b", 3, hash, 0, 100000);
        assert(r.outcome == BISHFPM_UPDATE_ZERO);
        r = d->query_text(state, "a\037b", 3, hash, 0);
        close_to(r.upper, 10);
        assert(d->prune(state, 0, 20) == 1);
        r = d->query_text(state, "a\037b", 3, hash, 0);
        assert(!r.resident);
        close_to(r.upper, 10); /* empty heap retains sketch history */
        r = d->update_text(state, "a\037b", 3, hash, 1, 0);
        assert(r.resident);
        close_to(r.upper, 11);
        r = d->update_text(state, "a\037b", 3, hash, DBL_MAX, 0);
        /* rounding DBL_MAX+10 is finite; a second maximum must reject
         * unchanged */
        assert(r.outcome == BISHFPM_UPDATE_REJECTED || r.upper == DBL_MAX);
        if (r.upper == DBL_MAX) {
          r = d->update_text(state, "a\037b", 3, hash, DBL_MAX, 0);
          assert(r.outcome == BISHFPM_UPDATE_REJECTED);
          close_to(d->query_text(state, "a\037b", 3, hash, 0).upper, DBL_MAX);
        }
        assert(d->update_text(state, "a\037b", 3, hash, -1, 0).outcome ==
               BISHFPM_UPDATE_REJECTED);
        assert(d->update_text(state, "a\037b", 3, hash, NAN, 0).outcome ==
               BISHFPM_UPDATE_REJECTED);
        d->destroy(&h, state);
      }
      d->destroy_config(&h, config);
    }
    assert(l.bytes == 0);
  }
  /* large counts beyond 2^53 must stay safe at the max normalized base on
   * collision */
  bishfpm_heavykeeper_options_t limit = {1, 1, 1, .99};
  ledger_t l = {0, 0, -1, 0};
  bishfpm_store_host_t h = {&l, allocate, zeroed, release};
  void *config = NULL, *state = NULL;
  assert(d->clone_config(&h, &c, &limit, sizeof(limit), &config));
  assert(d->create(&h, config, &state));
  assert(d->update_text(state, "a", 1, 0, 1e16, 0).upper == 1e16);
  assert(d->update_text(state, "b", 1, 0, 1e16, 0).outcome ==
         BISHFPM_UPDATE_FILTERED);
  d->destroy(&h, state);
  d->destroy_config(&h, config);
  assert(!l.bytes);
  const double valid_bases[] = {DBL_TRUE_MIN, .95, .99, 1 / .99, DBL_MAX};
  for (size_t i = 0; i < sizeof(valid_bases) / sizeof(*valid_bases); i++) {
    bishfpm_heavykeeper_options_t valid = o;
    valid.decay_base = valid_bases[i];
    assert(d->plan(&c, &valid, sizeof(valid), &plan));
    assert(d->clone_config(&h, &c, &valid, sizeof(valid), &config));
    d->destroy_config(&h, config);
    assert(!l.bytes);
  }
  for (unsigned bad = 0; bad < 13; bad++) {
    bishfpm_heavykeeper_options_t invalid = o;
    switch (bad) {
    case 0:
      invalid.capacity = 0;
      break;
    case 1:
      invalid.depth = 0;
      break;
    case 2:
      invalid.depth = 33;
      break;
    case 3:
      invalid.width = 0;
      break;
    case 4:
      invalid.width = (1U << 24) + 1;
      break;
    case 5:
      invalid.decay_base = 1;
      break;
    case 6:
      invalid.decay_base = NAN;
      break;
    case 7:
      invalid.decay_base = INFINITY;
      break;
    case 8:
      invalid.decay_base = -.5;
      break;
    case 9:
      invalid.decay_base = nextafter(.99, 1);
      break;
    case 10:
      invalid.decay_base = nextafter(1 / .99, 1);
      break;
    case 11:
      invalid.decay_base = 1 - 1e-16;
      break;
    case 12:
      invalid.decay_base = 1 / (1 - 1e-16);
      break;
    }
    assert(!d->plan(&c, &invalid, sizeof(invalid), &plan));
    assert(!d->clone_config(&h, &c, &invalid, sizeof(invalid), &config));
    assert(!l.bytes);
  }
  assert(!d->plan(NULL, &o, sizeof(o), &plan));
  assert(!d->plan(&c, &o, sizeof(o), NULL));
  assert(!d->plan(&c, NULL, 0, &plan));
  assert(!d->plan(&c, &o, sizeof(o) - 1, &plan));
}
static void identity_collisions(void) {
  bishfpm_config_t c = mining();
  bishfpm_heavykeeper_options_t o = {7, 4, 17, .95};
  ledger_t l = {0, 0, -1, 0};
  bishfpm_store_host_t host = {&l, allocate, zeroed, release};
  void *config = NULL, *state = NULL;
  assert(d->clone_config(&host, &c, &o, sizeof(o), &config));
  assert(d->create(&host, config, &state));
  for (unsigned step = 0; step < 1000; step++) {
    char key[32];
    snprintf(key, sizeof(key), "collision%u", step % 31);
    d->update_text(state, key, strlen(key), 0, step % 9 + 1, step);
    if (step % 31 == 0) {
      unsigned residents = 0;
      for (unsigned i = 0; i < 31; i++) {
        snprintf(key, sizeof(key), "collision%u", i);
        residents += d->query_text(state, key, strlen(key), 0, step).resident;
      }
      bishfpm_store_snapshot_t snap;
      d->snapshot(state, &snap);
      assert(residents == snap.resident_patterns && residents <= 7);
      d->prune(state, step, .8);
    }
  }
  d->destroy(&host, state);
  d->destroy_config(&host, config);
  assert(!l.bytes);
}
typedef struct {
  double rate;
  uint64_t time;
  unsigned count;
} small_update_alert_t;
static void small_update_alert(const char *group, const bishfpm_trigger_t *t,
                               void *ctx) {
  small_update_alert_t *expected = ctx;
  assert(!strcmp(group, "g") && !strcmp(t->pattern, "a\037b"));
  assert(t->timestamp == expected->time);
  close_to(t->pattern_rate, expected->rate);
  expected->count++;
}
static void small_matching_updates(int dynamic) {
  bishfpm_config_t c = mining();
  c.max_itemset_order = 2;
  c.min_lift = 0;
  bishfpm_heavykeeper_options_t s = {1, 1, 1, .95};
  bishfpm_t *e = create_with_options(d, &c, &s, sizeof(s), dynamic);
  assert(e);
  /* advance group watermark with a singleton so each late unit event adds
   * 1/256 to the pair sketch */
  assert(bishfpm_add_event(e, "g", 8000, items, 1, NULL, NULL) == 0);
  small_update_alert_t expected = {0, 8000, 0};
  for (unsigned i = 1; i <= 1000; i++) {
    expected.rate = i / 256.0;
    assert(bishfpm_add_event(e, "g", 0, items, 2, small_update_alert,
                             &expected) == (i >= 256));
    close_to(bishfpm_pattern_rate(e, "g", "a\037b", 8000), expected.rate);
  }
  assert(expected.count == 745);
  close_to(bishfpm_group_rate(e, "g", 8000), 1 + 1000 / 256.0);
  bishfpm_free(e);

  /* matching history survives when decay takes a counter below .01 */
  e = create_with_options(d, &c, &s, sizeof(s), dynamic);
  assert(e);
  expected = (small_update_alert_t){1, 0, 0};
  assert(bishfpm_add_event(e, "g", 0, items, 2, small_update_alert,
                           &expected) == 1);
  assert(bishfpm_add_event(e, "g", 8000, items, 1, NULL, NULL) == 0);
  close_to(bishfpm_pattern_rate(e, "g", "a\037b", 8000), 1 / 256.0);
  assert(bishfpm_add_event(e, "g", 0, items, 2, small_update_alert,
                           &expected) == 0);
  close_to(bishfpm_pattern_rate(e, "g", "a\037b", 8000), 2 / 256.0);
  expected.rate = 1 + 2 / 256.0;
  expected.time = 8000;
  assert(bishfpm_add_event(e, "g", 8000, items, 2, small_update_alert,
                           &expected) == 1);
  close_to(bishfpm_pattern_rate(e, "g", "a\037b", 8000), expected.rate);
  assert(expected.count == 2);
  bishfpm_free(e);
}
static void large_weight_collisions(int dynamic) {
  bishfpm_config_t c = mining();
  c.max_itemset_order = 2;
  c.min_lift = 0;
  bishfpm_heavykeeper_options_t s = {1, 1, 1, 1 - 1e-16};
  /* reject unbounded decay bases in either input convention */
  assert(!create_with_options(d, &c, &s, sizeof(s), dynamic));
  s.decay_base = 1 / s.decay_base;
  assert(!create_with_options(d, &c, &s, sizeof(s), dynamic));
  const double bases[] = {.99, 1 / .99};
  const uint32_t weights[] = {UINT32_MAX, 1024};
  for (size_t i = 0; i < sizeof(bases) / sizeof(*bases); i++) {
    s.decay_base = bases[i];
    for (size_t j = 0; j < sizeof(weights) / sizeof(*weights); j++) {
      bishfpm_t *e = create_with_options(d, &c, &s, sizeof(s), dynamic);
      assert(e);
      uint32_t initial = weights[j];
      assert(bishfpm_add_event_weighted(e, "g", 0, items, 2, initial, NULL,
                                        NULL) == 1);
      int replaced = initial == 1024;
      assert(bishfpm_add_event_weighted(e, "g", 0, items + 1, 2, UINT32_MAX,
                                        NULL, NULL) == replaced);
      close_to(bishfpm_pattern_rate(e, "g", "a\037b", 0),
               replaced ? 0 : initial);
      double estimate = bishfpm_pattern_rate(e, "g", "b\037c", 0);
      assert(estimate <= UINT32_MAX &&
             (replaced ? estimate > 0 : estimate == 0));
      close_to(bishfpm_group_rate(e, "g", 0), (double)initial + UINT32_MAX);
      bishfpm_free(e);
    }
  }
}
int main(void) {
  bishfpm_heavykeeper_options_t s = {16, 4, 17, .95};
  check_heuristic_capabilities(d, BISHFPM_STORE_HEURISTIC_ESTIMATES, &s,
                               sizeof(s));
  primitives();
  identity_collisions();
  for (int dynamic = 0; dynamic < 2; dynamic++) {
    check_heuristic_engines(d, &s, sizeof(s), dynamic, 4, 17);
    small_matching_updates(dynamic);
    large_weight_collisions(dynamic);
    check_heuristic_failures(d, &s, sizeof(s), dynamic, 10);
  }
  puts("test_heavykeeper passed.");
}
