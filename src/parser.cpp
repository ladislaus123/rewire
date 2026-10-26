#include "parser.hpp"

#include "json.hpp"
#include "text_lang.hpp"
#include "util.hpp"

#include "clang-c/Index.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace rewire {
namespace {

namespace fs = std::filesystem;

bool skipped_dir(const std::string& name) {
    static const std::unordered_set<std::string> names = {
        "build", "cmake-build", "out", "dist", "third_party", "thirdparty", "external", "node_modules",
        ".git", ".rewire", "CMakeFiles", "_deps", ".cache", "target", "__pycache__", "venv", "vendor",
        "site-packages", "Pods", "coverage", "bower_components"};
    if (name.empty() || name[0] == '.') return true;
    if (name.rfind("cmake-build", 0) == 0) return true;
    return names.count(name) != 0;
}

bool header_ext(const std::string& ext) {
    return ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx" || ext == ".ipp";
}

bool source_ext(const std::string& ext) {
    return header_ext(ext) || ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".C" || ext == ".c" ||
           is_text_language_ext(ext);
}

std::string canonical_path(const fs::path& path) {
    std::error_code error;
    fs::path canon = fs::weakly_canonical(path, error);
    if (error) {
        return path.lexically_normal().string();
    }
    return canon.string();
}

std::string shell_out(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return {};
    }
    std::string out;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        out += buffer;
    }
    pclose(pipe);
    return trim_copy(out);
}

