#include "claude_bridge.hpp"
#include "diagram.hpp"
#include "edit.hpp"
#include "json.hpp"
#include "model.hpp"
#include "parser.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* text, int line) {
    if (!condition) {
        std::cerr << "FAIL " << line << ": " << text << "\n";
        ++failures;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

bool has_rel(const rewire::ProjectModel& model, const std::string& from, rewire::RelationKind kind, const std::string& to) {
    for (const auto& relation : model.relations) {
        if (relation.from == from && relation.kind == kind && relation.to == to) return true;
    }
    return false;
}

std::string flow_text(const rewire::ProjectModel& model, const std::string& name) {
    std::string out;
    for (const auto& flow : model.flows) {
        if (flow.qualified_name.find(name) == std::string::npos) continue;
        for (const auto& step : flow.steps) out += step.kind + " " + step.text + "\n";
    }
    return out;
}

std::string join(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += line;
        out.push_back('\n');
    }
    return out;
}

std::string language_dump;

bool calls_across(const rewire::ProjectModel& model, const std::string& caller, const std::string& callee_class,
                  const std::string& leaf) {
    for (const auto& call : model.calls) {
        if (call.caller_class != caller || call.callee_class != callee_class) continue;
        if (call.callee.size() < leaf.size()) continue;
        const auto at = call.callee.size() - leaf.size();
        if (call.callee.compare(at, leaf.size(), leaf) != 0) continue;
        if (at > 0 && call.callee[at - 1] != '.' && call.callee[at - 1] != ':') continue;
        if (!call.callee_file.empty() && call.caller_file != call.callee_file) return true;
    }
    return false;
}

void keep_dump(int before, const rewire::ProjectModel& model) {
    if (failures > before) language_dump += "\n" + rewire::format_model(model);
}

void expect_language(const rewire::ProjectModel& model, const std::string& class_name, const std::string& language,
                     const std::string& snippet) {
    rewire::EditRequest edit;
    edit.action = rewire::EditAction::AddField;
    edit.class_name = class_name;
    edit.primary = "extra";
    edit.secondary = "int";
    edit.relationship = "association";
    auto prepared = rewire::prepare_edit(model, edit);
    CHECK(prepared.error.empty());
    CHECK(prepared.prompt.find("Language: " + language) != std::string::npos);
    CHECK(prepared.prompt.find(snippet) != std::string::npos);
}

}  // namespace

