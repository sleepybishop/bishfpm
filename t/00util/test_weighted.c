#include "bishfpm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int weighted_triggers = 0;

static void on_trigger(const char *group_key, const bishfpm_trigger_t *trigger,
                       void *udata) {
  (void)group_key;
  (void)trigger;
  (void)udata;
  weighted_triggers++;
}

int main(void) {
  bishfpm_config_t cfg;
  bishfpm_config_default(&cfg);
  cfg.ewma_half_life_ms = 4000.0;
  cfg.item_rate_threshold = 5.0;
  cfg.min_pattern_rate = 5.0;
  cfg.promotion_fraction = 0.20;

  bishfpm_t *engine = bishfpm_create(&cfg);
  assert(engine != NULL);

  /* ingest single batch event with weight 6 */
  const char *bulk_tx[] = {"item=bulk_item", "card=fraud_card"};
  uint64_t t = 1000;

  int res = bishfpm_add_event_weighted(engine, "store_bulk", t, bulk_tx, 2, 6,
                                       on_trigger, NULL);
  /* with weight 6, item rates are 6.0 >= 5.0 and pattern rate is 6.0 >= 5.0 */
  assert(res > 0);
  assert(weighted_triggers > 0);

  /* query rates */
  double gr = bishfpm_group_rate(engine, "store_bulk", t);
  assert(gr >= 6.0);

  double ir = bishfpm_item_rate(engine, "store_bulk", "item=bulk_item", t);
  assert(ir >= 6.0);

  /* edge cases */
  assert(bishfpm_add_event_weighted(NULL, "g", t, bulk_tx, 2, 1, NULL, NULL) ==
         -1);
  assert(bishfpm_add_event_weighted(engine, NULL, t, bulk_tx, 2, 1, NULL,
                                    NULL) == -1);
  assert(bishfpm_add_event_weighted(engine, "g", t, NULL, 0, 1, NULL, NULL) ==
         0);
  assert(bishfpm_add_event_weighted(engine, "g", t, bulk_tx, 2, 0, NULL,
                                    NULL) == 0);

  bishfpm_free(engine);

  printf("test_weighted passed.\n");
  return 0;
}
