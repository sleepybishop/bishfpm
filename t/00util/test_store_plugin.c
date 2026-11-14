#include "examples/exact_store.h"
#include "plugin_test.h"

static const char *ab[] = {"a", "b"};
static const char *abc[] = {"a", "b", "c"};
static const char *keys[] = {"a", "b", "c", "d", "e", "f"};
static const char *patterns[] = {"a\037b", "a\037c", "b\037c"};
static bishfpm_config_t config(void) {
  bishfpm_config_t c = mining();
  c.max_itemset_order = 2;
  c.min_pattern_rate = 0;
  return c;
}
static bishfpm_t *create(bishfpm_config_t *c, unsigned rep, int dynamic,
                         unsigned capacity) {
  exact_store_options_t s = {capacity, rep, "owned configuration"};
  return create_with_options(rep == BISHFPM_STORE_TEXT ? &exact_text_store
                                                       : &exact_pair_store,
                             c, &s, sizeof(s), dynamic);
}
static void check_count(bishfpm_t *e, const char *p, uint64_t time,
                        long double expected) {
  bishfpm_count_bounds_t b;
  assert(bishfpm_pattern_bounds(e, "g", p, time, &b) && b.known);
  close_to(b.lower, expected);
  close_to(b.upper, expected);
  close_to(bishfpm_pattern_rate(e, "g", p, time), expected);
}
static void accounting(bishfpm_t *e) {
  bishfpm_stats_t s;
  bishfpm_memory_stats_t m;
  bishfpm_get_stats(e, &s);
  bishfpm_get_memory_stats(e, &m);
  assert(s.memory_current_bytes == m.current_bytes);
  assert(m.current_bytes <= s.memory_bound_bytes);
  assert(s.memory_bound_bytes <= m.limit_bytes);
  if (m.mode == BISHFPM_MEMORY_DYNAMIC)
    assert(m.current_bytes <= m.peak_bytes && m.peak_bytes <= m.limit_bytes);
}
static void failures(unsigned rep, int dynamic) {
  bishfpm_config_t c = config();
  unsigned constructor_failures = 0, event_failures = 0;
  for (long fault = 0; fault < 16; fault++) {
    calls = 0;
    fail_at = fault;
    bishfpm_t *e = create(&c, rep, dynamic, 16);
    fail_at = -1;
    if (!e) {
      constructor_failures++;
      continue;
    }
    assert(bishfpm_add_event(e, "g", 0, ab, 2, NULL, NULL) == 1);
    accounting(e);
    bishfpm_free(e);
  }
  assert(constructor_failures >= 9);
  for (long fault = 0; fault < 20; fault++) {
    bishfpm_t *e = create(&c, rep, dynamic, 16);
    assert(e);
    calls = 0;
    fail_at = fault;
    int result = bishfpm_add_event(e, "g", 1000, abc, 3, NULL, NULL);
    fail_at = -1;
    if (result < 0) {
      event_failures++;
      assert(bishfpm_group_rate(e, "g", 1000) == 0);
      bishfpm_stats_t s;
      bishfpm_get_stats(e, &s);
      assert(s.active_groups == 0);
      assert(bishfpm_add_event(e, "g", 0, abc, 3, NULL, NULL) == 3);
      for (unsigned i = 0; i < 3; i++)
        check_count(e, patterns[i], 0, 1);
    } else {
      assert(result >= 0);
      for (unsigned i = 0; i < 3; i++)
        check_count(e, patterns[i], 1000, 1);
    }
    accounting(e);
    bishfpm_free(e);
  }
  assert(event_failures >= 9);
  /* existing group: prepare succeeds, then marginal limit forces cancellation
   */
  c.max_items_per_group = 2;
  bishfpm_t *e = create(&c, rep, dynamic, 16);
  assert(e);
  assert(bishfpm_add_event(e, "g", 0, ab, 2, NULL, NULL) == 1);
  assert(bishfpm_add_event(e, "g", 2000, abc, 3, NULL, NULL) == -1);
  check_count(e, patterns[0], 0, 1);
  assert(bishfpm_group_rate(e, "g", 0) == 1);
  bishfpm_store_snapshot_t snap;
  assert(bishfpm_get_store_snapshot(e, "g", sizeof(snap), &snap));
  assert(snap.diagnostic[0] == 2 && snap.diagnostic[1] == 1 &&
         snap.diagnostic[2] == 1 && snap.diagnostic[3] == 1);
  assert(bishfpm_add_event(e, "g", 0, ab, 2, NULL, NULL) == 1);
  check_count(e, patterns[0], 0, 2);
  accounting(e);
  bishfpm_free(e);
}
static void capacity_and_empty(unsigned rep, int dynamic) {
  bishfpm_config_t c = config();
  bishfpm_t *e = create(&c, rep, dynamic, 1);
  assert(e);
  assert(bishfpm_add_event(e, "g", 0, ab, 2, NULL, NULL) == 1);
  assert(bishfpm_add_event(e, "g", 2000, abc, 3, NULL, NULL) == -1);
  assert(bishfpm_item_rate(e, "g", "c", 0) == 0);
  check_count(e, patterns[0], 0, 1);
  assert(bishfpm_add_event(e, "g", 0, ab, 2, NULL, NULL) == 1);
  check_count(e, patterns[0], 0, 2);
  assert(bishfpm_prune_pairs(e, "g", 1000, 1) == 0);
  bishfpm_pattern_pressure_t pressure;
  assert(!bishfpm_get_pattern_pressure(e, "g", &pressure));
  assert(bishfpm_prune(e, 1000, .01) == 0);
  check_count(e, patterns[0], 1000, 1);
  assert(bishfpm_prune(e, 100000, 1) == 1);
  assert(bishfpm_add_event(e, "g", 0, ab, 2, NULL, NULL) == 1);
  check_count(e, patterns[0], 0, 1);
  accounting(e);
  bishfpm_free(e);
  /* gate-empty event still commits and finishes its token */
  e = create(&c, rep, dynamic, 4);
  assert(e);
  assert(bishfpm_add_event(e, "g", 2000000, ab, 2, NULL, NULL) == 1);
  assert(bishfpm_add_event(e, "g", 0, abc, 3, NULL, NULL) >= 0);
  check_count(e, patterns[0], 2000000, 1);
  check_count(e, patterns[1], 2000000, 0);
  bishfpm_free(e);
  c.item_rate_threshold = c.min_pattern_rate = 100;
  e = create(&c, rep, dynamic, 4);
  assert(e);
  assert(bishfpm_add_event(e, "g", 0, abc, 3, NULL, NULL) == 0);
  bishfpm_store_snapshot_t snap;
  assert(bishfpm_get_store_snapshot(e, "g", sizeof(snap), &snap));
  assert(!snap.resident_patterns && snap.diagnostic[0] == 1 &&
         snap.diagnostic[2] == 1 && snap.diagnostic[3] == 1);
  bishfpm_free(e);
}
static int unbounded_plan(const bishfpm_config_t *c, const void *raw, size_t n,
                          bishfpm_store_plan_t *out) {
  if (!exact_text_store.plan(c, raw, n, out))
    return 0;
  out->engine_peak_bytes = out->group_peak_bytes = SIZE_MAX;
  return 1;
}
static bishfpm_store_count_t uncertified(void *state, const char *key, size_t n,
                                         uint64_t hash, double weight,
                                         uint64_t now) {
  bishfpm_store_count_t r =
      exact_text_store.update_text(state, key, n, hash, weight, now);
  r.known = 0;
  return r;
}
static void budget_caps(void) {
  bishfpm_config_t c = config();
  unsigned denied = 0, accepted = 0;
  for (size_t limit = 8192; limit <= 262144; limit += 512) {
    c.max_memory_bytes = limit;
    bishfpm_t *e = create(&c, BISHFPM_STORE_TEXT, 1, 16);
    if (!e)
      continue;
    int result = bishfpm_add_event(e, "g", 0, abc, 3, NULL, NULL);
    if (result < 0) {
      denied++;
      assert(bishfpm_group_rate(e, "g", 0) == 0);
      bishfpm_stats_t s;
      bishfpm_get_stats(e, &s);
      assert(s.active_groups == 0);
    } else {
      accepted++;
      assert(result == 3);
      /* maintenance must remain possible when replacement allocation is denied
       */
      assert(bishfpm_prune(e, 1000000, 1) == 1);
    }
    accounting(e);
    bishfpm_free(e);
  }
  assert(denied && accepted);
}
static void descriptors(void) {
  bishfpm_config_t c = config();
  exact_store_options_t raw = {4, BISHFPM_STORE_TEXT, "name"};
  bishfpm_store_engine_options_t o;
  bishfpm_store_engine_options_default(&o);
  for (unsigned i = 0; i < 12; i++) {
    bishfpm_store_descriptor_t d = exact_text_store;
    switch (i) {
    case 0:
      d.abi_version++;
      break;
    case 1:
      d.struct_size = offsetof(bishfpm_store_descriptor_t, snapshot);
      break;
    case 2:
      d.capabilities = 0;
      break;
    case 3:
      d.capabilities |= UINT64_C(1) << 63;
      break;
    case 4:
      d.reserved[0] = 1;
      break;
    case 5:
      d.prepare_event = NULL;
      break;
    case 6:
      d.cancel_event = NULL;
      break;
    case 7:
      d.clone_config = NULL;
      break;
    case 8:
      d.query_text = NULL;
      break;
    case 9:
      d.representation = 3;
      break;
    case 10:
      d.reserved0 = 1;
      break;
    case 11:
      d.capabilities |= BISHFPM_STORE_STABLE_IDS;
      break;
    }
    assert(!bishfpm_create_with_store(&c, &o, &d, &raw, sizeof(raw)));
  }
  struct {
    uint32_t version, size;
  } short_d = {1, sizeof(short_d)};
  assert(!bishfpm_create_with_store(
      &c, &o, (const bishfpm_store_descriptor_t *)&short_d, &raw, sizeof(raw)));
  o.struct_size = 8;
  assert(
      !bishfpm_create_with_store(&c, &o, &exact_text_store, &raw, sizeof(raw)));
  bishfpm_store_engine_options_default(&o);
  o.reserved[0] = 1;
  assert(
      !bishfpm_create_with_store(&c, &o, &exact_text_store, &raw, sizeof(raw)));
  o.reserved[0] = 0;
  bishfpm_store_descriptor_t unusual = exact_text_store;
  unusual.plan = unbounded_plan;
  assert(!bishfpm_create_with_store(&c, &o, &unusual, &raw, sizeof(raw)));
  o.memory_mode = BISHFPM_MEMORY_DYNAMIC;
  bishfpm_t *unbounded =
      bishfpm_create_with_store(&c, &o, &unusual, &raw, sizeof(raw));
  assert(unbounded);
  bishfpm_free(unbounded);
  o.memory_mode = BISHFPM_MEMORY_CONSERVATIVE;
  unusual = exact_text_store;
  unusual.update_text = uncertified;
  bishfpm_t *invalid_bounds =
      bishfpm_create_with_store(&c, &o, &unusual, &raw, sizeof(raw));
  assert(invalid_bounds);
  assert(bishfpm_add_event(invalid_bounds, "g", 0, ab, 2, NULL, NULL) == 0);
  bishfpm_free(invalid_bounds);
  /* mandatory v1 prefix is valid without the optional bounded-prune tail */
  union {
    max_align_t align;
    unsigned char data[BISHFPM_STORE_DESCRIPTOR_V1_SIZE];
  } prefix;
  bishfpm_store_descriptor_t short_valid = exact_text_store;
  short_valid.struct_size = sizeof(prefix.data);
  memcpy(prefix.data, &short_valid, sizeof(prefix.data));
  bishfpm_t *prefix_engine = bishfpm_create_with_store(
      &c, &o, (const bishfpm_store_descriptor_t *)prefix.data, &raw,
      sizeof(raw));
  assert(prefix_engine);
  bishfpm_free(prefix_engine);
  struct {
    bishfpm_store_descriptor_t d;
    uint64_t tail;
  } large;
  large.d = exact_text_store;
  large.d.struct_size = sizeof(large);
  large.tail = UINT64_MAX;
  char *name = malloc(32);
  assert(name);
  strcpy(name, "temporary pointer");
  raw.name = name;
  bishfpm_t *e = bishfpm_create_with_store(&c, &o, &large.d, &raw, sizeof(raw));
  assert(e);
  memset(&large, 0, sizeof(large));
  memset(name, 0, 32);
  free(name);
  memset(&raw, 0, sizeof(raw));
  assert(bishfpm_add_event(e, "g", 0, ab, 2, NULL, NULL) == 1);
  struct {
    bishfpm_store_description_t d;
    uint64_t tail;
  } desc = {0};
  desc.d.abi_version = 1;
  desc.d.struct_size = sizeof(desc);
  desc.tail = UINT64_MAX;
  assert(bishfpm_get_store_description(e, &desc.d));
  assert(desc.tail == UINT64_MAX);
  assert(desc.d.representation == BISHFPM_STORE_TEXT &&
         desc.d.plan.group_peak_bytes);
  accounting(e);
  bishfpm_free(e);
}

