#pragma once

#include "book.hpp"

#include <memory>
#include <vector>

namespace store {

class Catalog {
public:
    void add(std::unique_ptr<Book> book);
    std::vector<Book*> by_author(const Author& author) const;
    std::size_t size() const;

private:
    std::vector<std::unique_ptr<Book>> books_;
};

class Inventory {
public:
    explicit Inventory(std::unique_ptr<Catalog> catalog);
    Catalog* catalog() const;

private:
    std::unique_ptr<Catalog> catalog_;
};

}  // namespace store
