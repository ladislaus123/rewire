#pragma once

#include "author.hpp"

#include <string>
#include <vector>

namespace store {

class Chapter {
public:
    Chapter(std::string title, int pages);
    const std::string& title() const;
    int pages() const;

private:
    std::string title_;
    int pages_ = 0;
};

class Book {
public:
    Book(std::string title, Author* author);
    virtual ~Book() = default;

    void add_chapter(Chapter chapter);
    int pages() const;
    Author* author() const;
    const std::string& title() const;
    virtual int shipping_weight() const;

private:
    std::string title_;
    Author* author_ = nullptr;
    std::vector<Chapter> chapters_;
};

class DigitalBook final : public Book {
public:
    DigitalBook(std::string title, Author* author, std::string url);
    int shipping_weight() const override;

private:
    std::string url_;
};

}  // namespace store
