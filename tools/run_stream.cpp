// Replays a text operation stream through the matching engine twice -- once
// through the bare FeedHandler and once through the StagedProfiler -- and
// asserts the two runs produce identical trades and identical final state.
// This is the profiler's core safety property: measuring must not change
// behavior. It then prints the profiled run's results for scripts/model.py,
// which diffs them against an independent naive Python model.
//
// Input:
//   A <id> <B|S> <L|M> <price> <qty>     add order
//   C <id>                              cancel order
//   END
//   Q <B|S> <price>                     quantity query (any number)
// Output:
//   T <trade_id> <taker_id> <maker_id> <price> <qty>
//   Q <B|S> <price> <qty>               answer per query
//   LIVE <n>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "lob/feed.hpp"
#include "prof/staged_profiler.hpp"

using namespace lob;
using namespace prof;

struct Op {
    char kind = 0;
    OrderId id = 0;
    int side = 0;
    int otype = 0;
    Price price = 0;
    Quantity qty = 0;
};

static Message to_message(const Op& op) {
    Message m{};
    m.type = op.kind;
    m.side = static_cast<std::uint8_t>(op.side);
    m.order_type = static_cast<std::uint8_t>(op.otype);
    m.id = op.id;
    m.price = op.price;
    m.quantity = op.qty;
    return m;
}

static bool same_trades(const std::vector<Trade>& a, const std::vector<Trade>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].trade_id != b[i].trade_id || a[i].taker_id != b[i].taker_id ||
            a[i].maker_id != b[i].maker_id || a[i].price != b[i].price ||
            a[i].quantity != b[i].quantity)
            return false;
    }
    return true;
}

int main() {
    std::vector<Op> ops;
    struct Query {
        int side;
        Price price;
    };
    std::vector<Query> queries;
    char line[256];
    bool in_queries = false;
    while (std::fgets(line, sizeof line, stdin)) {
        if (line[0] == 'E') {  // END
            in_queries = true;
            continue;
        }
        if (!in_queries && line[0] == 'A') {
            Op op{};
            char side_c, type_c;
            if (std::sscanf(line, "A %llu %c %c %lld %lld",
                            (unsigned long long*)&op.id, &side_c, &type_c,
                            (long long*)&op.price, (long long*)&op.qty) != 5) {
                std::fprintf(stderr, "bad line: %s", line);
                return 2;
            }
            op.kind = 'A';
            op.side = side_c == 'B' ? 0 : 1;
            op.otype = type_c == 'L' ? 0 : 1;
            ops.push_back(op);
        } else if (!in_queries && line[0] == 'C') {
            Op op{};
            if (std::sscanf(line, "C %llu", (unsigned long long*)&op.id) != 1) {
                std::fprintf(stderr, "bad line: %s", line);
                return 2;
            }
            op.kind = 'C';
            ops.push_back(op);
        } else if (in_queries && line[0] == 'Q') {
            Query q{};
            char side_c;
            if (std::sscanf(line, "Q %c %lld", &side_c, (long long*)&q.price) != 2) {
                std::fprintf(stderr, "bad line: %s", line);
                return 2;
            }
            q.side = side_c == 'B' ? 0 : 1;
            queries.push_back(q);
        }
    }

    OrderBook book_a;
    FeedHandler feed_a(book_a);
    std::vector<Trade> trades_a;
    for (const Op& op : ops) feed_a.on_message(to_message(op), trades_a);

    OrderBook book_b;
    StagedProfiler prof_b(book_b);
    std::vector<Trade> trades_b;
    for (const Op& op : ops) prof_b.on_message(to_message(op), trades_b);

    if (!same_trades(trades_a, trades_b) ||
        book_a.live_order_count() != book_b.live_order_count()) {
        std::fprintf(stderr, "MISMATCH: profiled run diverged from bare run\n");
        return 1;
    }

    for (const Trade& t : trades_b) {
        std::printf("T %llu %llu %llu %lld %lld\n",
                    (unsigned long long)t.trade_id,
                    (unsigned long long)t.taker_id,
                    (unsigned long long)t.maker_id, (long long)t.price,
                    (long long)t.quantity);
    }
    for (const Query& q : queries) {
        Quantity qty = book_b.quantity_at(q.side == 0 ? Side::Buy : Side::Sell, q.price);
        std::printf("Q %c %lld %lld\n", q.side == 0 ? 'B' : 'S',
                    (long long)q.price, (long long)qty);
    }
    std::printf("LIVE %zu\n", book_b.live_order_count());
    return 0;
}
