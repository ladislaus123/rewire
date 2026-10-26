#include "logger.hpp"

#include <iostream>

namespace store {

void Logger::info(const std::string& message) const { std::cout << "info: " << message << "\n"; }

void Logger::warn(const std::string& message) const { std::cerr << "warn: " << message << "\n"; }

}  // namespace store
