#ifndef BISHFPM_STATE_H
#define BISHFPM_STATE_H
/* Private layouts. Only library code and structural invariant tests include
 * this. */
#include "bishfpm_arena.h"
#include "bishfpm_rate.h"
#include "bishfpm_topk.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct bishfpm_memory_budget bishfpm_memory_budget_t;

/* continuous time-decayed EWMA rate */
struct bishfpm_rate {
  double rate;
  uint64_t last_time_ms;
  bool initialized;
};

/* item rate entry in hash table */
typedef struct {
  char *key;
  uint64_t hash;
  bishfpm_rate_t rate;
  uint32_t id; /* Stable while retained; numeric engines keep marginal keys. */
} bishfpm_rate_entry_t;

/* hash table for tracking item rates */
struct bishfpm_rate_table {
  bishfpm_rate_entry_t *entries;
  size_t capacity;
  size_t count;
  size_t key_bytes; /* Owned key payload, including terminating NUL bytes. */
  size_t max_items;
  uint64_t hash_seed;
  bishfpm_memory_budget_t *budget;
};

typedef struct {
  double count;
  uint64_t time_ms;
} bishfpm_sketch_cell_t;

/* top-k min-heap item */
typedef struct {
  char pattern[BISHFPM_TOPK_MAX_STR];
  uint64_t hash;
  double rate;
  double error;
  uint64_t last_time_ms;
} bishfpm_heap_entry_t;

/* slot map entry for O(1) heap position lookup */
typedef struct {
  uint64_t hash;
  uint32_t heap_idx;
  bool occupied;
} bishfpm_slot_entry_t;

/* decayed weighted SpaceSaving store with min-heap */
struct bishfpm_topk {
  uint32_t k;
  double half_life_ms;

  /* min-heap */
  bishfpm_heap_entry_t *heap;
  uint32_t heap_size;

  /* hash index for heap items */
  bishfpm_slot_entry_t *slot_map;
  size_t slot_map_cap;

  uint64_t hash_seed;
  uint64_t max_time_ms;
  double omitted_bound; /* upper bound for pruned history at max_time_ms */
  bool arena_backed;
  bishfpm_sketch_cell_t *sketch;
  uint32_t sketch_width;
  double sketch_total;
  bool history_started;
};

/* bump allocator arena */
struct bishfpm_arena {
  uint8_t *beg;
  uint8_t *end;
  uint8_t *base;
  bool owned;
  bishfpm_memory_budget_t *budget;
};

#endif
