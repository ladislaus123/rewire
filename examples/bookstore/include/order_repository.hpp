#pragma once

#include "logger.hpp"
#include "order.hpp"

#include <vector>

namespace store {

class OrderRepository {
public:
    explicit OrderRepository(Logger& logger);
    Order* find(int id);
    void save(const Order& order);

private:
    Logger& logger_;
    std::vector<Order> orders_;
};

}  // namespace store
