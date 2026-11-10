// Scenario checks for the latency profiler: exact percentile math, workload
// determinism, stage classification, and the profiler's core safety property
// -- a profiled run must produce exactly the trades of a bare run.
#include <cstdio>
#include <vector>

#include "lob/feed.hpp"
#include "prof/latency_stats.hpp"
#include "prof/staged_profiler.hpp"
#include "prof/workload.hpp"

using namespace lob;
using namespace prof;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                     \
    do {                                                \
        ++checks;                                       \
        if (!(cond)) {                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__,   \
                        __LINE__, #cond);               \
            ++failures;                                 \
        }                                               \
    } while (0)

static void test_percentile_exact() {
    LatencyStats s;
    for (std::uint64_t v = 1; v <= 100; ++v) s.add(v);
    s.finalize();
    CHECK(s.count() == 100);
    CHECK(s.min() == 1);
    CHECK(s.max() == 100);
    CHECK(s.p50() == 50);        // nearest-rank
    CHECK(s.p99() == 99);
    CHECK(s.p999() == 100);
    CHECK(s.mean() == 50.5);
    LatencyStats single;
    single.add(42);
    single.finalize();
    CHECK(single.p50() == 42 && single.p999() == 42 && single.max() == 42);
    LatencyStats empty;
    empty.finalize();
    CHECK(empty.count() == 0 && empty.p50() == 0 && empty.max() == 0);
}

static void test_percentile_unsorted_input() {
    LatencyStats s;
    // Same multiset as 1..100 but shuffled deterministically; results must
    // be identical because finalize() sorts.
    for (std::uint64_t i = 0; i < 100; ++i) s.add(((i * 37) % 100) + 1);
    s.finalize();
    CHECK(s.p50() == 50);
    CHECK(s.p99() == 99);
    CHECK(s.mean() == 50.5);
}

static bool same_msg(const Message& a, const Message& b) {
    return a.type == b.type && a.side == b.side && a.order_type == b.order_type &&
           a.id == b.id && a.price == b.price && a.quantity == b.quantity;
}

static void test_workload_determinism() {
    WorkloadGen g1(42, 100000, 5), g2(42, 100000, 5);
    for (int i = 0; i < 1000; ++i) {
        Message a, b;
        switch (i % 3) {
            case 0: a = g1.next_resting(); b = g2.next_resting(); break;
            case 1: a = g1.next_crossing(); b = g2.next_crossing(); break;
            default: a = g1.next_cancel(); b = g2.next_cancel(); break;
        }
        CHECK(same_msg(a, b));
    }
    // Resting orders never cross the mid by construction.
    WorkloadGen g3(7, 100000, 5);
    for (int i = 0; i < 500; ++i) {
        Message m = g3.next_resting();
        if (m.side == 0) CHECK(m.price < 100000);
        if (m.side == 1) CHECK(m.price > 100000);
    }
}

static void test_stage_classification() {
    OrderBook book;
    StagedProfiler prof(book);
    std::vector<Trade> trades;
    Message rest{};
    rest.type = 'A'; rest.side = 1; rest.order_type = 0;
    rest.id = 1; rest.price = 100100; rest.quantity = 10;
    prof.on_message(rest, trades);
    CHECK(prof.apply(Stage::Rest).count() == 1);
    CHECK(prof.apply(Stage::Match).count() == 0);
    Message cross{};
    cross.type = 'A'; cross.side = 0; cross.order_type = 0;
    cross.id = 2; cross.price = 100100; cross.quantity = 5;
    prof.on_message(cross, trades);
    CHECK(prof.apply(Stage::Match).count() == 1);
    CHECK(trades.size() == 1);
    Message cancel{};
    cancel.type = 'C'; cancel.id = 1;
    prof.on_message(cancel, trades);
    CHECK(prof.apply(Stage::Cancel).count() == 1);
    CHECK(prof.decode().count() == 3);
    CHECK(prof.total().count() == 3);
}

static void test_profiled_matches_bare() {
    // The safety property, over a long mixed deterministic stream.
    WorkloadGen gen(1234, 100000, 5);
    std::vector<Message> stream;
    for (int i = 0; i < 50000; ++i) {
        std::uint64_t r = gen.next_rand();
        if (r % 10 < 6) stream.push_back(gen.next_resting());
        else if (r % 10 < 8) stream.push_back(gen.next_crossing());
        else stream.push_back(gen.next_cancel());
    }
    OrderBook book_a;
    FeedHandler feed_a(book_a);
    std::vector<Trade> trades_a;
    for (const auto& m : stream) feed_a.on_message(m, trades_a);

    OrderBook book_b;
    StagedProfiler prof_b(book_b);
    std::vector<Trade> trades_b;
    for (const auto& m : stream) prof_b.on_message(m, trades_b);

    CHECK(trades_a.size() == trades_b.size());
    bool identical = trades_a.size() == trades_b.size();
    for (std::size_t i = 0; identical && i < trades_a.size(); ++i) {
        identical = trades_a[i].trade_id == trades_b[i].trade_id &&
                    trades_a[i].taker_id == trades_b[i].taker_id &&
                    trades_a[i].maker_id == trades_b[i].maker_id &&
                    trades_a[i].price == trades_b[i].price &&
                    trades_a[i].quantity == trades_b[i].quantity;
    }
    CHECK(identical);
    CHECK(book_a.live_order_count() == book_b.live_order_count());
    CHECK(book_a.best_bid() == book_b.best_bid());
    CHECK(book_a.best_ask() == book_b.best_ask());
    CHECK(!trades_a.empty());  // the stream really produced trades
    prof_b.finalize();
    CHECK(prof_b.total().count() == stream.size());
    CHECK(prof_b.decode().p50() > 0);
}

static void test_preload_book() {
    OrderBook book;
    auto ids = preload_book(book, 100000, 2000, 50, 3);
    CHECK(book.live_order_count() == 2000);
    CHECK(ids.size() == 2000);
    CHECK(book.best_bid() < 100000);
    CHECK(book.best_ask() > 100000);
    // No crossing inside the preloaded book.
    CHECK(book.best_bid() < book.best_ask());
}

int main() {
    test_percentile_exact();
    test_percentile_unsorted_input();
    test_workload_determinism();
    test_stage_classification();
    test_profiled_matches_bare();
    test_preload_book();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