/* independent long-double decayed oracle; callbacks observe each intermediate
 * candidate, including candidates later in the same event */
static struct {
  bishfpm_t *engine;
  long double counts[64], weight;
  unsigned candidates[64], next, count;
  uint64_t now;
} oracle;
static void serialize(unsigned mask, char *out) {
  size_t length = 0;
  for (unsigned i = 0; i < 6; i++)
    if (mask & (1U << i)) {
      if (length)
        out[length++] = '\037';
      out[length++] = 'a' + i;
    }
  out[length] = 0;
}
static void oracle_callback(const char *group, const bishfpm_trigger_t *t,
                            void *ctx) {
  (void)ctx;
  assert(strcmp(group, "g") == 0);
  assert(oracle.next < oracle.count);
  unsigned mask = oracle.candidates[oracle.next++];
  oracle.counts[mask] += oracle.weight;
  char text[16];
  serialize(mask, text);
  assert(strcmp(t->pattern, text) == 0);
  close_to(t->pattern_rate, oracle.counts[mask]);
  for (unsigned i = 0; i < oracle.count; i++) {
    serialize(oracle.candidates[i], text);
    check_count(oracle.engine, text, oracle.now,
                oracle.counts[oracle.candidates[i]]);
  }
}
static void oracle_test(unsigned rep, int dynamic) {
  bishfpm_config_t c = config();
  if (rep == BISHFPM_STORE_TEXT)
    c.max_itemset_order = 3;
  memset(&oracle, 0, sizeof(oracle));
  oracle.engine = create(&c, rep, dynamic, 64);
  assert(oracle.engine);
  uint64_t last = 0;
  for (unsigned e = 0; e < 200; e++) {
    unsigned event_mask = (e * 37 % 63) + 1;
    const char *items[6];
    size_t count = 0;
    for (unsigned i = 0; i < 6; i++)
      if (event_mask & (1U << i))
        items[count++] = keys[i];
    if (count < 2)
      continue;
    uint64_t stamp = e * 31;
    if (e % 7 == 0 && stamp >= 101)
      stamp -= 101;
    oracle.now = stamp > last ? stamp : last;
    long double decay = exp2l(-(long double)(oracle.now - last) / 1000);
    for (unsigned i = 0; i < 64; i++)
      oracle.counts[i] *= decay;
    oracle.weight =
        (1 + e % 13) * exp2l(-(long double)(oracle.now - stamp) / 1000);
    oracle.count = oracle.next = 0;
    for (unsigned i = 0; i < 6; i++)
      for (unsigned j = i + 1; j < 6; j++) {
        unsigned mask = (1U << i) | (1U << j);
        if ((event_mask & mask) == mask)
          oracle.candidates[oracle.count++] = mask;
      }
    if (c.max_itemset_order == 3)
      for (unsigned i = 0; i < 6; i++)
        for (unsigned j = i + 1; j < 6; j++)
          for (unsigned k = j + 1; k < 6; k++) {
            unsigned mask = (1U << i) | (1U << j) | (1U << k);
            if ((event_mask & mask) == mask)
              oracle.candidates[oracle.count++] = mask;
          }
    assert(bishfpm_add_event_weighted(oracle.engine, "g", stamp, items, count,
                                      1 + e % 13, oracle_callback,
                                      NULL) == (int)oracle.count);
    assert(oracle.next == oracle.count);
    last = oracle.now;
    char text[16];
    for (unsigned i = 1; i < 64; i++) {
      serialize(i, text);
      unsigned bits = 0;
      for (unsigned m = i; m; m >>= 1)
        bits += m & 1;
      if (bits < 2 || bits > c.max_itemset_order)
        continue;
      check_count(oracle.engine, text, last + 1000, oracle.counts[i] / 2);
      check_count(oracle.engine, text, 0, oracle.counts[i]);
    }
  }
  accounting(oracle.engine);
  bishfpm_free(oracle.engine);
}
/* Synthetic results exercise the host policy independently of either sketch. */
static bishfpm_store_count_t sample;
static bool subset_evidence;
static bishfpm_store_count_t sample_update(void *s, const char *key, size_t n,
                                           uint64_t h, double w, uint64_t t) {
  (void)s;
  (void)key;
  (void)n;
  (void)h;
  (void)w;
  (void)t;
  return sample;
}
static bishfpm_store_count_t sample_query(const void *s, const char *key,
                                          size_t n, uint64_t h, uint64_t t) {
  (void)s;
  (void)key;
  (void)n;
  (void)h;
  (void)t;
  bishfpm_store_count_t r = sample;
  if (subset_evidence)
    r.lower = 2;
  return r;
}
static double callback_rate;
static void evidence_alert(const char *group, const bishfpm_trigger_t *t,
                           void *ctx) {
  (void)group;
  (*(unsigned *)ctx)++;
  callback_rate = t->pattern_rate;
  assert(isinf(t->pattern_upper_bound) && isinf(t->pattern_error));
}
static void evidence_policy(void) {
  bishfpm_config_t c = config();
  c.min_pattern_rate = 5;
  exact_store_options_t raw = {4, BISHFPM_STORE_TEXT, "name"};
  bishfpm_store_descriptor_t d = exact_text_store;
  d.capabilities = BISHFPM_STORE_ALERT_EVIDENCE | BISHFPM_STORE_RESIDENT_BOUNDS;
  assert(!bishfpm_create_with_store(&c, NULL, &d, &raw, sizeof(raw)));
  d.capabilities =
      BISHFPM_STORE_ALERT_EVIDENCE | BISHFPM_STORE_HEURISTIC_ESTIMATES;
  d.update_text = sample_update;
  d.query_text = sample_query;
  /* A large parent estimate must not suppress a child with more evidence. */
  c.max_itemset_order = 3;
  c.subsumption_ratio = 1;
  c.trigger_cooldown_ms = 100;
  sample = (bishfpm_store_count_t){5, 99, 1, 0, BISHFPM_UPDATE_RESIDENT, 0};
  subset_evidence = true;
  bishfpm_t *subset_engine =
      bishfpm_create_with_store(&c, NULL, &d, &raw, sizeof(raw));
  assert(subset_engine);
  assert(bishfpm_add_event(subset_engine, "g", 0, abc, 3, NULL, NULL) == 4);
  bishfpm_free(subset_engine);
  subset_evidence = false;
  c = config();
  c.min_pattern_rate = 5;
  for (unsigned legacy = 0; legacy < 2; legacy++) {
    if (legacy)
      d.capabilities = BISHFPM_STORE_HEURISTIC_ESTIMATES;
    bishfpm_t *e = bishfpm_create_with_store(&c, NULL, &d, &raw, sizeof(raw));
    assert(e);
    unsigned alerts = 0;
    sample = (bishfpm_store_count_t){legacy ? 0 : 4,          99, 1, 0,
                                     BISHFPM_UPDATE_RESIDENT, 0};
    assert(bishfpm_add_event(e, "g", 0, ab, 2, evidence_alert, &alerts) ==
           (legacy ? 1 : 0));
    assert(alerts == legacy);
    assert(bishfpm_pattern_rate(e, "g", "a\037b", 0) == 99);
    bishfpm_count_bounds_t b;
    assert(bishfpm_pattern_bounds(e, "g", "a\037b", 0, &b));
    assert(!b.known && b.resident && b.lower == 0 && isinf(b.upper));
    if (legacy)
      assert(callback_rate == 99);
    else {
      sample.lower = 5;
      assert(bishfpm_add_event(e, "g", 0, ab, 2, evidence_alert, &alerts) == 1);
      assert(callback_rate == 5 && alerts == 1);
    }
    const bishfpm_store_count_t invalid[] = {
        {NAN, 99, 1, 0, BISHFPM_UPDATE_RESIDENT, 0},
        {INFINITY, 99, 1, 0, BISHFPM_UPDATE_RESIDENT, 0},
        {-1, 99, 1, 0, BISHFPM_UPDATE_RESIDENT, 0},
        {5, NAN, 1, 0, BISHFPM_UPDATE_RESIDENT, 0},
        {5, 99, 0, 0, BISHFPM_UPDATE_FILTERED, 0},
        {5, 99, 1, 1, BISHFPM_UPDATE_RESIDENT, 0},
        {5, 99, 1, 0, BISHFPM_UPDATE_RESIDENT, 1}};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
      sample = invalid[i];
      assert(bishfpm_add_event(e, "g", 0, ab, 2, evidence_alert, &alerts) == 0);
    }
    if (legacy) {
      sample = (bishfpm_store_count_t){5, 99, 1, 0, BISHFPM_UPDATE_RESIDENT, 0};
      assert(bishfpm_add_event(e, "g", 0, ab, 2, NULL, NULL) == 0);
    }
    bishfpm_free(e);
  }
}

