#include "bishfpm.h"
#include "bishfpm_lowlevel.h"
#include "lib/internal.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Oracle arithmetic is independent of bishfpm_rate_decay and uses long double.
 */
static void encloses(bishfpm_topk_bounds_t b, long double exact) {
  long double tolerance = 2e-10L * fmaxl(1, exact);
  assert(b.lower >= 0 && b.upper >= b.lower && b.error == b.upper - b.lower);
  if (b.known) {
    assert(b.lower <= exact + tolerance);
    assert(b.upper + tolerance >= exact);
  }
}
static void stream_oracle(unsigned width, bool arena_backed) {
  enum { N = 71 };
  char patterns[N][32];
  for (unsigned i = 0; i < N; i++)
    snprintf(patterns[i], sizeof(patterns[i]), "a%02u\037b%02u\037c%02u", i, i,
             i);
  topk_layout_t layout;
  assert(topk_layout_configured(7, 137, width, &layout));
  unsigned char buffer[16384];
  assert(layout.arena_bytes + 1 <= sizeof(buffer));
  bishfpm_arena_t *a =
      arena_backed ? bishfpm_arena_new_buffer(buffer + 1, layout.arena_bytes)
                   : NULL;
  bishfpm_topk_t *t =
      arena_backed ? (width ? bishfpm_topk_new_fss_arena(a, 7, 137, width, 42)
                            : bishfpm_topk_new_arena(a, 7, 137))
                   : (width ? bishfpm_topk_new_fss(7, 137, width, 42)
                            : bishfpm_topk_new(7, 137));
  assert(t && bishfpm_topk_try_set_seed(t, 42));
  assert(bishfpm_topk_storage_bytes(t) == sizeof(*t) + layout.heap_bytes +
                                              layout.map_bytes +
                                              layout.sketch_bytes);
  long double exact[N] = {0};
  uint64_t watermark = 0;
  unsigned rng = 71;
  for (unsigned step = 0; step < 4000; step++) {
    rng = rng * 1664525U + 1013904223U;
    unsigned id = step % 4 ? rng % N : step / 1000;
    uint64_t timestamp = step > 17 && step % 13 == 0 ? step - 17 : step;
    uint64_t now = timestamp > watermark ? timestamp : watermark;
    long double decay = exp2l(-(long double)(now - watermark) / 137);
    for (unsigned i = 0; i < N; i++)
      exact[i] *= decay;
    double weight = step % 11 ? 1 : 3.5;
    exact[id] += weight * exp2l(-(long double)(now - timestamp) / 137);
    topk_result_t add = topk_add_result(t, patterns[id], weight, timestamp);
    assert(add.hash == bishfpm_hash_str(patterns[id], 42));
    if (width)
      assert(!bishfpm_topk_try_set_seed(t, 99));
    watermark = now;
    if (step % 61 == 0)
      bishfpm_topk_prune(t, now, 4);
    for (unsigned i = 0; i < N; i++) {
      bishfpm_topk_bounds_t b;
      assert(bishfpm_topk_bounds(t, patterns[i], now, &b));
      assert(!width || b.known);
      encloses(b, exact[i]);
    }
    if (step % 127 == 0) {
      uint64_t before = t->max_time_ms;
      bishfpm_topk_add_weight(t, patterns[id], 0, now + 1000);
      assert(t->max_time_ms == before);
    }
  }
  bishfpm_topk_reset(t);
  assert(bishfpm_topk_try_set_seed(t, 99));
  for (unsigned i = 0; i < N; i++) {
    bishfpm_topk_bounds_t b;
    assert(bishfpm_topk_bounds(t, patterns[i], 0, &b));
    assert(b.upper == 0 && !b.resident);
  }
  bishfpm_topk_free(t);
  if (a)
    bishfpm_arena_free(a);
}

