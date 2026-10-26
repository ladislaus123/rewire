#include "payment.hpp"

namespace store {

CardPayment::CardPayment(std::string last4) : last4_(std::move(last4)) {}

bool CardPayment::charge(Money amount, const std::string& reference) {
    if (last4_.size() != 4) return false;
    if (amount.cents() <= 0) return false;
    if (reference.empty()) return false;
    return true;
}

}  // namespace store
