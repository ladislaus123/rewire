#include "gui_window.hpp"

#include "edit.hpp"
#include "json.hpp"
#include "parser.hpp"
#include "util.hpp"

#include <QCloseEvent>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QStyleOptionGraphicsItem>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>

namespace rewire {
namespace {

constexpr int kClassType = QGraphicsItem::UserType + 1;
constexpr int kEdgeType = QGraphicsItem::UserType + 2;
constexpr int kRoleKind = Qt::UserRole;
constexpr int kRoleId = Qt::UserRole + 1;

QString from_std(const std::string& text) { return QString::fromStdString(text); }

QColor relation_color(RelationKind kind) {
    switch (kind) {
        case RelationKind::Inheritance: return QColor("#8fbf6a");
        case RelationKind::Composition: return QColor("#e0af68");
        case RelationKind::Aggregation: return QColor("#7dcfff");
        case RelationKind::Association: return QColor("#7aa2f7");
        case RelationKind::Dependency: return QColor("#bb9af7");
    }
    return QColor("#bb9af7");
}

QFont code_font(int point_size, bool bold = false) {
    QFont font("Menlo");
    font.setPointSize(point_size);
    font.setBold(bold);
    if (!QFontInfo(font).exactMatch()) {
        font.setFamily("Menlo");
    }
    return font;
}

QString elide(const QFont& font, const QString& text, int width) {
    return QFontMetrics(font).elidedText(text, Qt::ElideRight, width);
}

bool paths_overlap(const QString& project, const QString& cwd) {
    const std::string left = project.toStdString();
    const std::string right = cwd.toStdString();
    if (left.empty() || right.empty()) return false;
    if (left == right) return true;
    return left.rfind(right + "/", 0) == 0 || right.rfind(left + "/", 0) == 0;
}

QString load_session_id(const QString& project) {
    json::Value value = json::parse(read_text(project.toStdString() + "/.rewire/config.json"));
    if (const json::Value* id = value.find("session_id")) return from_std(id->as_string());
    return {};
}

void save_session_id(const QString& project, const QString& session_id) {
    std::filesystem::create_directories((project + "/.rewire").toStdString());
    json::Value value;
    value.type = json::Value::Type::Object;
    json::Value id;
    id.type = json::Value::Type::String;
    id.text = session_id.toStdString();
    value.object.emplace_back("session_id", std::move(id));
    write_text(project.toStdString() + "/.rewire/config.json", json::dump(value) + "\n");
}

const ClaudeSession* find_query(const std::vector<ClaudeSession>& sessions, const QString& query) {
    const std::string needle = query.toStdString();
    const ClaudeSession* partial = nullptr;
    for (const auto& session : sessions) {
        if (session.session_id == needle || session.name == needle || std::to_string(session.pid) == needle) {
            return &session;
        }
        if (session.session_id.rfind(needle, 0) == 0 || session.name.find(needle) != std::string::npos) {
            partial = &session;
        }
    }
    return partial;
}

}  // namespace

class RelationEdge;

class ClassNode : public QGraphicsItem {
public:
    ClassNode(QString id, QString title, QString file_line, bool focus, std::vector<QString> fields,
              std::vector<QColor> field_colors, std::vector<QString> methods)
        : id_(std::move(id)),
          title_(std::move(title)),
          file_line_(std::move(file_line)),
          focus_(focus),
          fields_(std::move(fields)),
          field_colors_(std::move(field_colors)),
          methods_(std::move(methods)) {
        setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
        setAcceptHoverEvents(true);
        setZValue(10);
        const int field_n = std::min<int>(static_cast<int>(fields_.size()), 5);
        const int method_n = std::min<int>(static_cast<int>(methods_.size()), 4);
        more_fields_ = static_cast<int>(fields_.size()) > 5;
        more_methods_ = static_cast<int>(methods_.size()) > 4;
        height_ = 10 + 24 + 16 + 10 + field_n * 18 + (more_fields_ ? 16 : 0) + 8 + method_n * 18 +
                  (more_methods_ ? 16 : 0) + 12;
    }

    ~ClassNode() override;

    int type() const override { return kClassType; }
    QRectF boundingRect() const override { return QRectF(0, 0, width_, height_); }
    QString id() const { return id_; }
    qreal nodeWidth() const { return width_; }
    qreal nodeHeight() const { return height_; }
    void addEdge(RelationEdge* edge) { edges_.push_back(edge); }
    void removeEdge(RelationEdge* edge) {
        edges_.erase(std::remove(edges_.begin(), edges_.end(), edge), edges_.end());
    }

    std::function<void(QPointF)> onMoved;
    std::function<void()> onActivate;

    void setDimmed(bool dimmed) { setOpacity(dimmed ? 0.35 : 1.0); }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRectF card(0, 0, width_, height_);
        QPainterPath path;
        path.addRoundedRect(card, 10, 10);
        painter->setClipPath(path);
        painter->fillRect(card, hovered_ ? QColor("#2a303c") : QColor("#22262e"));
        painter->fillRect(QRectF(0, 0, width_, 44), focus_ ? QColor("#34425f") : QColor("#2a3142"));
        painter->setClipping(false);
        QColor border = isSelected() ? QColor("#8aaceb") : focus_ ? QColor("#5d7199") : QColor("#3c4250");
        painter->setPen(QPen(border, isSelected() ? 2.2 : 1.2));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(path);

        const QFont title_font = code_font(12, true);
        const QFont body = code_font(10);
        painter->setFont(title_font);
        painter->setPen(QColor("#f4f6fb"));
        painter->drawText(QRectF(12, 6, width_ - 24, 20), Qt::AlignLeft | Qt::AlignVCenter,
                          elide(title_font, title_, static_cast<int>(width_ - 24)));
        painter->setFont(body);
        painter->setPen(QColor("#9aa3b5"));
        painter->drawText(QRectF(12, 24, width_ - 24, 16), Qt::AlignLeft | Qt::AlignVCenter,
                          elide(body, file_line_, static_cast<int>(width_ - 24)));

        qreal y = 52;
        const int field_n = std::min<int>(static_cast<int>(fields_.size()), 5);
        for (int i = 0; i < field_n; ++i) {
            painter->setPen(field_colors_[static_cast<std::size_t>(i)]);
            painter->drawText(QRectF(12, y, width_ - 24, 18), Qt::AlignLeft | Qt::AlignVCenter,
                              elide(body, fields_[static_cast<std::size_t>(i)], static_cast<int>(width_ - 24)));
            y += 18;
        }
        if (more_fields_) {
            painter->setPen(QColor("#8b93a7"));
            painter->drawText(QRectF(12, y, width_ - 24, 16), Qt::AlignLeft | Qt::AlignVCenter, "…");
            y += 16;
        }
        y += 4;
        painter->setPen(QColor("#3c4250"));
        painter->drawLine(QPointF(10, y), QPointF(width_ - 10, y));
        y += 6;
        const int method_n = std::min<int>(static_cast<int>(methods_.size()), 4);
        painter->setPen(QColor("#c5cdf0"));
        for (int i = 0; i < method_n; ++i) {
            painter->drawText(QRectF(12, y, width_ - 24, 18), Qt::AlignLeft | Qt::AlignVCenter,
                              elide(body, methods_[static_cast<std::size_t>(i)], static_cast<int>(width_ - 24)));
            y += 18;
        }
        if (more_methods_) {
            painter->setPen(QColor("#8b93a7"));
            painter->drawText(QRectF(12, y, width_ - 24, 16), Qt::AlignLeft | Qt::AlignVCenter, "…");
        }
    }

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;

private:
    friend class RelationEdge;
    QString id_;
    QString title_;
    QString file_line_;
    bool focus_ = false;
    bool hovered_ = false;
    bool alive_ = true;
    bool more_fields_ = false;
    bool more_methods_ = false;
    qreal width_ = 280;
    qreal height_ = 80;
    std::vector<QString> fields_;
    std::vector<QColor> field_colors_;
    std::vector<QString> methods_;
    std::vector<RelationEdge*> edges_;
};

