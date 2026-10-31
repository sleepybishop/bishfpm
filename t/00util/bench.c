/*
 * bishfpm microbenchmark suite
 */

#include "bishfpm.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* monotonic nanoseconds timestamp */
static double ns(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t)) {
    perror("clock_gettime");
    exit(1);
  }
  return 1e9 * t.tv_sec + t.tv_nsec;
}

/* double comparator for qsort */
static int cmp(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

/* benchmark single attribute and elevation configuration */
static void run(int m, int elevated, int n) {
  char names[32][32];
  const char *items[32];
  for (int i = 0; i < m; i++) {
    snprintf(names[i], 32, "attribute_%02d=value", i);
    items[i] = names[i];
  }

  double means[3] = {0}, p50 = 0, p99 = 0;
  int errors = 0;

  for (int r = 0; r < 3; r++) {
    bishfpm_config_t c;
    bishfpm_config_default(&c);
    c.item_rate_threshold = elevated ? 1.0 : 1e100;
    c.min_pattern_rate = 1e100;

    bishfpm_t *e = bishfpm_create(&c);
    if (!e) {
      fprintf(stderr, "failed to create engine\n");
      exit(1);
    }

    /* warmup */
    for (int i = 0; i < 1000; i++) {
      if (bishfpm_add_event(e, "group", 1 + (uint64_t)i / 100, items, m, NULL,
                            NULL) < 0) {
        fprintf(stderr, "warmup rejected\n");
        exit(1);
      }
    }

    /* timed iterations */
    double t = ns();
    for (int i = 0; i < n; i++) {
      if (bishfpm_add_event(e, "group", 11 + (uint64_t)i / 100, items, m, NULL,
                            NULL) < 0) {
        errors++;
      }
    }
    means[r] = (ns() - t) / n;

    if (r == 2) {
      double *s = malloc(n * sizeof(*s));
      if (!s) {
        perror("malloc");
        exit(1);
      }
      {
        for (int i = 0; i < n; i++) {
          t = ns();
          if (bishfpm_add_event(e, "group", 11 + (uint64_t)(n + i) / 100, items,
                                m, NULL, NULL) < 0)
            errors++;
          s[i] = ns() - t;
        }
        qsort(s, n, sizeof(*s), cmp);
        p50 = s[n / 2];
        p99 = s[(n * 99) / 100];
        free(s);
      }
    }
    bishfpm_free(e);
  }

  if (errors) {
    fprintf(stderr, "benchmark rejected %d events\n", errors);
    exit(1);
  }
  printf("m=%2d elevated=%d n=%6d mean_ns=[%4.0f, %4.0f, %4.0f] p50_ns=%4.0f "
         "p99_ns=%4.0f errors=%d\n",
         m, elevated, n, means[0], means[1], means[2], p50, p99, errors);
}

int main(void) {
  printf("bishfpm microbenchmark suite\n");
  printf("--------------------------------------------------------------------"
         "------------\n");
  run(20, 0, 100000);
  run(1, 1, 100000);
  run(3, 1, 50000);
  run(10, 1, 20000);
  run(20, 1, 5000);
  run(32, 1, 2000);
  printf("--------------------------------------------------------------------"
         "------------\n");
  return 0;
}
