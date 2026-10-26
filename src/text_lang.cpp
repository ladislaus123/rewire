#include "text_lang.hpp"

#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace rewire {
namespace {

enum class Lang { Java, Python, TypeScript, Rust };

bool is_ident_start(unsigned char c) { return std::isalpha(c) || c == '_' || c >= 128; }
bool is_ident_cont(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 128; }

std::string relative_to(const std::string& root, const std::string& file) {
    const std::string prefix = root + "/";
    if (file.rfind(prefix, 0) == 0) return file.substr(prefix.size());
    return file;
}

std::string join_sep(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].empty()) continue;
        if (!out.empty()) out += sep;
        out += parts[i];
    }
    return out;
}

std::string module_for(Lang lang, const std::string& rel) {
    std::string path = rel;
    if (lang == Lang::Rust && path.rfind("src/", 0) == 0) path.erase(0, 4);
    std::vector<std::string> parts;
    std::string current;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!current.empty()) parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) parts.push_back(current);
    if (!parts.empty()) {
        auto dot = parts.back().rfind('.');
        if (dot != std::string::npos) parts.back().erase(dot);
        const std::string& stem = parts.back();
        if (stem == "__init__" || stem == "mod" || stem == "main" || stem == "lib" || stem == "index") {
            parts.pop_back();
        }
    }
    return join_sep(parts, lang == Lang::Rust ? "::" : ".");
}

std::string qualify(const std::string& scope, const std::string& name, Lang lang) {
    if (scope.empty()) return name;
    return scope + (lang == Lang::Rust ? "::" : ".") + name;
}

bool builtin_name(const std::string& name) {
    static const std::unordered_set<std::string> names = {
        "void", "bool", "boolean", "byte", "char", "short", "int", "long", "float", "double", "string", "String",
        "str", "i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16", "u32", "u64", "u128", "usize", "f32", "f64",
        "Self", "self", "any", "unknown", "never", "object", "Object", "var", "auto", "None", "number", "bigint",
        "symbol", "undefined", "null", "Integer", "Boolean", "Double", "Float", "Long", "Short", "Byte", "Character",
        "List", "ArrayList", "LinkedList", "Map", "HashMap", "Set", "HashSet", "Optional", "Vec", "Option", "Box",
        "Rc", "Arc", "Weak", "Result", "String", "Error", "Exception", "Promise", "Array", "ReadonlyArray", "Record",
        "Partial", "Required", "Pick", "Omit", "Ref", "RefMut", "Path", "PathBuf", "Cow", "Mutex", "RwLock"};
    auto simple = name;
    auto dot = simple.rfind('.');
    auto col = simple.rfind("::");
    if (col != std::string::npos) simple = simple.substr(col + 2);
    else if (dot != std::string::npos) simple = simple.substr(dot + 1);
    return names.count(simple) != 0 || simple.empty();
}

bool boring_call(const std::string& name) {
    static const std::unordered_set<std::string> names = {
        "if", "elif", "else", "for", "while", "switch", "match", "case", "catch", "except", "try", "return", "new",
        "sizeof", "throw", "raise", "do", "super", "len", "range", "print", "println", "require", "console", "is_none",
        "is_some", "unwrap", "expect", "clone", "to_string", "to_owned", "as_ref", "iter", "into_iter", "push",
        "is_empty", "format", "assert", "isinstance", "str", "int", "float", "list", "dict", "set", "tuple", "type",
        "await", "yield", "with", "lambda", "not", "and", "or", "in", "of", "as", "from", "import", "let", "const",
        "var", "fn", "pub", "struct", "enum", "impl", "trait", "class", "interface", "this", "self", "mut", "true",
        "false", "None", "Some", "Ok", "Err"};
    return names.count(name) != 0;
}

struct Tok {
    enum Kind { End, Id, Num, Str, Sym } kind = End;
    std::string text;
    int line = 1;
};

struct Lex {
    std::vector<Tok> toks;
    std::size_t i = 0;

    const Tok& peek(std::size_t ahead = 0) const {
        std::size_t n = i + ahead;
        if (n >= toks.size()) return toks.back();
        return toks[n];
    }
    const Tok& pop() {
        if (i >= toks.size()) return toks.back();
        return toks[i++];
    }
    bool at(const char* text) const { return peek().text == text; }
    bool eat(const char* text) {
        if (!at(text)) return false;
        pop();
        return true;
    }
};

void push_tok(std::vector<Tok>& out, Tok::Kind kind, std::string text, int line) {
    Tok tok;
    tok.kind = kind;
    tok.text = std::move(text);
    tok.line = line;
    out.push_back(std::move(tok));
}

std::vector<Tok> lex_brace(const std::string& src, Lang lang) {
    std::vector<Tok> out;
    const std::size_t n = src.size();
    std::size_t i = 0;
    int line = 1;
    auto bump_line = [&](char c) {
        if (c == '\n') ++line;
    };
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(src[i]);
        if (std::isspace(c)) {
            bump_line(src[i]);
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) bump_line(src[i++]);
            if (i + 1 < n) i += 2;
            continue;
        }
        if (c == '#' && lang == Lang::Rust) {
            push_tok(out, Tok::Sym, "#", line);
            ++i;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            if (c == '\'' && lang == Lang::Rust) {
                std::size_t j = i + 1;
                if (j < n && src[j] == '\\') {
                    j += 2;
                    if (j < n && src[j] == '\'') j++;
                    push_tok(out, Tok::Str, src.substr(i, j - i), line);
                    i = j;
                    continue;
                }
                if (j < n && src[j] != '\'' && (j + 1 >= n || src[j + 1] != '\'')) {
                    ++i;
                    std::string life = "'";
                    while (i < n && is_ident_cont(static_cast<unsigned char>(src[i]))) life.push_back(src[i++]);
                    continue;
                }
            }
            char quote = src[i];
            int triples = 0;
            if (quote != '`' && i + 2 < n && src[i + 1] == quote && src[i + 2] == quote) triples = 1;
            i += triples ? 3 : 1;
            int depth = quote == '`' ? 0 : 0;
            while (i < n) {
                bump_line(src[i]);
                if (src[i] == '\\') {
                    i += std::min<std::size_t>(2, n - i);
                    continue;
                }
                if (quote == '`' && src[i] == '$' && i + 1 < n && src[i + 1] == '{') {
                    ++depth;
                    i += 2;
                    continue;
                }
                if (quote == '`' && src[i] == '}' && depth > 0) {
                    --depth;
                    ++i;
                    continue;
                }
                if (triples && i + 2 < n && src[i] == quote && src[i + 1] == quote && src[i + 2] == quote) {
                    i += 3;
                    break;
                }
                if (!triples && depth == 0 && src[i] == quote) {
                    ++i;
                    break;
                }
                ++i;
            }
            push_tok(out, Tok::Str, "\"\"", line);
            continue;
        }
        if (std::isdigit(c)) {
            std::size_t begin = i++;
            while (i < n && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_' || src[i] == '.')) ++i;
            push_tok(out, Tok::Num, src.substr(begin, i - begin), line);
            continue;
        }
        if (is_ident_start(c)) {
            std::size_t begin = i++;
            while (i < n && is_ident_cont(static_cast<unsigned char>(src[i]))) ++i;
            push_tok(out, Tok::Id, src.substr(begin, i - begin), line);
            continue;
        }
        std::string two = i + 1 < n ? src.substr(i, 2) : std::string();
        static const char* pairs[] = {"==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "->", "=>", "::",
                                       "++", "--", "<<", "?.", "??"};
        bool paired = false;
        for (const char* pair : pairs) {
            if (two == pair) {
                push_tok(out, Tok::Sym, two, line);
                i += 2;
                paired = true;
                break;
            }
        }
        if (paired) continue;
        push_tok(out, Tok::Sym, src.substr(i, 1), line);
        ++i;
    }
    push_tok(out, Tok::End, "", line);
    return out;
}

std::string join_toks(const std::vector<Tok>& toks, std::size_t begin, std::size_t end, std::size_t limit = 120) {
    std::string out;
    for (std::size_t i = begin; i < end && i < toks.size() && out.size() < limit; ++i) {
        if (toks[i].kind == Tok::End) break;
        if (!out.empty() && toks[i].text != "." && toks[i].text != "(" && toks[i].text != ")" && toks[i].text != "," &&
            toks[i].text != ";" && toks[i].text != "::" && toks[i].text != "[" && toks[i].text != "]" &&
            (i == begin || (toks[i - 1].text != "." && toks[i - 1].text != "(" && toks[i - 1].text != "::"))) {
            out.push_back(' ');
        }
        out += toks[i].text;
    }
    return out;
}

struct TypeInfo {
    std::string spelling;
    std::string core;
    enum Wrap { Plain, Collection, Owned, Shared, Borrow } wrap = Plain;
    bool ok = false;
};

bool is_java_collection(const std::string& name) {
    static const std::unordered_set<std::string> names = {"List", "ArrayList", "LinkedList", "Set", "HashSet", "TreeSet",
                                                           "Map", "HashMap", "Collection", "Iterable", "Queue", "Deque",
                                                           "Vector", "Stack", "Optional"};
    return names.count(name) != 0;
}

bool is_ts_collection(const std::string& name) {
    return name == "Array" || name == "ReadonlyArray" || name == "Set" || name == "ReadonlySet" || name == "Map" ||
           name == "ReadonlyMap" || name == "WeakSet" || name == "WeakMap" || name == "Promise" || name == "Record";
}

bool is_map_type(const std::string& name) {
    return name == "Map" || name == "HashMap" || name == "ReadonlyMap" || name == "WeakMap" || name == "Record" ||
           name == "dict" || name == "Dict" || name == "HashMap" || name == "BTreeMap";
}

struct RawField {
    Field field;
    std::string core;
    bool relate = false;
};

struct PendingCall {
    std::string receiver_type;
    std::string method;
    std::string args;
    int line = 0;
    std::size_t step = 0;
};

struct RawMethod {
    Method method;
    std::vector<FlowStep> steps;
    std::vector<PendingCall> calls;
    std::vector<std::pair<std::string, std::string>> uses;
};

