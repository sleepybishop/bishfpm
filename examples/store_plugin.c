/* out-of-tree consumer: only the installed engine/plugin headers are needed */
#include "exact_store.h"
#include <stdio.h>
int main(void) {
  bishfpm_config_t c;
  bishfpm_config_default(&c);
  c.max_groups = 1;
  c.max_items_per_group = c.max_event_items = 16;
  c.min_itemset_order = c.max_itemset_order = 2;
  c.item_rate_threshold = c.item_share_threshold = 0;
  c.min_pattern_rate = 1;
  exact_store_options_t backend = {64, BISHFPM_STORE_PAIR64, "exact pairs"};
  bishfpm_store_engine_options_t options;
  bishfpm_store_engine_options_default(&options);
  options.notification_capacity = 16;
  bishfpm_t *e = bishfpm_create_with_store(&c, &options, &exact_pair_store,
                                           &backend, sizeof(backend));
  if (!e)
    return 1;
  const char *items[] = {"a", "b", "c"};
  int result = bishfpm_add_event(e, "g", 0, items, 3, NULL, NULL);
  bishfpm_count_bounds_t b;
  int valid = bishfpm_pattern_bounds(e, "g", "a\037b", 0, &b);
  bishfpm_store_snapshot_t s;
  int snapshot = bishfpm_get_store_snapshot(e, "g", sizeof(s), &s);
  bishfpm_free(e);
  if (result < 0 || !valid || !b.known || b.lower != 1 || b.upper != 1 ||
      !snapshot || s.resident_patterns != 3)
    return 2;
  puts("store_plugin example passed.");
  return 0;
}
