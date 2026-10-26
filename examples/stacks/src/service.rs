pub struct Payment {
    pub amount: i32,
}

impl Payment {
    pub fn charge(&self, amount: i32) -> bool {
        if amount <= 0 {
            return false;
        }
        true
    }
}

pub struct Order {
    pub book: Book,
}

pub struct OrderRepository {
    pub orders: Vec<Order>,
}

impl OrderRepository {
    pub fn find(&self, id: i32) -> Option<Order> {
        if id < 0 {
            return None;
        }
        self.orders.first().copied()
    }
}

pub struct OrderService {
    pub orders: OrderRepository,
    pub payment: Payment,
}

impl OrderService {
    pub fn checkout(&self, book: &Book) -> bool {
        let found = self.orders.find(book.page_count());
        if found.is_none() {
            return false;
        }
        self.payment.charge(book.page_count());
        true
    }
}
