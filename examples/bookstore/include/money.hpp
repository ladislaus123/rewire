#pragma once

#include <string>

namespace store {

class Money {
public:
    explicit Money(int cents = 0);
    int cents() const;
    Money plus(Money other) const;
    std::string format() const;

private:
    int cents_ = 0;
};

}  // namespace store
