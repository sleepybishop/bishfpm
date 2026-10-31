#include "bishfpm.h"
#include <stdio.h>
#include <stdlib.h>

static void alert_handler(const char *group_key,
                          const bishfpm_trigger_t *trigger, void *udata) {
  (void)udata;
  printf("\n🚨 [ALERT] Group '%s': Anomaly Detected!\n", group_key);
  printf("   Pattern:     %s\n", trigger->formatted);
  printf("   Items Count: %u\n", trigger->item_count);
  printf("   Burst Rate:  %.2f events/sec\n", trigger->pattern_rate);
  printf("   Group Rate:  %.2f events/sec\n", trigger->group_rate);
  printf("   Fraction:    %.1f%% of total store traffic\n\n",
         trigger->rate_fraction * 100.0);
}

int main(void) {
  printf("bishfpm Real-Time Burst Mining Demo\n");
  printf("------------------------------------\n");

  bishfpm_config_t cfg;
  bishfpm_config_default(&cfg);
  cfg.ewma_half_life_ms = 5000.0;
  cfg.item_rate_threshold = 4.0;
  cfg.promotion_fraction = 0.20;
  cfg.min_pattern_rate = 4.0;
  cfg.max_itemset_order = 3;

  bishfpm_t *engine = bishfpm_create(&cfg);
  if (!engine) {
    fprintf(stderr, "failed to create bishfpm engine\n");
    return 1;
  }

  uint64_t clock_ms = 1000;
  const char *store = "store_san_francisco";

  /* simulate benign transactions */
  printf("Simulating 30 normal retail transactions...\n");
  const char *products[] = {"item=coffee", "item=milk", "item=newspaper",
                            "item=sandwich"};
  const char *cards[] = {"country=US", "country=CA"};

  for (int i = 0; i < 30; i++) {
    const char *tx[3];
    tx[0] = products[i % 4];
    tx[1] = cards[i % 2];
    tx[2] = "channel=pos";
    bishfpm_add_event(engine, store, clock_ms, tx, 3, alert_handler, NULL);
    clock_ms += 100;
  }

  /* simulate fraud burst */
  printf("Simulating sudden burst of 12 suspicious PS5 purchases with foreign "
         "card...\n");
  const char *fraud_tx[] = {"item=PlayStation_5", "country=RU", "channel=pos"};
  for (int i = 0; i < 12; i++) {
    bishfpm_add_event(engine, store, clock_ms, fraud_tx, 3, alert_handler,
                      NULL);
    clock_ms += 40;
  }

  bishfpm_free(engine);
  printf("Simulation completed successfully.\n");
  return 0;
}
