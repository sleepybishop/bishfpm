#include "bishfpm_hash.h"
#include "bishfpm_rate.h"
#include "internal.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LN2 0.693147180559945309417

bishfpm_rate_t *bishfpm_rate_new(void) {
  return calloc(1, sizeof(bishfpm_rate_t));
}

void bishfpm_rate_free(bishfpm_rate_t *r) { free(r); }

/* initialize rate tracker */
void bishfpm_rate_init(bishfpm_rate_t *r) {
  if (!r) {
    return;
  }
  r->rate = 0.0;
  r->last_time_ms = 0;
  r->initialized = false;
}

/* calculate continuous decay */
double bishfpm_rate_decay(double current_rate, uint64_t delta_ms,
                          double half_life_ms) {
  if (current_rate <= 0.0) {
    return 0.0;
  }
  if (delta_ms == 0 || half_life_ms <= 0.0) {
    return current_rate;
  }
  double half_lives = (double)delta_ms / half_life_ms;
  if (half_lives < -DBL_MIN_EXP || !isfinite(current_rate) || isnan(half_lives))
    return current_rate * exp(-LN2 * half_lives);

  /* A tiny multiplier can underflow before a large count is applied. Shift
   * the combined exponent instead, rounding only the final count to zero. */
  int exponent;
  double fraction = frexp(current_rate, &exponent);
  if (half_lives >= exponent - DBL_MIN_EXP + DBL_MANT_DIG + 1)
    return 0;
  /* The underflow bound above also excludes huge/infinite values before this
   * conversion. The fractional multiplier stays between one half and one. */
  int whole = (int)half_lives;
  return ldexp(fraction * exp2(-(half_lives - whole)), exponent - whole);
}

/* update rate with new weighted event */
double bishfpm_rate_update_weighted(bishfpm_rate_t *r, uint64_t now_ms,
                                    double half_life_ms, double weight) {
  if (!r || !isfinite(weight) || !isfinite(half_life_ms) ||
      half_life_ms <= 0.0) {
    return 0.0;
  }
  if (weight <= 0.0) {
    return bishfpm_rate_query(r, now_ms, half_life_ms);
  }
  bishfpm_rate_t next = *r;
  if (!next.initialized) {
    next.rate = weight;
    next.last_time_ms = now_ms;
    next.initialized = true;
  } else if (now_ms >= next.last_time_ms) {
    next.rate = bishfpm_rate_decay(next.rate, now_ms - next.last_time_ms,
                                   half_life_ms) +
                weight;
    next.last_time_ms = now_ms;
  } else {
    next.rate +=
        bishfpm_rate_decay(weight, next.last_time_ms - now_ms, half_life_ms);
  }
  if (!isfinite(next.rate))
    return 0.0;
  *r = next;
  return r->rate;
}

/* update rate with new event */
double bishfpm_rate_update(bishfpm_rate_t *r, uint64_t now_ms,
                           double half_life_ms) {
  return bishfpm_rate_update_weighted(r, now_ms, half_life_ms, 1.0);
}

/* query rate decayed to current time */
double bishfpm_rate_query(const bishfpm_rate_t *r, uint64_t now_ms,
                          double half_life_ms) {
  if (!r || !r->initialized || r->rate <= 0.0) {
    return 0.0;
  }
  if (now_ms <= r->last_time_ms) {
    return r->rate;
  }
  return bishfpm_rate_decay(r->rate, now_ms - r->last_time_ms, half_life_ms);
}

/* create rate table */
bishfpm_rate_table_t *bishfpm_rate_table_new(size_t initial_cap) {
  return bishfpm_rate_table_new_configured(initial_cap, 0, 0);
}

size_t bishfpm_rate_table_count(const bishfpm_rate_table_t *tbl) {
  return tbl ? tbl->count : 0;
}

bishfpm_rate_table_t *bishfpm_rate_table_new_configured(size_t initial_cap,
                                                        size_t max_items,
                                                        uint64_t hash_seed) {
  return rate_table_new_budget(initial_cap, max_items, hash_seed, NULL);
}

