#!/bin/sh
set -eu
stage=$(mktemp -d "${TMPDIR:-/tmp}/bishfpm-install-XXXXXX")
trap 'rm -rf "$stage"' EXIT HUP INT TERM
make install DESTDIR="$stage" PREFIX=/usr LIBDIR=/usr/lib INCLUDEDIR=/usr/include
cat > "$stage/smoke.c" <<'C'
#include <bishfpm.h>
#include <string.h>
#if defined(BISHFPM_ARENA_H) || defined(BISHFPM_HASH_H) || defined(BISHFPM_RATE_H) || defined(BISHFPM_ITEMSET_H) || defined(BISHFPM_TOPK_H) || defined(BISHFPM_PAIRS_H)
#error engine header exposes low-level headers
#endif
#ifdef _Alignof
#error leaked macro
#endif
int main(void) {
  bishfpm_config_t cfg;
  bishfpm_config_default(&cfg);
  bishfpm_t *engine = bishfpm_create(&cfg);
  if (!engine) return 1;
  const char *items[] = {"a", "b"};
  int result = bishfpm_add_event(engine, "g", 0, items, 2, NULL, NULL);
  bishfpm_rejection_stats_t reasons;
  int rejected = bishfpm_add_event(engine, NULL, 0, items, 2, NULL, NULL);
  bishfpm_get_rejection_stats(engine, &reasons);
  bishfpm_free(engine);
  cfg.min_itemset_order = cfg.max_itemset_order = 2;
  cfg.item_rate_threshold = 0;
  cfg.max_groups = 1;
  bishfpm_pair_options_t options = {BISHFPM_PAIR_THRESHOLD, 16, 0, .25};
  engine = bishfpm_create_pairs(&cfg, &options);
  if (!engine) return 3;
  if (bishfpm_add_event(engine, "g", 0, items, 2, NULL, NULL) < 0) return 4;
  bishfpm_count_bounds_t bounds;
  if (!bishfpm_pattern_bounds(engine, "g", "a\x1f" "b", 0, &bounds) ||
      !bounds.known || bounds.lower != 1 || bounds.upper != 1) return 5;
  bishfpm_pair_stats_t pair_stats;
  bishfpm_get_pair_stats(engine, &pair_stats);
  if (pair_stats.resident_pairs != 1 || pair_stats.capacity_rejections) return 6;
  if (bishfpm_prune_pairs(engine, "g", 15000, 1) != 1) return 7;
  bishfpm_free(engine);
  options.store = BISHFPM_PAIR_FSS;
  options.capacity = 1;
  options.sketch_width = 3;
  engine = bishfpm_create_pairs(&cfg, &options);
  if (!engine) return 8;
  const char *other[] = {"a", "c"};
  if (bishfpm_add_event_weighted(engine, "g", 0, items, 2, 10, NULL, NULL) < 0 ||
      bishfpm_add_event(engine, "g", 0, other, 2, NULL, NULL) < 0) return 9;
  if (!bishfpm_pattern_bounds(engine, "g", "a\x1f" "c", 0, &bounds) ||
      !bounds.known || bounds.resident || bounds.upper < 1 ||
      bishfpm_pattern_rate(engine, "g", "a\x1f" "c", 0) != bounds.upper) return 10;
  bishfpm_free(engine);
  bishfpm_options_t general;
  bishfpm_options_default(&general);
  general.store = BISHFPM_STORE_FSS;
  general.sketch_width = 17;
  general.memory_mode = BISHFPM_MEMORY_DYNAMIC;
  cfg.topk_k = 8;
  cfg.max_groups = 1000;
  cfg.max_items_per_group = 16;
  cfg.max_event_items = 16;
  cfg.max_memory_bytes = 256U << 10;
  engine = bishfpm_create_with_options(&cfg, &general);
  if (!engine) return 11;
  if (bishfpm_add_event(engine, "g", 0, items, 2, NULL, NULL) < 0) return 12;
  bishfpm_vocabulary_stats_t vocabulary;
  if (!bishfpm_get_vocabulary_stats(engine, "g", &vocabulary) ||
      vocabulary.retained_items != 2 || vocabulary.remaining_items != 14) return 13;
  bishfpm_memory_stats_t memory;
  bishfpm_pattern_pressure_t pressure;
  if (!bishfpm_get_pattern_pressure(engine, "g", &pressure) ||
      pressure.candidates_evaluated != 1 || pressure.admissions != 1 ||
      pressure.resident_patterns != 1 || pressure.resident_capacity != 8) return 16;
  bishfpm_get_memory_stats(engine, &memory);
  if (memory.current_bytes > memory.peak_bytes || memory.peak_bytes > memory.limit_bytes) return 14;
  if (!bishfpm_pattern_bounds(engine, "g", "a\037b", 0, &bounds) || !bounds.known) return 15;
  bishfpm_free(engine);
  return rejected != -1 || reasons.invalid_input != 1 || result < 0 ||
         bishfpm_prune_pairs(NULL, "g", 0, 1) != 0 ||
         strcmp(bishfpm_version(), BISHFPM_VERSION_STRING) ||
         strcmp(BISHFPM_VERSION_STRING, EXPECT_VERSION);
}
C
# Compile the backend using only staged public headers, outside the checkout.
cp examples/exact_store.c examples/exact_store.h examples/store_plugin.c "$stage/"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -pedantic-errors -UNDEBUG \
  -I"$stage/usr/include" -I"$stage" -c "$stage/exact_store.c" -o "$stage/exact_store.o"