struct RawClass {
    Class cls;
    Lang lang = Lang::Java;
    std::vector<std::string> bases;
    std::vector<std::string> base_labels;
    std::vector<RawField> fields;
    std::vector<RawMethod> methods;
};

struct ParseBuf {
    std::vector<RawClass> classes;
    std::string file;
    Lang lang = Lang::Java;
    std::string scope;
};

std::size_t match_brace(const std::vector<Tok>& t, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < t.size(); ++i) {
        if (t[i].text == "{") ++depth;
        else if (t[i].text == "}") {
            --depth;
            if (depth == 0) return i;
        }
    }
    return t.size() - 1;
}

std::size_t skip_angles_from(const std::vector<Tok>& t, std::size_t i) {
    if (i >= t.size() || t[i].text != "<") return i;
    int depth = 0;
    for (; i < t.size(); ++i) {
        if (t[i].text == "<") ++depth;
        else if (t[i].text == ">") {
            --depth;
            if (depth == 0) return i + 1;
        } else if (t[i].kind == Tok::End) {
            break;
        }
    }
    return i;
}

TypeInfo read_java_like_type(const std::vector<Tok>& t, std::size_t& i, bool typescript) {
    TypeInfo info;
    while (i < t.size() && t[i].text == "@") {
        ++i;
        while (i < t.size() && (t[i].kind == Tok::Id || t[i].text == ".")) ++i;
        if (i < t.size() && t[i].text == "(") {
            int depth = 0;
            for (; i < t.size(); ++i) {
                if (t[i].text == "(") ++depth;
                else if (t[i].text == ")") {
                    --depth;
                    ++i;
                    if (depth == 0) break;
                }
            }
        }
    }
    if (i >= t.size() || t[i].kind != Tok::Id) return info;
    std::size_t begin = i;
    std::string name;
    while (i < t.size() && t[i].kind == Tok::Id) {
        if (!name.empty()) name += ".";
        name += t[i].text;
        ++i;
        if (i < t.size() && t[i].text == ".") {
            ++i;
            continue;
        }
        break;
    }
    info.spelling = name;
    info.core = name;
    info.ok = true;
    if (i < t.size() && t[i].text == "<") {
        std::size_t arg = i + 1;
        std::vector<TypeInfo> args;
        while (arg < t.size() && t[arg].text != ">" && t[arg].kind != Tok::End) {
            if (t[arg].text == ",") {
                ++arg;
                continue;
            }
            TypeInfo inner = read_java_like_type(t, arg, typescript);
            if (!inner.ok) {
                if (arg < t.size() && t[arg].text != ">") ++arg;
                continue;
            }
            args.push_back(std::move(inner));
        }
        i = skip_angles_from(t, i);
        auto simple = name;
        auto dot = simple.rfind('.');
        if (dot != std::string::npos) simple = simple.substr(dot + 1);
        const TypeInfo* chosen = nullptr;
        if (is_map_type(simple) && !args.empty()) chosen = &args.back();
        else if (!args.empty()) chosen = &args.front();
        if (chosen && !chosen->core.empty()) {
            info.core = chosen->core;
            info.spelling = name + "<" + chosen->spelling + ">";
        }
        if (is_java_collection(simple) || (typescript && is_ts_collection(simple)) || simple == "Optional") {
            info.wrap = TypeInfo::Collection;
        }
    }
    if (i + 1 < t.size() && t[i].text == "[" && t[i + 1].text == "]") {
        info.wrap = TypeInfo::Collection;
        info.spelling += "[]";
        i += 2;
    }
    if (info.spelling.empty()) info.spelling = join_toks(t, begin, i);
    return info;
}

TypeInfo read_rust_type(const std::vector<Tok>& t, std::size_t& i) {
    TypeInfo info;
    bool borrow = false;
    while (i < t.size() && (t[i].text == "&" || t[i].text == "mut" || t[i].text == "*")) {
        if (t[i].text == "&" || t[i].text == "*") borrow = true;
        info.spelling += t[i].text;
        ++i;
        if (i < t.size() && t[i].kind == Tok::Id && !t[i].text.empty() && t[i].text[0] == '\'') ++i;
    }
    if (i >= t.size() || t[i].kind != Tok::Id) return info;
    std::string name;
    while (i < t.size() && t[i].kind == Tok::Id) {
        if (!name.empty()) name += "::";
        name += t[i].text;
        ++i;
        if (i < t.size() && t[i].text == "::") {
            ++i;
            continue;
        }
        break;
    }
    info.ok = true;
    info.core = name;
    info.spelling += name;
    info.wrap = borrow ? TypeInfo::Borrow : TypeInfo::Owned;
    if (i < t.size() && t[i].text == "<") {
        std::size_t arg = i + 1;
        std::vector<TypeInfo> args;
        while (arg < t.size() && t[arg].text != ">" && t[arg].kind != Tok::End) {
            if (t[arg].text == "," || (t[arg].kind == Tok::Id && arg + 1 < t.size() && t[arg + 1].text == ":")) {
                ++arg;
                continue;
            }
            TypeInfo inner = read_rust_type(t, arg);
            if (!inner.ok) {
                ++arg;
                continue;
            }
            args.push_back(std::move(inner));
        }
        i = skip_angles_from(t, i);
        auto simple = name;
        auto col = simple.rfind("::");
        if (col != std::string::npos) simple = simple.substr(col + 2);
        if (!args.empty()) {
            const TypeInfo* chosen = is_map_type(simple) ? &args.back() : &args.front();
            if (!chosen->core.empty()) info.core = chosen->core;
            info.spelling += "<" + chosen->spelling + ">";
        }
        if (simple == "Rc" || simple == "Arc") info.wrap = TypeInfo::Shared;
        else if (simple == "Weak" || simple == "Ref" || simple == "RefMut") info.wrap = TypeInfo::Borrow;
        else if (!borrow) info.wrap = TypeInfo::Owned;
    }
    return info;
}

RelationKind relation_for(Lang lang, const TypeInfo& type, bool strong, bool constructed) {
    if (!type.ok || type.core.empty() || builtin_name(type.core)) return RelationKind::Dependency;
    if (lang == Lang::Rust) {
        if (type.wrap == TypeInfo::Borrow) return RelationKind::Association;
        if (type.wrap == TypeInfo::Shared) return RelationKind::Aggregation;
        return RelationKind::Composition;
    }
    if (type.wrap == TypeInfo::Collection) return RelationKind::Aggregation;
    if (strong || constructed) return RelationKind::Composition;
    return RelationKind::Association;
}

bool modifier_word(const std::string& text, Lang lang) {
    static const std::unordered_set<std::string> java = {
        "public", "private", "protected", "static", "final", "abstract", "synchronized", "native", "strictfp",
        "transient", "volatile", "default", "sealed", "strictfp", "const"};
    static const std::unordered_set<std::string> ts = {
        "public", "private", "protected", "static", "readonly", "abstract", "async", "override", "declare", "export",
        "default", "accessor", "get", "set"};
    static const std::unordered_set<std::string> rust = {"pub", "const", "unsafe", "async", "extern", "mut", "crate",
                                                          "super", "dyn"};
    if (lang == Lang::TypeScript) return ts.count(text) || java.count(text);
    if (lang == Lang::Rust) return rust.count(text);
    return java.count(text);
}

struct MemberMods {
    std::string access = "public";
    bool strong = false;
    bool is_static = false;
    bool explicit_vis = false;
};

MemberMods take_mods(const std::vector<Tok>& t, std::size_t& i, Lang lang) {
    MemberMods mods;
    if (lang == Lang::Rust) mods.access = "private";
    while (i < t.size() && t[i].text == "@") {
        ++i;
        while (i < t.size() && (t[i].kind == Tok::Id || t[i].text == ".")) ++i;
        if (i < t.size() && t[i].text == "(") {
            int depth = 0;
            do {
                if (t[i].text == "(") ++depth;
                else if (t[i].text == ")") --depth;
                ++i;
            } while (i < t.size() && depth > 0);
        }
    }
    if (lang == Lang::Rust && i < t.size() && t[i].text == "#") {
        ++i;
        if (i < t.size() && t[i].text == "!") ++i;
        if (i < t.size() && t[i].text == "[") {
            int depth = 0;
            do {
                if (t[i].text == "[") ++depth;
                else if (t[i].text == "]") --depth;
                ++i;
            } while (i < t.size() && depth > 0);
        }
    }
    while (i < t.size() && t[i].kind == Tok::Id && modifier_word(t[i].text, lang)) {
        if (t[i].text == "private") mods.access = "private";
        else if (t[i].text == "protected") mods.access = "protected";
        else if (t[i].text == "public" || t[i].text == "pub") mods.access = "public";
        if (t[i].text == "public" || t[i].text == "private" || t[i].text == "protected" || t[i].text == "pub" ||
            t[i].text == "readonly") {
            mods.explicit_vis = true;
        }
        if (t[i].text == "final" || t[i].text == "readonly") mods.strong = true;
        if (t[i].text == "static") mods.is_static = true;
        ++i;
        if (lang == Lang::Rust && i < t.size() && t[i].text == "(") {
            int depth = 0;
            do {
                if (t[i].text == "(") ++depth;
                else if (t[i].text == ")") --depth;
                ++i;
            } while (i < t.size() && depth > 0);
        }
    }
    return mods;
}

std::string chain_before(const std::vector<Tok>& t, std::size_t name_index) {
    if (name_index == 0 || (t[name_index - 1].text != "." && t[name_index - 1].text != "::")) return {};
    std::size_t i = name_index;
    while (i > 0 && (t[i - 1].text == "." || t[i - 1].text == "::")) {
        if (i < 2 || t[i - 2].kind != Tok::Id) break;
        i -= 2;
    }
    std::string chain;
    for (std::size_t k = i; k < name_index; ++k) {
        if (t[k].text == "::") chain += ".";
        else chain += t[k].text;
    }
    auto dot = chain.rfind('.');
    if (dot != std::string::npos) chain.erase(dot);
    return chain;
}

