#include "catalog.hpp"
#include "order_service.hpp"

#include <iostream>
#include <memory>

int main() {
    store::Logger logger;
    store::Author author("Ada");
    store::Book book("Diagrams", &author);
    book.add_chapter(store::Chapter("Boxes", 12));
    book.add_chapter(store::Chapter("Arrows", 8));

    store::Catalog catalog;
    catalog.add(std::make_unique<store::Book>(book));
    auto matches = catalog.by_author(author);

    store::OrderRepository orders(logger);
    orders.save(store::Order(7, book, store::Money(2400)));
    store::CardPayment card("4242");
    auto promo = std::make_shared<store::Promotion>("UML");
    store::OrderService service(orders, card, logger, promo);
    if (!service.checkout(7) || matches.empty()) {
        std::cerr << "bookstore demo failed\n";
        return 1;
    }
    store::DigitalBook digital("Diagrams", &author, "https://example.test/diagrams");
    return digital.shipping_weight() == 0 ? 0 : 1;
}
