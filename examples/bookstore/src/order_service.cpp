#include "order_service.hpp"

namespace store {

OrderService::OrderService(OrderRepository& orders, PaymentProcessor& payments, Logger& logger,
                           std::shared_ptr<Promotion> promo)
    : orders_(orders), payments_(payments), logger_(logger), promo_(std::move(promo)) {}

bool OrderService::checkout(int order_id) {
    Order* order = orders_.find(order_id);
    if (order == nullptr) {
        logger_.warn("missing order");
        return false;
    }
    if (promo_ && promo_->code().empty()) {
        logger_.warn("empty promotion");
        return false;
    }
    Money total = order->total();
    bool paid = payments_.charge(total, "checkout");
    if (!paid) {
        logger_.warn("payment declined");
        return false;
    }
    orders_.save(*order);
    logger_.info("checked out");
    return true;
}

}  // namespace store
