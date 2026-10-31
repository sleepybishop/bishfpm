#include "bishfpm_itemset.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int subset_count = 0;
static void count_subsets(const char *const *subset_items, size_t subset_len,
                          void *udata) {
  (void)subset_items;
  (void)subset_len;
  (void)udata;
  subset_count++;
}

int main(void) {
  /* test sorting */
  const char *items[] = {"item_c", "item_a", "item_b"};
  bishfpm_itemset_sort(items, 3);
  assert(strcmp(items[0], "item_a") == 0);
  assert(strcmp(items[1], "item_b") == 0);
  assert(strcmp(items[2], "item_c") == 0);

  /* test serialization */
  char buf[128];
  size_t len = bishfpm_itemset_serialize(items, 3, buf, sizeof(buf));
  assert(len > 0);
  assert(bishfpm_itemset_count(buf) == 3);

  /* test human-readable formatting */
  char fmt[128];
  bishfpm_itemset_format(buf, fmt, sizeof(fmt));
  assert(strcmp(fmt, "item_a, item_b, item_c") == 0);

  /* test subset generation for 3 items up to order 3: 2^3 - 1 = 7 */
  subset_count = 0;
  bishfpm_generate_subsets(items, 3, 3, count_subsets, NULL);
  assert(subset_count == 7);

  /* test subset generation up to order 2: 3 (size 1) + 3 (size 2) = 6 */
  subset_count = 0;
  bishfpm_generate_subsets(items, 3, 2, count_subsets, NULL);
  assert(subset_count == 6);

  printf("test_itemset passed.\n");
  return 0;
}
