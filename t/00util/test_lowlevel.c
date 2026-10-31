/* This file intentionally uses only installed public headers. It is also
 * compiled as C++ and linked against staged static/shared libraries. */
#include "bishfpm_lowlevel.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  bishfpm_pair_counter_t *pairs = bishfpm_pair_counter_new(3, 1024);
  assert(pairs);
  const uint32_t ids[] = {0, 2};
  assert(bishfpm_pair_counter_add(pairs, ids, 2, 7) == 0);
  assert(bishfpm_pair_counter_get(pairs, 2, 0) == 7);
  assert(bishfpm_pair_counter_total(pairs) == 7);
  bishfpm_pair_counter_reset(pairs);
  assert(bishfpm_pair_counter_get(pairs, 0, 2) == 0);
  assert(bishfpm_pair_counter_storage_bytes(pairs) <= 1024);
  bishfpm_pair_counter_free(pairs);
  bishfpm_rate_t *rate = bishfpm_rate_new();
  assert(rate);
  assert(bishfpm_rate_query(rate, 0, 1000) == 0);
  assert(bishfpm_rate_update_weighted(rate, 0, 1000, 8) == 8);
  assert(bishfpm_rate_query(rate, 1000, 1000) == 4);
  bishfpm_rate_init(rate);
  assert(bishfpm_rate_query(rate, 1000, 1000) == 0);
  bishfpm_rate_free(rate);

  bishfpm_rate_table_t *table = bishfpm_rate_table_new_configured(16, 1, 42);
  assert(table);
  assert(bishfpm_rate_table_update(table, "a", 0, 1000) == 1);
  assert(bishfpm_rate_table_update(table, "b", 0, 1000) == 0);
  assert(bishfpm_rate_table_count(table) == 1);
  assert(bishfpm_rate_table_query(table, "a", 1000, 1000) == 0.5);
  assert(bishfpm_rate_table_prune(table, 1000, 1000, 1) == 1);
  assert(bishfpm_rate_table_count(table) == 0);
  bishfpm_rate_table_free(table);

  /* Duplicate batches are invalid even if some keys already exist. Rejection
   * must not publish earlier new keys or consume the distinct-key budget. */
  table = bishfpm_rate_table_new_configured(16, 2, 42);
  assert(table);
  const char *duplicates[] = {"new", "new"};
  assert(!bishfpm_rate_table_prepare(table, duplicates, 2));
  assert(bishfpm_rate_table_count(table) == 0);
  assert(bishfpm_rate_table_update(table, "a", 1000, 1000) == 1);
  const char *mixed[] = {"new", "a", "a"};
  assert(!bishfpm_rate_table_prepare(table, mixed, 3));
  assert(bishfpm_rate_table_count(table) == 1);
  const char *distinct[] = {"a", "b"};
  assert(bishfpm_rate_table_prepare(table, distinct, 2));
  assert(bishfpm_rate_table_count(table) == 2);
  assert(bishfpm_rate_table_update(table, "b", 1000, 1000) == 1);
  /* No-removal pruning must not backdate a later counter. */
  assert(bishfpm_rate_table_prune(table, 0, 1000, .1) == 0);
  assert(bishfpm_rate_table_query(table, "a", 2000, 1000) == .5);
  assert(bishfpm_rate_table_prune(table, 2000, 1000, .1) == 0);
  assert(bishfpm_rate_table_query(table, "b", 3000, 1000) == .25);
  bishfpm_rate_table_free(table);

  /* Equal-rate truncation uses canonical pattern order across insertion orders.
   */
  for (int reverse = 0; reverse < 2; reverse++) {
    bishfpm_topk_t *ties = bishfpm_topk_new(3, 1000);
    assert(ties);
    const char *names[] = {"a", "b", "c"};
    for (int i = 0; i < 3; i++)
      assert(bishfpm_topk_add(ties, names[reverse ? 2 - i : i], 1, 0) == 1);
    bishfpm_topk_item_t top[2];
    assert(bishfpm_topk_get_top(ties, 1000, top, 2) == 2);
    assert(!strcmp(top[0].pattern, "a") && !strcmp(top[1].pattern, "b"));
    bishfpm_topk_free(ties);
  }

  unsigned char buffer[4096];
  bishfpm_arena_t *arena = bishfpm_arena_new_buffer(buffer, sizeof(buffer));
  assert(arena);
  assert(bishfpm_arena_capacity(arena) == sizeof(buffer));
  assert(bishfpm_arena_used(arena) == 0);
  bishfpm_topk_t *store = bishfpm_topk_new_arena(arena, 4, 1000);
  assert(store);
  assert(bishfpm_arena_used(arena) > 0);
  assert(bishfpm_arena_used(arena) <= bishfpm_arena_capacity(arena));
  assert(bishfpm_topk_storage_bytes(store) > 0);
  bishfpm_topk_set_seed(store, 42);
  assert(bishfpm_topk_add(store, "a", 8, 0) == 8);
  bishfpm_topk_item_t result;
  assert(bishfpm_topk_get_top(store, 1000, &result, 1) == 1);
  assert(!strcmp(result.pattern, "a") && result.rate == 4 && result.error == 0);
  strcpy(result.pattern, "changed");
  result.rate = 99;
  assert(bishfpm_topk_query(store, "a", 1000) == 4);
  assert(bishfpm_topk_query(store, "changed", 1000) == 0);
  bishfpm_topk_free(store); /* arena owns the store; this leaves it alive */
  bishfpm_arena_reset(arena);
  assert(bishfpm_arena_used(arena) == 0);
  char *text = bishfpm_arena_strdup(arena, "borrowed");
  assert(text && !strcmp(text, "borrowed"));
  bishfpm_arena_free(arena);              /* releases only the wrapper */
  assert(!memcmp(buffer, "borrowed", 9)); /* caller's buffer is still alive */

  assert(!bishfpm_arena_new_buffer(NULL, 1));
  assert(!bishfpm_arena_new_buffer(buffer, 0));
  assert(!bishfpm_arena_new_buffer(buffer, SIZE_MAX));
  assert(bishfpm_arena_capacity(NULL) == 0);
  assert(bishfpm_arena_used(NULL) == 0);
  assert(bishfpm_rate_table_count(NULL) == 0);
  assert(bishfpm_topk_storage_bytes(NULL) == 0);
  puts("test_lowlevel passed.");
  return 0;
}