bishfpm_rate_table_t *rate_table_new_budget(size_t initial_cap,
                                            size_t max_items,
                                            uint64_t hash_seed,
                                            bishfpm_memory_budget_t *budget) {
  size_t cap = next_pow2(initial_cap < 16 ? 16 : initial_cap);
  if (cap == 0 || cap > SIZE_MAX / sizeof(bishfpm_rate_entry_t)) {
    return NULL;
  }
  bishfpm_rate_table_t *tbl =
      memory_calloc(budget, 1, sizeof(bishfpm_rate_table_t));
  if (!tbl) {
    return NULL;
  }
  tbl->budget = budget;
  tbl->capacity = cap;
  tbl->entries =
      memory_calloc(budget, tbl->capacity, sizeof(bishfpm_rate_entry_t));
  if (!tbl->entries) {
    memory_free(budget, tbl, sizeof(*tbl));
    return NULL;
  }
  tbl->count = 0;
  tbl->max_items = max_items;
  tbl->hash_seed = hash_seed;
  return tbl;
}

/* free rate table */
void bishfpm_rate_table_free(bishfpm_rate_table_t *tbl) {
  if (!tbl) {
    return;
  }
  if (tbl->entries) {
    for (size_t i = 0; i < tbl->capacity; i++) {
      if (tbl->entries[i].key) {
        memory_free(tbl->budget, tbl->entries[i].key,
                    strlen(tbl->entries[i].key) + 1);
      }
    }
    memory_free(tbl->budget, tbl->entries,
                tbl->capacity * sizeof(*tbl->entries));
  }
  memory_free(tbl->budget, tbl, sizeof(*tbl));
}

/* Return either the matching entry or its insertion slot. */
static bishfpm_rate_entry_t *rate_slot(bishfpm_rate_entry_t *entries,
                                       size_t capacity, const char *key,
                                       uint64_t hash) {
  size_t i = hash & (capacity - 1);
  while (entries[i].key &&
         (entries[i].hash != hash || strcmp(entries[i].key, key)))
    i = (i + 1) & (capacity - 1);
  return &entries[i];
}

bishfpm_rate_entry_t *rate_table_find(const bishfpm_rate_table_t *tbl,
                                      const char *key, uint64_t hash) {
  bishfpm_rate_entry_t *entry =
      rate_slot(tbl->entries, tbl->capacity, key, hash);
  return entry->key ? entry : NULL;
}

double bishfpm_rate_table_update_weighted(bishfpm_rate_table_t *tbl,
                                          const char *key, uint64_t now_ms,
                                          double half_life_ms, double weight) {
  if (!tbl || !key || !isfinite(weight) || weight < 0 ||
      !isfinite(half_life_ms) || half_life_ms <= 0)
    return 0;
  if (!weight)
    return bishfpm_rate_table_query(tbl, key, now_ms, half_life_ms);
  /* Reservation owns all allocation/insertion rules, even for standalone use.
   */
  if (!bishfpm_rate_table_prepare(tbl, &key, 1))
    return 0;
  bishfpm_rate_entry_t *entry =
      rate_table_find(tbl, key, bishfpm_hash_str(key, tbl->hash_seed));
  return bishfpm_rate_update_weighted(&entry->rate, now_ms, half_life_ms,
                                      weight);
}

/* update rate for key */
double bishfpm_rate_table_update(bishfpm_rate_table_t *tbl, const char *key,
                                 uint64_t now_ms, double half_life_ms) {
  return bishfpm_rate_table_update_weighted(tbl, key, now_ms, half_life_ms,
                                            1.0);
}

/* query rate for key */
double bishfpm_rate_table_query(const bishfpm_rate_table_t *tbl,
                                const char *key, uint64_t now_ms,
                                double half_life_ms) {
  if (!tbl || !key || tbl->count == 0) {
    return 0.0;
  }

  bishfpm_rate_entry_t *entry =
      rate_table_find(tbl, key, bishfpm_hash_str(key, tbl->hash_seed));
  return entry ? bishfpm_rate_query(&entry->rate, now_ms, half_life_ms) : 0;
}

/* Dynamic mode must be able to release weak keys even when a replacement
 * table cannot fit. Keep capacity and repair probe chains without allocation.
 */
