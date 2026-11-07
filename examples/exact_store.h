#ifndef BISHFPM_EXAMPLE_EXACT_STORE_H
#define BISHFPM_EXAMPLE_EXACT_STORE_H
#include <bishfpm_store_plugin.h>
#ifdef __cplusplus
extern "C" {
#endif
/* deliberately simple, bounded linear store for extension conformance;
 * prune retains all identities/counts so absence remains exactly zero */
typedef struct {
  uint32_t capacity;
  uint32_t representation;
  const char *name; /* pointer-rich input: copied by clone_config */
} exact_store_options_t;
extern const bishfpm_store_descriptor_t exact_text_store, exact_pair_store;
#ifdef __cplusplus
}
#endif
#endif
