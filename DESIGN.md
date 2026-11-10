# Design Notes — LOB Latency Profiler

## Stage model

Every incoming 32-byte feed message passes through three timed stages:

1. **decode** — wire fields are read into a typed command (side, type, id,
   price, quantity). This is the feed handler's dispatch work.
2. **apply** — the engine call itself, classified *after the fact* by what
   it did: an add that produced trades is a **match**, an add that rested
   (or a no-op) is a **rest / book update**, a cancel message is a
   **cancel**. Classifying by outcome rather than by message type matters:
   a crossing limit order and a passive one are the same message type with
   completely different cost profiles.
3. **total** — decode + apply, end to end.

## Measurement method, and its honest limits

- Timing is `std::chrono::steady_clock` (monotonic) around each stage.
  Two clock reads cost tens of nanoseconds; that overhead is inside every
  sample, is roughly constant, and is the same for all stages and
  workloads — so comparisons between stages and between experiments are
  valid, while absolute numbers carry that additive offset. The decode
  stage (~50 ns at p50/p99) is close to the floor this method can resolve
  and should be read as "at most ~50 ns", not as a precise decode cost.
- Percentiles are **exact order statistics**: samples are stored and
  sorted, and percentiles use the nearest-rank definition. No histogram
  bucketing, no interpolation — a reported p99 is a real measured sample.
- A 20,000-message unmeasured warm-up precedes each run so the book has
  realistic depth and caches/branch predictors are hot; cold-start effects
  are a different study.
- The sandbox is a shared VM, so preemption spikes (occasionally
  milliseconds) contaminate the extreme tail. Runs were repeated to
  confirm which numbers are stable (medians, the sweep slope) and which
  are environmental (max, and p99.9 to a lesser degree), and the README
  says which is which.

## The experiments

Each experiment changes exactly one variable against the deterministic
generator (splitmix64, fixed seeds — the same stream every run):

- **books** — resting adds against preloaded books of 1k / 50k / 100k
  orders over 10 / 200 / 1000 levels. Result: p50 flat at ~110–130 ns.
  The engine's per-level `std::map` lookup is O(log L) with small L, and
  the pool allocator keeps adds allocation-free; neither shows up at these
  sizes.
- **sweeps** — a marketable order consuming exactly 1 / 10 / 50 price
  levels (fresh ladder per trial, construction untimed). Result: p50
  grows linearly at ~60 ns per level — the per-level cost of unlinking the
  level's orders, emitting trades, and erasing the level.
- **cancels** — cancelling every order of a 1k-order and a 100k-order
  book. Result: no p50 growth, confirming the O(1) cancel design (id index
  + intrusive unlink) empirically rather than by code inspection.

## The safety property

A profiler that perturbs the system it measures is worse than none. The
wrapper is therefore kept surgically thin — it reads the message, calls
the same `add_order` / `cancel_order` the bare `FeedHandler` calls, and
records times. Two layers of tests enforce equivalence: the C++ suite runs
a 50,000-message stream through both paths and compares every trade and
the final book; `tools/run_stream` repeats that comparison on externally
supplied streams, and `scripts/model.py` then diffs the profiled output
against a naive Python implementation of the matching rules.
