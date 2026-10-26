#pragma once

#include <string>

namespace store {

class Author {
public:
    explicit Author(std::string name);
    const std::string& name() const;

private:
    std::string name_;
};

}  // namespace store
