/* Reproducible engine benchmark, with an independent full-stream count oracle.
 * Usage: bench_general [events [seed [capacity [workload]]]], or --sweep.
 * Timing includes callback validation.
 * Both modes receive the same events and the same pattern-store byte budget;
 * FSS pays for its sketch by using a smaller resident capacity. */
#include "bishfpm.h"
#include "bishfpm_lowlevel.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { ITEMS = 24, PATTERNS = 2300 };
typedef struct {
  uint32_t mask;
  char text[96];
  long double exact;
  int seen;
  int observed;
} pattern_t;
static pattern_t patterns[PATTERNS];
static int pair_index[ITEMS][ITEMS], triple_index[ITEMS][ITEMS][ITEMS];
static long double group_count;
static unsigned callbacks, false_confirmations;
static double ns(void) {
  struct timespec now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &now));
  return now.tv_sec * 1e9 + now.tv_nsec;
}
static unsigned random_next(unsigned *seed) {
  return *seed = *seed * 1664525U + 1013904223U;
}
static size_t store_bytes(unsigned k, unsigned width) {
  bishfpm_topk_t *t = width ? bishfpm_topk_new_fss(k, 137, width, 42)
                            : bishfpm_topk_new(k, 137);
  assert(t);
  size_t bytes = bishfpm_topk_storage_bytes(t);
  bishfpm_topk_free(t);
  return bytes;
}
static void on_alert(const char *group, const bishfpm_trigger_t *t, void *ud) {
  (void)group;
  (void)ud;
  unsigned ids[3], n = 0;
  const char *p = t->pattern;
  do {
    assert(n < 3);
    ids[n++] = (unsigned)(p[10] - '0') * 10 + (unsigned)(p[11] - '0');
    p = strchr(p, BISHFPM_ITEM_DELIM);
    if (p)
      p++;
  } while (p);
  assert(n == 2 || n == 3);
  int index = n == 2 ? pair_index[ids[0]][ids[1]]
                     : triple_index[ids[0]][ids[1]][ids[2]];
  pattern_t *pat = &patterns[index];
  long double tolerance = 2e-9L * fmaxl(1, pat->exact);
  if (t->pattern_rate > pat->exact + tolerance || pat->exact + tolerance < 10 ||
      pat->exact + tolerance < .25L * group_count)
    false_confirmations++;
  assert(t->pattern_error == t->pattern_upper_bound - t->pattern_rate);
  pat->seen = 1;
  callbacks++;
}
typedef enum { STABLE, BURST, CHURN } workload_t;
static const char *workload_names[] = {"stable", "burst", "churn"};

static uint32_t event_mask(workload_t workload, unsigned step, unsigned events,
                           unsigned *seed) {
  uint32_t mask = 0;
  unsigned start = workload == CHURN ? (step < events / 2 ? 5 : 12) : 5;
  unsigned draws = workload == CHURN ? 8 : 2;
  unsigned vocabulary = workload == CHURN ? 12 : 4;
  for (unsigned i = 0; i < draws; i++)
    mask |= 1U << (start + ((random_next(seed) >> 16) % vocabulary));
  if ((random_next(seed) >> 16) % 4)
    mask |= 3;
  if (workload != STABLE && step >= events / 3 && step < 2 * events / 3 &&
      (random_next(seed) >> 16) % 8)
    mask |= 28;
  return mask;
}