typedef enum {
  FAULT_NONE,
  FAULT_ALL_UNKNOWN,
  FAULT_REJECT_LEN,
  FAULT_ZERO_LOWER_UPDATE,
  FAULT_INTERVAL_PARTITION,
  FAULT_UNCERTAIN_CHILD,
  FAULT_HEURISTIC_CHILD,
  FAULT_UNKNOWN_PARENT_QUERY,
  FAULT_WIDE_PARENT_QUERY
} fault_mode_t;

typedef struct {
  fault_mode_t mode;
  size_t target_len;
  const char *match_key;
} fault_state_t;

static fault_state_t fault;

static bishfpm_store_count_t apply_fault(bishfpm_store_count_t r, size_t n,
                                         bool is_update) {
  switch (fault.mode) {
  case FAULT_ALL_UNKNOWN:
    r.lower = 0;
    r.known = 0;
    break;
  case FAULT_ZERO_LOWER_UPDATE:
    if (is_update && n == fault.target_len)
      r.lower = 0; /* retain count without creating an initial cooldown */
    break;
  case FAULT_INTERVAL_PARTITION:
    if (r.resident) {
      if (n == 5)
        r.lower = fmin(r.lower, .25);
      else if (n == 3)
        r.upper *= 10;
    }
    break;
  case FAULT_UNCERTAIN_CHILD:
    if (r.resident && n == 5)
      r.lower = fmin(r.lower, 10);
    break;
  case FAULT_HEURISTIC_CHILD:
    r.known = 0;
    r.lower = 0;
    if (r.resident && n == 5)
      r.upper = fmin(r.upper, 10);
    break;
  case FAULT_UNKNOWN_PARENT_QUERY:
    if (!is_update && r.resident && n == 3) {
      r.known = 0;
      r.lower = r.upper = 1000;
    }
    break;
  case FAULT_WIDE_PARENT_QUERY:
    if (!is_update && r.resident && n == 3)
      r.upper *= 10;
    break;
  default:
    break;
  }
  return r;
}