class RelationEdge : public QGraphicsPathItem {
public:
    RelationEdge(ClassNode* from, ClassNode* to, RelationKind kind, QString label, int lane)
        : from_(from),
          to_(to),
          kind_(kind),
          label_(std::move(label)),
          lane_(lane),
          from_id_(from->id()),
          to_id_(to->id()) {
        setZValue(1);
        setFlag(ItemIsSelectable, true);
        from_->addEdge(this);
        to_->addEdge(this);
        refresh();
    }

    ~RelationEdge() override {
        if (from_) from_->removeEdge(this);
        if (to_) to_->removeEdge(this);
    }

    int type() const override { return kEdgeType; }
    QString fromId() const { return from_id_; }
    QString toId() const { return to_id_; }
    RelationKind kind() const { return kind_; }
    QString label() const { return label_; }

    void detach(ClassNode* node) {
        if (from_ == node) from_ = nullptr;
        if (to_ == node) to_ = nullptr;
    }

    void setHot(bool hot) {
        hot_ = hot;
        setZValue(hot ? 5 : 1);
        update();
    }

    void refresh() {
        if (!from_ || !to_ || !from_->scene() || !to_->scene()) return;
        prepareGeometryChange();
        const QRectF from_rect = from_->sceneBoundingRect();
        const QRectF to_rect = to_->sceneBoundingRect();
        const QPointF a = side_point(from_rect, to_rect.center());
        const QPointF b = side_point(to_rect, from_rect.center());
        QPainterPath path;
        path.moveTo(a);
        const qreal offset = lane_ * 16.0;
        if (std::abs(a.x() - b.x()) < 8 && std::abs(a.y() - b.y()) < 8) {
            path.lineTo(b);
        } else if (std::abs(b.y() - a.y()) >= std::abs(b.x() - a.x())) {
            const qreal mid = (a.y() + b.y()) / 2.0 + offset;
            path.lineTo(a.x(), mid);
            path.lineTo(b.x(), mid);
            path.lineTo(b);
        } else {
            const qreal mid = (a.x() + b.x()) / 2.0 + offset;
            path.lineTo(mid, a.y());
            path.lineTo(mid, b.y());
            path.lineTo(b);
        }
        setPath(path);
        update();
    }

    QRectF boundingRect() const override { return QGraphicsPathItem::boundingRect().adjusted(-14, -14, 14, 14); }

    QPainterPath shape() const override {
        QPainterPathStroker stroker;
        stroker.setWidth(14);
        return stroker.createStroke(path());
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        const QColor color = relation_color(kind_);
        QPen pen(color, (hot_ || isSelected()) ? 2.6 : 1.6);
        pen.setCosmetic(true);
        if (kind_ == RelationKind::Dependency) pen.setStyle(Qt::DashLine);
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(path());
        if (path().elementCount() >= 2) {
            const auto last = path().elementAt(path().elementCount() - 1);
            const auto prev = path().elementAt(path().elementCount() - 2);
            const QLineF segment(QPointF(prev.x, prev.y), QPointF(last.x, last.y));
            if (segment.length() > 1) {
                const QPointF tip = segment.p2();
                const QPointF left = tip + QLineF::fromPolar(11, segment.angle() + 152).p2();
                const QPointF right = tip + QLineF::fromPolar(11, segment.angle() - 152).p2();
                painter->setPen(Qt::NoPen);
                painter->setBrush(color);
                painter->drawPolygon(QPolygonF() << tip << left << right);
            }
        }
    }

    // Drawn by the scene foreground so class boxes cannot cover the words.
    void paintLabel(QPainter* painter) const {
        if (path().elementCount() < 2) return;
        qreal best = -1;
        QPointF anchor;
        for (int i = 1; i < path().elementCount(); ++i) {
            const QPointF a(path().elementAt(i - 1).x, path().elementAt(i - 1).y);
            const QPointF b(path().elementAt(i).x, path().elementAt(i).y);
            const qreal length = QLineF(a, b).length();
            if (length > best) {
                best = length;
                anchor = (a + b) / 2.0;
            }
        }
        const QString text = label_.isEmpty() ? QString(relation_name(kind_))
                                              : QString(relation_name(kind_)) + " " + label_;
        const QFont font = code_font(9);
        painter->setFont(font);
        const QFontMetrics metrics(font);
        QRect box = metrics.boundingRect(text).adjusted(-5, -2, 5, 2);
        box.moveCenter(anchor.toPoint());
        if (scene()) {
            auto hits = [&](const QRect& candidate) {
                for (QGraphicsItem* item : scene()->items(QRectF(candidate))) {
                    if (item->type() == kClassType) return true;
                }
                return false;
            };
            if (hits(box)) {
                const QPoint shifts[] = {{0, -18}, {0, 18}, {0, -36}, {0, 36}, {-36, 0}, {36, 0},
                                          {-36, -18}, {36, -18}, {0, -54}, {0, 54}};
                for (const QPoint& shift : shifts) {
                    const QRect moved = box.translated(shift);
                    if (!hits(moved)) {
                        box = moved;
                        break;
                    }
                }
            }
        }
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(20, 22, 28, 235));
        painter->drawRoundedRect(box, 4, 4);
        painter->setPen(relation_color(kind_));
        painter->drawText(box, Qt::AlignCenter, text);
    }

private:
    static QPointF side_point(const QRectF& rect, const QPointF& other) {
        const QPointF center = rect.center();
        if (std::abs(other.x() - center.x()) > std::abs(other.y() - center.y())) {
            return QPointF(other.x() >= center.x() ? rect.right() : rect.left(), center.y());
        }
        return QPointF(center.x(), other.y() >= center.y() ? rect.bottom() : rect.top());
    }

    ClassNode* from_ = nullptr;
    ClassNode* to_ = nullptr;
    RelationKind kind_ = RelationKind::Dependency;
    QString label_;
    int lane_ = 0;
    QString from_id_;
    QString to_id_;
    bool hot_ = false;
};

ClassNode::~ClassNode() {
    alive_ = false;
    for (RelationEdge* edge : edges_) edge->detach(this);
    edges_.clear();
}

