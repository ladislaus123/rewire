export interface Payable {
    charge(amount: number): boolean;
}

export class Payment implements Payable {
    charge(amount: number): boolean {
        if (amount <= 0) {
            return false;
        }
        return true;
    }
}

export class Order {
    constructor(public readonly book: Book) {}
}

export class OrderRepository {
    orders: Order[];

    find(id: number): Order {
        if (id < 0) {
            return this.orders[0];
        }
        return this.orders[0];
    }
}

export class OrderService {
    orders: OrderRepository;
    payment: Payment;

    constructor(orders: OrderRepository, payment: Payment) {
        this.orders = orders;
        this.payment = payment;
    }

    checkout(book: Book): boolean {
        const order = this.orders.find(book.pages());
        if (order == null) {
            return false;
        }
        this.payment.charge(book.pages());
        return true;
    }
}
