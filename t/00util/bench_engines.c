/* deterministic semantic replay and isolated throughput for all built-ins;
 * compile this same source against the baseline and candidate libraries */
#ifdef BENCH_LEGACY
#include "bishfpm.h"
#else
#include "store_twins.h"
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t digest = UINT64_C(1469598103934665603);
static bishfpm_t *active;
static void mix(const void *p, size_t n) {
  const unsigned char *s = p;
  while (n--) {
    digest ^= *s++;
    digest *= UINT64_C(1099511628211);
  }
}
static void callback(const char *g, const bishfpm_trigger_t *t, void *unused) {
  (void)unused;
  mix(g, strlen(g));
  mix(t->pattern, strlen(t->pattern));
  mix(&t->pattern_rate, sizeof(double));
  mix(&t->pattern_upper_bound, sizeof(double));
  mix(&t->timestamp, sizeof(t->timestamp));
  /* query current and not-yet-evaluated candidates from the callback */
  const char *patterns[] = {t->pattern, "i00\037i01", "i06\037i07"};
  for (size_t i = 0; i < 3; i++) {
    bishfpm_count_bounds_t b;
    assert(bishfpm_pattern_bounds(active, g, patterns[i], t->timestamp, &b));
    mix(&b.lower, sizeof(double));
    mix(&b.upper, sizeof(double));
    mix(&b.known, sizeof(int));
    mix(&b.resident, sizeof(int));
  }
}
static int compare_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}
static double seconds(void) {
  struct timespec t;
  assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
  return t.tv_sec + t.tv_nsec / 1e9;
}
int main(int argc, char **argv) {
  unsigned kind = argc > 1 ? (unsigned)atoi(argv[1]) : 0;
  size_t n = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : 30000;
  int trace = argc > 3 && strcmp(argv[3], "trace") == 0;
  int dynamic = argc > 4 ? atoi(argv[4]) : 0;
  assert(kind < 5 && n > 0);
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.topk_k = 64;
  c.max_groups = 4;
  c.max_items_per_group = 128;
  c.max_event_items = 16;
  c.max_itemset_order = kind < 2 ? 3 : 2;
  c.item_rate_threshold = c.item_share_threshold = 0;
  c.min_pattern_rate = .25;
  c.promotion_fraction = .01;
  c.min_lift = 0;
  c.subsumption_ratio = .8;
  c.trigger_cooldown_ms = 17;
  bishfpm_options_t o;
  bishfpm_options_default(&o);
  o.memory_mode =
      dynamic ? BISHFPM_MEMORY_DYNAMIC : BISHFPM_MEMORY_CONSERVATIVE;
  bishfpm_pair_options_t p = {BISHFPM_PAIR_SS, 64, 0, .1};
  if (kind == 1) {
    o.store = BISHFPM_STORE_FSS;
    o.sketch_width = 97;
  }
  if (kind == 3)
    p.store = BISHFPM_PAIR_THRESHOLD;
  if (kind == 4) {
    p.store = BISHFPM_PAIR_FSS;
    p.sketch_width = 97;
  }
  active = kind < 2 ? bishfpm_create_with_options(&c, &o)
                    : bishfpm_create_pairs_with_options(&c, &p, &o);
#ifndef BENCH_LEGACY
  if (argc > 5 && atoi(argv[5])) {
    bishfpm_free(active);
    twin_options_t twin = {kind, 64, kind == 1 || kind == 4 ? 97 : 0, .1};
    bishfpm_store_engine_options_t extension;
    bishfpm_store_engine_options_default(&extension);
    extension.memory_mode = o.memory_mode;
    active = bishfpm_create_with_store(
        &c, &extension, kind < 2 ? &twin_text_store : &twin_pair_store, &twin,
        sizeof(twin));
  }
#endif
  assert(active);
  char keys[48][8];
  for (unsigned i = 0; i < 48; i++)
    snprintf(keys[i], sizeof(keys[i]), "i%02u", i);
  const char *items[8];
  double latency[4096];
  size_t samples = 0;
  double start = seconds();
  for (size_t e = 0; e < n; e++) {
    size_t width = 2 + e % 7;
    for (size_t i = 0; i < width; i++)
      items[i] = keys[(e * 13 + i * 7) % 48];
    uint64_t now = e * 3;
    if (e % 9 == 0 && now >= 11)
      now -= 11;
    double event_start = !trace && e % 64 == 0 ? seconds() : 0;
    int result =
        bishfpm_add_event_weighted(active, e % 2 ? "g" : "h", now, items, width,
                                   1 + e % 5, trace ? callback : NULL, NULL);
    if (!trace && e % 64 == 0 && samples < 4096)
      latency[samples++] = (seconds() - event_start) * 1e9;
    if (trace) {
      mix(&result, sizeof(result));
      for (size_t i = 0; i < width; i++) {
        double value = bishfpm_item_rate(active, "g", items[i], now);
        mix(&value, sizeof(value));
      }
      if (e % 71 == 0) {
        size_t removed = bishfpm_prune(active, now, .3);
        mix(&removed, sizeof(removed));
        removed = bishfpm_prune_pairs(active, "h", now, 3);
        mix(&removed, sizeof(removed));
      }
    }
  }
  double elapsed = seconds() - start;
  bishfpm_stats_t s;
  bishfpm_get_stats(active, &s);
  printf("kind=%u mode=%d events=%zu accepted=%" PRIu64 " rejected=%" PRIu64
         " candidates=%" PRIu64 " digest=%016" PRIx64,
         kind, dynamic, n, s.events_accepted, s.events_rejected,
         s.candidates_evaluated, digest);
  if (!trace)
    printf(" ns/event=%.3f ns/candidate=%.3f bytes=%zu", elapsed * 1e9 / n,
           elapsed * 1e9 /
               (s.candidates_evaluated ? s.candidates_evaluated : 1),
           s.memory_current_bytes);
  if (!trace && samples) {
    qsort(latency, samples, sizeof(*latency), compare_double);
    printf(" p50=%.3f p95=%.3f p99=%.3f", latency[samples / 2],
           latency[(samples - 1) * 95 / 100],
           latency[(samples - 1) * 99 / 100]);
  }
  puts("");
  bishfpm_free(active);
  return 0;
}