static void lifecycle(void) {
  bishfpm_config_t cfg;
  bishfpm_config_default(&cfg);
  cfg.max_groups = 1;
  bishfpm_options_t options;
  bishfpm_options_default(&options);
  assert(options.store == BISHFPM_STORE_SS && options.sketch_width == 0 &&
         options.memory_mode == BISHFPM_MEMORY_CONSERVATIVE);
  options.store = BISHFPM_STORE_FSS;
  assert(!bishfpm_create_with_options(&cfg, &options));
  options.sketch_width = (1U << 24) + 1;
  assert(!bishfpm_create_with_options(&cfg, &options));
  options.sketch_width = 17;
  options.memory_mode = (bishfpm_memory_mode_t)99;
  assert(!bishfpm_create_with_options(&cfg, &options));
  options.memory_mode = BISHFPM_MEMORY_DYNAMIC;
  options.store = (bishfpm_store_t)99;
  assert(!bishfpm_create_with_options(&cfg, &options));
  options.store = BISHFPM_STORE_FSS;
  cfg.max_memory_bytes = 1;
  assert(!bishfpm_create_with_options(&cfg, &options));
  cfg.max_memory_bytes = 1U << 20;
  cfg.min_itemset_order = cfg.max_itemset_order = 2;
  bishfpm_pair_options_t pairs = {BISHFPM_PAIR_SS, 8, 0, 0};
  assert(!bishfpm_create_pairs_with_options(&cfg, &pairs, &options));

  assert(!bishfpm_topk_new_fss(4, 1000, 0, 0));
  assert(!bishfpm_topk_new_fss(4, 1000, (1U << 24) + 1, 0));
  assert(!bishfpm_topk_new_fss(0, 1000, 17, 0));
  assert(!bishfpm_topk_new_fss(4, NAN, 17, 0));
  bishfpm_topk_t *t = bishfpm_topk_new_fss(1, 1000, 1, 42);
  assert(t);
  assert(bishfpm_topk_add_weight(t, "a", 1e17, 0) == 1e17);
  assert(bishfpm_topk_prune(t, 0, 2e17) == 1);
  assert(!bishfpm_topk_try_set_seed(t, 99));
  bishfpm_topk_set_seed(t, 99);
  assert(t->hash_seed == 42);
  topk_result_t r = topk_add_result(t, "b", 1, 0);
  assert(r.present && r.lower == 0 && r.error == r.upper);
  bishfpm_topk_bounds_t b;
  assert(bishfpm_topk_bounds(t, "b", 0, &b) && b.lower == 0);
  bishfpm_topk_reset(t);
  assert(bishfpm_topk_add_weight(t, "a", DBL_MAX, 0) == DBL_MAX);
  assert(bishfpm_topk_add_weight(t, "b", DBL_MAX, 1) == 0);
  assert(t->max_time_ms == 0 && t->sketch_total == DBL_MAX);
  bishfpm_topk_bounds_t unchanged = {1, 2, 1, 1, 1}, copy = unchanged;
  assert(!bishfpm_topk_bounds(t, "", 0, &unchanged));
  assert(!memcmp(&copy, &unchanged, sizeof(copy)));
  bishfpm_topk_free(t);
  unsigned char buf[64];
  bishfpm_arena_t *a = bishfpm_arena_new_buffer(buf + 1, sizeof(buf) - 1);
  assert(a);
  size_t used = bishfpm_arena_used(a);
  assert(!bishfpm_topk_new_fss_arena(a, 8, 1000, 17, 0));
  assert(bishfpm_arena_used(a) == used);
  bishfpm_arena_free(a);
}