static bishfpm_store_count_t fault_update(void *state, const char *key,
                                          size_t n, uint64_t hash,
                                          double weight, uint64_t now) {
  if (fault.mode == FAULT_REJECT_LEN && weight > 0 && n == fault.target_len &&
      (!fault.match_key || !memcmp(key, fault.match_key, n))) {
    /* inject a guarded-update failure without rejecting the whole event */
    bishfpm_store_count_t r =
        exact_text_store.query_text(state, key, n, hash, now);
    r.outcome = BISHFPM_UPDATE_REJECTED;
    return r;
  }
  return apply_fault(
      exact_text_store.update_text(state, key, n, hash, weight, now), n, true);
}

static bishfpm_store_count_t fault_query(const void *state, const char *key,
                                         size_t n, uint64_t hash,
                                         uint64_t now) {
  return apply_fault(exact_text_store.query_text(state, key, n, hash, now), n,
                     false);
}

typedef struct {
  uint32_t order;
  const char *pattern;
  double lower, upper, error;
  unsigned calls;
} expect_alert_t;

static void expect_order_alert(const char *group, const bishfpm_trigger_t *t,
                               void *raw) {
  expect_alert_t *exp = raw;
  assert(!strcmp(group, "g"));
  if (t->item_count == exp->order) {
    exp->calls++;
    if (exp->pattern)
      assert(!strcmp(t->pattern, exp->pattern));
    close_to(t->pattern_rate, exp->lower);
    close_to(t->pattern_upper_bound, exp->upper);
    close_to(t->pattern_error, exp->error);
  }
}

