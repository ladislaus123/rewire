package shop;

interface Payable {
    boolean charge(int amount);
}

class Payment implements Payable {
    public boolean charge(int amount) {
        if (amount <= 0) {
            return false;
        }
        return true;
    }
}

class Order {
    private final Book book;

    Order(Book book) {
        this.book = book;
    }
}

class OrderRepository {
    private Order[] orders;

    Order find(int id) {
        if (id < 0 || orders == null) {
            return null;
        }
        return orders[0];
    }
}

class OrderService {
    private OrderRepository orders;
    private Payment payment;

    OrderService(OrderRepository orders, Payment payment) {
        this.orders = orders;
        this.payment = payment;
    }

    boolean checkout(Book book) {
        Order order = orders.find(book.pages());
        if (order == null) {
            return false;
        }
        payment.charge(book.pages());
        return true;
    }
}

record Receipt(Order order) {}
