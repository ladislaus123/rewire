#include "book.hpp"

namespace store {

Chapter::Chapter(std::string title, int pages) : title_(std::move(title)), pages_(pages) {}

const std::string& Chapter::title() const { return title_; }

int Chapter::pages() const { return pages_; }

Book::Book(std::string title, Author* author) : title_(std::move(title)), author_(author) {}

void Book::add_chapter(Chapter chapter) { chapters_.push_back(std::move(chapter)); }

int Book::pages() const {
    int sum = 0;
    for (const Chapter& chapter : chapters_) {
        sum += chapter.pages();
    }
    return sum;
}

Author* Book::author() const { return author_; }

const std::string& Book::title() const { return title_; }

int Book::shipping_weight() const { return pages() > 0 ? pages() / 20 + 1 : 1; }

DigitalBook::DigitalBook(std::string title, Author* author, std::string url)
    : Book(std::move(title), author), url_(std::move(url)) {}

int DigitalBook::shipping_weight() const {
    (void)url_;
    return 0;
}

}  // namespace store