static unsigned order(unsigned mask) {
  unsigned count = 0;
  for (; mask; mask >>= 1)
    count += mask & 1;
  return count;
}
static void pattern(unsigned mask, char *out) {
  size_t n = 0;
  for (unsigned i = 0; i < 8; i++) {
    if (mask & (1U << i)) {
      if (n)
        out[n++] = BISHFPM_ITEM_DELIM;
      out[n++] = 'a' + i;
    }
  }
  out[n] = 0;
}
typedef struct {
  bishfpm_t *engine;
  long double exact[256];
  unsigned calls;
} oracle_t;
static void alert(const char *group, const bishfpm_trigger_t *trig, void *ud) {
  oracle_t *o = ud;
  unsigned mask = 0;
  for (const char *p = trig->pattern; *p; p++)
    if (*p != BISHFPM_ITEM_DELIM)
      mask |= 1U << (*p - 'a');
  assert(trig->pattern_rate >= 2);
  assert(trig->pattern_rate <= o->exact[mask] + 1e-8L);
  bishfpm_count_bounds_t b;
  assert(bishfpm_pattern_bounds(o->engine, group, trig->pattern,
                                trig->timestamp, &b));
  assert(b.resident && b.known && b.lower == trig->pattern_rate &&
         b.upper == trig->pattern_upper_bound);
  o->calls++;
}
static void engine_oracle(void) {
  for (unsigned fss = 0; fss < 2; fss++) {
    bishfpm_config_t c;
    bishfpm_config_default(&c);
    c.max_groups = 1;
    c.item_rate_threshold = c.item_share_threshold = c.promotion_fraction = 0;
    c.min_pattern_rate = 2;
    c.ewma_half_life_ms = 137;
    c.topk_k = 11;
    c.trigger_cooldown_ms = 0;
    bishfpm_options_t opts = {fss ? BISHFPM_STORE_FSS : BISHFPM_STORE_SS,
                              fss ? 17U : 0U, BISHFPM_MEMORY_CONSERVATIVE};
    oracle_t o = {0};
    o.engine = bishfpm_create_with_options(&c, &opts);
    assert(o.engine);
    const char *names[] = {"a", "b", "c", "d", "e", "f", "g", "h"};
    uint64_t watermark = 0;
    unsigned rng = 17;
    for (unsigned step = 0; step < 1600; step++) {
      rng = rng * 1664525U + 1013904223U;
      unsigned event = step % 4 ? (rng >> 16) & 255 : 7;
      if (order(event) > 5)
        event &= 31;
      const char *items[9];
      size_t n = 0;
      for (int i = 7; i >= 0; i--)
        if (event & (1U << i))
          items[n++] = names[i];
      if (n)
        items[n] = items[0], n++;
      unsigned weight = step % 19 ? 1 : 7;
      uint64_t timestamp = step > 20 && step % 7 == 0 ? step - 20 : step;
      uint64_t now = n && timestamp > watermark ? timestamp : watermark;
      for (unsigned mask = 0; mask < 256; mask++) {
        o.exact[mask] *= exp2l(-(long double)(now - watermark) / 137);
        if ((event & mask) == mask && order(mask) >= 2 && order(mask) <= 3 && n)
          o.exact[mask] +=
              weight * exp2l(-(long double)(now - timestamp) / 137);
      }
      assert(bishfpm_add_event_weighted(o.engine, "g", timestamp, items, n,
                                        weight, alert, &o) >= 0);
      if (n)
        watermark = now;
      if (step % 89 == 0)
        bishfpm_prune(o.engine, watermark, .1);
      for (unsigned mask = 0; mask < 256; mask++) {
        if (order(mask) < 2 || order(mask) > 3)
          continue;
        char key[32];
        pattern(mask, key);
        bishfpm_count_bounds_t b;
        assert(bishfpm_pattern_bounds(o.engine, "g", key, watermark, &b));
        long double tolerance = 2e-9L * fmaxl(1, o.exact[mask]);
        assert(!fss || b.known);
        if (b.known) {
          assert(b.lower <= o.exact[mask] + tolerance);
          assert(b.upper + tolerance >= o.exact[mask]);
        }
      }
    }
    assert(o.calls > 0);
    bishfpm_free(o.engine);
  }
}
int main(void) {
  lifecycle();
  stream_oracle(0, false);
  stream_oracle(1, false);
  stream_oracle(17, false);
  stream_oracle(18, true);
  engine_oracle();
  puts("test_general_fss passed.");
}
