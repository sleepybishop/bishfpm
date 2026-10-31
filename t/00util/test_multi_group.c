#include "bishfpm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int store1_triggers = 0;
static int store2_triggers = 0;

static void on_trigger(const char *group_key, const bishfpm_trigger_t *trigger,
                       void *udata) {
  (void)trigger;
  (void)udata;
  if (strcmp(group_key, "store_1") == 0) {
    store1_triggers++;
  } else if (strcmp(group_key, "store_2") == 0) {
    store2_triggers++;
  }
}

int main(void) {
  bishfpm_config_t cfg;
  bishfpm_config_default(&cfg);
  cfg.ewma_half_life_ms = 4000.0;
  cfg.item_rate_threshold = 4.0;
  cfg.min_pattern_rate = 4.0;
  cfg.promotion_fraction = 0.25;
  cfg.trigger_cooldown_ms = 5000;

  bishfpm_t *engine = bishfpm_create(&cfg);
  assert(engine != NULL);

  uint64_t t = 1000;

  /* 1. normal varied traffic across both stores */
  const char *s1_a[] = {"item=bread", "country=US", "pay=visa"};
  const char *s1_b[] = {"item=milk", "country=CA", "pay=mc"};
  const char *s1_c[] = {"item=eggs", "country=UK", "pay=amex"};
  const char *s1_d[] = {"item=cereal", "country=FR", "pay=cash"};

  for (int i = 0; i < 16; i++) {
    const char *const *s_tx = (i % 4 == 0)   ? s1_a
                              : (i % 4 == 1) ? s1_b
                              : (i % 4 == 2) ? s1_c
                                             : s1_d;
    bishfpm_add_event(engine, "store_1", t, s_tx, 3, on_trigger, NULL);
    bishfpm_add_event(engine, "store_2", t, s_tx, 3, on_trigger, NULL);
    t += 100;
  }

  assert(store1_triggers == 0);
  assert(store2_triggers == 0);

  /* 2. burst in store_1 only */
  const char *burst_tx[] = {"item=PS5", "country=RU"};
  for (int i = 0; i < 12; i++) {
    bishfpm_add_event(engine, "store_1", t, burst_tx, 2, on_trigger, NULL);
    t += 50;
  }

  /* store_1 must trigger, store_2 must remain clean */
  assert(store1_triggers > 0);
  assert(store2_triggers == 0);

  /* 3. test engine query functions */
  double g_rate1 = bishfpm_group_rate(engine, "store_1", t);
  double g_rate2 = bishfpm_group_rate(engine, "store_2", t);
  assert(g_rate1 > g_rate2);

  double i_rate_ps5 = bishfpm_item_rate(engine, "store_1", "item=PS5", t);
  assert(i_rate_ps5 >= 4.0);
  double i_rate_s2 = bishfpm_item_rate(engine, "store_2", "item=PS5", t);
  assert(i_rate_s2 == 0.0);

  /* 4. test cooldown suppression */
  int count_before = store1_triggers;
  bishfpm_add_event(engine, "store_1", t + 10, burst_tx, 2, on_trigger, NULL);
  /* cooldown active, no new trigger */
  assert(store1_triggers == count_before);

  /* advance time past cooldown (5000 ms) */
  t += 6000;
  for (int i = 0; i < 6; i++) {
    bishfpm_add_event(engine, "store_1", t, burst_tx, 2, on_trigger, NULL);
    t += 50;
  }
  /* cooldown expired: should re-trigger */
  assert(store1_triggers > count_before);

  /* 5. test group pruning after long inactivity */
  t += 60000;
  size_t pruned = bishfpm_prune(engine, t, 0.01);
  assert(pruned > 0);

  bishfpm_free(engine);

  printf("test_multi_group passed.\n");
  return 0;
}