static size_t prune_in_place(bishfpm_rate_table_t *tbl, uint64_t now,
                             double half, double cutoff) {
  size_t removed = 0, mask = tbl->capacity - 1;
  for (size_t i = 0; i < tbl->capacity;) {
    bishfpm_rate_entry_t *entry = &tbl->entries[i];
    if (!entry->key) {
      i++;
      continue;
    }
    double rate = bishfpm_rate_query(&entry->rate, now, half);
    if (rate >= cutoff) {
      if (now >= entry->rate.last_time_ms) {
        entry->rate.rate = rate;
        entry->rate.last_time_ms = now;
      }
      i++;
      continue;
    }
    size_t bytes = strlen(entry->key) + 1;
    memory_free(tbl->budget, entry->key, bytes);
    tbl->key_bytes -= bytes;
    tbl->count--;
    removed++;
    size_t hole = i;
    for (size_t probe = (i + 1) & mask; tbl->entries[probe].key;
         probe = (probe + 1) & mask) {
      size_t home = tbl->entries[probe].hash & mask;
      if (((probe - home) & mask) >= ((probe - hole) & mask)) {
        tbl->entries[hole] = tbl->entries[probe];
        hole = probe;
      }
    }
    memset(&tbl->entries[hole], 0, sizeof(tbl->entries[hole]));
  }
  return removed;
}

/* prune entries with decayed rate below min_rate */
size_t bishfpm_rate_table_prune(bishfpm_rate_table_t *tbl, uint64_t now_ms,
                                double half_life_ms, double min_rate) {
  if (!tbl || tbl->count == 0 || !isfinite(min_rate) || min_rate <= 0 ||
      !isfinite(half_life_ms) || half_life_ms <= 0) {
    return 0;
  }

  size_t removed = 0;
  size_t survivors = 0;
  for (size_t i = 0; i < tbl->capacity; i++) {
    if (!tbl->entries[i].key) {
      continue;
    }
    double current =
        bishfpm_rate_query(&tbl->entries[i].rate, now_ms, half_life_ms);
    if (current < min_rate) {
      removed++;
    } else {
      survivors++;
    }
  }

  /* calculate compacted capacity */
  size_t new_cap = next_pow2(survivors * 2 < 16 ? 16 : survivors * 2);
  if (!removed && new_cap == tbl->capacity) {
    for (size_t i = 0; i < tbl->capacity; i++) {
      bishfpm_rate_entry_t *entry = &tbl->entries[i];
      if (entry->key && now_ms >= entry->rate.last_time_ms) {
        entry->rate.rate =
            bishfpm_rate_query(&entry->rate, now_ms, half_life_ms);
        entry->rate.last_time_ms = now_ms;
      }
    }
    return 0;
  }
  bishfpm_rate_entry_t *new_entries =
      memory_calloc(tbl->budget, new_cap, sizeof(bishfpm_rate_entry_t));
  if (!new_entries) {
    return tbl->budget ? prune_in_place(tbl, now_ms, half_life_ms, min_rate)
                       : 0;
  }

  size_t mask = new_cap - 1;
  size_t new_count = 0;

  for (size_t i = 0; i < tbl->capacity; i++) {
    if (!tbl->entries[i].key) {
      continue;
    }
    double current =
        bishfpm_rate_query(&tbl->entries[i].rate, now_ms, half_life_ms);
    if (current < min_rate) {
      tbl->key_bytes -= strlen(tbl->entries[i].key) + 1;
      memory_free(tbl->budget, tbl->entries[i].key,
                  strlen(tbl->entries[i].key) + 1);
      tbl->entries[i].key = NULL;
    } else {
      /* update decayed rate only if event timestamp is not later */
      if (now_ms >= tbl->entries[i].rate.last_time_ms) {
        tbl->entries[i].rate.rate = current;
        tbl->entries[i].rate.last_time_ms = now_ms;
      }
      tbl->entries[i].rate.initialized = true;

      size_t idx = (size_t)(tbl->entries[i].hash & mask);
      while (new_entries[idx].key) {
        idx = (idx + 1) & mask;
      }
      new_entries[idx] = tbl->entries[i];
      new_count++;
    }
  }

  memory_free(tbl->budget, tbl->entries, tbl->capacity * sizeof(*tbl->entries));
  tbl->entries = new_entries;
  tbl->capacity = new_cap;
  tbl->count = new_count;
  return removed;
}

