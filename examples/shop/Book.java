package shop;

import java.util.List;

class Chapter {
    private final String title;
    private final int count;

    Chapter(String title, int count) {
        this.title = title;
        this.count = count;
    }

    int pages() {
        return count;
    }
}

class Author {
    private final String name;

    Author(String name) {
        this.name = name;
    }
}

class Book {
    private final List<Chapter> chapters;
    private Author author;

    Book(Author author, List<Chapter> chapters) {
        this.author = author;
        this.chapters = chapters;
    }

    int pages() {
        int total = 0;
        for (Chapter chapter : chapters) {
            total += chapter.pages();
        }
        return total;
    }
}

class DigitalBook extends Book {
    private final String format;

    DigitalBook(Author author, List<Chapter> chapters, String format) {
        super(author, chapters);
        this.format = format;
    }
}
