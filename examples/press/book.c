#include "catalog.h"

int book_pages(const struct Book* book) {
    int total = 0;
    int i;
    if (book == 0) {
        return 0;
    }
    for (i = 0; i < book->chapter_count && i < 4; ++i) {
        total += book->chapters[i].pages;
    }
    return total;
}