QVariant ClassNode::itemChange(GraphicsItemChange change, const QVariant& value) {
    if (change == ItemPositionHasChanged && alive_) {
        for (RelationEdge* edge : edges_) edge->refresh();
        if (onMoved) onMoved(pos());
    }
    return QGraphicsItem::itemChange(change, value);
}

void ClassNode::hoverEnterEvent(QGraphicsSceneHoverEvent*) {
    hovered_ = true;
    for (RelationEdge* edge : edges_) edge->setHot(true);
    update();
}

void ClassNode::hoverLeaveEvent(QGraphicsSceneHoverEvent*) {
    hovered_ = false;
    for (RelationEdge* edge : edges_) edge->setHot(false);
    update();
}

void ClassNode::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) {
    if (onActivate) onActivate();
    event->accept();
}

namespace {

class DotScene : public QGraphicsScene {
public:
    explicit DotScene(QObject* parent = nullptr) : QGraphicsScene(parent) {}

protected:
    void drawBackground(QPainter* painter, const QRectF& rect) override {
        painter->fillRect(rect, QColor("#14161a"));
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(255, 255, 255, 16));
        const int step = 24;
        const int left = static_cast<int>(std::floor(rect.left() / step)) * step;
        const int top = static_cast<int>(std::floor(rect.top() / step)) * step;
        for (int y = top; y < rect.bottom(); y += step) {
            for (int x = left; x < rect.right(); x += step) {
                painter->drawEllipse(QPointF(x, y), 1.05, 1.05);
            }
        }
    }

    void drawForeground(QPainter* painter, const QRectF&) override {
        painter->setRenderHint(QPainter::Antialiasing, true);
        for (QGraphicsItem* item : items()) {
            if (item->type() != kEdgeType) continue;
            static_cast<RelationEdge*>(item)->paintLabel(painter);
        }
    }
};

QString field_line(const Field& field) {
    const QString access = field.access == "private" ? "-" : field.access == "protected" ? "#" : "+";
    return access + " " + from_std(field.name) + " : " + from_std(field.type_spelling);
}

QString method_line(const Method& method) {
    const QString access = method.access == "private" ? "-" : method.access == "protected" ? "#" : "+";
    return access + " " + from_std(method.signature());
}

}  // namespace

