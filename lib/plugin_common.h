/* shared helpers and decayed min-heap for store plugins */
#ifndef BISHFPM_PLUGIN_COMMON_H
#define BISHFPM_PLUGIN_COMMON_H
#include "bishfpm_hash.h"
#include "bishfpm_rate.h"
#include "bishfpm_store_plugin.h"
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline uint64_t effective_time(uint64_t a, uint64_t b) {
  return a > b ? a : b;
}
static inline double decay_since(double count, uint64_t then, uint64_t now,
                                 double half) {
  return now <= then ? count : bishfpm_rate_decay(count, now - then, half);
}
static inline bool size_add(size_t a, size_t b, size_t *out) {
  if (b > SIZE_MAX - a)
    return false;
  *out = a + b;
  return true;
}
static inline bool size_mul(size_t a, size_t b, size_t *out) {
  if (a && b > SIZE_MAX / a)
    return false;
  *out = a * b;
  return true;
}
static inline bool checked_add(size_t a, size_t b, size_t *out) {
  return size_add(a, b, out);
}
static inline bool checked_mul(size_t a, size_t b, size_t *out) {
  return size_mul(a, b, out);
}
static inline size_t next_pow2(size_t v) {
  if (v > (SIZE_MAX >> 1))
    return 0;
  size_t p = 16;
  while (p < v)
    p <<= 1;
  return p;
}
static inline size_t plugin_slots(uint32_t capacity) {
  return next_pow2((size_t)capacity * 2);
}
static inline bool valid_text_key(const char *key, size_t length) {
  return key && length && length < BISHFPM_MAX_PATTERN_LEN && key[0];
}
static inline int
plugin_prepare(void *state, const bishfpm_store_event_t *event, void **token) {
  (void)state;
  (void)event;
  *token = NULL;
  return 1;
}
static inline void plugin_complete(void *state, void *token) {
  (void)state;
  (void)token;
}
#define BISHFPM_PLUGIN_CONFIG_OPS(config_t, min_ord, max_ord)                  \
  static int plan(const bishfpm_config_t *m, const void *raw, size_t n,        \
                  bishfpm_store_plan_t *out) {                                 \
    config_t c;                                                                \
    if (!out || !configure(m, raw, n, &c))                                     \
      return 0;                                                                \
    *out = (bishfpm_store_plan_t){                                             \
        sizeof(c), c.group_bytes, (min_ord), (max_ord), {0, 0}};               \
    return 1;                                                                  \
  }                                                                            \
  static int clone_config(const bishfpm_store_host_t *h,                       \
                          const bishfpm_config_t *m, const void *raw,          \
                          size_t n, void **out) {                              \
    config_t c;                                                                \
    if (!h || !out || !configure(m, raw, n, &c))                               \
      return 0;                                                                \
    config_t *owned = h->allocate(h->context, sizeof(*owned));                 \
    if (!owned)                                                                \
      return 0;                                                                \
    *owned = c;                                                                \
    *out = owned;                                                              \
    return 1;                                                                  \
  }                                                                            \
  static void destroy_config(const bishfpm_store_host_t *h, void *c) {         \
    h->release(h->context, c, sizeof(config_t));                               \
  }

typedef struct {
  uint64_t hash;
  uint32_t heap_idx;
  bool occupied;
} plugin_text_slot_t;
#endif