/* Distinct transaction keys are required, as in the public reservation
 * contract. Stage copies and optional growth before publishing any key or
 * counter. */
rate_prepare_result_t rate_table_prepare_result(bishfpm_rate_table_t *tbl,
                                                const char *const *keys,
                                                size_t count) {
  if (!tbl || (!keys && count))
    return RATE_INVALID;
  size_t missing = 0;
  for (size_t i = 0; i < count; i++) {
    if (!keys[i])
      return RATE_INVALID;
    if (!rate_table_find(tbl, keys[i],
                         bishfpm_hash_str(keys[i], tbl->hash_seed)))
      missing++;
  }
  if (!missing)
    return RATE_PREPARED;
  size_t total;
  if (!size_add(tbl->count, missing, &total) ||
      (tbl->max_items && total > tbl->max_items))
    return RATE_LIMIT;
  size_t capacity = tbl->capacity;
  while (total > capacity / 2) {
    if (capacity > SIZE_MAX / 2)
      return RATE_ALLOCATION;
    capacity *= 2;
  }
  if (capacity > SIZE_MAX / sizeof(*tbl->entries))
    return RATE_ALLOCATION;
  bishfpm_rate_entry_t *staged =
      memory_calloc(tbl->budget, missing, sizeof(*staged));
  if (!staged)
    return RATE_ALLOCATION;
  size_t n = 0, key_bytes = 0;
  for (size_t i = 0; i < count; i++) {
    uint64_t hash = bishfpm_hash_str(keys[i], tbl->hash_seed);
    if (rate_table_find(tbl, keys[i], hash))
      continue;
    char *copy = memory_strdup(tbl->budget, keys[i]);
    if (!copy)
      goto fail;
    key_bytes += strlen(copy) + 1;
    staged[n].id = (uint32_t)(tbl->count + n);
    staged[n].key = copy;
    staged[n++].hash = hash;
  }

  bishfpm_rate_entry_t *entries = tbl->entries;
  if (capacity != tbl->capacity) {
    entries = memory_calloc(tbl->budget, capacity, sizeof(*entries));
    if (!entries)
      goto fail;
    for (size_t i = 0; i < tbl->capacity; i++) {
      if (tbl->entries[i].key)
        *rate_slot(entries, capacity, tbl->entries[i].key,
                   tbl->entries[i].hash) = tbl->entries[i];
    }
  }
  /* Commit: insertion and ownership transfer cannot fail. */
  for (size_t i = 0; i < n; i++)
    *rate_slot(entries, capacity, staged[i].key, staged[i].hash) = staged[i];
  if (entries != tbl->entries)
    memory_free(tbl->budget, tbl->entries,
                tbl->capacity * sizeof(*tbl->entries));
  tbl->entries = entries;
  tbl->capacity = capacity;
  tbl->count = total;
  tbl->key_bytes += key_bytes;
  memory_free(tbl->budget, staged, missing * sizeof(*staged));
  return RATE_PREPARED;
fail:
  for (size_t i = 0; i < n; i++)
    memory_free(tbl->budget, staged[i].key, strlen(staged[i].key) + 1);
  memory_free(tbl->budget, staged, missing * sizeof(*staged));
  return RATE_ALLOCATION;
}

bool bishfpm_rate_table_prepare(bishfpm_rate_table_t *tbl,
                                const char *const *keys, size_t count) {
  if (!tbl || (!keys && count))
    return false;
  /* Keep public input validation out of the engine's deduplicated hot path. */
  for (size_t i = 0; i < count; i++) {
    if (!keys[i])
      return false;
    for (size_t j = 0; j < i; j++)
      if (!strcmp(keys[i], keys[j]))
        return false;
  }
  return rate_table_prepare_result(tbl, keys, count) == RATE_PREPARED;
}
