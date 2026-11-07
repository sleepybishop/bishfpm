#include "exact_store.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
  exact_store_options_t options;
  double half;
  size_t name_bytes;
} config_t;
typedef struct {
  uint64_t pair, time;
  double count;
  char text[BISHFPM_MAX_PATTERN_LEN];
} entry_t;
typedef struct {
  bishfpm_store_host_t host;
  const config_t *config;
  size_t count;
  void *token;
  uint64_t prepared, canceled, committed, finished;
  entry_t entries[];
} state_t;
static int plan(const bishfpm_config_t *c, const void *raw, size_t bytes,
                bishfpm_store_plan_t *out) {
  if (!c || !raw || !out || bytes != sizeof(exact_store_options_t))
    return 0;
  const exact_store_options_t *o = raw;
  bool pair = o->representation == BISHFPM_STORE_PAIR64;
  if (!o->capacity || o->capacity > 1000000 || !o->name ||
      strlen(o->name) > 255 ||
      (o->representation != BISHFPM_STORE_TEXT && !pair) ||
      (pair && (c->min_itemset_order != 2 || c->max_itemset_order != 2)))
    return 0;
  size_t entries = (size_t)o->capacity * sizeof(entry_t);
  if (entries / sizeof(entry_t) != o->capacity ||
      entries > SIZE_MAX - sizeof(state_t) - sizeof(uint64_t))
    return 0;
  *out = (bishfpm_store_plan_t){sizeof(config_t) + strlen(o->name) + 1,
                                sizeof(state_t) +
                                    (size_t)o->capacity * sizeof(entry_t) +
                                    sizeof(uint64_t),
                                pair ? 2 : 1,
                                pair ? 2 : BISHFPM_MAX_ITEMSET_ORDER,
                                {0, 0}};
  return 1;
}
static int text_plan(const bishfpm_config_t *c, const void *raw, size_t n,
                     bishfpm_store_plan_t *p) {
  return plan(c, raw, n, p) &&
         ((const exact_store_options_t *)raw)->representation ==
             BISHFPM_STORE_TEXT;
}
static int pair_plan(const bishfpm_config_t *c, const void *raw, size_t n,
                     bishfpm_store_plan_t *p) {
  return plan(c, raw, n, p) &&
         ((const exact_store_options_t *)raw)->representation ==
             BISHFPM_STORE_PAIR64;
}
static int clone(const bishfpm_store_host_t *h, const bishfpm_config_t *mining,
                 const void *raw, size_t bytes, void **out) {
  (void)bytes;
  const exact_store_options_t *o = raw;
  config_t *c = h->allocate_zeroed(h->context, 1, sizeof(*c));
  if (!c)
    return 0;
  c->options = *o;
  c->half = mining->ewma_half_life_ms;
  c->name_bytes = strlen(o->name) + 1;
  char *name = h->allocate(h->context, c->name_bytes);
  if (!name) {
    h->release(h->context, c, sizeof(*c));
    return 0;
  }
  memcpy(name, o->name, c->name_bytes);
  c->options.name = name;
  *out = c;
  return 1;
}
static void destroy_config(const bishfpm_store_host_t *h, void *raw) {
  config_t *c = raw;
  h->release(h->context, (void *)c->options.name, c->name_bytes);
  h->release(h->context, c, sizeof(*c));
}
static int create(const bishfpm_store_host_t *h, const void *raw, void **out) {
  const config_t *c = raw;
  size_t bytes =
      sizeof(state_t) + (size_t)c->options.capacity * sizeof(entry_t);
  state_t *s = h->allocate_zeroed(h->context, 1, bytes);
  if (!s)
    return 0;
  s->host = *h;
  s->config = c;
  *out = s;
  return 1;
}
static void destroy(const bishfpm_store_host_t *h, void *raw) {
  state_t *s = raw;
  assert(!s->token && s->prepared == s->canceled + s->finished &&
         s->committed == s->finished);
  h->release(h->context, s,
             sizeof(*s) +
                 (size_t)s->config->options.capacity * sizeof(entry_t));
}
static size_t find(const state_t *s, const char *text, size_t length,
                   uint64_t pair) {
  for (size_t i = 0; i < s->count; i++)
    if (text ? (strlen(s->entries[i].text) == length &&
                memcmp(s->entries[i].text, text, length) == 0)
             : s->entries[i].pair == pair)
      return i;
  return s->count;
}
static uint64_t pack(uint32_t a, uint32_t b) {
  return a < b ? ((uint64_t)a << 32) | b : ((uint64_t)b << 32) | a;
}
static size_t missing_text(const state_t *s, const bishfpm_store_event_t *e,
                           size_t order, size_t depth, size_t start, char *text,
                           size_t length) {
  if (depth == order) {
    text[length] = 0;
    return find(s, text, length, 0) == s->count;
  }
  size_t missing = 0;
  for (size_t i = start; i + order - depth <= e->count; i++) {
    size_t pos = length;
    if (depth)
      text[pos++] = '\037';
    memcpy(text + pos, e->items[i], e->lengths[i]);
    missing +=
        missing_text(s, e, order, depth + 1, i + 1, text, pos + e->lengths[i]);
  }
  return missing;
}
static int prepare(void *raw, const bishfpm_store_event_t *e, void **token) {
  state_t *s = raw;
  assert(!s->token);
  size_t missing = 0;
  if (e->weight != 0) {
    if (s->config->options.representation == BISHFPM_STORE_PAIR64) {
      for (size_t i = 0; i < e->count; i++)
        for (size_t j = i + 1; j < e->count; j++)
          missing += find(s, NULL, 0, pack(e->ids[i], e->ids[j])) == s->count;
    } else {
      char text[BISHFPM_MAX_PATTERN_LEN];
      for (size_t k = e->min_order; k <= e->max_order && k <= e->count; k++)
        missing += missing_text(s, e, k, 0, 0, text, 0);
    }
  }
  if (missing > s->config->options.capacity - s->count)
    return 0;
  void *p = s->host.allocate(s->host.context, sizeof(uint64_t));
  if (!p)
    return -1;
  s->token = p;
  s->prepared++;
  *token = p;
  return 1;
}
static void cancel(void *raw, void *token) {
  state_t *s = raw;
  assert(token == s->token && token);
  s->host.release(s->host.context, token, sizeof(uint64_t));
  s->token = NULL;
  s->canceled++;
}
static void commit(void *raw, void *token) {
  (void)token;
  state_t *s = raw;
  assert(token == s->token && token);
  s->committed++;
}
static void finish(void *raw, void *token) {
  state_t *s = raw;
  assert(token == s->token && token);
  s->host.release(s->host.context, token, sizeof(uint64_t));
  s->token = NULL;
  s->finished++;
}
static double value(const state_t *s, const entry_t *e, uint64_t now) {
  return e->count *
         exp2(-(double)(now > e->time ? now - e->time : 0) / s->config->half);
}
static bishfpm_store_count_t query(const state_t *s, const char *text,
                                   size_t length, uint64_t pair, uint64_t now) {
  size_t i = find(s, text, length, pair);
  double count = i == s->count ? 0 : value(s, &s->entries[i], now);
  bishfpm_store_count_t result = {
      count, count, i != s->count, 1, BISHFPM_UPDATE_ZERO, 0};
  return result;
}
static bishfpm_store_count_t update(state_t *s, const char *text, size_t length,
                                    uint64_t pair, double weight,
                                    uint64_t now) {
  if (!weight)
    return query(s, text, length, pair, now);
  if (!(weight > 0))
    return (bishfpm_store_count_t){0, 0, 0, 0, BISHFPM_UPDATE_REJECTED, 0};
  size_t i = find(s, text, length, pair);
  double before = i == s->count ? 0 : value(s, &s->entries[i], now);
  if (weight > DBL_MAX - before) {
    bishfpm_store_count_t r = query(s, text, length, pair, now);
    r.outcome = BISHFPM_UPDATE_REJECTED;
    return r;
  }
  uint32_t outcome = BISHFPM_UPDATE_RESIDENT;
  if (i == s->count) {
    assert(i < s->config->options.capacity);
    s->count++;
    if (text) {
      memcpy(s->entries[i].text, text, length);
      s->entries[i].text[length] = 0;
    } else
      s->entries[i].pair = pair;
    outcome = BISHFPM_UPDATE_ADMITTED;
  }
  s->entries[i].count = before + weight;
  s->entries[i].time = now;
  bishfpm_store_count_t result = {
      before + weight, before + weight, 1, 1, outcome, 0};
  return result;
}
static bishfpm_store_count_t update_text(void *s, const char *key, size_t n,
                                         uint64_t hash, double w,
                                         uint64_t now) {
  (void)hash;
  if (!key || !n || n >= BISHFPM_MAX_PATTERN_LEN || !key[0])
    return (bishfpm_store_count_t){
        0, 0, 0, 0, w == 0 ? BISHFPM_UPDATE_ZERO : BISHFPM_UPDATE_REJECTED, 0};
  return update(s, key, n, 0, w, now);
}
static bishfpm_store_count_t update_pair(void *s, uint64_t key, uint64_t hash,
                                         double w, uint64_t now) {
  (void)hash;
  return update(s, NULL, 0, key, w, now);
}
static bishfpm_store_count_t query_text(const void *s, const char *key,
                                        size_t n, uint64_t hash, uint64_t now) {
  (void)hash;
  if (!key || !n || n >= BISHFPM_MAX_PATTERN_LEN || !key[0])
    return (bishfpm_store_count_t){0, 0, 0, 0, BISHFPM_UPDATE_ZERO, 0};
  return query(s, key, n, 0, now);
}
static bishfpm_store_count_t query_pair(const void *s, uint64_t key,
                                        uint64_t hash, uint64_t now) {
  (void)hash;
  return query(s, NULL, 0, key, now);
}
static size_t prune(void *s, uint64_t now, double cutoff) {
  (void)s;
  (void)now;
  (void)cutoff;
  return 0;
}
static void snapshot(const void *raw, bishfpm_store_snapshot_t *out) {
  const state_t *s = raw;
  *out = (bishfpm_store_snapshot_t){
      s->count,
      s->config->options.capacity,
      {s->prepared, s->canceled, s->committed, s->finished}};
}
const bishfpm_store_descriptor_t exact_text_store = {
    BISHFPM_STORE_ABI_V1,
    sizeof(bishfpm_store_descriptor_t),
    BISHFPM_STORE_TEXT,
    0,
    BISHFPM_STORE_RESIDENT_BOUNDS | BISHFPM_STORE_ABSENT_BOUNDS,
    {0, 0},
    text_plan,
    clone,
    destroy_config,
    create,
    destroy,
    prepare,
    cancel,
    commit,
    finish,
    update_text,
    NULL,
    query_text,
    NULL,
    prune,
    snapshot,
    NULL};
const bishfpm_store_descriptor_t exact_pair_store = {
    BISHFPM_STORE_ABI_V1,
    sizeof(bishfpm_store_descriptor_t),
    BISHFPM_STORE_PAIR64,
    0,
    BISHFPM_STORE_RESIDENT_BOUNDS | BISHFPM_STORE_ABSENT_BOUNDS |
        BISHFPM_STORE_STABLE_IDS,
    {0, 0},
    pair_plan,
    clone,
    destroy_config,
    create,
    destroy,
    prepare,
    cancel,
    commit,
    finish,
    NULL,
    update_pair,
    NULL,
    query_pair,
    prune,
    snapshot,
    NULL};