version=$(PKG_CONFIG_LIBDIR="$stage/usr/lib/pkgconfig" pkg-config --modversion bishfpm)
run_compiler() {
  # CC/CXX are shell commands, as in make; keep consumer arguments positional.
  sh -c "$compiler \"\$@\"" bishfpm-compiler "$@"
}
for language in c c++; do
  if [ "$language" = c ]; then compiler=${CC:-cc}; standard=c11
  else compiler=${CXX:-c++}; standard=c++11; fi
  # Each installed header must stand on its own in either language.
  for header in "$stage"/usr/include/*.h; do
    printf '#include <%s>\nint main(void){return 0;}\n' "$(basename "$header")" > "$stage/header.c"
    run_compiler -x "$language" -std="$standard" -Wall -Wextra -pedantic-errors \
      -I"$stage/usr/include" -fsyntax-only "$stage/header.c"
  done
  # The public handles deliberately have no visible storage layout.
  for handle in bishfpm_t bishfpm_rate_t bishfpm_rate_table_t bishfpm_topk_t bishfpm_arena_t bishfpm_pair_counter_t; do
    printf '#include <bishfpm.h>\n#include <bishfpm_lowlevel.h>\nint main(void){return sizeof(%s);}\n' "$handle" > "$stage/opaque.c"
    if run_compiler -x "$language" -std="$standard" -I"$stage/usr/include" \
        -fsyntax-only "$stage/opaque.c" > "$stage/opaque.log" 2>&1; then
      printf 'Public storage unexpectedly exposed for %s\n' "$handle" >&2
      exit 1
    fi
  done
  for linkage in static shared; do
    if [ "$linkage" = static ]; then library="$stage/usr/lib/libbishfpm.a"
    else library="$stage/usr/lib/libbishfpm.so"; fi
    run_compiler -x "$language" -std="$standard" -Wall -Wextra -pedantic-errors \
      -I"$stage/usr/include" "-DEXPECT_VERSION=\"$version\"" "$stage/smoke.c" \
      -x none "$library" -lm -o "$stage/smoke"
    LD_LIBRARY_PATH="$stage/usr/lib" "$stage/smoke"
    # Keep executable checks active even if CC/CXX defines NDEBUG.
    run_compiler -x "$language" -std="$standard" -Wall -Wextra -pedantic-errors \
      -UNDEBUG -I"$stage/usr/include" t/00util/test_lowlevel.c \
      -x none "$library" -lm -o "$stage/lowlevel"
    LD_LIBRARY_PATH="$stage/usr/lib" "$stage/lowlevel"
    run_compiler -x "$language" -std="$standard" -Wall -Wextra -pedantic-errors \
      -I"$stage/usr/include" -I"$stage" "$stage/store_plugin.c" \
      -x none "$stage/exact_store.o" "$library" -lm -o "$stage/plugin"
    LD_LIBRARY_PATH="$stage/usr/lib" "$stage/plugin"
  done
done
test ! -e "$stage/usr/include/internal.h"
test ! -e "$stage/usr/include/state.h"
test ! -e "$stage/usr/include/store.h"
test ! -e "$stage/usr/include/store_plugin.h"
if nm -D --defined-only "$stage/usr/lib/libbishfpm.so" | \
    awk '{print $3}' | \
    grep -Eq '^(topk_add_result|topk_add_sized|topk_query_result|rate_table_find|rate_table_new_budget|rate_table_prepare_result|arena_new_budget|bishfpm_arena_init|compact_.*|store_.*)$'; then
  printf 'Private helper exported from shared library\n' >&2
  exit 1
fi
soname=$(readelf -d "$stage/usr/lib/libbishfpm.so" | sed -n 's/.*(SONAME).*\[\(.*\)\].*/\1/p')
expected_soversion="${1:-$(sed -n 's/^SOVERSION[[:space:]]*=[[:space:]]*//p' Makefile 2>/dev/null || echo 0.2)}"
test "$soname" = "libbishfpm.so.$expected_soversion"
test "$(readlink "$stage/usr/lib/libbishfpm.so")" = "$soname"
test -f "$stage/usr/lib/$soname"
printf 'Installed C/C++ static/shared consumers and version %s passed.\n' "$version"
