#include "app.hpp"

#include "claude_bridge.hpp"
#include "diagram.hpp"
#include "edit.hpp"
#include "json.hpp"
#include "model.hpp"
#include "parser.hpp"
#include "util.hpp"
#include "watcher.hpp"

#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace rewire {
namespace {

namespace fs = std::filesystem;

std::string load_session_id(const std::string& project) {
    json::Value value = json::parse(read_text(project + "/.rewire/config.json"));
    if (const json::Value* id = value.find("session_id")) return id->as_string();
    return {};
}

void save_session_id(const std::string& project, const std::string& session_id) {
    fs::create_directories(project + "/.rewire");
    json::Value value;
    value.type = json::Value::Type::Object;
    json::Value id;
    id.type = json::Value::Type::String;
    id.text = session_id;
    value.object.emplace_back("session_id", std::move(id));
    write_text(project + "/.rewire/config.json", json::dump(value) + "\n");
}

bool paths_overlap(const std::string& project, const std::string& cwd) {
    if (project.empty() || cwd.empty()) return false;
    if (project == cwd) return true;
    return project.rfind(cwd + "/", 0) == 0 || cwd.rfind(project + "/", 0) == 0;
}

const ClaudeSession* find_query(const std::vector<ClaudeSession>& sessions, const std::string& query) {
    const ClaudeSession* partial = nullptr;
    for (const auto& session : sessions) {
        if (session.session_id == query || session.name == query || std::to_string(session.pid) == query) {
            return &session;
        }
        if (session.session_id.rfind(query, 0) == 0 || session.name.find(query) != std::string::npos) {
            partial = &session;
        }
    }
    return partial;
}

ftxui::Element flow_panel(const ProjectModel& model, const std::string& flow_name) {
    using namespace ftxui;
    const FunctionFlow* flow = nullptr;
    for (const auto& item : model.flows) {
        if (item.qualified_name == flow_name) flow = &item;
    }
    if (!flow) {
        return text("No function body is selected.") | dim;
    }
    Elements lines;
    lines.push_back(text(flow->qualified_name) | bold);
    lines.push_back(text(flow->file + ":" + std::to_string(flow->line)) | dim);
    lines.push_back(text(""));
    if (flow->steps.empty()) {
        lines.push_back(text("This function has no recorded steps.") | dim);
    }
    for (const auto& step : flow->steps) {
        std::string indent(static_cast<std::size_t>(std::max(0, step.depth)) * 2, ' ');
        auto line = text(indent + step.kind + "  " + step.text);
        if (step.kind == "call") line = line | color(Color::Cyan);
        else if (step.kind == "branch" || step.kind == "loop" || step.kind == "switch") line = line | color(Color::Yellow);
        else if (step.kind == "return") line = line | color(Color::Green);
        lines.push_back(line);
    }
    lines.push_back(text(""));
    lines.push_back(text("Data moving between files") | bold);
    bool any = false;
    for (const auto& call : model.calls) {
        if (call.caller != flow->qualified_name) continue;
        any = true;
        std::string row = call.caller_file + "  ->  " + (call.callee_file.empty() ? "?" : call.callee_file);
        row += "   " + short_name(call.callee) + "(" + call.args + ")";
        lines.push_back(text(row));
    }
    if (!any) lines.push_back(text("No calls were recorded for this function.") | dim);
    return vbox(std::move(lines));
}

ftxui::Element diagram_panel(const ProjectModel& model, const std::string& focus, bool show_all) {
    using namespace ftxui;
    auto rows = render_class_diagram(model, focus, show_all, 100);
    Elements lines;
    for (const auto& row : rows) {
        Element line = text(row.empty() ? " " : row);
        if (row.find("composition") != std::string::npos) line = line | color(Color::Yellow);
        else if (row.find("aggregation") != std::string::npos) line = line | color(Color::Cyan);
        else if (row.find("association") != std::string::npos) line = line | color(Color::Blue);
        else if (row.find("dependency") != std::string::npos) line = line | color(Color::Magenta);
        else if (row.find("inheritance") != std::string::npos) line = line | color(Color::Green);
        lines.push_back(line);
    }
    if (lines.empty()) lines.push_back(text(" "));
    return vbox(std::move(lines)) | yframe | flex;
}

}  // namespace

