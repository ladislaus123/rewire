#pragma once

#include "model.hpp"

#include <atomic>
#include <string>
#include <tuple>
#include <vector>

namespace rewire {

bool is_text_language_ext(const std::string& ext);

// Adds classes, methods, flows, and calls for Java, Python, TypeScript, and Rust.
// Returned triples are (from class, to class, label) dependency uses.
std::vector<std::tuple<std::string, std::string, std::string>> parse_text_sources(
    ProjectModel& model, const std::vector<std::string>& files, const std::atomic<bool>* stop);

}  // namespace rewire
