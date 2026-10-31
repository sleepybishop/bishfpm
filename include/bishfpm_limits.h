#ifndef BISHFPM_LIMITS_H
#define BISHFPM_LIMITS_H

/* Storage sizes include the terminating NUL. Group keys are a separate policy.
 */
#define BISHFPM_MAX_ITEM_LEN 256
#define BISHFPM_MAX_PATTERN_LEN 512
#define BISHFPM_MAX_GROUP_KEY_LEN 512
#define BISHFPM_MAX_ITEMSET_ORDER 32
/* Configurable per-call ceilings; defaults remain smaller. */
#define BISHFPM_MAX_EVENT_ITEMS 4096U
#define BISHFPM_MAX_EVENT_CANDIDATES 8388608U
#define BISHFPM_TOPK_MAX_STR BISHFPM_MAX_PATTERN_LEN

#endif
