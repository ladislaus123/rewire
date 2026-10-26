#include "author.hpp"

namespace store {

Author::Author(std::string name) : name_(std::move(name)) {}

const std::string& Author::name() const { return name_; }

}  // namespace store
