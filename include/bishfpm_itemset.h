#ifndef BISHFPM_ITEMSET_H
#define BISHFPM_ITEMSET_H

#include "bishfpm_limits.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BISHFPM_ITEM_DELIM '\x1f'

/* subset generation callback */
typedef void (*bishfpm_subset_cb)(const char *const *subset_items,
                                  size_t subset_len, void *udata);

/* sort array of item strings in-place */
void bishfpm_itemset_sort(const char **items, size_t num_items);

/* serialize sorted items into canonical string */
size_t bishfpm_itemset_serialize(const char *const *items, size_t num_items,
                                 char *out_buf, size_t buf_len);

/* count items in serialized pattern */
size_t bishfpm_itemset_count(const char *pattern);

/* format serialized pattern into human-readable comma separated string */
size_t bishfpm_itemset_format(const char *pattern, char *out_buf,
                              size_t buf_len);

/* generate all non-empty subsets up to max_k */
void bishfpm_generate_subsets(const char *const *items, size_t num_items,
                              size_t max_k, bishfpm_subset_cb cb, void *udata);

/* generate subsets with lengths from min_k to max_k */
void bishfpm_generate_subsets_order(const char *const *items, size_t num_items,
                                    size_t min_k, size_t max_k,
                                    bishfpm_subset_cb cb, void *udata);

#ifdef __cplusplus
}
#endif

#endif /* BISHFPM_ITEMSET_H */
