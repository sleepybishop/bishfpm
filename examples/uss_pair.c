#include "bishfpm_uss.h"
#include <math.h>
#include <stdio.h>

static void alert(const char *group, const bishfpm_trigger_t *t, void *ctx) {
  (void)ctx;
  printf("%s: %s, matching count %.3f\n", group, t->formatted, t->pattern_rate);
}
int main(void) {
  bishfpm_config_t cfg;
  bishfpm_config_default(&cfg);
  cfg.min_itemset_order = cfg.max_itemset_order = 2;
  cfg.max_groups = 1;
  cfg.max_event_items = cfg.max_items_per_group = 16;
  cfg.item_rate_threshold = 0;
  cfg.min_pattern_rate = 1;
  bishfpm_uss_options_t settings;
  bishfpm_uss_options_default(&settings);
  settings.capacity = 16;
  /* Estimates discover identities; matching updates confirm alerts. */
  bishfpm_t *engine = bishfpm_create_with_store(
      &cfg, NULL, &bishfpm_uss_pair_store, &settings, sizeof(settings));
  if (!engine)
    return 1;
  const char *items[] = {"a", "b", "c"};
  int alerts = bishfpm_add_event(engine, "g", 0, items, 3, alert, NULL);
  double estimate = bishfpm_pattern_estimate(engine, "g", "a\037b", 0);
  bishfpm_count_bounds_t bounds;
  int valid = bishfpm_pattern_bounds(engine, "g", "a\037b", 0, &bounds);
  bishfpm_free(engine);
  return alerts != 3 || estimate != 1 || !valid || bounds.known ||
         bounds.lower != 0 || !isinf(bounds.upper);
}