#ifdef PLUGIN_STORE_T
#ifndef PLUGIN_SLOTS
#define PLUGIN_SLOTS(s) ((s)->config->slots)
#endif
#ifndef PLUGIN_HALF_LIFE
#define PLUGIN_HALF_LIFE(s) ((s)->config->half_life_ms)
#endif
#ifndef PLUGIN_SLOT_T
#ifdef BISHFPM_USS_PAIR64
#define PLUGIN_SLOT_T uint32_t
#else
#define PLUGIN_SLOT_T plugin_text_slot_t
#endif
#endif
#ifdef BISHFPM_USS_PAIR64
typedef uint64_t plugin_key_t;
typedef PLUGIN_SLOT_T plugin_slot_t; /* zero empty; otherwise heap index + 1 */
static inline void copy_entry(PLUGIN_ENTRY_T *dst, const PLUGIN_ENTRY_T *src) {
  *dst = *src;
}
static inline uint64_t entry_hash(const PLUGIN_ENTRY_T *e) {
  uint64_t key = e->key;
  return bishfpm_splitmix64(&key);
}
static inline bool entry_matches(const PLUGIN_ENTRY_T *e, plugin_key_t key,
                                 size_t length) {
  (void)length;
  return e->key == key;
}
static inline void entry_identity(PLUGIN_ENTRY_T *e, plugin_key_t key,
                                  size_t length, uint64_t hash) {
  (void)length;
  (void)hash;
  e->key = key;
}
static inline bool slot_occupied(const plugin_slot_t *s) { return *s != 0; }
static inline uint32_t slot_index(const plugin_slot_t *s) { return *s - 1; }
static inline void slot_index_set(plugin_slot_t *s, uint32_t index) {
  *s = index + 1;
}
static inline uint64_t slot_hash(const PLUGIN_STORE_T *s, size_t slot) {
  return entry_hash(&s->heap[slot_index(&s->slot_map[slot])]);
}
#else
typedef const char *plugin_key_t;
typedef PLUGIN_SLOT_T plugin_slot_t;
static inline void copy_entry(PLUGIN_ENTRY_T *dst, const PLUGIN_ENTRY_T *src) {
  memcpy(dst, src, offsetof(PLUGIN_ENTRY_T, pattern) + src->length + 1);
}
static inline uint64_t entry_hash(const PLUGIN_ENTRY_T *e) { return e->hash; }
static inline bool entry_matches(const PLUGIN_ENTRY_T *e, plugin_key_t key,
                                 size_t length) {
  return e->length == length && memcmp(e->pattern, key, length) == 0;
}
static inline void entry_identity(PLUGIN_ENTRY_T *e, plugin_key_t key,
                                  size_t length, uint64_t hash) {
  memcpy(e->pattern, key, length);
  e->pattern[length] = '\0';
  e->length = (uint16_t)length;
  e->hash = hash;
}
static inline bool slot_occupied(const plugin_slot_t *s) { return s->occupied; }
static inline uint32_t slot_index(const plugin_slot_t *s) {
  return s->heap_idx;
}
static inline void slot_index_set(plugin_slot_t *s, uint32_t index) {
  s->heap_idx = index;
}
static inline uint64_t slot_hash(const PLUGIN_STORE_T *s, size_t slot) {
  return s->slot_map[slot].hash;
}
#endif

