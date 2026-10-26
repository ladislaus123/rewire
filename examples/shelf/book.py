class Chapter:
    def __init__(self, title, pages):
        self.title = title
        self.pages = pages

    def pages(self):
        return self.pages


class Author:
    def __init__(self, name):
        self.name = name


class Book:
    def __init__(self, author: Author, chapters: list[Chapter]):
        self.author = author
        self.chapters: list[Chapter] = chapters

    def page_count(self):
        total = 0
        for chapter in self.chapters:
            total += chapter.pages()
        return total


class DigitalBook(Book):
    def __init__(self, author: Author, chapters: list[Chapter]):
        self.format = "pdf"


class Shelf:
    def __init__(self, author: Author):
        self.featured = Book(author, [])
        self.spare = author
