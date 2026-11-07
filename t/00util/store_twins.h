#ifndef BISHFPM_TEST_STORE_TWINS_H
#define BISHFPM_TEST_STORE_TWINS_H
/* test-only adapter: routes the same built-in algorithms through the public
 * extension ABI to isolate dispatch and result conversion overhead; private
 * layouts are intentional here while external examples use public headers */
#include "lib/store_plugin.h"
typedef struct {
  unsigned kind;
  uint32_t capacity, width;
  double forget_below;
} twin_options_t;
BISHFPM_PRIVATE extern const bishfpm_store_descriptor_t twin_text_store,
    twin_pair_store;
#endif