static bishfpm_store_descriptor_t fault_descriptor(bool heuristic) {
  bishfpm_store_descriptor_t d = exact_text_store;
  if (heuristic)
    d.capabilities = BISHFPM_STORE_HEURISTIC_ESTIMATES;
  d.update_text = fault_update;
  d.query_text = fault_query;
  return d;
}

static void partition_plugin_policy(int dynamic) {
  bishfpm_config_t c = mining();
  c.min_lift = c.subsumption_ratio = 0;
  exact_store_options_t raw = {4, BISHFPM_STORE_TEXT, "partition evidence"};
  bishfpm_store_descriptor_t d = fault_descriptor(true);
  fault = (fault_state_t){FAULT_ALL_UNKNOWN, 0, NULL};
  bishfpm_t *e = create_with_options(&d, &c, &raw, sizeof(raw), dynamic);
  assert(e && bishfpm_set_min_partition_lift(e, 1.1));
  /* heuristic descriptor supplies no proof; candidate and subsets pass */
  assert(bishfpm_add_event_weighted(e, "g", 0, abc, 3, 3, heuristic_alert, e) ==
         4);
  assert(bishfpm_add_event(e, "g", 1000, abc, 3, heuristic_alert, e) == 4);
  bishfpm_partition_lift_stats_t quality;
  bishfpm_get_partition_lift_stats(e, &quality);
  assert(quality.candidates_checked == 2 && quality.candidates_unknown == 2 &&
         !quality.candidates_suppressed);
  accounting(e);
  bishfpm_free(e);

  d = fault_descriptor(false);
  e = create_with_options(&d, &c, &raw, sizeof(raw), dynamic);
  assert(e && bishfpm_set_min_partition_lift(e, 1.5));
  fault = (fault_state_t){FAULT_REJECT_LEN, 5, "a\037b\037c"};
  assert(bishfpm_add_event_weighted(e, "g", 0, abc, 3, 8, NULL, NULL) == 3);
  const char *background[] = {"d"};
  assert(bishfpm_add_event_weighted(e, "g", 0, background, 1, 8, NULL, NULL) ==
         0);
  fault.mode = FAULT_NONE;
  expect_alert_t alert_exp = {3, "a\037b\037c", 1, 1, 0, 0};
  /* at one half-life true ABC=5 and G=9 (lift 1.8); decayed omitted mass
   * keeps retained count 1 from giving false lift .36 */
  assert(bishfpm_add_event(e, "g", 1000, abc, 3, expect_order_alert,
                           &alert_exp) == 4);
  assert(alert_exp.calls == 1);
  check_count(e, "a\037b", 1000, 5);
  check_count(e, "a\037b\037c", 1000, 1);
  close_to(bishfpm_group_rate(e, "g", 1000), 9);
  bishfpm_get_partition_lift_stats(e, &quality);
  assert(quality.candidates_checked == 1 && !quality.candidates_suppressed);
  accounting(e);
  bishfpm_free(e);

  e = create_with_options(&d, &c, &raw, sizeof(raw), dynamic);
  assert(e && bishfpm_set_min_partition_lift(e, 1.5));
  fault.mode = FAULT_NONE;
  assert(bishfpm_add_event(e, "g", 0, abc, 3, NULL, NULL) == 3);
  assert(bishfpm_add_event_weighted(e, "g", 0, background, 1, 9, NULL, NULL) ==
         0);
  fault = (fault_state_t){FAULT_REJECT_LEN, 5, "a\037b\037c"};
  alert_exp = (expect_alert_t){3, "a\037b\037c", 1, 1, 0, 0};
  /* rejected resident update returns stale count 1; omitted weight 8 must be
   * recorded before notification so true lift 2 is not mistaken for 2/9 */
  assert(bishfpm_add_event_weighted(e, "g", 0, abc, 3, 8, expect_order_alert,
                                    &alert_exp) == 4);
  fault.mode = FAULT_NONE;
  assert(alert_exp.calls == 1);
  check_count(e, "a\037b", 0, 9);
  check_count(e, "a\037b\037c", 0, 1);
  close_to(bishfpm_group_rate(e, "g", 0), 18);
  bishfpm_get_partition_lift_stats(e, &quality);
  assert(quality.candidates_checked == 2 &&
         quality.candidates_suppressed == 1 && !quality.candidates_unknown);
  accounting(e);
  bishfpm_free(e);

  fault = (fault_state_t){FAULT_INTERVAL_PARTITION, 0, NULL};
  c.min_pattern_rate = .25;
  e = create_with_options(&d, &c, &raw, sizeof(raw), dynamic);
  assert(e && bishfpm_set_min_partition_lift(e, 2));
  assert(bishfpm_add_event(e, "g", 0, abc, 3, NULL, NULL) == 3);
  for (size_t i = 0; i < 3; i++)
    assert(bishfpm_add_event_weighted(e, "g", 0, &abc[i], 1, 18, NULL, NULL) ==
           0);
  assert(bishfpm_add_event_weighted(e, "g", 0, background, 1, 44, NULL, NULL) ==
         0);
  alert_exp = (expect_alert_t){3, "a\037b\037c", .25, 2, 1.75, 0};
  /* ABC in [.25, 2], pairs in [2, 20], singletons 20, G=100: conservative
   * split upper is 5 and survives threshold 2 */
  assert(bishfpm_add_event(e, "g", 0, abc, 3, expect_order_alert, &alert_exp) ==
         4);
  assert(alert_exp.calls == 1);
  close_to(bishfpm_group_rate(e, "g", 0), 100);
  bishfpm_get_partition_lift_stats(e, &quality);
  assert(quality.candidates_checked == 2 &&
         quality.candidates_suppressed == 1 && !quality.candidates_unknown);
  accounting(e);
  bishfpm_free(e);
  fault.mode = FAULT_NONE;
}