std::vector<std::string> split_shell(const std::string& command) {
    std::vector<std::string> parts;
    std::string current;
    char quote = 0;
    for (std::size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        if (quote) {
            if (c == quote) {
                quote = 0;
            } else if (c == '\\' && i + 1 < command.size()) {
                current.push_back(command[++i]);
            } else {
                current.push_back(c);
            }
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '\\' && i + 1 < command.size()) {
            current.push_back(command[++i]);
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

struct CompileCommand {
    std::string directory;
    std::vector<std::string> args;
};

bool flag_takes_path(const std::string& flag) {
    return flag == "-I" || flag == "-isystem" || flag == "-iframework" || flag == "-include" ||
           flag == "-imacros" || flag == "-resource-dir" || flag == "-isysroot" || flag == "--sysroot" ||
           flag == "-cxx-isystem";
}

std::string absolutize(const std::string& directory, const std::string& path) {
    if (path.empty() || path[0] == '/') {
        return path;
    }
    return (fs::path(directory) / path).lexically_normal().string();
}

class Parser {
public:
    explicit Parser(std::string root, const std::atomic<bool>* stop) : stop_(stop) {
        model_.root = canonical_path(root);
        note_languages();
        discover_toolchain();
        load_compile_commands();
        collect_includes();
    }

    ProjectModel run() {
        auto files = list_source_files(model_.root);
        std::vector<std::string> clang_files;
        std::vector<std::string> text_files;
        for (const auto& file : files) {
            if (is_text_language_ext(fs::path(file).extension().string())) text_files.push_back(file);
            else clang_files.push_back(file);
        }
        auto cap = [&](std::vector<std::string>& list, const char* label) {
            if (list.size() <= 500) return;
            model_.diagnostics.push_back("Parsing the first 500 of " + std::to_string(list.size()) + " " + label +
                                          ". Narrow the project directory for the rest.");
            list.resize(500);
        };
        cap(clang_files, "C and C++ files");
        cap(text_files, "Java, Python, TypeScript, and Rust files");
        CXIndex index = clang_createIndex(0, 0);
        clang_toggleCrashRecovery(0);
        for (const auto& file : clang_files) {
            if (stop_ && stop_->load()) {
                model_.diagnostics.push_back("Parse stopped.");
                break;
            }
            parse_file(index, file);
        }
        clang_disposeIndex(index);
        if (!(stop_ && stop_->load())) {
            for (auto& use : parse_text_sources(model_, text_files, stop_)) {
                uses_.push_back(std::move(use));
            }
        }
        attach_c_callees();
        build_relations();
        std::sort(model_.classes.begin(), model_.classes.end(), [](const Class& a, const Class& b) {
            return a.qualified_name < b.qualified_name;
        });
        return std::move(model_);
    }

private:
    ProjectModel model_;
    const std::atomic<bool>* stop_ = nullptr;
    std::string clang_bin_ = "clang++";
    std::string resource_dir_;
    std::string sdk_;
    std::vector<std::string> include_dirs_;
    std::unordered_map<std::string, CompileCommand> commands_;
    std::unordered_map<std::string, std::string> file_cache_;
    std::vector<std::tuple<std::string, std::string, std::string>> uses_;
    std::unordered_map<std::string, std::string> c_method_owner_;
    bool project_has_c_ = false;
    bool project_has_cxx_ = false;
    bool parsing_c_ = false;
    int diagnostic_budget_ = 40;

    void note_languages() {
        for (const auto& file : list_source_files(model_.root)) {
            const std::string ext = fs::path(file).extension().string();
            if (ext == ".c") project_has_c_ = true;
            if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".C" || ext == ".hpp" || ext == ".hh" ||
                ext == ".hxx" || ext == ".ipp") {
                project_has_cxx_ = true;
            }
        }
    }

    bool looks_like_cxx(const std::string& file) const {
        const std::string text = read_text(file);
        return text.find("namespace ") != std::string::npos || text.find("class ") != std::string::npos ||
               text.find("template") != std::string::npos || text.find("typename") != std::string::npos ||
               text.find("::") != std::string::npos;
    }

    bool parse_as_c(const std::string& file) const {
        const std::string ext = fs::path(file).extension().string();
        if (ext == ".c") return true;
        if (ext != ".h") return false;
        if (!project_has_c_) return false;
        if (!project_has_cxx_) return true;
        return !looks_like_cxx(file);
    }

    void discover_toolchain() {
        if (fs::exists("/Library/Developer/CommandLineTools/usr/bin/clang++")) {
            clang_bin_ = "/Library/Developer/CommandLineTools/usr/bin/clang++";
        }
        fs::path resource_root = "/Library/Developer/CommandLineTools/usr/lib/clang";
        if (fs::exists(resource_root)) {
            std::string best;
            for (const auto& entry : fs::directory_iterator(resource_root)) {
                if (entry.is_directory() && fs::exists(entry.path() / "include" / "stddef.h")) {
                    best = entry.path().string();
                }
            }
            resource_dir_ = best;
        }
        sdk_ = shell_out("xcrun --show-sdk-path 2>/dev/null");
    }

    void load_compile_commands() {
        std::vector<fs::path> candidates = {
            fs::path(model_.root) / "compile_commands.json",
            fs::path(model_.root) / "build" / "compile_commands.json",
        };
        std::string text;
        std::string directory_hint = model_.root;
        for (const auto& candidate : candidates) {
            text = read_text(candidate.string());
            if (!text.empty()) {
                break;
            }
        }
        if (text.empty()) {
            return;
        }
        json::Value root = json::parse(text);
        if (root.type != json::Value::Type::Array) {
            model_.diagnostics.push_back("compile_commands.json is not a JSON array.");
            return;
        }
        for (const auto& entry : root.array) {
            const json::Value* file = entry.find("file");
            const json::Value* directory = entry.find("directory");
            if (!file) {
                continue;
            }
            std::string dir = directory ? directory->as_string() : model_.root;
            std::string source = file->as_string();
            if (!source.empty() && source[0] != '/') {
                source = absolutize(dir, source);
            }
            source = canonical_path(source);
            CompileCommand command;
            command.directory = dir;
            const json::Value* arguments = entry.find("arguments");
            if (arguments && arguments->type == json::Value::Type::Array) {
                for (const auto& arg : arguments->array) {
                    command.args.push_back(arg.as_string());
                }
            } else if (const json::Value* raw = entry.find("command")) {
                command.args = split_shell(raw->as_string());
            }
            if (command.args.empty()) {
                continue;
            }
            command.args = clean_compile_args(command.args, dir, source);
            commands_[source] = std::move(command);
        }
    }

    std::vector<std::string> clean_compile_args(const std::vector<std::string>& raw, const std::string& directory,
                                                 const std::string& source) const {
        std::vector<std::string> args;
        if (!raw.empty()) {
            args.push_back(raw.front());
        }
        auto same_file = [&](const std::string& value) {
            if (value.empty() || value[0] == '-') {
                return false;
            }
            std::string absolute = value[0] == '/' ? value : absolutize(directory, value);
            std::error_code error;
            return canonical_path(absolute) == source || fs::path(absolute).filename() == fs::path(source).filename();
        };
        for (std::size_t i = 1; i < raw.size(); ++i) {
            const std::string& arg = raw[i];
            if (arg == "-c" || arg == "-MP" || arg == "-MD" || arg == "-MMD") {
                continue;
            }
            if (arg == "-o" || arg == "-MF" || arg == "-MT" || arg == "-MQ" || arg == "-MJ") {
                if (i + 1 < raw.size()) {
                    ++i;
                }
                continue;
            }
            if (same_file(arg)) {
                continue;
            }
            if (flag_takes_path(arg) && i + 1 < raw.size()) {
                args.push_back(arg);
                args.push_back(absolutize(directory, raw[++i]));
                continue;
            }
            auto attached = [&](const char* prefix) {
                return arg.rfind(prefix, 0) == 0 && arg.size() > std::char_traits<char>::length(prefix);
            };
            if (attached("-I") || attached("-isystem") || attached("-iframework")) {
                std::string prefix = arg.rfind("-isystem", 0) == 0 ? "-isystem" : arg.rfind("-iframework", 0) == 0 ? "-iframework" : "-I";
                args.push_back(prefix + absolutize(directory, arg.substr(prefix.size())));
                continue;
            }
            args.push_back(arg);
        }
        args.push_back("-Wno-error");
        return args;
    }

    void collect_includes() {
        std::set<std::string> dirs;
        dirs.insert(model_.root);
        for (const auto& file : list_source_files(model_.root)) {
            if (header_ext(fs::path(file).extension().string())) {
                dirs.insert(fs::path(file).parent_path().string());
            }
            if (dirs.size() > 200) {
                break;
            }
        }
        include_dirs_.assign(dirs.begin(), dirs.end());
    }

    bool in_project(const std::string& file) const {
        if (file.empty()) {
            return false;
        }
        std::string path = canonical_path(file);
        if (path == model_.root || path.rfind(model_.root + "/", 0) != 0) {
            return false;
        }
        return path.find("/build/") == std::string::npos && path.find("/_deps/") == std::string::npos;
    }

    std::string relative_to_root(const std::string& file) const {
        std::string path = canonical_path(file);
        std::string prefix = model_.root + "/";
        if (path.rfind(prefix, 0) == 0) {
            return path.substr(prefix.size());
        }
        return path;
    }

    static std::string cx(CXString value) {
        const char* text = clang_getCString(value);
        std::string out = text ? text : "";
        clang_disposeString(value);
        return out;
    }

    struct Loc {
        std::string file;
        unsigned line = 0;
        bool system = false;
        bool valid = false;
    };

    Loc locate(CXCursor cursor) const {
        Loc loc;
        CXSourceLocation location = clang_getCursorLocation(cursor);
        loc.system = clang_Location_isInSystemHeader(location) != 0;
        CXFile file = nullptr;
        unsigned line = 0;
        unsigned column = 0;
        unsigned offset = 0;
        clang_getFileLocation(location, &file, &line, &column, &offset);
        if (!file) {
            return loc;
        }
        loc.file = cx(clang_getFileName(file));
        loc.line = line;
        loc.valid = true;
        return loc;
    }

    bool project_cursor(CXCursor cursor) const {
        Loc loc = locate(cursor);
        return loc.valid && !loc.system && in_project(loc.file);
    }

    static bool is_scope(CXCursorKind kind) {
        return kind == CXCursor_Namespace || kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl ||
               kind == CXCursor_ClassTemplate || kind == CXCursor_ClassTemplatePartialSpecialization;
    }

    static bool is_class(CXCursorKind kind) {
        return kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl || kind == CXCursor_ClassTemplate ||
               kind == CXCursor_ClassTemplatePartialSpecialization;
    }

    static bool is_method(CXCursorKind kind) {
        return kind == CXCursor_CXXMethod || kind == CXCursor_Constructor || kind == CXCursor_Destructor ||
               kind == CXCursor_ConversionFunction;
    }

    std::string spelling(CXCursor cursor) const { return cx(clang_getCursorSpelling(cursor)); }

    std::string qualified_name(CXCursor cursor) const {
        std::vector<std::string> parts;
        CXCursor current = cursor;
        for (int guard = 0; guard < 24 && !clang_Cursor_isNull(current); ++guard) {
            CXCursorKind kind = clang_getCursorKind(current);
            if (clang_isTranslationUnit(kind)) {
                break;
            }
            if (is_scope(kind)) {
                std::string name = spelling(current);
                if (name == "(anonymous namespace)") {
                    parts.emplace_back("(anonymous)");
                } else if (!name.empty()) {
                    parts.push_back(std::move(name));
                }
            }
            CXCursor parent = clang_getCursorSemanticParent(current);
            if (clang_equalCursors(parent, current)) {
                break;
            }
            current = parent;
        }
        std::reverse(parts.begin(), parts.end());
        std::string out;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) out += "::";
            out += parts[i];
        }
        return out;
    }

    const std::string& cached_file(const std::string& path) {
        auto found = file_cache_.find(path);
        if (found != file_cache_.end()) {
            return found->second;
        }
        return file_cache_[path] = read_text(path);
    }

    std::string snippet(CXCursor cursor) {
        CXSourceRange range = clang_getCursorExtent(cursor);
        CXFile start_file = nullptr;
        CXFile end_file = nullptr;
        unsigned line = 0, column = 0, begin = 0, end_line = 0, end_column = 0, end = 0;
        clang_getSpellingLocation(clang_getRangeStart(range), &start_file, &line, &column, &begin);
        clang_getSpellingLocation(clang_getRangeEnd(range), &end_file, &end_line, &end_column, &end);
        if (!start_file || start_file != end_file || end <= begin) {
            return {};
        }
        std::string path = cx(clang_getFileName(start_file));
        const std::string& text = cached_file(path);
        if (end > text.size()) {
            return {};
        }
        return collapse_ws(text.substr(begin, end - begin), 140);
    }

    bool has_body_extent(CXCursor cursor) {
        return !snippet(cursor).empty();
    }

    static std::string access_name(CXCursor cursor) {
        switch (clang_getCXXAccessSpecifier(cursor)) {
            case CX_CXXProtected: return "protected";
            case CX_CXXPrivate: return "private";
            case CX_CXXPublic:
            default: return "public";
        }
    }

    static CXType peel(CXType type) {
        for (int i = 0; i < 6; ++i) {
            CXType next = type;
            if (type.kind == CXType_Elaborated) {
                next = clang_Type_getNamedType(type);
            } else if (type.kind == CXType_Typedef || type.kind == CXType_Unexposed || type.kind == CXType_Auto ||
                       type.kind == CXType_Attributed) {
                next = clang_getCanonicalType(type);
            } else {
                break;
            }
            if (next.kind == CXType_Invalid || clang_equalTypes(next, type)) {
                break;
            }
            type = next;
        }
        return type;
    }

    std::string template_name(CXType type) const {
        CXCursor decl = clang_getTypeDeclaration(peel(type));
        if (clang_Cursor_isNull(decl)) {
            return {};
        }
        CXCursor specialized = clang_getSpecializedCursorTemplate(decl);
        CXCursor use = clang_Cursor_isNull(specialized) ? decl : specialized;
        return spelling(use);
    }

    static bool is_sequence(const std::string& name) {
        return name == "vector" || name == "list" || name == "deque" || name == "set" || name == "unordered_set" ||
               name == "optional" || name == "array" || name == "stack" || name == "queue" || name == "multiset" ||
               name == "forward_list";
    }

    static bool is_map(const std::string& name) {
        return name == "map" || name == "unordered_map" || name == "multimap" || name == "unordered_multimap";
    }

    struct Classified {
        bool ok = false;
        std::string target;
        RelationKind kind = RelationKind::Composition;
    };

    Classified classify_member(CXType type) const {
        RelationKind kind = RelationKind::Composition;
        CXType current = type;
        for (int depth = 0; depth < 12; ++depth) {
            current = peel(current);
            if (current.kind == CXType_Pointer || current.kind == CXType_LValueReference ||
                current.kind == CXType_RValueReference || current.kind == CXType_MemberPointer) {
                if (kind == RelationKind::Composition) {
                    kind = RelationKind::Association;
                }
                current = clang_getPointeeType(current);
                continue;
            }
            if (current.kind == CXType_ConstantArray || current.kind == CXType_IncompleteArray ||
                current.kind == CXType_VariableArray || current.kind == CXType_DependentSizedArray) {
                current = clang_getArrayElementType(current);
                continue;
            }
            if (current.kind != CXType_Record && current.kind != CXType_Unexposed && current.kind != CXType_Elaborated) {
                return {};
            }
            std::string name = template_name(current);
            int nargs = clang_Type_getNumTemplateArguments(current);
            if (nargs < 0) {
                nargs = 0;
            }
            if (name == "unique_ptr" && nargs > 0) {
                kind = RelationKind::Composition;
                current = clang_Type_getTemplateArgumentAsType(current, 0);
                continue;
            }
            if (name == "shared_ptr" && nargs > 0) {
                kind = RelationKind::Aggregation;
                current = clang_Type_getTemplateArgumentAsType(current, 0);
                continue;
            }
            if (name == "weak_ptr" && nargs > 0) {
                kind = RelationKind::Association;
                current = clang_Type_getTemplateArgumentAsType(current, 0);
                continue;
            }
            if (is_sequence(name) && nargs > 0) {
                current = clang_Type_getTemplateArgumentAsType(current, 0);
                continue;
            }
            if (is_map(name) && nargs > 1) {
                current = clang_Type_getTemplateArgumentAsType(current, 1);
                continue;
            }
            CXCursor decl = clang_getTypeDeclaration(current);
            if (clang_Cursor_isNull(decl)) {
                return {};
            }
            CXCursor specialized = clang_getSpecializedCursorTemplate(decl);
            if (!clang_Cursor_isNull(specialized)) {
                decl = specialized;
            }
            if (!project_cursor(decl)) {
                return {};
            }
            Classified result;
            result.ok = true;
            result.target = qualified_name(decl);
            result.kind = kind;
            return result;
        }
        return {};
    }

    void collect_types(CXType type, std::vector<std::string>& out, int depth = 0) const {
        if (depth > 10 || type.kind == CXType_Invalid) {
            return;
        }
        type = peel(type);
        if (type.kind == CXType_Pointer || type.kind == CXType_LValueReference || type.kind == CXType_RValueReference) {
            collect_types(clang_getPointeeType(type), out, depth + 1);
            return;
        }
        if (type.kind == CXType_ConstantArray || type.kind == CXType_IncompleteArray) {
            collect_types(clang_getArrayElementType(type), out, depth + 1);
            return;
        }
        if (type.kind != CXType_Record && type.kind != CXType_Unexposed) {
            return;
        }
        std::string name = template_name(type);
        int nargs = clang_Type_getNumTemplateArguments(type);
        if (nargs < 0) nargs = 0;
        if ((name == "unique_ptr" || name == "shared_ptr" || name == "weak_ptr" || is_sequence(name)) && nargs > 0) {
            collect_types(clang_Type_getTemplateArgumentAsType(type, 0), out, depth + 1);
            return;
        }
        if (is_map(name) && nargs > 1) {
            collect_types(clang_Type_getTemplateArgumentAsType(type, 0), out, depth + 1);
            collect_types(clang_Type_getTemplateArgumentAsType(type, 1), out, depth + 1);
            return;
        }
        CXCursor decl = clang_getTypeDeclaration(type);
        if (clang_Cursor_isNull(decl) || !project_cursor(decl)) {
            return;
        }
        CXCursor specialized = clang_getSpecializedCursorTemplate(decl);
        if (!clang_Cursor_isNull(specialized)) {
            decl = specialized;
        }
        std::string qualified = qualified_name(decl);
        if (!qualified.empty()) {
            out.push_back(std::move(qualified));
        }
    }

    std::size_t upsert_class(CXCursor cursor) {
        std::string qualified = qualified_name(cursor);
        if (qualified.empty()) {
            qualified = spelling(cursor);
        }
        for (std::size_t i = 0; i < model_.classes.size(); ++i) {
            if (model_.classes[i].qualified_name == qualified) {
                if (model_.classes[i].line == 0) {
                    Loc loc = locate(cursor);
                    model_.classes[i].line = static_cast<int>(loc.line);
                    model_.classes[i].file = relative_to_root(loc.file);
                }
                return i;
            }
        }
        Class created;
        created.qualified_name = qualified;
        created.name = spelling(cursor);
        if (created.name.empty()) {
            created.name = short_name(qualified);
        }
        Loc loc = locate(cursor);
        created.file = relative_to_root(loc.file);
        created.line = static_cast<int>(loc.line);
        created.is_struct = clang_getCursorKind(cursor) == CXCursor_StructDecl;
        model_.classes.push_back(std::move(created));
        return model_.classes.size() - 1;
    }

    void remember_use(const std::string& from, CXType type, const std::string& label) {
        if (from.empty()) {
            return;
        }
        std::vector<std::string> targets;
        collect_types(type, targets);
        for (const auto& target : targets) {
            if (target != from) {
                uses_.emplace_back(from, target, label);
            }
        }
    }

    void add_method(std::size_t class_index, CXCursor cursor) {
        if (!has_body_extent(cursor)) {
            return;
        }
        std::string body = snippet(cursor);
        if (body.find("= default") != std::string::npos || body.find("= delete") != std::string::npos) {
            return;
        }
        Method method;
        CXCursorKind kind = clang_getCursorKind(cursor);
        method.name = spelling(cursor);
        if (method.name.empty()) {
            return;
        }
        method.access = access_name(cursor);
        method.is_const = clang_CXXMethod_isConst(cursor) != 0;
        method.is_static = clang_CXXMethod_isStatic(cursor) != 0;
        method.is_virtual = clang_CXXMethod_isVirtual(cursor) != 0;
        method.is_pure = clang_CXXMethod_isPureVirtual(cursor) != 0;
        Loc loc = locate(cursor);
        method.file = relative_to_root(loc.file);
        method.line = static_cast<int>(loc.line);
        if (kind != CXCursor_Constructor && kind != CXCursor_Destructor) {
            method.return_type = cx(clang_getTypeSpelling(clang_getCursorResultType(cursor)));
        }
        int count = clang_Cursor_getNumArguments(cursor);
        if (count < 0) count = 0;
        for (int i = 0; i < count; ++i) {
            CXCursor arg = clang_Cursor_getArgument(cursor, static_cast<unsigned>(i));
            method.params.emplace_back(cx(clang_getTypeSpelling(clang_getCursorType(arg))), spelling(arg));
        }
        std::string key = method.signature();
        Class& owner = model_.classes[class_index];
        bool exists = false;
        for (const auto& existing : owner.methods) {
            if (existing.signature() == key) {
                exists = true;
                break;
            }
        }
        std::string owner_name = owner.qualified_name;
        std::string flow_name = owner_name.empty() ? method.name : owner_name + "::" + method.name;
        if (kind != CXCursor_Destructor) {
            flow_name += "(";
            for (std::size_t i = 0; i < method.params.size(); ++i) {
                if (i) flow_name += ", ";
                flow_name += method.params[i].first;
            }
            flow_name += ")";
        }
        if (!exists) {
            owner.methods.push_back(method);
        }
        remember_use(owner_name, clang_getCursorResultType(cursor), method.name);
        for (int i = 0; i < count; ++i) {
            CXCursor arg = clang_Cursor_getArgument(cursor, static_cast<unsigned>(i));
            remember_use(owner_name, clang_getCursorType(arg), method.name);
        }
        if (clang_isCursorDefinition(cursor)) {
            walk_function(cursor, owner_name, flow_name, method.file, method.line);
        }
    }

    void walk_function(CXCursor cursor, const std::string& owner, const std::string& flow_name, const std::string& file,
                       int line) {
        FunctionFlow flow;
        flow.qualified_name = flow_name;
        flow.owner_class = owner;
        flow.file = file;
        flow.line = line;
        std::vector<CXCursor> kids = children(cursor);
        for (CXCursor child : kids) {
            if (clang_isStatement(clang_getCursorKind(child))) {
                walk_stmt(child, 0, flow);
            }
        }
        if (flow.steps.empty()) {
            return;
        }
        for (auto& existing : model_.flows) {
            if (existing.qualified_name == flow.qualified_name) {
                if (flow.steps.size() > existing.steps.size()) {
                    existing = std::move(flow);
                }
                return;
            }
        }
        model_.flows.push_back(std::move(flow));
    }

    void walk_stmt(CXCursor cursor, int depth, FunctionFlow& flow) {
        if (flow.steps.size() > 120) {
            return;
        }
        CXCursorKind kind = clang_getCursorKind(cursor);
        if (kind == CXCursor_CompoundStmt) {
            for (CXCursor child : children(cursor)) {
                walk_stmt(child, depth, flow);
            }
            return;
        }
        if (kind == CXCursor_IfStmt || kind == CXCursor_WhileStmt || kind == CXCursor_ForStmt ||
            kind == CXCursor_DoStmt || kind == CXCursor_SwitchStmt || kind == CXCursor_CXXForRangeStmt ||
            kind == CXCursor_CXXTryStmt || kind == CXCursor_CXXCatchStmt) {
            std::string text = snippet(cursor);
            auto brace = text.find('{');
            if (brace != std::string::npos) {
                text = trim_copy(text.substr(0, brace));
            }
            if (text.empty()) {
                if (kind == CXCursor_IfStmt) text = "if";
                else if (kind == CXCursor_SwitchStmt) text = "switch";
                else if (kind == CXCursor_CXXTryStmt) text = "try";
                else if (kind == CXCursor_CXXCatchStmt) text = "catch";
                else text = "loop";
            }
            std::string step_kind = "branch";
            if (kind == CXCursor_WhileStmt || kind == CXCursor_ForStmt || kind == CXCursor_DoStmt ||
                kind == CXCursor_CXXForRangeStmt) {
                step_kind = "loop";
            } else if (kind == CXCursor_SwitchStmt) {
                step_kind = "switch";
            } else if (kind == CXCursor_CXXTryStmt || kind == CXCursor_CXXCatchStmt) {
                step_kind = "try";
            }
            FlowStep step;
            step.depth = depth;
            step.kind = step_kind;
            step.text = text;
            Loc loc = locate(cursor);
            step.file = relative_to_root(loc.file);
            step.line = static_cast<int>(loc.line);
            flow.steps.push_back(std::move(step));
            for (CXCursor child : children(cursor)) {
                walk_stmt(child, depth + 1, flow);
            }
            return;
        }
        if (kind == CXCursor_ReturnStmt || kind == CXCursor_CXXThrowExpr || kind == CXCursor_BreakStmt ||
            kind == CXCursor_ContinueStmt) {
            FlowStep step;
            step.depth = depth;
            step.kind = kind == CXCursor_ReturnStmt ? "return" : kind == CXCursor_CXXThrowExpr ? "throw" : "jump";
            step.text = snippet(cursor);
            if (step.text.empty()) step.text = step.kind;
            Loc loc = locate(cursor);
            step.line = static_cast<int>(loc.line);
            flow.steps.push_back(std::move(step));
            return;
        }
        if (kind == CXCursor_CallExpr) {
            record_call(cursor, depth, flow);
            for (CXCursor child : children(cursor)) {
                walk_stmt(child, depth, flow);
            }
            return;
        }
        if (kind == CXCursor_VarDecl) {
            remember_use(flow.owner_class, clang_getCursorType(cursor), short_name(flow.qualified_name));
        }
        for (CXCursor child : children(cursor)) {
            walk_stmt(child, depth, flow);
        }
    }

    void record_call(CXCursor cursor, int depth, FunctionFlow& flow) {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        if (clang_Cursor_isNull(referenced) || locate(referenced).system) {
            return;
        }
        std::string name = spelling(referenced);
        // A by-value argument shows up as a call whose source text is only the
        // argument name. That is a constructor, not a step in the function.
        if (name.empty() || name.rfind("operator", 0) == 0 || name[0] == '~' ||
            clang_getCursorKind(referenced) == CXCursor_Constructor) {
            return;
        }
        Loc loc = locate(cursor);
        std::string callee = name;
        std::string callee_class;
        std::string callee_file;
        if (!clang_Cursor_isNull(referenced)) {
            CXCursor parent = clang_getCursorSemanticParent(referenced);
            if (is_class(clang_getCursorKind(parent))) {
                callee_class = qualified_name(parent);
                callee = callee_class + "::" + name;
            } else {
                std::string scope = qualified_name(referenced);
                if (!scope.empty() && scope != name) {
                    callee = scope + "::" + name;
                }
            }
            CXCursor definition = clang_getCursorDefinition(referenced);
            Loc target_loc = locate(clang_Cursor_isNull(definition) ? referenced : definition);
            if (target_loc.valid) {
                callee_file = relative_to_root(target_loc.file);
            }
        }
        std::vector<std::string> arg_types;
        int count = clang_Cursor_getNumArguments(cursor);
        if (count < 0) count = 0;
        for (int i = 0; i < count; ++i) {
            CXCursor arg = clang_Cursor_getArgument(cursor, static_cast<unsigned>(i));
            arg_types.push_back(cx(clang_getTypeSpelling(clang_getCursorType(arg))));
            remember_use(flow.owner_class, clang_getCursorType(arg), short_name(flow.qualified_name));
        }
        std::string args;
        for (std::size_t i = 0; i < arg_types.size(); ++i) {
            if (i) args += ", ";
            args += arg_types[i];
        }
        FlowStep step;
        step.depth = depth;
        step.kind = "call";
        step.text = snippet(cursor);
        if (step.text.empty()) {
            step.text = callee + "(" + args + ")";
        }
        step.callee = callee;
        step.file = relative_to_root(loc.file);
        step.line = static_cast<int>(loc.line);
        flow.steps.push_back(step);
        if (!callee.empty() && !callee_file.empty() && callee_file[0] != '/') {
            CallEdge edge;
            edge.caller = flow.qualified_name;
            edge.caller_class = flow.owner_class;
            edge.caller_file = flow.file;
            edge.callee = callee;
            edge.callee_class = callee_class;
            edge.callee_file = callee_file;
            edge.args = args;
            edge.line = step.line;
            bool duplicate = false;
            for (const auto& existing : model_.calls) {
                if (existing.caller == edge.caller && existing.callee == edge.callee && existing.line == edge.line) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate && model_.calls.size() < 4000) {
                model_.calls.push_back(std::move(edge));
            }
        }
    }

    void handle_class(CXCursor cursor) {
        if (!project_cursor(cursor)) {
            return;
        }
        std::size_t index = upsert_class(cursor);
        for (CXCursor child : children(cursor)) {
            CXCursorKind kind = clang_getCursorKind(child);
            if (kind == CXCursor_CXXBaseSpecifier) {
                CXCursor referenced = clang_getCursorReferenced(child);
                if (clang_Cursor_isNull(referenced) || !project_cursor(referenced)) {
                    continue;
                }
                std::size_t base_index = upsert_class(referenced);
                BaseSpec base;
                base.name = model_.classes[base_index].qualified_name;
                base.access = access_name(child);
                bool exists = false;
                for (const auto& item : model_.classes[index].bases) {
                    if (item.name == base.name) exists = true;
                }
                if (!exists) {
                    model_.classes[index].bases.push_back(std::move(base));
                }
            } else if (kind == CXCursor_FieldDecl) {
                std::string name = spelling(child);
                if (name.empty()) {
                    continue;
                }
                Field field;
                field.name = name;
                field.type_spelling = cx(clang_getTypeSpelling(clang_getCursorType(child)));
                field.access = access_name(child);
                field.line = static_cast<int>(locate(child).line);
                Classified info = classify_member(clang_getCursorType(child));
                if (info.ok) {
                    field.target = info.target;
                    field.relation = info.kind;
                }
                bool exists = false;
                for (auto& item : model_.classes[index].fields) {
                    if (item.name == field.name) {
                        if (item.target.empty() && !field.target.empty()) {
                            item = field;
                        }
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    model_.classes[index].fields.push_back(std::move(field));
                }
            } else if (is_method(kind)) {
                add_method(index, child);
            } else if (is_class(kind)) {
                handle_class(child);
            } else if (kind == CXCursor_FunctionTemplate) {
                for (CXCursor inner : children(child)) {
                    if (is_method(clang_getCursorKind(inner))) {
                        add_method(index, inner);
                    }
                }
            }
        }
    }

    void handle_function(CXCursor cursor) {
        if (!project_cursor(cursor)) {
            return;
        }
        CXCursor parent = clang_getCursorSemanticParent(cursor);
        if (is_class(clang_getCursorKind(parent))) {
            if (project_cursor(parent)) {
                add_method(upsert_class(parent), cursor);
            }
            return;
        }
        if (!clang_isCursorDefinition(cursor)) {
            return;
        }
        std::string name = spelling(cursor);
        if (name.empty() || name.rfind("operator", 0) == 0) {
            return;
        }
        if (parsing_c_) {
            int count = clang_Cursor_getNumArguments(cursor);
            if (count > 0) {
                CXCursor arg = clang_Cursor_getArgument(cursor, 0);
                std::vector<std::string> targets;
                collect_types(clang_getCursorType(arg), targets);
                if (!targets.empty()) {
                    const std::string owner = targets.front();
                    for (std::size_t index = 0; index < model_.classes.size(); ++index) {
                        if (model_.classes[index].qualified_name != owner) continue;
                        c_method_owner_[name] = owner;
                        add_method(index, cursor);
                        return;
                    }
                }
            }
        }
        Loc loc = locate(cursor);
        std::string scope = qualified_name(cursor);
        std::string flow_name = scope.empty() ? name : scope + "::" + name;
        walk_function(cursor, "", flow_name, relative_to_root(loc.file), static_cast<int>(loc.line));
    }

    void walk_top(CXCursor cursor) {
        for (CXCursor child : children(cursor)) {
            if (locate(child).system) {
                continue;
            }
            CXCursorKind kind = clang_getCursorKind(child);
            if (kind == CXCursor_Namespace || kind == CXCursor_LinkageSpec || kind == CXCursor_UnexposedDecl) {
                walk_top(child);
            } else if (is_class(kind)) {
                handle_class(child);
            } else if (kind == CXCursor_FunctionDecl || kind == CXCursor_FunctionTemplate || is_method(kind)) {
                handle_function(child);
            } else if (clang_isDeclaration(kind)) {
                walk_top(child);
            }
        }
    }

    std::vector<std::string> args_for(const std::string& file) const {
        auto found = commands_.find(canonical_path(file));
        if (found != commands_.end() && !found->second.args.empty()) {
            return found->second.args;
        }
        std::vector<std::string> args;
        args.push_back(clang_bin_);
        if (parse_as_c(file)) {
            args.push_back("-std=c11");
            args.push_back("-xc");
        } else {
            args.push_back("-std=c++20");
            args.push_back("-xc++");
        }
        args.push_back("-ferror-limit=8");
        args.push_back("-Wno-error");
        if (!resource_dir_.empty()) {
            args.push_back("-resource-dir");
            args.push_back(resource_dir_);
        }
        if (!sdk_.empty()) {
            args.push_back("-isysroot");
            args.push_back(sdk_);
        }
        for (const auto& include : include_dirs_) {
            args.push_back("-I");
            args.push_back(include);
        }
        return args;
    }

    void attach_c_callees() {
        for (auto& call : model_.calls) {
            if (!call.callee_class.empty()) continue;
            auto found = c_method_owner_.find(call.callee);
            if (found == c_method_owner_.end()) {
                const auto pos = call.callee.rfind("::");
                const std::string leaf = pos == std::string::npos ? call.callee : call.callee.substr(pos + 2);
                found = c_method_owner_.find(leaf);
            }
            if (found != c_method_owner_.end()) call.callee_class = found->second;
        }
    }

    void parse_file(CXIndex index, const std::string& file) {
        parsing_c_ = parse_as_c(file);
        std::vector<std::string> storage = args_for(file);
        std::vector<const char*> argv;
        argv.reserve(storage.size());
        for (const auto& arg : storage) {
            argv.push_back(arg.c_str());
        }
        CXTranslationUnit unit = nullptr;
        CXErrorCode error = clang_parseTranslationUnit2FullArgv(
            index, file.c_str(), argv.data(), static_cast<int>(argv.size()), nullptr, 0,
            CXTranslationUnit_KeepGoing | CXTranslationUnit_Incomplete |
                CXTranslationUnit_IgnoreNonErrorsFromIncludedFiles,
            &unit);
        if (error != CXError_Success || !unit) {
            if (diagnostic_budget_-- > 0) {
                model_.diagnostics.push_back("Failed to parse " + relative_to_root(file) + " (libclang error " +
                                              std::to_string(static_cast<int>(error)) + ").");
            }
            if (unit) clang_disposeTranslationUnit(unit);
            return;
        }
        unsigned count = clang_getNumDiagnostics(unit);
        for (unsigned i = 0; i < count && diagnostic_budget_ > 0; ++i) {
            CXDiagnostic diagnostic = clang_getDiagnostic(unit, i);
            CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
            if (severity >= CXDiagnostic_Error) {
                model_.diagnostics.push_back(cx(clang_formatDiagnostic(diagnostic, clang_defaultDiagnosticDisplayOptions())));
                --diagnostic_budget_;
            }
            clang_disposeDiagnostic(diagnostic);
        }
        walk_top(clang_getTranslationUnitCursor(unit));
        clang_disposeTranslationUnit(unit);
    }

    bool known_class(const std::string& name) const {
        for (const auto& item : model_.classes) {
            if (item.qualified_name == name) return true;
        }
        return false;
    }

    void build_relations() {
        std::set<std::tuple<std::string, int, std::string, std::string>> seen;
        auto add = [&](const std::string& from, RelationKind kind, const std::string& to, const std::string& label) {
            if (from.empty() || to.empty() || from == to || !known_class(from) || !known_class(to)) {
                return;
            }
            auto key = std::make_tuple(from, static_cast<int>(kind), to, label);
            if (!seen.insert(key).second) {
                return;
            }
            Relation relation;
            relation.from = from;
            relation.to = to;
            relation.kind = kind;
            relation.label = label;
            model_.relations.push_back(std::move(relation));
        };
        for (const auto& item : model_.classes) {
            for (const auto& base : item.bases) {
                add(item.qualified_name, RelationKind::Inheritance, base.name, base.access);
            }
            for (const auto& field : item.fields) {
                if (!field.target.empty()) {
                    add(item.qualified_name, field.relation, field.target, field.name);
                }
            }
        }
        auto structural = [&](const std::string& from, const std::string& to) {
            for (const auto& relation : model_.relations) {
                if (relation.from == from && relation.to == to && relation.kind != RelationKind::Dependency) {
                    return true;
                }
            }
            return false;
        };
        std::map<std::pair<std::string, std::string>, std::vector<std::string>> deps;
        auto consider = [&](const std::string& from, const std::string& to, const std::string& label) {
            if (!known_class(from) || !known_class(to) || from == to || structural(from, to)) {
                return;
            }
            auto& labels = deps[{from, to}];
            if (std::find(labels.begin(), labels.end(), label) == labels.end() && labels.size() < 4) {
                labels.push_back(label);
            }
        };
        for (const auto& [from, to, label] : uses_) {
            consider(from, to, label);
        }
        for (const auto& call : model_.calls) {
            if (!call.caller_class.empty() && !call.callee_class.empty()) {
                consider(call.caller_class, call.callee_class, "calls " + short_name(call.callee));
            }
        }
        for (const auto& [pair, labels] : deps) {
            std::string label;
            for (std::size_t i = 0; i < labels.size(); ++i) {
                if (i) label += ", ";
                label += labels[i];
            }
            add(pair.first, RelationKind::Dependency, pair.second, label);
        }
    }

    static std::vector<CXCursor> children(CXCursor cursor) {
        std::vector<CXCursor> out;
        clang_visitChildren(
            cursor,
            [](CXCursor child, CXCursor, CXClientData data) {
                static_cast<std::vector<CXCursor>*>(data)->push_back(child);
                return CXChildVisit_Continue;
            },
            &out);
        return out;
    }
};

}  // namespace

std::vector<std::string> list_source_files(const std::string& root) {
    std::vector<std::string> files;
    fs::path start = canonical_path(root);
    if (!fs::exists(start)) {
        return files;
    }
    std::error_code error;
    fs::recursive_directory_iterator it(start, fs::directory_options::skip_permission_denied, error);
    fs::recursive_directory_iterator end;
    for (; it != end; it.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (it->is_directory(error)) {
            if (skipped_dir(it->path().filename().string())) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(error)) {
            continue;
        }
        std::string ext = it->path().extension().string();
        if (!source_ext(ext)) {
            continue;
        }
        std::error_code size_error;
        if (it->file_size(size_error) > 2 * 1024 * 1024) {
            continue;
        }
        files.push_back(canonical_path(it->path()));
    }
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
        bool ah = header_ext(fs::path(a).extension().string());
        bool bh = header_ext(fs::path(b).extension().string());
        if (ah != bh) return ah > bh;
        return a < b;
    });
    return files;
}

ProjectModel parse_project(const std::string& root, const std::atomic<bool>* stop) {
    Parser parser(canonical_path(root), stop);
    return parser.run();
}

}  // namespace rewire
