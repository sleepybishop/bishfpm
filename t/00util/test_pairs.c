#include "bishfpm_pairs.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void boundaries(void) {
  assert(!bishfpm_pair_counter_new(UINT32_MAX, SIZE_MAX));
  assert(!bishfpm_pair_counter_new(65536, 1024));
  assert(!bishfpm_pair_counter_new(0, 0));
  assert(bishfpm_pair_counter_add(NULL, NULL, 0, 0) == -1);
  assert(bishfpm_pair_counter_get(NULL, 0, 1) == 0);
  assert(bishfpm_pair_counter_total(NULL) == 0);
  assert(bishfpm_pair_counter_storage_bytes(NULL) == 0);
  bishfpm_pair_counter_reset(NULL);
  bishfpm_pair_counter_free(NULL);
  for (uint32_t items = 0; items <= 2; items++) {
    bishfpm_pair_counter_t *c = bishfpm_pair_counter_new(items, 4096);
    assert(c);
    size_t bytes = bishfpm_pair_counter_storage_bytes(c);
    assert(!bishfpm_pair_counter_new(items, bytes - 1));
    assert(bishfpm_pair_counter_add(c, NULL, 0, 7) == 0);
    assert(bishfpm_pair_counter_total(c) == 7);
    bishfpm_pair_counter_free(c);
  }
  bishfpm_pair_counter_t *c = bishfpm_pair_counter_new(2, 4096);
  assert(c);
  uint32_t pair[] = {0, 1};
  assert(bishfpm_pair_counter_add(c, pair, 2, UINT64_MAX) == 0);
  assert(bishfpm_pair_counter_get(c, 0, 1) == UINT64_MAX);
  assert(bishfpm_pair_counter_add(c, pair, 2, 1) == -1);
  assert(bishfpm_pair_counter_add(c, pair, 2, 0) == 0);
  assert(bishfpm_pair_counter_total(c) == UINT64_MAX);
  assert(bishfpm_pair_counter_get(c, 0, 1) == UINT64_MAX);
  bishfpm_pair_counter_reset(c);
  assert(bishfpm_pair_counter_get(c, 0, 1) == 0);
  assert(bishfpm_pair_counter_total(c) == 0);
  bishfpm_pair_counter_free(c);
}

static void oracle(void) {
  enum { N = 37 };
  uint64_t exact[N][N] = {{0}}, total = 0;
  uint32_t ids[N], rng = 42;
  bishfpm_pair_counter_t *c = bishfpm_pair_counter_new(N, 1024 * 1024);
  assert(c);
  for (unsigned step = 0; step < 2000; step++) {
    size_t count = 0;
    for (uint32_t i = 0; i < N; i++) {
      rng = rng * 1664525U + 1013904223U;
      if (rng % 5 == 0)
        ids[count++] = i;
    }
    uint64_t weight = rng % 13;
    assert(bishfpm_pair_counter_add(c, ids, count, weight) == 0);
    total += weight;
    for (size_t i = 0; i < count; i++)
      for (size_t j = i + 1; j < count; j++)
        exact[ids[i]][ids[j]] += weight;
  }
  uint32_t duplicate[] = {1, 1}, unsorted[] = {2, 1}, outside[] = {0, N};
  assert(bishfpm_pair_counter_add(c, duplicate, 2, 3) == -1);
  assert(bishfpm_pair_counter_add(c, unsorted, 2, 0) == -1);
  assert(bishfpm_pair_counter_add(c, outside, 2, 3) == -1);
  assert(bishfpm_pair_counter_add(c, NULL, 1, 3) == -1);
  assert(bishfpm_pair_counter_add(c, ids, N + 1, 3) == -1);
  assert(bishfpm_pair_counter_total(c) == total);
  for (uint32_t i = 0; i < N; i++) {
    assert(bishfpm_pair_counter_get(c, i, i) == 0);
    assert(bishfpm_pair_counter_get(c, i, N) == 0);
    for (uint32_t j = i + 1; j < N; j++) {
      assert(bishfpm_pair_counter_get(c, i, j) == exact[i][j]);
      assert(bishfpm_pair_counter_get(c, j, i) == exact[i][j]);
    }
  }
  bishfpm_pair_counter_free(c);
}
int main(void) {
  boundaries();
  oracle();
  puts("test_pairs passed.");
  return 0;
}