std::string type_of_chain(const std::string& chain, const RawClass& cls, const std::unordered_map<std::string, std::string>& locals) {
    if (chain.empty()) return {};
    std::vector<std::string> parts;
    std::string current;
    for (char c : chain) {
        if (c == '.') {
            if (!current.empty()) parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) parts.push_back(current);
    if (parts.empty()) return {};
    std::size_t index = 0;
    std::string type;
    if (parts[0] == "self" || parts[0] == "this") {
        type = "@self";
        index = 1;
    } else if (locals.count(parts[0])) {
        type = locals.at(parts[0]);
        index = 1;
    } else {
        bool field = false;
        for (const auto& item : cls.fields) {
            if (item.field.name == parts[0]) {
                type = item.core;
                field = true;
                break;
            }
        }
        if (field) index = 1;
        else return parts[0];
    }
    for (; index < parts.size(); ++index) {
        if (type == "@self" || type == cls.cls.name || type == cls.cls.qualified_name) {
            bool found = false;
            for (const auto& item : cls.fields) {
                if (item.field.name == parts[index]) {
                    type = item.core;
                    found = true;
                    break;
                }
            }
            if (!found) return {};
        } else {
            return type;
        }
    }
    return type;
}

std::string expr_type(const std::vector<Tok>& t, std::size_t begin, std::size_t end, const RawClass& cls,
                      const std::unordered_map<std::string, std::string>& locals) {
    while (begin < end && (t[begin].text == "&" || t[begin].text == "mut" || t[begin].text == "*")) ++begin;
    std::string chain;
    for (std::size_t i = begin; i < end; ++i) {
        if (t[i].kind == Tok::Id) {
            if (!chain.empty()) chain.push_back('.');
            chain += t[i].text;
        } else if (t[i].text == "." || t[i].text == "::") {
            continue;
        } else {
            break;
        }
    }
    return type_of_chain(chain, cls, locals);
}

void add_step(RawMethod& method, int depth, const std::string& kind, const std::string& text, const std::string& file, int line) {
    if (method.steps.size() >= 80 || text.empty()) return;
    FlowStep step;
    step.depth = depth;
    step.kind = kind;
    step.text = text;
    step.file = file;
    step.line = line;
    method.steps.push_back(std::move(step));
}

void note_call(RawMethod& method, const RawClass& cls, const std::unordered_map<std::string, std::string>& locals,
               const std::string& receiver, const std::string& name, const std::string& args, const std::string& text,
               const std::string& file, int line, int depth) {
    if (boring_call(name) || name.empty()) return;
    add_step(method, depth, "call", text, file, line);
    PendingCall call;
    call.method = name;
    call.args = args;
    call.line = line;
    call.step = method.steps.empty() ? 0 : method.steps.size() - 1;
    if (!receiver.empty()) call.receiver_type = type_of_chain(receiver, cls, locals);
    else {
        for (const auto& existing : cls.methods) {
            if (existing.method.name == name) {
                call.receiver_type = "@self";
                break;
            }
        }
        if (call.receiver_type.empty() && method.method.name == name) call.receiver_type = "@self";
    }
    method.calls.push_back(std::move(call));
}

void scan_calls(const std::vector<Tok>& t, std::size_t begin, std::size_t end, RawMethod& method, const RawClass& cls,
                const std::unordered_map<std::string, std::string>& locals, const std::string& file, int depth) {
    for (std::size_t i = begin; i < end && i < t.size(); ++i) {
        if (t[i].kind != Tok::Id || i + 1 >= end || t[i + 1].text != "(") continue;
        if (i > begin && t[i - 1].text == "new") continue;
        std::string receiver = chain_before(t, i);
        std::size_t close = i + 1;
        int paren = 0;
        for (; close < t.size(); ++close) {
            if (t[close].text == "(") ++paren;
            else if (t[close].text == ")") {
                --paren;
                if (paren == 0) break;
            }
        }
        std::string args = join_toks(t, i + 2, close, 80);
        std::size_t from = i;
        while (from > begin && (t[from - 1].text == "." || t[from - 1].text == "::" || t[from - 1].kind == Tok::Id)) {
            if (t[from - 1].text == "." || t[from - 1].text == "::") {
                if (from < 2) break;
                from -= 2;
            } else {
                break;
            }
        }
        note_call(method, cls, locals, receiver, t[i].text, args, join_toks(t, from, std::min(close + 1, t.size())), file,
                  t[i].line, depth);
    }
}

void bind_loop(const std::vector<Tok>& t, std::size_t begin, std::size_t end, Lang lang, const RawClass& cls,
               std::unordered_map<std::string, std::string>& locals) {
    if (lang == Lang::Java) {
        for (std::size_t i = begin; i + 1 < end; ++i) {
            if (t[i].text == ":" && t[i - 1].kind == Tok::Id && i >= 2 && t[i - 2].kind == Tok::Id) {
                locals[t[i - 1].text] = t[i - 2].text;
                return;
            }
        }
        return;
    }
    const char* marker = lang == Lang::TypeScript ? "of" : "in";
    for (std::size_t i = begin; i < end; ++i) {
        if (t[i].text != marker || i == 0 || t[i - 1].kind != Tok::Id) continue;
        std::string var = t[i - 1].text;
        if (var == "const" || var == "let" || var == "var" || var == "mut") continue;
        locals[var] = expr_type(t, i + 1, end, cls, locals);
        return;
    }
}

void scan_brace_body(const std::vector<Tok>& t, std::size_t begin, std::size_t end, int depth, RawMethod& method,
                     const RawClass& cls, std::unordered_map<std::string, std::string> locals, const std::string& file,
                     Lang lang) {
    std::size_t i = begin;
    while (i < end && method.steps.size() < 80) {
        const std::size_t guard = i;
        if (t[i].text == ";") {
            ++i;
            continue;
        }
        if (t[i].text == "{") {
            std::size_t close = match_brace(t, i);
            scan_brace_body(t, i + 1, close, depth, method, cls, locals, file, lang);
            i = std::min(close + 1, end);
            continue;
        }
        const std::string& word = t[i].text;
        const bool branch = word == "if" || word == "else" || word == "switch" || word == "try" || word == "catch" ||
                            word == "match";
        const bool loop = word == "for" || word == "while" || word == "do" || word == "loop";
        if (t[i].kind == Tok::Id && (branch || loop)) {
            std::size_t header = i + 1;
            int nest = 0;
            while (header < end) {
                if (t[header].text == "(" || t[header].text == "[") ++nest;
                else if (t[header].text == ")" || t[header].text == "]") --nest;
                else if (nest == 0 && (t[header].text == "{" || t[header].text == ";")) break;
                ++header;
            }
            std::string kind = loop ? "loop" : word == "switch" || word == "match" ? "switch" : word == "try" || word == "catch" ? "try" : "branch";
            add_step(method, depth, kind, join_toks(t, i, header), file, t[i].line);
            auto body_locals = locals;
            if (loop) bind_loop(t, i, header, lang, cls, body_locals);
            if (header < end && t[header].text == "{") {
                std::size_t close = match_brace(t, header);
                scan_brace_body(t, header + 1, close, depth + 1, method, cls, body_locals, file, lang);
                i = std::min(close + 1, end);
                if (i < end && t[i].text == "else") continue;
            } else {
                i = std::min(header + 1, end);
            }
            locals = body_locals;
            continue;
        }
        if (word == "return" || word == "throw" || word == "break" || word == "continue") {
            std::size_t stop = i + 1;
            int nest = 0;
            while (stop < end) {
                if (t[stop].text == "(" || t[stop].text == "[" || t[stop].text == "{") ++nest;
                else if (t[stop].text == ")" || t[stop].text == "]" || t[stop].text == "}") {
                    if (nest == 0) break;
                    --nest;
                } else if (nest == 0 && t[stop].text == ";") break;
                ++stop;
            }
            add_step(method, depth, word == "return" ? "return" : word == "throw" ? "throw" : "jump",
                     join_toks(t, i, stop), file, t[i].line);
            scan_calls(t, i, stop, method, cls, locals, file, depth);
            i = std::min(stop + 1, end);
            continue;
        }
        std::size_t stop = i;
        int nest = 0;
        while (stop < end) {
            if (t[stop].text == "(" || t[stop].text == "[" || t[stop].text == "{") ++nest;
            else if (t[stop].text == ")" || t[stop].text == "]" || t[stop].text == "}") {
                if (nest == 0) break;
                --nest;
            } else if (nest == 0 && t[stop].text == ";") break;
            ++stop;
        }
        std::size_t cursor = i;
        MemberMods mods = take_mods(t, cursor, lang);
        (void)mods;
        if (lang == Lang::Rust && cursor < stop && t[cursor].text == "let") {
            ++cursor;
            if (cursor < stop && t[cursor].text == "mut") ++cursor;
            if (cursor < stop && t[cursor].kind == Tok::Id) {
                std::string var = t[cursor].text;
                ++cursor;
                if (cursor < stop && t[cursor].text == ":") {
                    ++cursor;
                    TypeInfo ty = read_rust_type(t, cursor);
                    if (ty.ok && !builtin_name(ty.core)) locals[var] = ty.core;
                }
            }
        } else if (lang == Lang::TypeScript && cursor < stop &&
                   (t[cursor].text == "let" || t[cursor].text == "const" || t[cursor].text == "var")) {
            ++cursor;
            if (cursor < stop && t[cursor].kind == Tok::Id) {
                std::string var = t[cursor].text;
                ++cursor;
                if (cursor < stop && t[cursor].text == ":") {
                    ++cursor;
                    TypeInfo ty = read_java_like_type(t, cursor, true);
                    if (ty.ok && !builtin_name(ty.core)) locals[var] = ty.core;
                }
            }
        } else if (lang == Lang::Java && cursor < stop && t[cursor].kind == Tok::Id) {
            TypeInfo ty = read_java_like_type(t, cursor, false);
            if (ty.ok && cursor < stop && t[cursor].kind == Tok::Id) {
                std::string var = t[cursor].text;
                ++cursor;
                if (cursor < stop && (t[cursor].text == "=" || t[cursor].text == ";")) {
                    if (!builtin_name(ty.core)) locals[var] = ty.core;
                }
            }
        }
        scan_calls(t, i, stop, method, cls, locals, file, depth);
        i = std::min(stop + (stop < end && t[stop].text == ";" ? 1 : 0), end);
        if (i == guard) ++i;
    }
}

std::size_t skip_balanced_paren(const std::vector<Tok>& t, std::size_t i) {
    if (i >= t.size() || t[i].text != "(") return i;
    int depth = 0;
    for (; i < t.size(); ++i) {
        if (t[i].text == "(") ++depth;
        else if (t[i].text == ")") {
            --depth;
            if (depth == 0) return i + 1;
        }
    }
    return i;
}

void parse_params(const std::vector<Tok>& t, std::size_t begin, std::size_t end, Lang lang, RawMethod& method,
                  std::unordered_map<std::string, std::string>& locals, bool& saw_self) {
    std::size_t i = begin;
    while (i < end) {
        if (t[i].text == "," || t[i].text == ")") {
            ++i;
            continue;
        }
        MemberMods mods = take_mods(t, i, lang);
        if (i >= end) break;
        if (lang == Lang::Rust) {
            std::size_t cursor = i;
            if (cursor < end && t[cursor].text == "&") ++cursor;
            if (cursor < end && t[cursor].text == "mut") ++cursor;
            if (cursor < end && t[cursor].text == "self") {
                saw_self = true;
                i = cursor + 1;
                continue;
            }
        }
        if (lang == Lang::TypeScript && i + 1 < end && t[i].kind == Tok::Id && t[i + 1].text == ":") {
            std::string pname = t[i].text;
            i += 2;
            TypeInfo ty = read_java_like_type(t, i, true);
            method.method.params.emplace_back(ty.spelling.empty() ? ty.core : ty.spelling, pname);
            if (!builtin_name(ty.core)) {
                locals[pname] = ty.core;
                method.uses.emplace_back(ty.core, method.method.name);
            }
            if (mods.explicit_vis && !builtin_name(ty.core)) {
                method.uses.push_back({"@paramfield:" + mods.access + ":" + (mods.strong ? "1" : "0"),
                                        pname + ":" + ty.spelling + ":" + ty.core + ":" +
                                            std::to_string(static_cast<int>(relation_for(lang, ty, mods.strong, false)))});
            }
            continue;
        }
        if (lang == Lang::TypeScript && i + 2 < end && modifier_word(t[i].text, lang)) {
            continue;
        }
        if (lang == Lang::Rust && i + 1 < end && t[i].kind == Tok::Id && t[i + 1].text == ":") {
            std::string pname = t[i].text;
            i += 2;
            TypeInfo ty = read_rust_type(t, i);
            method.method.params.emplace_back(ty.spelling.empty() ? ty.core : ty.spelling, pname);
            if (ty.ok && !builtin_name(ty.core)) {
                locals[pname] = ty.core;
                method.uses.emplace_back(ty.core, method.method.name);
            }
            continue;
        }
        TypeInfo ty = lang == Lang::Rust ? read_rust_type(t, i) : read_java_like_type(t, i, lang == Lang::TypeScript);
        if (!ty.ok || i >= end || t[i].kind != Tok::Id) {
            if (i < end) ++i;
            continue;
        }
        std::string pname = t[i].text;
        ++i;
        method.method.params.emplace_back(ty.spelling, pname);
        if (!builtin_name(ty.core)) {
            locals[pname] = ty.core;
            method.uses.emplace_back(ty.core, method.method.name);
        }
        if (lang == Lang::TypeScript && mods.explicit_vis && !builtin_name(ty.core)) {
            method.uses.push_back({"@paramfield:" + mods.access + ":" + (mods.strong ? "1" : "0"),
                                    pname + ":" + ty.spelling + ":" + ty.core + ":" +
                                        std::to_string(static_cast<int>(relation_for(lang, ty, mods.strong, false)))});
        }
    }
}

void finish_method(ParseBuf& buf, RawClass& cls, RawMethod method, const std::vector<Tok>& t, std::size_t body_open,
                   std::unordered_map<std::string, std::string> locals) {
    if (body_open < t.size() && t[body_open].text == "{") {
        std::size_t close = match_brace(t, body_open);
        scan_brace_body(t, body_open + 1, close, 0, method, cls, std::move(locals), buf.file, buf.lang);
    }
    bool exists = false;
    for (const auto& existing : cls.methods) {
        if (existing.method.signature() == method.method.signature()) exists = true;
    }
    if (!exists) cls.methods.push_back(std::move(method));
}

void add_field(RawClass& cls, RawField field) {
    if (field.field.name.empty()) return;
    for (auto& existing : cls.fields) {
        if (existing.field.name == field.field.name) {
            if (existing.core.empty() && !field.core.empty()) existing = field;
            return;
        }
    }
    cls.fields.push_back(std::move(field));
}

std::size_t parse_brace_type(ParseBuf& buf, const std::vector<Tok>& t, std::size_t i, const std::string& kind);

std::size_t parse_members(ParseBuf& buf, RawClass& cls, const std::vector<Tok>& t, std::size_t i, std::size_t end) {
    while (i < end) {
        const std::size_t guard = i;
        if (t[i].text == "}") return i + 1;
        if (t[i].text == ";") {
            ++i;
            continue;
        }
        MemberMods mods = take_mods(t, i, buf.lang);
        if (i < end && t[i].text == "<") i = skip_angles_from(t, i);
        if (i >= end) break;
        if (t[i].kind == Tok::Id && (t[i].text == "class" || t[i].text == "interface" || t[i].text == "enum" ||
                                     t[i].text == "record" || t[i].text == "struct" || t[i].text == "trait" ||
                                     t[i].text == "impl" || t[i].text == "type" || t[i].text == "fn")) {
            if (t[i].text == "class" || t[i].text == "interface" || t[i].text == "enum" || t[i].text == "record" ||
                t[i].text == "struct") {
                std::string saved = buf.scope;
                buf.scope = cls.cls.qualified_name;
                i = parse_brace_type(buf, t, i, t[i].text);
                buf.scope = saved;
                continue;
            }
            if (t[i].text == "type") {
                while (i < end && t[i].text != ";") ++i;
                continue;
            }
            if (t[i].text == "fn" || (buf.lang == Lang::Rust && t[i].text == "fn")) {
                // handled below for rust impl; fall through only for fn keyword
            } else {
                ++i;
                continue;
            }
        }
        if (buf.lang == Lang::Rust && i < end && t[i].kind == Tok::Id && t[i].text != "fn" && t[i].text != "type" &&
            t[i].text != "const" && i + 1 < end && t[i + 1].text == ":") {
            RawField field;
            field.field.name = t[i].text;
            field.field.access = mods.access;
            field.field.line = t[i].line;
            i += 2;
            TypeInfo ty = read_rust_type(t, i);
            field.field.type_spelling = ty.spelling;
            field.core = ty.core;
            field.relate = ty.ok && !builtin_name(ty.core);
            field.field.relation = relation_for(Lang::Rust, ty, false, false);
            add_field(cls, std::move(field));
            while (i < end && t[i].text != "," && t[i].text != ";" && t[i].text != "}") ++i;
            if (i < end && (t[i].text == "," || t[i].text == ";")) ++i;
            continue;
        }
        if (buf.lang == Lang::Rust && t[i].text == "fn") {
            ++i;
            if (i < end && t[i].text == "<") i = skip_angles_from(t, i);
            if (i >= end || t[i].kind != Tok::Id) continue;
            RawMethod method;
            method.method.name = t[i].text;
            method.method.access = mods.access;
            method.method.line = t[i].line;
            method.method.file = buf.file;
            method.method.is_static = false;
            ++i;
            if (i < end && t[i].text == "<") i = skip_angles_from(t, i);
            std::unordered_map<std::string, std::string> locals;
            bool saw_self = false;
            if (i < end && t[i].text == "(") {
                std::size_t close = skip_balanced_paren(t, i);
                parse_params(t, i + 1, close, Lang::Rust, method, locals, saw_self);
                i = close;
            }
            method.method.is_static = !saw_self;
            if (i < end && t[i].text == "->") {
                ++i;
                TypeInfo ret = read_rust_type(t, i);
                method.method.return_type = ret.spelling;
                if (ret.ok && !builtin_name(ret.core)) method.uses.emplace_back(ret.core, method.method.name);
            }
            while (i < end && t[i].text != "{" && t[i].text != ";") ++i;
            finish_method(buf, cls, std::move(method), t, i, std::move(locals));
            if (i < end && t[i].text == "{") i = match_brace(t, i) + 1;
            else if (i < end && t[i].text == ";") ++i;
            continue;
        }
        if (i < end && t[i].kind == Tok::Id && t[i].text == cls.cls.name && i + 1 < end && t[i + 1].text == "(") {
            RawMethod method;
            method.method.name = cls.cls.name;
            method.method.access = mods.access;
            method.method.line = t[i].line;
            method.method.file = buf.file;
            ++i;
            std::unordered_map<std::string, std::string> locals;
            bool saw_self = false;
            std::size_t close = skip_balanced_paren(t, i);
            parse_params(t, i + 1, close, buf.lang, method, locals, saw_self);
            for (const auto& use : method.uses) {
                if (use.first.rfind("@paramfield:", 0) == 0) {
                    // constructor parameter property
                    const std::string& packed = use.second;
                    auto c1 = packed.find(':');
                    auto c2 = c1 == std::string::npos ? std::string::npos : packed.find(':', c1 + 1);
                    auto c3 = c2 == std::string::npos ? std::string::npos : packed.find(':', c2 + 1);
                    if (c1 != std::string::npos && c2 != std::string::npos && c3 != std::string::npos) {
                        RawField field;
                        field.field.name = packed.substr(0, c1);
                        field.field.type_spelling = packed.substr(c1 + 1, c2 - c1 - 1);
                        field.core = packed.substr(c2 + 1, c3 - c2 - 1);
                        field.field.relation = static_cast<RelationKind>(std::atoi(packed.substr(c3 + 1).c_str()));
                        field.field.access = use.first.find("private") != std::string::npos ? "private"
                                             : use.first.find("protected") != std::string::npos ? "protected"
                                                                                                 : "public";
                        field.field.line = method.method.line;
                        field.relate = !builtin_name(field.core);
                        add_field(cls, std::move(field));
                    }
                }
            }
            method.uses.erase(std::remove_if(method.uses.begin(), method.uses.end(),
                                              [](const auto& use) { return use.first.rfind("@paramfield:", 0) == 0; }),
                              method.uses.end());
            i = close;
            while (i < end && t[i].text != "{" && t[i].text != ";") ++i;
            finish_method(buf, cls, std::move(method), t, i, std::move(locals));
            if (i < end && t[i].text == "{") i = match_brace(t, i) + 1;
            continue;
        }
        if (buf.lang == Lang::TypeScript && i < end && t[i].kind == Tok::Id) {
            std::size_t look = i;
            std::string member = t[look].text;
            ++look;
            if (look < end && t[look].text == "<") look = skip_angles_from(t, look);
            if (look < end && t[look].text == "(") {
                RawMethod method;
                method.method.name = member;
                method.method.access = mods.access;
                method.method.is_static = mods.is_static;
                method.method.line = t[i].line;
                method.method.file = buf.file;
                std::unordered_map<std::string, std::string> locals;
                bool saw_self = false;
                std::size_t close = skip_balanced_paren(t, look);
                parse_params(t, look + 1, close, Lang::TypeScript, method, locals, saw_self);
                i = close;
                if (i < end && t[i].text == ":") {
                    ++i;
                    TypeInfo ret = read_java_like_type(t, i, true);
                    method.method.return_type = ret.spelling;
                    if (ret.ok && !builtin_name(ret.core)) method.uses.emplace_back(ret.core, method.method.name);
                }
                while (i < end && t[i].text != "{" && t[i].text != ";" && t[i].text != "=") ++i;
                if (i < end && t[i].text == "=") {
                    while (i < end && t[i].text != "{" && t[i].text != ";") ++i;
                }
                for (const auto& use : method.uses) {
                    if (use.first.rfind("@paramfield:", 0) != 0) continue;
                    const std::string& packed = use.second;
                    auto c1 = packed.find(':');
                    auto c2 = packed.find(':', c1 == std::string::npos ? 0 : c1 + 1);
                    auto c3 = packed.find(':', c2 == std::string::npos ? 0 : c2 + 1);
                    if (c1 == std::string::npos || c2 == std::string::npos || c3 == std::string::npos) continue;
                    RawField field;
                    field.field.name = packed.substr(0, c1);
                    field.field.type_spelling = packed.substr(c1 + 1, c2 - c1 - 1);
                    field.core = packed.substr(c2 + 1, c3 - c2 - 1);
                    field.field.relation = static_cast<RelationKind>(std::atoi(packed.substr(c3 + 1).c_str()));
                    field.field.access = mods.access == "public" && use.first.find("private") != std::string::npos
                                             ? "private"
                                         : use.first.find("protected") != std::string::npos ? "protected"
                                                                                             : mods.access;
                    if (use.first.find("private") != std::string::npos) field.field.access = "private";
                    if (use.first.find("protected") != std::string::npos) field.field.access = "protected";
                    field.field.line = method.method.line;
                    field.relate = !builtin_name(field.core);
                    add_field(cls, std::move(field));
                }
                method.uses.erase(std::remove_if(method.uses.begin(), method.uses.end(),
                                                  [](const auto& use) { return use.first.rfind("@paramfield:", 0) == 0; }),
                                  method.uses.end());
                finish_method(buf, cls, std::move(method), t, i, std::move(locals));
                if (i < end && t[i].text == "{") i = match_brace(t, i) + 1;
                else if (i < end && t[i].text == ";") ++i;
                continue;
            }
            if (look < end && t[look].text == ":") {
                TypeInfo ty = read_java_like_type(t, ++look, true);
                bool constructed = false;
                std::size_t cursor = look;
                if (cursor < end && t[cursor].text == "=") {
                    ++cursor;
                    if (cursor < end && t[cursor].text == "new") constructed = true;
                }
                RawField field;
                field.field.name = member;
                field.field.type_spelling = ty.spelling;
                field.field.access = mods.access;
                field.field.line = t[i].line;
                field.core = ty.core;
                field.relate = ty.ok && !builtin_name(ty.core);
                field.field.relation = relation_for(Lang::TypeScript, ty, mods.strong, constructed);
                add_field(cls, std::move(field));
                while (i < end && t[i].text != ";" && t[i].text != "}") ++i;
                if (i < end && t[i].text == ";") ++i;
                continue;
            }
        }
        if ((buf.lang == Lang::Java) && i < end && t[i].kind == Tok::Id) {
            TypeInfo ty = read_java_like_type(t, i, false);
            if (!ty.ok || i >= end) {
                if (i < end) ++i;
                continue;
            }
            if (t[i].text == "(") {
                // constructor already handled; skip
                i = skip_balanced_paren(t, i);
                if (i < end && t[i].text == "{") i = match_brace(t, i) + 1;
                continue;
            }
            if (t[i].kind != Tok::Id) {
                ++i;
                continue;
            }
            std::string member = t[i].text;
            int line = t[i].line;
            ++i;
            if (i < end && t[i].text == "<") i = skip_angles_from(t, i);
            if (i < end && t[i].text == "(") {
                RawMethod method;
                method.method.name = member;
                method.method.access = mods.access;
                method.method.is_static = mods.is_static;
                method.method.return_type = ty.spelling;
                method.method.line = line;
                method.method.file = buf.file;
                if (!builtin_name(ty.core)) method.uses.emplace_back(ty.core, member);
                std::unordered_map<std::string, std::string> locals;
                bool saw_self = false;
                std::size_t close = skip_balanced_paren(t, i);
                parse_params(t, i + 1, close, Lang::Java, method, locals, saw_self);
                i = close;
                while (i < end && t[i].text != "{" && t[i].text != ";") ++i;
                finish_method(buf, cls, std::move(method), t, i, std::move(locals));
                if (i < end && t[i].text == "{") i = match_brace(t, i) + 1;
                else if (i < end && t[i].text == ";") ++i;
                continue;
            }
            RawField field;
            field.field.name = member;
            field.field.type_spelling = ty.spelling;
            field.field.access = mods.access;
            field.field.line = line;
            field.core = ty.core;
            field.relate = ty.ok && !builtin_name(ty.core);
            field.field.relation = relation_for(Lang::Java, ty, mods.strong, false);
            add_field(cls, std::move(field));
            while (i < end && t[i].text != ";" && t[i].text != "}") ++i;
            if (i < end && t[i].text == ";") ++i;
            continue;
        }
        if (i == guard) ++i;
    }
    return i;
}

std::vector<std::string> read_bases(const std::vector<Tok>& t, std::size_t& i, const char* keyword) {
    std::vector<std::string> bases;
    if (!keyword || i >= t.size() || t[i].text != keyword) return bases;
    ++i;
    std::string current;
    int depth = 0;
    for (; i < t.size(); ++i) {
        if (t[i].text == "<") ++depth;
        else if (t[i].text == ">") {
            if (depth) --depth;
        } else if (depth == 0 && (t[i].text == "{" || t[i].text == "implements" || t[i].text == "where" ||
                                  t[i].text == "(" || t[i].text == ";")) {
            break;
        } else if (depth == 0 && t[i].kind == Tok::Id) {
            if (!current.empty() && current.back() != '.' && current.back() != ':') current += ".";
            current += t[i].text;
        } else if (depth == 0 && t[i].text == "::") {
            if (!current.empty() && current.back() != ':') current += "::";
        } else if (depth == 0 && t[i].text == ".") {
            if (!current.empty() && current.back() != '.') current.push_back('.');
        } else if (depth == 0 && t[i].text == ",") {
            if (!current.empty()) bases.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) bases.push_back(current);
    return bases;
}

std::size_t parse_brace_type(ParseBuf& buf, const std::vector<Tok>& t, std::size_t i, const std::string& kind) {
    int line = i < t.size() ? t[i].line : 1;
    ++i;
    if (i < t.size() && t[i].text == "<") i = skip_angles_from(t, i);
    if (i >= t.size() || t[i].kind != Tok::Id) return i;
    RawClass cls;
    cls.lang = buf.lang;
    cls.cls.name = t[i].text;
    cls.cls.line = line;
    cls.cls.file = buf.file;
    cls.cls.qualified_name = qualify(buf.scope, cls.cls.name, buf.lang);
    cls.cls.is_struct = kind == "struct" || kind == "record" || kind == "interface";
    ++i;
    if (i < t.size() && t[i].text == "<") i = skip_angles_from(t, i);
    if (kind == "record" && i < t.size() && t[i].text == "(") {
        std::size_t close = skip_balanced_paren(t, i);
        std::size_t cursor = i + 1;
        while (cursor < close) {
            MemberMods mods = take_mods(t, cursor, buf.lang);
            TypeInfo ty = read_java_like_type(t, cursor, false);
            if (!ty.ok || cursor >= close || t[cursor].kind != Tok::Id) {
                if (cursor < close) ++cursor;
                continue;
            }
            RawField field;
            field.field.name = t[cursor].text;
            field.field.type_spelling = ty.spelling;
            field.field.access = mods.access;
            field.field.line = t[cursor].line;
            field.field.relation = RelationKind::Composition;
            field.core = ty.core;
            field.relate = true;
            add_field(cls, std::move(field));
            ++cursor;
        }
        i = close;
    }
    auto extends = read_bases(t, i, "extends");
    for (const auto& base : extends) {
        cls.bases.push_back(base);
        cls.base_labels.push_back(kind == "interface" ? "extends" : "extends");
    }
    auto implements = read_bases(t, i, "implements");
    for (const auto& base : implements) {
        cls.bases.push_back(base);
        cls.base_labels.push_back("implements");
    }
    if (i < t.size() && t[i].text == "where") {
        while (i < t.size() && t[i].text != "{" && t[i].text != ";") ++i;
    }
    if (kind == "enum") {
        if (i < t.size() && t[i].text == "{") i = match_brace(t, i) + 1;
        buf.classes.push_back(std::move(cls));
        return i;
    }
    if (i < t.size() && t[i].text == "{") {
        std::size_t close = match_brace(t, i);
        parse_members(buf, cls, t, i + 1, close);
        i = close + 1;
    }
    buf.classes.push_back(std::move(cls));
    return i;
}

void parse_rust_impl(ParseBuf& buf, const std::vector<Tok>& t, std::size_t& i) {
    int line = t[i].line;
    ++i;
    if (i < t.size() && t[i].text == "<") i = skip_angles_from(t, i);
    std::string first;
    std::size_t name_at = i;
    while (i < t.size() && t[i].text != "{" && t[i].text != "for" && t[i].text != "where" && t[i].kind != Tok::End) {
        if (t[i].kind == Tok::Id) {
            if (!first.empty()) first += "::";
            first += t[i].text;
        }
        ++i;
        if (i < t.size() && t[i].text == "<") i = skip_angles_from(t, i);
    }
    std::string target = first;
    std::string trait;
    if (i < t.size() && t[i].text == "for") {
        trait = first;
        ++i;
        target.clear();
        while (i < t.size() && t[i].text != "{" && t[i].text != "where" && t[i].kind != Tok::End) {
            if (t[i].kind == Tok::Id) {
                if (!target.empty()) target += "::";
                target += t[i].text;
            }
            ++i;
            if (i < t.size() && t[i].text == "<") i = skip_angles_from(t, i);
        }
    }
    while (i < t.size() && t[i].text != "{" && t[i].text != ";") ++i;
    if (target.empty()) return;
    RawClass* owner = nullptr;
    for (auto& cls : buf.classes) {
        if (cls.cls.name == target || cls.cls.qualified_name == target ||
            cls.cls.qualified_name == qualify(buf.scope, target, Lang::Rust)) {
            owner = &cls;
            break;
        }
    }
    RawClass created;
    bool made = false;
    if (!owner) {
        created.lang = Lang::Rust;
        auto simple = target;
        auto col = simple.rfind("::");
        created.cls.name = col == std::string::npos ? simple : simple.substr(col + 2);
        created.cls.qualified_name = target.find("::") == std::string::npos ? qualify(buf.scope, target, Lang::Rust) : target;
        created.cls.file = buf.file;
        created.cls.line = line;
        created.cls.is_struct = true;
        owner = &created;
        made = true;
    }
    if (!trait.empty()) {
        owner->bases.push_back(trait);
        owner->base_labels.push_back("implements");
    }
    if (i < t.size() && t[i].text == "{") {
        std::size_t close = match_brace(t, i);
        parse_members(buf, *owner, t, i + 1, close);
        i = close + 1;
    }
    (void)name_at;
    if (made) buf.classes.push_back(std::move(created));
}

void parse_java_ts_rust(ParseBuf& buf, const std::string& text) {
    auto toks = lex_brace(text, buf.lang);
    std::size_t i = 0;
    if (buf.lang == Lang::Java) {
        while (i < toks.size() && toks[i].kind != Tok::End) {
            if (toks[i].text == "package") {
                ++i;
                buf.scope.clear();
                while (i < toks.size() && toks[i].text != ";") {
                    if (toks[i].kind == Tok::Id || toks[i].text == ".") buf.scope += toks[i].text;
                    ++i;
                }
            } else {
                break;
            }
            if (i < toks.size() && toks[i].text == ";") ++i;
            break;
        }
    }
    while (i < toks.size() && toks[i].kind != Tok::End) {
        const std::size_t guard = i;
        if (toks[i].text == "import" || toks[i].text == "use" || toks[i].text == "from") {
            while (i < toks.size() && toks[i].text != ";") ++i;
            if (i < toks.size()) ++i;
            continue;
        }
        MemberMods mods = take_mods(toks, i, buf.lang);
        (void)mods;
        if (i >= toks.size()) break;
        if (buf.lang == Lang::Rust && toks[i].text == "impl") {
            parse_rust_impl(buf, toks, i);
            continue;
        }
        if (toks[i].kind == Tok::Id && (toks[i].text == "class" || toks[i].text == "interface" || toks[i].text == "enum" ||
                                        toks[i].text == "record" || toks[i].text == "struct" || toks[i].text == "trait")) {
            i = parse_brace_type(buf, toks, i, toks[i].text);
            continue;
        }
        if (buf.lang == Lang::Rust && toks[i].text == "fn") {
            RawClass* free = nullptr;
            for (auto& cls : buf.classes) {
                if (cls.cls.name == "@file") free = &cls;
            }
            if (!free) {
                RawClass holder;
                holder.lang = Lang::Rust;
                holder.cls.name = module_for(Lang::Rust, buf.file);
                if (holder.cls.name.empty()) holder.cls.name = "module";
                holder.cls.qualified_name = holder.cls.name;
                holder.cls.file = buf.file;
                holder.cls.line = toks[i].line;
                buf.classes.push_back(std::move(holder));
                free = &buf.classes.back();
            }
            parse_members(buf, *free, toks, i, toks.size() - 1);
            break;
        }
        if (i == guard) ++i;
    }
}

struct PyLine {
    int indent = 0;
    int line = 1;
    std::string text;
};

std::vector<PyLine> python_lines(const std::string& src) {
    std::vector<PyLine> lines;
    const std::size_t n = src.size();
    std::size_t i = 0;
    int line_no = 1;
    while (i < n) {
        int indent = 0;
        while (i < n && (src[i] == ' ' || src[i] == '\t')) {
            indent += src[i] == '\t' ? 4 : 1;
            ++i;
        }
        if (i < n && src[i] == '#') {
            while (i < n && src[i] != '\n') ++i;
        }
        if (i < n && src[i] == '\n') {
            ++line_no;
            ++i;
            continue;
        }
        if (i >= n) break;
        PyLine line;
        line.indent = indent;
        line.line = line_no;
        int paren = 0;
        bool quote = false;
        char q = 0;
        int triple = 0;
        while (i < n) {
            char c = src[i];
            if (quote) {
                line.text.push_back(c == '\n' ? ' ' : c);
                if (c == '\\') {
                    if (i + 1 < n) line.text.push_back(src[++i]);
                } else if (triple && i + 2 < n && src[i] == q && src[i + 1] == q && src[i + 2] == q) {
                    line.text.push_back(src[i + 1]);
                    line.text.push_back(src[i + 2]);
                    i += 2;
                    quote = false;
                } else if (!triple && c == q) {
                    quote = false;
                }
                if (c == '\n') ++line_no;
                ++i;
                continue;
            }
            if (c == '#' && paren == 0) {
                while (i < n && src[i] != '\n') ++i;
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = true;
                q = c;
                triple = (i + 2 < n && src[i + 1] == q && src[i + 2] == q) ? 1 : 0;
                line.text.push_back(c);
                ++i;
                if (triple) {
                    line.text.push_back(q);
                    line.text.push_back(q);
                    i += 2;
                }
                continue;
            }
            if (c == '(' || c == '[' || c == '{') ++paren;
            else if (c == ')' || c == ']' || c == '}') --paren;
            if (c == '\n') {
                ++line_no;
                ++i;
                if (paren <= 0 && (line.text.empty() || line.text.back() != '\\')) break;
                if (!line.text.empty() && line.text.back() == '\\') line.text.pop_back();
                line.text.push_back(' ');
                continue;
            }
            line.text.push_back(c);
            ++i;
        }
        line.text = trim_copy(line.text);
        if (!line.text.empty()) lines.push_back(std::move(line));
    }
    return lines;
}

struct PyType {
    std::string spelling;
    std::string core;
    bool collection = false;
};

PyType parse_py_type(std::string text) {
    PyType type;
    text = trim_copy(text);
    if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') || (text.front() == '\'' && text.back() == '\''))) {
        text = text.substr(1, text.size() - 2);
    }
    type.spelling = text;
    auto bracket = text.find('[');
    std::string head = bracket == std::string::npos ? text : text.substr(0, bracket);
    head = trim_copy(head);
    auto simple = head;
    auto dot = simple.rfind('.');
    if (dot != std::string::npos) simple = simple.substr(dot + 1);
    std::string inner = bracket == std::string::npos ? "" : text.substr(bracket + 1);
    if (!inner.empty() && inner.back() == ']') inner.pop_back();
    if (simple == "list" || simple == "List" || simple == "set" || simple == "Set" || simple == "Sequence" ||
        simple == "Iterable" || simple == "tuple" || simple == "Tuple") {
        type.collection = true;
        type.core = trim_copy(inner);
    } else if (simple == "dict" || simple == "Dict" || simple == "Mapping") {
        type.collection = true;
        auto comma = inner.rfind(',');
        type.core = trim_copy(comma == std::string::npos ? inner : inner.substr(comma + 1));
    } else if (simple == "Optional") {
        type.core = trim_copy(inner);
    } else {
        type.core = head;
    }
    auto generic = type.core.find('[');
    if (generic != std::string::npos) type.core = parse_py_type(type.core).core;
    return type;
}

