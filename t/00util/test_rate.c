#include "bishfpm_rate.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
  /* test single rate initialization */
  bishfpm_rate_t r;
  bishfpm_rate_init(&r);
  assert(r.rate == 0.0);
  assert(r.last_time_ms == 0);

  /* first event at t=1000 */
  double rate = bishfpm_rate_update(&r, 1000, 1000.0);
  assert(rate == 1.0);
  assert(r.last_time_ms == 1000);

  /* second event at same timestamp */
  rate = bishfpm_rate_update(&r, 1000, 1000.0);
  assert(rate == 2.0);

  /* decay after 1 half-life (1000 ms) without update */
  double q = bishfpm_rate_query(&r, 2000, 1000.0);
  assert(fabs(q - 1.0) < 1e-4);

  /* third event at t=2000 (after 1 half life) */
  rate = bishfpm_rate_update(&r, 2000, 1000.0);
  /* 2.0 * 0.5 + 1.0 = 2.0 */
  assert(fabs(rate - 2.0) < 1e-4);

  /* test rate table */
  bishfpm_rate_table_t *tbl = bishfpm_rate_table_new(8);
  assert(tbl != NULL);

  bishfpm_rate_table_update(tbl, "item_a", 1000, 1000.0);
  bishfpm_rate_table_update(tbl, "item_a", 1000, 1000.0);
  bishfpm_rate_table_update(tbl, "item_b", 1000, 1000.0);

  double ra = bishfpm_rate_table_query(tbl, "item_a", 1000, 1000.0);
  assert(ra == 2.0);

  double rb = bishfpm_rate_table_query(tbl, "item_b", 1000, 1000.0);
  assert(rb == 1.0);

  /* prune after 10 half lives (rate decays to < 0.01) */
  size_t pruned = bishfpm_rate_table_prune(tbl, 11000, 1000.0, 0.05);
  assert(pruned == 2);
  assert(tbl->count == 0);

  bishfpm_rate_table_free(tbl);

  printf("test_rate passed.\n");
  return 0;
}
