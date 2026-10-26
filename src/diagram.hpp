#pragma once

#include "model.hpp"

#include <string>
#include <vector>

namespace rewire {

std::vector<std::string> render_class_diagram(const ProjectModel& model, const std::string& focus, bool show_all,
                                               int max_columns);

}  // namespace rewire
