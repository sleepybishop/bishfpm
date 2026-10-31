#include "bishfpm_pairs.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct bishfpm_pair_counter {
  uint32_t items;
  size_t pairs, storage_bytes;
  uint64_t total;
  uint64_t *counts;
};

/* Product fits uint64_t for every uint32_t n. */
static uint64_t triangle(uint32_t n) {
  return n ? (uint64_t)n * (n - 1) / 2 : 0;
}

bishfpm_pair_counter_t *bishfpm_pair_counter_new(uint32_t items,
                                                 size_t max_memory_bytes) {
  uint64_t pairs = triangle(items);
  if (pairs > (SIZE_MAX - sizeof(bishfpm_pair_counter_t)) / sizeof(uint64_t))
    return NULL;
  size_t bytes =
      sizeof(bishfpm_pair_counter_t) + (size_t)pairs * sizeof(uint64_t);
  if (bytes > max_memory_bytes || bytes > PTRDIFF_MAX)
    return NULL;
  bishfpm_pair_counter_t *c = calloc(1, sizeof(*c));
  if (!c)
    return NULL;
  c->items = items;
  c->pairs = (size_t)pairs;
  c->storage_bytes = bytes;
  if (pairs) {
    c->counts = calloc((size_t)pairs, sizeof(*c->counts));
    if (!c->counts) {
      free(c);
      return NULL;
    }
  }
  return c;
}

void bishfpm_pair_counter_free(bishfpm_pair_counter_t *c) {
  if (c) {
    free(c->counts);
    free(c);
  }
}

void bishfpm_pair_counter_reset(bishfpm_pair_counter_t *c) {
  if (c) {
    if (c->pairs)
      memset(c->counts, 0, c->pairs * sizeof(*c->counts));
    c->total = 0;
  }
}

size_t bishfpm_pair_counter_storage_bytes(const bishfpm_pair_counter_t *c) {
  return c ? c->storage_bytes : 0;
}

int bishfpm_pair_counter_add(bishfpm_pair_counter_t *c, const uint32_t *ids,
                             size_t count, uint64_t weight) {
  if (!c || (count && !ids) || count > c->items ||
      weight > UINT64_MAX - c->total)
    return -1;
  for (size_t i = 0; i < count; i++)
    if (ids[i] >= c->items || (i && ids[i] <= ids[i - 1]))
      return -1;
  if (!weight)
    return 0;
  /* Every pair count <= total, so the total overflow guard protects all cells.
   */
  for (size_t i = 0; i + 1 < count; i++) {
    size_t base = c->pairs - (size_t)triangle(c->items - ids[i]);
    for (size_t j = i + 1; j < count; j++)
      c->counts[base + ids[j] - ids[i] - 1] += weight;
  }
  c->total += weight;
  return 0;
}

uint64_t bishfpm_pair_counter_get(const bishfpm_pair_counter_t *c, uint32_t a,
                                  uint32_t b) {
  if (!c || a == b || a >= c->items || b >= c->items)
    return 0;
  if (a > b) {
    uint32_t swap = a;
    a = b;
    b = swap;
  }
  size_t base = c->pairs - (size_t)triangle(c->items - a);
  return c->counts[base + b - a - 1];
}

uint64_t bishfpm_pair_counter_total(const bishfpm_pair_counter_t *c) {
  return c ? c->total : 0;
}
