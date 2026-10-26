#ifndef PRESS_CATALOG_H
#define PRESS_CATALOG_H

struct Chapter {
    char title[32];
    int pages;
};

struct Author {
    char name[32];
};

struct Book {
    char title[32];
    struct Chapter chapters[4];
    struct Author* author;
    int chapter_count;
};

struct DigitalBook {
    struct Book book;
    char format[16];
};

struct Catalog {
    struct Book* featured;
    int count;
};

int book_pages(const struct Book* book);
struct Book* catalog_lookup(struct Catalog* catalog, const struct Author* author);

#endif
