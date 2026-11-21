# ChronoForge

**A high-cardinality, in-memory time-series storage engine in modern C++20 Gorilla compression, an inverted label index with galloping set algebra, and a HyperLogLog-backed cardinality guard.**

The same problem class that sits under Prometheus, Facebook's Gorilla / Beringei, and every metrics backend at scale: *ingest millions of samples per second, keep them in RAM at a few bytes each, and answer "which series match these labels?" without scanning the world.* No external dependencies; ~2,400 lines including tests.

```
$ make && make test && make run
184k+ checks, 0 failed          (also clean under ASan+UBSan and TSan)
```

---

## 1. The problem and its constraints

A monitoring backend for a large fleet is dominated by three hard facts:

| Constraint | Back-of-envelope (illustrative fleet: 100k hosts x 100 metrics, 15 s scrape) |
|---|---|
| **Write volume** | 10M series -> 10M / 15 s ≈ **667k samples/s**, ≈ 57.6 B samples/day |
| **Memory** | 16 B/sample raw (8 B ts + 8 B double) ≈ **920 GB/day** uncompressed |
| **Cardinality** | One bad deploy that puts `user_id` in a label can mint **millions of series in minutes** and OOM the backend |
| **Query shape** | Selectors like `{__name__="cpu", region="us", host!="h7"}` must cost O(*matches*), not O(*total series*) |

ChronoForge attacks each one with a purpose-chosen data structure rather than a general-purpose container:

| Problem | Structure / algorithm | Where |
|---|---|---|
| Sample memory | **Gorilla** delta-of-delta timestamps + XOR floats, bit-packed | `algorithms/gorilla_codec` |
| "Which series match?" | **Inverted index** of *sorted* posting lists + **galloping** intersection, k-way heap union, galloping difference | `algorithms/postings`, `services/inverted_index` |
| Cardinality explosions | **Exact admission control + HyperLogLog** observability | `services/cardinality_guard`, `algorithms/hyperloglog` |
| Write contention | **Sharded locks**, global `shared_mutex` only on series *creation*, cache-line-aligned shards | `services/storage_engine` |
| Retention | **`std::deque` of time-ordered chunks**: O(1) expiry + O(log C) time seek | `services/storage_engine` |

## 2. Architecture

```
                        ┌───────────────────────── StorageEngine ─────────────────────────┐
 append(labels,ts,v) ──►│  resolve(): shared_lock ─► InvertedIndex.lookup (hash → id)     │
                        │     miss ─► unique_lock ─► re-check ─► CardinalityGuard.admit   │
                        │                           ─► index.insert  (postings push_back) │
                        │                                                                  │
 appendById(id,ts,v) ──►│  shard[id % N].mutex ─► SeriesData { deque<sealed chunk>, head } │
                        │                           head.append → Gorilla bits             │
                        │                           count ≥ 120 → seal, retention pop_front│
                        └──────────────────────────────────────────────────────────────────┘
 QueryEngine.query ──► index.select (set algebra) ─► per-series: binary-search chunks by time
                      ─► decode only overlapping chunks ─► downsample() single pass
```

**Lock hierarchy:** `index_mu_` → `shard.mu`, never reversed. Steady-state appends touch *one* shard mutex and, if the caller caches the `SeriesId` (as a scrape loop caches per-target series refs), never the global lock.

## 3. The "why" behind each algorithmic choice

### 3.1 Gorilla compression instead of zstd/LZ4
- **Streaming O(1) append.** LZ-family codecs need a buffer to find matches; per-sample append would mean re-compressing the block.
- **Exploits the *arithmetic* redundancy of metrics**, not byte repetition: scrape timestamps are near-periodic (delta-of-delta ≈ 0 -> **1 bit**), adjacent values share sign/exponent/high mantissa (XOR -> few meaningful bits).
- **Deliberate bounded cost:** a chunk is not random-access (sample *i* depends on all previous), so chunks are capped at **120 samples**. Decode cost per query per chunk is O(120); chunk `[minTime,maxTime]` lets the engine skip chunks without decoding.
- **Hardening beyond the paper:**
  - Timestamp deltas use **wrapping `uint64` arithmetic** -> even `INT64_MIN -> INT64_MAX` round-trips with no signed-overflow UB (tested).
  - The escape bucket stores a full **64-bit** delta-of-delta (the paper uses 32).
  - `std::bit_cast` -> **NaN payloads, `-0.0`, denormals are bit-exact** (tested).
  - The decoder is bounds-checked and **validates every field**; truncated or bit-flipped chunks terminate cleanly instead of over-reading (tested at every truncation length + 2,000 random bit-flip trials, under ASan/UBSan).

