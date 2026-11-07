/* Same replay builds against perf's legacy store and the extracted descriptor.
 * Quantize estimates to 1e-7 before hashing to tolerate libm rounding
 * differences. */
#include "bishfpm_hash.h"
#include "bishfpm_heavykeeper.h"
#include "bishfpm_rate.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HK_PERF_REFERENCE
#include "bishfpm_topk.h"
#endif
static uint64_t digest = UINT64_C(1469598103934665603);
static void mix(uint64_t v) {
  for (unsigned i = 0; i < 8; i++) {
    digest ^= (v >> (i * 8)) & 255;
    digest *= UINT64_C(1099511628211);
  }
}
#ifndef HK_PERF_REFERENCE
static void *alloc(void *ctx, size_t n) {
  (void)ctx;
  return malloc(n);
}
static void *zero(void *ctx, size_t n, size_t z) {
  (void)ctx;
  return calloc(n, z);
}
static void release(void *ctx, void *p, size_t n) {
  (void)ctx;
  (void)n;
  free(p);
}
#endif
int main(int argc, char **argv) {
  unsigned depth = argc > 1 ? (unsigned)atoi(argv[1]) : 4;
  unsigned width = argc > 2 ? (unsigned)atoi(argv[2]) : 17;
#ifdef HK_PERF_REFERENCE
  bishfpm_heavykeeper_options_t options = {depth, width, .95};
  bishfpm_topk_t *state = bishfpm_topk_new_heavykeeper(7, 137, &options);
  assert(state);
  bishfpm_topk_set_seed(state, 42);
#else
  bishfpm_heavykeeper_options_t options = {7, depth, width, .95};
  const bishfpm_store_descriptor_t *d = &bishfpm_heavykeeper_store;
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.ewma_half_life_ms = 137;
  c.hash_seed = 42;
  bishfpm_store_host_t host = {NULL, alloc, zero, release};
  void *config = NULL, *state = NULL;
  assert(d->clone_config(&host, &c, &options, sizeof(options), &config));
  assert(d->create(&host, config, &state));
  uint64_t clock = 0;
#endif
  for (unsigned step = 0; step < 5000; step++) {
    char key[32];
    snprintf(key, sizeof(key), "k%u", step % 3 ? step % 31 : step / 1000);
    uint64_t stamp = step > 20 && step % 7 == 0 ? step - 20 : step;
    double weight = step % 13 ? 1 : 3.5;
#ifdef HK_PERF_REFERENCE
    double estimate = bishfpm_topk_add_weight(state, key, weight, stamp);
#else
    uint64_t now = stamp > clock ? stamp : clock;
    weight = bishfpm_rate_decay(weight, now - stamp, 137);
    bishfpm_store_count_t count = d->update_text(
        state, key, strlen(key), bishfpm_hash_str(key, 42), weight, now);
    assert(!count.known && count.lower == 0);
    double estimate = count.upper;
    clock = now;
#endif
    mix((uint64_t)llround(estimate * 1e7));
    if (step % 79 == 0) {
#ifdef HK_PERF_REFERENCE
      mix(bishfpm_topk_prune(state, step, .8));
      mix(bishfpm_topk_count(state));
#else
      mix(d->prune(state, step, .8));
      clock = step;
      bishfpm_store_snapshot_t snap;
      d->snapshot(state, &snap);
      mix(snap.resident_patterns);
#endif
      for (unsigned k = 0; k < 31; k++) {
        snprintf(key, sizeof(key), "k%u", k);
#ifdef HK_PERF_REFERENCE
        double value = bishfpm_topk_estimate(state, key, step + 137);
#else
        double value = d->query_text(state, key, strlen(key),
                                     bishfpm_hash_str(key, 42), step + 137)
                           .upper;
#endif
        mix((uint64_t)llround(value * 1e7));
      }
    }
  }
#ifdef HK_PERF_REFERENCE
  bishfpm_topk_free(state);
#else
  d->destroy(&host, state);
  d->destroy_config(&host, config);
#endif
  printf("depth=%u width=%u digest=%016llx\n", depth, width,
         (unsigned long long)digest);
  return 0;
}
