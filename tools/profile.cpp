// Latency profile of the limit order book matching engine.
//
//   profile stages   per-stage latency distribution over a mixed workload
//   profile books    controlled experiment: shallow vs deep resting book
//   profile sweeps   controlled experiment: match latency vs levels consumed
//   profile cancels  controlled experiment: cancel latency vs live order count
//
// Every number printed is measured in this run. Workloads are deterministic
// (splitmix64, fixed seeds), so experiments are repeatable.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "lob/order_book.hpp"
#include "prof/staged_profiler.hpp"
#include "prof/workload.hpp"

using namespace lob;
using namespace prof;

static void print_row(const char* label, const LatencyStats& s) {
    std::printf("%-38s n=%-8zu mean=%9.0f ns  p50=%7llu  p99=%7llu  "
                "p99.9=%8llu  max=%9llu ns\n",
                label, s.count(), s.mean(),
                (unsigned long long)s.p50(), (unsigned long long)s.p99(),
                (unsigned long long)s.p999(), (unsigned long long)s.max());
}

static int stages() {
    OrderBook book;
    WorkloadGen gen(42, 100000, 5);
    std::vector<Trade> trades;
    // Warm-up, unmeasured: depth on the book and hot caches before the
    // profiler starts recording.
    FeedHandler warm(book);
    for (int i = 0; i < 20000; ++i) warm.on_message(gen.next_resting(), trades);
    trades.clear();
    StagedProfiler prof(book);
    const int kMessages = 300000;
    for (int i = 0; i < kMessages; ++i) {
        std::uint64_t r = gen.next_rand();
        Message m;
        if (r % 10 < 6) {
            m = gen.next_resting();
        } else if (r % 10 < 8) {
            m = gen.next_crossing();
        } else {
            m = gen.next_cancel();
        }
        prof.on_message(m, trades);
    }
    prof.finalize();
    std::printf("== per-stage latency, mixed workload (%d messages) ==\n", kMessages);
    print_row("decode", prof.decode());
    print_row("apply: rest (book update)", prof.apply(Stage::Rest));
    print_row("apply: match", prof.apply(Stage::Match));
    print_row("apply: cancel", prof.apply(Stage::Cancel));
    print_row("total (decode + apply)", prof.total());
    std::printf("trades produced: %zu, live orders at end: %zu\n", trades.size(),
                book.live_order_count());
    return 0;
}

static void book_depth_run(const char* label, int resting, int levels) {
    OrderBook book;
    preload_book(book, 100000, resting, levels, 7);
    StagedProfiler prof(book);
    WorkloadGen gen(99, 100000, 5);
    std::vector<Trade> trades;
    for (int i = 0; i < 20000; ++i) prof.on_message(gen.next_resting(), trades);
    prof.finalize();
    print_row(label, prof.apply(Stage::Rest));
}

static int books() {
    std::printf("== experiment: resting-add latency vs book depth ==\n");
    book_depth_run("shallow book (1k orders, 10 levels)", 1000, 10);
    book_depth_run("deep book (50k orders, 200 levels)", 50000, 200);
    book_depth_run("very deep (100k orders, 1000 levels)", 100000, 1000);
    return 0;
}

static void sweep_run(int depth_levels) {
    LatencyStats st;
    std::vector<Trade> trades;
    const int kTrials = 2000;
    for (int t = 0; t < kTrials; ++t) {
        // Fresh ladder each trial (construction is outside the timed region):
        // one ask level per tick, 100 shares each, so the timed marketable
        // buy consumes exactly depth_levels levels.
        OrderBook book;
        std::vector<Trade> sink;
        OrderId id = 1;
        for (int i = 0; i < depth_levels; ++i) {
            book.add_order(id++, Side::Sell, OrderType::Limit, 100000 + i, 100, sink);
        }
        auto t0 = std::chrono::steady_clock::now();
        book.add_order(id, Side::Buy, OrderType::Limit, 100000 + depth_levels,
                       depth_levels * 100, trades);
        auto t1 = std::chrono::steady_clock::now();
        st.add((std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        trades.clear();
    }
    st.finalize();
    char label[96];
    std::snprintf(label, sizeof label, "sweep consuming %d levels", depth_levels);
    print_row(label, st);
}

static int sweeps() {
    std::printf("== experiment: match latency vs levels consumed ==\n");
    sweep_run(1);
    sweep_run(10);
    sweep_run(50);
    return 0;
}

static void cancel_run(int live) {
    OrderBook book;
    auto ids = preload_book(book, 100000, live, 500, 11);
    LatencyStats st;
    for (OrderId id : ids) {
        auto t0 = std::chrono::steady_clock::now();
        book.cancel_order(id);
        auto t1 = std::chrono::steady_clock::now();
        st.add((std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    st.finalize();
    char label[96];
    std::snprintf(label, sizeof label, "cancel with %d live orders", live);
    print_row(label, st);
}

static int cancels() {
    std::printf("== experiment: cancel latency vs live order count ==\n");
    cancel_run(1000);
    cancel_run(100000);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: profile <stages|books|sweeps|cancels>\n");
        return 2;
    }
    if (std::strcmp(argv[1], "stages") == 0) return stages();
    if (std::strcmp(argv[1], "books") == 0) return books();
    if (std::strcmp(argv[1], "sweeps") == 0) return sweeps();
    if (std::strcmp(argv[1], "cancels") == 0) return cancels();
    std::printf("unknown mode %s\n", argv[1]);
    return 2;
}