class DiagramView : public QGraphicsView {
public:
    DiagramView(QGraphicsScene* scene, RewireWindow* window) : QGraphicsView(scene), window_(window) {
        setFrameShape(QFrame::NoFrame);
        setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
        setDragMode(QGraphicsView::NoDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setResizeAnchor(QGraphicsView::AnchorViewCenter);
        viewport()->setCursor(Qt::ArrowCursor);
    }

protected:
    void wheelEvent(QWheelEvent* event) override {
        const int delta = event->angleDelta().y();
        if (delta == 0) {
            QGraphicsView::wheelEvent(event);
            return;
        }
        const qreal factor = delta > 0 ? 1.12 : 1.0 / 1.12;
        scale(factor, factor);
        event->accept();
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && !itemAt(event->pos())) {
            panning_ = true;
            pan_origin_ = event->pos();
            viewport()->setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (panning_) {
            const QPoint delta = event->pos() - pan_origin_;
            pan_origin_ = event->pos();
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
            verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
            event->accept();
            return;
        }
        QGraphicsView::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (panning_ && event->button() == Qt::LeftButton) {
            panning_ = false;
            viewport()->setCursor(Qt::ArrowCursor);
            event->accept();
            return;
        }
        QGraphicsView::mouseReleaseEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent* event) override {
        auto* node = dynamic_cast<ClassNode*>(itemAt(event->pos()));
        if (!node) {
            QGraphicsView::contextMenuEvent(event);
            return;
        }
        const QString id = node->id();
        QMenu menu;
        menu.addAction("Focus neighborhood", window_, [this, id] { window_->focusClass(id); });
        menu.addSeparator();
        menu.addAction("Add field", window_, [this, id] {
            window_->selectClass(id);
            window_->beginEdit(static_cast<int>(EditAction::AddField));
        });
        menu.addAction("Add method", window_, [this, id] {
            window_->selectClass(id);
            window_->beginEdit(static_cast<int>(EditAction::AddMethod));
        });
        menu.addAction("Add base", window_, [this, id] {
            window_->selectClass(id);
            window_->beginEdit(static_cast<int>(EditAction::AddBase));
        });
        menu.addAction("Rename class", window_, [this, id] {
            window_->selectClass(id);
            window_->beginEdit(static_cast<int>(EditAction::RenameClass));
        });
        menu.addAction("Remove member", window_, [this, id] {
            window_->selectClass(id);
            window_->beginEdit(static_cast<int>(EditAction::RemoveMember));
        });
        menu.exec(event->globalPos());
    }

private:
    RewireWindow* window_ = nullptr;
    bool panning_ = false;
    QPoint pan_origin_;
};

RewireWindow::RewireWindow(QString project, QString session_query)
    : project_(std::move(project)), session_query_(std::move(session_query)), watcher_(project_.toStdString()) {
    const QString base = QFileInfo(project_).fileName();
    setWindowTitle(base.isEmpty() ? "Rewire" : "Rewire — " + base);
    buildUi();
    applyStyle();
    link_.set_wake([this] {
        auto alive = alive_;
        QMetaObject::invokeMethod(
            this,
            [this, alive] {
                if (!alive->load()) return;
                for (const auto& line : link_.drain()) status(from_std(line));
            },
            Qt::QueuedConnection);
    });
    if (link_.start(project_.toStdString())) {
        inbox_note_ = "Listening for Claude replies.";
    } else {
        inbox_note_ = "Reply inbox is off. Sends still go to the Claude session you pick.";
    }
    watcher_.start([this] {
        auto alive = alive_;
        QMetaObject::invokeMethod(
            this,
            [this, alive] {
                if (!alive->load()) return;
                refresh_from_edit_ = waiting_;
                requestParse();
            },
            Qt::QueuedConnection);
    });
    worker_ = std::thread([this] { parserLoop(); });
    refreshSessions();
    loadSavedSession();
    requestParse();
}

RewireWindow::~RewireWindow() { shutdown(); }

void RewireWindow::closeEvent(QCloseEvent* event) {
    shutdown();
    QMainWindow::closeEvent(event);
}

void RewireWindow::shutdown() {
    if (shut_) return;
    shut_ = true;
    alive_->store(false);
    job_stop_.store(true);
    job_cv_.notify_all();
    watcher_.stop();
    link_.stop();
    if (worker_.joinable()) worker_.join();
}

void RewireWindow::parserLoop() {
    while (true) {
        std::unique_lock<std::mutex> lock(job_mu_);
        job_cv_.wait(lock, [this] { return job_stop_.load() || job_; });
        if (job_stop_.load()) return;
        job_ = false;
        lock.unlock();
        ProjectModel parsed = parse_project(project_.toStdString(), &job_stop_);
        if (job_stop_.load() || !alive_->load()) return;
        auto alive = alive_;
        QMetaObject::invokeMethod(
            this,
            [this, alive, parsed]() mutable {
                if (!alive->load()) return;
                applyModel(std::move(parsed));
            },
            Qt::QueuedConnection);
    }
}

void RewireWindow::requestParse() {
    {
        std::lock_guard<std::mutex> lock(job_mu_);
        job_ = true;
    }
    job_cv_.notify_one();
    status("Reading sources...");
}

void RewireWindow::status(const QString& text) { statusBar()->showMessage(text, 0); }

void RewireWindow::buildUi() {
    scene_ = new DotScene(this);
    view_ = new DiagramView(scene_, this);
    connect(scene_, &QGraphicsScene::selectionChanged, this, [this] { syncSelection(); });

    search_ = new QLineEdit;
    search_->setPlaceholderText("Filter classes");
    class_list_ = new QListWidget;
    class_list_->setMinimumWidth(200);
    auto* left = new QWidget;
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(8, 8, 8, 8);
    auto* classes_heading = new QLabel("Classes");
    classes_heading->setObjectName("section");
    left_layout->addWidget(classes_heading);
    left_layout->addWidget(search_);
    left_layout->addWidget(class_list_, 1);

    class_title_ = new QLabel("Select a class");
    class_title_->setObjectName("heading");
    class_title_->setWordWrap(true);
    class_file_ = new QLabel(" ");
    class_file_->setObjectName("section");
    class_file_->setWordWrap(true);
    inspector_ = new QTextEdit;
    inspector_->setReadOnly(true);
    inspector_->setMinimumHeight(120);
    inspector_->setFont(code_font(11));
    jump_list_ = new QListWidget;
    jump_list_->setMinimumHeight(100);
    action_combo_ = new QComboBox;
    action_combo_->addItems({"Add class", "Rename class", "Add field", "Add method", "Add base", "Remove member"});
    action_combo_->setCurrentIndex(static_cast<int>(EditAction::AddField));
    edit_class_ = new QLineEdit;
    edit_class_->setPlaceholderText("Class name");
    edit_primary_ = new QLineEdit;
    edit_secondary_ = new QLineEdit;
    access_combo_ = new QComboBox;
    access_combo_->addItems({"public", "protected", "private"});
    relation_combo_ = new QComboBox;
    relation_combo_->addItems({"auto", "composition", "aggregation", "association", "dependency", "inheritance"});
    summary_ = new QLabel("The edit is sent to Claude. Rewire does not rewrite the files itself.");
    summary_->setWordWrap(true);
    send_button_ = new QPushButton("Send to Claude");
    auto* form = new QFormLayout;
    form->addRow("Action", action_combo_);
    form->addRow("Class", edit_class_);
    form->addRow("Primary", edit_primary_);
    form->addRow("Secondary", edit_secondary_);
    form->addRow("Access", access_combo_);
    form->addRow("Relationship", relation_combo_);
    auto* right = new QWidget;
    auto* right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(8, 8, 8, 8);
    right_layout->addWidget(class_title_);
    right_layout->addWidget(class_file_);
    right_layout->addWidget(inspector_, 1);
    auto* jump_heading = new QLabel("Open");
    jump_heading->setObjectName("section");
    right_layout->addWidget(jump_heading);
    right_layout->addWidget(jump_list_);
    auto* edit_heading = new QLabel("Edit the diagram");
    edit_heading->setObjectName("section");
    right_layout->addWidget(edit_heading);
    right_layout->addLayout(form);
    right_layout->addWidget(summary_);
    right_layout->addWidget(send_button_);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(right);
    scroll->setMinimumWidth(300);

    auto* columns = new QSplitter(Qt::Horizontal);
    columns->addWidget(left);
    columns->addWidget(view_);
    columns->addWidget(scroll);
    columns->setStretchFactor(1, 1);
    columns->setSizes({230, 860, 330});

    flow_list_ = new QListWidget;
    step_list_ = new QListWidget;
    call_list_ = new QListWidget;
    auto make_column = [](const QString& title, QListWidget* list) {
        auto* wrap = new QWidget;
        auto* layout = new QVBoxLayout(wrap);
        layout->setContentsMargins(8, 4, 8, 8);
        auto* label = new QLabel(title);
        label->setObjectName("section");
        layout->addWidget(label);
        layout->addWidget(list);
        return wrap;
    };
    auto* flow_split = new QSplitter(Qt::Horizontal);
    flow_split->addWidget(make_column("Functions", flow_list_));
    flow_split->addWidget(make_column("Steps", step_list_));
    flow_split->addWidget(make_column("Data moving between files", call_list_));
    flow_split->setStretchFactor(1, 2);

    auto* vertical = new QSplitter(Qt::Vertical);
    vertical->addWidget(columns);
    vertical->addWidget(flow_split);
    vertical->setStretchFactor(0, 4);
    vertical->setStretchFactor(1, 1);
    setCentralWidget(vertical);

    auto* toolbar = addToolBar("Rewire");
    toolbar->setMovable(false);
    auto* title = new QLabel(" Rewire ");
    title->setObjectName("heading");
    toolbar->addWidget(title);
    auto* refresh = new QPushButton("Refresh");
    auto* fit = new QPushButton("Fit");
    auto* relayout = new QPushButton("Relayout");
    mode_combo_ = new QComboBox;
    mode_combo_->addItems({"Neighborhood", "All classes"});
    auto* zoom_in = new QPushButton("+");
    auto* zoom_out = new QPushButton("−");
    session_combo_ = new QComboBox;
    session_combo_->setMinimumWidth(280);
    auto* sessions = new QPushButton("Sessions");
    toolbar->addWidget(refresh);
    toolbar->addWidget(fit);
    toolbar->addWidget(relayout);
    toolbar->addWidget(mode_combo_);
    toolbar->addWidget(zoom_in);
    toolbar->addWidget(zoom_out);
    toolbar->addSeparator();
    for (const auto& entry : {std::pair<const char*, RelationKind>{"inheritance", RelationKind::Inheritance},
                               {"composition", RelationKind::Composition},
                               {"aggregation", RelationKind::Aggregation},
                               {"association", RelationKind::Association},
                               {"dependency", RelationKind::Dependency}}) {
        auto* swatch = new QLabel(entry.first);
        swatch->setStyleSheet(QString("color: %1; padding: 0 8px;").arg(relation_color(entry.second).name()));
        toolbar->addWidget(swatch);
    }
    toolbar->addSeparator();
    toolbar->addWidget(session_combo_);
    toolbar->addWidget(sessions);

    counts_ = new QLabel;
    statusBar()->addPermanentWidget(counts_);

    connect(refresh, &QPushButton::clicked, this, [this] { requestParse(); });
    connect(fit, &QPushButton::clicked, this, [this] { fitDiagram(); });
    connect(relayout, &QPushButton::clicked, this, [this] {
        pinned_.clear();
        rebuildScene();
        fitDiagram();
    });
    connect(zoom_in, &QPushButton::clicked, this, [this] { view_->scale(1.15, 1.15); });
    connect(zoom_out, &QPushButton::clicked, this, [this] { view_->scale(1.0 / 1.15, 1.0 / 1.15); });
    connect(mode_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (updating_) return;
        show_all_ = index == 1;
        rebuildScene();
        fitDiagram();
    });
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        for (int i = 0; i < class_list_->count(); ++i) {
            auto* item = class_list_->item(i);
            const bool match = text.isEmpty() || item->text().contains(text, Qt::CaseInsensitive) ||
                               item->data(Qt::UserRole).toString().contains(text, Qt::CaseInsensitive);
            item->setHidden(!match);
        }
        for (auto* item : scene_->items()) {
            if (item->type() != kClassType) continue;
            auto* node = static_cast<ClassNode*>(item);
            const bool match = text.isEmpty() || node->id().contains(text, Qt::CaseInsensitive);
            node->setDimmed(!match);
        }
    });
    connect(search_, &QLineEdit::returnPressed, this, [this] {
        for (int i = 0; i < class_list_->count(); ++i) {
            auto* item = class_list_->item(i);
            if (!item->isHidden()) {
                focusClass(item->data(Qt::UserRole).toString());
                return;
            }
        }
    });
    connect(class_list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item, QListWidgetItem*) {
        if (updating_ || !item) return;
        selectClass(item->data(Qt::UserRole).toString());
    });
    connect(jump_list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const QString kind = item->data(kRoleKind).toString();
        const QString id = item->data(kRoleId).toString();
        QTimer::singleShot(0, this, [this, kind, id] {
            if (kind == "flow") showFlowContaining(id);
            else selectClass(id);
        });
    });
    connect(flow_list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item, QListWidgetItem*) {
        if (updating_ || !item) return;
        const std::string name = item->data(Qt::UserRole).toString().toStdString();
        const FunctionFlow* flow = nullptr;
        for (const auto& candidate : model_.flows) {
            if (candidate.qualified_name == name) flow = &candidate;
        }
        step_list_->clear();
        call_list_->clear();
        if (!flow) return;
        for (const auto& step : flow->steps) {
            const QString indent(std::max(0, step.depth) * 2, ' ');
            auto* row = new QListWidgetItem(indent + from_std(step.kind + "  " + step.text));
            const QString callee = classOfCallee(from_std(step.callee));
            row->setData(Qt::UserRole, callee);
            if (step.kind == "call") row->setForeground(QColor("#7dcfff"));
            else if (step.kind == "branch" || step.kind == "loop" || step.kind == "switch")
                row->setForeground(QColor("#e0af68"));
            else if (step.kind == "return") row->setForeground(QColor("#8fbf6a"));
            step_list_->addItem(row);
        }
        for (const auto& call : model_.calls) {
            if (call.caller != flow->qualified_name) continue;
            const QString row = from_std(call.caller_file) + "  →  " + from_std(call.callee_file) + "    " +
                                from_std(short_name(call.callee)) + "(" + from_std(call.args) + ")";
            auto* item_row = new QListWidgetItem(row);
            item_row->setData(Qt::UserRole, classOfCallee(from_std(call.callee)));
            call_list_->addItem(item_row);
        }
    });
    connect(step_list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const QString id = item->data(Qt::UserRole).toString();
        if (!id.isEmpty()) selectClass(id);
    });
    connect(call_list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const QString id = item->data(Qt::UserRole).toString();
        if (!id.isEmpty()) selectClass(id);
    });
    connect(send_button_, &QPushButton::clicked, this, [this] { sendEdit(); });
    connect(sessions, &QPushButton::clicked, this, [this] { refreshSessions(); });
    connect(session_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (updating_) return;
        const QString id = session_combo_->currentData().toString();
        if (id.isEmpty()) {
            has_session_ = false;
            status("No Claude session selected.");
            return;
        }
        for (const auto& session : sessions_) {
            if (from_std(session.session_id) == id) adoptSession(session, true);
        }
    });
    auto refresh_summary = [this] { refreshSummary(); };
    connect(action_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, refresh_summary](int) {
        updatePlaceholders();
        refresh_summary();
    });
    connect(edit_class_, &QLineEdit::textChanged, this, [this, refresh_summary] {
        updatePlaceholders();
        refresh_summary();
    });
    connect(edit_primary_, &QLineEdit::textChanged, this, refresh_summary);
    connect(edit_secondary_, &QLineEdit::textChanged, this, refresh_summary);
    connect(access_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, refresh_summary);
    connect(relation_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, refresh_summary);
    updatePlaceholders();
}

