#ifndef BISHFPM_PRIVATE_STORE_PLUGIN_H
#define BISHFPM_PRIVATE_STORE_PLUGIN_H
#include "bishfpm_store_plugin.h"
#include "store.h"

typedef struct {
  bishfpm_store_descriptor_t descriptor;
  bishfpm_store_plan_t plan;
  bishfpm_store_host_t host;
  void *config;
  size_t bytes, limit;
  bishfpm_memory_budget_t *budget;
} store_extension_t;
BISHFPM_PRIVATE const store_ops_t *
store_extension_ops(const store_extension_t *);
BISHFPM_PRIVATE bool store_extension_validate(
    const bishfpm_store_descriptor_t *, const bishfpm_store_engine_options_t *,
    const bishfpm_config_t *, const void *, size_t, store_extension_t *);
BISHFPM_PRIVATE bool store_extension_init(store_extension_t *,
                                          bishfpm_memory_budget_t *,
                                          const bishfpm_config_t *,
                                          const void *, size_t);
BISHFPM_PRIVATE topk_result_t store_extension_update_count(
    const store_extension_t *, bishfpm_store_count_t, uint64_t);
#endif
