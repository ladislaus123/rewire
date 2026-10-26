#include "gui_window.hpp"

#include <QApplication>
#include <QMouseEvent>
#include <QTest>
#include <QWidget>

#include <cmath>
#include <iostream>
#include <string>

namespace {

int fails = 0;

void check(bool condition, const char* text, int line) {
    if (condition) return;
    std::cerr << "FAIL " << line << ": " << text << "\n";
    ++fails;
}

#define CHECK(cond) check((cond), #cond, __LINE__)

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc, argv);
    const QString project = QString(REWIRE_SOURCE_DIR) + "/examples/bookstore";
    rewire::RewireWindow window(project, QString());
    window.resize(1440, 920);
    window.show();
    QApplication::setActiveWindow(&window);
    CHECK(window.waitUntilParsed(30000));
    CHECK(window.focusClass("store::Book"));
    CHECK(window.nodeCount() >= 4);
    CHECK(window.edgeCount() >= 2);
    CHECK(window.focusedClass() == "store::Book");
    const QPoint book_point = window.viewPointForClass("store::Book");
    QWidget* viewport = window.diagramViewport();
    CHECK(viewport->rect().contains(book_point));
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, book_point);
    QApplication::processEvents();
    CHECK(window.selectedClass() == "store::Book");
    const QString inspector = window.inspectorText();
    CHECK(inspector.contains("composition"));
    CHECK(inspector.contains("Chapter"));
    CHECK(inspector.contains("Author"));

    const QPoint edge_point = window.viewPointForEdge("store::Book", "store::Chapter");
    CHECK(viewport->rect().contains(edge_point));
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, edge_point);
    QApplication::processEvents();
    CHECK(window.inspectorText().contains("composition"));

    CHECK(window.focusClass("store::Book"));
    CHECK(window.showFlowContaining("store::OrderService::checkout"));
    const QString flow = window.flowText();
    CHECK(flow.contains("charge"));
    CHECK(flow.contains("find"));
    CHECK(!flow.contains("call total"));

    const QPoint author_point = window.viewPointForClass("store::Author");
    CHECK(viewport->rect().contains(author_point));
    QTest::mouseDClick(viewport, Qt::LeftButton, Qt::NoModifier, author_point);
    QApplication::processEvents();
    CHECK(window.focusedClass() == "store::Author");

    CHECK(window.focusClass("store::Book"));
    window.grab().save("/tmp/rewire-gui.png");
    const QPoint before = window.viewPointForClass("store::Book");
    const QPoint dropped = before + QPoint(70, 40);
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, before);
    for (int step = 1; step <= 7; ++step) {
        const QPoint pos = before + QPoint(70 * step / 7, 40 * step / 7);
        QMouseEvent move(QEvent::MouseMove, QPointF(pos), viewport->mapToGlobal(pos), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &move);
    }
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, dropped);
    QApplication::processEvents();
    const QPoint after = window.viewPointForClass("store::Book");
    CHECK(std::abs(after.x() - before.x()) + std::abs(after.y() - before.y()) > 15);

    if (fails != 0) {
        std::cerr << fails << " gui checks failed\n";
        return 1;
    }
    std::cerr << "gui ok nodes=" << window.nodeCount() << " edges=" << window.edgeCount() << "\n";
    return 0;
}
