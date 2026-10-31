#ifndef BISHFPM_RATE_H
#define BISHFPM_RATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque single-counter and marginal-table state. */
typedef struct bishfpm_rate bishfpm_rate_t;
typedef struct bishfpm_rate_table bishfpm_rate_table_t;

/* New counters start at zero. init resets an existing counter. */
bishfpm_rate_t *bishfpm_rate_new(void);
void bishfpm_rate_free(bishfpm_rate_t *r);

/* single rate operations */
void bishfpm_rate_init(bishfpm_rate_t *r);
double bishfpm_rate_decay(double current_rate, uint64_t delta_ms,
                          double half_life_ms);
double bishfpm_rate_update(bishfpm_rate_t *r, uint64_t now_ms,
                           double half_life_ms);
double bishfpm_rate_update_weighted(bishfpm_rate_t *r, uint64_t now_ms,
                                    double half_life_ms, double weight);
double bishfpm_rate_query(const bishfpm_rate_t *r, uint64_t now_ms,
                          double half_life_ms);

/* Reserve distinct keys atomically; newly reserved counters start at zero. */
bool bishfpm_rate_table_prepare(bishfpm_rate_table_t *, const char *const *,
                                size_t);

/* rate table operations */
bishfpm_rate_table_t *bishfpm_rate_table_new(size_t initial_cap);
/* Configure limits and hashing at construction; zero max_items is unlimited. */
bishfpm_rate_table_t *bishfpm_rate_table_new_configured(size_t initial_cap,
                                                        size_t max_items,
                                                        uint64_t hash_seed);
size_t bishfpm_rate_table_count(const bishfpm_rate_table_t *tbl);
void bishfpm_rate_table_free(bishfpm_rate_table_t *tbl);
double bishfpm_rate_table_update(bishfpm_rate_table_t *tbl, const char *key,
                                 uint64_t now_ms, double half_life_ms);
double bishfpm_rate_table_update_weighted(bishfpm_rate_table_t *tbl,
                                          const char *key, uint64_t now_ms,
                                          double half_life_ms, double weight);
double bishfpm_rate_table_query(const bishfpm_rate_table_t *tbl,
                                const char *key, uint64_t now_ms,
                                double half_life_ms);
size_t bishfpm_rate_table_prune(bishfpm_rate_table_t *tbl, uint64_t now_ms,
                                double half_life_ms, double min_rate);

#ifdef __cplusplus
}
#endif

#endif /* BISHFPM_RATE_H */
