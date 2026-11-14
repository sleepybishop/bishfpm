# Design and operating contract

Bishfpm tracks recurring combinations of categorical attributes within groups.
Each group has its own decayed counts, candidate store, and notification state.
The engine turns incoming events into count updates and synchronous alerts;
the application chooses the groups, thresholds, and meaning of those alerts.

## Event processing

```mermaid
flowchart TD
    A["Incoming event"] --> B["Validate, sort, and deduplicate items"]
    B --> C["Check work limits and reserve group/dictionary capacity"]
    C --> D["Commit group and item counts at the group clock"]
    D --> E["Select items passing count/share gates"]
    E --> F["Generate and update a candidate"]
    F --> G["Check promotion, lift, optional partition lift, subset suppression, and cooldown"]
    G --> H["Deliver callback if eligible"]
    H --> I{"More candidates?"}
    I -->|Yes| F
    I -->|No| J["Return alert decision count"]
```

Preparation checks the complete event before committing counts. It validates
encoded lengths, bounds candidate work, and reserves all missing dictionary
keys. Stores that require capacity reservation also check whether the complete
event can fit. Failure at this stage rejects the whole event.

After reservation, the engine updates the group count and every distinct item's
count. Items passing the marginal count/share gates participate in candidate
generation. The general engine enumerates configured itemset orders from smallest
to largest; numeric engines enumerate pairs directly.

Each candidate update and its notification finish before the next candidate is
updated. A callback querying the engine sees that intermediate state. Processing
all candidates first and delivering callbacks afterward would change this contract.

## Counts and time

All count thresholds and query results use exponentially decayed counts. With
half-life `H` and `lambda = ln(2) / H`, event weights `w_i` at times `t_i`
contribute to the count at time `T`:

```text
decay(dt) = exp(-lambda * dt) = 2^(-dt / H)
C(T)      = sum_i(w_i * decay(T - t_i))
```

Times and half-life are in milliseconds. After one half-life, a contribution of
10 becomes 5. Exponential decay permits an incremental update without retaining
the events: advance the previous count by multiplying by `decay(dt)`, then add
the new weight. Decay factors compose:

```text
decay(a + b) = decay(a) * decay(b)
```

Advancing in several steps therefore gives the same count as advancing once,
up to floating-point rounding. At a common time, aging multiplies counts and
their error bounds by the same positive factor, preserving inequalities and
count ratios until new input or pruning changes them.

### Relating counts to arrival rates

Counts measure weighted arrival history. The exponential kernel has total mass
`1 / lambda` milliseconds, so `C(T) * 1000 * lambda` gives a normalized smoothed
per-second rate estimate.

For an unfiltered counter receiving stationary Poisson arrivals at `r` events
per second, with unit weights, the steady-state moments at an arbitrary
observation time are:

```text
E[C]   = r * H / (1000 * ln(2))
Var[C] = r * H / (2000 * ln(2))
```

A longer half-life accumulates more mass and responds more slowly to changes.
For example, 10 events/second with a 1,000 ms half-life gives an expected count
of about 14.43. This calibrates thresholds; an individual observation fluctuates.

Periodic arrivals have a different shape. With one unit event every `d`
milliseconds, the steady-state count immediately after an arrival is
`1 / (1 - exp(-lambda * d))`. Substituting the mean Poisson spacing into that
formula does not give the Poisson expectation. These formulas describe the
underlying counter; gates, retention uncertainty, and pruning affect pattern
estimates.

### Watermarks and late input

Each group maintains a monotonic watermark. Accepted events and successful
maintenance advance it; queries and no-op calls do not. Timestamp zero is valid.
Late events are discounted to the watermark before updating counts or evaluating
gates and alerts. Callback timestamps use this effective group time.

Queries before the watermark clamp to it. Queries into the future decay the
current state without changing the watermark. Historical state cannot be
reconstructed by querying an old timestamp. Standalone rate counters and top-k
stores maintain their own clocks.

