#pragma once

#include <string>

namespace store {

class Logger {
public:
    void info(const std::string& message) const;
    void warn(const std::string& message) const;
};

}  // namespace store
