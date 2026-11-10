#pragma once

#include <cstdint>

#include "lob/order_book.hpp"

namespace lob {

// A market-data / order-entry message in a fixed 32-byte binary layout,
// modelled on exchange feeds such as Nasdaq ITCH: fixed size, no pointers,
// no variable-length fields, so decoding is memcpy + dispatch.
//
//   offset 0   type    'A' add order, 'C' cancel order
//   offset 1   side    0 = buy, 1 = sell        (add only)
//   offset 2   otype   0 = limit, 1 = market    (add only)
//   offset 8   id      uint64 order id
//   offset 16  price   int64 ticks              (add only)
//   offset 24  qty     int64                    (add only)
struct alignas(8) Message {
    char type;
    std::uint8_t side;
    std::uint8_t order_type;
    std::uint8_t reserved[5];
    OrderId id;
    Price price;
    Quantity quantity;
};
static_assert(sizeof(Message) == 32, "feed messages must stay 32 bytes");

// Applies a stream of messages to a book, counting trades produced.
class FeedHandler {
public:
    explicit FeedHandler(OrderBook& book) : book_(book) {}

    // Returns the number of trades this message generated.
    std::size_t on_message(const Message& msg, std::vector<Trade>& trades) {
        std::size_t before = trades.size();
        switch (msg.type) {
            case 'A':
                book_.add_order(msg.id,
                                msg.side == 0 ? Side::Buy : Side::Sell,
                                msg.order_type == 0 ? OrderType::Limit
                                                    : OrderType::Market,
                                msg.price, msg.quantity, trades);
                break;
            case 'C':
                book_.cancel_order(msg.id);
                break;
            default:
                break;  // unknown message types are ignored, like a real feed
        }
        return trades.size() - before;
    }

private:
    OrderBook& book_;
};

}  // namespace lob