static void conservative_subsumption_policy(int dynamic) {
  bishfpm_config_t c = mining();
  c.min_pattern_rate = 5;
  c.min_lift = 0;
  c.subsumption_ratio = .85;
  c.trigger_cooldown_ms = 10000;
  exact_store_options_t raw = {4, BISHFPM_STORE_TEXT, "subsumption evidence"};
  bishfpm_store_descriptor_t d = fault_descriptor(false);
  fault = (fault_state_t){FAULT_UNCERTAIN_CHILD, 0, NULL};
  const char *background[] = {"d"};
  for (int enabled = 0; enabled < 2; enabled++) {
    bishfpm_t *e = create_with_options(&d, &c, &raw, sizeof(raw), dynamic);
    assert(e);
    if (enabled)
      assert(bishfpm_set_conservative_subsumption(e, 1));
    assert(bishfpm_add_event_weighted(e, "g", 0, background, 1, 900, NULL,
                                      NULL) == 0);
    assert(bishfpm_add_event_weighted(e, "g", 0, ab, 2, 10, NULL, NULL) == 1);
    expect_alert_t alert_exp = {3, NULL, 10, 90, 80, 0};
    /* true ABC/AB is .90; certified upper 90 prevents lower 10 from hiding */
    assert(bishfpm_add_event_weighted(e, "g", 0, abc, 3, 90, expect_order_alert,
                                      &alert_exp) == 2 + enabled);
    assert(alert_exp.calls == (unsigned)enabled);
    accounting(e);
    bishfpm_free(e);
  }
  fault.mode = FAULT_NONE;
}

