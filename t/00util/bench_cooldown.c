/* Isolate cooldown churn from candidate serialization and store updates. */
#include "../../lib/bishfpm.c"
#include <assert.h>
#include <time.h>
static double ns(void) {
  struct timespec t;
  assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
  return t.tv_sec * 1e9 + t.tv_nsec;
}
int main(void) {
  bishfpm_group_t g = {0};
  g.cooldown_cap = COOLDOWN_GROWTH_LIMIT;
  g.cooldowns =
      calloc(g.cooldown_cap, cooldown_entry_size(false) + 2 * sizeof(uint32_t));
  assert(g.cooldowns);
  unsigned triggers = 0, suppressed = 0;
  double start = ns();
  for (unsigned round = 0; round < 10; round++) {
    for (unsigned i = 0; i < 4500; i++) {
      char pattern[32];
      snprintf(pattern, sizeof(pattern), "p%u", i);
      bool saturated = false;
      bool blocked = check_and_update_cooldown(
          &g, i, pattern, 1, 1000 + round * 100, 100, &saturated);
      triggers += !blocked;
      suppressed += saturated;
    }
  }
  printf("cooldown_ns_per_attempt=%.1f triggers=%u saturation=%u\n",
         (ns() - start) / 45000, triggers, suppressed);
  free_group_resources(&g);
}
