#pragma once

#include "book.hpp"
#include "money.hpp"

namespace store {

class Order {
public:
    Order(int id, Book book, Money total);
    int id() const;
    const Book& book() const;
    Money total() const;

private:
    int id_ = 0;
    Book book_;
    Money total_;
};

}  // namespace store