void RewireWindow::updatePlaceholders() {
    const auto action = static_cast<EditAction>(action_combo_->currentIndex());
    auto set = [this](const QString& primary, const QString& secondary, bool secondary_on) {
        edit_primary_->setPlaceholderText(primary);
        edit_secondary_->setPlaceholderText(secondary);
        edit_secondary_->setEnabled(secondary_on);
    };
    QString type_hint = "Type, for example std::string";
    if (const Class* item = model_.find_class(edit_class_->text().trimmed().toStdString())) {
        const std::string language = project_language(model_, item->file);
        if (language == "Java") type_hint = "Type, for example String";
        else if (language == "Python") type_hint = "Type, for example str";
        else if (language == "TypeScript") type_hint = "Type, for example string";
        else if (language == "Rust") type_hint = "Type, for example String";
        else if (language == "C") type_hint = "Type, for example int";
    }
    switch (action) {
        case EditAction::AddClass: set("New class name", "Optional base class", true); break;
        case EditAction::RenameClass: set("New name", "", false); break;
        case EditAction::AddField: set("Field name", type_hint, true); break;
        case EditAction::AddMethod: set("Signature, for example cancel()", "", false); break;
        case EditAction::AddBase: set("Base class", "", false); break;
        case EditAction::RemoveMember: set("Member name", "", false); break;
    }
}

void RewireWindow::applyStyle() {
    setStyleSheet(
        "QMainWindow, QWidget { background: #1b1d22; color: #e6e6ea; }"
        "QLineEdit, QComboBox, QTextEdit, QListWidget {"
        "  background: #23262e; color: #e6e6ea; border: 1px solid #3a3f4b; border-radius: 6px; padding: 3px; }"
        "QListWidget::item { padding: 3px 6px; }"
        "QListWidget::item:selected { background: #31456e; color: white; }"
        "QPushButton { background: #31456e; color: white; border: none; border-radius: 6px; padding: 6px 12px; }"
        "QPushButton:hover { background: #3c5690; }"
        "QToolBar { background: #1b1d22; border: none; spacing: 6px; padding: 4px; }"
        "QStatusBar { background: #16181d; color: #b8bcc8; }"
        "QSplitter::handle { background: #2c3038; }"
        "QScrollArea { border: none; }"
        "QLabel#heading { font-size: 16px; font-weight: 600; color: #d7e3ff; }"
        "QLabel#section { color: #9aa3b5; }"
        "QGraphicsView { border: none; background: #14161a; }");
}

