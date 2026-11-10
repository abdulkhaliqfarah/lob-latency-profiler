#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

#include "lob/feed.hpp"
#include "lob/order_book.hpp"
#include "prof/latency_stats.hpp"

namespace prof {

// What one feed message turned out to be, decided by what the engine did.
enum class Stage : std::uint8_t { Rest = 0, Match = 1, Cancel = 2, Count = 3 };

inline const char* stage_name(Stage s) {
    switch (s) {
        case Stage::Rest: return "rest (book update)";
        case Stage::Match: return "match";
        case Stage::Cancel: return "cancel";
        default: return "?";
    }
}

// A profiling wrapper around the matching engine. Every message is timed
// through three stages:
//
//   decode   the 32-byte wire message is read into a typed command -- the
//            work a real feed handler does before the engine sees anything.
//   apply    the engine call itself (add_order / cancel_order), classified
//            after the fact as rest / match / cancel by its outcome.
//   total    decode + apply, end to end.
//
// Timing uses std::chrono::steady_clock around each stage. The clock calls
// themselves cost tens of nanoseconds; that overhead is inside every sample
// and is the same for all stages, so cross-stage and cross-workload
// comparisons stand. See DESIGN.md for the measurement methodology.
class StagedProfiler {
public:
    explicit StagedProfiler(lob::OrderBook& book) : book_(book) {}

    void on_message(const lob::Message& msg, std::vector<lob::Trade>& trades) {
        using Clock = std::chrono::steady_clock;

        auto t0 = Clock::now();
        // ---- decode stage -------------------------------------------------
        char type = msg.type;
        lob::Side side = msg.side == 0 ? lob::Side::Buy : lob::Side::Sell;
        lob::OrderType otype =
            msg.order_type == 0 ? lob::OrderType::Limit : lob::OrderType::Market;
        lob::OrderId id = msg.id;
        lob::Price price = msg.price;
        lob::Quantity qty = msg.quantity;
        auto t1 = Clock::now();

        // ---- apply stage --------------------------------------------------
        Stage stage;
        std::size_t trades_before = trades.size();
        if (type == 'A') {
            book_.add_order(id, side, otype, price, qty, trades);
            stage = trades.size() > trades_before ? Stage::Match : Stage::Rest;
        } else if (type == 'C') {
            book_.cancel_order(id);
            stage = Stage::Cancel;
        } else {
            stage = Stage::Rest;  // unknown types are ignored, like a real feed
        }
        auto t2 = Clock::now();

        auto ns = [](Clock::time_point a, Clock::time_point b) {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        };
        decode_.add(ns(t0, t1));
        apply_[static_cast<std::size_t>(stage)].add(ns(t1, t2));
        total_.add(ns(t0, t2));
    }

    void finalize() {
        decode_.finalize();
        total_.finalize();
        for (auto& s : apply_) s.finalize();
    }

    const LatencyStats& decode() const { return decode_; }
    const LatencyStats& total() const { return total_; }
    const LatencyStats& apply(Stage s) const { return apply_[static_cast<std::size_t>(s)]; }

private:
    lob::OrderBook& book_;
    LatencyStats decode_;
    LatencyStats total_;
    LatencyStats apply_[static_cast<std::size_t>(Stage::Count)];
};

}  // namespace prof
