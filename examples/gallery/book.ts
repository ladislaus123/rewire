export class Chapter {
    readonly title: string;
    readonly count: number;

    constructor(title: string, count: number) {
        this.title = title;
        this.count = count;
    }

    pages(): number {
        return this.count;
    }
}

export class Author {
    readonly name: string;

    constructor(name: string) {
        this.name = name;
    }
}

export class Book {
    chapters: Chapter[];
    author: Author;

    constructor(author: Author, chapters: Chapter[]) {
        this.author = author;
        this.chapters = chapters;
    }

    pages(): number {
        let total = 0;
        for (const chapter of this.chapters) {
            total += chapter.pages();
        }
        return total;
    }
}

export class DigitalBook extends Book {
    readonly format: string;

    constructor(author: Author, chapters: Chapter[]) {
        super(author, chapters);
        this.format = "pdf";
    }
}

export class Shelf {
    readonly featured: Book;

    constructor(author: Author) {
        this.featured = new Book(author, []);
    }
}