Engine event weights are integers through `UINT32_MAX`. Standalone floating
weight APIs require finite, nonnegative weights and a finite, positive half-life.
Double precision limits apply: unit increments may lose resolution above `2^53`,
and sufficiently old contributions underflow to zero.

## How the stages work together

The engine separates which combinations to examine, how much recent support
they have, and whether to notify the application. Each stage addresses a
different part of the problem.

### Limit discovery work before counting combinations

A candidate is a combination of distinct items from the current event, within
the configured itemset orders. An item is eligible when its updated marginal
count meets `item_rate_threshold` and, when enabled, its share of the updated
group count meets `item_share_threshold`. Every item in a candidate must qualify.
These gates include the current event's contribution.

An event with `M` distinct items has `2^M - 1` nonempty subsets. Limiting itemset
order and selecting items through marginal gates reduces the combinations sent
to the store. With `m` eligible items and pair-only tracking, this becomes
`m * (m - 1) / 2` candidate updates.

The mathematical motivation is that a combination cannot occur more often than
any of its constituent items when counted over the same history. However, the
online gate is a deliberate approximation: combinations receive no updates
before all their items qualify. Passing a gate later cannot recover that earlier
history. The work budget separately rejects an event whose worst-case expansion
is too large, even if gates might have reduced its actual expansion.

### Compare recent support within a group

Group, item, and pattern counts use the same half-life and effective time. This
makes ratios compare mass over the same aging rule. Let `G` be group count,
`I_i` item counts, and `L` a conservative pattern count. The decision quantities
are:

```text
absolute support         = L
group share              = L / G
independence expectation = G * product_i(I_i / G)
lift                     = L / independence expectation
```

Lift defaults to `1.0` for singletons or nonpositive count inputs. Positive
counts use scaled arithmetic when needed to avoid intermediate underflow;
lift above the representable double range is positive infinity. Group share is
zero when the group count is zero.

Absolute support requires enough observed mass. Group share asks whether the
pattern accounts for a substantial part of this group's activity. Lift asks
whether co-occurrence exceeds the expectation from the current item marginals.
For example, `G=1000`, `I_a=200`, `I_b=100`, and `L=50` give 5% group share and
lift 2.5: the pair is concentrated relative to independence while representing
a modest share of the group. The application chooses which conditions matter.
Lift uses the maintained marginals, including any effects of marginal pruning;
it does not establish statistical significance or a change from past behavior.

### Keep counting uncertainty separate from notification policy

The store supplies count estimates; it determines which candidates remain
available and how uncertain their counts are. Where conservative bounds are
available, promotion uses the lower bound. This prevents upper-estimate error
alone from creating support, share, or lift eligibility, but uncertainty and
missing history can still cause missed promotions. Changing the store changes
these accuracy and resource tradeoffs without changing the role of the other
stages.

Subset suppression and cooldowns then control overlapping and repeated alerts.
They do not stop candidate counting. A pattern can remain frequent while
producing no callback, and a high count-recovery rate need not imply the same
notification recall. Counting quality and notification policy must therefore
be evaluated separately.

## Input and identity

Events belong to a nonempty group key. Items are sorted and deduplicated, so
repeated attributes count once per event; NULL and empty item entries are
ignored. Input strings are borrowed for ingestion, and retained keys are copied.

Valid zero-weight or empty-input calls return zero without changing group state
or evaluating notifications. They count as `events_noop`; basic argument and
input-width validation still applies.

Patterns use sorted unique items joined by unit separators (`0x1f`), which cannot
appear inside an item. Use this canonical encoding as identity; formatted output
is for display and can be truncated or ambiguous. Hash collisions are resolved
using complete identities. Representation limits are defined in
`bishfpm_limits.h`.

### Numeric identities

Numeric pair engines assign stable per-group IDs to item strings and pack two
IDs into a pair key. Strings remain in the dictionary for input lookup and
output; public pattern queries still use canonical two-item strings.

