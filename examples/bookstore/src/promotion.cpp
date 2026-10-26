#include "promotion.hpp"

namespace store {

Promotion::Promotion(std::string code) : code_(std::move(code)) {}

const std::string& Promotion::code() const { return code_; }

}  // namespace store
