#include "catalog.hpp"

namespace store {

void Catalog::add(std::unique_ptr<Book> book) { books_.push_back(std::move(book)); }

std::vector<Book*> Catalog::by_author(const Author& author) const {
    std::vector<Book*> found;
    for (const std::unique_ptr<Book>& book : books_) {
        if (book && book->author() != nullptr && book->author()->name() == author.name()) {
            found.push_back(book.get());
        }
    }
    return found;
}

std::size_t Catalog::size() const { return books_.size(); }

Inventory::Inventory(std::unique_ptr<Catalog> catalog) : catalog_(std::move(catalog)) {}

Catalog* Inventory::catalog() const { return catalog_.get(); }

}  // namespace store
