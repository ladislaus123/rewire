#include "app.hpp"

#include "gui_window.hpp"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace rewire {
namespace {

void apply_palette(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette palette;
    const QColor window("#1b1d22");
    const QColor base("#23262e");
    const QColor text("#e6e6ea");
    const QColor button("#2c3344");
    const QColor highlight("#3d5a99");
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, QColor("#2a2e38"));
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, button);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::ToolTipBase, base);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::PlaceholderText, QColor("#8b93a7"));
    app.setPalette(palette);
}

}  // namespace

int run_gui(int argc, char** argv, const std::string& project, const std::string& session_query) {
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    QApplication::setApplicationName("Rewire");
    apply_palette(app);
    RewireWindow window(QString::fromStdString(project), QString::fromStdString(session_query));
    window.resize(1440, 920);
    window.show();
    return app.exec();
}

}  // namespace rewire
