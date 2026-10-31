#include "bishfpm_topk.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  /* test top-k with capacity 4 */
  bishfpm_topk_t *topk = bishfpm_topk_new(4, 4, 128, 5000.0, 0.95);
  assert(topk != NULL);

  /* stream items */
  for (int i = 0; i < 20; i++) {
    bishfpm_topk_update(topk, "pattern_heavy", 1000 + i * 10);
  }
  for (int i = 0; i < 10; i++) {
    bishfpm_topk_update(topk, "pattern_medium", 1000 + i * 20);
  }
  for (int i = 0; i < 5; i++) {
    bishfpm_topk_update(topk, "pattern_light", 1000 + i * 40);
  }
  bishfpm_topk_update(topk, "pattern_rare1", 1000);
  bishfpm_topk_update(topk, "pattern_rare2", 1000);

  /* get top 4 */
  bishfpm_heap_item_t items[4];
  size_t n = bishfpm_topk_get_top(topk, 1300, items, 4);
  assert(n == 4);

  /* pattern_heavy should be rank 0 */
  assert(strcmp(items[0].pattern, "pattern_heavy") == 0);
  assert(strcmp(items[1].pattern, "pattern_medium") == 0);
  assert(items[0].rate > items[1].rate);
  assert(items[1].rate > items[2].rate);

  /* test arena backed top-k */
  bishfpm_arena_t *a = bishfpm_arena_new(64 * 1024);
  assert(a != NULL);
  bishfpm_topk_t *topk_a = bishfpm_topk_new_arena(a, 4, 4, 128, 5000.0, 0.95);
  assert(topk_a != NULL);

  for (int i = 0; i < 15; i++) {
    bishfpm_topk_update(topk_a, "arena_test", 1000 + i * 10);
  }
  double r = bishfpm_topk_query(topk_a, "arena_test", 1150);
  assert(r >= 10.0);

  bishfpm_arena_free(a);
  bishfpm_topk_free(topk);

  printf("test_topk passed.\n");
  return 0;
}
