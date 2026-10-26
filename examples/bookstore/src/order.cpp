#include "order.hpp"

namespace store {

Order::Order(int id, Book book, Money total) : id_(id), book_(std::move(book)), total_(total) {}

int Order::id() const { return id_; }

const Book& Order::book() const { return book_; }

Money Order::total() const { return total_; }

}  // namespace store
