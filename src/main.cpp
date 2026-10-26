#include "app.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void usage() {
    std::cout
        << "Rewire is a terminal harness for C, C++, Java, Python, TypeScript, and Rust.\n"
        << "It draws UML relationships and control flow from the sources in a project,\n"
        << "and sends diagram edits to a running Claude Code session.\n\n"
        << "Usage:\n"
        << "  rewire [project] [--session name|pid|id]\n"
        << "  rewire gui [project] [--session name|pid|id]\n"
        << "  rewire dump [project]\n"
        << "  rewire diagram [project] [--focus QualifiedName] [--all]\n"
        << "  rewire sessions\n\n"
        << "gui opens the Qt window. The plain command opens the terminal interface.\n\n"
        << "Inside the terminal interface:\n"
        << "  1 2 3     classes, control flow, edit\n"
        << "  j k       move in the list\n"
        << "  f m b n a x   field, method, base, rename, add class, remove\n"
        << "  g         toggle the full class map\n"
        << "  s         choose the Claude Code session\n"
        << "  r         read the sources again\n"
        << "  ?         help\n"
        << "  q         quit\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string command = "tui";
    std::string project = ".";
    std::string session;
    std::string focus;
    bool show_all = false;
    std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        }
        if (arg == "--session" && i + 1 < args.size()) {
            session = args[++i];
            continue;
        }
        if (arg == "--focus" && i + 1 < args.size()) {
            focus = args[++i];
            continue;
        }
        if (arg == "--all") {
            show_all = true;
            continue;
        }
        if (arg == "dump" || arg == "diagram" || arg == "sessions" || arg == "tui" || arg == "gui") {
            command = arg;
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option " << arg << "\n";
            usage();
            return 1;
        }
        project = arg;
    }

    if (command == "sessions") {
        return rewire::run_sessions();
    }

    std::error_code error;
    std::filesystem::path canon = std::filesystem::weakly_canonical(project, error);
    if (error || !std::filesystem::is_directory(canon)) {
        std::cerr << "Project directory not found: " << project << "\n";
        return 1;
    }
    project = canon.string();
    if (command == "dump") return rewire::run_dump(project);
    if (command == "diagram") return rewire::run_diagram(project, focus, show_all);
    if (command == "gui") return rewire::run_gui(argc, argv, project, session);
    return rewire::run_tui(project, session);
}
