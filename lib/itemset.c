#include "bishfpm_itemset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* string comparison helper for qsort */
static int cmp_str(const void *a, const void *b) {
  const char *const *sa = (const char *const *)a;
  const char *const *sb = (const char *const *)b;
  return strcmp(*sa, *sb);
}

/* sort array of item pointers */
void bishfpm_itemset_sort(const char **items, size_t num_items) {
  if (!items || num_items <= 1) {
    return;
  }
  qsort(items, num_items, sizeof(const char *), cmp_str);
}

/* serialize sorted items into delimited buffer */
size_t bishfpm_itemset_serialize(const char *const *items, size_t num_items,
                                 char *out_buf, size_t buf_len) {
  if (!out_buf || buf_len == 0) {
    return 0;
  }
  if (!items || num_items == 0) {
    out_buf[0] = '\0';
    return 0;
  }

  size_t pos = 0;
  const char *prev = NULL;

  for (size_t i = 0; i < num_items; i++) {
    if (!items[i] || items[i][0] == '\0') {
      continue;
    }
    /* reject items containing unit delimiter to prevent aliasing */
    if (strchr(items[i], BISHFPM_ITEM_DELIM) != NULL) {
      out_buf[0] = '\0';
      return 0;
    }
    /* skip consecutive duplicates */
    if (prev && strcmp(prev, items[i]) == 0) {
      continue;
    }

    size_t len = strlen(items[i]);
    if (pos > 0) {
      if (pos + 1 >= buf_len) {
        out_buf[0] = '\0';
        return 0;
      }
      out_buf[pos++] = BISHFPM_ITEM_DELIM;
    }

    if (pos + len >= buf_len) {
      out_buf[0] = '\0';
      return 0;
    }

    memcpy(out_buf + pos, items[i], len);
    pos += len;
    prev = items[i];
  }

  out_buf[pos] = '\0';
  return pos;
}

/* count items in serialized pattern */
size_t bishfpm_itemset_count(const char *pattern) {
  if (!pattern || pattern[0] == '\0') {
    return 0;
  }
  size_t count = 1;
  const char *p = pattern;
  while (*p) {
    if (*p == BISHFPM_ITEM_DELIM) {
      count++;
    }
    p++;
  }
  return count;
}

/* format serialized pattern into human-readable string */
size_t bishfpm_itemset_format(const char *pattern, char *out_buf,
                              size_t buf_len) {
  if (!out_buf || buf_len == 0) {
    return 0;
  }
  if (!pattern || pattern[0] == '\0') {
    out_buf[0] = '\0';
    return 0;
  }

  size_t pos = 0;
  const char *p = pattern;
  while (*p && pos + 1 < buf_len) {
    if (*p == BISHFPM_ITEM_DELIM) {
      if (pos + 2 < buf_len) {
        out_buf[pos++] = ',';
        out_buf[pos++] = ' ';
      } else {
        break;
      }
    } else {
      out_buf[pos++] = *p;
    }
    p++;
  }
  out_buf[pos] = '\0';
  return pos;
}

/* generate combinations of exact length k */
static void generate_combinations_exact(const char *const *items,
                                        size_t num_items, size_t target_k,
                                        size_t start_idx,
                                        const char **cur_items, size_t cur_len,
                                        bishfpm_subset_cb cb, void *udata) {
  if (cur_len == target_k) {
    cb(cur_items, cur_len, udata);
    return;
  }
  size_t needed = target_k - cur_len;
  for (size_t i = start_idx; i + needed <= num_items; i++) {
    cur_items[cur_len] = items[i];
    generate_combinations_exact(items, num_items, target_k, i + 1, cur_items,
                                cur_len + 1, cb, udata);
  }
}

/* generate subsets with lengths from min_k to max_k in increasing order */
void bishfpm_generate_subsets_order(const char *const *items, size_t num_items,
                                    size_t min_k, size_t max_k,
                                    bishfpm_subset_cb cb, void *udata) {
  if (!items || num_items == 0 || max_k == 0 || !cb) {
    return;
  }
  if (min_k < 1) {
    min_k = 1;
  }
  if (max_k > num_items) {
    max_k = num_items;
  }
  if (max_k > BISHFPM_MAX_ITEMSET_ORDER) {
    max_k = BISHFPM_MAX_ITEMSET_ORDER;
  }
  if (min_k > max_k) {
    return;
  }

  const char *scratch[BISHFPM_MAX_ITEMSET_ORDER];
  for (size_t k = min_k; k <= max_k; k++) {
    generate_combinations_exact(items, num_items, k, 0, scratch, 0, cb, udata);
  }
}

/* generate all non-empty subsets up to max_k in increasing order */
void bishfpm_generate_subsets(const char *const *items, size_t num_items,
                              size_t max_k, bishfpm_subset_cb cb, void *udata) {
  bishfpm_generate_subsets_order(items, num_items, 1, max_k, cb, udata);
}