void RewireWindow::applyModel(ProjectModel model) {
    const QString previous_focus = focus_id_;
    const QString previous_flow = flow_list_->currentItem() ? flow_list_->currentItem()->data(Qt::UserRole).toString()
                                                            : QString();
    model_ = std::move(model);
    parsed_ = true;
    if (!previous_focus.isEmpty() && model_.find_class(previous_focus.toStdString())) {
        focus_id_ = previous_focus;
    } else if (!model_.classes.empty()) {
        focus_id_ = from_std(model_.classes.front().qualified_name);
    } else {
        focus_id_.clear();
    }
    rebuildClassList();
    rebuildScene();
    refillFlows();
    if (!previous_flow.isEmpty()) showFlowContaining(previous_flow);
    if (!focus_id_.isEmpty()) {
        showInspectorForClass(focus_id_);
        syncListTo(focus_id_);
        if (auto* node = findNode(focus_id_)) {
            updating_ = true;
            node->setSelected(true);
            updating_ = false;
        }
    }
    if (!fitted_) {
        fitDiagram();
        fitted_ = true;
    }
    QString message = QString("Read %1 classes and %2 links. ").arg(model_.classes.size()).arg(model_.relations.size());
    if (refresh_from_edit_) {
        refresh_from_edit_ = false;
        waiting_ = false;
        message = "Code changed. The diagram matches the sources. ";
    } else if (!first_model_) {
        message = "Sources changed. Diagram refreshed. ";
    }
    first_model_ = false;
    updatePlaceholders();
    if (!model_.diagnostics.empty()) message += from_std(model_.diagnostics.front());
    else message += inbox_note_;
    status(message);
    counts_->setText(QString("%1 classes   %2 links").arg(model_.classes.size()).arg(model_.relations.size()));
}

void RewireWindow::rebuildClassList() {
    const QString keep = class_list_->currentItem() ? class_list_->currentItem()->data(Qt::UserRole).toString() : focus_id_;
    const QString filter = search_->text();
    updating_ = true;
    class_list_->clear();
    std::vector<const Class*> classes;
    for (const auto& item : model_.classes) classes.push_back(&item);
    std::sort(classes.begin(), classes.end(), [](const Class* a, const Class* b) {
        return a->qualified_name < b->qualified_name;
    });
    for (const Class* item : classes) {
        const QString qualified = from_std(item->qualified_name);
        const QString file = QFileInfo(from_std(item->file)).fileName();
        auto* row = new QListWidgetItem(from_std(item->name) + "    " + file);
        row->setData(Qt::UserRole, qualified);
        row->setToolTip(qualified + "\n" + from_std(item->file) + ":" + QString::number(item->line));
        class_list_->addItem(row);
        const bool match = filter.isEmpty() || row->text().contains(filter, Qt::CaseInsensitive) ||
                           qualified.contains(filter, Qt::CaseInsensitive);
        row->setHidden(!match);
    }
    updating_ = false;
    if (!keep.isEmpty()) syncListTo(keep);
}

ClassNode* RewireWindow::findNode(const QString& qualified) const {
    for (auto* item : scene_->items()) {
        if (item->type() == kClassType && static_cast<ClassNode*>(item)->id() == qualified) {
            return static_cast<ClassNode*>(item);
        }
    }
    return nullptr;
}

RelationEdge* RewireWindow::findEdge(const QString& from, const QString& to) const {
    for (auto* item : scene_->items()) {
        if (item->type() != kEdgeType) continue;
        auto* edge = static_cast<RelationEdge*>(item);
        if (edge->fromId() == from && edge->toId() == to) return edge;
    }
    return nullptr;
}

ClassNode* RewireWindow::makeNode(const Class& item, bool focus) {
    std::vector<QString> fields;
    std::vector<QColor> colors;
    for (const auto& field : item.fields) {
        fields.push_back(field_line(field));
        colors.push_back(field.target.empty() ? QColor("#d5d8e0") : relation_color(field.relation));
    }
    std::vector<QString> methods;
    for (const auto& method : item.methods) methods.push_back(method_line(method));
    const QString id = from_std(item.qualified_name);
    const QString file = QFileInfo(from_std(item.file)).fileName() + ":" + QString::number(item.line);
    auto* node = new ClassNode(id, from_std(item.name), file, focus, std::move(fields), std::move(colors),
                               std::move(methods));
    node->setToolTip(id + "\n" + from_std(item.file) + ":" + QString::number(item.line));
    node->onMoved = [this, id](QPointF pos) {
        if (!laying_out_) pinned_[id] = pos;
    };
    node->onActivate = [this, id] { focusClass(id); };
    scene_->addItem(node);
    return node;
}

void RewireWindow::addEdge(ClassNode* from, ClassNode* to, const Relation& relation, int lane) {
    auto* edge = new RelationEdge(from, to, relation.kind, from_std(relation.label), lane);
    scene_->addItem(edge);
}

void RewireWindow::rebuildScene() {
    updating_ = true;
    laying_out_ = true;
    scene_->clear();
    if (model_.classes.empty()) {
        auto* text = scene_->addText("No classes were found in this project.");
        text->setDefaultTextColor(QColor("#9aa3b5"));
        laying_out_ = false;
        updating_ = false;
        return;
    }
    if (show_all_ || focus_id_.isEmpty() || !model_.find_class(focus_id_.toStdString())) {
        std::vector<const Class*> classes;
        for (const auto& item : model_.classes) classes.push_back(&item);
        std::sort(classes.begin(), classes.end(), [](const Class* a, const Class* b) {
            return a->qualified_name < b->qualified_name;
        });
        const int columns = classes.size() > 12 ? 4 : 3;
        qreal x = 0;
        qreal y = 0;
        qreal row_h = 0;
        int column = 0;
        for (const Class* item : classes) {
            auto* node = makeNode(*item, from_std(item->qualified_name) == focus_id_);
            const QString id = node->id();
            if (pinned_.count(id)) node->setPos(pinned_[id]);
            else node->setPos(x, y);
            row_h = std::max(row_h, node->nodeHeight());
            x += node->nodeWidth() + 120;
            if (++column == columns) {
                column = 0;
                x = 0;
                y += row_h + 100;
                row_h = 0;
            }
        }
    } else {
        const Class* focus = model_.find_class(focus_id_.toStdString());
        std::vector<QString> bases, parts, deps, derived, owners;
        auto add_unique = [&](std::vector<QString>& bucket, const std::string& name) {
            const QString id = from_std(name);
            if (id.isEmpty() || id == focus_id_) return;
            if (std::find(bucket.begin(), bucket.end(), id) != bucket.end()) return;
            if (!model_.find_class(name)) return;
            bucket.push_back(id);
        };
        for (const auto& relation : model_.relations) {
            if (relation.from == focus_id_.toStdString()) {
                if (relation.kind == RelationKind::Inheritance) add_unique(bases, relation.to);
                else if (relation.kind == RelationKind::Dependency) add_unique(deps, relation.to);
                else add_unique(parts, relation.to);
            } else if (relation.to == focus_id_.toStdString()) {
                if (relation.kind == RelationKind::Inheritance) add_unique(derived, relation.from);
                else add_unique(owners, relation.from);
            }
        }
        auto place_row = [&](const std::vector<QString>& ids, qreal left, qreal y) {
            qreal x = left;
            qreal height = 0;
            for (const auto& id : ids) {
                const Class* item = model_.find_class(id.toStdString());
                auto* node = makeNode(*item, false);
                if (pinned_.count(id)) node->setPos(pinned_[id]);
                else node->setPos(x, y);
                x += node->nodeWidth() + 100;
                height = std::max(height, node->nodeHeight());
            }
            return height;
        };
        qreal y = 24;
        qreal height = place_row(bases, 48, y);
        if (height > 0) y += height + 96;
        auto* focus_node = makeNode(*focus, true);
        if (pinned_.count(focus_id_)) focus_node->setPos(pinned_[focus_id_]);
        else focus_node->setPos(36, y);
        qreal x = 36 + focus_node->nodeWidth() + 160;
        qreal mid = focus_node->nodeHeight();
        for (const auto& id : derived) {
            const Class* item = model_.find_class(id.toStdString());
            auto* node = makeNode(*item, false);
            if (pinned_.count(id)) node->setPos(pinned_[id]);
            else node->setPos(x, y);
            x += node->nodeWidth() + 100;
            mid = std::max(mid, node->nodeHeight());
        }
        y += mid + 96;
        height = place_row(parts, 16, y);
        if (height > 0) y += height + 96;
        height = place_row(deps, 16, y);
        if (height > 0) y += height + 96;
        place_row(owners, 16, y);
    }

    std::map<std::pair<QString, QString>, int> lanes;
    for (const auto& relation : model_.relations) {
        const QString from = from_std(relation.from);
        const QString to = from_std(relation.to);
        if (!show_all_ && from != focus_id_ && to != focus_id_) continue;
        auto* from_node = findNode(from);
        auto* to_node = findNode(to);
        if (!from_node || !to_node || from_node == to_node) continue;
        const auto key = from < to ? std::make_pair(from, to) : std::make_pair(to, from);
        const int lane = lanes[key]++;
        addEdge(from_node, to_node, relation, lane);
    }
    laying_out_ = false;
    updating_ = false;
    scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-70, -70, 70, 70));
}