int main() {
    const std::string root = std::string(REWIRE_SOURCE_DIR) + "/examples/bookstore";
    rewire::ProjectModel model = rewire::parse_project(root);
    if (failures == 0 && model.classes.empty()) {
        std::cerr << rewire::format_model(model);
    }
    CHECK(!model.classes.empty());
    CHECK(model.find_class("store::Book") != nullptr);
    CHECK(has_rel(model, "store::Book", rewire::RelationKind::Composition, "store::Chapter"));
    CHECK(has_rel(model, "store::Book", rewire::RelationKind::Association, "store::Author"));
    CHECK(has_rel(model, "store::DigitalBook", rewire::RelationKind::Inheritance, "store::Book"));
    CHECK(has_rel(model, "store::CardPayment", rewire::RelationKind::Inheritance, "store::PaymentProcessor"));
    CHECK(has_rel(model, "store::Order", rewire::RelationKind::Composition, "store::Book"));
    CHECK(has_rel(model, "store::OrderRepository", rewire::RelationKind::Association, "store::Logger"));
    CHECK(has_rel(model, "store::OrderRepository", rewire::RelationKind::Composition, "store::Order"));
    CHECK(has_rel(model, "store::OrderService", rewire::RelationKind::Aggregation, "store::Promotion"));
    CHECK(has_rel(model, "store::OrderService", rewire::RelationKind::Association, "store::OrderRepository"));
    CHECK(has_rel(model, "store::OrderService", rewire::RelationKind::Association, "store::PaymentProcessor"));
    CHECK(has_rel(model, "store::Catalog", rewire::RelationKind::Composition, "store::Book"));
    CHECK(has_rel(model, "store::Inventory", rewire::RelationKind::Composition, "store::Catalog"));
    CHECK(has_rel(model, "store::Catalog", rewire::RelationKind::Dependency, "store::Author"));
    CHECK(has_rel(model, "store::OrderService", rewire::RelationKind::Dependency, "store::Order"));

    std::string checkout = flow_text(model, "checkout");
    CHECK(checkout.find("find") != std::string::npos);
    CHECK(checkout.find("charge") != std::string::npos);
    CHECK(checkout.find("if") != std::string::npos);
    CHECK(checkout.find("call total\n") == std::string::npos);
    std::string by_author = flow_text(model, "by_author");
    CHECK(by_author.find("loop") != std::string::npos);

    auto diagram = rewire::render_class_diagram(model, "store::Book", false, 100);
    std::string picture = join(diagram);
    CHECK(picture.find("Book") != std::string::npos);
    CHECK(picture.find("composition") != std::string::npos);
    CHECK(picture.find("Chapter") != std::string::npos);
    CHECK(picture.find("association") != std::string::npos);
    bool author_box_intact = false;
    for (const auto& line : diagram) {
        auto pos = line.find("Author");
        if (pos != std::string::npos && pos > 0 && line[pos - 1] == '|') author_box_intact = true;
    }
    CHECK(author_box_intact);

    rewire::EditRequest edit;
    edit.action = rewire::EditAction::AddMethod;
    edit.class_name = "store::OrderService";
    edit.primary = "bool cancel(int order_id)";
    edit.access = "public";
    auto prepared = rewire::prepare_edit(model, edit);
    CHECK(prepared.error.empty());
    CHECK(prepared.prompt.find("add_method") != std::string::npos);
    CHECK(prepared.prompt.find("store::OrderService") != std::string::npos);
    CHECK(prepared.prompt.find(model.root) != std::string::npos);
    CHECK(prepared.prompt.find("Language: C++") != std::string::npos);
    CHECK(prepared.prompt.find("C++ project") != std::string::npos);

    const std::string source_root = std::string(REWIRE_SOURCE_DIR);
    {
        int before = failures;
        rewire::ProjectModel shop = rewire::parse_project(source_root + "/examples/shop");
        CHECK(shop.find_class("shop.Book") != nullptr);
        CHECK(has_rel(shop, "shop.Book", rewire::RelationKind::Aggregation, "shop.Chapter"));
        CHECK(has_rel(shop, "shop.Book", rewire::RelationKind::Association, "shop.Author"));
        CHECK(has_rel(shop, "shop.DigitalBook", rewire::RelationKind::Inheritance, "shop.Book"));
        CHECK(has_rel(shop, "shop.DigitalBook", rewire::RelationKind::Dependency, "shop.Author"));
        CHECK(has_rel(shop, "shop.Order", rewire::RelationKind::Composition, "shop.Book"));
        CHECK(has_rel(shop, "shop.OrderRepository", rewire::RelationKind::Aggregation, "shop.Order"));
        CHECK(has_rel(shop, "shop.OrderService", rewire::RelationKind::Association, "shop.OrderRepository"));
        CHECK(has_rel(shop, "shop.OrderService", rewire::RelationKind::Association, "shop.Payment"));
        CHECK(has_rel(shop, "shop.OrderService", rewire::RelationKind::Dependency, "shop.Book"));
        CHECK(has_rel(shop, "shop.Payment", rewire::RelationKind::Inheritance, "shop.Payable"));
        CHECK(has_rel(shop, "shop.Receipt", rewire::RelationKind::Composition, "shop.Order"));
        std::string checkout = flow_text(shop, "checkout");
        CHECK(checkout.find("find") != std::string::npos);
        CHECK(checkout.find("charge") != std::string::npos);
        CHECK(checkout.find("if") != std::string::npos);
        CHECK(checkout.find("pages") != std::string::npos);
        CHECK(calls_across(shop, "shop.OrderService", "shop.Book", "pages"));
        expect_language(shop, "shop.OrderService", "Java", "non-final");
        keep_dump(before, shop);
    }
    {
        int before = failures;
        rewire::ProjectModel shelf = rewire::parse_project(source_root + "/examples/shelf");
        CHECK(shelf.find_class("book.Book") != nullptr);
        CHECK(has_rel(shelf, "book.Book", rewire::RelationKind::Aggregation, "book.Chapter"));
        CHECK(has_rel(shelf, "book.Book", rewire::RelationKind::Association, "book.Author"));
        CHECK(has_rel(shelf, "book.DigitalBook", rewire::RelationKind::Inheritance, "book.Book"));
        CHECK(has_rel(shelf, "book.DigitalBook", rewire::RelationKind::Dependency, "book.Author"));
        CHECK(has_rel(shelf, "book.Shelf", rewire::RelationKind::Composition, "book.Book"));
        CHECK(has_rel(shelf, "service.Order", rewire::RelationKind::Association, "book.Book"));
        CHECK(has_rel(shelf, "service.OrderRepository", rewire::RelationKind::Aggregation, "service.Order"));
        CHECK(has_rel(shelf, "service.OrderService", rewire::RelationKind::Association, "service.OrderRepository"));
        CHECK(has_rel(shelf, "service.OrderService", rewire::RelationKind::Association, "service.Payment"));
        CHECK(has_rel(shelf, "service.OrderService", rewire::RelationKind::Dependency, "book.Book"));
        std::string checkout = flow_text(shelf, "checkout");
        CHECK(checkout.find("find") != std::string::npos);
        CHECK(checkout.find("charge") != std::string::npos);
        CHECK(checkout.find("if") != std::string::npos);
        CHECK(checkout.find("page_count") != std::string::npos);
        CHECK(calls_across(shelf, "service.OrderService", "book.Book", "page_count"));
        expect_language(shelf, "service.OrderService", "Python", "parameter");
        keep_dump(before, shelf);
    }
    {
        int before = failures;
        rewire::ProjectModel gallery = rewire::parse_project(source_root + "/examples/gallery");
        CHECK(gallery.find_class("book.Book") != nullptr);
        CHECK(has_rel(gallery, "book.Book", rewire::RelationKind::Aggregation, "book.Chapter"));
        CHECK(has_rel(gallery, "book.Book", rewire::RelationKind::Association, "book.Author"));
        CHECK(has_rel(gallery, "book.DigitalBook", rewire::RelationKind::Inheritance, "book.Book"));
        CHECK(has_rel(gallery, "book.DigitalBook", rewire::RelationKind::Dependency, "book.Author"));
        CHECK(has_rel(gallery, "book.Shelf", rewire::RelationKind::Composition, "book.Book"));
        CHECK(has_rel(gallery, "service.Order", rewire::RelationKind::Composition, "book.Book"));
        CHECK(has_rel(gallery, "service.OrderRepository", rewire::RelationKind::Aggregation, "service.Order"));
        CHECK(has_rel(gallery, "service.OrderService", rewire::RelationKind::Association, "service.OrderRepository"));
        CHECK(has_rel(gallery, "service.OrderService", rewire::RelationKind::Association, "service.Payment"));
        CHECK(has_rel(gallery, "service.OrderService", rewire::RelationKind::Dependency, "book.Book"));
        CHECK(has_rel(gallery, "service.Payment", rewire::RelationKind::Inheritance, "service.Payable"));
        std::string checkout = flow_text(gallery, "checkout");
        CHECK(checkout.find("find") != std::string::npos);
        CHECK(checkout.find("charge") != std::string::npos);
        CHECK(checkout.find("if") != std::string::npos);
        CHECK(checkout.find("pages") != std::string::npos);
        CHECK(calls_across(gallery, "service.OrderService", "book.Book", "pages"));
        expect_language(gallery, "service.OrderService", "TypeScript", "readonly");
        keep_dump(before, gallery);
    }
    {
        int before = failures;
        rewire::ProjectModel stacks = rewire::parse_project(source_root + "/examples/stacks");
        CHECK(stacks.find_class("book::Book") != nullptr);
        CHECK(has_rel(stacks, "book::Book", rewire::RelationKind::Composition, "book::Author"));
        CHECK(has_rel(stacks, "book::Book", rewire::RelationKind::Composition, "book::Chapter"));
        CHECK(has_rel(stacks, "book::Review", rewire::RelationKind::Association, "book::Book"));
        CHECK(has_rel(stacks, "book::SharedCatalog", rewire::RelationKind::Aggregation, "book::Book"));
        CHECK(has_rel(stacks, "book::Book", rewire::RelationKind::Inheritance, "book::Listed"));
        CHECK(has_rel(stacks, "service::Order", rewire::RelationKind::Composition, "book::Book"));
        CHECK(has_rel(stacks, "service::OrderRepository", rewire::RelationKind::Composition, "service::Order"));
        CHECK(has_rel(stacks, "service::OrderService", rewire::RelationKind::Composition, "service::OrderRepository"));
        CHECK(has_rel(stacks, "service::OrderService", rewire::RelationKind::Composition, "service::Payment"));
        CHECK(has_rel(stacks, "service::OrderService", rewire::RelationKind::Dependency, "book::Book"));
        std::string checkout = flow_text(stacks, "checkout");
        CHECK(checkout.find("find") != std::string::npos);
        CHECK(checkout.find("charge") != std::string::npos);
        CHECK(checkout.find("if") != std::string::npos);
        CHECK(checkout.find("page_count") != std::string::npos);
        CHECK(calls_across(stacks, "service::OrderService", "book::Book", "page_count"));
        expect_language(stacks, "service::OrderService", "Rust", "Weak");
        keep_dump(before, stacks);
    }
    {
        int before = failures;
        rewire::ProjectModel press = rewire::parse_project(source_root + "/examples/press");
        CHECK(press.find_class("Book") != nullptr);
        CHECK(has_rel(press, "Book", rewire::RelationKind::Composition, "Chapter"));
        CHECK(has_rel(press, "Book", rewire::RelationKind::Association, "Author"));
        CHECK(has_rel(press, "DigitalBook", rewire::RelationKind::Composition, "Book"));
        CHECK(has_rel(press, "Catalog", rewire::RelationKind::Association, "Book"));
        CHECK(has_rel(press, "Catalog", rewire::RelationKind::Dependency, "Author"));
        for (const auto& relation : press.relations) {
            CHECK(relation.kind != rewire::RelationKind::Inheritance);
        }
        std::string pages = flow_text(press, "book_pages");
        CHECK(pages.find("for") != std::string::npos);
        CHECK(pages.find("if") != std::string::npos);
        std::string lookup = flow_text(press, "catalog_lookup");
        CHECK(lookup.find("book_pages") != std::string::npos);
        CHECK(lookup.find("if") != std::string::npos);
        CHECK(calls_across(press, "Catalog", "Book", "book_pages"));
        expect_language(press, "Book", "C", "pointer");
        keep_dump(before, press);
    }

    rewire::json::Value parsed = rewire::json::parse(R"({"type":"user","message":{"role":"user","content":"hello"},"n":2})");
    CHECK(parsed.find("type") && parsed.find("type")->as_string() == "user");
    CHECK(parsed.find("message") && parsed.find("message")->find("content")->as_string() == "hello");

    std::string socket_path = "/tmp/rewire-selftest-" + std::to_string(getpid()) + ".sock";
    ::unlink(socket_path.c_str());
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(server >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    CHECK(bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    CHECK(listen(server, 1) == 0);
    std::string received;
    std::thread reader([&] {
        int conn = accept(server, nullptr, nullptr);
        if (conn < 0) return;
        char buffer[8192];
        while (true) {
            ssize_t n = recv(conn, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            received.append(buffer, static_cast<std::size_t>(n));
        }
        close(conn);
    });
    rewire::OutboundMessage outbound;
    outbound.socket_path = socket_path;
    outbound.session_id = "11111111-2222-4333-8444-555555555555";
    outbound.text = "Add a cancel method to OrderService.";
    outbound.from_address = "uds:/tmp/cc-socks/rewire-test.sock";
    outbound.from_name = "rewire";
    outbound.token = "abc123";
    auto sent = rewire::send_message(outbound);
    reader.join();
    close(server);
    ::unlink(socket_path.c_str());
    CHECK(sent.ok);
    CHECK(received.find("\"type\":\"auth\"") != std::string::npos);
    CHECK(received.find("\"type\":\"user\"") != std::string::npos);
    CHECK(received.find("Add a cancel method") != std::string::npos);
    CHECK(received.find("cross-session-message") != std::string::npos);
    CHECK(received.find(outbound.session_id) != std::string::npos);
    CHECK(received.find("\"msgV\":1") != std::string::npos);

    auto sessions = rewire::list_sessions();
    (void)sessions;

    if (failures) {
        std::cerr << "\n" << rewire::format_model(model) << "\n";
        std::cerr << language_dump << "\n";
        std::cerr << failures << " failed\n";
        return 1;
    }
    std::cout << "ok classes=" << model.classes.size() << " relations=" << model.relations.size()
              << " flows=" << model.flows.size() << " calls=" << model.calls.size() << "\n";
    return 0;
}
