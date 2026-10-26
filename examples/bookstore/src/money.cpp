#include "money.hpp"

namespace store {

Money::Money(int cents) : cents_(cents) {}

int Money::cents() const { return cents_; }

Money Money::plus(Money other) const { return Money(cents_ + other.cents_); }

std::string Money::format() const {
    int dollars = cents_ / 100;
    int cents = cents_ % 100;
    if (cents < 0) cents = -cents;
    std::string text = std::to_string(dollars) + ".";
    if (cents < 10) text.push_back('0');
    text += std::to_string(cents);
    return text;
}

}  // namespace store