### 3.2 Sorted `vector<uint32_t>` posting lists + galloping, not `std::set`, hash sets, or bitmaps
- **Series ids are assigned monotonically**, so every new posting is the largest element: insertion is an O(1) amortised `push_back` that *preserves sortedness*. The index never sorts anything.
- **4 B/posting contiguous** vs ~32-48 B/node for `std::set` at 10^7 series x ~8 labels that is the gap between hundreds of MB and several GB.
- **Galloping (exponential + binary) search** makes intersecting a tiny list against a huge one **O(m log(n/m))** instead of O(n). Below a size ratio of 8 the code falls back to a plain two-pointer merge, which is faster when lists are similar in size.
- Lists are intersected **smallest-first**: the running result is bounded by the rarest label and an empty intermediate short-circuits the rest.
- **Negative matchers last** (`host!="h7"` = set *difference*), because they can only shrink an already-reduced set. Series ids are dense, so the "universe" for negative-only selectors is *implicit* (`iota`), costing zero stored bytes.
- **`PREFIX` matchers** use an ordered `std::map` of values -> `lower_bound` + contiguous walk = O(log V + K); a hash map would need an O(V) scan. K matching value lists are unioned with a **min-heap k-way merge, O(N log K)**, instead of concatenate-sort-unique (O(N log N) + 2N allocation).
- *Honest alternative:* Roaring bitmaps win for very **dense** lists. This engine targets sparse, high-cardinality lists, where sorted arrays are smaller and simpler.

### 3.3 Exact admission + HyperLogLog observability (two structures on purpose)
The cardinality guard makes its **hard decision with exact counts** a new series is shed only if it introduces a *new* value to a label key already at its limit. Existing series keep working (graceful degradation, never a blackout).
But shed series are, by design, stored nowhere so "how bad is the attack?" is unrecoverable from the index. A **4 KiB HyperLogLog per label key** counts every *attempted* value in O(1) space:

```
user_id   attempted (HLL) = 195,173   admitted (exact) = 50,000     (true attempted: 200,000; error -2.4%, σ = 1.62%)
```
Also: label *names* are attacker-controlled too, so tracked keys are capped at 256 otherwise the guard itself would be the memory leak (tested).

### 3.4 Concurrency: lock striping, double-checked registration, false-sharing avoidance
- Series creation is rare; appends are constant. So the **global lock is a `shared_mutex` taken exclusively only to create a series**, and sample data lives behind **N striped mutexes** (`id % N`; dense ids -> perfectly even partition).
- `resolve()` is **double-checked**: without the re-check under the write lock, two racing writers would mint two ids for one series. Tested with 8 threads racing to register the same 300 series.
- The shard entry is created **before** the index lock is released, so no thread can ever observe "id exists but data doesn't".
- Shards are `alignas(64)`: neighbouring shards' mutexes would otherwise share a cache line and defeat the striping (false sharing).
- Validated with **ThreadSanitizer**: concurrent registration + ingest + live reads, zero reports.

### 3.5 `std::deque` for sealed chunks
Retention pops from the *front* (`vector::erase(begin)` is O(C) and shifts every chunk), time seeks need *random access* for `lower_bound` (rules out `list`), and `push_back` must not relocate existing chunk buffers. `deque` is the one standard container satisfying all three.

## 4. Complexity

*n* = total series, *C* = chunks in a series, *L* = labels per series, *V* = distinct values of a label, *m* = smallest posting list in a selector, *k* = matchers.

