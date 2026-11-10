#include "lob/order_book.hpp"

#include <algorithm>

namespace lob {

// ---------------------------------------------------------------- OrderPool

OrderPool::OrderPool(std::size_t chunk_size) : chunk_size_(chunk_size) {
    grow();
}

OrderPool::~OrderPool() {
    for (Order* chunk : chunks_) delete[] chunk;
}

void OrderPool::grow() {
    Order* chunk = new Order[chunk_size_];
    chunks_.push_back(chunk);
    for (std::size_t i = 0; i < chunk_size_; ++i) {
        chunk[i].next = free_list_;
        free_list_ = &chunk[i];
    }
}

Order* OrderPool::acquire() {
    if (free_list_ == nullptr) grow();
    Order* order = free_list_;
    free_list_ = order->next;
    order->prev = nullptr;
    order->next = nullptr;
    order->level = nullptr;
    return order;
}

void OrderPool::release(Order* order) {
    order->prev = nullptr;
    order->next = free_list_;
    free_list_ = order;
}

// ---------------------------------------------------------------- OrderBook

Level* OrderBook::find_or_create_level(Side side, Price price) {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        if (it != bids_.end()) return it->second;
        auto* level = new Level{price};
        bids_.emplace(price, level);
        return level;
    }
    auto it = asks_.find(price);
    if (it != asks_.end()) return it->second;
    auto* level = new Level{price};
    asks_.emplace(price, level);
    return level;
}

void OrderBook::erase_level_if_empty(Side side, Price price, Level* level) {
    if (level->head != nullptr) return;
    if (side == Side::Buy) {
        bids_.erase(price);
    } else {
        asks_.erase(price);
    }
    delete level;
}

// Unlink an order from its level's FIFO queue and from the id index,
// then return its node to the pool. Strictly O(1): the order carries a
// pointer to its level, so there is no lookup here at all.
void OrderBook::unlink_and_release(Order* order) {
    Level* level = order->level;
    if (order->prev != nullptr) {
        order->prev->next = order->next;
    } else {
        level->head = order->next;
    }
    if (order->next != nullptr) {
        order->next->prev = order->prev;
    } else {
        level->tail = order->prev;
    }
    level->total_quantity -= order->quantity;
    index_.erase(order->id);
    erase_level_if_empty(order->side, order->price, level);
    pool_.release(order);
}

void OrderBook::rest(Order* order) {
    Level* level = find_or_create_level(order->side, order->price);
    order->level = level;
    order->prev = level->tail;
    order->next = nullptr;
    if (level->tail != nullptr) {
        level->tail->next = order;
    } else {
        level->head = order;
    }
    level->tail = order;
    level->total_quantity += order->quantity;
    index_.emplace(order->id, order);
}

template <typename OpposingMap, typename CrossFn>
Quantity OrderBook::match(Order* taker, OpposingMap& opposing, CrossFn crosses,
                          std::vector<Trade>& trades) {
    while (taker->quantity > 0 && !opposing.empty()) {
        auto best_it = opposing.begin();  // best price on the opposing side
        Level* level = best_it->second;
        if (!crosses(level->price)) break;

        while (taker->quantity > 0 && level->head != nullptr) {
            Order* maker = level->head;
            Quantity fill = std::min(taker->quantity, maker->quantity);

            trades.push_back(Trade{++trade_seq_, taker->id, maker->id,
                                   maker->price, fill});
            taker->quantity -= fill;
            maker->quantity -= fill;
            level->total_quantity -= fill;

            if (maker->quantity == 0) {
                OrderId maker_id = maker->id;
                index_.erase(maker_id);
                level->head = maker->next;
                if (maker->next != nullptr) {
                    maker->next->prev = nullptr;
                } else {
                    level->tail = nullptr;
                }
                pool_.release(maker);
            }
        }
        if (level->head == nullptr) {
            opposing.erase(best_it);
            delete level;
        }
    }
    return taker->quantity;
}

void OrderBook::add_order(OrderId id, Side side, OrderType type, Price price,
                          Quantity quantity, std::vector<Trade>& trades) {
    if (quantity <= 0) return;

    Order* taker = pool_.acquire();
    taker->id = id;
    taker->side = side;
    taker->price = price;
    taker->quantity = quantity;

    if (side == Side::Buy) {
        auto crosses = [taker, type](Price ask) {
            return type == OrderType::Market || ask <= taker->price;
        };
        match(taker, asks_, crosses, trades);
    } else {
        auto crosses = [taker, type](Price bid) {
            return type == OrderType::Market || bid >= taker->price;
        };
        match(taker, bids_, crosses, trades);
    }

    if (taker->quantity > 0 && type == OrderType::Limit) {
        rest(taker);
    } else {
        pool_.release(taker);  // fully filled, or market-order remainder
    }
}

bool OrderBook::cancel_order(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    unlink_and_release(it->second);
    return true;
}

bool OrderBook::empty(Side side) const {
    return side == Side::Buy ? bids_.empty() : asks_.empty();
}

Price OrderBook::best_bid() const {
    return bids_.empty() ? 0 : bids_.begin()->first;
}

Price OrderBook::best_ask() const {
    return asks_.empty() ? 0 : asks_.begin()->first;
}

Quantity OrderBook::quantity_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second->total_quantity;
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second->total_quantity;
}

}  // namespace lob
