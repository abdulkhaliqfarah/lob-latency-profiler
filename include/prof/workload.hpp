#pragma once

#include <cstdint>
#include <vector>

#include "lob/feed.hpp"

namespace prof {

// splitmix64: deterministic on every platform. Same seed -> same workload,
// which is what makes latency comparisons between runs meaningful.
class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next_u64() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::uint64_t below(std::uint64_t n) { return next_u64() % n; }

private:
    std::uint64_t state_;
};

// Deterministic workload generator for the matching engine. All prices are
// ticks around a mid price; ids are unique and increasing. The generator
// tracks which ids are live so cancels always target a resting order, the
// same discipline a real order-entry session has.
class WorkloadGen {
public:
    WorkloadGen(std::uint64_t seed, lob::Price mid, lob::Price spread_ticks)
        : rng_(seed), mid_(mid), spread_(spread_ticks) {}

    // Raw draw from the generator's stream (for workload mixing decisions).
    std::uint64_t next_rand() { return rng_.next_u64(); }

    // A limit order placed away from the touch: it rests, it never crosses.
    lob::Message next_resting() {
        lob::Message m{};
        bool buy = (rng_.next_u64() & 1) != 0;
        lob::Price offset = spread_ + static_cast<lob::Price>(rng_.below(20));
        m.type = 'A';
        m.side = buy ? 0 : 1;
        m.order_type = 0;
        m.price = buy ? mid_ - offset : mid_ + offset;
        m.quantity = 1 + static_cast<lob::Quantity>(rng_.below(100));
        m.id = next_id_++;
        live_.push_back(m.id);
        return m;
    }

    // A marketable limit order priced through the touch: it matches.
    lob::Message next_crossing() {
        lob::Message m{};
        bool buy = (rng_.next_u64() & 1) != 0;
        lob::Price through = 1 + static_cast<lob::Price>(rng_.below(5));
        m.type = 'A';
        m.side = buy ? 0 : 1;
        m.order_type = 0;
        m.price = buy ? mid_ + through : mid_ - through;
        m.quantity = 1 + static_cast<lob::Quantity>(rng_.below(50));
        m.id = next_id_++;
        return m;  // may partially rest; not tracked for cancels
    }

    // A cancel for a live resting order; a resting add if none are live.
    lob::Message next_cancel() {
        if (live_.empty()) return next_resting();
        std::size_t idx = static_cast<std::size_t>(rng_.below(live_.size()));
        lob::Message m{};
        m.type = 'C';
        m.id = live_[idx];
        live_[idx] = live_.back();
        live_.pop_back();
        return m;
    }

private:
    SplitMix64 rng_;
    lob::Price mid_;
    lob::Price spread_;
    lob::OrderId next_id_ = 1;
    std::vector<lob::OrderId> live_;
};

// Builds a book with `resting` passive orders spread over `levels` distinct
// prices per side, and returns the ids placed (for later cancels). Used to
// create shallow vs deep books for the controlled experiments.
inline std::vector<lob::OrderId> preload_book(lob::OrderBook& book,
                                              lob::Price mid, int resting,
                                              int levels, std::uint64_t seed) {
    SplitMix64 rng(seed);
    std::vector<lob::OrderId> ids;
    std::vector<lob::Trade> sink;
    lob::OrderId id = 1;
    for (int i = 0; i < resting; ++i) {
        bool buy = (i % 2 == 0);
        lob::Price lvl = 1 + static_cast<lob::Price>(rng.below(levels));
        lob::Price price = buy ? mid - lvl : mid + lvl;
        book.add_order(id, buy ? lob::Side::Buy : lob::Side::Sell,
                       lob::OrderType::Limit, price,
                       10 + static_cast<lob::Quantity>(rng.below(90)), sink);
        ids.push_back(id++);
    }
    return ids;
}

}  // namespace prof
