#pragma once

#include "model.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace rewire {

std::vector<std::string> list_source_files(const std::string& root);
ProjectModel parse_project(const std::string& root, const std::atomic<bool>* stop = nullptr);

}  // namespace rewire