| Operation | Time | Space / notes |
|---|---|---|
| Append (existing series) | **O(1)** amortised; ≤ 145 bits written; one shard lock | ~0.6–7.8 B/sample payload depending on data shape (§5) |
| Register new series | O(L · log V) + O(1) per posting push | one-time write-lock |
| Selector with positives | ≈ Σᵢ O(m · log(1 + nᵢ/m)) via galloping | cost tracks *selectivity*, not *n* |
| `Prefix` matcher | O(log V + K) to locate + O(N_K log K) to union | |
| Negative matcher | O(\|R\| · log(1 + n/\|R\|)) per matcher, R = current result | universe implicit |
| Range read of one series | **O(log C)** seek + O(120 · overlapping chunks) decode | non-overlapping, time-ordered chunks |
| Retention (per chunk) | **O(1)** amortised `pop_front` | chunk-granular: ≤ 1 chunk of slack |
| HyperLogLog add / merge / estimate | O(1) / O(m) / O(m), m = 4096 | **4 KiB**, σ = 1.04/√m = 1.62% |
| Downsample | O(N) single pass, O(1) extra memory | one open accumulator; NaNs skipped |

## 5. Measured results

Hardware: **a single-vCPU Linux sandbox** (`g++ 13.3 -O2`, one hardware thread). Workload is deterministic (counter-based RNG), so results reproduce. Full output: [`docs/sample_output.txt`](docs/sample_output.txt).

### Compression is workload-dependent and I report the bad cases too
| Shape (120-sample chunks, 15 s scrape, 5% ±5 ms jitter) | bytes/sample | ratio vs 16 B | encode / decode |
|---|---:|---:|---:|
| Constant (`up{}`, config gauges) | 0.59 | **27.3x** | 13 / 10 ns |
| Counter (`requests_total`) | 2.13 | **7.5x** | 26 / 19 ns |
| Gauge quantized to 0.01 (cpu %) | 6.94 | **2.3x** | 34 / 25 ns |
| Random doubles (adversarial) | 7.79 | **2.1x** | 31 / 26 ns |

Realistic mixed fleet (40% gauges, 40% counters, 20% constant): **3.75 B/sample, 4.26x** (25.8 MiB vs 109.9 MiB raw for 7.2M samples). All shapes verified bit-exact lossless.

> **Finding worth knowing:** decimal-quantized gauges (`51.27`) compress poorly because most decimals aren't exact in binary, so the XOR of neighbours still has ~50 random mantissa bits. Production systems (VictoriaMetrics, InfluxDB, ALP) address this with *decimal scaling* storing `5127` as an integer. This is on the roadmap (§8); the benchmark deliberately includes it instead of cherry-picking friendly data.

### Ingestion
| | Result |
|---|---|
| Series registration | 10,000 series in 8.6 ms (**0.86 µs/series**, write-lock path) |
| Single-thread append | **≈ 5.2 M samples/s** (timing *includes* workload generation, so it is conservative) |

On the back-of-envelope fleet in §1 (667k samples/s), that is roughly 8x headroom for the storage layer on one core *excluding* network, parsing and protocol costs, which dominate in real deployments.
**Not measured:** multi-core scaling. The sandbox had one hardware thread, so the `--threads N` flag exists but its speedup is unverified here; run `./build/chronoforge --threads 8` on a multi-core machine. Correctness under concurrency *is* verified (TSan).

### Query: inverted index vs brute-force label scan (results materialised on both sides, best of 20)
| Query | matches | 10k series | 100k series |
|---|---:|---:|---:|
| `host="host-00042"` (selective) | 5 | **101x** | **398x** |
| `host` prefix `host-0001` ∧ `__name__="up"` | 10 | 41x | 196x |
| `__name__="requests_total" ∧ service="svc-3" ∧ region!="eu-west"` | 250 | 6.8x | 6.7x |
| `__name__="cpu_util" ∧ region="us-east"` | 500 | 4.4x | 4.4x |
| `__name__!="up"` (matches 80% of series) | 8,000 | 0.7x | 0.6x |

Read it as theory predicts: a scan is O(n); the index is O(*matches*). **Selective queries scale with cardinality; unselective queries are output-bound and the index offers no advantage** (the last row is slightly *slower*: the index must build the implicit "all series" universe and subtract the `up` list, work a plain scan doesn't do I'm not hiding it).

### Cardinality defence
200,000 unique-`user_id` series offered against a 50,000 limit: **50,000 admitted, 150,000 shed in 183 ms, existing series still writable.**

## 6. Correctness strategy

Every structure is tested against a trivially-correct oracle rather than hand-picked examples (184k+ assertions):

