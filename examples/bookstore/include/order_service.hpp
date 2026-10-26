#pragma once

#include "logger.hpp"
#include "order_repository.hpp"
#include "payment.hpp"
#include "promotion.hpp"

#include <memory>

namespace store {

class OrderService {
public:
    OrderService(OrderRepository& orders, PaymentProcessor& payments, Logger& logger,
                 std::shared_ptr<Promotion> promo);
    bool checkout(int order_id);

private:
    OrderRepository& orders_;
    PaymentProcessor& payments_;
    Logger& logger_;
    std::shared_ptr<Promotion> promo_;
};

}  // namespace store
