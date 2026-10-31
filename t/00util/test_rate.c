#include "bishfpm_rate.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

static void test_extreme_decay(void) {
  double mass = DBL_MAX / 2;
  double expected = ldexp(mass, -1075);
  double direct = bishfpm_rate_decay(mass, 1075, 1);
  double split = bishfpm_rate_decay(bishfpm_rate_decay(mass, 500, 1), 575, 1);
  /* The multiplier underflows, but the decayed count is a normal double. */
  assert(expected >= DBL_MIN);
  assert(fabs(direct / expected - 1) < 1e-12);
  assert(fabs(split / direct - 1) < 1e-12);
  double fractional = bishfpm_rate_decay(mass, 2151, 2);
  assert(fabs(fractional / (expected * sqrt(.5)) - 1) < 1e-12);

  /* Preserve representable subnormals, including fractional half-lives. */
  assert(bishfpm_rate_decay(mass, 2097, 1) == DBL_TRUE_MIN);
  assert(bishfpm_rate_decay(mass, 4195, 2) == DBL_TRUE_MIN);
  assert(bishfpm_rate_decay(mass, 2098, 1) == 0);
  assert(bishfpm_rate_decay(DBL_TRUE_MIN, 1, 1) == 0);
  /* These elapsed-half-life values must not be converted to an integer. */
  assert(bishfpm_rate_decay(mass, UINT64_MAX, 1) == 0);
  assert(bishfpm_rate_decay(mass, UINT64_MAX, DBL_TRUE_MIN) == 0);
}

int main(void) {
  test_extreme_decay();

  /* test single rate initialization */
  bishfpm_rate_t *r = bishfpm_rate_new();
  assert(r && bishfpm_rate_query(r, 0, 1000) == 0);

  /* first event at t=1000 */
  double rate = bishfpm_rate_update(r, 1000, 1000.0);
  assert(rate == 1.0);
  assert(bishfpm_rate_query(r, 0, 1000) == 1);

  /* second event at same timestamp */
  rate = bishfpm_rate_update(r, 1000, 1000.0);
  assert(rate == 2.0);

  /* decay after 1 half-life (1000 ms) without update */
  double q = bishfpm_rate_query(r, 2000, 1000.0);
  assert(fabs(q - 1.0) < 1e-4);

  /* third event at t=2000 (after 1 half life) */
  rate = bishfpm_rate_update(r, 2000, 1000.0);
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
  assert(bishfpm_rate_table_count(tbl) == 0);

  bishfpm_rate_table_free(tbl);
  bishfpm_rate_init(r);
  assert(bishfpm_rate_query(r, 2000, 1000) == 0);
  bishfpm_rate_free(r);

  printf("test_rate passed.\n");
  return 0;
}
