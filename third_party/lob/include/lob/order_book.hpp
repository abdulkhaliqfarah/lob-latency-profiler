#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace lob {

// Prices are integer ticks (1 tick = $0.01). Integer prices keep matching
// exact -- no floating-point comparisons anywhere in the hot path.
using Price = std::int64_t;
using Quantity = std::int64_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class OrderType : std::uint8_t { Limit = 0, Market = 1 };

struct Trade {
    std::uint64_t trade_id;
    OrderId taker_id;   // the incoming order that caused the trade
    OrderId maker_id;   // the resting order that was matched
    Price price;        // execution price = the maker's (resting) price
    Quantity quantity;
};

// A single order. Orders live in a preallocated pool and are linked into a
// per-price-level FIFO queue through intrusive prev/next pointers, so
// insert-at-tail and cancel-by-id are both pointer surgery -- no allocation
// and no searching on the hot path.
struct Level;  // defined below

struct Order {
    OrderId id;
    Price price;
    Quantity quantity;      // remaining quantity
    Side side;
    Level* level = nullptr;  // owning price level, set while resting
    Order* prev = nullptr;   // older order at the same price level
    Order* next = nullptr;   // newer order at the same price level
};

// All orders resting at one price. Head is the oldest order and is matched
// first: strict price-time priority, the rule real exchanges use.
struct Level {
    Price price;
    Quantity total_quantity = 0;
    Order* head = nullptr;
    Order* tail = nullptr;
};

// Chunked pool allocator for orders. The matching loop never calls new:
// it pops a node from the free list and pushes it back on removal.
// Freed nodes are recycled, so steady-state operation is allocation-free.
class OrderPool {
public:
    explicit OrderPool(std::size_t chunk_size = 1 << 16);
    ~OrderPool();
    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete;

    Order* acquire();
    void release(Order* order);

private:
    void grow();
    std::size_t chunk_size_;
    std::vector<Order*> chunks_;
    Order* free_list_ = nullptr;
};

// A continuous double-auction limit order book for one instrument.
//
// Complexity (L = number of live price levels, usually small):
//   add, no match        O(log L)   price-level lookup
//   add, matching        O(log L)   per level consumed
//   cancel               O(1)       id index + intrusive unlink
//   best bid / ask       O(1)       map begin()
class OrderBook {
public:
    OrderBook() = default;

    // Submit an order. Incoming orders match immediately against the
    // opposite side while prices cross; any unfilled remainder of a limit
    // order rests on the book. A market order's remainder is cancelled
    // (immediate-or-cancel semantics). Fills are reported through the
    // trades vector, in execution order.
    void add_order(OrderId id, Side side, OrderType type, Price price,
                   Quantity quantity, std::vector<Trade>& trades);

    // Remove a resting order by id. Returns false if the id is not live
    // (unknown, already filled, or already cancelled).
    bool cancel_order(OrderId id);

    bool empty(Side side) const;
    Price best_bid() const;  // 0 when the bid side is empty
    Price best_ask() const;  // 0 when the ask side is empty
    Quantity quantity_at(Side side, Price price) const;
    std::size_t live_order_count() const { return index_.size(); }
    std::uint64_t next_trade_id() const { return trade_seq_; }

private:
    using BidMap = std::map<Price, Level*, std::greater<Price>>;
    using AskMap = std::map<Price, Level*, std::less<Price>>;

    // Walks the opposing book from its best price while `crosses(level
    // price)` holds, filling the taker. Returns the taker's remaining qty.
    template <typename OpposingMap, typename CrossFn>
    Quantity match(Order* taker, OpposingMap& opposing, CrossFn crosses,
                   std::vector<Trade>& trades);

    Level* find_or_create_level(Side side, Price price);
    void rest(Order* order);
    void unlink_and_release(Order* order);
    void erase_level_if_empty(Side side, Price price, Level* level);

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, Order*> index_;  // id -> resting order
    OrderPool pool_;
    std::uint64_t trade_seq_ = 0;
};

}  // namespace lob
