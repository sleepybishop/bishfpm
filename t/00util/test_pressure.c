#include "bishfpm.h"
#include "lib/internal.h"
#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_calloc(size_t, size_t);
static int deny_calloc;
void *__wrap_calloc(size_t n, size_t size) {
  return deny_calloc ? NULL : __real_calloc(n, size);
}

static bishfpm_pattern_pressure_t snapshot(bishfpm_t *e, const char *group) {
  bishfpm_pattern_pressure_t p;
  assert(bishfpm_get_pattern_pressure(e, group, &p));
  assert(p.candidates_evaluated == p.resident_updates + p.admissions +
                                       p.filtered_updates + p.rejected_updates +
                                       p.zero_weight_queries);
  assert(p.replacements <= p.admissions);
  assert(p.resident_patterns <= p.resident_capacity);
  return p;
}

static void stream(unsigned fss, unsigned dynamic, unsigned fallback) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.max_groups = 2;
  c.max_items_per_group = c.max_event_items = 16;
  c.topk_k = 1;
  c.ewma_half_life_ms = 1000;
  c.item_rate_threshold = c.item_share_threshold = 0;
  c.min_itemset_order = c.max_itemset_order = 2;
  c.min_pattern_rate = 100000;
  c.max_memory_bytes = 256U << 20;
  bishfpm_options_t options = {
      fss ? BISHFPM_STORE_FSS : BISHFPM_STORE_SS, fss ? 1 : 0,
      dynamic ? BISHFPM_MEMORY_DYNAMIC : BISHFPM_MEMORY_CONSERVATIVE};
  bishfpm_t *e = bishfpm_create_with_options(&c, &options);
  assert(e);
  bishfpm_pattern_pressure_t p = snapshot(e, NULL);
  assert(!p.candidates_evaluated && !p.resident_capacity);
  const char *ab[] = {"a", "b"}, *ac[] = {"a", "c"}, *bc[] = {"b", "c"};
  assert(bishfpm_add_event_weighted(e, "g", 0, ab, 2, 10, NULL, NULL) == 0);
  assert(bishfpm_add_event(e, "g", 0, ab, 2, NULL, NULL) == 0);
  assert(bishfpm_add_event(e, "g", 0, ac, 2, NULL, NULL) == 0);
  assert(bishfpm_add_event_weighted(e, "g", 0, ac, 2, 10, NULL, NULL) == 0);
  assert(bishfpm_add_event(e, "g", 0, bc, 2, NULL, NULL) == 0);
  p = snapshot(e, "g");
  assert(p.candidates_evaluated == 5 && p.resident_patterns == 1 &&
         p.resident_capacity == 1 && !p.rejected_updates);
  assert(p.resident_updates == (fss ? 1 : 2));
  assert(p.admissions == (fss ? 2 : 3));
  assert(p.replacements == (fss ? 1 : 2));
  assert(p.filtered_updates == (fss ? 2 : 0));
  bishfpm_stats_t stats;
  bishfpm_get_stats(e, &stats);
  assert(stats.candidates_evaluated == p.candidates_evaluated &&
         stats.pattern_replacements == p.replacements);
  assert(bishfpm_add_event(e, "h", 0, ab, 2, NULL, NULL) == 0);
  assert(bishfpm_add_event(e, NULL, 0, ab, 2, NULL, NULL) == -1);
  assert(bishfpm_add_event_weighted(e, "h", 0, ab, 2, 0, NULL, NULL) == 0);
  p = snapshot(e, NULL);
  assert(p.candidates_evaluated == 6 && p.resident_capacity == 2);
  deny_calloc = fallback;
  assert(bishfpm_prune(e, 10000000, .1) == 2);
  deny_calloc = 0;
  p = snapshot(e, NULL);
  assert(p.candidates_evaluated == 6 && !p.resident_patterns &&
         !p.resident_capacity);
  assert(bishfpm_add_event(e, "g", 10000000, ab, 2, NULL, NULL) == 0);
  p = snapshot(e, "g");
  assert(p.candidates_evaluated == 1 && p.admissions == 1);
  /* Very late positive input discounts to zero, unlike weight-zero no-ops. */
  assert(bishfpm_add_event(e, "g", 0, ac, 2, NULL, NULL) == 0);
  p = snapshot(e, "g");
  assert(p.candidates_evaluated == 2 && p.zero_weight_queries == 1);
  p = snapshot(e, NULL);
  assert(p.candidates_evaluated == 8 && p.resident_capacity == 1);
  /* Snapshot/invalid-group queries never mutate state or output on failure. */
  unsigned char saved[sizeof(p)];
  memset(&p, 0x5a, sizeof(p));
  memcpy(saved, &p, sizeof(p));
  assert(!bishfpm_get_pattern_pressure(e, "missing", &p));
  assert(!bishfpm_get_pattern_pressure(NULL, NULL, &p));
  assert(!bishfpm_get_pattern_pressure(e, NULL, NULL));
  assert(!memcmp(saved, &p, sizeof(p)));
  bishfpm_free(e);

  bishfpm_pair_options_t pairs = {BISHFPM_PAIR_SS, 1, 0, 0};
  e = bishfpm_create_pairs(&c, &pairs);
  assert(e && !bishfpm_get_pattern_pressure(e, NULL, &p));
  assert(!memcmp(saved, &p, sizeof(p)));
  bishfpm_free(e);
}

static void rejected_store_updates(void) {
  for (unsigned fss = 0; fss < 2; fss++) {
    bishfpm_topk_t *t =
        fss ? bishfpm_topk_new_fss(1, 1000, 1, 42) : bishfpm_topk_new(1, 1000);
    assert(t);
    assert(topk_add_result(t, "a", DBL_MAX, 0).accepted);
    assert(!topk_add_result(t, "a", DBL_MAX, 0).accepted);
    assert(!topk_add_result(t, "b", DBL_MAX, 0).accepted);
    assert(!topk_add_result(t, "a", -1, 0).accepted);
    bishfpm_topk_free(t);
  }
}

int main(void) {
  for (unsigned fss = 0; fss < 2; fss++) {
    stream(fss, 0, 0);
    stream(fss, 1, 0);
    stream(fss, 1, 1);
  }
  rejected_store_updates();
  puts("test_pressure passed.");
}
