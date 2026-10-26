#include "catalog.h"

struct Book* catalog_lookup(struct Catalog* catalog, const struct Author* author) {
    if (catalog == 0 || catalog->featured == 0) {
        return 0;
    }
    int pages = book_pages(catalog->featured);
    if (pages > 0 && catalog->featured->author == author) {
        return catalog->featured;
    }
    return 0;
}