static void run(unsigned events, unsigned initial_seed, unsigned ss_capacity,
                workload_t workload, unsigned fss) {
  const size_t budget = store_bytes(ss_capacity, 0);
  unsigned capacity = ss_capacity;
  while (store_bytes(capacity, fss ? 4 * capacity : 0) +
             (fss ? sizeof(uint64_t) - 1 : 0) >
         budget)
    capacity--;
  unsigned width = fss ? 4 * capacity : 0;
  /* Spend remaining bytes on buckets, allowing the extra arena alignment
   * padding too, so FSS's total engine footprint cannot exceed SS's. */
  while (fss &&
         store_bytes(capacity, width + 1) + sizeof(uint64_t) - 1 <= budget)
    width++;
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.max_groups = 1;
  c.max_items_per_group = 64;
  c.max_memory_bytes = 8U << 20;
  c.item_rate_threshold = c.item_share_threshold = 0;
  c.min_pattern_rate = 10;
  c.promotion_fraction = .25;
  c.min_itemset_order = 2;
  c.max_itemset_order = 3;
  c.trigger_cooldown_ms = 0;
  c.ewma_half_life_ms = 137;
  c.topk_k = capacity;
  c.hash_seed = 42;
  bishfpm_options_t options = {fss ? BISHFPM_STORE_FSS : BISHFPM_STORE_SS,
                               width, BISHFPM_MEMORY_DYNAMIC};
  bishfpm_t *e = bishfpm_create_with_options(&c, &options);
  assert(e);
  char names[ITEMS][32];
  for (unsigned i = 0; i < ITEMS; i++)
    snprintf(names[i], sizeof(names[i]), "attribute_%02u=value", i);
  unsigned count = 0;
  for (unsigned i = 0; i < ITEMS; i++) {
    for (unsigned j = i + 1; j < ITEMS; j++) {
      pair_index[i][j] = (int)count;
      patterns[count].mask = (1U << i) | (1U << j);
      snprintf(patterns[count++].text, 96, "%.31s\037%.31s", names[i],
               names[j]);
      for (unsigned k = j + 1; k < ITEMS; k++) {
        triple_index[i][j][k] = (int)count;
        patterns[count].mask = (1U << i) | (1U << j) | (1U << k);
        snprintf(patterns[count++].text, 96, "%.31s\037%.31s\037%.31s",
                 names[i], names[j], names[k]);
      }
    }
  }
  assert(count == PATTERNS);
  for (unsigned i = 0; i < PATTERNS; i++) {
    patterns[i].exact = 0;
    patterns[i].observed = 0;
  }
  group_count = 0;
  callbacks = false_confirmations = 0;
  unsigned seed = initial_seed;
  uint64_t watermark = 0, eligible = 0, confirmed = 0, intervals = 0;
  uint64_t candidate_updates = 0;
  unsigned distinct_patterns = 0;
  int first_eligible[2] = {-1, -1}, first_alert[2] = {-1, -1};
  int targets[] = {pair_index[0][1], triple_index[2][3][4]};
  double elapsed = 0;
  for (unsigned step = 0; step < events; step++) {
    uint32_t mask = event_mask(workload, step, events, &seed);
    const char *items[ITEMS];
    size_t n = 0;
    for (unsigned i = 0; i < ITEMS; i++)
      if (mask & (1U << i))
        items[n++] = names[i];
    uint64_t timestamp = step > 10 && step % 37 == 0 ? step - 10 : step;
    uint64_t now = timestamp > watermark ? timestamp : watermark;
    unsigned weight = step % 31 ? 1 : 4;
    long double decay = exp2l(-(long double)(now - watermark) / 137);
    long double effective_weight =
        weight * exp2l(-(long double)(now - timestamp) / 137);
    group_count = group_count * decay + effective_weight;
    for (unsigned i = 0; i < PATTERNS; i++) {
      patterns[i].exact *= decay;
      if ((mask & patterns[i].mask) == patterns[i].mask) {
        patterns[i].exact += effective_weight;
        candidate_updates++;
        if (!patterns[i].observed) {
          patterns[i].observed = 1;
          distinct_patterns++;
        }
      }
      patterns[i].seen = 0;
    }
    double begin = ns();
    assert(bishfpm_add_event_weighted(e, "g", timestamp, items, n, weight,
                                      on_alert, NULL) >= 0);
    elapsed += ns() - begin;
    watermark = now;
    for (unsigned i = 0; i < PATTERNS; i++) {
      bishfpm_count_bounds_t bounds;
      assert(bishfpm_pattern_bounds(e, "g", patterns[i].text, now, &bounds));
      long double tolerance = 2e-9L * fmaxl(1, patterns[i].exact);
      if (bounds.known) {
        intervals++;
        assert(bounds.lower <= patterns[i].exact + tolerance);
        assert(bounds.upper + tolerance >= patterns[i].exact);
      }
      int qualifies =
          patterns[i].exact >= 10 && patterns[i].exact >= .25L * group_count;
      if (qualifies && (mask & patterns[i].mask) == patterns[i].mask) {
        eligible++;
        confirmed += patterns[i].seen;
      }
      for (unsigned target = 0; target < 2; target++) {
        if ((int)i == targets[target]) {
          if (qualifies && first_eligible[target] < 0)
            first_eligible[target] = (int)step;
          if (patterns[i].seen && first_alert[target] < 0)
            first_alert[target] = (int)step;
        }
      }
    }
    if (step % 503 == 0)
      bishfpm_prune(e, now, .1);
  }
  assert(!false_confirmations);
  bishfpm_stats_t stats;
  bishfpm_memory_stats_t memory;
  bishfpm_pattern_pressure_t pressure;
  bishfpm_get_stats(e, &stats);
  bishfpm_get_memory_stats(e, &memory);
  assert(bishfpm_get_pattern_pressure(e, NULL, &pressure));
  assert(pressure.candidates_evaluated == candidate_updates &&
         pressure.replacements == stats.pattern_replacements);
  printf("%s,%u,%u,%u,%zu,%zu,%.0f,%.6f,%llu,%llu,%u,%u,%d,%d,%llu,%zu,%zu,%u,"
         "%s,%u,%u,%llu,%d,%d,%llu,%llu,%llu,%llu,%llu,%zu,%zu\n",
         fss ? "fss" : "ss", events, initial_seed, capacity, budget,
         store_bytes(capacity, width), events * 1e9 / elapsed,
         eligible ? (double)confirmed / eligible : 0,
         (unsigned long long)eligible, (unsigned long long)intervals, callbacks,
         false_confirmations,
         first_alert[0] < 0 ? -1 : first_alert[0] - first_eligible[0],
         first_alert[1] < 0 ? -1 : first_alert[1] - first_eligible[1],
         (unsigned long long)stats.pattern_replacements, memory.current_bytes,
         memory.peak_bytes, width, workload_names[workload], ss_capacity,
         distinct_patterns, (unsigned long long)candidate_updates,
         first_eligible[0], first_eligible[1],
         (unsigned long long)pressure.resident_updates,
         (unsigned long long)pressure.admissions,
         (unsigned long long)pressure.filtered_updates,
         (unsigned long long)pressure.rejected_updates,
         (unsigned long long)pressure.zero_weight_queries,
         pressure.resident_patterns, pressure.resident_capacity);
  bishfpm_free(e);
}
static int parse_unsigned(const char *text, unsigned *out) {
  if (!text[0] || text[0] < '0' || text[0] > '9')
    return 0;
  char *end;
  errno = 0;
  unsigned long value = strtoul(text, &end, 10);
  if (errno || *end || value > UINT_MAX)
    return 0;
  *out = (unsigned)value;
  return 1;
}

