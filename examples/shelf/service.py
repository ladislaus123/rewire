class Payment:
    def charge(self, amount: int) -> bool:
        if amount <= 0:
            return False
        return True


class Order:
    def __init__(self, book: Book):
        self.book = book


class OrderRepository:
    def __init__(self):
        self.orders: list[Order] = []

    def find(self, order_id: int):
        if order_id < 0:
            return None
        return self.orders[0]


class OrderService:
    def __init__(self, orders: OrderRepository, payment: Payment):
        self.orders = orders
        self.payment = payment

    def checkout(self, book: Book) -> bool:
        found = self.orders.find(book.page_count())
        if found is None:
            return False
        self.payment.charge(book.page_count())
        return True
