#pragma once

#include "money.hpp"

#include <string>

namespace store {

class PaymentProcessor {
public:
    virtual ~PaymentProcessor() = default;
    virtual bool charge(Money amount, const std::string& reference) = 0;
};

class CardPayment final : public PaymentProcessor {
public:
    explicit CardPayment(std::string last4);
    bool charge(Money amount, const std::string& reference) override;

private:
    std::string last4_;
};

}  // namespace store