static int slot_map_find(const PLUGIN_STORE_T *s, plugin_key_t key,
                         size_t length, uint64_t hash, uint32_t *out_idx) {
  size_t slots = PLUGIN_SLOTS(s), mask = slots - 1;
  for (size_t i = 0; i < slots; i++) {
    size_t slot = (hash + i) & mask;
    if (!slot_occupied(&s->slot_map[slot]))
      return 0;
#ifndef BISHFPM_USS_PAIR64
    if (s->slot_map[slot].hash != hash)
      continue;
#endif
    uint32_t index = slot_index(&s->slot_map[slot]);
    if (entry_matches(&s->heap[index], key, length)) {
      if (out_idx)
        *out_idx = index;
      return 1;
    }
  }
  return 0;
}
static void slot_map_set(PLUGIN_STORE_T *s, uint64_t hash, uint32_t index) {
  size_t slots = PLUGIN_SLOTS(s), mask = slots - 1;
  for (size_t i = 0; i < slots; i++) {
    size_t slot = (hash + i) & mask;
    if (!slot_occupied(&s->slot_map[slot])) {
#ifndef BISHFPM_USS_PAIR64
      s->slot_map[slot].hash = hash;
      s->slot_map[slot].occupied = true;
#endif
      slot_index_set(&s->slot_map[slot], index);
      return;
    }
  }
}
static size_t slot_for_index(const PLUGIN_STORE_T *s, uint32_t index) {
  size_t mask = PLUGIN_SLOTS(s) - 1;
  size_t slot = entry_hash(&s->heap[index]) & mask;
  while (!slot_occupied(&s->slot_map[slot]) ||
         slot_index(&s->slot_map[slot]) != index)
    slot = (slot + 1) & mask;
  return slot;
}
static void slot_map_remove(PLUGIN_STORE_T *s, uint32_t index) {
  size_t mask = PLUGIN_SLOTS(s) - 1, j = slot_for_index(s, index);
  for (size_t k = (j + 1) & mask; slot_occupied(&s->slot_map[k]);
       k = (k + 1) & mask) {
    size_t orig = slot_hash(s, k) & mask;
    if (((k - orig) & mask) >= ((k - j) & mask)) {
      s->slot_map[j] = s->slot_map[k];
      j = k;
    }
  }
  s->slot_map[j] = (plugin_slot_t){0};
}
static void slot_map_rebuild(PLUGIN_STORE_T *s) {
  memset(s->slot_map, 0, PLUGIN_SLOTS(s) * sizeof(*s->slot_map));
  for (uint32_t i = 0; i < s->heap_size; i++)
    slot_map_set(s, entry_hash(&s->heap[i]), i);
}
static inline double item_rate(const PLUGIN_ENTRY_T *item, uint64_t now_ms,
                               double half_life_ms) {
  return decay_since(item->rate, item->last_time_ms, now_ms, half_life_ms);
}
static void heapify_up(PLUGIN_STORE_T *topk, uint32_t idx, uint64_t now_ms) {
  if (!idx)
    return;
  double half = PLUGIN_HALF_LIFE(topk);
  uint32_t parent = (idx - 1) / 2;
  double rate = item_rate(&topk->heap[idx], now_ms, half);
  if (!(rate < item_rate(&topk->heap[parent], now_ms, half)))
    return;
  PLUGIN_ENTRY_T held;
  copy_entry(&held, &topk->heap[idx]);
  size_t held_slot = slot_for_index(topk, idx);
  uint32_t hole = idx;
  for (;;) {
    size_t slot = slot_for_index(topk, parent);
    copy_entry(&topk->heap[hole], &topk->heap[parent]);
    slot_index_set(&topk->slot_map[slot], hole);
    hole = parent;
    if (!hole)
      break;
    parent = (hole - 1) / 2;
    if (!(rate < item_rate(&topk->heap[parent], now_ms, half)))
      break;
  }
  copy_entry(&topk->heap[hole], &held);
  slot_index_set(&topk->slot_map[held_slot], hole);
}
static uint32_t min_child(const PLUGIN_STORE_T *topk, uint32_t hole,
                          uint64_t now_ms, double half, double *out_rate) {
  uint32_t child = 2 * hole + 1;
  double rate = item_rate(&topk->heap[child], now_ms, half);
  if (child + 1 < topk->heap_size) {
    double right = item_rate(&topk->heap[child + 1], now_ms, half);
    if (right < rate) {
      child++;
      rate = right;
    }
  }
  *out_rate = rate;
  return child;
}
static void heapify_down(PLUGIN_STORE_T *topk, uint32_t idx, uint64_t now_ms) {
  if (2 * idx + 1 >= topk->heap_size)
    return;
  double half = PLUGIN_HALF_LIFE(topk), child_rate;
  double held_rate = item_rate(&topk->heap[idx], now_ms, half);
  uint32_t child = min_child(topk, idx, now_ms, half, &child_rate);
  if (!(child_rate < held_rate))
    return;
  PLUGIN_ENTRY_T held;
  copy_entry(&held, &topk->heap[idx]);
  size_t held_slot = slot_for_index(topk, idx);
  uint32_t hole = idx;
  for (;;) {
    size_t child_slot = slot_for_index(topk, child);
    copy_entry(&topk->heap[hole], &topk->heap[child]);
    slot_index_set(&topk->slot_map[child_slot], hole);
    hole = child;
    if (2 * hole + 1 >= topk->heap_size)
      break;
    child = min_child(topk, hole, now_ms, half, &child_rate);
    if (!(child_rate < held_rate))
      break;
  }
  copy_entry(&topk->heap[hole], &held);
  slot_index_set(&topk->slot_map[held_slot], hole);
}
#ifndef PLUGIN_NO_PRUNE
static size_t heap_prune(PLUGIN_STORE_T *s, uint64_t now, double cutoff,
                         bool zero_only) {
  double half = PLUGIN_HALF_LIFE(s);
  now = effective_time(now, s->max_time_ms);
  s->total = decay_since(s->total, s->max_time_ms, now, half);
  s->max_time_ms = now;
  uint32_t i = 0;
  size_t removed = 0;
  while (i < s->heap_size) {
    double r = item_rate(&s->heap[i], now, half);
    if (zero_only ? r == 0 : r < cutoff) {
      --s->heap_size;
      if (i < s->heap_size)
        copy_entry(&s->heap[i], &s->heap[s->heap_size]);
      removed++;
    } else
      i++;
  }
  if (removed) {
    slot_map_rebuild(s);
    for (uint32_t j = s->heap_size / 2; j > 0; j--)
      heapify_down(s, j - 1, now);
  }
  return removed;
}
#endif
#endif
