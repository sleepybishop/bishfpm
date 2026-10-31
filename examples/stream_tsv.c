#include "bishfpm.h"
#include "record.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int alert_count = 0;
static double first_alert_sec = -1.0;
static double last_alert_sec = -1.0;

static void on_alert(const char *group_key, const bishfpm_trigger_t *trigger,
                     void *udata) {
  (void)udata;
  alert_count++;
  double alert_sec = (double)trigger->timestamp / 1000.0;
  if (first_alert_sec < 0.0) {
    first_alert_sec = alert_sec;
  }
  last_alert_sec = alert_sec;

  printf("[%7.2fs] 🚨 ALERT #%d [Group: %s]\n", alert_sec, alert_count,
         group_key);
  printf("           Pattern:    %s\n", trigger->formatted);
  printf("           Count:      %.2f decayed events (%.1f%% of group rate "
         "%.2f, Lift: %.2fx)\n\n",
         trigger->pattern_rate, trigger->rate_fraction * 100.0,
         trigger->group_rate, trigger->lift);
}

int main(int argc, char **argv) {
  if (argc > 3 || (argc == 3 && strcmp(argv[2], "--sensitive") != 0)) {
    fprintf(stderr, "usage: %s [tsv_path [--sensitive]]\n", argv[0]);
    return 1;
  }
  const char *tsv_path = (argc > 1) ? argv[1] : "data/sample_checkout.tsv";
  FILE *fp = fopen(tsv_path, "r");
  if (!fp) {
    fprintf(stderr, "cannot open %s\n", tsv_path);
    return 1;
  }

  int sensitive = argc == 3;
  char header[128];
  if (fgets(header, sizeof(header), fp) &&
      strcmp(header, "# bishfpm-profile: sensitive\n") == 0)
    sensitive = 1;
  rewind(fp);

  printf("bishfpm TSV Stream Ingestion\n");
  printf("Loading data file: %s\n", tsv_path);
  printf("Threshold profile: %s\n", sensitive ? "sensitive" : "standard");
  printf("--------------------------------------------------\n");

  bishfpm_config_t cfg;
  bishfpm_config_default(&cfg);
  cfg.ewma_half_life_ms = 4000.0;
  cfg.item_rate_threshold = 15.0;
  cfg.item_share_threshold = 0.20;
  cfg.promotion_fraction = 0.40;
  cfg.min_pattern_rate = 15.0;
  cfg.min_lift = 1.15;
  if (sensitive) {
    cfg.item_rate_threshold = 3.0;
    cfg.item_share_threshold = 0.15;
    cfg.promotion_fraction = 0.30;
    cfg.min_pattern_rate = 6.0;
    cfg.min_lift = 1.10;
  }
  cfg.subsumption_ratio = 0.85;
  cfg.min_itemset_order = 2;
  cfg.max_itemset_order = 4;
  cfg.topk_k = 1024;
  cfg.max_items_per_group = 2048;
  cfg.max_groups = 32;
  cfg.trigger_cooldown_ms = 8000;

  bishfpm_t *engine = bishfpm_create(&cfg);
  if (!engine) {
    fclose(fp);
    return 1;
  }

  char line[4096];
  size_t line_count = 0, rejected = 0;
  double first_burst_sec = -1.0;
  double last_burst_sec = -1.0;
  uint64_t last_prune_ms = 0;
  const uint64_t prune_interval_ms = 5000;
  const double prune_min_rate = 0.05;

  clock_t start_clk = clock();

  int record_status;
  while ((record_status = read_record(fp, line, sizeof(line))) != 0) {
    if (record_status < 0) {
      rejected++;
      continue;
    }
    if (line[0] == '\0' || line[0] == '#') {
      continue;
    }

    char *fields[34];
    size_t n_fields;
    double time_sec;
    uint64_t time_ms;
    if (!split_tabs(line, fields, 34, &n_fields) || n_fields < 3 ||
        !parse_seconds(fields[0], &time_sec, &time_ms)) {
      rejected++;
      continue;
    }

    char *group = fields[1];
    if (group[0] == '\0') {
      rejected++;
      continue;
    }

    const char *items[32];
    size_t n_items = 0;
    int is_burst_item = 0;
    for (size_t i = 2; i < n_fields; i++) {
      if (fields[i][0] == '\0') {
        continue;
      }
      if (strcmp(fields[i], "item=playstation_5") == 0 ||
          strstr(fields[i], "anomaly=sandwich_burst") != NULL ||
          strstr(fields[i], "anomaly=flash_loan_drain") != NULL ||
          strstr(fields[i], "exploit=") != NULL) {
        is_burst_item = 1;
      }
      items[n_items++] = fields[i];
    }

    if (!n_items) {
      rejected++;
      continue;
    }

    int rc = bishfpm_add_event(engine, group, time_ms, items, n_items, on_alert,
                               NULL);
    if (rc < 0) {
      rejected++;
      continue;
    }
    line_count++;

    /* rejected records must not advance maintenance or burst bookkeeping */
    if (is_burst_item) {
      if (first_burst_sec < 0.0) {
        first_burst_sec = time_sec;
      }
      last_burst_sec = time_sec;
    }

    if (time_ms >= last_prune_ms &&
        time_ms - last_prune_ms >= prune_interval_ms) {
      if (last_prune_ms > 0) {
        bishfpm_prune(engine, time_ms, prune_min_rate);
      }
      last_prune_ms = time_ms;
    }
  }

  clock_t end_clk = clock();
  double cpu_sec = (double)(end_clk - start_clk) / CLOCKS_PER_SEC;

  int io_error = ferror(fp);
  if (fclose(fp) != 0)
    io_error = 1;

  bishfpm_stats_t stats;
  bishfpm_get_stats(engine, &stats);
  bishfpm_free(engine);

  printf("--------------------------------------------------\n");
  printf("Summary:\n");
  printf("  Events processed: %zu\n", line_count);
  printf("  Alerts triggered: %d\n", alert_count);
  if (stats.items_pruned > 0) {
    printf("  Stale items pruned: %llu\n",
           (unsigned long long)stats.items_pruned);
  }
  if (first_burst_sec >= 0.0) {
    printf("  Burst arrival window: %.2fs - %.2fs (duration: %.2fs)\n",
           first_burst_sec, last_burst_sec, last_burst_sec - first_burst_sec);
  }
  if (first_alert_sec >= 0.0) {
    printf("  First alert time: %.2fs\n", first_alert_sec);
    if (first_burst_sec >= 0.0) {
      printf("  Time to detection (TTD): %.2fs\n",
             first_alert_sec - first_burst_sec);
    }
    printf("  Alert observation span: %.2fs - %.2fs (duration: %.2fs)\n",
           first_alert_sec, last_alert_sec, last_alert_sec - first_alert_sec);
  }
  printf("  CPU processing time: %.4f seconds (%.0f events/sec)\n", cpu_sec,
         cpu_sec > 0 ? (double)line_count / cpu_sec : 0.0);

  if (fflush(stdout) == EOF)
    io_error = 1;
  if (rejected || io_error)
    fprintf(stderr, "Rejected records: %zu; I/O error: %d\n", rejected,
            io_error);
  return (rejected || io_error || ferror(stdout)) ? 1 : 0;
}
