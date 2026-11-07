#include "bishfpm.h"
#include "examples/exact_store.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
static void alert(const char *g, const bishfpm_trigger_t *t, void *ud) {
  (void)g;
  (void)ud;
  assert(isfinite(t->pattern_rate) && isfinite(t->group_rate));
  assert(t->pattern_rate >= 0 && t->rate_fraction <= 1.000001);
  assert(memchr(t->pattern, 0, sizeof(t->pattern)));
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (!size)
    return 0;
  unsigned selector = *data++;
  unsigned store = selector % 7;
  size--;
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.max_groups = 4;
  c.max_items_per_group = 64;
  c.topk_k = 8;
  c.max_event_items = 8;
  c.max_candidates_per_event = 128;
  c.item_rate_threshold = c.min_pattern_rate = c.promotion_fraction = 0;
  c.min_itemset_order = 1;
  c.max_itemset_order = 3;
  c.trigger_cooldown_ms = 10;
  c.subsumption_ratio = .8;
  bishfpm_options_t engine_options;
  bishfpm_options_default(&engine_options);
  if (selector & 128) {
    engine_options.memory_mode = BISHFPM_MEMORY_DYNAMIC;
    c.max_memory_bytes = 64U << 10;
  }
  bishfpm_t *e;
  if (store >= 5) {
    if (store == 6)
      c.min_itemset_order = c.max_itemset_order = 2;
    exact_store_options_t backend = {
        16, store == 5 ? BISHFPM_STORE_TEXT : BISHFPM_STORE_PAIR64, "fuzz"};
    bishfpm_store_engine_options_t options;
    bishfpm_store_engine_options_default(&options);
    options.memory_mode = engine_options.memory_mode;
    options.notification_capacity = c.topk_k;
    e = bishfpm_create_with_store(
        &c, &options, store == 5 ? &exact_text_store : &exact_pair_store,
        &backend, sizeof(backend));
  } else if (store >= 2) {
    c.min_itemset_order = c.max_itemset_order = 2;
    bishfpm_pair_options_t options = {store == 2   ? BISHFPM_PAIR_SS
                                      : store == 3 ? BISHFPM_PAIR_THRESHOLD
                                                   : BISHFPM_PAIR_FSS,
                                      16, store == 4 ? 64 : 0, .01};
    e = bishfpm_create_pairs_with_options(&c, &options, &engine_options);
  } else {
    if (store == 1) {
      engine_options.store = BISHFPM_STORE_FSS;
      engine_options.sketch_width = 17;
    }
    e = bishfpm_create_with_options(&c, &engine_options);
  }
  if (!e)
    return 0;
  uint64_t t = 0;
  while (size >= 4) {
    unsigned op = data[0], n = data[1] % 9, w = data[2];
    t += data[3];
    data += 4;
    size -= 4;
    char keys[8][32];
    const char *items[8];
    for (unsigned i = 0; i < n; i++) {
      size_t len = size ? data[0] % 31 : 0;
      if (len > size)
        len = size;
      memcpy(keys[i], data, len);
      keys[i][len] = 0;
      items[i] = keys[i];
      data += len;
      size -= len;
    }
    const char *group = (op & 1) ? "a" : "b";
    uint64_t event_time = (op & 2) ? t / 2 : t;
    if (store >= 2 && (op & 8))
      bishfpm_prune_pairs(e, group, event_time, 1 + op % 16);
    else if (op & 4)
      bishfpm_prune(e, event_time, .01);
    else
      bishfpm_add_event_weighted(e, group, event_time, items, n, w, alert,
                                 NULL);
    bishfpm_stats_t st;
    bishfpm_get_stats(e, &st);
    assert(st.memory_current_bytes <= st.memory_bound_bytes);
    assert(st.active_groups <= 4 &&
           st.memory_bound_bytes <= c.max_memory_bytes);
    assert(st.events_total ==
           st.events_accepted + st.events_rejected + st.events_noop);
    bishfpm_memory_stats_t memory;
    bishfpm_get_memory_stats(e, &memory);
    assert(memory.current_bytes == st.memory_current_bytes);
    if (memory.mode == BISHFPM_MEMORY_DYNAMIC)
      assert(memory.current_bytes <= memory.peak_bytes &&
             memory.peak_bytes <= memory.limit_bytes);
    bishfpm_rejection_stats_t reasons;
    bishfpm_get_rejection_stats(e, &reasons);
    assert(reasons.invalid_input + reasons.event_items_limit +
               reasons.pattern_length_limit + reasons.candidate_limit +
               reasons.group_limit + reasons.marginal_limit +
               reasons.allocation_failure + reasons.reentrant ==
           st.events_rejected);
  }
  bishfpm_free(e);
  return 0;
}