static void conservative_subsumption_evidence(int dynamic) {
  bishfpm_config_t c = mining();
  c.min_pattern_rate = 5;
  c.min_lift = 0;
  c.subsumption_ratio = .85;
  c.trigger_cooldown_ms = 10000;
  exact_store_options_t raw = {4, BISHFPM_STORE_TEXT, "subsumption bounds"};
  const char *background[] = {"d"};
  const fault_mode_t modes[] = {FAULT_HEURISTIC_CHILD,
                                FAULT_UNKNOWN_PARENT_QUERY,
                                FAULT_WIDE_PARENT_QUERY};
  for (unsigned policy = 0; policy < 3; policy++) {
    bishfpm_store_descriptor_t d = fault_descriptor(policy == 0);
    fault = (fault_state_t){modes[policy], 0, NULL};
    bishfpm_t *e = create_with_options(&d, &c, &raw, sizeof(raw), dynamic);
    assert(e && bishfpm_set_conservative_subsumption(e, 1));
    assert(bishfpm_add_event_weighted(e, "g", 0, background, 1, 900, NULL,
                                      NULL) == 0);
    assert(bishfpm_add_event_weighted(e, "g", 0, ab, 2, 10, NULL, NULL) == 1);
    /* unknown candidate/parent bounds and wide parent uppers cannot suppress */
    assert(bishfpm_add_event_weighted(e, "g", 0, abc, 3, 90,
                                      policy == 0 ? heuristic_alert : NULL,
                                      e) == 3);
    accounting(e);
    bishfpm_free(e);
  }
  fault.mode = FAULT_NONE;
}