int main(int argc, char **argv) {
  unsigned events = 3000, seed = 42, capacity = 64;
  workload_t workload = CHURN;
  bool sweep = argc > 1 && !strcmp(argv[1], "--sweep");
  unsigned offset = sweep ? 2 : 1;
  if (argc > (sweep ? 4 : 5) ||
      (argc > (int)offset && !parse_unsigned(argv[offset], &events)) ||
      (argc > (int)offset + 1 && !parse_unsigned(argv[offset + 1], &seed)) ||
      (!sweep && argc > 3 && !parse_unsigned(argv[3], &capacity)) ||
      events < 100 || events > 100000 || capacity < 16 || capacity > 4096)
    goto usage;
  if (!sweep && argc > 4) {
    for (workload = STABLE; workload <= CHURN; workload++)
      if (!strcmp(argv[4], workload_names[workload]))
        break;
    if (workload > CHURN)
      goto usage;
  }
  puts(
      "store,events,seed,capacity,store_budget,store_bytes,events_per_second,"
      "alert_recall,eligible_opportunities,certified_intervals,callbacks,"
      "false_confirmations,persistent_delay_events,burst_delay_events,"
      "replacements,current_bytes,peak_bytes,sketch_width,workload,ss_capacity,"
      "distinct_patterns,candidate_updates,persistent_first_eligible_event,"
      "burst_first_eligible_event,resident_updates,admissions,filtered_updates,"
      "rejected_updates,zero_weight_queries,resident_patterns,resident_"
      "capacity");
  if (sweep) {
    for (workload = STABLE; workload <= CHURN; workload++)
      for (capacity = 16; capacity <= 2048; capacity *= 2)
        for (unsigned fss = 0; fss < 2; fss++)
          run(events, seed, capacity, workload, fss);
  } else {
    run(events, seed, capacity, workload, 0);
    run(events, seed, capacity, workload, 1);
  }
  return 0;
usage:
  fprintf(stderr, "usage: bench_general [events:100..100000 [seed "
                  "[capacity:16..4096 [stable|burst|churn]]]]\n"
                  "       bench_general --sweep [events:100..100000 [seed]]\n");
  return 1;
}
