#pragma once

#include <string>

namespace rewire {

int run_dump(const std::string& project);
int run_diagram(const std::string& project, const std::string& focus, bool show_all);
int run_sessions();
int run_tui(const std::string& project, const std::string& session_query);
int run_gui(int argc, char** argv, const std::string& project, const std::string& session_query);

}  // namespace rewire
