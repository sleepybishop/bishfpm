#include "bishfpm.h"
#include "bishfpm_hash.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Independent allocator accounting: no library-private layouts or ledger reads.
 */
typedef struct {
  void *p;
  size_t bytes;
} allocation_t;
static allocation_t allocations[65536];
static size_t live_bytes, peak_bytes, fail_after = SIZE_MAX;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
char *__real_strdup(const char *);
void __real_free(void *);
static int fail(void) {
  if (fail_after == SIZE_MAX)
    return 0;
  if (!fail_after)
    return 1;
  fail_after--;
  return 0;
}
static void *record(void *p, size_t bytes) {
  if (!p)
    return NULL;
  for (size_t i = 0; i < 65536; i++) {
    if (!allocations[i].p) {
      allocations[i] = (allocation_t){p, bytes};
      live_bytes += bytes;
      if (live_bytes > peak_bytes)
        peak_bytes = live_bytes;
      return p;
    }
  }
  abort();
}
void *__wrap_malloc(size_t n) {
  return fail() ? NULL : record(__real_malloc(n), n);
}
void *__wrap_calloc(size_t n, size_t size) {
  return fail() ? NULL : record(__real_calloc(n, size), n * size);
}
char *__wrap_strdup(const char *s) {
  return fail() ? NULL : record(__real_strdup(s), strlen(s) + 1);
}
void __wrap_free(void *p) {
  if (!p)
    return;
  for (size_t i = 0; i < 65536; i++) {
    if (allocations[i].p == p) {
      live_bytes -= allocations[i].bytes;
      allocations[i].p = NULL;
      __real_free(p);
      return;
    }
  }
  abort();
}
static void audit(bishfpm_t *e) {
  bishfpm_memory_stats_t m;
  bishfpm_stats_t s;
  bishfpm_get_memory_stats(e, &m);
  bishfpm_get_stats(e, &s);
  assert(m.current_bytes == live_bytes && s.memory_current_bytes == live_bytes);
  assert(m.peak_bytes == peak_bytes && m.peak_bytes <= m.limit_bytes);
  assert(m.current_bytes <= m.peak_bytes &&
         s.memory_bound_bytes == m.limit_bytes);
}
static void clean(void) {
  assert(live_bytes == 0);
  peak_bytes = 0;
}
static bishfpm_config_t config(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.max_groups = 1000;
  c.max_items_per_group = 4096;
  c.topk_k = 1;
  c.item_rate_threshold = c.item_share_threshold = c.promotion_fraction =
      c.min_pattern_rate = 0;
  c.subsumption_ratio = 0;
  return c;
}
static bishfpm_options_t dynamic(unsigned fss) {
  bishfpm_options_t o = {fss ? BISHFPM_STORE_FSS : BISHFPM_STORE_SS,
                         fss ? 17U : 0U, BISHFPM_MEMORY_DYNAMIC};
  return o;
}
static void growth_and_denial(void) {
  bishfpm_config_t c = config();
  c.max_memory_bytes = 128U << 10;
  c.min_itemset_order = c.max_itemset_order = 1;
  assert(!bishfpm_create(&c));
  bishfpm_options_t o = dynamic(0);
  bishfpm_t *e = bishfpm_create_with_options(&c, &o);
  assert(e);
  audit(e);
  size_t baseline = live_bytes;
  char names[512][16];
  const char *keys[512];
  for (unsigned i = 0; i < 512; i++) {
    snprintf(names[i], sizeof(names[i]), "key%03u", i);
    keys[i] = names[i];
  }
  int emitted = bishfpm_add_event(e, "g", 1000, keys, 256, NULL, NULL);
  assert(emitted > 0 && emitted < 256);
  audit(e);
  bishfpm_memory_stats_t memory;
  bishfpm_get_memory_stats(e, &memory);
  assert(memory.allocation_denials && memory.event_rejections == 0);
  assert(bishfpm_add_event(e, "g", 1000, keys, 1, NULL, NULL) == 0);
  assert(bishfpm_group_rate(e, "g", 1000) == 2);
  bishfpm_vocabulary_stats_t v;
  assert(bishfpm_get_vocabulary_stats(e, "g", &v));
  assert(v.retained_items == 256 && v.remaining_items == 3840 &&
         !v.lifetime_retention);
  double before = bishfpm_group_rate(e, "g", 1000);
  assert(bishfpm_add_event(e, "g", 100000, keys + 256, 256, NULL, NULL) == -1);
  assert(bishfpm_group_rate(e, "g", 1000) == before);
  assert(bishfpm_item_rate(e, "g", keys[256], 1000) == 0);
  assert(bishfpm_get_vocabulary_stats(e, "g", &v) && v.retained_items == 256);
  bishfpm_get_memory_stats(e, &memory);
  assert(memory.event_rejections == 1);
  bishfpm_rejection_stats_t r;
  bishfpm_get_rejection_stats(e, &r);
  assert(r.allocation_failure == 1);
  audit(e);
  /* An expired cooldown rebuild adds a spare; audit the transient overlap. */
  assert(bishfpm_prune(e, 11000, .001) == 0);
  audit(e);
  /* No allocation is required to retire a group, even with no spare budget. */
  fail_after = 0;
  assert(bishfpm_prune(e, 11000, 3) == 1);
  fail_after = SIZE_MAX;
  audit(e);
  assert(live_bytes == baseline);
  assert(!bishfpm_get_vocabulary_stats(e, "g", &v));
  bishfpm_free(e);
  clean();
}
static void group_tables_and_pruning(unsigned fss) {
  bishfpm_config_t c = config();
  c.max_memory_bytes = 4U << 20;
  bishfpm_options_t o = dynamic(fss);
  bishfpm_t *e = bishfpm_create_with_options(&c, &o);
  assert(e);
  const char *key[] = {"a"};
  char groups[100][32];
  unsigned nonce = 0;
  for (unsigned i = 0; i < 100; i++) {
    do {
      snprintf(groups[i], sizeof(groups[i]), "group%u", nonce++);
    } while ((bishfpm_hash_str(groups[i], c.hash_seed) & 255) != 255);
    assert(bishfpm_add_event(e, groups[i], 1000, key, 1, NULL, NULL) == 0);
    audit(e);
  }
  /* Keep half the groups, exercise a group-table rebuild and marginal pruning.
   */
  for (unsigned i = 0; i < 100; i += 2) {
    assert(bishfpm_add_event_weighted(e, groups[i], 1000, key, 1, 10, NULL,
                                      NULL) == 0);
  }
  fail_after = 0;
  assert(bishfpm_prune(e, 1000, 2) == 50);
  fail_after = SIZE_MAX;
  audit(e);
  for (unsigned i = 0; i < 100; i++)
    assert(bishfpm_group_rate(e, groups[i], 1000) == (i % 2 ? 0 : 11));
  assert(bishfpm_prune(e, 1000, 12) == 50);
  audit(e);
  bishfpm_free(e);
  clean();
}
static void numeric_and_vocabulary(void) {
  for (int kind = BISHFPM_PAIR_SS; kind <= BISHFPM_PAIR_FSS; kind++) {
    bishfpm_config_t c = config();
    c.max_memory_bytes = 256U << 10;
    c.min_itemset_order = c.max_itemset_order = 2;
    c.max_items_per_group = 3;
    bishfpm_pair_options_t pairs = {kind, 8,
                                    kind == BISHFPM_PAIR_FSS ? 17U : 0U, .1};
    bishfpm_options_t o = dynamic(0);
    bishfpm_t *e = bishfpm_create_pairs_with_options(&c, &pairs, &o);
    assert(e);
    const char *ab[] = {"a", "b"}, *other[] = {"background"};
    assert(bishfpm_add_event(e, "g", 0, ab, 2, NULL, NULL) >= 0);
    assert(bishfpm_add_event_weighted(e, "g", 0, other, 1, 10, NULL, NULL) ==
           0);
    audit(e);
    assert(bishfpm_prune(e, 0, 2) == 0);
    audit(e);
    bishfpm_vocabulary_stats_t v;
    assert(bishfpm_get_vocabulary_stats(e, "g", &v));
    assert(v.retained_items == 3 && v.remaining_items == 0 &&
           v.lifetime_retention);
    const char *new_key[] = {"new"};
    assert(bishfpm_add_event(e, "g", 0, new_key, 1, NULL, NULL) == -1);
    bishfpm_rejection_stats_t r;
    bishfpm_get_rejection_stats(e, &r);
    assert(r.marginal_limit == 1);
    bishfpm_memory_stats_t m;
    bishfpm_get_memory_stats(e, &m);
    assert(m.event_rejections == 0);
    bishfpm_vocabulary_stats_t unchanged = v;
    assert(!bishfpm_get_vocabulary_stats(e, "missing", &v));
    assert(v.retained_items == unchanged.retained_items &&
           v.item_limit == unchanged.item_limit &&
           v.remaining_items == unchanged.remaining_items &&
           v.lifetime_retention == unchanged.lifetime_retention);
    assert(bishfpm_prune(e, 10000, .01) == 0);
    audit(e);
    bishfpm_free(e);
    clean();
  }
}
static void marginal_prune_without_allocation(void) {
  bishfpm_config_t c = config();
  c.max_memory_bytes = 256U << 10;
  c.min_pattern_rate = 1e100;
  bishfpm_options_t o = dynamic(0);
  bishfpm_t *e = bishfpm_create_with_options(&c, &o);
  assert(e);
  char names[32][16];
  const char *keys[32];
  unsigned nonce = 0;
  for (unsigned i = 0; i < 32; i++) {
    do {
      snprintf(names[i], sizeof(names[i]), "key%u", nonce++);
    } while ((bishfpm_hash_str(names[i], c.hash_seed) & 63) != 63);
    keys[i] = names[i];
    assert(bishfpm_add_event_weighted(e, "g", 1000, keys + i, 1, i % 2 ? 1 : 10,
                                      NULL, NULL) == 0);
  }
  fail_after = 0;
  assert(bishfpm_prune(e, 1000, 2) == 0);
  fail_after = SIZE_MAX;
  audit(e);
  bishfpm_vocabulary_stats_t v;
  assert(bishfpm_get_vocabulary_stats(e, "g", &v) && v.retained_items == 16);
  for (unsigned i = 0; i < 32; i++)
    assert(bishfpm_item_rate(e, "g", keys[i], 1000) == (i % 2 ? 0 : 10));
  assert(bishfpm_add_event(e, "g", 1001, keys + 1, 1, NULL, NULL) == 0);
  audit(e);
  bishfpm_free(e);
  clean();
}

