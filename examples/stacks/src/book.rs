pub struct Chapter {
    pub title: String,
    pub pages: i32,
}

impl Chapter {
    pub fn pages(&self) -> i32 {
        self.pages
    }
}

pub struct Author {
    pub name: String,
}

pub struct Book {
    pub author: Author,
    pub chapters: Vec<Chapter>,
}

impl Book {
    pub fn page_count(&self) -> i32 {
        let mut total = 0;
        for chapter in &self.chapters {
            total += chapter.pages();
        }
        total
    }
}

pub trait Listed {
    fn label(&self) -> String;
}

impl Listed for Book {
    fn label(&self) -> String {
        String::from("book")
    }
}

pub struct Review {
    pub subject: Weak<Book>,
}

pub struct SharedCatalog {
    pub featured: Rc<Book>,
}
