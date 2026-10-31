# Architecture & Design Specification: `bishfpm`
## Real-Time Burst Anomaly and Frequent Pattern Mining Engine

---

## 1. Executive Summary & Problem Scope

`bishfpm` is a real-time, low-latency C library designed to identify coordinated burst anomalies across multi-attribute event streams. 

In fraud detection, cybersecurity, and operational telemetry, coordinated attacks or anomalies share a subset of correlated attributes (e.g., an unexpected spike in purchases where `item_name="PlayStation 5"` co-occurs with `card_country="RU"` and `payment_type="prepaid"`). 

Traditional Frequent Pattern Mining (FPM) algorithms (e.g., Apriori, FP-Growth, Eclat, LCM) are fundamentally ill-suited for this problem:
1. **Static / Batch Orientation**: They operate over bounded, static transaction databases in batch passes rather than high-throughput streaming events.
2. **Combinatorial Explosion**: A transaction with $M$ attributes contains $2^M - 1$ possible itemsets. For $M=20$, each event generates $1,048,575$ subsets, making real-time line-rate tracking intractable.
3. **Static Support vs. Velocity Bursts**: Traditional support measures static frequency counts over a historical window, failing to isolate rapid temporal rate spikes relative to dynamic localized baselines.

`bishfpm` solves this by introducing a **Gated Two-Phase Candidate Promotion Algorithm**:
1. **Fast-path $O(M)$ Continuous Rate Tracking**: Tracks continuous exponentially decaying rates for individual items and group baselines with zero memory allocations on the hot path.
2. **Gated Candidate Expansion**: Candidate combinations are evaluated *only if two or more items in the transaction are already individually elevated above baseline rate thresholds*.
3. **Decayed HeavyKeeper Top-K Tracking**: Maintains high-rate candidate patterns in a contiguous, time-decayed sketch with exponential collision suppression.
4. **Promotion Engine**: Promotes a candidate pattern to a triggered event when its rate accounts for a meaningful fraction $\alpha$ of the group's total rate.

---

## 2. High-Level Architecture

```
Incoming Event: (group_key, timestamp, items[0..m-1])
                       │
                       ▼
┌────────────────────────────────────────────────────────┐
│  Phase 1: Rate Engine (EWMA Continuous Time Decay)     │
│  - Update Group Rate: R(group, t)                      │
│  - For each item i: Update R(item_i | group, t)        │
└──────────────────────┬─────────────────────────────────┘
                       │
                       ▼
┌────────────────────────────────────────────────────────┐
│  Phase 2: Elevation Gating & Candidate Expansion       │
│  - Filter elevated items: E = { i | R(i) >= tau_item } │
│  - If |E| < min_order: Normal event, exit fast path    │
│  - If |E| >= min_order: Generate k-subsets             │
│  - Canonicalize & serialize itemsets (e.g. sorted \x1f)│
└──────────────────────┬─────────────────────────────────┘
                       │
                       ▼
┌────────────────────────────────────────────────────────┐
│  Phase 3: Decayed HeavyKeeper Top-K Tracker            │
│  - Update candidate itemsets in Group's Top-K Sketch   │
│  - Probabilistic collision decay: P(decay) = b^(-c)    │
│  - Query current rate estimate R(candidate | group)    │
└──────────────────────┬─────────────────────────────────┘
                       │
                       ▼
┌────────────────────────────────────────────────────────┐
│  Phase 4: Promotion & Temporal Query Engine            │
│  - Condition: R(candidate) >= alpha * R(group)         │
│  - If matched: emit Triggered Pattern Event            │
│  - Cooldown filter suppresses duplicate alert storms   │
│  - Priority Search Tree (PST) temporal range indexing  │
└────────────────────────────────────────────────────────┘
```

---

## 3. Mathematical Foundations

### 3.1 Continuous Time-Decayed EWMA Rates

Events arrive at arbitrary timestamps $t$ represented as 64-bit timestamps in milliseconds. Instead of fixed or leaping sliding-window ring buffers (which suffer from quantization artifacts and memory overhead), rates are modeled as continuous exponentially decaying counters.

Given an event at timestamp $t$ and prior event at $t_{\text{prev}}$:
$$\Delta t = t - t_{\text{prev}}$$
$$\text{decay}(\Delta t) = e^{-\lambda \Delta t}$$
$$R_t = R_{t_{\text{prev}}} \cdot \text{decay}(\Delta t) + 1.0$$
$$t_{\text{prev}} \leftarrow t$$

where:
- Half-life $T_{1/2}$ is the duration over which an unrefreshed rate decays by 50%.
- Decay constant $\lambda = \frac{\ln 2}{T_{1/2}}$.
- When $\Delta t \ge 0$, decay is in range $(0, 1]$. Out-of-order events ($\Delta t < 0$) are counted additively without backwards time mutation.

### 3.2 Steady-State Rate Convergence & Threshold Calibration