struct FoundCall {
    std::string receiver;
    std::string name;
    std::string text;
};

std::vector<FoundCall> calls_in_text(const std::string& text) {
    std::vector<FoundCall> calls;
    std::string chain;
    bool dot = false;
    for (std::size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\'') {
            char q = text[i++];
            int triple = (i + 1 < text.size() && text[i] == q && text[i + 1] == q) ? 1 : 0;
            if (triple) i += 2;
            while (i < text.size()) {
                if (text[i] == '\\') {
                    i += std::min<std::size_t>(2, text.size() - i);
                    continue;
                }
                if (triple && i + 2 < text.size() && text[i] == q && text[i + 1] == q && text[i + 2] == q) {
                    i += 3;
                    break;
                }
                if (!triple && text[i] == q) {
                    ++i;
                    break;
                }
                ++i;
            }
            chain.clear();
            dot = false;
            continue;
        }
        if (is_ident_start(c)) {
            std::size_t begin = i++;
            while (i < text.size() && is_ident_cont(static_cast<unsigned char>(text[i]))) ++i;
            std::string ident = text.substr(begin, i - begin);
            if (dot) chain += "." + ident;
            else chain = ident;
            dot = false;
            continue;
        }
        if (c == '.' ) {
            dot = !chain.empty();
            ++i;
            continue;
        }
        if (c == '(' && !chain.empty()) {
            auto dotpos = chain.rfind('.');
            FoundCall call;
            if (dotpos == std::string::npos) call.name = chain;
            else {
                call.receiver = chain.substr(0, dotpos);
                call.name = chain.substr(dotpos + 1);
            }
            std::size_t close = i;
            int depth = 0;
            for (; close < text.size(); ++close) {
                if (text[close] == '(') ++depth;
                else if (text[close] == ')') {
                    --depth;
                    if (depth == 0) break;
                }
            }
            call.text = chain + text.substr(i, std::min<std::size_t>(close + 1, text.size()) - i);
            if (call.text.size() > 120) call.text.resize(120);
            if (!boring_call(call.name)) calls.push_back(std::move(call));
            chain.clear();
            dot = false;
            ++i;
            continue;
        }
        chain.clear();
        dot = false;
        ++i;
    }
    return calls;
}