Marginal pruning resets weak counts but keeps these identities, so the item
limit bounds lifetime vocabulary until the group is removed. Pruning a pair
neither recycles its IDs nor releases its cooldown. Group removal discards the
dictionary, patterns, and cooldowns together.

## Store interface and count uncertainty

The engine sends each evaluated candidate's identity, discounted weight, and
common group time to its store. The store returns residency and a count estimate
with any available uncertainty bounds. Its retention policy determines which
patterns remain available under the configured memory limit. See the
`README.md` (Choosing a store) for store selection and
`PRIOR_ART.md` (Ancestry) for algorithm lineage.

Bounds cover the evaluated candidate stream: only candidates whose items pass
the marginal gates receive updates. Disabling both gates includes every
combination of the configured orders in accepted events. Weighted and
unit-by-unit replay can cross gates at different times.

A known interval encloses the candidate's true decayed count for that history,
subject to floating-point rounding. Estimates and their uncertainty age together.
Where an upper estimate `U` and error `E` are available:

```text
lower = max(0, U - E)
upper = U
```

The engine uses the conservative lower count for promotion and lift. Triggers
expose these values as `pattern_rate`, `pattern_upper_bound`, and `pattern_error`.
At support threshold `S`, a known lower bound at least `S` confirms support;
a known upper bound below `S` rules it out. An interval crossing `S` is uncertain.
Conservative counting can therefore coexist with missed promotions.

`bishfpm_pattern_bounds` reports the interval, residency, and whether the interval
is known. Absence from the resident set does not establish zero support: the
store may supply an absent-key bound or report uncertainty as unknown.
`bishfpm_pattern_rate` returns the upper estimate, or zero when the store cannot
estimate an absent key. Use the bounds query to distinguish a certified zero
from an unknown result. A missing group has zero history in its current lifetime.

## Notification decisions

Each generated candidate is submitted to the store for counting. Generation
alone guarantees neither retention nor an alert. After the update, promotion
requires both conditions, using the conservative pattern count `L` and updated
group count `G`:

```text
L     >= min_pattern_rate
L / G >= promotion_fraction
```

