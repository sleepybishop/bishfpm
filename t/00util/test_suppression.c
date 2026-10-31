#include "bishfpm.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_ALERTS 512
static int alert_count = 0;
static char alert_patterns[MAX_ALERTS][BISHFPM_MAX_PATTERN_LEN];
static double alert_lifts[MAX_ALERTS];

static void on_test_trigger(const char *group_key,
                            const bishfpm_trigger_t *trigger, void *udata) {
  (void)group_key;
  (void)udata;
  if (alert_count < MAX_ALERTS) {
    snprintf(alert_patterns[alert_count], sizeof(alert_patterns[alert_count]),
             "%s", trigger->formatted);
    alert_lifts[alert_count] = trigger->lift;
  }
  alert_count++;
}

int main(void) {
  /* test 1: verify lift calculation and filtering */
  {
    bishfpm_config_t cfg;
    bishfpm_config_default(&cfg);
    cfg.ewma_half_life_ms = 2000.0;
    cfg.item_rate_threshold = 4.0;
    cfg.promotion_fraction = 0.15;
    cfg.min_pattern_rate = 4.0;
    cfg.min_lift = 1.20; /* require at least 1.2x lift */
    cfg.min_itemset_order = 2;
    cfg.trigger_cooldown_ms = 3000;

    bishfpm_t *engine = bishfpm_create(&cfg);
    assert(engine != NULL);

    alert_count = 0;
    uint64_t t = 1000;

    /* send stream: 75% background "other=item", 25% coordinated burst {attr=A,
     * attr=B} */
    /* P(A) = 0.25, P(B) = 0.25, P(A,B) = 0.25 -> Lift = 0.25 / (0.25 * 0.25)
     * = 4.0 */
    for (int i = 0; i < 150; i++) {
      if (i % 4 == 0) {
        const char *items[] = {"attr=A", "attr=B"};
        bishfpm_add_event(engine, "test_grp", t, items, 2, on_test_trigger,
                          NULL);
      } else {
        const char *items[] = {"other=normal"};
        bishfpm_add_event(engine, "test_grp", t, items, 1, on_test_trigger,
                          NULL);
      }
      t += 20;
    }

    /* correlated burst must trigger alert with lift >= 1.20 */
    assert(alert_count > 0);
    assert(alert_lifts[alert_count - 1] >= 1.20);
    assert(strstr(alert_patterns[alert_count - 1], "attr=A") != NULL);
    assert(strstr(alert_patterns[alert_count - 1], "attr=B") != NULL);

    bishfpm_free(engine);
  }

  /* test 2: verify subsumption pruning of noisy supersets */
  {
    bishfpm_config_t cfg;
    bishfpm_config_default(&cfg);
    cfg.ewma_half_life_ms = 2000.0;
    cfg.item_rate_threshold = 4.0;
    cfg.promotion_fraction = 0.15;
    cfg.min_pattern_rate = 4.0;
    cfg.min_itemset_order = 2;
    cfg.max_itemset_order = 3;
    cfg.subsumption_ratio = 0.85; /* require 85% retention */
    cfg.trigger_cooldown_ms = 3000;

    bishfpm_t *engine = bishfpm_create(&cfg);
    assert(engine != NULL);

    alert_count = 0;
    uint64_t t = 10000;

    /* send 60% background, 40% attack {A, B}, with noisy passenger {C} on only
     * 10% */
    for (int i = 0; i < 200; i++) {
      if (i % 5 < 2) {
        /* attack event */
        const char *items[3];
        items[0] = "attr=A";
        items[1] = "attr=B";
        size_t n = 2;
        if (i % 10 == 0) {
          items[n++] = "attr=C_noise";
        }
        bishfpm_add_event(engine, "subsume_grp", t, items, n, on_test_trigger,
                          NULL);
      } else {
        const char *bg[] = {"other=item"};
        bishfpm_add_event(engine, "subsume_grp", t, bg, 1, on_test_trigger,
                          NULL);
      }
      t += 20;
    }

    /* root pattern {A, B} must alert, while {A, B, C_noise} must never alert */
    assert(alert_count > 0);
    int c_noise_count = 0;
    for (int k = 0; k < alert_count && k < MAX_ALERTS; k++) {
      if (strstr(alert_patterns[k], "attr=C_noise") != NULL) {
        c_noise_count++;
      }
    }
    assert(c_noise_count == 0);

    bishfpm_free(engine);
  }

  /* test 3: verify item_share_threshold */
  {
    bishfpm_config_t cfg;
    bishfpm_config_default(&cfg);
    cfg.ewma_half_life_ms = 1000.0;
    cfg.item_rate_threshold = 5.0;
    cfg.item_share_threshold = 0.50; /* item must have >= 50% of traffic */
    cfg.promotion_fraction = 0.20;
    cfg.min_pattern_rate = 5.0;
    cfg.min_itemset_order = 2;

    bishfpm_t *engine = bishfpm_create(&cfg);
    assert(engine != NULL);

    alert_count = 0;
    uint64_t t = 20000;

    /* send 100 events: all have X, 30% have Y (rate = 15/s, but share = 30% <
     * 50%) */
    for (int i = 0; i < 100; i++) {
      const char *items[2];
      items[0] = "attr=X";
      size_t n = 1;
      if (i % 3 == 0) {
        items[n++] = "attr=Y_low_share";
      }
      bishfpm_add_event(engine, "share_grp", t, items, n, on_test_trigger,
                        NULL);
      t += 20;
    }

    /* Y_low_share should not be gated as elevated despite rate > 5.0 */
    assert(alert_count == 0);

    bishfpm_free(engine);
  }

  printf("PASS: all suppression, lift, and subsumption tests passed\n");
  return 0;
}