std::string py_chain_type(const std::string& chain, const RawClass& cls, const std::unordered_map<std::string, std::string>& locals) {
    return type_of_chain(chain, cls, locals);
}

std::size_t parse_python_block(ParseBuf& buf, const std::vector<PyLine>& lines, std::size_t index, int min_indent, RawClass* owner);

std::size_t parse_python_method(ParseBuf& buf, const std::vector<PyLine>& lines, std::size_t index, RawClass& cls) {
    const PyLine& header = lines[index];
    std::string text = header.text;
    if (text.rfind("async ", 0) == 0) text = trim_copy(text.substr(6));
    if (text.rfind("def ", 0) != 0) return index + 1;
    text = text.substr(4);
    auto paren = text.find('(');
    if (paren == std::string::npos) return index + 1;
    RawMethod method;
    method.method.name = trim_copy(text.substr(0, paren));
    method.method.access = "public";
    method.method.file = buf.file;
    method.method.line = header.line;
    auto ret = text.rfind("->");
    if (ret != std::string::npos) {
        auto colon = text.find(':', ret);
        method.method.return_type = trim_copy(text.substr(ret + 2, (colon == std::string::npos ? text.size() : colon) - (ret + 2)));
        PyType rtype = parse_py_type(method.method.return_type);
        if (!builtin_name(rtype.core)) method.uses.emplace_back(rtype.core, method.method.name);
    }
    auto close = text.rfind(')');
    std::string params = close == std::string::npos ? "" : text.substr(paren + 1, close - paren - 1);
    std::unordered_map<std::string, std::string> locals;
    std::string current;
    int depth = 0;
    auto flush_param = [&](std::string piece) {
        piece = trim_copy(piece);
        if (piece.empty() || piece == "self" || piece == "cls" || piece[0] == '*') return;
        auto colon = piece.find(':');
        auto eq = piece.find('=');
        std::string pname = trim_copy(piece.substr(0, colon == std::string::npos ? eq : colon));
        std::string ptype;
        if (colon != std::string::npos) {
            std::size_t stop = eq == std::string::npos || eq < colon ? piece.size() : eq;
            ptype = trim_copy(piece.substr(colon + 1, stop - colon - 1));
        }
        if (pname.empty()) return;
        method.method.params.emplace_back(ptype, pname);
        PyType ty = parse_py_type(ptype);
        if (!builtin_name(ty.core)) {
            locals[pname] = ty.core;
            method.uses.emplace_back(ty.core, method.method.name);
        }
    };
    for (char c : params) {
        if (c == '[' || c == '(' || c == '{') ++depth;
        else if (c == ']' || c == ')' || c == '}') --depth;
        else if (c == ',' && depth == 0) {
            flush_param(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    flush_param(current);
    int body_indent = header.indent;
    std::size_t cursor = index + 1;
    if (cursor < lines.size() && lines[cursor].indent > header.indent) body_indent = lines[cursor].indent;
    while (cursor < lines.size() && lines[cursor].indent >= body_indent && method.steps.size() < 80) {
        const PyLine& line = lines[cursor];
        std::string body = line.text;
        int depth_steps = std::max(0, (line.indent - body_indent) / 4);
        auto starts = [&](const char* word) {
            std::string key = word;
            return body.rfind(key, 0) == 0 && (body.size() == key.size() || !is_ident_cont(static_cast<unsigned char>(body[key.size()])));
        };
        if (starts("if") || starts("elif") || starts("else") || starts("while") || starts("for") || starts("try") ||
            starts("except") || starts("with") || starts("match") || starts("case")) {
            std::string kind = (starts("for") || starts("while")) ? "loop" : starts("try") || starts("except") ? "try" : "branch";
            add_step(method, depth_steps, kind, body, buf.file, line.line);
            if (starts("for")) {
                auto name_end = body.find(' ', 4);
                std::string var = name_end == std::string::npos ? "" : trim_copy(body.substr(4, name_end - 4));
                auto in_at = body.find(" in ");
                if (!var.empty() && in_at != std::string::npos) {
                    std::string expr = trim_copy(body.substr(in_at + 4));
                    if (!expr.empty() && expr.back() == ':') expr.pop_back();
                    locals[var] = py_chain_type(trim_copy(expr), cls, locals);
                }
            }
        } else if (starts("return") || starts("raise") || starts("break") || starts("continue")) {
            add_step(method, depth_steps, starts("return") ? "return" : starts("raise") ? "throw" : "jump", body, buf.file, line.line);
        }
        if (body.rfind("self.", 0) == 0 || body.find("self.") != std::string::npos) {
            auto self_at = body.find("self.");
            if (self_at != std::string::npos) {
                std::size_t name_at = self_at + 5;
                std::size_t name_end = name_at;
                while (name_end < body.size() && is_ident_cont(static_cast<unsigned char>(body[name_end]))) ++name_end;
                std::string fname = body.substr(name_at, name_end - name_at);
                std::string after = trim_copy(body.substr(name_end));
                if (!fname.empty() && !after.empty() && after[0] == ':') {
                    auto eq = after.find('=');
                    PyType ty = parse_py_type(trim_copy(after.substr(1, eq == std::string::npos ? std::string::npos : eq - 1)));
                    RawField field;
                    field.field.name = fname;
                    field.field.type_spelling = ty.spelling;
                    field.field.access = "public";
                    field.field.line = line.line;
                    field.core = ty.core;
                    field.relate = !builtin_name(ty.core);
                    field.field.relation = ty.collection ? RelationKind::Aggregation : RelationKind::Association;
                    add_field(cls, std::move(field));
                } else if (!fname.empty() && !after.empty() && after[0] == '=') {
                    std::string rhs = trim_copy(after.substr(1));
                    bool constructed = false;
                    std::string constructed_type;
                    for (const auto& call : calls_in_text(rhs)) {
                        if (call.receiver.empty()) {
                            constructed = true;
                            constructed_type = call.name;
                            break;
                        }
                    }
                    std::string core;
                    RelationKind rel = RelationKind::Association;
                    if (constructed && !builtin_name(constructed_type)) {
                        core = constructed_type;
                        rel = RelationKind::Composition;
                    } else {
                        std::string ident = rhs;
                        auto space = ident.find_first_of(" .([");
                        if (space != std::string::npos) ident = ident.substr(0, space);
                        if (locals.count(ident)) core = locals[ident];
                    }
                    if (!core.empty() && !builtin_name(core)) {
                        RawField field;
                        field.field.name = fname;
                        field.field.type_spelling = core;
                        field.field.access = "public";
                        field.field.line = line.line;
                        field.core = core;
                        field.relate = true;
                        field.field.relation = rel;
                        add_field(cls, std::move(field));
                    }
                }
            }
        }
        for (const auto& call : calls_in_text(body)) {
            std::unordered_map<std::string, std::string> view = locals;
            note_call(method, cls, view, call.receiver, call.name, "", call.text, buf.file, line.line, depth_steps);
        }
        ++cursor;
    }
    bool exists = false;
    for (const auto& existing : cls.methods) {
        if (existing.method.name == method.method.name && existing.method.params.size() == method.method.params.size()) {
            exists = true;
        }
    }
    if (!exists && !method.method.name.empty()) cls.methods.push_back(std::move(method));
    return cursor;
}

std::size_t parse_python_class(ParseBuf& buf, const std::vector<PyLine>& lines, std::size_t index) {
    const PyLine& header = lines[index];
    std::string text = header.text.substr(6);
    std::string name;
    std::size_t k = 0;
    while (k < text.size() && is_ident_cont(static_cast<unsigned char>(text[k]))) name.push_back(text[k++]);
    RawClass cls;
    cls.lang = Lang::Python;
    cls.cls.name = name;
    cls.cls.qualified_name = qualify(buf.scope, name, Lang::Python);
    cls.cls.file = buf.file;
    cls.cls.line = header.line;
    if (k < text.size() && text[k] == '(') {
        int depth = 0;
        std::string current;
        for (; k < text.size(); ++k) {
            char c = text[k];
            if (c == '(') {
                ++depth;
                if (depth == 1) continue;
            } else if (c == ')') {
                --depth;
                if (depth == 0) break;
            }
            if (depth == 1 && c == ',') {
                current = trim_copy(current);
                if (!current.empty()) {
                    cls.bases.push_back(current);
                    cls.base_labels.emplace_back("extends");
                }
                current.clear();
            } else if (depth >= 1 && c != ':') {
                current.push_back(c);
            }
        }
        current = trim_copy(current);
        if (!current.empty()) {
            cls.bases.push_back(current);
            cls.base_labels.emplace_back("extends");
        }
    }
    std::size_t cursor = index + 1;
    int body_indent = header.indent + 4;
    if (cursor < lines.size() && lines[cursor].indent > header.indent) body_indent = lines[cursor].indent;
    while (cursor < lines.size() && lines[cursor].indent >= body_indent) {
        const std::string& body = lines[cursor].text;
        if (body.rfind("def ", 0) == 0 || body.rfind("async def ", 0) == 0) {
            cursor = parse_python_method(buf, lines, cursor, cls);
            continue;
        }
        if (body.rfind("class ", 0) == 0) {
            std::string saved = buf.scope;
            buf.scope = cls.cls.qualified_name;
            cursor = parse_python_class(buf, lines, cursor);
            buf.scope = saved;
            continue;
        }
        auto colon = body.find(':');
        if (colon != std::string::npos && body.find('(') == std::string::npos) {
            std::string fname = trim_copy(body.substr(0, colon));
            if (fname.rfind("self.", 0) == 0) fname = fname.substr(5);
            bool name_ok = !fname.empty();
            for (char c : fname) {
                if (!is_ident_cont(static_cast<unsigned char>(c))) name_ok = false;
            }
            if (name_ok) {
                auto eq = body.find('=', colon);
                PyType ty = parse_py_type(trim_copy(body.substr(colon + 1, (eq == std::string::npos ? body.size() : eq) - colon - 1)));
                RawField field;
                field.field.name = fname;
                field.field.type_spelling = ty.spelling;
                field.field.access = "public";
                field.field.line = lines[cursor].line;
                field.core = ty.core;
                field.relate = !builtin_name(ty.core);
                field.field.relation = ty.collection ? RelationKind::Aggregation : RelationKind::Association;
                add_field(cls, std::move(field));
            }
        }
        ++cursor;
    }
    if (!cls.cls.name.empty()) buf.classes.push_back(std::move(cls));
    return cursor;
}

std::size_t parse_python_block(ParseBuf& buf, const std::vector<PyLine>& lines, std::size_t index, int min_indent, RawClass* owner) {
    (void)owner;
    while (index < lines.size() && lines[index].indent >= min_indent) {
        if (lines[index].text.rfind("@", 0) == 0) {
            ++index;
            continue;
        }
        if (lines[index].text.rfind("class ", 0) == 0) {
            index = parse_python_class(buf, lines, index);
            continue;
        }
        ++index;
    }
    return index;
}

void parse_python(ParseBuf& buf, const std::string& text) {
    auto lines = python_lines(text);
    parse_python_block(buf, lines, 0, 0, nullptr);
}

bool same_type_name(const std::string& raw, const Class& cls) {
    if (raw.empty()) return false;
    if (raw == cls.qualified_name || raw == cls.name) return true;
    if (raw.size() > cls.qualified_name.size()) {
        const std::size_t at = raw.size() - cls.qualified_name.size();
        if (raw.compare(at, cls.qualified_name.size(), cls.qualified_name) == 0 && at > 0 &&
            (raw[at - 1] == '.' || raw[at - 1] == ':')) {
            return true;
        }
    }
    return false;
}

const RawClass* resolve_class(const std::vector<RawClass>& classes, const std::string& raw) {
    if (raw.empty() || raw == "@self" || builtin_name(raw)) return nullptr;
    const RawClass* found = nullptr;
    int matches = 0;
    for (const auto& cls : classes) {
        if (!same_type_name(raw, cls.cls)) continue;
        if (raw == cls.cls.qualified_name) return &cls;
        found = &cls;
        ++matches;
    }
    if (matches == 1) return found;
    found = nullptr;
    matches = 0;
    std::string simple = raw;
    auto dots = simple.rfind('.');
    auto cols = simple.rfind("::");
    if (cols != std::string::npos) simple = simple.substr(cols + 2);
    else if (dots != std::string::npos) simple = simple.substr(dots + 1);
    for (const auto& cls : classes) {
        if (cls.cls.name != simple) continue;
        found = &cls;
        ++matches;
    }
    return matches == 1 ? found : nullptr;
}

std::string resolved_name(const std::vector<RawClass>& classes, const std::string& raw, const RawClass* self_class) {
    if (raw == "@self" && self_class) return self_class->cls.qualified_name;
    const RawClass* cls = resolve_class(classes, raw);
    return cls ? cls->cls.qualified_name : "";
}

void materialize(ProjectModel& model, std::vector<RawClass>& classes,
                 std::vector<std::tuple<std::string, std::string, std::string>>& uses) {
    for (const auto& raw : classes) {
        if (raw.cls.name.empty() || raw.cls.name[0] == '@') continue;
        bool exists = false;
        for (const auto& item : model.classes) {
            if (item.qualified_name == raw.cls.qualified_name) exists = true;
        }
        if (!exists) model.classes.push_back(raw.cls);
    }
    for (auto& raw : classes) {
        if (raw.cls.name.empty()) continue;
        Class* owner = nullptr;
        for (auto& item : model.classes) {
            if (item.qualified_name == raw.cls.qualified_name) owner = &item;
        }
        if (!owner) continue;
        for (std::size_t b = 0; b < raw.bases.size(); ++b) {
            std::string target = resolved_name(classes, raw.bases[b], &raw);
            if (target.empty() || target == owner->qualified_name) continue;
            bool seen = false;
            for (const auto& base : owner->bases) {
                if (base.name == target) seen = true;
            }
            if (seen) continue;
            BaseSpec base;
            base.name = target;
            base.access = b < raw.base_labels.size() ? raw.base_labels[b] : "extends";
            owner->bases.push_back(std::move(base));
        }
        for (const auto& field : raw.fields) {
            Field stored = field.field;
            if (field.relate) {
                std::string target = resolved_name(classes, field.core, &raw);
                if (!target.empty() && target != owner->qualified_name) {
                    stored.target = target;
                } else {
                    stored.relation = RelationKind::Association;
                }
            }
            bool seen = false;
            for (auto& existing : owner->fields) {
                if (existing.name == stored.name) {
                    if (existing.target.empty() && !stored.target.empty()) existing = stored;
                    seen = true;
                    break;
                }
            }
            if (!seen) owner->fields.push_back(std::move(stored));
        }
        for (auto& method : raw.methods) {
            bool seen = false;
            for (const auto& existing : owner->methods) {
                if (existing.signature() == method.method.signature()) seen = true;
            }
            if (!seen) owner->methods.push_back(method.method);
            for (const auto& use : method.uses) {
                if (use.first.rfind("@", 0) == 0) continue;
                std::string target = resolved_name(classes, use.first, &raw);
                if (!target.empty() && target != owner->qualified_name) {
                    uses.emplace_back(owner->qualified_name, target, use.second);
                }
            }
            if (method.steps.empty()) continue;
            const std::string sep = raw.lang == Lang::Rust ? "::" : ".";
            std::string flow_name = owner->qualified_name + sep + method.method.name;
            for (auto& call : method.calls) {
                if (call.step >= method.steps.size()) continue;
                std::string target = resolved_name(classes, call.receiver_type, &raw);
                if (target.empty()) continue;
                const RawClass* callee = nullptr;
                for (const auto& candidate : classes) {
                    if (candidate.cls.qualified_name == target) callee = &candidate;
                }
                method.steps[call.step].callee = target + sep + call.method;
                if (!callee || model.calls.size() >= 4000) continue;
                CallEdge edge;
                edge.caller = flow_name;
                edge.caller_class = owner->qualified_name;
                edge.caller_file = owner->file;
                edge.callee = target + sep + call.method;
                edge.callee_class = target;
                edge.callee_file = callee->cls.file;
                edge.args = call.args;
                edge.line = call.line;
                bool duplicate = false;
                for (const auto& existing : model.calls) {
                    if (existing.caller == edge.caller && existing.callee == edge.callee && existing.line == edge.line) {
                        duplicate = true;
                    }
                }
                if (!duplicate) model.calls.push_back(std::move(edge));
            }
            FunctionFlow flow;
            flow.qualified_name = flow_name;
            flow.owner_class = owner->qualified_name;
            flow.file = method.method.file.empty() ? owner->file : method.method.file;
            flow.line = method.method.line;
            flow.steps = method.steps;
            bool replaced = false;
            for (auto& existing : model.flows) {
                if (existing.qualified_name == flow.qualified_name) {
                    if (flow.steps.size() > existing.steps.size()) existing = flow;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) model.flows.push_back(std::move(flow));
        }
    }
}

}  // namespace

bool is_text_language_ext(const std::string& ext) {
    return ext == ".java" || ext == ".py" || ext == ".ts" || ext == ".tsx" || ext == ".rs";
}

std::vector<std::tuple<std::string, std::string, std::string>> parse_text_sources(
    ProjectModel& model, const std::vector<std::string>& files, const std::atomic<bool>* stop) {
    std::vector<RawClass> classes;
    for (const auto& file : files) {
        if (stop && stop->load()) {
            model.diagnostics.push_back("Parse stopped.");
            break;
        }
        std::string ext = file;
        auto dot = ext.rfind('.');
        ext = dot == std::string::npos ? "" : ext.substr(dot);
        Lang lang = Lang::Java;
        if (ext == ".py") lang = Lang::Python;
        else if (ext == ".ts" || ext == ".tsx") lang = Lang::TypeScript;
        else if (ext == ".rs") lang = Lang::Rust;
        std::string text = read_text(file);
        if (text.empty()) continue;
        ParseBuf buf;
        buf.file = relative_to(model.root, file);
        buf.lang = lang;
        buf.scope = lang == Lang::Java ? "" : module_for(lang, buf.file);
        if (lang == Lang::Python) parse_python(buf, text);
        else parse_java_ts_rust(buf, text);
        for (auto& cls : buf.classes) classes.push_back(std::move(cls));
    }
    std::vector<std::tuple<std::string, std::string, std::string>> uses;
    materialize(model, classes, uses);
    return uses;
}

}  // namespace rewire
