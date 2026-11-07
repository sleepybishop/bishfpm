# Writing a pattern store

`bishfpm_store_plugin.h` lets applications supply a custom pattern store to
`bishfpm_create_with_store` without changing mining policy or built-in store
layouts. Plugins are synchronous in-process C objects; see
`examples/exact_store.{c,h}` and `examples/store_plugin.c` for a minimal
implementation using only public headers.

## Descriptor and capabilities

Pass `bishfpm_create_with_store(cfg, engine_opts, descriptor, settings, size)`.
The engine copies the v1 descriptor prefix (`prune_bounded` is optional when
`struct_size` covers it) and calls `clone_config` so caller inputs can be freed
immediately after construction. `notification_capacity` in `engine_opts` sizes
cooldown storage (`next_pow2(2 * n)`, min 16, growth cap `max(initial, 4096)`)
and replaces `cfg.topk_k`; store residency capacity belongs in `settings`.

Each descriptor selects one key representation and one estimate mode:

| Flag / representation | Contract |
| --- | --- |
| `BISHFPM_STORE_TEXT` | Implements `update_text` / `query_text` over canonical NUL-terminated itemsets (`<= BISHFPM_MAX_PATTERN_LEN`). Advertises supported orders (`1..32`) in `plan`. |
| `BISHFPM_STORE_PAIR64` | Implements `update_pair` / `query_pair` over packed `uint64_t` item IDs (`(min_id << 32) \| max_id`). Requires `BISHFPM_STORE_STABLE_IDS` and order `2`. `UINT64_MAX` is reserved for queries with unknown item IDs. |
| `BISHFPM_STORE_RESIDENT_BOUNDS` | Certified interval store (`0 <= lower <= upper`, finite, `known = 1`). Optional `BISHFPM_STORE_ABSENT_BOUNDS` when absent keys also have certified upper bounds. |
| `BISHFPM_STORE_HEURISTIC_ESTIMATES` | Uncertified estimate store (`known = 0`, `lower = 0`, finite `upper >= 0`). Public bounds report `known = 0` with `[0, +infinity]`. |
| `BISHFPM_STORE_ALERT_EVIDENCE` | Requires `BISHFPM_STORE_HEURISTIC_ESTIMATES`. `upper` holds the discovery estimate; `lower` holds decayed matching-update mass since the resident's latest admission (used for promotion, lift, subsumption, and callback `pattern_rate`). |

## Keys, time, and update outcomes

- **Identity ownership**: Text keys passed to `update_text` are borrowed; copy
  them if retained. Treat the supplied 64-bit hash as an accelerator and compare
  full keys on collision.
- **Clocks and decay**: The host clamps timestamps to the group watermark and
  discounts late event weights before calling the store. Do not discount weight
  a second time. Queries and zero effective weights (`BISHFPM_UPDATE_ZERO`) must
  not mutate state, RNGs, or clocks.
- **Outcomes**: Return `BISHFPM_UPDATE_RESIDENT`, `ADMITTED`, `REPLACED`,
  `FILTERED`, `REJECTED` (on guarded numeric overflow), or `ZERO`. Resident
  membership (`resident = 1`) is required for alert promotion.

## Event lifecycle

For each accepted event, the host runs:

1. `prepare_event(group_state, view, &token)`: Return `1` (ok), `0` (capacity
   rejection), or `-1` (allocation failure). Perform all fallible per-event
   reservations here before counts mutate. `view` arrays are borrowed for the
   duration of this call only.
2. Marginal table reservation: If marginal reservation fails, the host calls
   `cancel_event(group_state, token)` and rolls back.
3. `commit_prepared(group_state, token)`: Commits any preflight pruning or
   reservation. Commit, candidate updates, and `finish_event(group_state, token)`
   cannot fail the event.

Candidates are updated one by one in canonical order; callbacks can query the
engine during ingestion and observe intermediate state.

## Memory and maintenance

- **Host allocator**: Allocate all cloned config, group state, and tokens via
  `bishfpm_store_host_t` (`allocate`, `zeroed`, `release` with exact byte size).
  Failed `clone_config` or `create` calls must free any partial allocations
  before returning `0`.
- **Sizing (`plan`)**: Report `engine_peak_bytes` and `group_peak_bytes`
  (including transient reservation/maintenance buffers). Conservative memory
  mode requires finite bounds; dynamic mode allows `SIZE_MAX` and enforces the
  budget on each allocation.
- **Pruning**: `prune(state, now_ms, min_rate)` must not require new allocations
  and must preserve any absent-history upper bounds. Pair stores may also
  provide `prune_bounded` for `bishfpm_prune_pairs`.
