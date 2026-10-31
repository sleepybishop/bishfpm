/* Decayed SpaceSaving quality against an exact finite-universe oracle.
 * This evaluates a counter store, not complete miners or anomaly accuracy. */
#include "bishfpm.h"
#include "bishfpm_lowlevel.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define UNIVERSE 512
#define K 64
#define N 100000
static double ns(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t)) {
    perror("clock_gettime");
    exit(1);
  }
  return (double)t.tv_sec * 1e9 + t.tv_nsec;
}
static int desc(const void *a, const void *b) {
  const bishfpm_topk_item_t *x = a, *y = b;
  int order = (x->rate < y->rate) - (x->rate > y->rate);
  return order ? order : strcmp(x->pattern, y->pattern);
}
static void run(int mode) {
  char keys[UNIVERSE][24];
  double cdf[UNIVERSE], sum = 0;
  bishfpm_rate_t *truth[UNIVERSE];
  for (int i = 0; i < UNIVERSE; i++) {
    truth[i] = bishfpm_rate_new();
    assert(truth[i]);
    snprintf(keys[i], sizeof(keys[i]), "key-%04d", i);
    sum += 1.0 / pow(i + 1, 1.1);
    cdf[i] = sum;
  }
  for (int i = 0; i < UNIVERSE; i++)
    cdf[i] /= sum;
  bishfpm_topk_t *s = bishfpm_topk_new(K, 2000);
  assert(s);
  unsigned *sequence = malloc(N * sizeof(*sequence));
  assert(sequence);
  uint64_t rng = 42;
  for (int i = 0; i < N; i++) {
    double u = (bishfpm_splitmix64(&rng) >> 11) * 0x1.0p-53;
    unsigned key = 0;
    while (key < UNIVERSE - 1 && u >= cdf[key])
      key++;
    if (mode == 1 && i > N / 2)
      key = (key + UNIVERSE / 2) % UNIVERSE; /* drift */
    if (mode == 2)
      key = (unsigned)i % UNIVERSE; /* adversarial round robin */
    sequence[i] = key;
  }
  double t = ns();
  for (int i = 0; i < N; i++)
    bishfpm_topk_add(s, keys[sequence[i]], 1, 1 + i / 10);
  double elapsed = ns() - t;
  for (int i = 0; i < N; i++)
    bishfpm_rate_update(truth[sequence[i]], 1 + i / 10, 2000);
  uint64_t now = 1 + (N - 1) / 10;
  bishfpm_topk_item_t exact[UNIVERSE], top[K];
  for (int i = 0; i < UNIVERSE; i++) {
    memset(&exact[i], 0, sizeof(exact[i]));
    strcpy(exact[i].pattern, keys[i]);
    exact[i].rate = bishfpm_rate_query(truth[i], now, 2000);
  }
  qsort(exact, UNIVERSE, sizeof(*exact), desc);
  size_t count = bishfpm_topk_get_top(s, now, top, K);
  assert(count == K);
  unsigned hits = 0, tp = 0, fp = 0, fn = 0;
  double mae = 0, max_width = 0, threshold = 100;
  for (int i = 0; i < K; i++) {
    for (int j = 0; j < K; j++)
      if (!strcmp(top[i].pattern, exact[j].pattern))
        hits++;
    double width = top[i].error;
    if (width > max_width)
      max_width = width;
  }
  for (int i = 0; i < UNIVERSE; i++) {
    double actual = bishfpm_rate_query(truth[i], now, 2000);
    double estimate = bishfpm_topk_query(s, keys[i], now);
    double lower = bishfpm_topk_query_lower(s, keys[i], now);
    mae += fabs(estimate - actual); /* absent output is zero */
    if (estimate > 0) {
      assert(estimate + 1e-7 >= actual);
      assert(lower <= actual + 1e-7);
    }
    if (lower >= threshold) {
      if (actual >= threshold)
        tp++;
      else
        fp++;
    } else if (actual >= threshold)
      fn++;
  }
  size_t payload = bishfpm_topk_storage_bytes(s);
  printf("workload=%s N=%d universe=%d k=%d requested_payload_bytes=%zu "
         "update_mean_ns=%.1f returned_topk_overlap=%.4f all_key_MAE=%.4f "
         "threshold=%.0f TP=%u FP=%u FN=%u precision=%.4f recall=%.4f "
         "max_resident_interval_width=%.4f\n",
         mode == 0   ? "zipf"
         : mode == 1 ? "drift"
                     : "round_robin",
         N, UNIVERSE, K, payload, elapsed / N, (double)hits / K, mae / UNIVERSE,
         threshold, tp, fp, fn, tp + fp ? (double)tp / (tp + fp) : 1,
         tp + fn ? (double)tp / (tp + fn) : 1, max_width);
  for (int i = 0; i < UNIVERSE; i++)
    bishfpm_rate_free(truth[i]);
  free(sequence);
  bishfpm_topk_free(s);
}
int main(void) {
  puts("Counter-store quality only; exact oracle is untimed. No equal-budget "
       "miner speedup claim.");
  puts("Top-k overlap uses equal output sizes; oracle ties break by key order. "
       "Threshold decisions use the conservative lower "
       "count.");
  run(0);
  run(1);
  run(2);
  return 0;
}
