#include "bishfpm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int trigger_count = 0;
static bool triggered_ps5_ru = false;

static void on_pattern_trigger(const char *group_key,
                               const bishfpm_trigger_t *trigger, void *udata) {
  (void)udata;
  trigger_count++;
  if (strstr(trigger->formatted, "item=PS5") &&
      strstr(trigger->formatted, "country=RU")) {
    triggered_ps5_ru = true;
  }
  printf("TRIGGER! [Group: %s] Pattern: '%s' (Rate: %.2f, Group Rate: %.2f, "
         "Frac: %.2f%%)\n",
         group_key, trigger->formatted, trigger->pattern_rate,
         trigger->group_rate, trigger->rate_fraction * 100.0);
}

int main(void) {
  bishfpm_config_t cfg;
  bishfpm_config_default(&cfg);
  cfg.ewma_half_life_ms = 4000.0;
  cfg.item_rate_threshold = 4.0;
  cfg.min_pattern_rate = 4.0;
  cfg.promotion_fraction = 0.25; /* 25% of group traffic */
  cfg.trigger_cooldown_ms = 5000;

  bishfpm_t *engine = bishfpm_create(&cfg);
  assert(engine != NULL);

  uint64_t t = 1000;

  /* 1. normal background traffic */
  const char *tx1[] = {"item=bread", "country=US", "pay=visa"};
  const char *tx2[] = {"item=milk", "country=US", "pay=mastercard"};
  const char *tx3[] = {"item=eggs", "country=US", "pay=cash"};
  const char *tx4[] = {"item=cereal", "country=US", "pay=amex"};

  for (int i = 0; i < 20; i++) {
    const char *const *tx = (i % 4 == 0)   ? tx1
                            : (i % 4 == 1) ? tx2
                            : (i % 4 == 2) ? tx3
                                           : tx4;
    bishfpm_add_event(engine, "store_101", t, tx, 3, on_pattern_trigger, NULL);
    t += 100;
  }

  /* no anomaly should trigger on uniform background traffic */
  assert(trigger_count == 0);

  /* 2. burst fraud attack: sudden spike in PS5 bought with foreign RU card */
  const char *burst_tx[] = {"item=PS5", "country=RU", "pay=visa"};
  for (int i = 0; i < 14; i++) {
    bishfpm_add_event(engine, "store_101", t, burst_tx, 3, on_pattern_trigger,
                      NULL);
    t += 50;
  }

  /* verify burst was detected */
  assert(trigger_count > 0);
  assert(triggered_ps5_ru);

  bishfpm_free(engine);

  printf("test_engine passed: detected %d burst triggers.\n", trigger_count);
  return 0;
}
