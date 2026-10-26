#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace rewire::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double number = 0;
    std::string text;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    const Value* find(std::string_view key) const;
    std::string as_string() const;
    long long as_int() const;
};

Value parse(std::string_view input);
std::string dump(const Value& value);

}  // namespace rewire::json
