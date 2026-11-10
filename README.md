# LOB Latency Profiler — Where the Nanoseconds Go

A per-stage latency profiler for a C++20
[limit order book matching engine](https://github.com/abdulkhaliqfarah/limit-order-book).
Matching-engine work is usually quoted as a single throughput number; this
project splits every feed message into **decode**, **book update (rest)**,
**match**, and **cancel**, and reports exact percentiles — p50, p99, p99.9,
max — for each stage, then runs controlled experiments that isolate one
variable at a time: book depth, levels consumed per match, and live order
count.

The profiler's first rule: **measuring must not change behavior.** A
profiled run and a bare run of the same stream must produce identical trade
logs and identical final book state. That property is tested, not assumed
(see Verification).

## Quick start

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/profile stages    # per-stage distribution, 300k mixed messages
./build/profile books     # resting-add latency vs book depth
./build/profile sweeps    # match latency vs levels consumed
./build/profile cancels   # cancel latency vs live order count
ctest --test-dir build                         # 1,532 scenario checks
python3 scripts/model.py ./build/run_stream    # differential vs Python model
```

## Measured findings

Numbers below are from runs on a shared sandbox VM (1.5 vCPU) — read the
medians as engine cost and the extreme tail as mostly environment, which is
itself one of the findings. All workloads are deterministic (splitmix64,
fixed seeds), and the experiments were re-run to confirm they reproduce.

**Per-stage, mixed workload of 300,000 messages** (61,817 trades):

| Stage | p50 | p99 |
|---|---|---|
| decode | ~50 ns | ~51 ns |
| rest (book update) | ~130 ns | ~0.3–0.4 µs |
| match | ~110–120 ns | ~1.7 µs |
| cancel | ~270–370 ns | ~0.7–0.9 µs |
| **total per message** | **~180–190 ns** | **~0.6–0.8 µs** |

**Match cost is linear in levels consumed.** A marketable order sweeping a
ladder of one-share-deep levels costs ~150 ns at 1 level, ~0.70–0.73 µs at
10 levels, and ~3.0–3.2 µs at 50 levels — about **60 ns per additional
price level**, reproducibly across runs. Depth per level, not book size,
is what a big aggressive order pays for.

**Resting-add and cancel latency are flat in book size.** Resting adds hold
p50 ≈ 110–130 ns from a 1,000-order book to a 100,000-order book, and
cancel p50 does not grow from 1k to 100k live orders — the empirical
signature of the engine's O(1) cancel (intrusive level queues + id index)
and the level lookup staying off the critical path.

**The tail is not the engine.** On this shared VM, total latency reaches
p99.9 ≈ 2–8 µs and single samples spike into milliseconds while the median
sits at ~180 ns. That gap is scheduler preemption, not matching work — the
reason serious latency work pins cores and measures on isolated hardware,
and the reason this README reports percentiles instead of a heroic mean.

## Verification

- **1,532 scenario checks** (`tests/test_profiler.cpp`): exact percentile
  math on known distributions, workload determinism (same seed → identical
  stream), stage classification, preloaded-book invariants, and the safety
  property — a 50,000-message profiled run produces exactly the trades and
  final state of a bare run.
- **Differential test** (`scripts/model.py`): an independent naive Python
  model of the matching rules agrees exactly with the profiled C++ run on
  20,000- and 50,000-operation streams — full trade log, every final level
  quantity, and live order count.

The engine under measurement is vendored under `third_party/lob/` from the
author's limit-order-book repo so this project builds standalone. See
`DESIGN.md` for methodology and its limits.
