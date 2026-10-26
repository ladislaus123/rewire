#pragma once

#include "model.hpp"

#include <string>

namespace rewire {

struct PreparedEdit {
    std::string summary;
    std::string prompt;
    std::string error;
};

PreparedEdit prepare_edit(const ProjectModel& model, const EditRequest& request);
std::string source_language(const std::string& file);
// Language of a file in this model. A .h is C when the project has C sources and no C++ sources.
std::string project_language(const ProjectModel& model, const std::string& file);

}  // namespace rewire