static void conservative_subsumption_rejection(int dynamic) {
  for (size_t order = 3; order <= 5; order += 2) {
    bishfpm_config_t c = mining();
    c.min_itemset_order = order - 1;
    c.max_itemset_order = order;
    c.min_lift = 0;
    c.subsumption_ratio = .85;
    c.trigger_cooldown_ms = 1000;
    exact_store_options_t raw = {order + 1, BISHFPM_STORE_TEXT,
                                 "rejected subsumption child"};
    bishfpm_store_descriptor_t d = fault_descriptor(false);
    size_t child_len = 2 * order - 1;
    fault = (fault_state_t){FAULT_ZERO_LOWER_UPDATE, child_len, NULL};
    bishfpm_t *e = create_with_options(&d, &c, &raw, sizeof(raw), dynamic);
    assert(e && bishfpm_set_conservative_subsumption(e, 1));
    assert(bishfpm_add_event(e, "g", 0, keys, order, NULL, NULL) == (int)order);
    fault.mode = FAULT_REJECT_LEN;
    expect_alert_t observed = {(uint32_t)order, NULL, 1, 1, 0, 0};
    /* parents reach 9 while child retains 1; rejected weight 8 enters bound */
    assert(bishfpm_add_event_weighted(e, "g", 0, keys, order, 8,
                                      expect_order_alert, &observed) == 1);
    assert(observed.calls == 1);
    assert(bishfpm_prune(e, 1000, .25) == 0);
    fault.mode = FAULT_NONE;
    observed = (expect_alert_t){(uint32_t)order, NULL, 1.5, 1.5, 0, 0};
    /* surviving-group prune retains decayed omitted mass 4 (full upper 5.5) */
    assert(bishfpm_add_event(e, "g", 1000, keys, order, expect_order_alert,
                             &observed) == (int)order + 1);
    assert(observed.calls == 1);
    close_to(bishfpm_group_rate(e, "g", 1000), 5.5);
    accounting(e);
    bishfpm_free(e);
  }
}
int main(void) {
  evidence_policy();
  for (int dynamic = 0; dynamic < 2; dynamic++) {
    conservative_subsumption_policy(dynamic);
    conservative_subsumption_evidence(dynamic);
    conservative_subsumption_rejection(dynamic);
    partition_plugin_policy(dynamic);
  }
  descriptors();
  budget_caps();
  for (unsigned rep = BISHFPM_STORE_TEXT; rep <= BISHFPM_STORE_PAIR64; rep++)
    for (int dynamic = 0; dynamic < 2; dynamic++) {
      failures(rep, dynamic);
      capacity_and_empty(rep, dynamic);
      oracle_test(rep, dynamic);
    }
  puts("test_store_plugin passed.");
}
