#pragma once

#include <string>
#include <vector>

namespace rewire {

enum class RelationKind {
    Inheritance,
    Composition,
    Aggregation,
    Association,
    Dependency,
};

enum class EditAction {
    AddClass,
    RenameClass,
    AddField,
    AddMethod,
    AddBase,
    RemoveMember,
};

const char* relation_name(RelationKind kind);
const char* relation_marker(RelationKind kind);

struct Field {
    std::string name;
    std::string type_spelling;
    std::string access;
    RelationKind relation = RelationKind::Association;
    std::string target;
    int line = 0;
};

struct Method {
    std::string name;
    std::string return_type;
    std::vector<std::pair<std::string, std::string>> params;
    std::string access;
    bool is_virtual = false;
    bool is_const = false;
    bool is_static = false;
    bool is_pure = false;
    std::string file;
    int line = 0;

    std::string signature() const;
};

struct BaseSpec {
    std::string name;
    std::string access;
};

struct Class {
    std::string qualified_name;
    std::string name;
    std::string file;
    int line = 0;
    bool is_struct = false;
    std::vector<BaseSpec> bases;
    std::vector<Field> fields;
    std::vector<Method> methods;
};

struct Relation {
    std::string from;
    std::string to;
    RelationKind kind = RelationKind::Dependency;
    std::string label;
};

struct FlowStep {
    int depth = 0;
    std::string kind;
    std::string text;
    std::string callee;
    std::string file;
    int line = 0;
};

struct FunctionFlow {
    std::string qualified_name;
    std::string owner_class;
    std::string file;
    int line = 0;
    std::vector<FlowStep> steps;
};

struct CallEdge {
    std::string caller;
    std::string caller_class;
    std::string caller_file;
    std::string callee;
    std::string callee_class;
    std::string callee_file;
    std::string args;
    int line = 0;
};

struct ProjectModel {
    std::string root;
    std::vector<Class> classes;
    std::vector<Relation> relations;
    std::vector<FunctionFlow> flows;
    std::vector<CallEdge> calls;
    std::vector<std::string> diagnostics;

    const Class* find_class(const std::string& qualified) const;
};

struct EditRequest {
    EditAction action = EditAction::AddField;
    std::string class_name;
    std::string primary;
    std::string secondary;
    std::string access = "public";
    std::string relationship = "auto";
};

std::string format_model(const ProjectModel& model);
std::string short_name(const std::string& qualified);

}  // namespace rewire