| Component | Oracle / technique |
|---|---|
| Bit stream | 20k random-width round-trips; overrun & empty-buffer reads |
| Gorilla | 300 randomized series incl. arbitrary 64-bit patterns; NaN payloads; every delta-of-delta bucket edge ±1; `INT64_MIN/MAX`; truncation at every length; 2,000 bit-flip trials |
| Posting algebra | `std::set_intersection/difference/union`; `gallopLowerBound` vs `std::lower_bound` for random `lo`/target |
| Inverted index | **1,500 random selectors (Equal/NotEqual/Prefix/Exists) vs brute-force `Matcher::matches`** over 4k series |
| HyperLogLog | error bounds at 1…500k; duplicate immunity; `merge(A,B) == sketch(A∪B)` |
| Engine | chunk-boundary inclusive ranges, retention + idle sweep, ordering survives sweep |
| Concurrency | 8 threads race to register identical series; concurrent writers + live readers assert every observed prefix is gap-free |

`make sanitize` (ASan+UBSan) and `make tsan` both pass. The library builds with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` and **zero warnings**.

## 7. Build & run

Requires `g++ ≥ 11` or `clang++ ≥ 14` (C++20 `<bit>`), `make` *or* CMake ≥ 3.16.

```bash
make -j                 # builds build/chronoforge and build/chronoforge_tests
make test               # 184k+ assertions
make run                # full demo + benchmarks
make sanitize           # ASan + UBSan test run
make tsan               # ThreadSanitizer test run

# CMake alternative
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure

# Scale knobs
./build/chronoforge --hosts 20000 --samples 10 --threads 8
```

## 8. Limitations and roadmap

Stated plainly, because knowing where a system breaks is part of designing it:

- **In-memory only, no WAL/snapshot** a crash loses data. *Next:* append-only WAL with CRC32 and torn-tail truncation on replay; periodic chunk snapshots.
- **Strictly in-order per series.** Out-of-order samples are rejected (counted in stats). Real systems add a bounded out-of-order head or a WAL-time sort buffer.
- **Series are never garbage-collected from the index** (expired series keep their postings). *Next:* block-based index rewrite à la Prometheus compaction.
- **Decimal-quantised gauges compress poorly** (see §5). *Next:* decimal-scaling / ALP-style pre-transform with integer delta coding.
- **Retention is chunk-granular** (≤ 1 chunk of slack, shown in the demo).
- **HLL uses the raw estimator**; a few-percent bias exists between ~2.5m and ~5m. HLL++ bias tables would fix it; admission decisions are exact so this affects reporting only.
- **Global index `shared_mutex`** is a contention point for *uncached* `append(labels,…)` at very high thread counts (reader-count atomics bounce one cache line). That is why `resolve()`+`appendById()` exist; the fix beyond that is a striped registry.
- **Multi-core ingest scaling is unmeasured** (single-vCPU test machine); concurrency *correctness* is.

## 9. Project layout

```
include/chronoforge/
  models/       types.hpp  label_set.hpp  matcher.hpp          # immutable, validated domain types
  algorithms/   bit_stream.hpp  hashing.hpp  gorilla_codec.hpp # pure algorithms, no I/O, no locks
                postings.hpp  hyperloglog.hpp
  services/     inverted_index.hpp  cardinality_guard.hpp      # stateful components
                storage_engine.hpp  query_engine.hpp
src/            mirrors include/ for .cpp; main.cpp = demo driver; simulation/workload.hpp = deterministic data
tests/          test_suite.cpp                                 # oracle-based, dependency-free
docs/           sample_output.txt
```

Layering is strict: `algorithms/` knows nothing about services; `services/` compose algorithms; `main.cpp` only uses the public service APIs.

## 10. Design questions this project is built to answer

- *Why not just `unordered_map<labels, vector<Sample>>`?* → 16 B/sample, no label queries, one global lock.
- *Why is the first delta stored in 64 bits instead of the paper's 14?* → Losslessness for any input beats saving ~6 bytes once per 120 samples.
- *What breaks first at 10x the series count?* → Index memory (strings in `LabelSet` per series), then the global registry lock; both have identified fixes in §8.
- *Why exact counts for admission but HLL for reporting?* → Decisions that reject user data must be deterministic and explainable; estimates are for visibility.

## License
MIT see [`LICENSE`](LICENSE).