static void injected_failures(void) {
  for (int backend = -2; backend <= BISHFPM_PAIR_FSS; backend++) {
    bishfpm_config_t c = config();
    c.max_memory_bytes = 1U << 20;
    c.topk_k = 8;
    c.min_itemset_order = 2;
    c.max_itemset_order = backend < 0 ? 3 : 2;
    c.min_pattern_rate = 1e100;
    bishfpm_options_t o = dynamic(backend == -1);
    bishfpm_pair_options_t p = {
        backend < 0 ? BISHFPM_PAIR_SS : (bishfpm_pair_store_t)backend, 1024,
        backend == BISHFPM_PAIR_FSS ? 17U : 0U, .1};
    unsigned success = 0;
    for (unsigned site = 0; site < 32; site++) {
      fail_after = site;
      bishfpm_t *e = backend < 0
                         ? bishfpm_create_with_options(&c, &o)
                         : bishfpm_create_pairs_with_options(&c, &p, &o);
      fail_after = SIZE_MAX;
      if (e) {
        audit(e);
        bishfpm_free(e);
        success++;
      }
      clean();
      if (success)
        break;
    }
    assert(success);
    char names[34][16];
    const char *keys[34];
    for (unsigned i = 0; i < 34; i++) {
      snprintf(names[i], sizeof(names[i]), "key%03u", i);
      keys[i] = names[i];
    }
    for (unsigned seeded = 0; seeded < 2; seeded++) {
      success = 0;
      for (unsigned site = 0; site < 32; site++) {
        bishfpm_t *e = backend < 0
                           ? bishfpm_create_with_options(&c, &o)
                           : bishfpm_create_pairs_with_options(&c, &p, &o);
        assert(e);
        if (seeded)
          assert(bishfpm_add_event(e, "g", 1000, keys, 32, NULL, NULL) == 0);
        audit(e);
        bishfpm_count_bounds_t old_bounds;
        assert(bishfpm_pattern_bounds(e, "g", "key000\037key001", 1000,
                                      &old_bounds));
        fail_after = site;
        int result =
            bishfpm_add_event(e, "g", 100000, keys + 32, 2, NULL, NULL);
        fail_after = SIZE_MAX;
        if (result < 0) {
          bishfpm_count_bounds_t bounds;
          assert(bishfpm_pattern_bounds(e, "g", "key000\037key001", 1000,
                                        &bounds));
          assert(bounds.lower == old_bounds.lower &&
                 bounds.upper == old_bounds.upper &&
                 bounds.resident == old_bounds.resident &&
                 bounds.known == old_bounds.known);
          assert(bishfpm_group_rate(e, "g", 1000) == seeded);
          assert(bishfpm_item_rate(e, "g", keys[32], 1000) == 0);
          bishfpm_memory_stats_t m;
          bishfpm_get_memory_stats(e, &m);
          assert(m.event_rejections == 0); /* OS failure, not budget denial. */
          assert(bishfpm_add_event(e, "g", 1001, keys + 32, 2, NULL, NULL) ==
                 0);
        } else {
          success++;
        }
        audit(e);
        bishfpm_free(e);
        clean();
        if (success)
          break;
      }
      assert(success);
    }
  }
}
int main(void) {
  growth_and_denial();
  group_tables_and_pruning(0);
  group_tables_and_pruning(1);
  numeric_and_vocabulary();
  marginal_prune_without_allocation();
  injected_failures();
  puts("test_dynamic passed.");
}