int run_dump(const std::string& project) {
    std::cout << format_model(parse_project(project));
    return 0;
}

int run_diagram(const std::string& project, const std::string& focus, bool show_all) {
    ProjectModel model = parse_project(project);
    std::string selected = focus;
    if (selected.empty() && !model.classes.empty()) selected = model.classes.front().qualified_name;
    for (const auto& line : render_class_diagram(model, selected, show_all, 100)) {
        std::cout << line << "\n";
    }
    return model.classes.empty() ? 1 : 0;
}

int run_sessions() {
    auto sessions = list_sessions();
    if (sessions.empty()) {
        std::cout << "No reachable Claude Code session.\n";
        return 0;
    }
    for (const auto& session : sessions) {
        std::cout << session.pid << "\t" << session.status << "\t" << session.name << "\t" << session.session_id
                  << "\t" << session.cwd << "\n";
    }
    return 0;
}

int run_tui(const std::string& project, const std::string& session_query) {
    if (!isatty(STDOUT_FILENO)) {
        std::cerr << "The visual interface needs a terminal. Use `rewire dump` or `rewire diagram` in a pipe.\n";
        return 1;
    }

    std::cerr << "Reading sources in " << project << "\n";
    std::atomic<bool> stop{false};
    ProjectModel model = parse_project(project, &stop);

    using namespace ftxui;
    auto screen = ScreenInteractive::Fullscreen();

    std::vector<std::string> class_entries;
    std::vector<std::string> class_ids;
    std::vector<std::string> flow_entries;
    std::vector<std::string> flow_ids;
    std::vector<std::string> action_entries = {"Add class", "Rename class", "Add field", "Add method", "Add base",
                                                "Remove member"};
    std::vector<std::string> access_labels = {"public", "protected", "private"};
    std::vector<std::string> rel_labels = {"auto", "composition", "aggregation", "association", "dependency",
                                            "inheritance"};
    std::vector<std::string> session_entries;
    std::vector<ClaudeSession> session_cache;
    std::vector<std::string> notes;
    int class_index = 0;
    int flow_index = 0;
    int action_index = 2;
    int access_index = 0;
    int rel_index = 0;
    int session_index = 0;
    int page = 0;
    int root_page = 0;
    bool show_all = false;
    bool has_session = false;
    bool waiting = false;
    ClaudeSession connected;
    std::string form_class;
    std::string form_primary;
    std::string form_secondary;
    std::string placeholder_class = "class name";
    std::string placeholder_primary = "name, signature, base, or new class";
    std::string placeholder_secondary = "field type, or optional base";

    auto note = [&](std::string text) {
        notes.push_back(std::move(text));
        if (notes.size() > 20) notes.erase(notes.begin(), notes.begin() + static_cast<std::ptrdiff_t>(notes.size() - 20));
    };

    auto selected_class = [&]() -> std::string {
        if (class_index < 0 || class_index >= static_cast<int>(class_ids.size())) return {};
        return class_ids[static_cast<std::size_t>(class_index)];
    };
    auto selected_flow = [&]() -> std::string {
        if (flow_index < 0 || flow_index >= static_cast<int>(flow_ids.size())) return {};
        return flow_ids[static_cast<std::size_t>(flow_index)];
    };

    auto rebuild = [&] {
        std::string keep_class = selected_class();
        std::string keep_flow = selected_flow();
        class_entries.clear();
        class_ids.clear();
        flow_entries.clear();
        flow_ids.clear();
        for (const auto& item : model.classes) {
            class_ids.push_back(item.qualified_name);
            class_entries.push_back(item.name + "  " + item.file);
        }
        for (const auto& item : model.flows) {
            flow_ids.push_back(item.qualified_name);
            flow_entries.push_back(short_name(item.qualified_name) + "  " + item.file);
        }
        if (class_entries.empty()) class_entries.push_back("(no classes)");
        if (flow_entries.empty()) flow_entries.push_back("(no function bodies)");
        class_index = 0;
        flow_index = 0;
        for (std::size_t i = 0; i < class_ids.size(); ++i) {
            if (class_ids[i] == keep_class) class_index = static_cast<int>(i);
        }
        for (std::size_t i = 0; i < flow_ids.size(); ++i) {
            if (flow_ids[i] == keep_flow) flow_index = static_cast<int>(i);
        }
    };
    rebuild();

    std::mutex job_mu;
    std::condition_variable job_cv;
    bool job = false;
    std::thread worker([&] {
        while (true) {
            std::unique_lock<std::mutex> lock(job_mu);
            job_cv.wait(lock, [&] { return stop.load() || job; });
            if (stop.load()) return;
            job = false;
            lock.unlock();
            ProjectModel parsed = parse_project(project, &stop);
            if (stop.load()) return;
            screen.Post([&, parsed = std::move(parsed)]() mutable {
                model = std::move(parsed);
                rebuild();
                if (waiting) {
                    waiting = false;
                    note("Code changed. The diagram now matches the sources.");
                } else {
                    note("Sources changed. Diagram refreshed.");
                }
            });
        }
    });

    auto request_parse = [&] {
        {
            std::lock_guard<std::mutex> lock(job_mu);
            job = true;
        }
        job_cv.notify_one();
        note("Reading sources again...");
    };

    ClaudeLink link;
    link.set_wake([&] { screen.PostEvent(Event::Custom); });
    bool inbox = link.start(project);
    if (inbox) note("Listening for Claude replies.");
    else note("Reply inbox is off. Sends still go to the Claude session you pick.");

    auto refresh_sessions = [&] {
        session_cache = list_sessions();
        session_entries.clear();
        for (const auto& session : session_cache) {
            session_entries.push_back(session.name + "  " + session.status + "  " + session.cwd);
        }
        if (session_entries.empty()) session_entries.push_back("(no reachable Claude Code session)");
        if (session_index >= static_cast<int>(session_entries.size())) session_index = 0;
    };

    auto adopt = [&](const ClaudeSession& session, bool persist) {
        connected = session;
        has_session = true;
        std::string line = "Connected to " + session.name + " (" + session.status + ") in " + session.cwd;
        if (!paths_overlap(project, session.cwd)) {
            line += ". That session is not in this project, so it may refuse to edit these files.";
        }
        note(line);
        if (persist) save_session_id(project, session.session_id);
    };

    refresh_sessions();
    if (!session_query.empty()) {
        if (const ClaudeSession* found = find_query(session_cache, session_query)) adopt(*found, true);
        else note("No reachable Claude session matches " + session_query + ". Press s to look again.");
    } else {
        std::string saved = load_session_id(project);
        const ClaudeSession* found = nullptr;
        if (!saved.empty()) found = find_query(session_cache, saved);
        if (!found) found = match_project(session_cache, project);
        if (found) adopt(*found, false);
        else note("No Claude Code session is open in this project. Press s to choose one, or start Claude here.");
    }

    SourceWatcher watcher(project);
    watcher.start([&] { screen.Post([&] { request_parse(); }); });

    auto class_menu = Menu(&class_entries, &class_index);
    auto flow_menu = Menu(&flow_entries, &flow_index);
    auto action_menu = Menu(&action_entries, &action_index);
    auto class_input = Input(&form_class, &placeholder_class);
    auto primary_input = Input(&form_primary, &placeholder_primary);
    auto secondary_input = Input(&form_secondary, &placeholder_secondary);
    auto access_toggle = Toggle(&access_labels, &access_index);
    auto rel_toggle = Toggle(&rel_labels, &rel_index);

    auto current_request = [&] {
        EditRequest request;
        request.action = static_cast<EditAction>(action_index);
        request.class_name = form_class.empty() ? selected_class() : form_class;
        request.primary = form_primary;
        request.secondary = form_secondary;
        request.access = access_labels[static_cast<std::size_t>(std::max(0, access_index))];
        request.relationship = rel_labels[static_cast<std::size_t>(std::max(0, rel_index))];
        return request;
    };

    auto send_edit = [&] {
        if (action_index < 0 || action_index > 5) return;
        EditRequest request = current_request();
        PreparedEdit prepared = prepare_edit(model, request);
        if (!prepared.error.empty()) {
            note(prepared.error);
            return;
        }
        if (!has_session) {
            note("Choose a Claude Code session with s before sending an edit.");
            return;
        }
        SendResult result = link.send(connected, prepared.prompt);
        if (!result.ok) {
            note(result.error.empty() ? "The edit did not reach Claude." : result.error);
            return;
        }
        waiting = true;
        note("Sent to " + connected.name + ": " + prepared.summary +
             ". Approve it in that Claude session if it asks. The diagram refreshes when the files change.");
    };
    auto send_button = Button("Send to Claude", send_edit);

    auto begin_edit = [&](int action) {
        action_index = action;
        if (form_class.empty()) form_class = selected_class();
        page = 2;
    };

    auto class_page = Container::Vertical({class_menu});
    auto flow_page = Container::Vertical({flow_menu});
    auto edit_page = Container::Vertical(
        {action_menu, class_input, primary_input, secondary_input, access_toggle, rel_toggle, send_button});
    auto pages = Container::Tab({class_page, flow_page, edit_page}, &page);

    MenuOption session_option;
    session_option.on_enter = [&] {
        if (session_index >= 0 && session_index < static_cast<int>(session_cache.size())) {
            adopt(session_cache[static_cast<std::size_t>(session_index)], true);
        }
        root_page = 0;
    };
    auto session_menu = Menu(&session_entries, &session_index, session_option);
    auto help_page = Renderer([] { return text(""); });
    auto root = Container::Tab({pages, session_menu, help_page}, &root_page);

    auto component = CatchEvent(root, [&](Event event) {
        for (auto& line : link.drain()) note(line);
        if (root_page == 1) {
            if (event == Event::Escape) {
                root_page = 0;
                return true;
            }
            if (event == Event::Character('r')) {
                refresh_sessions();
                note("Session list refreshed.");
                return true;
            }
            return false;
        }
        if (root_page == 2) {
            if (event == Event::Custom || event.is_mouse()) return true;
            root_page = 0;
            return true;
        }
        if (event == Event::Character('q') && page != 2) {
            screen.Exit();
            return true;
        }
        if (event == Event::Character('1') || (event == Event::Escape && page == 2)) {
            page = 0;
            return true;
        }
        if (event == Event::Character('2')) {
            page = 1;
            return true;
        }
        if (event == Event::Character('3') && page != 2) {
            if (form_class.empty()) form_class = selected_class();
            page = 2;
            return true;
        }
        if (page == 2) return false;
        if (event == Event::Character('?')) {
            root_page = 2;
            return true;
        }
        if (event == Event::Character('s')) {
            refresh_sessions();
            root_page = 1;
            return true;
        }
        if (event == Event::Character('r')) {
            request_parse();
            return true;
        }
        if (page == 0 && event == Event::Character('g')) {
            show_all = !show_all;
            return true;
        }
        if (page == 0 && event == Event::Character('f')) {
            begin_edit(2);
            return true;
        }
        if (page == 0 && event == Event::Character('m')) {
            begin_edit(3);
            return true;
        }
        if (page == 0 && event == Event::Character('b')) {
            begin_edit(4);
            return true;
        }
        if (page == 0 && event == Event::Character('n')) {
            begin_edit(1);
            return true;
        }
        if (page == 0 && event == Event::Character('a')) {
            begin_edit(0);
            return true;
        }
        if (page == 0 && event == Event::Character('x')) {
            begin_edit(5);
            return true;
        }
        return false;
    });

    auto view = Renderer(component, [&] {
        for (auto& line : link.drain()) note(line);
        std::string session_label = has_session ? connected.name + " (" + connected.status + ")" : "not connected";
        Element header = hbox({
            text(" Rewire ") | bold | color(Color::Cyan),
            text(project) | dim,
            text("   "),
            text(page == 0 ? "Classes" : page == 1 ? "Flow" : "Edit") | bold,
            filler(),
            text(session_label) | (has_session ? color(Color::Green) : color(Color::Red)),
            text("  "),
            text(std::to_string(model.classes.size()) + " classes  " + std::to_string(model.relations.size()) +
                 " links"),
        });

        Element body;
        if (root_page == 1) {
            Elements rows;
            rows.push_back(text("Claude Code sessions") | bold);
            rows.push_back(text("Enter connects. Esc returns. r refreshes.") | dim);
            rows.push_back(separator());
            rows.push_back(session_menu->Render() | flex);
            body = vbox(std::move(rows)) | flex;
        } else if (root_page == 2) {
            body = vbox({
                       text("Keys") | bold,
                       text("1 classes    2 control flow    3 edit the diagram"),
                       text("f add field  m add method  b add base  n rename  a add class  x remove"),
                       text("g all classes or the neighborhood of the selected class"),
                       text("s choose the Claude session    r read sources again    q quit"),
                       text(""),
                       text("Composition *  owns the part (value, unique_ptr, container of values)"),
                       text("Aggregation o  shares the part (shared_ptr)"),
                       text("Association +  non-owning pointer or reference"),
                       text("Dependency .   used in a signature, local, or call, not stored"),
                       text("Inheritance ^  base class, supertrait, or implements"),
                       text("C, C++, Java, Python, TypeScript, and Rust share this diagram."),
                       text(""),
                       text("Sending an edit posts it into the Claude session. Claude changes the files.") | dim,
                       text("When the files change, this diagram is read again.") | dim,
                       text("Press any key to close.") | dim,
                   }) |
                   flex;
        } else if (page == 2) {
            EditRequest request = current_request();
            PreparedEdit prepared = prepare_edit(model, request);
            std::string hint = prepared.error.empty() ? prepared.summary : prepared.error;
            body = vbox({
                       text("Edit the diagram. Send asks Claude to change the source.") | dim,
                       hbox({action_menu->Render() | size(WIDTH, EQUAL, 24) | border,
                             vbox({
                                 text("Class"),
                                 class_input->Render(),
                                 text("Primary"),
                                 primary_input->Render(),
                                 text("Secondary"),
                                 secondary_input->Render(),
                                 text("Access"),
                                 access_toggle->Render(),
                                 text("Relationship"),
                                 rel_toggle->Render(),
                                 send_button->Render(),
                             }) | flex |
                                 border,
                       }) | flex,
                       text(hint) | color(prepared.error.empty() ? Color::White : Color::Red),
                   }) |
                   flex;
        } else if (page == 1) {
            body = hbox({
                       flow_menu->Render() | size(WIDTH, EQUAL, 36) | border | yframe,
                       separator(),
                       flow_panel(model, selected_flow()) | flex | border | yframe,
                   }) |
                   flex;
        } else {
            body = hbox({
                       class_menu->Render() | size(WIDTH, EQUAL, 36) | border | yframe,
                       separator(),
                       diagram_panel(model, selected_class(), show_all) | flex | border,
                   }) |
                   flex;
        }

        Elements status;
        if (notes.empty()) status.push_back(text("q quit   s session   r refresh   ? help") | dim);
        std::size_t begin = notes.size() > 4 ? notes.size() - 4 : 0;
        for (std::size_t i = begin; i < notes.size(); ++i) status.push_back(text(notes[i]));
        if (!model.diagnostics.empty() && page != 2) {
            status.push_back(text(model.diagnostics.front()) | color(Color::Red));
        }
        return vbox({header, separator(), body, separator(), vbox(std::move(status))}) | flex;
    });

    screen.Loop(view);
    stop = true;
    watcher.stop();
    link.stop();
    job_cv.notify_all();
    if (worker.joinable()) worker.join();
    return 0;
}

}  // namespace rewire