void RewireWindow::refillFlows() {
    const QString keep = flow_list_->currentItem() ? flow_list_->currentItem()->data(Qt::UserRole).toString() : QString();
    updating_ = true;
    flow_list_->clear();
    for (const auto& flow : model_.flows) {
        auto* row = new QListWidgetItem(from_std(short_name(flow.qualified_name) + "    " + flow.file));
        row->setData(Qt::UserRole, from_std(flow.qualified_name));
        row->setToolTip(from_std(flow.qualified_name));
        flow_list_->addItem(row);
    }
    updating_ = false;
    if (!keep.isEmpty() && showFlowContaining(keep)) return;
    if (flow_list_->count() > 0) flow_list_->setCurrentRow(0);
    else {
        step_list_->clear();
        call_list_->clear();
    }
}

void RewireWindow::showInspectorForClass(const QString& qualified) {
    const Class* item = model_.find_class(qualified.toStdString());
    if (!item) return;
    class_title_->setText(from_std(item->qualified_name));
    class_file_->setText(from_std(item->file) + ":" + QString::number(item->line));
    QString body;
    if (!item->bases.empty()) {
        body += "Bases\n";
        for (const auto& base : item->bases) body += "  " + from_std(base.access) + " " + from_std(base.name) + "\n";
        body += "\n";
    }
    body += "Fields\n";
    if (item->fields.empty()) body += "  (none)\n";
    for (const auto& field : item->fields) {
        body += "  " + field_line(field);
        if (!field.target.empty()) body += "    " + from_std(relation_name(field.relation));
        body += "\n";
    }
    body += "\nMethods\n";
    if (item->methods.empty()) body += "  (none)\n";
    for (const auto& method : item->methods) body += "  " + method_line(method) + "\n";
    inspector_->setPlainText(body);
    jump_list_->clear();
    for (const auto& relation : model_.relations) {
        QString other;
        QString direction;
        if (relation.from == item->qualified_name) {
            other = from_std(relation.to);
            direction = "→";
        } else if (relation.to == item->qualified_name) {
            other = from_std(relation.from);
            direction = "←";
        } else {
            continue;
        }
        QString text = from_std(relation_name(relation.kind)) + " " + direction + " " + other;
        if (!relation.label.empty()) text += "  (" + from_std(relation.label) + ")";
        auto* row = new QListWidgetItem(text);
        row->setData(kRoleKind, "class");
        row->setData(kRoleId, other);
        row->setForeground(relation_color(relation.kind));
        jump_list_->addItem(row);
    }
    for (const auto& flow : model_.flows) {
        if (flow.owner_class != item->qualified_name &&
            flow.qualified_name.rfind(item->qualified_name + "::", 0) != 0 &&
            flow.qualified_name.rfind(item->qualified_name + ".", 0) != 0) {
            continue;
        }
        auto* row = new QListWidgetItem("flow  " + from_std(short_name(flow.qualified_name)));
        row->setData(kRoleKind, "flow");
        row->setData(kRoleId, from_std(flow.qualified_name));
        jump_list_->addItem(row);
    }
    if (action_combo_->currentIndex() != static_cast<int>(EditAction::AddClass) && !edit_class_->hasFocus()) {
        edit_class_->setText(qualified);
    }
}

void RewireWindow::showInspectorForEdge(const QString& from, const QString& to, const QString& kind, const QString& label) {
    class_title_->setText(kind);
    class_file_->setText(from + "  →  " + to);
    QString body = kind + "\n" + from + "\n  →  " + to + "\n";
    if (!label.isEmpty()) body += "\n" + label + "\n";
    inspector_->setPlainText(body);
}

void RewireWindow::syncSelection() {
    if (updating_) return;
    ClassNode* node = nullptr;
    RelationEdge* edge = nullptr;
    for (auto* item : scene_->selectedItems()) {
        if (item->type() == kClassType) node = static_cast<ClassNode*>(item);
        else if (item->type() == kEdgeType) edge = static_cast<RelationEdge*>(item);
    }
    if (node) {
        showInspectorForClass(node->id());
        syncListTo(node->id());
        return;
    }
    if (edge) {
        showInspectorForEdge(edge->fromId(), edge->toId(), relation_name(edge->kind()), edge->label());
    }
}

void RewireWindow::syncListTo(const QString& qualified) {
    updating_ = true;
    for (int i = 0; i < class_list_->count(); ++i) {
        auto* item = class_list_->item(i);
        if (item->data(Qt::UserRole).toString() == qualified) {
            class_list_->setCurrentItem(item);
            class_list_->scrollToItem(item);
            break;
        }
    }
    updating_ = false;
}

QString RewireWindow::classOfCallee(const QString& callee) const {
    QString best;
    for (const auto& item : model_.classes) {
        const QString name = from_std(item.qualified_name);
        if (callee == name || callee.startsWith(name + "::") || callee.startsWith(name + ".")) {
            if (name.size() > best.size()) best = name;
        }
    }
    return best;
}

EditRequest RewireWindow::currentRequest() const {
    EditRequest request;
    request.action = static_cast<EditAction>(action_combo_->currentIndex());
    request.class_name = edit_class_->text().trimmed().toStdString();
    request.primary = edit_primary_->text().trimmed().toStdString();
    request.secondary = edit_secondary_->text().trimmed().toStdString();
    request.access = access_combo_->currentText().toStdString();
    request.relationship = relation_combo_->currentText().toStdString();
    return request;
}

