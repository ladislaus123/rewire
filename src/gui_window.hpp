#pragma once

#include "claude_bridge.hpp"
#include "model.hpp"
#include "watcher.hpp"

#include <QMainWindow>
#include <QPoint>
#include <QPointF>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class QComboBox;
class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTextEdit;

namespace rewire {

class DiagramView;

// Qt window for the class diagram, control flow, and Claude edits.
class RewireWindow : public QMainWindow {
    Q_OBJECT

public:
    RewireWindow(QString project, QString session_query);
    ~RewireWindow() override;

    bool waitUntilParsed(int timeout_ms);
    int nodeCount() const;
    int edgeCount() const;
    bool selectClass(const QString& qualified);
    bool focusClass(const QString& qualified);
    QString selectedClass() const;
    QString focusedClass() const;
    QString inspectorText() const;
    QString flowText() const;
    bool showFlowContaining(const QString& text);
    QPoint viewPointForClass(const QString& qualified) const;
    QPoint viewPointForEdge(const QString& from, const QString& to) const;
    QWidget* diagramViewport() const;
    void fitDiagram();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    friend class DiagramView;

    void buildUi();
    void applyStyle();
    void parserLoop();
    void requestParse();
    void applyModel(ProjectModel model);
    void rebuildClassList();
    void rebuildScene();
    void refillFlows();
    void showInspectorForClass(const QString& qualified);
    void showInspectorForEdge(const QString& from, const QString& to, const QString& kind, const QString& label);
    void syncSelection();
    void syncListTo(const QString& qualified);
    void refreshSummary();
    void beginEdit(int action);
    void sendEdit();
    void refreshSessions();
    void adoptSession(const ClaudeSession& session, bool persist);
    void loadSavedSession();
    void status(const QString& text);
    void shutdown();
    class ClassNode* findNode(const QString& qualified) const;
    class RelationEdge* findEdge(const QString& from, const QString& to) const;
    class ClassNode* makeNode(const Class& item, bool focus);
    void addEdge(class ClassNode* from, class ClassNode* to, const Relation& relation, int lane);
    QString classOfCallee(const QString& callee) const;
    EditRequest currentRequest() const;
    void updatePlaceholders();

    QString project_;
    QString session_query_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    ProjectModel model_;
    std::vector<ClaudeSession> sessions_;
    ClaudeSession connected_{};
    bool has_session_ = false;
    bool waiting_ = false;
    bool show_all_ = false;
    bool updating_ = false;
    bool parsed_ = false;
    bool laying_out_ = false;
    bool shut_ = false;
    bool refresh_from_edit_ = false;
    bool first_model_ = true;
    bool fitted_ = false;
    QString focus_id_;
    QString inbox_note_;
    std::map<QString, QPointF> pinned_;

    QGraphicsScene* scene_ = nullptr;
    DiagramView* view_ = nullptr;
    QListWidget* class_list_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* mode_combo_ = nullptr;
    QLabel* class_title_ = nullptr;
    QLabel* class_file_ = nullptr;
    QTextEdit* inspector_ = nullptr;
    QListWidget* jump_list_ = nullptr;
    QComboBox* action_combo_ = nullptr;
    QLineEdit* edit_class_ = nullptr;
    QLineEdit* edit_primary_ = nullptr;
    QLineEdit* edit_secondary_ = nullptr;
    QComboBox* access_combo_ = nullptr;
    QComboBox* relation_combo_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* send_button_ = nullptr;
    QComboBox* session_combo_ = nullptr;
    QListWidget* flow_list_ = nullptr;
    QListWidget* step_list_ = nullptr;
    QListWidget* call_list_ = nullptr;
    QLabel* counts_ = nullptr;

    ClaudeLink link_;
    SourceWatcher watcher_;
    std::mutex job_mu_;
    std::condition_variable job_cv_;
    bool job_ = false;
    std::atomic<bool> job_stop_{false};
    std::thread worker_;
};

}  // namespace rewire
