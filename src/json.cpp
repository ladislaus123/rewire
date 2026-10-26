#include "json.hpp"

#include <cctype>
#include <cmath>
#include <sstream>

namespace rewire::json {
namespace {

struct Parser {
    std::string_view in;
    std::size_t i = 0;

    void skip() {
        while (i < in.size() && std::isspace(static_cast<unsigned char>(in[i]))) {
            ++i;
        }
    }

    bool eat(char c) {
        skip();
        if (i < in.size() && in[i] == c) {
            ++i;
            return true;
        }
        return false;
    }

    Value parse_value() {
        skip();
        if (i >= in.size()) {
            return {};
        }
        char c = in[i];
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return parse_string();
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        return parse_number();
    }

    Value parse_null() {
        if (in.substr(i, 4) == "null") {
            i += 4;
        }
        return {};
    }

    Value parse_bool() {
        Value value;
        value.type = Value::Type::Bool;
        if (in.substr(i, 4) == "true") {
            value.b = true;
            i += 4;
        } else if (in.substr(i, 5) == "false") {
            i += 5;
        }
        return value;
    }

    Value parse_number() {
        Value value;
        value.type = Value::Type::Number;
        std::size_t start = i;
        if (i < in.size() && (in[i] == '-' || in[i] == '+')) {
            ++i;
        }
        while (i < in.size() && (std::isdigit(static_cast<unsigned char>(in[i])) || in[i] == '.' || in[i] == 'e' ||
                                 in[i] == 'E' || in[i] == '+' || in[i] == '-')) {
            ++i;
        }
        try {
            value.number = std::stod(std::string(in.substr(start, i - start)));
        } catch (...) {
            value.number = 0;
        }
        value.text = std::string(in.substr(start, i - start));
        return value;
    }

    Value parse_string() {
        Value value;
        value.type = Value::Type::String;
        ++i;
        std::string out;
        while (i < in.size()) {
            char c = in[i++];
            if (c == '"') {
                break;
            }
            if (c == '\\' && i < in.size()) {
                char e = in[i++];
                switch (e) {
                    case '"':
                    case '\\':
                    case '/': out.push_back(e); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u':
                        if (i + 4 <= in.size()) {
                            int code = 0;
                            for (int k = 0; k < 4; ++k) {
                                char h = in[i++];
                                code <<= 4;
                                if (h >= '0' && h <= '9') code += h - '0';
                                else if (h >= 'a' && h <= 'f') code += h - 'a' + 10;
                                else if (h >= 'A' && h <= 'F') code += h - 'A' + 10;
                            }
                            if (code < 0x80) {
                                out.push_back(static_cast<char>(code));
                            } else if (code < 0x800) {
                                out.push_back(static_cast<char>(0xc0 | (code >> 6)));
                                out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                            } else {
                                out.push_back(static_cast<char>(0xe0 | (code >> 12)));
                                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                                out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                            }
                        }
                        break;
                    default: out.push_back(e); break;
                }
            } else {
                out.push_back(c);
            }
        }
        value.text = std::move(out);
        return value;
    }

    Value parse_array() {
        Value value;
        value.type = Value::Type::Array;
        ++i;
        skip();
        if (eat(']')) {
            return value;
        }
        while (i < in.size()) {
            value.array.push_back(parse_value());
            skip();
            if (eat(']')) {
                break;
            }
            eat(',');
        }
        return value;
    }

    Value parse_object() {
        Value value;
        value.type = Value::Type::Object;
        ++i;
        skip();
        if (eat(']')) {
            return value;
        }
        if (eat('}')) {
            return value;
        }
        while (i < in.size()) {
            Value key = parse_string();
            eat(':');
            Value child = parse_value();
            value.object.emplace_back(std::move(key.text), std::move(child));
            skip();
            if (eat('}')) {
                break;
            }
            eat(',');
        }
        return value;
    }
};

void dump_into(const Value& value, std::string& out) {
    switch (value.type) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += value.b ? "true" : "false"; break;
        case Value::Type::Number:
            if (!value.text.empty() && value.text.find_first_not_of("0123456789+-") == std::string::npos) {
                out += value.text;
            } else if (std::isfinite(value.number) && std::floor(value.number) == value.number &&
                       std::abs(value.number) < 1e15) {
                out += std::to_string(static_cast<long long>(value.number));
            } else {
                out += std::to_string(value.number);
            }
            break;
        case Value::Type::String:
            out.push_back('"');
            for (unsigned char c : value.text) {
                switch (c) {
                    case '\\': out += "\\\\"; break;
                    case '"': out += "\\\""; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (c < 0x20) {
                            constexpr char hex[] = "0123456789abcdef";
                            out += "\\u00";
                            out.push_back(hex[c >> 4]);
                            out.push_back(hex[c & 0xf]);
                        } else {
                            out.push_back(static_cast<char>(c));
                        }
                }
            }
            out.push_back('"');
            break;
        case Value::Type::Array:
            out.push_back('[');
            for (std::size_t i = 0; i < value.array.size(); ++i) {
                if (i) out.push_back(',');
                dump_into(value.array[i], out);
            }
            out.push_back(']');
            break;
        case Value::Type::Object:
            out.push_back('{');
            for (std::size_t i = 0; i < value.object.size(); ++i) {
                if (i) out.push_back(',');
                out.push_back('"');
                out += value.object[i].first;
                out += "\":";
                dump_into(value.object[i].second, out);
            }
            out.push_back('}');
            break;
    }
}

}  // namespace

const Value* Value::find(std::string_view key) const {
    for (const auto& [name, child] : object) {
        if (name == key) {
            return &child;
        }
    }
    return nullptr;
}

std::string Value::as_string() const {
    if (type == Type::String) {
        return text;
    }
    if (type == Type::Number) {
        if (!text.empty()) {
            return text;
        }
        return std::to_string(static_cast<long long>(number));
    }
    if (type == Type::Bool) {
        return b ? "true" : "false";
    }
    return {};
}

long long Value::as_int() const {
    if (type == Type::Number) {
        return static_cast<long long>(number);
    }
    if (type == Type::String) {
        try {
            return std::stoll(text);
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

Value parse(std::string_view input) {
    Parser parser{input, 0};
    return parser.parse_value();
}

std::string dump(const Value& value) {
    std::string out;
    dump_into(value, out);
    return out;
}

}  // namespace rewire::json