For a stationary Poisson stream arriving at mean rate $r$ (events/sec) with half-life $T_{1/2}$, the expected inter-arrival time is $\Delta t = \frac{1}{r}$. 

The steady-state rate $R_{\infty}$ satisfies:
$$R_{\infty} = R_{\infty} \cdot e^{-\lambda / r} + 1 \implies R_{\infty} = \frac{1}{1 - e^{-\lambda / r}}$$

Using the first-order Taylor approximation $1 - e^{-x} \approx x$ for small $\lambda / r$:
$$R_{\infty} \approx \frac{r}{\lambda} = r \cdot \frac{T_{1/2}}{\ln 2} \approx 1.4427 \cdot r \cdot T_{1/2}$$

**Operational Calibration Rule:**
If an item's expected normal velocity is $r_{\text{baseline}}$ events/sec, an operator seeking to flag anomalies that exceed $k \times$ normal traffic sets:
$$\tau_{\text{item}} = 1.4427 \cdot (k \cdot r_{\text{baseline}}) \cdot T_{1/2}$$

### 3.3 Dynamic Baseline & Promotion Criterion

- **Group Velocity**: $R(G, t)$ represents the total rate of group $G$ (e.g., store, merchant, account).
- **Candidate Velocity**: $R(S \mid G, t)$ is the estimated rate of candidate pattern $S \subseteq I$ in group $G$.
- **Promotion Threshold**:
  A candidate itemset $S$ with $|S| \ge \text{min\_order}$ is promoted to a **Triggered Pattern** if:
  $$R(S \mid G, t) \ge \alpha \cdot R(G, t) \quad \text{and} \quad R(S \mid G, t) \ge \tau_{\text{min\_pattern}}$$
  where $\alpha \in (0, 1]$ represents the burst fraction (e.g., $\alpha = 0.25$ indicates that this pattern alone accounts for 25% or more of the entire group's recent traffic).

---

## 4. Algorithmic Components & Complexity Analysis

### 4.1 Gated Candidate Generation

Let transaction $T$ arrive with $M$ attributes: $I = \{i_1, i_2, \dots, i_M\}$.
1. For each item $i_j \in I$, query and update $R(i_j \mid G)$.
2. Collect the set of elevated items:
   $$E = \{ i \in I \mid R(i \mid G) \ge \tau_{\text{item}} \}$$
3. **Complexity Comparison**:
   - **Unbounded FPM**: Evaluates $2^M - 1$ subsets. For $M=20$, $1,048,575$ subsets per event.
   - **`bishfpm` Gated Evaluation**:
     - **Normal Traffic**: $|E| < \text{min\_order} \implies 0$ candidate combinations generated. Processing time is strictly $\mathcal{O}(M)$ hash table lookups.
     - **Burst Traffic**: Only items with elevated rates enter $E$. Evaluating combinations up to size $k_{\text{max}}$ generates:
       $$|\mathcal{C}| = \sum_{k=\text{min\_order}}^{k_{\text{max}}} \binom{|E|}{k}$$
       Because $|E|$ is typically 2 or 3 during an anomaly, $|\mathcal{C}| \le \binom{3}{2} + \binom{3}{3} = 4$, entirely preventing combinatorial explosion while targeting the exact anomalous correlation.

### 4.2 Canonical Itemset Serialization

To maintain order-independent pattern identification:
1. Elevated items are sorted lexicographically: `["card_country=RU", "item=PS5"]`.
2. Delimited with the ASCII Unit Separator `\x1f` (value 31): `"card_country=RU\x1fitem=PS5"`.
3. Guarantees deterministic hashing and avoids string allocations via reusable scratch arenas.

### 4.3 HeavyKeeper with Exponential Collision Suppression

`bishfpm` tracks top candidates in a contiguous 2D table of $d$ rows and $w$ columns of buckets:
```c
typedef struct {
    uint32_t fingerprint;
    float    rate;
    uint64_t last_time_ms;
} bishfpm_hk_bucket_t;
```

#### Probabilistic Decay Math
When candidate $S$ with fingerprint $\text{fp}$ maps to bucket $B$ where $B\text{.fingerprint} \neq \text{fp}$:
$$P(\text{decay}) = b^{-\lfloor B\text{.rate} \rfloor} \quad (b \approx 0.95)$$

- **Frequent / Burst Patterns**: If $B\text{.rate} = 20$, $P(\text{decay}) = 0.95^{20} \approx 0.358$. With 4 rows, the probability of simultaneous decrement across all rows is $(0.358)^4 \approx 0.016$. The true burst pattern receives additive $+1$ updates on each occurrence, rapidly overpowering sporadic decay.
- **Background / Noise Items**: If $B\text{.rate} \le 1$, $P(\text{decay}) = 0.95^1 = 0.95$. Uncorrelated items are almost immediately decremented and evicted.

#### Top-K Min-Heap with $O(1)$ Hash Map Index
Instead of linear scans in the heap on updates:
- An open-addressing slot map maintains `pattern_hash -> heap_index`.
- Lookups are $\mathcal{O}(1)$. Heap sift-up and sift-down maintain slot map entries during swaps, achieving $\mathcal{O}(d + \log K)$ per update.

### 4.4 Temporal Indexing via Priority Search Tree

To support real-time upstream queries over recent burst events:
- Triggered patterns and candidate histories can be indexed into a **Priority Search Tree** (PST).
- The PST stores 2D points $(x, y) = (\text{timestamp}, \text{rate})$.
- Upstream systems execute 3-sided range queries:
  $$\text{Query}([t_{\text{start}}, t_{\text{end}}], y \ge \tau_{\text{rate}})$$
  returning all burst events active within the time window exceeding rate $\tau$ in optimal $\mathcal{O}(\log n + k)$ time, where $k$ is the number of reported events.

---

## 5. API Specification (`bishfpm.h`)

```c
/* configuration parameters */
typedef struct {
    double   ewma_half_life_ms;     /* half-life for continuous decay */
    double   item_rate_threshold;   /* item rate to trigger elevation */
    double   promotion_fraction;    /* alpha: fraction of group rate for promotion */
    double   min_pattern_rate;      /* absolute minimum rate for promotion */
    uint32_t min_itemset_order;     /* min size of candidate pattern to trigger (default 2) */
    uint32_t max_itemset_order;     /* max size of candidate combinations (default 3) */
    uint32_t topk_k;                /* number of heavy hitters to track per group */
    uint32_t topk_width;            /* HeavyKeeper table width */
    uint32_t topk_depth;            /* HeavyKeeper table depth */
    uint64_t trigger_cooldown_ms;   /* cooldown before re-triggering same pattern */
} bishfpm_config_t;

/* triggered pattern result */
typedef struct {
    char     pattern[BISHFPM_MAX_PATTERN_LEN];   /* canonical serialized pattern string */
    char     formatted[BISHFPM_MAX_PATTERN_LEN]; /* human readable comma-separated string */
    double   pattern_rate;                       /* current pattern rate */
    double   group_rate;                         /* current total group rate */
    double   rate_fraction;                      /* pattern_rate / group_rate */
    uint32_t item_count;                         /* number of items in pattern */
    uint64_t timestamp;                          /* timestamp of triggering event */
} bishfpm_trigger_t;

typedef void (*bishfpm_trigger_cb)(const char *group_key,
                                  const bishfpm_trigger_t *trigger,
                                  void *udata);

/* engine lifecycle */
bishfpm_t *bishfpm_create(const bishfpm_config_t *cfg);
void       bishfpm_free(bishfpm_t *engine);

/* event ingestion */
int bishfpm_add_event(bishfpm_t *engine,
                      const char *group_key,
                      uint64_t timestamp_ms,
                      const char *const *items,
                      size_t num_items,
                      bishfpm_trigger_cb cb,
                      void *udata);
```

---

## 6. Repository Layout (Matching `../nanorq`)

```
bishfpm/
├── .clang-format           /* LLVM, 4-space, Linux braces */
├── .gitignore              /* ignore binaries, objects, profiles */
├── .gitmodules             /* submodule registration */
├── Makefile                /* all, lib, test, indent, clean */
├── deps/
├── docs/
│   └── DESIGN.md           /* formal architecture & mathematical specification */
├── include/
│   ├── bishfpm.h           /* public interface */
│   ├── bishfpm_arena.h     /* bump-pointer arena allocator */
│   ├── bishfpm_hash.h      /* 64-bit MurmurHash3 */
│   ├── bishfpm_itemset.h   /* subset generation & canonicalization */
│   ├── bishfpm_rate.h      /* EWMA & rate math */
│   └── bishfpm_topk.h      /* HeavyKeeper top-k sketch */
├── lib/
│   ├── bishfpm.c           /* core engine and group orchestration */
│   ├── itemset.c           /* sorting and combinatorial subset expansion */
│   ├── rate.c              /* rate updates and continuous decay */
│   └── topk.c              /* contiguous HeavyKeeper + indexed min-heap */
├── t/
│   ├── 00util/             /* C test regression binaries */
│   │   ├── test_engine.c
│   │   ├── test_itemset.c
│   │   ├── test_rate.c
│   │   └── test_topk.c
│   ├── Util.pm             /* Perl test harness utilities */
│   ├── 10rate.t            /* rate decay unit tests */
│   ├── 20topk.t            /* HeavyKeeper accuracy and eviction tests */
│   ├── 30itemset.t         /* combinatorial subset & canonicalization tests */
│   ├── 40engine.t          /* end-to-end multi-attribute burst tests */
│   └── 50fraud_burst.t     /* simulation verification test */
└── examples/
    └── fraud_burst.c       /* retail store credit card attack demonstration */
```
