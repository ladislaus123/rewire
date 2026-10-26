#include "order_repository.hpp"

namespace store {

OrderRepository::OrderRepository(Logger& logger) : logger_(logger) {}

Order* OrderRepository::find(int id) {
    for (Order& order : orders_) {
        if (order.id() == id) return &order;
    }
    logger_.warn("order not found");
    return nullptr;
}

void OrderRepository::save(const Order& order) {
    Order copy = order;
    for (Order& existing : orders_) {
        if (existing.id() == copy.id()) {
            existing = copy;
            logger_.info("order updated");
            return;
        }
    }
    orders_.push_back(std::move(copy));
    logger_.info("order saved");
}

}  // namespace store