void RewireWindow::refreshSummary() {
    if (!action_combo_) return;
    const PreparedEdit prepared = prepare_edit(model_, currentRequest());
    if (!prepared.error.empty()) {
        summary_->setStyleSheet("color: #f7768e;");
        summary_->setText(from_std(prepared.error));
        return;
    }
    summary_->setStyleSheet("color: #c0caf5;");
    summary_->setText(from_std(prepared.summary));
}

void RewireWindow::beginEdit(int action) {
    action_combo_->setCurrentIndex(action);
    const QString selected = selectedClass();
    if (action != static_cast<int>(EditAction::AddClass) && !selected.isEmpty()) edit_class_->setText(selected);
    edit_primary_->setFocus();
    refreshSummary();
}

void RewireWindow::sendEdit() {
    const PreparedEdit prepared = prepare_edit(model_, currentRequest());
    refreshSummary();
    if (!prepared.error.empty()) {
        status(from_std(prepared.error));
        return;
    }
    if (!has_session_) {
        status("Choose a Claude Code session before sending an edit.");
        return;
    }
    const SendResult result = link_.send(connected_, prepared.prompt);
    if (!result.ok) {
        status(result.error.empty() ? "The edit did not reach Claude." : from_std(result.error));
        return;
    }
    waiting_ = true;
    status("Sent to " + from_std(connected_.name) + ": " + from_std(prepared.summary) +
           ". Approve it in that Claude session if it asks. The diagram refreshes when the files change.");
}

void RewireWindow::refreshSessions() {
    sessions_ = list_sessions();
    const QString keep = has_session_ ? from_std(connected_.session_id) : session_combo_->currentData().toString();
    updating_ = true;
    session_combo_->clear();
    session_combo_->addItem("Not connected", QString());
    for (const auto& session : sessions_) {
        const QString label = from_std(session.name) + "  " + from_std(session.status) + "  " + from_std(session.cwd);
        session_combo_->addItem(label, from_std(session.session_id));
    }
    int index = 0;
    if (!keep.isEmpty()) {
        const int found = session_combo_->findData(keep);
        if (found >= 0) index = found;
    }
    session_combo_->setCurrentIndex(index);
    updating_ = false;
}

void RewireWindow::adoptSession(const ClaudeSession& session, bool persist) {
    connected_ = session;
    has_session_ = true;
    QString line = "Connected to " + from_std(session.name) + " (" + from_std(session.status) + ") in " +
                   from_std(session.cwd);
    if (!paths_overlap(project_, from_std(session.cwd))) {
        line += ". That session is not in this project, so it may refuse to edit these files.";
    }
    status(line);
    if (persist) save_session_id(project_, from_std(session.session_id));
    updating_ = true;
    const int found = session_combo_->findData(from_std(session.session_id));
    if (found >= 0) session_combo_->setCurrentIndex(found);
    updating_ = false;
}

void RewireWindow::loadSavedSession() {
    const ClaudeSession* found = nullptr;
    bool persist = false;
    if (!session_query_.isEmpty()) {
        found = find_query(sessions_, session_query_);
        persist = found != nullptr;
        if (!found) status("No reachable Claude session matches " + session_query_ + ".");
    } else {
        const QString saved = load_session_id(project_);
        if (!saved.isEmpty()) found = find_query(sessions_, saved);
        if (!found) found = match_project(sessions_, project_.toStdString());
        if (!found) {
            status("No Claude Code session is open in this project. Choose one from the session list, or start Claude here.");
        }
    }
    if (found) adoptSession(*found, persist);
}

void RewireWindow::fitDiagram() {
    const QRectF bounds = scene_->itemsBoundingRect().adjusted(-40, -40, 40, 40);
    if (bounds.isEmpty()) return;
    view_->fitInView(bounds, Qt::KeepAspectRatio);
}

bool RewireWindow::waitUntilParsed(int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (!parsed_ && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return parsed_;
}

int RewireWindow::nodeCount() const {
    int count = 0;
    for (auto* item : scene_->items()) {
        if (item->type() == kClassType) ++count;
    }
    return count;
}

int RewireWindow::edgeCount() const {
    int count = 0;
    for (auto* item : scene_->items()) {
        if (item->type() == kEdgeType) ++count;
    }
    return count;
}

bool RewireWindow::selectClass(const QString& qualified) {
    if (!findNode(qualified) && !focusClass(qualified)) return false;
    auto* node = findNode(qualified);
    if (!node) return false;
    updating_ = true;
    scene_->clearSelection();
    node->setSelected(true);
    updating_ = false;
    view_->centerOn(node);
    showInspectorForClass(qualified);
    syncListTo(qualified);
    return true;
}

bool RewireWindow::focusClass(const QString& qualified) {
    if (!model_.find_class(qualified.toStdString())) return false;
    focus_id_ = qualified;
    show_all_ = false;
    updating_ = true;
    mode_combo_->setCurrentIndex(0);
    updating_ = false;
    rebuildScene();
    showInspectorForClass(qualified);
    syncListTo(qualified);
    if (auto* node = findNode(qualified)) {
        updating_ = true;
        node->setSelected(true);
        updating_ = false;
    }
    fitDiagram();
    return true;
}

QString RewireWindow::selectedClass() const {
    for (auto* item : scene_->selectedItems()) {
        if (item->type() == kClassType) return static_cast<ClassNode*>(item)->id();
    }
    return {};
}

QString RewireWindow::focusedClass() const { return focus_id_; }
QString RewireWindow::inspectorText() const { return inspector_->toPlainText(); }

QString RewireWindow::flowText() const {
    QString text;
    for (int i = 0; i < step_list_->count(); ++i) text += step_list_->item(i)->text() + "\n";
    return text;
}

bool RewireWindow::showFlowContaining(const QString& text) {
    for (int i = 0; i < flow_list_->count(); ++i) {
        auto* item = flow_list_->item(i);
        if (item->data(Qt::UserRole).toString().contains(text)) {
            flow_list_->setCurrentItem(item);
            flow_list_->scrollToItem(item);
            return true;
        }
    }
    return false;
}

QPoint RewireWindow::viewPointForClass(const QString& qualified) const {
    const ClassNode* node = findNode(qualified);
    if (!node) return QPoint(-1, -1);
    const QPoint view_point = view_->mapFromScene(node->sceneBoundingRect().center());
    return view_->viewport()->mapFrom(view_, view_point);
}

QPoint RewireWindow::viewPointForEdge(const QString& from, const QString& to) const {
    const RelationEdge* edge = findEdge(from, to);
    if (!edge || edge->path().isEmpty()) return QPoint(-1, -1);
    const QPoint view_point = view_->mapFromScene(edge->path().pointAtPercent(0.5));
    return view_->viewport()->mapFrom(view_, view_point);
}

QWidget* RewireWindow::diagramViewport() const { return view_->viewport(); }

}  // namespace rewire