The group count includes every accepted event, including events with no eligible
candidates; the no-op calls described under [input](#input-and-identity) contribute
nothing. For example, with `G=100`, `min_pattern_rate=5`, and
`promotion_fraction=0.20`, a candidate with `L=15` fails the share requirement;
one with `L=25` passes both requirements.

Promotion is evaluated on candidate updates. It is an eligibility decision each
time, rather than a permanent status or a requirement to cross a threshold from
below. Queries and the passage of time alone do not produce notifications.
A promoted candidate still passes through optional suppression checks:

1. **Lift:** require sufficient concentration relative to the item marginals.
2. **Partition lift:** when enabled, suppress triples or quadruples only when
   count bounds prove a split's lift is below the configured threshold.
3. **Subset suppression:** when `subsumption_ratio > 0`, suppress a candidate if
   a proper subset has a positive maintained count `P` and, by default,
   `L < subsumption_ratio * P`, provided that subset's prior notification decision
   is still within `trigger_cooldown_ms`. The opt-in conservative mode instead
   requires a full-history candidate upper bound and a certified parent lower
   bound. A prior decision counts even when no callback was supplied. Numeric
   pair engines have no subset suppression. This reduces overlap without
   producing closed or maximal itemsets.
4. **Cooldown:** suppress repeats until the configured duration has elapsed.

Cooldown protection is independent of store residency. When notification storage
is full, active protection is retained and new alerts are suppressed. Candidate
counts continue to update. A separate index has twice as many slots as the
identity capacity, so saturation leaves empty probe slots. Its payload is
included in both current-memory statistics and the conservative memory bound.

Setting `trigger_cooldown_ms` to zero disables both repeated-alert cooldowns and
subset suppression: no prior notification decisions are retained for either
check.

Ingestion returns `-1` on rejection. Otherwise, it returns the number of eligible
alert decisions, even if no callback was supplied. Counting a decision and
delivering a callback are distinct operations.

### Optional partition lift filter

Call `bishfpm_set_min_partition_lift(engine, threshold)` before the first accepted
event. It returns `1` on success and `0` for a null or busy engine, accepted data,
or a negative/nonfinite threshold. Zero disables the filter and is the default.
This additive API leaves config and trigger layouts unchanged, including the
existing meaning of callback `lift`.

Only triples and quadruples are examined: three and seven unique two-part splits,
respectively. Other orders keep their existing behavior. For candidate `X` and
split `P | Q`, define the full-history split lift as
`C_X * G / (C_P * C_Q)`. The filter uses an upper bound `U_X` for the candidate
and lower bounds `L_P`, `L_Q` for the two parts:

```text
split_lift_upper = U_X * G / (L_P * L_Q)
```

It suppresses only when this upper score is below the threshold, allowing a small
rounding guard. Singleton parts use observed marginal counts as lower bounds;
larger parts require certified store lower bounds for configured subset orders.
For example, configure orders 2 through 4 to make pair and triple evidence
available for quadruples. Missing or uncertified evidence and zero lower counts
cannot establish suppression. Another split with sufficient evidence can still
suppress the candidate. Marginal forgetting or uncertain retention weakens this
test rather than supplying evidence of redundancy.

Store bounds alone cover evaluated candidates, so the filter separately accounts
for omitted occurrences using one decayed group counter `B`, shared with
[conservative subset suppression](#conservative-subset-suppression) when enabled.
The affected orders are triples/quadruples for partition lift and configured text
orders above `min_itemset_order` for conservative subsumption. An accepted event
capable of producing an affected candidate contributes its effective weight to
`B` if any item is gated, or an affected candidate update is rejected. It
contributes at most once, even when both conditions occur or many candidates are
omitted. Canonicalization gives each pattern at most one contribution per event,
so `B` bounds that pattern's missing mass. Given a certified evaluated-history
candidate upper bound `U_eval`:

```text
U_X = min(G, U_eval + B)
```

`B` uses the same effective time and late-discounted weight as the other counts.
It survives pruning within a group and resets with a new group lifetime. This
adds constant-size group state without retaining transactions. Gated unrelated
items can make `B` loose and leave more alerts unsuppressed. Enabling both modes
can broaden the events counted in `B` and weaken partition suppression; this
remains conservative. The public `bishfpm_pattern_bounds` API continues to report
evaluated-history bounds; this full-history adjustment is internal to the two
suppression policies.

With tight bounds, `AB=320`, `C=900`, `ABC=288`, and `G=1000` give split lift
`288 * 1000 / (320 * 900) = 1` for `AB | C`; threshold `1.1` suppresses the
triple. Uncertainty can prevent that decision. This is a descriptive filter,
not a statistical significance test, and pass-through is not a guarantee that
every split exceeds the threshold.

Checks run after count/share and existing `min_lift` eligibility, before subset
suppression and cooldown, without changing count updates. The partition filter
queries at most ten proper subsets of size two or three per quadruple (reusing
the updated upper bound on resident built-in candidates and querying the
candidate once on store extensions), stopping as soon as any split proves
suppression. `bishfpm_get_partition_lift_stats` returns cumulative
`candidates_checked`, `candidates_suppressed`, and `candidates_unknown`
separately from existing statistics. Unknown counts pass-through decisions with
unavailable evidence; snapshots do not reset counters.

### Conservative subset suppression

`bishfpm_set_conservative_subsumption(engine, enabled)` selects the subset policy
before the first accepted event. Only `0` and `1` are valid; zero is the default
and preserves the existing comparison of maintained candidate and parent counts.
The setter returns `1` on success, or `0` for invalid arguments, a busy engine,
or an engine that has accepted data. Config and trigger layouts are unchanged.

Conservative mode still requires a proper subset with a notification decision
inside the current `trigger_cooldown_ms` window. It queries that parent's
certified lower bound `L_P` and the candidate's full-history upper bound `U_X`,
including the shared omitted mass described above. Suppression requires

```text
U_X < subsumption_ratio * L_P
```

with a small rounding guard. Since `C_X <= U_X` and `L_P <= C_P`, this establishes
the same inequality for the true decayed counts, subject to floating-point
rounding. A low candidate lower bound alone cannot establish it. Missing,
uncertified, nonfinite, or nonpositive parent evidence supplies no proof; another
active proper subset may still prove suppression. An unavailable candidate upper
bound passes through. Passing this check does not guarantee that a candidate is
more informative than its subsets.

The existing `subsumption_ratio` and cooldown controls remain in force; zero for
either disables subset suppression. Conservative mode covers configured text
orders above `min_itemset_order`, including pairs when the minimum order is one
and orders above four. Singleton parents use certified store counts and must
have an active notification just like larger parents. Numeric pair engines
remain excluded. The existing bounded notification table and shared group bound
suffice; no transaction history or unbounded state is added. Counting, callback
`lift`, and public evaluated-history bounds retain their existing meanings.

## Pruning and group lifetime

Full pruning through `bishfpm_prune` retires weak groups and forgets weak marginal
and pattern history in surviving groups. Forgetting marginal mass can change
later gates and lift. Group retirement starts a new observation epoch and permits
a fresh numeric dictionary. Surviving groups also reclaim expired notification
identities when maintenance storage is available, while retaining active
cooldown protection.

Bounded pair pruning removes weak pairs from one group while retaining marginal
histories and identities. The store's retention policy governs what may be
forgotten and how uncertainty survives readmission. Removing entries frees
resident slots but need not reduce allocated storage. A removal budget bounds
entries removed, not wall-clock time.

The threshold pair store can also prune automatically during ingestion to reserve
capacity for the complete event. It only forgets pairs whose upper counts are at
or below `forget_below`; if too few slots can be made available, the event is
rejected before counts change.

See the public API in `bishfpm.h` for pruning options and store-specific
cutoff behavior.

## Resource limits and complexity

The engine bounds groups, marginal identities, resident patterns, notification
state, input width, candidate work, and library allocation payload. See the
configuration guidance in `README.md` for constructor
constraints, the public API in `bishfpm.h` for configuration fields, and
`bishfpm_limits.h` for representation limits.

Admission sums `binomial(unique_items, order)` over configured orders before
applying gates. This bounds the worst-case expansion for the whole event;
exceeding the budget rejects the event rather than processing a partial set of
candidates.

The default conservative memory mode checks a configured worst-case payload
bound against `max_memory_bytes` at construction, including retained state,
workspaces, and temporary allocations that may coexist. Reported current payload
can be below this bound.

Opt-in dynamic memory mode requires the initial engine payload to fit, then
charges every engine-owned allocation against the cap as the engine grows.
Charges include staging and simultaneous old/new buffers. Denial of a required
reservation rejects the event before counts change; denial of optional cooldown
maintenance can suppress alerts after acceptance while preserving active
protection. See Optional allocation ledger and general FSS below for details.

In both modes, allocator overhead, stack, caller buffers, callback allocations,
and other engines are outside the budget; it is not a bound on process memory.

Preparation sorts items, typically `O(M log M)`. Candidate cost depends on store
updates and notification checks. Collisions can lengthen lookups, and maintenance
can scan or rebuild state. These limits do not guarantee latency; measure
maintenance and callback time as part of the application workload.

## Failure and diagnostics

Rejected events leave counts, retained identities, cooldowns, and watermarks
unchanged. Internal capacity and diagnostic counters may change. A failed new
group is removed, so the caller can retry the complete event.

All fallible reservation for count updates finishes before commit. Notification
storage can still fail or saturate afterward; existing cooldown protection is
preserved and alerts may be suppressed. This never turns an accepted event into
a rejected one.

Statistics distinguish event acceptance and rejection, candidate work,
notification decisions and delivery, and memory use. A separate cumulative
rejection snapshot identifies why ingestion failed. Suppressed notifications
are not rejected events. Counter fields and query functions are defined in the
public API in `bishfpm.h`.

## Ownership and concurrency

Callbacks run synchronously with borrowed group/trigger pointers. Copy results
needed after the callback returns. Read-only queries are allowed during a
callback. Nested ingestion returns `-1`; prune and free during ingestion are
ignored. Free the engine after the outer ingestion call has returned.

The application serializes all access to an engine, including reads. There are
no background workers. Do not unwind through library frames using exceptions,
`longjmp`, or asynchronous cancellation.

State is in memory. Restart begins empty unless the application replays history.
Retrying a rejected event is safe for counts; replaying an accepted event counts
it again. Persistence, durable offsets, deduplication, and delivery policy belong
to the application.

## API reference

The engine API is declared in `bishfpm.h`. Independent
counters, stores, itemset helpers, and arenas are available through
`bishfpm_lowlevel.h` and its component headers,
which document their ownership and use.

For algorithm ancestry, see `PRIOR_ART.md`. For store selection and
build commands, see the `README.md`.

## Experimental general-store FSS and memory options

Existing constructors keep Space-Saving and conservative memory sizing. The new
options API can select general-itemset FSS, dynamic memory budgeting, or both:

```c
bishfpm_config_t cfg;
bishfpm_config_default(&cfg);
cfg.max_groups = 1000;
cfg.max_memory_bytes = 64U << 20;

bishfpm_options_t options;
bishfpm_options_default(&options);
options.store = BISHFPM_STORE_FSS;
options.sketch_width = 256;
options.memory_mode = BISHFPM_MEMORY_DYNAMIC;
bishfpm_t *engine = bishfpm_create_with_options(&cfg, &options);
/* Check engine for NULL, then ingest events as usual. */
```

FSS can filter nonresident candidates without copying their strings or replacing
heap entries. It adds sketch memory and changes retention; it is experimental,
and improved recall is workload-dependent. Its absent-pattern queries return
certified bucket bounds. As with numeric FSS, bounds cover evaluated candidates;
disable both marginal gates for full transaction history. Filtered misses do not
emit alerts. Standalone stores support `bishfpm_topk_new_fss` and
`bishfpm_topk_new_fss_arena`, with matching ownership rules. FSS seeds are fixed
after positive input: `bishfpm_topk_try_set_seed` reports refusal, while the legacy
setter leaves the store unchanged. Reset starts a new history epoch and permits
reseeding.

Dynamic mode charges every engine-owned allocation against `max_memory_bytes`,
including key copies, staging, old/new tables, store arenas, sketches, and cooldown
spares. This allows sparse groups to share a budget without reserving their full
configured growth potential. Required reservation denial returns `-1` before count
or watermark changes. Cooldown growth/maintenance is optional after acceptance;
its denial preserves active protection and can suppress new alerts. Dynamic
pruning can release weak groups and marginal keys without allocating replacement
tables, so reclamation remains possible at the cap. The cap is
requested payload, excluding allocator overhead, caller buffers, standalone
stores, and allocations made by callbacks.

`bishfpm_get_memory_stats` reports dynamic current/peak payload, allocation denials,
and events rejected by the cap. `bishfpm_get_stats().memory_bound_bytes` is the
configured cap in dynamic mode. A memory-denied event also increments the existing
`allocation_failure` reason and `budget_rejections`; OS allocation failures do not
increment the budget-denial counters. Conservative mode retains its existing
static bound and reports zero for untracked peak/denial fields.

`bishfpm_get_vocabulary_stats(engine, group, &snapshot)` reports retained items,
item limit, remaining slots, and whether identities persist until group removal.
Dormant numeric identities still occupy slots. The query is read-only and does
not recycle IDs. Numeric engines can opt into dynamic budgeting with
`bishfpm_create_pairs_with_options`; their store is still selected by pair options,
so the engine options must use `BISHFPM_STORE_SS` and zero sketch width.

Run `make t/00util/bench_general` followed by
`./t/00util/bench_general 3000 42` for a reproducible SS/FSS comparison. CSV output
includes capacities and sketch width, the shared store budget, actual store
bytes, total engine current/peak payload, throughput, replacements, alert recall,
and persistent/burst detection delays. Delay `-1` means no alert was observed.
The FSS resident capacity is reduced to pay for its sketch and alignment within
the SS budget, and its total peak payload is checked against SS. Both stores see
the same weighted/late events, shifting vocabulary, persistent pair, and triple
burst. An independent long-double oracle checks reported intervals and callback
support. Timing includes callback validation but excludes the oracle sweep.
These are diagnostic workloads, not performance or recall guarantees.

### Store sizing and pattern pressure

General pattern capacity (`topk_k`) bounds the resident working set per group.
Because transactions generate combinations combinatorially ($O(k^2)$ pairs,
$O(k^3)$ triples), size `topk_k` against active candidate pattern volume rather
than raw item counts. Undersized stores suffer replacement churn that evicts
emerging patterns before they accumulate sufficient support to alert, degrading
recall; increasing `max_memory_bytes` raises only the memory budget, not `topk_k`.

To monitor capacity health under load, call `bishfpm_get_pattern_pressure(engine, group, &out)`
(pass `NULL` for engine-wide totals). A high replacement rate relative to resident
updates indicates store pressure. Workload capacity sweeps across stable, burst,
and churn streams can be benchmarked with `./t/00util/bench_general --sweep 3000 42`.

## Optional allocation ledger and general FSS

General-store pressure snapshots keep per-group cumulative counters in group
metadata. Engine totals combine active groups with counters retired during group
removal, so the hot path updates only one counter set. Both allocating and
in-place pruning retire counts before freeing a group. Gauges are computed on
read. Numeric engines return unsupported rather than reinterpreting their
different admission policies. A private accepted-update flag separates count
overflow from FSS filtering without changing the public count-bound layout.

`bishfpm_options_t` selects two independent extensions without changing the old
configuration or constructor ABI. The general store can use experimental FSS;
SS remains the default. FSS retains a decayed bucket upper bound for absent keys,
transfers evicted/pruned upper counts into buckets, and inherits uncertainty on
admission even into free slots after pruning. Filtered misses update only the
sketch and cannot promote. Queries do not advance time. Guards stage all clock,
bucket, and heap changes until the input can be represented. Reseeding bucket
history is unsupported because its identities are unavailable; reset starts a
new epoch before changing the seed.

Conservative memory mode still checks configured worst-case payload at creation.
Dynamic mode instead maintains an allocation ledger stored in the engine. All
engine-owned allocations receive that ledger, including nested rate tables,
numeric stores, and owned group arenas. Arena suballocations are already charged
as part of the buffer and are not charged twice. The allocator checks each new
request against the remaining budget before calling the OS and charges only a
successful request. Releases subtract its exact payload. The high-water mark
therefore includes simultaneous old/new arrays and staging/key copies, rather
than assuming one capacity doubling per event. Caller-owned allocations are
outside this ledger. The independent public payload snapshot can be compared
with the ledger; allocator-wrapping tests also verify it against actual requests.

Group/marginal reservations still precede counter mutation. A denied required
request rejects the event atomically for counts, identities, and watermarks;
previously completed table growth can remain allocated. If a replacement group or marginal table cannot fit, dynamic pruning releases
weak entries in place with backward-shift deletion and retains the old capacity.
This permits reclamation without spare budget. Cooldown maintenance/growth occurs after acceptance: denial can suppress
notifications but preserves existing cooldown protection. Such optional denials
count in memory diagnostics, not event rejections. Failed OS allocations are
separate from budget denials. Group removal releases all owned state and makes
those budget bytes available to other groups.

Vocabulary diagnostics count retained dictionary strings, including dormant
numeric entries. Numeric IDs are still stable for a group's lifetime. Memory
budgeting does not introduce ID recycling, history retirement, Apriori update
skipping, or a different notification ordering policy.
