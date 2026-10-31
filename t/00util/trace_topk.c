/* Optional compatibility probe: compare stdout when linked against baseline
 * and candidate libraries on the same platform. Hashes public count bounds and
 * sorted resident snapshots after every update; no platform-specific golden
 * floating-point digest is imposed. */
#include "bishfpm_lowlevel.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t digest;
static void bytes(const void *p, size_t n) {
  const unsigned char *s = p;
  while (n--)
    digest = (digest ^ *s++) * UINT64_C(1099511628211);
}
static void snapshot(bishfpm_topk_t *t, char names[40][16], uint64_t now) {
  for (unsigned i = 0; i < 40; i++) {
    bishfpm_topk_bounds_t b;
    assert(bishfpm_topk_bounds(t, names[i], now, &b));
    bytes(&b.lower, sizeof(b.lower));
    bytes(&b.upper, sizeof(b.upper));
    bytes(&b.error, sizeof(b.error));
    bytes(&b.resident, sizeof(b.resident));
    bytes(&b.known, sizeof(b.known));
  }
  bishfpm_topk_item_t top[7];
  size_t n = bishfpm_topk_get_top(t, now, top, 7);
  bytes(&n, sizeof(n));
  for (size_t i = 0; i < n; i++) {
    bytes(top[i].pattern, strlen(top[i].pattern) + 1);
    bytes(&top[i].rate, sizeof(top[i].rate));
    bytes(&top[i].error, sizeof(top[i].error));
    bytes(&top[i].last_time_ms, sizeof(top[i].last_time_ms));
  }
}
int main(void) {
  unsigned widths[] = {0, 1, 17, 128};
  char names[40][16];
  for (unsigned i = 0; i < 40; i++)
    snprintf(names[i], 16, "a%02u\037b%02u", i, i);
  for (unsigned w = 0; w < 4; w++) {
    digest = UINT64_C(1469598103934665603);
    for (unsigned seed = 1; seed <= 3; seed++) {
      bishfpm_topk_t *t = widths[w]
                              ? bishfpm_topk_new_fss(7, 137, widths[w], seed)
                              : bishfpm_topk_new(7, 137);
      assert(t);
      unsigned rng = seed;
      for (unsigned step = 0; step < 3000; step++) {
        rng = rng * 1664525U + 1013904223U;
        unsigned id = step % 5 ? (rng >> 16) % 40 : step / 1000;
        uint64_t now = step * 3,
                 time = step > 15 && step % 17 == 0 ? now - 37 : now;
        double weight = ldexp(1.0, (int)(step % 47) - 23);
        double result = bishfpm_topk_add_weight(t, names[id], weight, time);
        bytes(&result, sizeof(result));
        if (step % 97 == 0)
          bishfpm_topk_prune(t, now, .0001);
        if (step % 131 == 0)
          bishfpm_topk_add_weight(t, names[id], 0, now + 10000);
        snapshot(t, names, now);
        if (step % 101 == 0)
          snapshot(t, names, now + 10000000);
        if (step == 2099) {
          bishfpm_topk_reset(t);
          assert(bishfpm_topk_try_set_seed(t, seed + 99));
        }
      }
      bishfpm_topk_reset(t);
      bishfpm_topk_add_weight(t, names[0], DBL_MAX, 0);
      bishfpm_topk_add_weight(t, names[1], DBL_MAX, 1);
      snapshot(t, names, 0);
      bishfpm_topk_free(t);
    }
    printf("width=%u digest=%016llx\n", widths[w], (unsigned long long)digest);
  }
}
