#pragma once

#include <string>

namespace store {

class Promotion {
public:
    explicit Promotion(std::string code);
    const std::string& code() const;

private:
    std::string code_;
};

}  // namespace store
